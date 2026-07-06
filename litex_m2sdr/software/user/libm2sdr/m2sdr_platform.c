/* SPDX-License-Identifier: BSD-2-Clause */

#include "m2sdr_platform.h"

#include <errno.h>
#include <stdlib.h>

#if defined(_WIN32)

/* Device open/close and stream setup can run on different threads, so the
 * Winsock reference count needs real synchronization. */
static SRWLOCK m2sdr_winsock_lock = SRWLOCK_INIT;
static int m2sdr_winsock_refs;

static void m2sdr_sleep_ms(uint64_t ms)
{
    while (ms > 0) {
        DWORD chunk = ms > 0x7fffffffULL ? 0x7fffffffUL : (DWORD)ms;

        Sleep(chunk);
        ms -= chunk;
    }
}

int m2sdr_platform_socket_init(void)
{
    WSADATA wsa;
    int ret = 0;

    AcquireSRWLockExclusive(&m2sdr_winsock_lock);
    if (m2sdr_winsock_refs == 0 && WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        ret = -1;
    else
        m2sdr_winsock_refs++;
    ReleaseSRWLockExclusive(&m2sdr_winsock_lock);
    return ret;
}

void m2sdr_platform_socket_cleanup(void)
{
    AcquireSRWLockExclusive(&m2sdr_winsock_lock);
    if (m2sdr_winsock_refs > 0 && --m2sdr_winsock_refs == 0)
        WSACleanup();
    ReleaseSRWLockExclusive(&m2sdr_winsock_lock);
}

int m2sdr_clock_gettime(int clk_id, struct timespec *ts)
{
    if (!ts)
        return -1;

    if (clk_id == CLOCK_MONOTONIC) {
        uint64_t us = m2sdr_monotonic_us();

        ts->tv_sec = (time_t)(us / 1000000ULL);
        ts->tv_nsec = (long)((us % 1000000ULL) * 1000ULL);
        return 0;
    }

    if (clk_id == CLOCK_REALTIME) {
        FILETIME ft;
        ULARGE_INTEGER uli;
        uint64_t unix_100ns;

        GetSystemTimeAsFileTime(&ft);
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        unix_100ns = uli.QuadPart - 116444736000000000ULL;
        ts->tv_sec = (time_t)(unix_100ns / 10000000ULL);
        ts->tv_nsec = (long)((unix_100ns % 10000000ULL) * 100ULL);
        return 0;
    }

    errno = EINVAL;
    return -1;
}

int m2sdr_socket_close(m2sdr_socket_t sock)
{
    return closesocket(sock);
}

int m2sdr_socket_set_nonblock(m2sdr_socket_t sock, int nonblock)
{
    u_long mode = nonblock ? 1u : 0u;

    return ioctlsocket(sock, FIONBIO, &mode);
}

int m2sdr_poll(m2sdr_pollfd *fds, unsigned long nfds, int timeout_ms)
{
    return WSAPoll(fds, nfds, timeout_ms);
}

int m2sdr_socket_last_error(void)
{
    return WSAGetLastError();
}

int m2sdr_socket_error_is_would_block(int err)
{
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}

int m2sdr_socket_error_is_interrupted(int err)
{
    return err == WSAEINTR;
}

void m2sdr_sleep_us(unsigned int usec)
{
    /* Sleep() rounds to the scheduler tick (~1-15.6 ms); busy-wait short
     * delays so the AD9361 driver's udelay()-scale calls do not inflate
     * ~1000x and blow the RF init deadline. */
    if (usec < 2000) {
        uint64_t now = m2sdr_monotonic_us();

        if (now) {
            uint64_t deadline = now + usec;

            while (m2sdr_monotonic_us() < deadline)
                YieldProcessor();
            return;
        }
    }
    m2sdr_sleep_ms(((uint64_t)usec + 999ULL) / 1000ULL);
}

unsigned int m2sdr_sleep(unsigned int seconds)
{
    m2sdr_sleep_ms((uint64_t)seconds * 1000ULL);
    return 0;
}

int m2sdr_nanosleep(const struct timespec *req, struct timespec *rem)
{
    uint64_t ms;

    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    ms = (uint64_t)req->tv_sec * 1000ULL + ((uint64_t)req->tv_nsec + 999999ULL) / 1000000ULL;
    m2sdr_sleep_ms(ms);
    return 0;
}

uint64_t m2sdr_monotonic_us(void)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER count;
    uint64_t ticks;
    uint64_t hz;

    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&count))
        return 0;
    /* Split the conversion: ticks * 1e6 overflows 64 bits after ~21 days of
     * uptime at the standard 10 MHz counter frequency (sooner on TSC-backed
     * counters). */
    ticks = (uint64_t)count.QuadPart;
    hz    = (uint64_t)freq.QuadPart;
    return (ticks / hz) * 1000000ULL + ((ticks % hz) * 1000000ULL) / hz;
}

void *m2sdr_aligned_malloc(size_t alignment, size_t size)
{
    return _aligned_malloc(size, alignment);
}

void m2sdr_aligned_free(void *ptr)
{
    _aligned_free(ptr);
}

#else

#include <fcntl.h>
#include <unistd.h>

int m2sdr_platform_socket_init(void)
{
    return 0;
}

void m2sdr_platform_socket_cleanup(void)
{
}

int m2sdr_socket_close(m2sdr_socket_t sock)
{
    return close(sock);
}

int m2sdr_socket_set_nonblock(m2sdr_socket_t sock, int nonblock)
{
    int flags = fcntl(sock, F_GETFL, 0);

    if (flags < 0)
        return -1;
    if (nonblock)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    return fcntl(sock, F_SETFL, flags);
}

int m2sdr_poll(m2sdr_pollfd *fds, unsigned long nfds, int timeout_ms)
{
    return poll(fds, (nfds_t)nfds, timeout_ms);
}

int m2sdr_socket_last_error(void)
{
    return errno;
}

int m2sdr_socket_error_is_would_block(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK;
}

int m2sdr_socket_error_is_interrupted(int err)
{
    return err == EINTR;
}

void m2sdr_sleep_us(unsigned int usec)
{
    usleep(usec);
}

unsigned int m2sdr_sleep(unsigned int seconds)
{
    return sleep(seconds);
}

int m2sdr_nanosleep(const struct timespec *req, struct timespec *rem)
{
    return nanosleep(req, rem);
}

uint64_t m2sdr_monotonic_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void *m2sdr_aligned_malloc(size_t alignment, size_t size)
{
    void *ptr = NULL;

    if (posix_memalign(&ptr, alignment, size) != 0)
        return NULL;
    return ptr;
}

void m2sdr_aligned_free(void *ptr)
{
    free(ptr);
}

#endif

int64_t m2sdr_monotonic_ms(void)
{
    uint64_t us = m2sdr_monotonic_us();

    return us ? (int64_t)(us / 1000ULL) : 0;
}
