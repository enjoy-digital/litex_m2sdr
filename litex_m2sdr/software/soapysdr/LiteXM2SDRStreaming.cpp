/*
 * SoapySDR driver for the LiteX M2SDR.
 *
 * Copyright (c) 2021-2025 Enjoy Digital.
 * Copyright (c) 2021 Julia Computing.
 * Copyright (c) 2015-2015 Fairwaves, Inc.
 * Copyright (c) 2015-2015 Rice University
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 */

#include <chrono>
#include <cassert>
#include <thread>
#include <sys/mman.h>

#include "ad9361/ad9361.h"
#include "ad9361/ad9361_api.h"

#include "LiteXM2SDRDevice.hpp"

/* RX DMA Header */
#if USE_LITEPCIE && defined(_RX_DMA_HEADER_TEST)
static constexpr size_t RX_DMA_HEADER_SIZE = 16;
#else
static constexpr size_t RX_DMA_HEADER_SIZE = 0;
#endif

/* TX DMA Header */
#if USE_LITEPCIE && defined(_TX_DMA_HEADER_TEST)
static constexpr size_t TX_DMA_HEADER_SIZE = 16;
#else
static constexpr size_t TX_DMA_HEADER_SIZE = 0;
#endif

/* Setup and configure a stream for RX or TX. */
SoapySDR::Stream *SoapyLiteXM2SDR::setupStream(
    const int direction,
    const std::string &format,
    const std::vector<size_t> &channels,
    const SoapySDR::Kwargs &args) {
    std::lock_guard<std::mutex> lock(_mutex);

    SoapySDR::Kwargs searchArgs = args;
    if (searchArgs.empty())
        searchArgs = _deviceArgs;

    /* Variable to hold the selected channels */
    std::vector<size_t> selected_channels;

    if (direction == SOAPY_SDR_RX) {
        if (_rx_stream.opened) {
            throw std::runtime_error("RX stream already opened.");
        }

        /* Configure the file descriptor watcher. */
#if USE_LITEPCIE
        _rx_stream.fds.fd     = _fd;
        _rx_stream.dma.fds.fd = _fd;
#endif
        _rx_stream.fds.events = POLLIN;

#if USE_LITEPCIE
        /* Initialize RX DMA Writer */
        _rx_stream.dma.shared_fd  = 1;
        _rx_stream.dma.use_reader = 0;
        _rx_stream.dma.use_writer = 1;
        _rx_stream.dma.loopback   = 0;
        _rx_stream.dma.zero_copy  = 1;
        if (litepcie_dma_init(&_rx_stream.dma, "", _rx_stream.dma.zero_copy) < 0)
            throw std::runtime_error("DMA Writer/RX not available (litepcie_dma_init failed).");

        /* Get Buffer and Parameters from RX DMA Writer */
        _rx_stream.buf = _rx_stream.dma.buf_rd;
        _rx_buf_size   = _rx_stream.dma.mmap_dma_info.dma_rx_buf_size - RX_DMA_HEADER_SIZE;
        _rx_buf_count  = _rx_stream.dma.mmap_dma_info.dma_rx_buf_count;

        /* Ensure the DMA is disabled initially to avoid counters being in a bad state. */
        litepcie_dma_writer(_fd, 0, &_rx_stream.hw_count, &_rx_stream.sw_count);

#elif USE_LITEETH
        _rx_buf_size = _rx_udp_receiver->buffer_size();
        _rx_buf_count = _rx_udp_receiver->buffer_count();
        _rx_stream.buf = malloc(_rx_buf_size * _rx_buf_count);
        if (!_rx_stream.buf)
            throw std::runtime_error("Malloc failed.");
#endif

        _rx_stream.opened = true;
        _rx_stream.format = format;

        /* Determine channels: prioritize searchArgs over the provided vector */
        std::vector<size_t> selected_channels;
        auto it = searchArgs.find("channels");
        if (it != searchArgs.end()) {
            /* Extract channel string from searchArgs and override */
            std::string chan_str = it->second;
            if (chan_str == "0") {
                selected_channels = {0};
            } else if (chan_str == "1") {
                selected_channels = {1};
            } else if (chan_str == "0,1" || chan_str == "0, 1") {
                selected_channels = {0, 1};
            } else {
                throw std::runtime_error("Invalid channels in searchArgs: " + chan_str + "; use '0', '1', or '0,1'");
            }
        } else if (!channels.empty()) {
            /* Use the provided channels vector if no device argument overrides it */
            selected_channels = channels;
        } else {
            /* Default to channel 0 if nothing provided */
            selected_channels = {0};
        }

        /* Validate selected channels */
        if (selected_channels.size() > 2) {
            throw std::runtime_error("Invalid RX channel count: must be 1 or 2 channels");
        }
        for (size_t chan : selected_channels) {
            if (chan > 1) {
                throw std::runtime_error("Invalid RX channel index: must be 0 (RX1) or 1 (RX2)");
            }
        }
        if (selected_channels.size() == 2 && (selected_channels[0] != 0 || selected_channels[1] != 1)) {
            throw std::runtime_error("Dual RX channels must be {0, 1} for RX1+RX2");
        }

        /* Log the selected RX channels for debugging */
        if (selected_channels.size() == 1) {
            SoapySDR_logf(SOAPY_SDR_INFO, "RX setupStream: Selected channel %zu", selected_channels[0]);
        } else {
            SoapySDR_logf(SOAPY_SDR_INFO, "RX setupStream: Selected channels %zu, %zu",
                          selected_channels[0], selected_channels[1]);
        }

        _rx_stream.channels = selected_channels;
        _nChannels = _rx_stream.channels.size();
    } else if (direction == SOAPY_SDR_TX) {
        if (_tx_stream.opened) {
            throw std::runtime_error("TX stream already opened.");
        }

        /* Configure the file descriptor watcher. */

#if USE_LITEPCIE
        _tx_stream.fds.fd     = _fd;
        _tx_stream.dma.fds.fd = _fd;
#endif
        _tx_stream.fds.events = POLLOUT;

#if USE_LITEPCIE
        /* Initialize TX DMA Reader */
        _tx_stream.dma.shared_fd  = 1;
        _tx_stream.dma.use_reader = 1;
        _tx_stream.dma.use_writer = 0;
        _tx_stream.dma.loopback   = 0;
        _tx_stream.dma.zero_copy  = 1;
        if (litepcie_dma_init(&_tx_stream.dma, "", _tx_stream.dma.zero_copy) < 0)
            throw std::runtime_error("DMA Reader/TX not available (litepcie_dma_init failed).");

        /* Get Buffer and Parameters from TX DMA Reader */
        _tx_stream.buf = _tx_stream.dma.buf_wr;
        _tx_buf_size   = _tx_stream.dma.mmap_dma_info.dma_tx_buf_size - TX_DMA_HEADER_SIZE;
        _tx_buf_count  = _tx_stream.dma.mmap_dma_info.dma_tx_buf_count;

        /* Ensure the DMA is disabled initially to avoid counters being in a bad state. */
        litepcie_dma_reader(_fd, 0, &_tx_stream.hw_count, &_tx_stream.sw_count);
#endif

        _tx_stream.opened = true;
        _tx_stream.format = format;

        /* Determine channels: override provided vector if searchArgs contains "channels" */
        std::vector<size_t> selected_channels;
        auto it = searchArgs.find("channels");
        if (it != searchArgs.end()) {
            /* Extract channel string from searchArgs */
            std::string chan_str = it->second;
            if (chan_str == "0") {
                selected_channels = {0};
            } else if (chan_str == "1") {
                selected_channels = {1};
            } else if (chan_str == "0,1" || chan_str == "0, 1") {
                selected_channels = {0, 1};
            } else {
                throw std::runtime_error("Invalid channels in searchArgs: " + chan_str + "; use '0', '1', or '0,1'");
            }
        } else if (!channels.empty()) {
            /* Use the provided channels vector if no override is present */
            selected_channels = channels;
        } else {
            /* Default to TX1 if nothing is specified */
            selected_channels = {0};
        }

        /* Validate selected channels */
        if (selected_channels.size() > 2) {
            throw std::runtime_error("Invalid TX channel count: must be 1 or 2 channels");
        }
        for (size_t chan : selected_channels) {
            if (chan > 1) {
                throw std::runtime_error("Invalid TX channel index: must be 0 (TX1) or 1 (TX2)");
            }
        }
        if (selected_channels.size() == 2 && (selected_channels[0] != 0 || selected_channels[1] != 1)) {
            throw std::runtime_error("Dual TX channels must be {0, 1} for TX1+TX2");
        }

        _tx_stream.channels = selected_channels;
        _nChannels = _tx_stream.channels.size();
    } else {
        throw std::runtime_error("Invalid direction.");
    }

    /* Configure 2T2R/1T1R mode (PHY) */
    litex_m2sdr_writel(_fd, CSR_AD9361_PHY_CONTROL_ADDR, _nChannels == 1 ? 1 : 0);

    /* AD9361 Channel en/dis */
    ad9361_phy->pdata->rx2tx2 = (_nChannels == 2);
    if (_nChannels == 1) {
        if (direction == SOAPY_SDR_RX)
            ad9361_phy->pdata->rx1tx1_mode_use_rx_num = _rx_stream.channels[0] == 0 ? RX_1 : RX_2;
        else if (direction == SOAPY_SDR_TX)
            ad9361_phy->pdata->rx1tx1_mode_use_tx_num = _tx_stream.channels[0] == 0 ? TX_1 : TX_2;
    } else {
        if (direction == SOAPY_SDR_RX)
            ad9361_phy->pdata->rx1tx1_mode_use_rx_num = RX_1 | RX_2;
        else if (direction == SOAPY_SDR_TX)
            ad9361_phy->pdata->rx1tx1_mode_use_tx_num = TX_1 | TX_2;
    }

    /* AD9361 Port Control 2t2r timing enable */
    struct ad9361_phy_platform_data *pd = ad9361_phy->pdata;
    pd->port_ctrl.pp_conf[0] &= ~(1 << 2);
    if (_nChannels == 2)
        pd->port_ctrl.pp_conf[0] |= (1 << 2);

    ad9361_set_no_ch_mode(ad9361_phy, _nChannels);

    return direction == SOAPY_SDR_RX ? RX_STREAM : TX_STREAM;
}

