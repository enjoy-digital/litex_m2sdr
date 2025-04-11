/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2025 Enjoy Digital.
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <stdexcept>
#include <cstring>
#include <mutex>
#include <queue>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <SoapySDR/Constants.h>

#include <errno.h>
#include <unistd.h>

#include "LiteXM2SDRUDP.hpp"

LiteXM2SDRUDP::LiteXM2SDRUDP(std::string ip_addr, std::string port, size_t min_size, size_t max_size, size_t buffer_size,
    uint32_t bytesPerComplex):
    _rxtx_sock(-1), _tx_running(true), _rx_running(true),
    _max_size(max_size), _min_size(min_size), _buffer_size(buffer_size * bytesPerComplex), _overflow(0), _tx_started(false), _rx_started(false)
{
    /* Prepare Read/Write streams socket */
    struct addrinfo hints;
    int err;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags    = AI_ADDRCONFIG;
    err = getaddrinfo(ip_addr.c_str(), port.c_str(), &hints, &_addr);
    if (err != 0) {
        char mess[256];
        snprintf(mess, 256, "failed to _addrolve remote socket add_addrs (err=%d / %s)\n",
            err, gai_strerror(err));
        throw std::runtime_error(mess);
    }

    struct sockaddr_in si_read;
    memset((char *) &si_read, 0, sizeof(si_read));
    si_read.sin_family = _addr->ai_family;
    si_read.sin_port   = ((struct sockaddr_in *)_addr->ai_addr)->sin_port;
    si_read.sin_addr.s_addr = htobe32(INADDR_ANY);

    /* Read Socket */
    _rxtx_sock = socket(_addr->ai_family, _addr->ai_socktype, _addr->ai_protocol);
    if (_rxtx_sock == -1) {
        char mess[256];
        snprintf(mess, 256, "Unable to create Rx socket: %s\n", strerror(errno));
        throw std::runtime_error(mess);
    }

    if (bind(_rxtx_sock, (struct sockaddr*)&si_read, sizeof(si_read)) == -1) {
        char mess[256];
        snprintf(mess, 256, "Unable to bind Rx socket to port: %s\n", strerror(errno));
        close(_rxtx_sock);
        freeaddrinfo(_addr);
        throw std::runtime_error(mess);
    }

    // Initialize server address structure (destination)
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    unsigned long val = strtoul(port.c_str(), NULL, 10);
    server_addr.sin_port = htons(val);
    if (inet_pton(AF_INET, ip_addr.c_str(), &server_addr.sin_addr) <= 0) {
        char mess[256];
        snprintf(mess, 256, "Unable to create server socket to port: %s\n", strerror(errno));
        close(_rxtx_sock);
        freeaddrinfo(_addr);
        throw std::runtime_error(mess);
    }
}

LiteXM2SDRUDP::~LiteXM2SDRUDP(void)
{
    /* Thread must be disabled/stopped to avoid errors */
    stop(SOAPY_SDR_RX);
    stop(SOAPY_SDR_TX);
    /* Close socket */
    close(_rxtx_sock);
    freeaddrinfo(_addr);
}

bool LiteXM2SDRUDP::add_data(const std::vector<char>& data)
{
    std::unique_lock<std::mutex> lock(_mtx);
    if (_buffer.size() >= _max_size) {
        _overflow++;
        return false;
    }
    _buffer.push(data);
    _overflow = 0;
    return true;
}

std::vector<char> LiteXM2SDRUDP::get_data()
{
#ifdef USE_THREAD
    std::unique_lock<std::mutex> lock(_mtx);
    if (_buffer.size() < _min_size)
        return std::vector<char>();

    std::vector<char> data = _buffer.front();
    _buffer.pop();
#else
    size_t pos = 0;
    char bytes[_buffer_size];
    while(pos < _buffer_size) {
        int nb = recvfrom(_rxtx_sock, &bytes[pos], _buffer_size - pos, 0, NULL, NULL);
        if (nb == -1) {
            char mess[256];
            snprintf(mess, 256, "socket error: %s", strerror(errno));
            throw std::runtime_error(mess);
        }
        pos += nb;
    }
    std::vector<char> data(bytes, bytes+ pos);
#endif

    return data;
}

void LiteXM2SDRUDP::set_data(const int8_t *data, uint32_t length)
{
    size_t pos = 0;
    while(pos < length) {
        int nb = sendto(_rxtx_sock, &data[pos], length - pos, 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
        if (nb == -1) {
            char mess[256];
            snprintf(mess, 256, "socket error: %s", strerror(errno));
            throw std::runtime_error(mess);
        }
        pos += nb;
    }
}

void LiteXM2SDRUDP::rx_callback(void)
{
    size_t pos = 0;
    char bytes[_buffer_size];
    while (_rx_running) {
        int nb = recvfrom(_rxtx_sock, &bytes[pos], _buffer_size - pos, 0, NULL, NULL);
        if (nb == -1) {
            char mess[256];
            snprintf(mess, 256, "socket error: %s", strerror(errno));
            throw std::runtime_error(mess);
        }
        pos += nb;
        if (pos >= _buffer_size) {
            std::vector<char> data(bytes, bytes+ pos);
            add_data(data);
            pos = 0;
        }
    }
}

void LiteXM2SDRUDP::start(const int direction)
{
    if (direction == SOAPY_SDR_RX) {
        if (_rx_started)
            return;
        _rx_started = true;
#ifdef USE_THREAD
        _thread = std::thread(&LiteXM2SDRUDP::rx_callback, this);
#endif
    } else {
        if (_tx_started)
            return;
        _start_time = std::chrono::high_resolution_clock::now();
        _tx_started = true;
    }
}

void LiteXM2SDRUDP::stop(const int direction)
{
    if (direction == SOAPY_SDR_RX) {
        if (!_rx_started)
            return;
        _rx_started = false;
        /* tells thread to stop after next recv */
        _rx_running = false;
#ifdef USE_THREAD
        /* wait thread end completion before return
         * we must ensure thread is stopped before closing socket
         * to avoid "bad file descriptor" error
         */
        if (_thread.joinable())
            _thread.join();
#endif
    } else {
        if (!_tx_started)
            return;
        _tx_started = false;
        /* tells thread to stop after next recv */
        _tx_running = false;
    }
}

size_t LiteXM2SDRUDP::tx_buffers_available(void)
{
    std::chrono::high_resolution_clock::time_point time2 = std::chrono::high_resolution_clock::now();
    std::chrono::nanoseconds duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time2 - _start_time);
    long long duration = duration_ns.count();

    long long delay = static_cast<long long>((1e9/_samplerate) * buffer_size());
    if (duration < delay)
        std::this_thread::sleep_for(std::chrono::nanoseconds(delay - duration));

    _start_time = std::chrono::high_resolution_clock::now();
    return 1;
}
