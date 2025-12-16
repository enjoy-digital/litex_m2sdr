def drive_packet(dut, ts_when_due, packet_id=0, test_id = 1, header=0xDEADBEEF, frames_per_packet=1024, verbose=False):
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
    yield dut.enable.eq(1)
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
            yield dut.sink.data.eq((test_id << 32) | (packet_id << 16) | i)
        
        # Wait for handshake
        while True:
            ready = (yield dut.sink.ready)
            yield
            if ready:
                break
    
    yield dut.sink.valid.eq(0)
    print(f"      [drive] Packet {packet_id}: Complete\n")

def write_manual_time(dut, new_time):
    """Write a new manual time to the scheduler."""
    print(f"[CSR] Writing manual time: {new_time}")
    yield dut._write_time.storage.eq(new_time)
    yield dut.control.fields.write.eq(1)
    yield
    yield dut.control.fields.write.eq(0)
    print(f"[CSR] Manual time write complete.\n")

def read_time(dut):
    """Write a new manual time to the scheduler."""
    print(f"[CSR] Reading current time")
    yield dut.control.fields.read.eq(1)
    yield
    yield dut.control.fields.read.eq(0)
    print(f"[CSR] Time read complete.\n")

def read_current_ts(dut):
    """Read the current timestamp from the scheduler."""
    print(f"[CSR] Reading current timestamp")
    yield dut.control.fields.read_current_ts.eq(1)
    yield
    yield dut.control.fields.read_current_ts.eq(0)
    current_ts = (yield dut._current_ts.status)
    print(f"[CSR] Current timestamp read complete: {current_ts}\n")
    return current_ts

def read_fifo_level(dut):
    """Read the current FIFO level from the scheduler."""
    print(f"[CSR] Reading FIFO level")
    fifo_level = (yield dut._fifo_level.status)
    print(f"[CSR] FIFO level read complete: {fifo_level}\n")
    return fifo_level

def wait(wait_cycles):
    """Wait for N cycles."""
    for _ in range(wait_cycles):
        yield