/* Close the specified stream and release associated resources. */
void SoapyLiteXM2SDR::closeStream(SoapySDR::Stream *stream) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (stream == RX_STREAM) {
#if USE_LITEPCIE
        litepcie_dma_cleanup(&_rx_stream.dma);
#elif USE_LITEETH
        free(_rx_stream.buf);
#endif
        _rx_stream.opened = false;
    } else if (stream == TX_STREAM) {
#if USE_LITEPCIE
        litepcie_dma_cleanup(&_tx_stream.dma);
#endif
    }
}

/* Activate the specified stream (configure the DMA engines). */
int SoapyLiteXM2SDR::activateStream(
    SoapySDR::Stream *stream,
    const int /*flags*/,
    const long long /*timeNs*/,
    const size_t /*numElems*/) {

    /* RX */
    if (stream == RX_STREAM) {
        for (size_t i = 0; i < _rx_stream.channels.size(); i++)
            channel_configure(SOAPY_SDR_RX, _rx_stream.channels[i]);
#if USE_LITEPCIE
        /* Crossbar Demux: Select PCIe streaming */
        litex_m2sdr_writel(_fd, CSR_CROSSBAR_DEMUX_SEL_ADDR, 0);
        /* Configure the DMA engine for RX, but don't enable it yet. */
        litepcie_dma_writer(_fd, 0, &_rx_stream.hw_count, &_rx_stream.sw_count);
#elif USE_LITEETH
        /* Crossbar Demux: Select Ethernet streaming */
        litex_m2sdr_writel(_fd, CSR_CROSSBAR_DEMUX_SEL_ADDR, 1);
        _rx_udp_receiver->start();
#endif
        _rx_stream.user_count = 0;
        _rx_stream.burst_end = false;

    /* TX */
    } else if (stream == TX_STREAM) {
#if USE_LITEPCIE
        for (size_t i = 0; i < _tx_stream.channels.size(); i++)
            channel_configure(SOAPY_SDR_TX, _tx_stream.channels[i]);
        /* Configure the DMA engine for TX, but don't enable it yet. */
        litepcie_dma_reader(_fd, 0, &_tx_stream.hw_count, &_tx_stream.sw_count);
        _tx_stream.user_count = 0;
#endif
    }

    return 0;
}

