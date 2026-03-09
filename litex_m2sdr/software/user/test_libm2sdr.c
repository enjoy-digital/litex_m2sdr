/* SPDX-License-Identifier: BSD-2-Clause */

#include <stdint.h>
#include <stdio.h>
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

    printf("test_libm2sdr: ok\n");
    return 0;
}
