#include <efi.h>
#include <csmwrap.h>
#include <bootdev.h>
#include <printf.h>

/*
 * Boot device detection and BBS table building for CSMWrap
 *
 * This module detects which drive CSMWrap was booted from and creates a BBS
 * table that prioritizes that drive for SeaBIOS boot order. Two facts about
 * that table shape it. SeaBIOS addresses it by slot rather than by PCI
 * location, so entries are written where SeaBIOS will look for them; and a
 * disk with no active partition cannot boot, so when that is the case the
 * disk is stepped behind and the installer leads instead.
 */

/*
 * Parse device path to extract boot device information
 */
static bool parse_device_path(EFI_DEVICE_PATH_PROTOCOL *device_path,
                              struct boot_device_info *info)
{
    EFI_DEVICE_PATH_PROTOCOL *node;
    bool found_pci = false;

    if (!device_path || !info) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    info->device_type = BBS_HARDDISK;  /* Default to hard disk */

    /* Walk the device path to extract information */
    for (node = device_path; !IsDevicePathEnd(node); node = NextDevicePathNode(node)) {
        uint8_t type = DevicePathType(node);
        uint8_t subtype = DevicePathSubType(node);

        switch (type) {
        case HARDWARE_DEVICE_PATH:
            if (subtype == HW_PCI_DP) {
                PCI_DEVICE_PATH *pci = (PCI_DEVICE_PATH *)node;
                info->bus = 0;  /* Will be updated if we find ACPI path first */
                info->device = pci->Device;
                info->function = pci->Function;
                found_pci = true;
            }
            break;

        case MESSAGING_DEVICE_PATH:
            switch (subtype) {
            case MSG_SATA_DP:
                {
                    SATA_DEVICE_PATH *sata = (SATA_DEVICE_PATH *)node;
                    info->sata_port = sata->HBAPortNumber;
                    info->device_type = BBS_HARDDISK;
                }
                break;
            case MSG_USB_DP:
                info->is_usb = true;
                info->device_type = BBS_USB;
                break;
            case MSG_ATAPI_DP:
                {
                    ATAPI_DEVICE_PATH *atapi = (ATAPI_DEVICE_PATH *)node;
                    /*
                     * UEFI has no separate ATA subtype: this one node describes
                     * both an ATA disk and an ATAPI drive, and it is the only
                     * place the IDE channel and the drive select are visible on
                     * the UEFI side. SeaBIOS's CSM bridge does not look drives
                     * up by PCI location, it indexes the BBS table as
                     * 1 + channel * 2 + slave, so these two numbers decide
                     * which slot a drive can be found in.
                     */
                    info->ata_channel = atapi->PrimarySecondary;
                    info->ata_slave = atapi->SlaveMaster;
                    info->has_ata_position = true;
                }
                break;
            case MSG_SCSI_DP:
                info->device_type = BBS_HARDDISK;
                break;
            }
            break;

        case MEDIA_DEVICE_PATH:
            switch (subtype) {
            case MEDIA_HARDDRIVE_DP:
                info->device_type = BBS_HARDDISK;
                break;
            case MEDIA_CDROM_DP:
                info->device_type = BBS_CDROM;
                break;
            }
            break;
        }
    }

    info->valid = found_pci;
    return found_pci;
}

/*
 * Get PCI bus number by walking up to the PCI I/O protocol
 */