/* Deactivate the specified stream (disable DMA engine). */
int SoapyLiteXM2SDR::deactivateStream(
    SoapySDR::Stream *stream,
    const int /*flags*/,
    const long long /*timeNs*/) {
    if (stream == RX_STREAM) {
        /* Disable the DMA engine for RX. */
#if USE_LITEPCIE
        litepcie_dma_writer(_fd, 0, &_rx_stream.hw_count, &_rx_stream.sw_count);
#elif USE_LITEETH
        _rx_udp_receiver->stop();
#endif
        /* set burst_end: if readStream is called after this point SOAPY_SDR_END_BURST
         * will be set
         */
        _rx_stream.burst_end = true;
    } else if (stream == TX_STREAM) {
#if USE_LITEPCIE
        /* Disable the DMA engine for TX. */
        litepcie_dma_reader(_fd, 0, &_tx_stream.hw_count, &_tx_stream.sw_count);
#endif
    }
    return 0;
}

/*******************************************************************
 * Direct buffer API
 ******************************************************************/

/* Retrieve the maximum transmission unit (MTU) for a stream. */
size_t SoapyLiteXM2SDR::getStreamMTU(SoapySDR::Stream *stream) const {
    if (stream == RX_STREAM) {
        /* Each sample is 2 * Complex{Int16}. */
        return _rx_buf_size / (_nChannels * _bytesPerComplex);
    } else if (stream == TX_STREAM) {
        return _tx_buf_size / (_nChannels * _bytesPerComplex);
    } else {
        throw std::runtime_error("SoapySDR::getStreamMTU(): Invalid stream.");
    }
}

/* Retrieve the number of direct access buffers available for a stream. */
size_t SoapyLiteXM2SDR::getNumDirectAccessBuffers(SoapySDR::Stream *stream) {
    if (stream == RX_STREAM) {
        return _rx_buf_count;
    } else if (stream == TX_STREAM) {
        return _dma_mmap_info.dma_tx_buf_count;
    } else {
        throw std::runtime_error("SoapySDR::getNumDirectAccessBuffers(): Invalid stream.");
    }
}

/* Retrieve buffer addresses for a direct access buffer. */
int SoapyLiteXM2SDR::getDirectAccessBufferAddrs(
    SoapySDR::Stream *stream,
    const size_t handle,
    void **buffs) {
    if (stream == RX_STREAM) {
        buffs[0] = (char *)_rx_stream.buf + handle * _dma_mmap_info.dma_rx_buf_size + RX_DMA_HEADER_SIZE;
    } else if (stream == TX_STREAM) {
        buffs[0] = (char *)_tx_stream.buf + handle * _dma_mmap_info.dma_tx_buf_size + TX_DMA_HEADER_SIZE;
    } else {
        throw std::runtime_error("SoapySDR::getDirectAccessBufferAddrs(): Invalid stream.");
    }
    return 0;
}

