#!/usr/bin/env python3

#
# This file is part of LiteX-M2SDR.
#
# Copyright (c) 2026 Enjoy-Digital <enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import ipaddress
import socket
import struct
import sys
import time


def parse_vrt_signal_packet(pkt: bytes):
    if len(pkt) < 20:
        raise ValueError("packet too short for VRT signal header")

    common = struct.unpack(">I", pkt[0:4])[0]
    packet_type = (common >> 28) & 0xF
    c = (common >> 27) & 0x1
    t = (common >> 26) & 0x1
    tsi_type = (common >> 22) & 0x3
    tsf_type = (common >> 20) & 0x3
    packet_count = (common >> 16) & 0xF
    packet_words = common & 0xFFFF

    if packet_type != 0x1:
        raise ValueError(f"unsupported VRT packet type {packet_type} (expected signal-data-with-stream-id=1)")
    if c or t:
        raise ValueError("class-id/trailer VRT packets not supported by this utility")
    if packet_words < 5:
        raise ValueError("invalid VRT packet size")

    expected_len = packet_words * 4
    if len(pkt) < expected_len:
        raise ValueError(f"truncated VRT packet ({len(pkt)} < {expected_len})")

    stream_id = struct.unpack(">I", pkt[4:8])[0]
    timestamp_int = struct.unpack(">I", pkt[8:12])[0]
    timestamp_fra = struct.unpack(">Q", pkt[12:20])[0]
    payload = pkt[20:expected_len]

    return {
        "packet_type": packet_type,
        "tsi_type": tsi_type,
        "tsf_type": tsf_type,
        "packet_count": packet_count,
        "packet_words": packet_words,
        "stream_id": stream_id,
        "timestamp_int": timestamp_int,
        "timestamp_fra": timestamp_fra,
        "payload": payload,
    }


def main():
    p = argparse.ArgumentParser(description="Receive and decode LiteX-M2SDR VRT RX packets")
    p.add_argument("--bind-ip", default="0.0.0.0", help="Local bind IP")
    p.add_argument("--port", type=int, default=4991, help="UDP port")
    p.add_argument("--group", default=None, help="Optional multicast group to join (e.g. 239.168.1.100)")
    p.add_argument("--iface-ip", default="0.0.0.0", help="Local interface IP for multicast join")
    p.add_argument("--count", type=int, default=0, help="Stop after N packets (0 = forever)")
    p.add_argument("--hexdump-bytes", type=int, default=0, help="Show first N payload bytes as hex")
    p.add_argument("--payload-out", default=None, help="Append raw payload bytes to file")
    p.add_argument("--channels", type=int, default=2, choices=[1, 2], help="Expected Soapy channel count for sample estimate")
    p.add_argument("--bytes-per-complex", type=int, default=2, choices=[2, 4], help="Expected bytes per complex sample per channel (8-bit=2, 16-bit=4)")
    args = p.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind_ip, args.port))

    if args.group:
        maddr = ipaddress.ip_address(args.group)
        if not maddr.is_multicast:
            raise SystemExit(f"{args.group} is not multicast")
        mreq = socket.inet_aton(args.group) + socket.inet_aton(args.iface_ip)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    fout = open(args.payload_out, "ab") if args.payload_out else None

    try:
        seen = 0
        last_pc = None
        while True:
            pkt, addr = sock.recvfrom(65535)
            ts_local = time.time()
            try:
                v = parse_vrt_signal_packet(pkt)
            except Exception as e:
                print(f"[drop] {addr[0]}:{addr[1]} len={len(pkt)} err={e}", file=sys.stderr)
                continue

            payload = v["payload"]
            samples = len(payload) // (args.channels * args.bytes_per_complex)
            ts_ns = v["timestamp_int"] * 1_000_000_000 + (v["timestamp_fra"] // 1000)
            pc_gap = None if last_pc is None else ((v["packet_count"] - last_pc) & 0xF)
            last_pc = v["packet_count"]

            print(
                f"pkt#{seen:06d} from {addr[0]}:{addr[1]} "
                f"pc={v['packet_count']:x}"
                f"{'' if pc_gap is None else f' gap={pc_gap}'} "
                f"sid=0x{v['stream_id']:08x} "
                f"words={v['packet_words']} payload={len(payload)}B "
                f"samples~{samples} "
                f"tsi={v['tsi_type']} tsf={v['tsf_type']} "
                f"vrt_ns={ts_ns} local={ts_local:.6f}"
            )

            if args.hexdump_bytes > 0:
                n = min(args.hexdump_bytes, len(payload))
                print("  payload:", payload[:n].hex(" "))

            if fout:
                fout.write(payload)
                fout.flush()

            seen += 1
            if args.count and seen >= args.count:
                break
    finally:
        if fout:
            fout.close()
        sock.close()


if __name__ == "__main__":
    main()

