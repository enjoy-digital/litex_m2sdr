
import argparse
from litex import RemoteClient

def test_pcie_bridge():
    # Create Bus.
    bus = RemoteClient()
    bus.open()

    # # #

    # Access SDRAM
    bus.write(0x40000, 0x1234)
    value = bus.read(0x40000)
    print("SDRAM read/write test: wrote 0x1234, read 0x{:08x}".format(value))

    # # Access SDRAM (with bus.mems and base address)
    # bus.write(bus.mems.main_ram.base, 0x12345678)
    # value = bus.read(bus.mems.main_ram.base)

    # Trigger a reset of the SoC
    # bus.regs.ctrl_reset.write(1)
    
    # Dump all CSR registers of the SoC
    for name, reg in bus.regs.__dict__.items():
        print("0x{:08x} : 0x{:08x} {}".format(reg.addr, reg.read(), name))


def main():
    # parser = argparse.ArgumentParser()
    # parser.add_argument("--header",       default="header",       help="Header Module Name.")
    # parser.add_argument("--frame-size",   default=1024, type=int, help="Header Frame Size.")
    # parser.add_argument("--loops",        default=8,    type=int, help="Test Loops.")
    # args = parser.parse_args()
    test_pcie_bridge()




if __name__ == "__main__":
    main()
