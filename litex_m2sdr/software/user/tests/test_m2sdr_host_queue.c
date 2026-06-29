/* SPDX-License-Identifier: BSD-2-Clause */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../m2sdr_host_queue.h"

static int expect_pop(struct m2sdr_host_queue *queue,
                      const char *expected,
                      uint64_t expected_tag)
{
    char buf[8] = {0};
    size_t len = 0;
    uint64_t tag = 0;

    if (m2sdr_host_queue_pop(queue, buf, sizeof(buf), &len, &tag) != M2SDR_HOST_QUEUE_OK)
        return -1;
    if (len != strlen(expected) || memcmp(buf, expected, len) != 0)
        return -1;
    if (tag != expected_tag)
        return -1;
    return 0;
}

static int test_fifo_and_close(void)
{
    struct m2sdr_host_queue queue;
    char buf[8];
    size_t len = 0;

    if (m2sdr_host_queue_init(&queue, 2, sizeof(buf)) != M2SDR_HOST_QUEUE_OK)
        return -1;
    if (m2sdr_host_queue_push(&queue, "aa", 2, 10) != M2SDR_HOST_QUEUE_OK)
        return -1;
    if (m2sdr_host_queue_push(&queue, "bbb", 3, 11) != M2SDR_HOST_QUEUE_OK)
        return -1;
    if (expect_pop(&queue, "aa", 10) != 0)
        return -1;
    if (expect_pop(&queue, "bbb", 11) != 0)
        return -1;
    m2sdr_host_queue_close_writer(&queue);
    if (m2sdr_host_queue_pop(&queue, buf, sizeof(buf), &len, NULL) != M2SDR_HOST_QUEUE_CLOSED)
        return -1;
    m2sdr_host_queue_destroy(&queue);
    return 0;
}

int main(void)
{
    if (test_fifo_and_close() != 0) {
        fprintf(stderr, "test_fifo_and_close failed\n");
        return 1;
    }
    printf("test_m2sdr_host_queue: ok\n");
    return 0;
}