/***************************************************************************************************
 * DMA Buffer Management
 *
 * The DMA readers/writers utilize a zero-copy mechanism (i.e., a single buffer shared
 * with the kernel) and employ three counters to index that buffer:
 * - hw_count: Indicates the position where the hardware has read from or written to.
 * - sw_count: Indicates the position where userspace has read from or written to.
 * - user_count: Indicates the current position where userspace is reading from or
 *   writing to.
 *
 * The distinction between sw_count and user_count enables tracking of which buffers are
 * currently being processed. This feature is not directly supported by the LitePCIe DMA
 * library, so it is implemented separately.
 *
 * Separating user_count enables advancing read/write buffers without requiring a syscall
 * (interfacing with the kernel only when retiring buffers). However, this can result in
 * slower detection of overflows and underflows, so overflow/underflow detection is made
 * configurable.
 **************************************************************************************************/

#define DETECT_EVERY_OVERFLOW  true  /* Detect overflow every time it occurs. */
#define DETECT_EVERY_UNDERFLOW true  /* Detect underflow every time it occurs. */

static constexpr uint64_t DMA_HEADER_SYNC_WORD = 0x5aa55aa55aa55aa5ULL;

/* Acquire a buffer for reading. */
int SoapyLiteXM2SDR::acquireReadBuffer(
    SoapySDR::Stream *stream,
    size_t &handle,
    const void **buffs,
    int &flags,
#if USE_LITEPCIE && defined(_RX_DMA_HEADER_TEST)
     long long &timeNs,
#else
     long long &/*timeNs*/,
#endif
    const long timeoutUs) {
    if (stream != RX_STREAM) {
        return SOAPY_SDR_STREAM_ERROR;
    }

    if (_rx_stream.burst_end)
        flags |= SOAPY_SDR_END_BURST;

#if USE_LITEETH
#ifdef USE_THREAD
    int buffers_available = _rx_udp_receiver->buffers_available();
    /* No buffer: fails */
    if (buffers_available <= 0) {
        return SOAPY_SDR_TIMEOUT;
    }

    /* Detect overflows of the underlying circular buffer. */
    if (_rx_udp_receiver->overflow()) {
        flags |= SOAPY_SDR_END_ABRUPT;
        return SOAPY_SDR_OVERFLOW;
    }
#endif
    buffs[0] = (char *)_rx_stream.buf;
    int pos = 0;
    char *ptr = (char *)_rx_stream.buf;
    std::vector<char> vc = _rx_udp_receiver->get_data();
    memcpy(ptr, vc.data(), _rx_buf_size);
    pos += _rx_buf_size;

    handle = pos;
    return getStreamMTU(stream);

#elif USE_LITEPCIE

    /* Check if there are buffers available. */
    int buffers_available = _rx_stream.hw_count - _rx_stream.user_count;
    assert(buffers_available >= 0);

    /* If not, check with the DMA engine. */
    if (buffers_available == 0 || DETECT_EVERY_OVERFLOW) {
        litepcie_dma_writer(_fd, 1, &_rx_stream.hw_count, &_rx_stream.sw_count);
        buffers_available = _rx_stream.hw_count - _rx_stream.user_count;
    }

    /* If no buffers available, wait for new buffers to arrive. */
    if (buffers_available == 0) {
        if (timeoutUs == 0) {
            return SOAPY_SDR_TIMEOUT;
        }
        int ret = poll(&_rx_stream.fds, 1, timeoutUs / 1000);
        if (ret < 0) {
            throw std::runtime_error("SoapyLiteXM2SDR::acquireReadBuffer(): Poll failed, " +
                                     std::string(strerror(errno)) + ".");
        } else if (ret == 0) {
            return SOAPY_SDR_TIMEOUT;
        }

        /* Get new DMA counters. */
        litepcie_dma_writer(_fd, 1, &_rx_stream.hw_count, &_rx_stream.sw_count);
        buffers_available = _rx_stream.hw_count - _rx_stream.user_count;
        assert(buffers_available > 0);
    }

    /* Detect overflows of the underlying circular buffer. */
    if ((_rx_stream.hw_count - _rx_stream.sw_count) >
        ((int64_t)_dma_mmap_info.dma_rx_buf_count / 2)) {
        /* Drain all buffers to get out of the overflow quicker. */
        struct litepcie_ioctl_mmap_dma_update mmap_dma_update;
        mmap_dma_update.sw_count = _rx_stream.hw_count;
        checked_ioctl(_fd, LITEPCIE_IOCTL_MMAP_DMA_WRITER_UPDATE, &mmap_dma_update);
        _rx_stream.user_count = _rx_stream.hw_count;
        _rx_stream.sw_count = _rx_stream.hw_count;
        handle = -1;

        flags |= SOAPY_SDR_END_ABRUPT;
        return SOAPY_SDR_OVERFLOW;
    } else {
        /* Get the buffer. */
        int buf_offset = _rx_stream.user_count % _dma_mmap_info.dma_rx_buf_count;

        /* Extract Sync Word and Timestamp from the DMA header */
#if defined(_RX_DMA_HEADER_TEST)
        {
            /* Header is at the beginning of the DMA buffer */
            const uint8_t *header_ptr = reinterpret_cast<const uint8_t *>(_rx_stream.buf) + buf_offset * _dma_mmap_info.dma_rx_buf_size;

            /* Extract sync word from bytes 0 to 8 of the Header */
            uint64_t header = *reinterpret_cast<const uint64_t*>(header_ptr);
            if (header != DMA_HEADER_SYNC_WORD) {
                SoapySDR_logf(SOAPY_SDR_WARNING, "RX DMA Header Sync Word is not matching! Expected 0x%llx, got 0x%llx", DMA_HEADER_SYNC_WORD, header);
            }

            /* Timestamp is stored in bytes 8 to 16 of the header */
            uint64_t timestamp = *reinterpret_cast<const uint64_t*>(header_ptr + 8);

            /* Assign the extracted timestamp to the provided timeNs reference. */
            timeNs = static_cast<long long>(timestamp);

            /* Track the previous timestamp */
            static uint64_t prevTimestamp = 0;
            uint64_t diff = (prevTimestamp == 0) ? 0 : (timestamp - prevTimestamp);
            prevTimestamp = timestamp;

            /* Debug RX DMA Timestamp and Diff vs previous */
            SoapySDR_logf(SOAPY_SDR_DEBUG, "RX DMA Timestamp: %llu ns (diff: %llu ns)", timestamp, diff);
        }
#endif

        /* Get the pointer to the actual sample data (skipping the header). */
        getDirectAccessBufferAddrs(stream, buf_offset, (void **)buffs);

        /* Update the DMA counters. */
        handle = _rx_stream.user_count;
        _rx_stream.user_count++;

        return getStreamMTU(stream);
    }
#endif
}

