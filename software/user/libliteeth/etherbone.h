/*
 * Etherbone C Library.
 *
 * Copyright (c) 2020-2025 LiteX-Hub community.
 * SPDX-License-Identifier: Apache-2.0
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Copied from https://github.com/litex-hub/wishbone-utils/tree/master/libeb-c
 */

#ifndef __ETHERBONE_H__
#define __ETHERBONE_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stdint.h>

/*

The EtherBone record has a struct that looks like this:

struct etherbone_record {
	// 1...
	uint8_t bca : 1;
	uint8_t rca : 1;
	uint8_t rff : 1;
	uint8_t ign1 : 1;
	uint8_t cyc : 1;
	uint8_t wca : 1;
	uint8_t wff : 1;
	uint8_t ign2 : 1;

	uint8_t byte_enable;

	uint8_t wcount;

	uint8_t rcount;

	uint32_t write_addr;
    union {
    	uint32_t value;
        read_addr;
    };
} __attribute__((packed));

This is wrapped inside of an EtherBone network packet header:

struct etherbone_packet {
	uint8_t magic[2]; // 0x4e 0x6f
	uint8_t version : 4;
	uint8_t ign : 1;
	uint8_t no_reads : 1;
	uint8_t probe_reply : 1;
	uint8_t probe_flag : 1;
	uint8_t port_size : 4;
	uint8_t addr_size : 4;
	uint8_t padding[4];

	struct etherbone_record records[0];
} __attribute__((packed));

LiteX only supports a single record per packet, so either wcount or rcount
is set to 1.  For a read, the read_addr is specified.  For a write, the
write_addr is specified along with a value.

The same type of record is returned, so your data is at offset 16.
*/

struct eb_connection;

int eb_unfill_read32(uint8_t wb_buffer[20]);
int eb_fill_write32(uint8_t wb_buffer[20], uint32_t data, uint32_t address);
int eb_fill_read32(uint8_t wb_buffer[20], uint32_t address);

struct eb_connection *eb_connect(const char *addr, const char *port, int is_direct);
void eb_disconnect(struct eb_connection **conn);
uint32_t eb_read32(struct eb_connection *conn, uint32_t addr);
void eb_write32(struct eb_connection *conn, uint32_t val, uint32_t addr);

#ifdef __cplusplus
};
#endif /* __cplusplus */

#endif /* __ETHERBONE_H__ */
