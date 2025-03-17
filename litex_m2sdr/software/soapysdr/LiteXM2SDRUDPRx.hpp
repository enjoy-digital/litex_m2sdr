/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2025 Enjoy Digital.
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#ifndef LITEXM2SDRUDPRX_HPP
#define LITEXM2SDRUDPRX_HPP
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <vector>

class LiteXM2SDRUPDRx {
    public:
        /* Constructor:
         * ip_addr (string): server addr
         * port (string):    server port
         * min_size:         minimum queue length before popping buffer
         * max_size:         maximum queue size (avoid to increase indefinitively the queue)
         * buffer_size:      size of each queue slots (in byte)
         */
        LiteXM2SDRUPDRx(std::string ip_addr, std::string port,
            size_t min_size, size_t max_size, size_t buffer_size, uint32_t bytesPerComplex);
        /* Destructor
         */
        ~LiteXM2SDRUPDRx(void);

        /* Thread method for acquisition and buffer filling */
        void rx_callback();

        /* Start thread acquisition */
        void start();
        void stop();

        /* Return a vector with buffer_size char or
         * and empty vector
         */
        std::vector<char> get_data(void);

        size_t buffers_available(void) {
            std::unique_lock<std::mutex> lock(_mtx);
            size_t nb_buff = _buffer.size() - _min_size - 1;
            if (nb_buff <= 0)
                return 0;
            else
                return nb_buff;
        }

        /* return true when one or more overflow, false otherwise */
        bool overflow() { return !(_overflow == 0); }
        size_t buffer_count() {return _max_size;}
        size_t buffer_size()  {return _buffer_size;}

    private:
        /* Fill queue with buffer_size char */
        bool add_data(const std::vector<char> &data);
        int _read_sock;         /* UDP socket */
        bool _running;          /* loop until goes false */
        struct addrinfo *_addr; /* UDP related */
        std::thread _thread;    /* thread used to receive data */
        std::mutex _mtx;        /* mutex to lock fifo access */
        size_t _max_size;       /* queue max len */
        size_t _min_size;       /* queue min len before pop */
        size_t _buffer_size;    /* size of a buffer per queue slots */
        std::queue<std::vector<char>> _buffer;
        size_t _overflow;
        bool _started;
};

#endif /* LITEXM2SDRUDPRX_HPP */