/* Release a read buffer after use. */
void SoapyLiteXM2SDR::releaseReadBuffer(
    SoapySDR::Stream */*stream*/,
    size_t handle) {
    assert(handle != (size_t)-1 && "Attempt to release an invalid buffer (e.g., from an overflow).");

#if USE_LITEPCIE
    /* Update the DMA counters. */
    struct litepcie_ioctl_mmap_dma_update mmap_dma_update;
    mmap_dma_update.sw_count = handle + 1;
    checked_ioctl(_fd, LITEPCIE_IOCTL_MMAP_DMA_WRITER_UPDATE, &mmap_dma_update);
#endif
}

/* Acquire a buffer for writing. */
int SoapyLiteXM2SDR::acquireWriteBuffer(
    SoapySDR::Stream *stream,
    size_t &handle,
    void **buffs,
    const long timeoutUs) {
    if (stream != TX_STREAM) {
        return SOAPY_SDR_STREAM_ERROR;
    }

#if USE_LITEPCIE
    /* Check if there are buffers available. */
    int buffers_pending = _tx_stream.user_count - _tx_stream.hw_count;
    assert(buffers_pending <= (int)_dma_mmap_info.dma_tx_buf_count);

    /* If not, check with the DMA engine. */
    if (buffers_pending == ((int64_t)_dma_mmap_info.dma_tx_buf_count) || DETECT_EVERY_UNDERFLOW) {
        litepcie_dma_reader(_fd, 1, &_tx_stream.hw_count, &_tx_stream.sw_count);
        buffers_pending = _tx_stream.user_count - _tx_stream.hw_count;
    }

    /* If no buffers available, wait for new buffers to become available. */
    if (buffers_pending == ((int64_t)_dma_mmap_info.dma_tx_buf_count)) {
        if (timeoutUs == 0) {
            return SOAPY_SDR_TIMEOUT;
        }
        int ret = poll(&_tx_stream.fds, 1, timeoutUs / 1000);
        if (ret < 0) {
            throw std::runtime_error("SoapyLiteXM2SDR::acquireWriteBuffer(): Poll failed, " +
                                     std::string(strerror(errno)) + ".");
        } else if (ret == 0) {
            return SOAPY_SDR_TIMEOUT;
        }

        /* Get new DMA counters. */
        litepcie_dma_reader(_fd, 1, &_tx_stream.hw_count, &_tx_stream.sw_count);
        buffers_pending = _tx_stream.user_count - _tx_stream.hw_count;
        assert(buffers_pending < ((int64_t)_dma_mmap_info.dma_tx_buf_count));
    }

    /* Get the buffer. */
    int buf_offset = _tx_stream.user_count % _dma_mmap_info.dma_tx_buf_count;
    getDirectAccessBufferAddrs(stream, buf_offset, buffs);

    /* Update the DMA counters. */
    handle = _tx_stream.user_count;
    _tx_stream.user_count++;

    /* Write Sync Word and Timestamp to DMA header */
#if defined(_TX_DMA_HEADER_TEST)
    {
        /* Header is at the beginning of the DMA buffer */
        uint8_t *tx_buffer = reinterpret_cast<uint8_t*>(_tx_stream.buf) + (buf_offset * _dma_mmap_info.dma_tx_buf_size);

        /* Extract Sync Word to bytes 0 to 8 of the Header */
        uint64_t header = DMA_HEADER_SYNC_WORD;
        *reinterpret_cast<uint64_t*>(tx_buffer) = header;

        /* Compute the number of samples per DMA buffer. */
        uint32_t samples_per_buffer = _dma_mmap_info.dma_tx_buf_size / (_nChannels * _bytesPerComplex);

        /* Compute time increment (in nanoseconds) for this buffer */
        uint64_t time_increment = static_cast<uint64_t>((samples_per_buffer / _tx_stream.samplerate) * 1e9);

        /* Write Timestamp */
        static uint64_t fakeTimestamp = 0;
        fakeTimestamp += time_increment;
        *reinterpret_cast<uint64_t*>(tx_buffer + 8) = fakeTimestamp;
        SoapySDR_logf(SOAPY_SDR_DEBUG, "TX DMA Header inserted: timestamp increment: %llu, new timestamp: %llu",
                      time_increment, fakeTimestamp);
    }
#endif


    /* Detect underflows. */
    if (buffers_pending < 0) {
        return SOAPY_SDR_UNDERFLOW;
    } else {
        return getStreamMTU(stream);
    }
#elif USE_LITEETH
    return SOAPY_SDR_NOT_SUPPORTED;
#endif
}