static bool get_pci_location(EFI_HANDLE device_handle, struct boot_device_info *info)
{
    EFI_STATUS status;
    EFI_GUID pci_io_guid = EFI_PCI_IO_PROTOCOL_GUID;
    EFI_GUID device_path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *device_path;
    EFI_PCI_IO_PROTOCOL *pci_io;
    EFI_HANDLE pci_handle;
    UINTN seg, bus, dev, func;

    /* First get the device path */
    status = gBS->HandleProtocol(device_handle, &device_path_guid,
                                  (void **)&device_path);
    if (EFI_ERROR(status) || !device_path) {
        printf("bootdev: Failed to get device path: %d\n", (int)status);
        return false;
    }

    /* Parse the device path for basic info */
    parse_device_path(device_path, info);

    /* Try to find PCI I/O protocol on this device path */
    status = gBS->LocateDevicePath(&pci_io_guid, &device_path, &pci_handle);
    if (EFI_ERROR(status)) {
        printf("bootdev: No PCI I/O on device path: %d\n", (int)status);
        return info->valid;  /* Return what we got from device path parsing */
    }

    /* Get the PCI I/O protocol */
    status = gBS->HandleProtocol(pci_handle, &pci_io_guid, (void **)&pci_io);
    if (EFI_ERROR(status)) {
        printf("bootdev: Failed to get PCI I/O protocol: %d\n", (int)status);
        return info->valid;
    }

    /* Get the actual PCI location */
    status = pci_io->GetLocation(pci_io, &seg, &bus, &dev, &func);
    if (EFI_ERROR(status)) {
        printf("bootdev: Failed to get PCI location: %d\n", (int)status);
        return info->valid;
    }

    info->bus = (uint8_t)bus;
    info->device = (uint8_t)dev;
    info->function = (uint8_t)func;
    info->valid = true;

    /* Read PCI class code from config space offset 0x0B (class) and 0x0A (subclass) */
    uint8_t class_code[2];
    status = pci_io->Pci.Read(pci_io, EfiPciIoWidthUint8, 0x0A, 2, class_code);
    if (!EFI_ERROR(status)) {
        info->pci_subclass = class_code[0];  /* Offset 0x0A */
        info->pci_class = class_code[1];     /* Offset 0x0B */
    }

    printf("bootdev: PCI location %02x:%02x.%x class=%02x subclass=%02x\n",
           info->bus, info->device, info->function, info->pci_class, info->pci_subclass);

    return true;
}

/*
 * Check if two boot device info structures match (same controller)
 */
static bool devices_match(const struct boot_device_info *a,
                         const struct boot_device_info *b)
{
    if (!a->valid || !b->valid) {
        return false;
    }

    return (a->bus == b->bus &&
            a->device == b->device &&
            a->function == b->function);
}

/*
 * Get device type string for debug output
 */
static const char *device_type_str(uint16_t type)
{
    switch (type) {
    case BBS_FLOPPY:    return "Floppy";
    case BBS_HARDDISK:  return "HDD";
    case BBS_CDROM:     return "CDROM";
    case BBS_PCMCIA:    return "PCMCIA";
    case BBS_USB:       return "USB";
    case BBS_EMBED_NETWORK: return "Network";
    default:            return "Unknown";
    }
}

/*
 * Does this disk carry an operating system to boot?
 *
 * A disk with no active partition cannot boot, and SeaBIOS will waste the whole
 * boot on it: its own decline test is the 0xAA55 signature at offset 0x1FE,
 * which the partition table also depends on. Removing the signature would hide
 * the partition holding CSMWrap from mkfs.vfat and from OVMF, which must find
 * it to load CSMWrap at all, so the disk is demoted in the BBS instead.
 *
 * The test is the active-partition flag rather than "is the boot code empty"
 * because it is the standard answer to this question and because it corrects
 * itself: once an installer writes a real MBR the flag is set and the disk
 * takes priority back.
 *
 * A read failure is reported as bootable, so a device that cannot be inspected
 * keeps the existing behaviour rather than being silently demoted.
 */
static bool disk_carries_boot_os(EFI_BLOCK_IO_PROTOCOL *block_io)
{
    uint8_t sector[512];
    EFI_STATUS status;
    int i;

    if (!block_io || !block_io->Media) {
        return true;
    }

    status = block_io->ReadBlocks(block_io, block_io->Media->MediaId, 0,
                                  sizeof(sector), sector);
    if (EFI_ERROR(status)) {
        printf("bootdev: sector 0 read failed (%d), treating disk as bootable\n",
               (int)status);
        return true;
    }

    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        printf("bootdev: sector 0 has no AA55 signature, not a partition table\n");
        return false;
    }

    for (i = 0; i < 4; i++) {
        const uint8_t *entry = &sector[0x1be + i * 16];

        /* Active flag set and a non-zero partition type: an installed OS. */
        if ((entry[0] & 0x80) && entry[4] != 0x00) {
            printf("bootdev: partition %d is active and typed, disk is bootable\n",
                   i + 1);
            return true;
        }
    }

    printf("bootdev: no active partition in sector 0, disk cannot boot\n");
    return false;
}

