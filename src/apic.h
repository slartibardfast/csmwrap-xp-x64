/*
 * APIC handling for legacy BIOS compatibility
 */

#ifndef _APIC_H
#define _APIC_H

/* IA32_APIC_BASE, and the field within it that holds the MMIO base address. Shared with
 * apic.c and apic_measure.c so the value is defined once. */
#define MSR_IA32_APIC_BASE      0x1B
#define APIC_BASE_ADDR_MASK     0xFFFFFFFFFFFFF000ULL

/*
 * Prepare APIC for legacy BIOS operation.
 *
 * Disables LAPIC or configures it for ExtINT passthrough so that
 * legacy 8259 PIC interrupts (especially IRQ0 timer) can reach the CPU.
 *
 * Must be called after ExitBootServices but before CSM initialization.
 */
void apic_prepare_for_legacy(void);

/*
 * Sample the LAPIC timer against the 8254 PIT and report the result on the serial log.
 * Runs before the MADT is built, because a MADT Local APIC Timer entry can only carry a
 * frequency that has already been measured. Takes no argument: it reads the APIC base
 * from IA32_APIC_BASE itself, so callers need no MSR access.
 */
void apic_measure_timer(void);

/*
 * The LAPIC timer bus clock measured by apic_measure_timer(), in Hz, or 0 when the
 * measurement did not produce a value that fits the MADT override field.
 */
uint32_t apic_measured_bus_hz(void);

#endif /* _APIC_H */
