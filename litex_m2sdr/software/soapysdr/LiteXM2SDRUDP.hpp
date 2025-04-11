/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2025 Enjoy Digital.
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef LITEXM2SDRUDP_HPP
#define LITEXM2SDRUDP_HPP
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

class LiteXM2SDRUDP {
    public:
        /* Constructor:
         * ip_addr (string): server addr
         * port (string):    server port
         * min_size:         minimum queue length before popping buffer
         * max_size:         maximum queue size (avoid to increase indefinitively the queue)
         * buffer_size:      size of each queue slots (in byte)
         */
        LiteXM2SDRUDP(std::string ip_addr, std::string port,
            size_t min_size, size_t max_size, size_t buffer_size, uint32_t bytesPerComplex);
        /* Destructor
         */
        ~LiteXM2SDRUDP(void);

        /* Thread method for acquisition and buffer filling */
        void rx_callback();

        /* Start thread acquisition */
        void start(const int direction);
        /* Stop thread acquisition */
        void stop(const int direction);

        /* Return a vector with buffer_size char or
         * and empty vector
         */
        std::vector<char> get_data(void);

        /* Send data
         */
        void set_data(const int8_t *data, const uint32_t length);

        size_t buffers_available(void) {
            std::unique_lock<std::mutex> lock(_mtx);
            size_t nb_buff = _buffer.size() - _min_size - 1;
            if (nb_buff <= 0)
                return 0;
            else
                return nb_buff;
        }

        size_t tx_buffers_available(void);

        void set_samplerate(float samplerate) {_samplerate = samplerate; }

        /* return true when one or more overflow, false otherwise */
        bool overflow() { return !(_overflow == 0); }
        size_t buffer_count() {return _max_size;}
        size_t buffer_size()  {return _buffer_size;}

    private:
        /* Fill queue with buffer_size char */
        bool add_data(const std::vector<char> &data);
        int _rxtx_sock;         /* UDP socket */
        bool _tx_running;       /* loop until goes false */
        bool _rx_running;       /* loop until goes false */
        struct addrinfo *_addr; /* UDP related */
        std::thread _thread;    /* thread used to receive data */
        std::mutex _mtx;        /* mutex to lock fifo access */
        size_t _max_size;       /* queue max len */
        size_t _min_size;       /* queue min len before pop */
        size_t _buffer_size;    /* size of a buffer per queue slots */
        std::queue<std::vector<char>> _buffer;
        size_t _overflow;
        bool _tx_started;
        bool _rx_started;
        struct sockaddr_in server_addr;
        std::chrono::high_resolution_clock::time_point _start_time;
        float _samplerate;
};

#endif /* LITEXM2SDRUDPRX_HPP */