/*
 * Add a BBS entry for a block device
 *
 * index:    the slot the entry goes in. SeaBIOS does not look entries up by
 *           PCI location for ATA drives: it indexes the table as
 *           1 + channel * 2 + slave, with slot 0 reserved for the floppy
 *           controller and slot 5 onward for PCI devices. An entry written
 *           anywhere else is unreachable.
 * priority: BBS priority, 0 being highest. BBS_DO_NOT_BOOT_FROM maps to no
 *           priority at all and drops the entry back to SeaBIOS's own default,
 *           which is how a drive is stepped behind rather than excluded.
 */
static void add_bbs_entry(struct low_stub *low_stub,
                         const struct boot_device_info *info,
                         size_t index,
                         UINT16 priority)
{
    BBS_TABLE *entry;
    char *desc;

    if (index >= MAX_BBS_ENTRIES) {
        printf("bootdev: slot %zu is past the %d-entry table, skipping device\n",
               index, MAX_BBS_ENTRIES);
        return;
    }

    entry = &low_stub->bbs_entries[index];
    desc = low_stub->bbs_desc_strings[index];

    memset(entry, 0, sizeof(*entry));

    /* Set boot priority - 0 is highest, higher numbers = lower priority */
    entry->BootPriority = priority;

    /* PCI location */
    entry->Bus = info->bus;
    entry->Device = info->device;
    entry->Function = info->function;

    /* Device type */
    entry->DeviceType = info->device_type;

    /* PCI class codes - use actual values from device */
    entry->Class = info->pci_class;
    entry->SubClass = info->pci_subclass;

    /* Status flags - mark as enabled and media present */
    entry->StatusFlags.Enabled = 1;
    entry->StatusFlags.MediaPresent = 2;  /* Media present and bootable */

    /* Description string - stored in low memory */
    if (priority == 0) {
        snprintf(desc, BBS_DESC_STRING_SIZE, "Boot %s %02x:%02x.%x",
                 device_type_str(info->device_type),
                 info->bus, info->device, info->function);
    } else {
        snprintf(desc, BBS_DESC_STRING_SIZE, "%s %02x:%02x.%x",
                 device_type_str(info->device_type),
                 info->bus, info->device, info->function);
    }

    /* Set description string pointer (segment:offset for real mode) */
    uintptr_t desc_addr = (uintptr_t)desc;
    entry->DescStringSegment = EFI_SEGMENT(desc_addr);
    entry->DescStringOffset = EFI_OFFSET(desc_addr);

    printf("bootdev: BBS[%zu] %s pri=%u%s\n", index, desc, (unsigned)priority,
           priority == BBS_DO_NOT_BOOT_FROM ? " (demoted)" : "");

    /* Entries are addressed by slot, so the count has to cover the highest
     * slot written rather than assume they were appended in order. */
    if (index + 1 > low_stub->bbs_entry_count) {
        low_stub->bbs_entry_count = index + 1;
    }
}

/*
 * Slot layout of the BBS table, as SeaBIOS addresses it. It does not look
 * entries up by PCI location for ATA drives: in the CSM's SeaBIOS,
 * csm_bootprio_ata() computes 1 + channel * 2 + slave, csm_bootprio_fdc()
 * reads slot 0 and csm_bootprio_pci() scans from slot 5. Appending entries in
 * enumeration order therefore does not describe the machine: with one disk and
 * one CD sharing an IDE controller both landed in the first two slots and the
 * CD's own slot was never written, so it inherited whatever was there.
 */