/* Release a write buffer after use. */
void SoapyLiteXM2SDR::releaseWriteBuffer(
    SoapySDR::Stream */*stream*/,
    size_t handle,
    const size_t /*numElems*/,
    int &/*flags*/,
    const long long /*timeNs*/) {
    /* XXX: Inspect user-provided numElems and flags, and act upon them? */

#if USE_LITEPCIE
    /* Update the DMA counters so that the engine can submit this buffer. */
    struct litepcie_ioctl_mmap_dma_update mmap_dma_update;
    mmap_dma_update.sw_count = handle + 1;
    checked_ioctl(_fd, LITEPCIE_IOCTL_MMAP_DMA_READER_UPDATE, &mmap_dma_update);
#endif
}

/* Interleave CF32 samples. */
void SoapyLiteXM2SDR::interleaveCF32(
    const void *src,
    void *dst,
    uint32_t len,
    size_t offset) {
    const float *samples_cf32 = reinterpret_cast<const float*>(src) + (offset * _samplesPerComplex);

    if (_bytesPerSample == 2) {
        int16_t *dst_int16 = reinterpret_cast<int16_t*>(dst) + (offset * 2 * _samplesPerComplex);
        for (uint32_t i = 0; i < len; i++) {
            dst_int16[0] = static_cast<int16_t>(samples_cf32[0] * _samplesScaling); /* I. */
            dst_int16[1] = static_cast<int16_t>(samples_cf32[1] * _samplesScaling); /* Q. */
            samples_cf32 += 2;
            dst_int16 += _nChannels * _samplesPerComplex;
        }
    } else if (_bytesPerSample == 1) {
        int8_t *dst_int8 = reinterpret_cast<int8_t*>(dst) + (offset * 2 * _samplesPerComplex);
        for (uint32_t i = 0; i < len; i++) {
            dst_int8[0] = static_cast<int8_t>(samples_cf32[0] * (_samplesScaling)); /* I. */
            dst_int8[1] = static_cast<int8_t>(samples_cf32[1] * (_samplesScaling)); /* Q. */
            samples_cf32 += 2;
            dst_int8 += _nChannels * _samplesPerComplex;
        }
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported _bytesPerSample value: %u.", _bytesPerSample);
    }
}

/* Deinterleave CF32 samples. */
void SoapyLiteXM2SDR::deinterleaveCF32(
    const void *src,
    void *dst,
    uint32_t len,
    size_t offset) {
    float *samples_cf32 = reinterpret_cast<float*>(dst) + (offset * _samplesPerComplex);

    if (_bytesPerSample == 2) {
        const int16_t *src_int16 = reinterpret_cast<const int16_t*>(src);

        for (uint32_t i = 0; i < len; i++) {
            samples_cf32[0] = static_cast<float>(src_int16[0]) / _samplesScaling; /* I. */
            samples_cf32[1] = static_cast<float>(src_int16[1]) / _samplesScaling; /* Q. */
            samples_cf32 += 2;
            src_int16 += _nChannels * _samplesPerComplex;
        }
    } else if (_bytesPerSample == 1) {
        const int8_t *src_int8 = reinterpret_cast<const int8_t*>(src);

        for (uint32_t i = 0; i < len; i++) {
            samples_cf32[0] = static_cast<float>(src_int8[0]) / _samplesScaling; /* I. */
            samples_cf32[1] = static_cast<float>(src_int8[1]) / _samplesScaling; /* Q. */
            samples_cf32 += 2;
            src_int8 += _nChannels * _samplesPerComplex;
        }
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported _bytesPerSample value: %u.", _bytesPerSample);
    }
}

