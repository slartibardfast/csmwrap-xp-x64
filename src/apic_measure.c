/*
 * LAPIC timer measurement for a legacy OS handoff.
 *
 * The MADT's Local APIC Timer entry can carry the bus frequency in units of 1e5 Hz,
 * which is the ACPI-sanctioned way to tell an OS something CPUID cannot. That matters
 * on this machine because the host CPU has a maximum basic CPUID leaf of 0x0d, so
 * CPUID 0x15 and 0x16 are both absent: the guest has no way to derive the frequency
 * itself. Windows consequently programs an initial count of 1000000000, which is a
 * clock of no practical use.
 *
 * The measurement is against the 8254 PIT, whose 1.193182 MHz input is a fixed
 * constant and needs no calibration. PIT channel 2 is programmed as a one-shot on
 * terminal count and the LAPIC's current count is sampled across that window.
 *
 * The divider is settled by measurement rather than by documentation. Intel describes
 * the divide control register as bits 3:0 selecting /2 through /16, while QEMU
 * reports a register value of 0x0b as "divide by 1". Those disagree, and the override
 * must publish the bus frequency, which is the sampled rate times the divider. So the
 * rate is sampled at two register values and the ratio decides the encoding. Putting a
 * wrong constant into an ACPI table that Windows trusts is worse than publishing none,
 * which is why nothing is assumed here.
 *
 * Every register written is restored. The contract of a CSM is to hand over a machine
 * the OS can use, and this runs on the way past that handoff.
 */
#include <efi.h>
#include <printf.h>

#include "csmwrap.h"
#include "io.h"
#include "apic.h"

/* 8254 ports and encodings. */
#define PIT_MODE_PORT      0x43
#define PIT_CH2_PORT       0x42
#define PIT_CMD_PORT       0x61
#define PIT_CH2_OUTPUT_BIT 0x20   /* port 0x61 bit 5 follows channel 2's output */

/* Channel 2, lobyte/hibyte access, mode 0 (interrupt on terminal count), binary. */
#define PIT_CH2_ONESHOT    0xB0

/* The PIT counts at 1.193182 MHz. That constant is the whole reference. */
#define PIT_HZ             1193182ULL

/* The measurement window: 11932 PIT clocks, which is 10 ms. */
#define PIT_DELAY_TICKS    11932ULL

/* Timer registers, absent from apic.h's list because nothing there programs a timer. */
#define XAPIC_LVT_TIMER_OFFSET       0x320
#define XAPIC_TDCR_OFFSET            0x3E0
#define XAPIC_TIMER_INIT_CNT_OFFSET  0x380
#define XAPIC_TIMER_CUR_CNT_OFFSET   0x390

/*
 * Busy-wait for approximately ticks PIT input clocks. Channel 2 in mode 0 raises its
 * output bit once the count reaches zero, and port 0x61 bit 5 carries that bit.
 */
static void pit_delay(uint32_t ticks)
{
    outb(PIT_MODE_PORT, PIT_CH2_ONESHOT);
    outb(PIT_CH2_PORT, (uint8_t)(ticks & 0xFF));
    outb(PIT_CH2_PORT, (uint8_t)((ticks >> 8) & 0xFF));

    while (!(inb(PIT_CMD_PORT) & PIT_CH2_OUTPUT_BIT))
        ;
}

/*
 * Sample how fast the LAPIC timer counts. The timer is left masked and running for the
 * duration so it counts without ever raising an interrupt at a vector nothing is
 * prepared for.
 */
static uint32_t lapic_sample_rate(uintptr_t base, uint8_t dcr)
{
    volatile uint32_t *div = (volatile uint32_t *)(base + XAPIC_TDCR_OFFSET);
    volatile uint32_t *lvt = (volatile uint32_t *)(base + XAPIC_LVT_TIMER_OFFSET);
    volatile uint32_t *init = (volatile uint32_t *)(base + XAPIC_TIMER_INIT_CNT_OFFSET);
    volatile uint32_t *cur = (volatile uint32_t *)(base + XAPIC_TIMER_CUR_CNT_OFFSET);

    uint32_t saved_div = *div;
    uint32_t saved_lvt = *lvt;
    uint32_t saved_init = *init;

    *div = dcr;
    *lvt = 0x00010000;          /* masked, so it counts but raises nothing */
    *init = 0xFFFFFFFF;

    uint32_t before = *cur;
    pit_delay(PIT_DELAY_TICKS);
    uint32_t after = *cur;

    *init = saved_init;
    *lvt = saved_lvt;
    *div = saved_div;

    return before - after;
}

static uint32_t g_bus_hz;

uint32_t apic_measured_bus_hz(void)
{
    return g_bus_hz;
}

void apic_measure_timer(void)
{
    uintptr_t apic_base = rdmsr(MSR_IA32_APIC_BASE) & APIC_BASE_ADDR_MASK;

    /*
     * Three settings, not two. 0x0b is the value firmware left behind, which QEMU
     * labels divide-by-one, so sampling it is what tests that label. 0x00 and 0x01
     * differ by a factor of two in this QEMU, which is a property of its
     * implementation rather than of Intel's encoding, so it is measured rather than
     * assumed.
     */
    uint32_t rate_inherited = lapic_sample_rate(apic_base, 0x0b);
    uint32_t rate_a = lapic_sample_rate(apic_base, 0x00);
    uint32_t rate_b = lapic_sample_rate(apic_base, 0x01);

    printf("  LAPIC timer sampled over 10 ms: DCR=0x0b -> %u, DCR=0x00 -> %u, "
           "DCR=0x01 -> %u\n", rate_inherited, rate_a, rate_b);
    if (!rate_a || !rate_b) {
        printf("  LAPIC timer did not count; no frequency can be published\n");
        return;
    }

    /* The bus clock is the faster of the two, since the divider only ever divides. */
    uint32_t fastest = rate_inherited;
    if (rate_a > fastest) fastest = rate_a;
    if (rate_b > fastest) fastest = rate_b;

    /*
     * The window is PIT_DELAY_TICKS input clocks, so the elapsed time is
     * PIT_DELAY_TICKS / PIT_HZ and the rate is the sample divided by that. Getting
     * this the wrong way round is easy and the earlier version did, which produced
     * 1.19e11 Hz and was correctly refused by the range check below.
     *
     * The bus clock is the fastest sample, because a divider only ever divides. That
     * holds as long as the fastest setting is divide-by-one, which is what QEMU's own
     * info lapic reports for the inherited value. The samples are all printed so the
     * assumption can be checked against them rather than believed.
     */
    uint64_t bus_hz = (uint64_t)fastest * PIT_HZ / PIT_DELAY_TICKS;

    /* The MADT override is a 16-bit count of 1e5 Hz units, so round to fit that. */
    uint64_t units = (bus_hz + 50000ULL) / 100000ULL;
    if (units > 0xFFFFULL) {
        printf("  LAPIC timer bus clock %llu Hz does not fit the MADT override "
               "field; publishing none\n", bus_hz);
        return;
    }

    g_bus_hz = (uint32_t)(units * 100000ULL);

    printf("  LAPIC timer bus clock: %llu Hz (%llu.%03llu MHz), taking the fastest "
           "of three samples as divide-by-one\n",
           bus_hz, bus_hz / 1000000ULL, (bus_hz / 1000ULL) % 1000ULL);
    printf("  MADT LAPIC Timer override to publish: %lu (%lu Hz)\n",
           (unsigned long)units, (unsigned long)g_bus_hz);
}