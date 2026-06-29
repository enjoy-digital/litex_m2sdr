/* SPDX-License-Identifier: BSD-2-Clause */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "m2sdr.h"
#include "m2sdr_internal.h"
#include "../m2sdr_cli.h"
#include "../m2sdr_tool.h"

static int test_parse_identifier_invalid_ports(void)
{
    uint16_t port = 0;

    if (m2sdr_test_parse_identifier("eth:192.168.1.10:abc", &port) == 0)
        return -1;
    if (m2sdr_test_parse_identifier("eth:192.168.1.10:70000", &port) == 0)
        return -1;
    if (m2sdr_test_parse_identifier("eth::1234", &port) == 0)
        return -1;
    if (m2sdr_test_parse_identifier("eth:192.168.1.10:", &port) == 0)
        return -1;

    if (m2sdr_test_parse_identifier("eth:192.168.1.10:1234", &port) != 0)
        return -1;
    if (port != 1234)
        return -1;

    return 0;
}

static int test_parse_identifier_forms(void)
{
    uint16_t port = 1234;
    struct m2sdr_device_addr addr;

    if (m2sdr_test_parse_identifier(NULL, &port) != 0 || port != 1234)
        return -1;
    if (m2sdr_test_parse_identifier("pcie:/dev/m2sdr3", &port) != 0)
        return -1;
    if (m2sdr_test_parse_identifier("/dev/m2sdr2", &port) != 0)
        return -1;
    if (m2sdr_test_parse_identifier("eth:192.168.1.10:2345", &port) != 0 || port != 2345)
        return -1;
    port = 1234;
    if (m2sdr_test_parse_identifier("192.168.1.10:3456", &port) != 0 || port != 3456)
        return -1;

    if (m2sdr_resolve_device_identifier("pcie:/dev/m2sdr3", &addr) != M2SDR_ERR_OK)
        return -1;
    if (addr.transport != M2SDR_TRANSPORT_KIND_LITEPCIE)
        return -1;
    if (strcmp(addr.identifier, "pcie:/dev/m2sdr3") != 0 || strcmp(addr.path, "/dev/m2sdr3") != 0)
        return -1;

    if (m2sdr_resolve_device_identifier("192.168.1.10:3456", &addr) != M2SDR_ERR_OK)
        return -1;
    if (addr.transport != M2SDR_TRANSPORT_KIND_LITEETH)
        return -1;
    if (strcmp(addr.identifier, "eth:192.168.1.10:3456") != 0 || strcmp(addr.ip, "192.168.1.10") != 0)
        return -1;
    if (addr.port != 3456)
        return -1;

    return 0;
}

static int test_cli_device_selection(void)
{
    struct m2sdr_cli_device dev;

    m2sdr_cli_device_init(&dev);
    if (!m2sdr_cli_finalize_device(&dev))
        return -1;
#ifdef M2SDR_DEFAULT_TRANSPORT_LITEETH
    if (strcmp(m2sdr_cli_device_id(&dev), "eth:192.168.1.50:1234") != 0)
        return -1;
#else
    if (strcmp(m2sdr_cli_device_id(&dev), "pcie:/dev/m2sdr0") != 0)
        return -1;
    if (strcmp(m2sdr_cli_pcie_path(&dev), "/dev/m2sdr0") != 0)
        return -1;
#endif

    m2sdr_cli_device_init(&dev);
    if (m2sdr_cli_handle_device_option(&dev, 'i', "192.168.1.10") != 0)
        return -1;
    if (m2sdr_cli_handle_device_option(&dev, 'p', "2345") != 0)
        return -1;
    if (!m2sdr_cli_finalize_device(&dev))
        return -1;
    if (strcmp(m2sdr_cli_device_id(&dev), "eth:192.168.1.10:2345") != 0)
        return -1;

    m2sdr_cli_device_init(&dev);
    if (m2sdr_cli_handle_device_option(&dev, 'c', "2") != 0)
        return -1;
    if (!m2sdr_cli_finalize_device(&dev))
        return -1;
    if (strcmp(m2sdr_cli_device_id(&dev), "pcie:/dev/m2sdr2") != 0)
        return -1;

    m2sdr_cli_device_init(&dev);
    if (m2sdr_cli_set_device_id(&dev, "/dev/m2sdr3") != 0)
        return -1;
    if (!m2sdr_cli_finalize_device(&dev))
        return -1;
    if (strcmp(m2sdr_cli_device_id(&dev), "pcie:/dev/m2sdr3") != 0)
        return -1;
    if (strcmp(m2sdr_cli_pcie_path(&dev), "/dev/m2sdr3") != 0)
        return -1;

    return 0;
}