/* Interleave CS16 samples */
void SoapyLiteXM2SDR::interleaveCS16(
    const void *src,
    void *dst,
    uint32_t len,
    size_t offset) {
    const int16_t *samples_cs16 = reinterpret_cast<const int16_t*>(src) + (offset * _samplesPerComplex);

    if (_bytesPerSample == 2) {
        int16_t *dst_int16 = reinterpret_cast<int16_t*>(dst) + (offset * 2 * _samplesPerComplex);

        for (uint32_t i = 0; i < len; i++) {
            dst_int16[0] = samples_cs16[0]; /* I. */
            dst_int16[1] = samples_cs16[1]; /* Q. */
            samples_cs16 += 2;
            dst_int16 += _nChannels * _samplesPerComplex;
        }
    } else if (_bytesPerSample == 1) {
        int8_t *dst_int8 = reinterpret_cast<int8_t*>(dst) + (offset * 2 * _samplesPerComplex);

        for (uint32_t i = 0; i < len; i++) {
            dst_int8[0] = samples_cs16[0]; /* I. */
            dst_int8[1] = samples_cs16[1]; /* Q. */
            samples_cs16 += 2;
            dst_int8 += _nChannels * _samplesPerComplex;
        }
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported _bytesPerSample value: %u.", _bytesPerSample);
    }
}

/* Deinterleave CS16 samples */
void SoapyLiteXM2SDR::deinterleaveCS16(
    const void *src,
    void *dst,
    uint32_t len,
    size_t offset) {
    int16_t *samples_cs16 = reinterpret_cast<int16_t*>(dst) + (offset * _samplesPerComplex);

    if (_bytesPerSample == 2) {
        const int16_t *src_int16 = reinterpret_cast<const int16_t*>(src);

        for (uint32_t i = 0; i < len; i++) {
            samples_cs16[0] = src_int16[0]; /* I. */
            samples_cs16[1] = src_int16[1]; /* Q. */
            samples_cs16 += 2;
            src_int16 += _nChannels * _samplesPerComplex;
        }
    } else if (_bytesPerSample == 1) {
        const int8_t *src_int8 = reinterpret_cast<const int8_t*>(src);

        for (uint32_t i = 0; i < len; i++) {
            samples_cs16[0] = static_cast<int16_t>(src_int8[0]); /* I. */
            samples_cs16[1] = static_cast<int16_t>(src_int8[1]); /* Q. */
            samples_cs16 += 2;
            src_int8 += _nChannels * _samplesPerComplex;
        }
    } else {
        printf("Unsupported _bytesPerSample value: %u\n", _bytesPerSample);
    }
}

/* Interleave samples */
void SoapyLiteXM2SDR::interleave(
    const void *src,
    void *dst,
    uint32_t len,
    const std::string &format,
    size_t offset) {
    if (format == SOAPY_SDR_CF32) {
        interleaveCF32(src, dst, len, offset);
    } else if (format == SOAPY_SDR_CS16) {
        interleaveCS16(src, dst, len, offset);
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported format: %s.", format.c_str());
    }
}

/* Deinterleave samples */
void SoapyLiteXM2SDR::deinterleave(
    const void *src,
    void *dst,
    uint32_t len,
    const std::string &format,
    size_t offset) {
    if (format == SOAPY_SDR_CF32) {
        deinterleaveCF32(src, dst, len, offset);
    } else if (format == SOAPY_SDR_CS16) {
        deinterleaveCS16(src, dst, len, offset);
    } else {
        SoapySDR_logf(SOAPY_SDR_ERROR, "Unsupported format: %s.", format.c_str());
    }
}

/* Read from the RX stream. */
int SoapyLiteXM2SDR::readStream(
    SoapySDR::Stream *stream,
    void *const *buffs,
    const size_t numElems,
    int &flags,
    long long &timeNs,
    const long timeoutUs) {
    if (stream != RX_STREAM) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    /* Determine the number of samples to return, respecting the MTU. */
    size_t returnedElems = std::min(numElems, this->getStreamMTU(stream));

    size_t samp_avail = 0;

    /* If there's a remainder buffer from a previous read, process that first. */
    if (_rx_stream.remainderHandle >= 0) {
        const size_t n = std::min(_rx_stream.remainderSamps, returnedElems);
        const uint32_t remainderOffset = _rx_stream.remainderOffset *  _nChannels *_bytesPerComplex;

        if (n < returnedElems) {
            samp_avail = n;
        }

        /* Read out channels from the remainder buffer. */
        for (size_t i = 0; i < _rx_stream.channels.size(); i++) {
            const uint32_t chan = _rx_stream.channels[i];
            this->deinterleave(
                _rx_stream.remainderBuff + (remainderOffset + chan * _bytesPerComplex),
                buffs[i],
                n,
                _rx_stream.format,
                0
            );
        }
        _rx_stream.remainderSamps -= n;
        _rx_stream.remainderOffset += n;

        if (_rx_stream.remainderSamps == 0) {
            this->releaseReadBuffer(stream, _rx_stream.remainderHandle);
            _rx_stream.remainderHandle = -1;
            _rx_stream.remainderOffset = 0;
        }

        if (n == returnedElems) {
            return returnedElems;
        }
    }

    /* Acquire a new read buffer from the DMA engine. */
    size_t handle;
    int ret = this->acquireReadBuffer(
        stream,
        handle,
        (const void **)&_rx_stream.remainderBuff,
        flags,
        timeNs,
        timeoutUs);

    if (ret < 0) {
        if ((ret == SOAPY_SDR_TIMEOUT) && (samp_avail > 0)) {
            return samp_avail;
        }
        return ret;
    }

    _rx_stream.remainderHandle = handle;
    _rx_stream.remainderSamps = ret;

    const size_t n = std::min((returnedElems - samp_avail), _rx_stream.remainderSamps);

    /* Read out channels from the new buffer. */
    for (size_t i = 0; i < _rx_stream.channels.size(); i++) {
        const uint32_t chan = _rx_stream.channels[i];
        this->deinterleave(
            _rx_stream.remainderBuff + (chan * _bytesPerComplex),
            buffs[i],
            n,
            _rx_stream.format,
            samp_avail
        );
    }
    _rx_stream.remainderSamps -= n;
    _rx_stream.remainderOffset += n;

    if (_rx_stream.remainderSamps == 0) {
        this->releaseReadBuffer(stream, _rx_stream.remainderHandle);
        _rx_stream.remainderHandle = -1;
        _rx_stream.remainderOffset = 0;
    }

    return returnedElems;
}

