/* SPDX-License-Identifier: BSD-2-Clause
 *
 * MONITORX/MWAITX (AMD user-mode monitor-wait) helpers for the low-latency RX wait.
 *
 * These wrap the two instructions and the CPUID gate. They live in their own translation unit
 * ON PURPOSE: the RX wait arms MONITORX, then re-checks the monitored line with a pure load, then
 * MWAITX -- and that ordering must not be reordered or the monitored load hoisted. Cross-TU calls
 * are opaque to the optimizer, so the sequence is preserved without any inlining (and the build
 * uses no LTO). Raw asm is used so no -mmwaitx toolchain flag is required.
 *
 * MONITORX arms a monitor on the cache line at rAX (ECX=extensions, EDX=hints). MWAITX waits until
 * that line is written by ANY agent -- including a coherent DMA write -- or a timeout: EAX=hints
 * (C-state; 0 = shallow/fast wake), ECX=extensions (bit1 enables the EBX timeout), EBX=timeout in
 * TSC cycles. Unlike plain MONITOR/MWAIT, the 'X' variants are usable at CPL>0 (user mode).
 *
 * PORTABILITY: MONITORX/MWAITX, CPUID, and RDTSC are x86-only. On every other architecture this
 * translation unit compiles to stubs whose m2sdr_mwaitx_supported() returns 0, so the RX wait
 * transparently falls back to poll() (see m2sdr_stream.c) with no behavioural change on x86. This
 * keeps libm2sdr building on aarch64/arm and any other non-x86 host.
 */

#include <stdint.h>

#include "m2sdr_internal.h"

#if defined(__x86_64__) || defined(__i386__)

#include <cpuid.h>
#include <time.h>

/* CPUID Fn8000_0001_ECX[29] = MONITORX -> MONITORX/MWAITX are available (and user-mode usable). */
int m2sdr_mwaitx_supported(void)
{
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(0x80000001u, &eax, &ebx, &ecx, &edx))
        return 0;
    return (ecx & (1u << 29)) != 0;
}

void m2sdr_monitorx(const void *addr)
{
    __asm__ __volatile__("monitorx" : : "a"(addr), "c"(0), "d"(0));
}

void m2sdr_mwaitx(unsigned int extensions, unsigned int hints, unsigned int clocks)
{
    __asm__ __volatile__("mwaitx" : : "a"(hints), "c"(extensions), "b"(clocks));
}

/* TSC frequency in Hz, calibrated once against CLOCK_MONOTONIC and cached. Used to turn the
 * MWAITX safety-net timeout (a few hundred microseconds) into a TSC-cycle count. */
uint64_t m2sdr_tsc_hz(void)
{
    static uint64_t cached_hz = 0;
    if (cached_hz)
        return cached_hz;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t c0 = __builtin_ia32_rdtsc();
    nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 }, NULL);
    uint64_t c1 = __builtin_ia32_rdtsc();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
    if (ns <= 0.0)
        return 0;
    cached_hz = (uint64_t)((double)(c1 - c0) / (ns / 1e9));
    return cached_hz;
}

#else /* !x86: MONITORX/MWAITX/CPUID/RDTSC unavailable -> the RX wait uses poll() */

int m2sdr_mwaitx_supported(void) { return 0; }

void m2sdr_monitorx(const void *addr) { (void)addr; }

void m2sdr_mwaitx(unsigned int extensions, unsigned int hints, unsigned int clocks)
{
    (void)extensions; (void)hints; (void)clocks;
}

uint64_t m2sdr_tsc_hz(void) { return 0; }

#endif /* x86 */
