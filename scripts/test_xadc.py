#!/usr/bin/env python3

import argparse

from litex import RemoteClient

# XADC Driver --------------------------------------------------------------------------------------

class XADCDriver:
    def __init__(self, bus):
        """
        Initialize the driver with a RemoteClient bus instance.
        """
        self.bus = bus

    def get_temp(self):
        """Return the temperature in °C."""
        raw = self.bus.regs.xadc_temperature.read()
        return raw * 503.975 / 4096 - 273.15

    def get_vccint(self):
        """Return the VCCINT voltage in V."""
        raw = self.bus.regs.xadc_vccint.read()
        return raw * 3 / 4096

    def get_vccaux(self):
        """Return the VCCAUX voltage in V."""
        raw = self.bus.regs.xadc_vccaux.read()
        return raw * 3 / 4096

    def get_vccbram(self):
        """Return the VCCBRAM voltage in V."""
        raw = self.bus.regs.xadc_vccbram.read()
        return raw * 3 / 4096

    def gen_data(self, channel, n):
        """
        Generator yielding a sliding window (length n) of measurements for the specified channel.

        Args:
            channel (str): One of 'temp', 'vccint', 'vccaux', or 'vccbram'.
            n (int): The number of data points to maintain.
        Yields:
            list: A list of n most recent measurements.
        """
        if channel == "temp":
            getter = self.get_temp
        elif channel == "vccint":
            getter = self.get_vccint
        elif channel == "vccaux":
            getter = self.get_vccaux
        elif channel == "vccbram":
            getter = self.get_vccbram
        else:
            raise ValueError(f"Unknown channel: {channel}")

        data = [getter()] * n
        while True:
            data.pop(-1)
            data.insert(0, getter())
            yield data

# Main ---------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LiteX M2SDR XADC Test Script")
    parser.add_argument("--csr-csv", default="csr.csv", help="CSR definition file (default: csr.csv)")
    parser.add_argument("--host",    default="localhost", help="Remote host (default: localhost)")
    parser.add_argument("--port",    default=1234, type=int, help="Remote port (default: 1234)")
    args = parser.parse_args()

    bus = RemoteClient(host=args.host, port=args.port, csr_csv=args.csr_csv)
    bus.open()

    xadc = XADCDriver(bus)
    print("XADC Readings:")
    print(f"  Temperature: {xadc.get_temp():.2f} °C")
    print(f"  VCCINT:      {xadc.get_vccint():.2f} V")
    print(f"  VCCAUX:      {xadc.get_vccaux():.2f} V")
    print(f"  VCCBRAM:     {xadc.get_vccbram():.2f} V")

    bus.close()

if __name__ == "__main__":
    main()