/* Write to the TX stream. */
int SoapyLiteXM2SDR::writeStream(
    SoapySDR::Stream *stream,
    const void *const *buffs,
    const size_t numElems,
    int &flags,
    const long long timeNs,
    const long timeoutUs) {
    if (stream != TX_STREAM) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    /* Determine the number of samples to return, respecting the MTU. */
    size_t returnedElems = std::min(numElems, this->getStreamMTU(stream));

    size_t samp_avail = 0;

    /* If there's a remainder buffer from a previous write, process that first. */
    if (_tx_stream.remainderHandle >= 0) {
        const size_t n = std::min(_tx_stream.remainderSamps, returnedElems);
        const uint32_t remainderOffset = _tx_stream.remainderOffset * _nChannels * _bytesPerComplex;

        if (n < returnedElems) {
            samp_avail = n;
        }

        /* Write out channels to the remainder buffer. */
        for (size_t i = 0; i < _tx_stream.channels.size(); i++) {
            this->interleave(
                buffs[i],
                _tx_stream.remainderBuff + remainderOffset + (_tx_stream.channels[i] * _bytesPerComplex),
                n,
                _tx_stream.format,
                0
            );
        }
        _tx_stream.remainderSamps -= n;
        _tx_stream.remainderOffset += n;

        if (_tx_stream.remainderSamps == 0) {
            this->releaseWriteBuffer(stream, _tx_stream.remainderHandle, _tx_stream.remainderOffset, flags, timeNs);
            _tx_stream.remainderHandle = -1;
            _tx_stream.remainderOffset = 0;
        }

        if (n == returnedElems) {
            return returnedElems;
        }
    }

    /* Acquire a new write buffer from the DMA engine. */
    size_t handle;

    int ret = this->acquireWriteBuffer(
        stream,
        handle,
        (void **)&_tx_stream.remainderBuff,
        timeoutUs);
    if (ret < 0) {
        if ((ret == SOAPY_SDR_TIMEOUT) && (samp_avail > 0)) {
            return samp_avail;
        }
        return ret;
    }

    _tx_stream.remainderHandle = handle;
    _tx_stream.remainderSamps = ret;

    const size_t n = std::min((returnedElems - samp_avail), _tx_stream.remainderSamps);

    /* Write out channels to the new buffer. */
    for (size_t i = 0; i < _tx_stream.channels.size(); i++) {
        this->interleave(
            buffs[i],
            _tx_stream.remainderBuff + (_tx_stream.channels[i] * _bytesPerComplex),
            n,
            _tx_stream.format,
            samp_avail
        );
    }
    _tx_stream.remainderSamps -= n;
    _tx_stream.remainderOffset += n;

    if (_tx_stream.remainderSamps == 0) {
        this->releaseWriteBuffer(stream, _tx_stream.remainderHandle, _tx_stream.remainderOffset, flags, timeNs);
        _tx_stream.remainderHandle = -1;
        _tx_stream.remainderOffset = 0;
    }

    return returnedElems;
}

/* Check the status of the TX/RX streams. */
int SoapyLiteXM2SDR::readStreamStatus(
    SoapySDR::Stream *stream,
    size_t &chanMask,
    int &flags,
    long long &timeNs,
    const long timeoutUs){

    /* For now we only suport TX stream. */
    if(stream != TX_STREAM){
        return SOAPY_SDR_NOT_SUPPORTED;
    }

    /* Calculate the timeout duration and exit time. */
    const auto timeout = std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(std::chrono::microseconds(timeoutUs));
    const auto exitTime = std::chrono::high_resolution_clock::now() + timeout;

    /* Poll for status events until the timeout expires. */
    while (true) {
        if(_tx_stream.underflow){
            _tx_stream.underflow=false;
            SoapySDR::log(SOAPY_SDR_SSI, "U");
            return SOAPY_SDR_UNDERFLOW;
        }

        /* Sleep for a fraction of the total timeout. */
        const auto sleepTimeUs = std::min<long>(1000, timeoutUs/10);
        std::this_thread::sleep_for(std::chrono::microseconds(sleepTimeUs));

        /* Check if the timeout has expired. */
        const auto timeNow = std::chrono::high_resolution_clock::now();
        if (exitTime < timeNow) return SOAPY_SDR_TIMEOUT;
    }
}