static int test_discovery_targets(void)
{
    struct m2sdr_discovery_config cfg;
    struct m2sdr_device_addr targets[4];
    size_t count = 0;

    m2sdr_discovery_config_init(&cfg);
    cfg.enable_pcie = false;
    cfg.liteeth_targets = "192.168.1.10; eth:192.168.1.11:2345, 192.168.1.10";
    cfg.liteeth_port = 1234;
    if (m2sdr_get_discovery_targets(&cfg, targets, 4, &count) != M2SDR_ERR_OK)
        return -1;
    if (count != 2)
        return -1;
    if (strcmp(targets[0].identifier, "eth:192.168.1.10:1234") != 0)
        return -1;
    if (strcmp(targets[1].identifier, "eth:192.168.1.11:2345") != 0)
        return -1;

    m2sdr_discovery_config_init(&cfg);
    cfg.enable_liteeth = false;
    cfg.pcie_first = 2;
    cfg.pcie_count = 2;
    if (m2sdr_get_discovery_targets(&cfg, targets, 4, &count) != M2SDR_ERR_OK)
        return -1;
    if (count != 2)
        return -1;
    if (strcmp(targets[0].identifier, "pcie:/dev/m2sdr2") != 0)
        return -1;
    if (strcmp(targets[1].identifier, "pcie:/dev/m2sdr3") != 0)
        return -1;

    if (m2sdr_get_discovery_targets(&cfg, targets, 1, &count) != M2SDR_ERR_RANGE)
        return -1;

    return 0;
}

