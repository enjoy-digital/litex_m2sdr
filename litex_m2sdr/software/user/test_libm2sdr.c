/* SPDX-License-Identifier: BSD-2-Clause */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libm2sdr/m2sdr.h"
#include "libm2sdr/m2sdr_internal.h"

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
    if (m2sdr_sync_rx(&dev, samples, 8, NULL, 1) != M2SDR_ERR_STATE)
        return -1;
    if (m2sdr_sync_tx(&dev, samples, 8, NULL, 1) != M2SDR_ERR_STATE)
        return -1;

    return 0;
}

static int test_rf_range_validation(void)
{
    struct m2sdr_dev dev;
    struct m2sdr_config cfg;

    memset(&dev, 0, sizeof(dev));
    m2sdr_config_init(&cfg);

    cfg.sample_rate = -1;
    if (m2sdr_apply_config(&dev, &cfg) != M2SDR_ERR_RANGE)
        return -1;

    m2sdr_config_init(&cfg);
    cfg.tx_gain = -100;
    if (m2sdr_apply_config(&dev, &cfg) != M2SDR_ERR_RANGE)
        return -1;
    if (m2sdr_set_sample_rate(&dev, -1) != M2SDR_ERR_RANGE)
        return -1;
    if (m2sdr_set_bandwidth(&dev, -1) != M2SDR_ERR_RANGE)
        return -1;

    return 0;
}

static int test_transport_helpers(void)
{
    struct m2sdr_dev dev;
    enum m2sdr_transport_kind kind = M2SDR_TRANSPORT_KIND_UNKNOWN;

    memset(&dev, 0, sizeof(dev));
    dev.transport = M2SDR_TRANSPORT_LITEPCIE;
    if (m2sdr_get_transport(&dev, &kind) != M2SDR_ERR_OK)
        return -1;
    if (kind != M2SDR_TRANSPORT_KIND_LITEPCIE)
        return -1;
    if (m2sdr_get_eb_handle(&dev) != NULL)
        return -1;
    if (m2sdr_get_transport(NULL, &kind) != M2SDR_ERR_INVAL)
        return -1;
    if (m2sdr_get_transport(&dev, NULL) != M2SDR_ERR_INVAL)
        return -1;

    return 0;
}

static int test_rfic_helpers(void)
{
    struct m2sdr_dev dev;
    struct m2sdr_rfic_caps caps;
    char name[32];
    char value[32];

    memset(&dev, 0, sizeof(dev));

    if (m2sdr_get_rfic_name(&dev, name, sizeof(name)) != M2SDR_ERR_OK)
        return -1;
    if (strcmp(name, "ad9361") != 0)
        return -1;

    if (m2sdr_get_rfic_caps(&dev, &caps) != M2SDR_ERR_OK)
        return -1;
    if (caps.kind != M2SDR_RFIC_KIND_AD9361)
        return -1;
    if ((caps.features & M2SDR_RFIC_FEATURE_BIND_EXTERNAL) == 0)
        return -1;
    if (caps.native_iq_bits != 12u)
        return -1;
    if ((caps.supported_iq_bits_mask & M2SDR_IQ_BITS_MASK(8)) == 0)
        return -1;
    if ((caps.supported_iq_bits_mask & M2SDR_IQ_BITS_MASK(12)) == 0)
        return -1;

    if (m2sdr_set_iq_bits(&dev, 7) != M2SDR_ERR_RANGE)
        return -1;
    if (m2sdr_set_iq_bits(&dev, 16) != M2SDR_ERR_RANGE)
        return -1;
    if (m2sdr_get_iq_bits(&dev, NULL) != M2SDR_ERR_INVAL)
        return -1;

    if (m2sdr_set_property(&dev, "ad9361.test", "1") != M2SDR_ERR_UNSUPPORTED)
        return -1;
    if (m2sdr_get_property(&dev, "ad9361.test", value, sizeof(value)) != M2SDR_ERR_UNSUPPORTED)
        return -1;

    return 0;
}

static int test_mock_backend(void)
{
    struct m2sdr_dev dev;
    struct m2sdr_rfic_caps caps;
    uint64_t freq = 0;
    int64_t value = 0;
    unsigned bits = 0;
    char name[32];

    memset(&dev, 0, sizeof(dev));
    setenv("M2SDR_RFIC", "mock", 1);

    if (m2sdr_get_rfic_name(&dev, name, sizeof(name)) != M2SDR_ERR_OK)
        return -1;
    if (strcmp(name, "mock") != 0)
        return -1;
    if (m2sdr_get_rfic_caps(&dev, &caps) != M2SDR_ERR_OK)
        return -1;
    if (caps.kind != M2SDR_RFIC_KIND_MOCK)
        return -1;
    if (caps.native_iq_bits != 16u)
        return -1;

    if (m2sdr_set_frequency(&dev, M2SDR_RX, 123456789ULL) != M2SDR_ERR_OK)
        return -1;
    if (m2sdr_get_frequency(&dev, M2SDR_RX, &freq) != M2SDR_ERR_OK || freq != 123456789ULL)
        return -1;
    if (m2sdr_set_sample_rate(&dev, 2000000) != M2SDR_ERR_OK)
        return -1;
    if (m2sdr_get_sample_rate(&dev, &value) != M2SDR_ERR_OK || value != 2000000)
        return -1;
    if (m2sdr_set_bandwidth(&dev, 1500000) != M2SDR_ERR_OK)
        return -1;
    if (m2sdr_get_bandwidth(&dev, &value) != M2SDR_ERR_OK || value != 1500000)
        return -1;
    if (m2sdr_set_gain(&dev, M2SDR_TX, -5) != M2SDR_ERR_OK)
        return -1;
    if (m2sdr_get_gain(&dev, M2SDR_TX, &value) != M2SDR_ERR_OK || value != -5)
        return -1;
    if (m2sdr_set_iq_bits(&dev, 16) != M2SDR_ERR_OK)
        return -1;
    if (m2sdr_get_iq_bits(&dev, &bits) != M2SDR_ERR_OK || bits != 16u)
        return -1;
    if (m2sdr_rfic_configure_stream_channels(&dev, 1, (const size_t[]){0}, 1, (const size_t[]){0}) != M2SDR_ERR_UNSUPPORTED)
        return -1;

    m2sdr_rfic_detach(&dev);
    unsetenv("M2SDR_RFIC");
    return 0;
}

int main(void)
{
    if (test_parse_identifier_invalid_ports() != 0) {
        fprintf(stderr, "test_parse_identifier_invalid_ports failed\n");
        return 1;
    }
    if (test_stream_direction_validation() != 0) {
        fprintf(stderr, "test_stream_direction_validation failed\n");
        return 1;
    }
    if (test_rf_range_validation() != 0) {
        fprintf(stderr, "test_rf_range_validation failed\n");
        return 1;
    }
    if (test_transport_helpers() != 0) {
        fprintf(stderr, "test_transport_helpers failed\n");
        return 1;
    }
    if (test_rfic_helpers() != 0) {
        fprintf(stderr, "test_rfic_helpers failed\n");
        return 1;
    }
    if (test_mock_backend() != 0) {
        fprintf(stderr, "test_mock_backend failed\n");
        return 1;
    }

    printf("test_libm2sdr: ok\n");
    return 0;
}
