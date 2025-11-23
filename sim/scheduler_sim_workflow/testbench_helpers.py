def drive_packet(dut, ts_when_due, packet_id=0, header=0xDEADBEEF, frames_per_packet=1024, verbose=False):
    """
    Drive one packet into sched.sink with a given timestamp.
    
    Args:
        dut: Device under test
        ts_when_due: Timestamp for this packet
        packet_id: Packet identifier (for logging)
        header: Header word (default 0xDEADBEEF)
        frames_per_packet: Number of frames in the packet
    """
    print(f"      [drive] Packet {packet_id}: ts={ts_when_due}, frames={frames_per_packet}")
    
    yield dut.sink.valid.eq(1)
    sink_ready = (yield dut.sink.ready)
    
    # Wait until FIFO can accept
    while sink_ready == 0:
        print(f"      [drive] Waiting for sink.ready...")
        yield
        sink_ready = (yield dut.sink.ready)
    
    # Drive the frames
    for i in range(frames_per_packet):
        if verbose:        print(f"[stim] Driving word {i}") 
        yield dut.sink.valid.eq(1)
        yield dut.sink.first.eq(i == 0)
        yield dut.sink.last.eq(i == (frames_per_packet - 1))
        
        if i == 0:
            yield dut.sink.data.eq(header)
        elif i == 1:
            yield dut.sink.data.eq(ts_when_due)
        else:
            yield dut.sink.data.eq((packet_id << 16) | i)
        
        # Wait for handshake
        while True:
            ready = (yield dut.sink.ready)
            yield
            if ready:
                break
    
    yield dut.sink.valid.eq(0)
    print(f"      [drive] Packet {packet_id}: Complete\n")


def wait(wait_cycles):
    """Wait for N cycles."""
    for _ in range(wait_cycles):
        yield