#define BBS_SLOT_FDC       0
#define BBS_SLOT_ATA_BASE  1
#define BBS_SLOT_PCI       5

/*
 * Enumerate block I/O devices and build BBS entries
 */
static int enumerate_block_devices(struct low_stub *low_stub,
                                   const struct boot_device_info *boot_info)
{
    EFI_STATUS status;
    EFI_GUID block_io_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_HANDLE *handles = NULL;
    UINTN handle_count = 0;
    UINT16 next_priority = 1;  /* Priority 0 reserved for the boot target */
    bool top_taken = false;
    bool boot_disk_bootable = true;
    size_t next_pci_slot = BBS_SLOT_PCI;

    /* Find all block I/O devices */
    status = gBS->LocateHandleBuffer(ByProtocol, &block_io_guid, NULL,
                                     &handle_count, &handles);
    if (EFI_ERROR(status)) {
        printf("bootdev: Failed to locate block devices: %d\n", (int)status);
        return -1;
    }

    printf("bootdev: Found %lu block devices\n", (unsigned long)handle_count);

    /*
     * Whether the disk CSMWrap was loaded from can boot has to be settled before
     * anything is written: enumeration can reach the CD before that disk, and
     * the priority order must be decided up front.
     */
    if (boot_info->valid) {
        for (UINTN i = 0; i < handle_count; i++) {
            EFI_BLOCK_IO_PROTOCOL *block_io;
            struct boot_device_info dev_info;

            if (EFI_ERROR(gBS->HandleProtocol(handles[i], &block_io_guid,
                                              (void **)&block_io))) {
                continue;
            }
            if (block_io->Media && block_io->Media->LogicalPartition) {
                continue;
            }
            if (!get_pci_location(handles[i], &dev_info)) {
                continue;
            }
            if (!devices_match(&dev_info, boot_info)) {
                continue;
            }
            boot_disk_bootable = disk_carries_boot_os(block_io);
            break;
        }
    }

    /*
     * The floppy slot is read by csm_bootprio_fdc() whether or not an entry is
     * written, and left at zero it would tie with the CD for first place. The
     * medium CSMWrap booted from is the disk, not the floppy, so send any
     * floppy to the back of the order rather than let it compete. It stays
     * reachable as a last resort.
     */
    low_stub->bbs_entries[BBS_SLOT_FDC].BootPriority = BBS_UNPRIORITIZED_ENTRY;
    if (low_stub->bbs_entry_count < BBS_SLOT_FDC + 1) {
        low_stub->bbs_entry_count = BBS_SLOT_FDC + 1;
    }
    printf("bootdev: BBS[%d] floppy demoted to unprioritised\n", BBS_SLOT_FDC);

    for (UINTN i = 0; i < handle_count; i++) {
        EFI_BLOCK_IO_PROTOCOL *block_io;
        struct boot_device_info dev_info;
        bool is_boot_device;
        size_t slot;
        UINT16 priority;

        status = gBS->HandleProtocol(handles[i], &block_io_guid, (void **)&block_io);
        if (EFI_ERROR(status)) {
            continue;
        }

        /* Skip logical partitions, only want raw devices */
        if (block_io->Media->LogicalPartition) {
            continue;
        }

        /* Get PCI location for this device */
        if (!get_pci_location(handles[i], &dev_info)) {
            continue;
        }

          /*
           * An optical drive behind an IDE/SATA controller cannot be told apart
           * by PCI class: the class code belongs to the controller function and
           * reads class=01 subclass=01 for the disk and the CD alike. Its device
           * path ends in MSG_ATAPI_DP with no MEDIA_CDROM_DP node on this stack,
           * so without this the CD inherits the default BBS_HARDDISK type.
           * RemovableBlockMedia is per media and is the reliable signal. Note
           * that the type does not reach SeaBIOS's ATA lookup, which reads the
           * slot and the priority and never looks at DeviceType.
           */
          if (dev_info.device_type == BBS_HARDDISK &&
              block_io->Media != NULL && block_io->Media->RemovableMedia) {
              dev_info.device_type = BBS_CDROM;
          }

        /* Slot: where SeaBIOS will compute this drive's index. */
        if (dev_info.has_ata_position &&
            dev_info.ata_channel < 2 && dev_info.ata_slave < 2) {
            slot = BBS_SLOT_ATA_BASE + dev_info.ata_channel * 2 + dev_info.ata_slave;
        } else {
            slot = next_pci_slot++;
        }

        /*
         * devices_match() compares PCI location alone, and both drives of one
         * IDE controller share a BDF, so on its own it matches the CD as well as
         * the disk. Requiring the type as well is what makes "the device CSMWrap
         * booted from" name one drive rather than two.
         */
        is_boot_device = boot_info->valid &&
                         dev_info.device_type == boot_info->device_type &&
                         devices_match(&dev_info, boot_info);

        if (is_boot_device && boot_disk_bootable) {
            priority = 0;
        } else if (is_boot_device) {
            /* Nothing to boot here: step behind, do not exclude outright. */
            priority = BBS_DO_NOT_BOOT_FROM;
        } else if (!boot_disk_bootable && dev_info.device_type == BBS_CDROM) {
            /* Nothing on the boot disk, so the installer leads. */
            priority = 0;
        } else {
            priority = next_priority++;
        }

        /* Exactly one entry may hold priority 0. */
        if (priority == 0) {
            if (top_taken) {
                priority = next_priority++;
            } else {
                top_taken = true;
            }
        }

        add_bbs_entry(low_stub, &dev_info, slot, priority);
    }

    gBS->FreePool(handles);

    return 0;
}

