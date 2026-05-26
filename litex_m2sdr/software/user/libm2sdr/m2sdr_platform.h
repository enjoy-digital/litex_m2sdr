/* SPDX-License-Identifier: BSD-2-Clause
 *
 * Small host portability layer for the M2SDR userspace tools.
 */

#ifndef M2SDR_PLATFORM_H
#define M2SDR_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <malloc.h>
#include <io.h>
#include <BaseTsd.h>

#define M2SDR_PLATFORM_WINDOWS 1
#define M2SDR_SOCKET_INVALID INVALID_SOCKET

typedef SOCKET m2sdr_socket_t;
typedef WSAPOLLFD m2sdr_pollfd;
#ifndef _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif
#if defined(_MSC_VER)
typedef unsigned int useconds_t;
#endif
#if defined(_MSC_VER) && !defined(_OFF_T_DEFINED)
typedef __int64 off_t;
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef usleep
#define usleep(usec) m2sdr_sleep_us((unsigned int)(usec))
#endif
#ifndef sleep
#define sleep(seconds) m2sdr_sleep((unsigned int)(seconds))
#endif
#ifndef nanosleep
#define nanosleep(req, rem) m2sdr_nanosleep((req), (rem))
#endif
#ifndef clock_gettime
#define clock_gettime m2sdr_clock_gettime
#endif

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#ifndef fseeko
#define fseeko _fseeki64
#endif
#ifndef ftello
#define ftello _ftelli64
#endif
#ifndef access
#define access _access
#endif
#ifndef isatty
#define isatty _isatty
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef F_OK
#define F_OK 0
#endif

#ifndef aligned_alloc
#define aligned_alloc(alignment, size) m2sdr_aligned_malloc((alignment), (size))
#endif

static inline size_t m2sdr_strnlen(const char *s, size_t maxlen)
{
    size_t n = 0;

    if (!s)
        return 0;
    while (n < maxlen && s[n])
        n++;
    return n;
}

#ifndef strnlen
#define strnlen m2sdr_strnlen
#endif

static inline struct tm *m2sdr_localtime_r(const time_t *t, struct tm *tmv)
{
    return localtime_s(tmv, t) == 0 ? tmv : NULL;
}

static inline struct tm *m2sdr_gmtime_r(const time_t *t, struct tm *tmv)
{
    return gmtime_s(tmv, t) == 0 ? tmv : NULL;
}

#ifndef localtime_r
#define localtime_r m2sdr_localtime_r
#endif
#ifndef gmtime_r
#define gmtime_r m2sdr_gmtime_r
#endif

#ifndef htobe32
#define htobe32(x) htonl((uint32_t)(x))
#endif
#ifndef be32toh
#define be32toh(x) ntohl((uint32_t)(x))
#endif

#else

#define M2SDR_PLATFORM_POSIX 1
#define M2SDR_SOCKET_INVALID (-1)

#include <errno.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#ifndef htobe32
#define htobe32(x) OSSwapHostToBigInt32((uint32_t)(x))
#endif
#ifndef be32toh
#define be32toh(x) OSSwapBigToHostInt32((uint32_t)(x))
#endif
#else
#if defined(__FreeBSD__)
#include <sys/endian.h>
#else
#include <endian.h>
#endif
#endif

typedef int m2sdr_socket_t;
typedef struct pollfd m2sdr_pollfd;

#endif

#ifdef __cplusplus
extern "C" {
#endif

int m2sdr_platform_socket_init(void);
void m2sdr_platform_socket_cleanup(void);
int m2sdr_clock_gettime(int clk_id, struct timespec *ts);
int m2sdr_socket_close(m2sdr_socket_t sock);
int m2sdr_socket_set_nonblock(m2sdr_socket_t sock, int nonblock);
int m2sdr_poll(m2sdr_pollfd *fds, unsigned long nfds, int timeout_ms);
int m2sdr_socket_last_error(void);
int m2sdr_socket_error_is_would_block(int err);
int m2sdr_socket_error_is_interrupted(int err);
void m2sdr_sleep_us(unsigned int usec);
unsigned int m2sdr_sleep(unsigned int seconds);
int m2sdr_nanosleep(const struct timespec *req, struct timespec *rem);
uint64_t m2sdr_monotonic_us(void);
int64_t m2sdr_monotonic_ms(void);
void *m2sdr_aligned_malloc(size_t alignment, size_t size);
void m2sdr_aligned_free(void *ptr);

#if !defined(USE_LITEPCIE)
#ifndef get_time_ms
#define get_time_ms m2sdr_monotonic_ms
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* M2SDR_PLATFORM_H */
