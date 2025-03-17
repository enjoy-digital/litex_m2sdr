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

#include <netdb.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <errno.h>
#include <unistd.h>

#include "LiteXM2SDRUDPRx.hpp"

LiteXM2SDRUPDRx::LiteXM2SDRUPDRx(std::string ip_addr, std::string port, size_t min_size, size_t max_size, size_t buffer_size,
    uint32_t bytesPerComplex):
    _read_sock(-1), _running(true),
    _max_size(max_size), _min_size(min_size), _buffer_size(buffer_size * bytesPerComplex), _overflow(0), _started(false)
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
    _read_sock = socket(_addr->ai_family, _addr->ai_socktype, _addr->ai_protocol);
    if (_read_sock == -1) {
        char mess[256];
        snprintf(mess, 256, "Unable to create Rx socket: %s\n", strerror(errno));
        throw std::runtime_error(mess);
    }

    if (bind(_read_sock, (struct sockaddr*)&si_read, sizeof(si_read)) == -1) {
        char mess[256];
        snprintf(mess, 256, "Unable to bind Rx socket to port: %s\n", strerror(errno));
        close(_read_sock);
        freeaddrinfo(_addr);
        throw std::runtime_error(mess);
    }
}

LiteXM2SDRUPDRx::~LiteXM2SDRUPDRx(void)
{
    /* Thread must be disabled/stopped to avoid errors */
    stop();
    /* Close socket */
    close(_read_sock);
    freeaddrinfo(_addr);
}

bool LiteXM2SDRUPDRx::add_data(const std::vector<char>& data)
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

std::vector<char> LiteXM2SDRUPDRx::get_data()
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
        int nb = recvfrom(_read_sock, &bytes[pos], _buffer_size - pos, 0, NULL, NULL);
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

void LiteXM2SDRUPDRx::rx_callback(void)
{
    size_t pos = 0;
    char bytes[_buffer_size];
    while (_running) {
        int nb = recvfrom(_read_sock, &bytes[pos], _buffer_size - pos, 0, NULL, NULL);
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

void LiteXM2SDRUPDRx::start(void)
{
    if (_started)
        return;
    _started = true;
#ifdef USE_THREAD
    _thread = std::thread(&LiteXM2SDRUPDRx::rx_callback, this);
#endif
}

void LiteXM2SDRUPDRx::stop(void)
{
    if (!_started)
        return;
    _started = false;
    /* tells thread to stop after next recv */
    _running = false;
#ifdef USE_THREAD
    /* wait thread end completion before return
     * we must ensure thread is stopped before closing socket
     * to avoid "bad file descriptor" error
     */
    if (_thread.joinable())
        _thread.join();
#endif
}