/*
 * Main entry point: build BBS table for SeaBIOS
 */
int build_bbs_table(struct csmwrap_priv *priv, EFI_HANDLE image_handle)
{
    EFI_STATUS status;
    EFI_GUID loaded_image_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;
    struct boot_device_info boot_info = {0};
    struct low_stub *low_stub = priv->low_stub;

    if (!low_stub) {
        printf("bootdev: low_stub not initialized\n");
        return -1;
    }

    printf("bootdev: Building BBS table...\n");

    /* Get loaded image protocol to find boot device */
    status = gBS->HandleProtocol(image_handle, &loaded_image_guid,
                                  (void **)&loaded_image);
    if (EFI_ERROR(status) || !loaded_image) {
        printf("bootdev: Failed to get loaded image: %d\n", (int)status);
        /* Continue without boot device info - will enumerate all devices */
    } else if (loaded_image->DeviceHandle) {
        /* Get boot device information */
        printf("bootdev: Detecting boot device...\n");
        if (get_pci_location(loaded_image->DeviceHandle, &boot_info)) {
            printf("bootdev: Boot device: PCI %02x:%02x.%x type=%s\n",
                   boot_info.bus, boot_info.device, boot_info.function,
                   device_type_str(boot_info.device_type));
        }
    }

    /* Reset BBS table */
    low_stub->bbs_entry_count = 0;
    memset(low_stub->bbs_entries, 0, sizeof(low_stub->bbs_entries));
    memset(low_stub->bbs_desc_strings, 0, sizeof(low_stub->bbs_desc_strings));

    /* Enumerate all block devices and build BBS entries */
    if (enumerate_block_devices(low_stub, &boot_info) < 0) {
        printf("bootdev: Failed to enumerate block devices\n");
        /* Not fatal - SeaBIOS can still enumerate drives itself */
    }

    /* Set up boot_table to point to our BBS table */
    if (low_stub->bbs_entry_count > 0) {
        low_stub->boot_table.NumberBbsEntries = low_stub->bbs_entry_count;
        low_stub->boot_table.BbsTable = (uintptr_t)low_stub->bbs_entries;
        printf("bootdev: BBS table built with %zu entries\n", low_stub->bbs_entry_count);
    } else {
        printf("bootdev: No BBS entries created\n");
    }

    return 0;
}