static int test_cli_numeric_parser(void)
{
    int64_t value = 0;
    uint64_t uvalue = 0;
    uint32_t u32value = 0;
    unsigned unsigned_value = 0;
    int int_value = 0;
    double dvalue = 0.0;
    uint16_t u16value = 0;
    uint8_t u8value = 0;
    bool bvalue = false;

    if (m2sdr_cli_parse_int64("30720000", &value) != 0 || value != 30720000)
        return -1;
    if (m2sdr_cli_parse_int64("30.72e6", &value) != 0 || value != 30720000)
        return -1;
    if (m2sdr_cli_parse_int64("20M", &value) != 0 || value != 20000000)
        return -1;
    if (m2sdr_cli_parse_int64("2.4e9", &value) != 0 || value != 2400000000LL)
        return -1;
    if (m2sdr_cli_parse_int64(" 1.25M ", &value) != 0 || value != 1250000)
        return -1;
    if (m2sdr_cli_parse_int64("-2k", &value) != 0 || value != -2000)
        return -1;
    if (m2sdr_cli_parse_int64("10.5", &value) == 0)
        return -1;
    if (m2sdr_cli_parse_int64("10.01", &value) == 0)
        return -1;
    if (m2sdr_cli_parse_int64("30.72foo", &value) == 0)
        return -1;
    if (m2sdr_cli_parse_int64("nan", &value) == 0)
        return -1;
    if (m2sdr_cli_parse_int64(NULL, &value) == 0)
        return -1;
    if (m2sdr_cli_parse_int64("1", NULL) == 0)
        return -1;

    if (m2sdr_cli_parse_double("30.72e6", &dvalue) != 0 || dvalue != 30720000.0)
        return -1;
    if (m2sdr_cli_parse_double("2.4G", &dvalue) != 0 || dvalue != 2400000000.0)
        return -1;
    if (m2sdr_cli_parse_double(" 1.5k ", &dvalue) != 0 || dvalue != 1500.0)
        return -1;
    if (m2sdr_cli_parse_double("-1.25", &dvalue) != 0 || dvalue != -1.25)
        return -1;
    if (m2sdr_cli_parse_double("1.0foo", &dvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_double("inf", &dvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_double(NULL, &dvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_double("1", NULL) == 0)
        return -1;

    if (m2sdr_cli_parse_u64("0x100", &uvalue) != 0 || uvalue != 0x100)
        return -1;
    if (m2sdr_cli_parse_u64(" 42 ", &uvalue) != 0 || uvalue != 42)
        return -1;
    if (m2sdr_cli_parse_u64("-1", &uvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_u64("12bad", &uvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_u64("1k", &uvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_u64(NULL, &uvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_u64("1", NULL) == 0)
        return -1;

    if (m2sdr_cli_parse_u32("4294967295", &u32value) != 0 || u32value != UINT32_MAX)
        return -1;
    if (m2sdr_cli_parse_u32("4294967296", &u32value) == 0)
        return -1;
    if (m2sdr_cli_parse_u32("-1", &u32value) == 0)
        return -1;
    if (m2sdr_cli_parse_u32("1", NULL) == 0)
        return -1;

    if (m2sdr_cli_parse_u16("65535", &u16value) != 0 || u16value != UINT16_MAX)
        return -1;
    if (m2sdr_cli_parse_u16("65536", &u16value) == 0)
        return -1;
    if (m2sdr_cli_parse_u16("-1", &u16value) == 0)
        return -1;
    if (m2sdr_cli_parse_u16("1", NULL) == 0)
        return -1;

    if (m2sdr_cli_parse_u8("255", &u8value) != 0 || u8value != UINT8_MAX)
        return -1;
    if (m2sdr_cli_parse_u8("256", &u8value) == 0)
        return -1;
    if (m2sdr_cli_parse_u8("-1", &u8value) == 0)
        return -1;
    if (m2sdr_cli_parse_u8("1", NULL) == 0)
        return -1;

    if (m2sdr_cli_parse_bool("enabled", &bvalue) != 0 || !bvalue)
        return -1;
    if (m2sdr_cli_parse_bool("OFF", &bvalue) != 0 || bvalue)
        return -1;
    if (m2sdr_cli_parse_bool("maybe", &bvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_bool(NULL, &bvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_bool("true", NULL) == 0)
        return -1;

    if (m2sdr_cli_parse_uint_range("3", 0, 3, &unsigned_value) != 0 || unsigned_value != 3)
        return -1;
    if (m2sdr_cli_parse_uint_range("4", 0, 3, &unsigned_value) == 0)
        return -1;
    if (m2sdr_cli_parse_uint_range("1", 3, 0, &unsigned_value) == 0)
        return -1;
    if (m2sdr_cli_parse_uint_range("1", 0, 3, NULL) == 0)
        return -1;
    if (m2sdr_cli_parse_int_range("-1", -2, 2, &int_value) != 0 || int_value != -1)
        return -1;
    if (m2sdr_cli_parse_int_range("3", -2, 2, &int_value) == 0)
        return -1;
    if (m2sdr_cli_parse_int_range("1", 2, -2, &int_value) == 0)
        return -1;
    if (m2sdr_cli_parse_int_range("1", -2, 2, NULL) == 0)
        return -1;
    if (m2sdr_cli_parse_double_range("0.5", 0.0, 1.0, &dvalue) != 0 || dvalue != 0.5)
        return -1;
    if (m2sdr_cli_parse_double_range("1.5", 0.0, 1.0, &dvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_double_range("-1.5", -2.0, 0.0, &dvalue) != 0 || dvalue != -1.5)
        return -1;
    if (m2sdr_cli_parse_double_range("0.5", 1.0, 0.0, &dvalue) == 0)
        return -1;
    if (m2sdr_cli_parse_double_range("0.5", 0.0, 1.0, NULL) == 0)
        return -1;

    return 0;
}

static int test_cli_format_parser(void)
{
    enum m2sdr_format format = M2SDR_FORMAT_SC16_Q11;

    if (m2sdr_cli_parse_format("sc16", &format) != 0 || format != M2SDR_FORMAT_SC16_Q11)
        return -1;
    if (m2sdr_cli_parse_format("sc8", &format) != 0 || format != M2SDR_FORMAT_SC8_Q7)
        return -1;
    if (m2sdr_cli_parse_format("bfp8", &format) != 0 || format != M2SDR_FORMAT_BFP8_Q11)
        return -1;
    if (m2sdr_cli_parse_format("ci16", &format) == 0)
        return -1;
    if (m2sdr_cli_parse_format(NULL, &format) == 0)
        return -1;
    if (m2sdr_cli_parse_format("sc16", NULL) == 0)
        return -1;
    if (strcmp(m2sdr_cli_format_name(M2SDR_FORMAT_SC16_Q11), "sc16") != 0)
        return -1;
    if (strcmp(m2sdr_cli_format_name(M2SDR_FORMAT_SC8_Q7), "sc8") != 0)
        return -1;
    if (strcmp(m2sdr_cli_format_name(M2SDR_FORMAT_BFP8_Q11), "bfp8") != 0)
        return -1;

    return 0;
}

static int test_rx_gain_mode_helpers(void)
{
    enum m2sdr_rx_gain_mode mode = M2SDR_RX_GAIN_MODE_MANUAL;

    if (m2sdr_parse_rx_gain_mode("manual", &mode) != M2SDR_ERR_OK ||
        mode != M2SDR_RX_GAIN_MODE_MANUAL)
        return -1;
    if (m2sdr_parse_rx_gain_mode("slow", &mode) != M2SDR_ERR_OK ||
        mode != M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC)
        return -1;
    if (m2sdr_parse_rx_gain_mode("fast-attack", &mode) != M2SDR_ERR_OK ||
        mode != M2SDR_RX_GAIN_MODE_FAST_ATTACK_AGC)
        return -1;
    if (m2sdr_parse_rx_gain_mode("hybrid", &mode) != M2SDR_ERR_OK ||
        mode != M2SDR_RX_GAIN_MODE_HYBRID_AGC)
        return -1;
    if (m2sdr_parse_rx_gain_mode("bad", &mode) != M2SDR_ERR_PARSE)
        return -1;
    if (m2sdr_parse_rx_gain_mode(NULL, &mode) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_parse_rx_gain_mode("slow", NULL) != M2SDR_ERR_INVAL)
        return -1;

    if (strcmp(m2sdr_rx_gain_mode_name(M2SDR_RX_GAIN_MODE_MANUAL), "manual") != 0)
        return -1;
    if (strcmp(m2sdr_rx_gain_mode_name(M2SDR_RX_GAIN_MODE_SLOW_ATTACK_AGC), "slow") != 0)
        return -1;
    if (strcmp(m2sdr_rx_gain_mode_name(M2SDR_RX_GAIN_MODE_FAST_ATTACK_AGC), "fast") != 0)
        return -1;
    if (strcmp(m2sdr_rx_gain_mode_name(M2SDR_RX_GAIN_MODE_HYBRID_AGC), "hybrid") != 0)
        return -1;

    return 0;
}

static int test_format_size_helpers(void)
{
    if (m2sdr_format_size(M2SDR_FORMAT_SC16_Q11) != 4)
        return -1;
    if (m2sdr_format_size(M2SDR_FORMAT_SC8_Q7) != 2)
        return -1;
    if (m2sdr_format_size(M2SDR_FORMAT_BFP8_Q11) != M2SDR_BFP8_BLOCK_BYTES)
        return -1;
    if (m2sdr_bytes_to_samples(M2SDR_FORMAT_BFP8_Q11, M2SDR_BUFFER_BYTES) !=
        M2SDR_BUFFER_BYTES / M2SDR_BFP8_BLOCK_BYTES)
        return -1;
    if (m2sdr_samples_to_bytes(M2SDR_FORMAT_BFP8_Q11, 2) != 2 * M2SDR_BFP8_BLOCK_BYTES)
        return -1;

    return 0;
}

static int test_dma_header_helper(void)
{
    uint8_t header[16];
    uint64_t timestamp = 0;

    memset(header, 0, sizeof(header));
    m2sdr_tool_write_dma_header(header, 123456789ULL);

    if (m2sdr_tool_parse_dma_header(header, &timestamp) != 1)
        return -1;
    if (timestamp != 123456789ULL)
        return -1;

    header[0] ^= 0xff;
    timestamp = 0;
    if (m2sdr_tool_parse_dma_header(header, &timestamp) != 0)
        return -1;

    return 0;
}

static int test_stream_direction_validation(void)
{
    struct m2sdr_dev dev;
    struct m2sdr_sync_params params;
    int16_t samples[8];

    memset(&dev, 0, sizeof(dev));
    m2sdr_sync_params_init(&params);
    params.direction = (enum m2sdr_direction)42;
    params.buffer_size = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);

    if (m2sdr_sync_config_ex(&dev, &params) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_get_buffer(&dev, (enum m2sdr_direction)42, (void **)&params, &params.buffer_size, 1) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_try_get_buffer(&dev, (enum m2sdr_direction)42, (void **)&params, &params.buffer_size) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_sync_rx(&dev, samples, 8, NULL, 1) != M2SDR_ERR_STATE)
        return -1;
    if (m2sdr_sync_tx(&dev, samples, 8, NULL, 1) != M2SDR_ERR_STATE)
        return -1;

    return 0;
}

static int test_stream_config_size_validation(void)
{
    struct m2sdr_dev dev;
    unsigned sc16_samples = m2sdr_bytes_to_samples(M2SDR_FORMAT_SC16_Q11, M2SDR_BUFFER_BYTES);
    int log_enabled = m2sdr_log_is_enabled();
    int ret = -1;

    memset(&dev, 0, sizeof(dev));
    dev.fd = -1;
    m2sdr_set_log_enabled(false);

    if (m2sdr_sync_config(NULL, M2SDR_RX, M2SDR_FORMAT_SC16_Q11,
                          0, sc16_samples, 0, 1000) != M2SDR_ERR_INVAL)
        goto out;
    if (m2sdr_sync_config(&dev, M2SDR_RX, M2SDR_FORMAT_SC16_Q11,
                          0, 0, 0, 1000) != M2SDR_ERR_INVAL)
        goto out;
    if (m2sdr_sync_config(&dev, M2SDR_RX, M2SDR_FORMAT_SC16_Q11,
                          0, sc16_samples - 1, 0, 1000) != M2SDR_ERR_RANGE)
        goto out;
    if (m2sdr_sync_config(&dev, M2SDR_RX, (enum m2sdr_format)99,
                          0, sc16_samples, 0, 1000) != M2SDR_ERR_UNSUPPORTED)
        goto out;

    ret = 0;
out:
    m2sdr_set_log_enabled(log_enabled != 0);
    return ret;
}

static int test_stream_stats_validation(void)
{
    struct m2sdr_dev dev;
    struct m2sdr_stream_stats stats;

    memset(&dev, 0, sizeof(dev));
    dev.fd = -1;
    dev.transport = M2SDR_TRANSPORT_LITEPCIE;

    if (m2sdr_get_stream_stats(NULL, M2SDR_RX, &stats) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_get_stream_stats(&dev, (enum m2sdr_direction)42, &stats) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_get_stream_stats(&dev, M2SDR_RX, NULL) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_get_stream_stats(&dev, M2SDR_RX, &stats) != M2SDR_ERR_STATE)
        return -1;
    if (m2sdr_clear_stream_stats(NULL, M2SDR_RX) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_clear_stream_stats(&dev, (enum m2sdr_direction)42) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_clear_stream_stats(&dev, M2SDR_TX) != M2SDR_ERR_STATE)
        return -1;

    dev.transport = M2SDR_TRANSPORT_LITEETH;
    if (m2sdr_get_stream_stats(&dev, M2SDR_RX, &stats) != M2SDR_ERR_STATE)
        return -1;
    if (m2sdr_clear_stream_stats(&dev, M2SDR_RX) != M2SDR_ERR_UNSUPPORTED)
        return -1;

    return 0;
}

static int test_rf_range_validation(void)
{
    struct m2sdr_dev dev;
    struct m2sdr_config cfg;
    enum m2sdr_rx_gain_mode gain_mode = M2SDR_RX_GAIN_MODE_MANUAL;

    memset(&dev, 0, sizeof(dev));
    m2sdr_config_init(&cfg);

    cfg.sample_rate = -1;
    if (m2sdr_apply_config(&dev, &cfg) != M2SDR_ERR_RANGE)
        return -1;

    m2sdr_config_init(&cfg);
    cfg.tx_att = 100;
    if (m2sdr_apply_config(&dev, &cfg) != M2SDR_ERR_RANGE)
        return -1;

    m2sdr_config_init(&cfg);
    cfg.clock_source = (enum m2sdr_clock_source)99;
    if (m2sdr_apply_config(&dev, &cfg) != M2SDR_ERR_INVAL)
        return -1;

    if (m2sdr_set_sample_rate(&dev, -1) != M2SDR_ERR_RANGE)
        return -1;
    if (m2sdr_set_bandwidth(&dev, -1) != M2SDR_ERR_RANGE)
        return -1;

    if (m2sdr_set_channel_mode(NULL, 2, 0, 0) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_set_channel_mode(&dev, 3, 0, 0) != M2SDR_ERR_RANGE)
        return -1;
    if (m2sdr_set_channel_mode(&dev, 2, 2, 0) != M2SDR_ERR_RANGE)
        return -1;
    if (m2sdr_set_channel_mode(&dev, 2, 0, 0) != M2SDR_ERR_STATE)
        return -1;

    if (m2sdr_set_rx_gain_mode(NULL, 0, M2SDR_RX_GAIN_MODE_MANUAL) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_set_rx_gain_mode_all(NULL, M2SDR_RX_GAIN_MODE_MANUAL) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_set_rx_gain_mode(&dev, 2, M2SDR_RX_GAIN_MODE_MANUAL) != M2SDR_ERR_RANGE)
        return -1;
    if (m2sdr_set_rx_gain_mode(&dev, 0, (enum m2sdr_rx_gain_mode)99) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_set_rx_gain_mode(&dev, 0, M2SDR_RX_GAIN_MODE_MANUAL) != M2SDR_ERR_STATE)
        return -1;

    if (m2sdr_get_rx_gain_mode(NULL, 0, &gain_mode) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_get_rx_gain_mode(&dev, 0, NULL) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_get_rx_gain_mode(&dev, 2, &gain_mode) != M2SDR_ERR_RANGE)
        return -1;
    if (m2sdr_get_rx_gain_mode(&dev, 0, &gain_mode) != M2SDR_ERR_STATE)
        return -1;

    m2sdr_config_init(&cfg);
    if (!cfg.program_rx_gains || cfg.program_rx_gain_modes ||
        cfg.rx_gain_mode1 != M2SDR_RX_GAIN_MODE_MANUAL ||
        cfg.rx_gain_mode2 != M2SDR_RX_GAIN_MODE_MANUAL)
        return -1;
    cfg.program_rx_gain_modes = true;
    cfg.rx_gain_mode1 = (enum m2sdr_rx_gain_mode)99;
    if (m2sdr_apply_config(&dev, &cfg) != M2SDR_ERR_INVAL)
        return -1;

    if (m2sdr_set_agc_pin(NULL, true) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_get_agc_pin(NULL, NULL) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_configure_agc_counter(NULL, M2SDR_AGC_DETECTOR_RX1_LOW, NULL) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_clear_agc_counter(NULL, M2SDR_AGC_DETECTOR_RX1_LOW) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_get_agc_count(NULL, M2SDR_AGC_DETECTOR_RX1_LOW, NULL) != M2SDR_ERR_INVAL)
        return -1;

    return 0;
}

static int test_apply_config_if_needed_validation(void)
{
    struct m2sdr_dev dev;
    struct m2sdr_config cfg;
    struct m2sdr_config other;

    memset(&dev, 0, sizeof(dev));
    m2sdr_config_init(&cfg);

    if (m2sdr_apply_config_if_needed(NULL, &cfg) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_apply_config_if_needed(&dev, NULL) != M2SDR_ERR_INVAL)
        return -1;

    other = cfg;
    other.sample_rate = -1;
    if (m2sdr_apply_config_if_needed(&dev, &other) != M2SDR_ERR_RANGE)
        return -1;

    dev.ad9361_phy = (struct ad9361_rf_phy *)(uintptr_t)1;
    dev.rf_last_config = cfg;
    dev.rf_last_config_valid = 1;
    if (m2sdr_apply_config_if_needed(&dev, &cfg) != M2SDR_ERR_OK)
        return -1;

    other = cfg;
    other.rx_freq++;
    if (m2sdr_apply_config_if_needed(&dev, &other) != M2SDR_ERR_STATE)
        return -1;

    dev.rf_last_config_valid = 0;
    if (m2sdr_apply_config_if_needed(&dev, &cfg) != M2SDR_ERR_STATE)
        return -1;

    return 0;
}

static int test_transport_helpers(void)
{
    struct m2sdr_dev dev;
    enum m2sdr_transport_kind kind = M2SDR_TRANSPORT_KIND_UNKNOWN;

    memset(&dev, 0, sizeof(dev));
    dev.transport = M2SDR_TRANSPORT_LITEPCIE;
    dev.fd = 42;
    if (m2sdr_get_transport(&dev, &kind) != M2SDR_ERR_OK)
        return -1;
    if (kind != M2SDR_TRANSPORT_KIND_LITEPCIE)
        return -1;
    if (m2sdr_get_fd(&dev) != 42)
        return -1;
    if (m2sdr_get_eb_handle(&dev) != NULL)
        return -1;
    if ((intptr_t)m2sdr_get_handle(&dev) != 42)
        return -1;

    dev.transport = M2SDR_TRANSPORT_LITEETH;
    dev.eb = (struct eb_connection *)(uintptr_t)0x10000;
    if (m2sdr_get_transport(&dev, &kind) != M2SDR_ERR_OK)
        return -1;
    if (kind != M2SDR_TRANSPORT_KIND_LITEETH)
        return -1;
    if (m2sdr_get_fd(&dev) != -1)
        return -1;
    if (m2sdr_get_eb_handle(&dev) != dev.eb)
        return -1;
    if (m2sdr_get_handle(&dev) != dev.eb)
        return -1;

    if (m2sdr_get_transport(NULL, &kind) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_get_transport(&dev, NULL) != M2SDR_ERR_INVAL)
        return -1;

    return 0;
}

int main(void)
{
    if (test_parse_identifier_invalid_ports() != 0) {
        fprintf(stderr, "test_parse_identifier_invalid_ports failed\n");
        return 1;
    }
    if (test_parse_identifier_forms() != 0) {
        fprintf(stderr, "test_parse_identifier_forms failed\n");
        return 1;
    }
    if (test_cli_device_selection() != 0) {
        fprintf(stderr, "test_cli_device_selection failed\n");
        return 1;
    }
    if (test_discovery_targets() != 0) {
        fprintf(stderr, "test_discovery_targets failed\n");
        return 1;
    }
    if (test_cli_numeric_parser() != 0) {
        fprintf(stderr, "test_cli_numeric_parser failed\n");
        return 1;
    }
    if (test_cli_format_parser() != 0) {
        fprintf(stderr, "test_cli_format_parser failed\n");
        return 1;
    }
    if (test_rx_gain_mode_helpers() != 0) {
        fprintf(stderr, "test_rx_gain_mode_helpers failed\n");
        return 1;
    }
    if (test_format_size_helpers() != 0) {
        fprintf(stderr, "test_format_size_helpers failed\n");
        return 1;
    }
    if (test_dma_header_helper() != 0) {
        fprintf(stderr, "test_dma_header_helper failed\n");
        return 1;
    }
    if (test_stream_direction_validation() != 0) {
        fprintf(stderr, "test_stream_direction_validation failed\n");
        return 1;
    }
    if (test_stream_config_size_validation() != 0) {
        fprintf(stderr, "test_stream_config_size_validation failed\n");
        return 1;
    }
    if (test_stream_stats_validation() != 0) {
        fprintf(stderr, "test_stream_stats_validation failed\n");
        return 1;
    }
    if (test_rf_range_validation() != 0) {
        fprintf(stderr, "test_rf_range_validation failed\n");
        return 1;
    }
    if (test_apply_config_if_needed_validation() != 0) {
        fprintf(stderr, "test_apply_config_if_needed_validation failed\n");
        return 1;
    }
    if (test_transport_helpers() != 0) {
        fprintf(stderr, "test_transport_helpers failed\n");
        return 1;
    }

    printf("test_libm2sdr: ok\n");
    return 0;
}
