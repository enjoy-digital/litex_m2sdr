#!/usr/bin/env python3

import re
import argparse
import subprocess

# Test Constants -----------------------------------------------------------------------------------

SAMPLERATES = [
    2.5e6,
    5.0e6,
    10.0e6,
    20.0e6,
    30.72e6,
    61.44e6,
]

VCC_INT_NOMINAL  = 1.00
VCC_AUX_NOMINAL  = 1.80
VCC_BRAM_NOMINAL = 1.00
VCC_MARGIN       = 0.10

FPGA_PCIE_VENDOR_ID          = "0x10ee"
FPGA_PCIE_DEVICE_IDS         = ["0x7021", "0x7022", "0x7024"]
FPGA_PCIE_SPEED_NOMINAL      = "5.0 GT/s PCIe"
FPGA_PCIE_LINK_WIDTH_NOMINAL = "4"

AD9361_PRODUCT_ID = "000a"

# Color Constants ----------------------------------------------------------------------------------

ANSI_COLOR_RED    = "\x1b[31m"
ANSI_COLOR_GREEN  = "\x1b[32m"
ANSI_COLOR_YELLOW = "\x1b[33m"
ANSI_COLOR_BLUE   = "\x1b[34m"
ANSI_COLOR_RESET  = "\x1b[0m"

# Helpers ------------------------------------------------------------------------------------------

def print_pass():
    print(f"{ANSI_COLOR_GREEN}[PASS]{ANSI_COLOR_RESET}")

def print_fail():
    print(f"{ANSI_COLOR_RED}[FAIL]{ANSI_COLOR_RESET}")

def print_result(condition):
    if condition:
        print_pass()
    else:
        print_fail()
    return not condition

def within_margin(value, nominal, margin=0.10):
    return nominal * (1 - margin) <= value <= nominal * (1 + margin)

def get_pcie_device_id(vendor, device):
    try:
        lspci_output = subprocess.check_output(["lspci", "-d", f"{vendor}:{device}"]).decode()
        device_id = lspci_output.split()[0]
        return device_id
    except:
        return None

def verify_pcie_speed(device_id):
    try:
        device_path = f"/sys/bus/pci/devices/0000:{device_id}"
        current_link_speed = subprocess.check_output(f"cat {device_path}/current_link_speed", shell=True).decode().strip()
        current_link_width = subprocess.check_output(f"cat {device_path}/current_link_width", shell=True).decode().strip()

        speed_check = (current_link_speed == FPGA_PCIE_SPEED_NOMINAL)
        width_check = (current_link_width == FPGA_PCIE_LINK_WIDTH_NOMINAL)

        return current_link_speed, current_link_width, speed_check and width_check
    except:
        return None, None, False

# PCIe Device Test ---------------------------------------------------------------------------------

def pcie_device_autotest():
    print("PCIe Device Autotest...", end="")

    errors = 1
    device_ids = [get_pcie_device_id(FPGA_PCIE_VENDOR_ID, device_id) for device_id in FPGA_PCIE_DEVICE_IDS]
    for device_id in device_ids:
        if device_id:
            print(f"\n\tChecking PCIe Device {device_id}: ", end="")
            device_present = device_id is not None
            errors += print_result(device_present)
            if device_present:
                print(f"\tVerifying PCIe speed for {device_id}: ", end="")
                current_link_speed, current_link_width, speed_check = verify_pcie_speed(device_id)
                print(f"[{ANSI_COLOR_BLUE}{current_link_speed} x{current_link_width}{ANSI_COLOR_RESET}] ", end="")
                return print_result(speed_check)
    print_fail()
    return errors

# M2SDR Util Test ----------------------------------------------------------------------------------

def m2sdr_util_info_autotest():
    print("M2SDR Util Info Autotest...")
    result = subprocess.run("cd user && ./m2sdr_util info", shell=True, capture_output=True, text=True)
    output = result.stdout

    # Check for LiteX-M2SDR presence in identifier.
    print("\tChecking LiteX-M2SDR identifier: ", end="")
    if print_result("LiteX-M2SDR" in output):
        return 1

    # Extract FPGA DNA, temperatures, VCC values, and AD9361 info.
    fpga_dna_match          = re.search(r"FPGA DNA\s*:\s*(0x\w+)", output)
    fpga_temp_match         = re.search(r"FPGA Temperature\s*:\s*([\d.]+) °C", output)
    vcc_int_match           = re.search(r"FPGA VCC-INT\s*:\s*([\d.]+) V", output)
    vcc_aux_match           = re.search(r"FPGA VCC-AUX\s*:\s*([\d.]+) V", output)
    vcc_bram_match          = re.search(r"FPGA VCC-BRAM\s*:\s*([\d.]+) V", output)
    ad9361_product_id_match = re.search(r"AD9361 Product ID\s*:\s*(\w+)", output)
    ad9361_temp_match       = re.search(r"AD9361 Temperature\s*:\s*([\d.]+) °C", output)

    if not (fpga_dna_match and fpga_temp_match and vcc_int_match and vcc_aux_match and vcc_bram_match and ad9361_product_id_match and ad9361_temp_match):
        print("Failed to retrieve necessary information from m2sdr_util info.")
        print_fail()
        return 1

    fpga_dna          = fpga_dna_match.group(1)
    fpga_temp         = float(fpga_temp_match.group(1))
    vcc_int           = float(vcc_int_match.group(1))
    vcc_aux           = float(vcc_aux_match.group(1))
    vcc_bram          = float(vcc_bram_match.group(1))
    ad9361_product_id = ad9361_product_id_match.group(1)
    ad9361_temp       = float(ad9361_temp_match.group(1))

    errors = 0

    # Print FPGA DNA/Temperature and AD9361 Temperature.
    print(f"\tFPGA DNA: \t\t\t[{ANSI_COLOR_BLUE}{fpga_dna}{ANSI_COLOR_RESET}]")
    print(f"\tFPGA Temperature: \t\t[{ANSI_COLOR_BLUE}{fpga_temp} °C{ANSI_COLOR_RESET}]")

    # Print/Verify VCC values.
    print(f"\tChecking VCCINT: \t\t[{ANSI_COLOR_BLUE}{vcc_int} V{ANSI_COLOR_RESET}] ", end="")
    errors += print_result(within_margin(vcc_int, VCC_INT_NOMINAL, VCC_MARGIN))
    print(f"\tChecking VCCAUX: \t\t[{ANSI_COLOR_BLUE}{vcc_aux} V{ANSI_COLOR_RESET}] ", end="")
    errors += print_result(within_margin(vcc_aux, VCC_AUX_NOMINAL, VCC_MARGIN))
    print(f"\tChecking VCCBRAM: \t\t[{ANSI_COLOR_BLUE}{vcc_bram} V{ANSI_COLOR_RESET}] ", end="")
    errors += print_result(within_margin(vcc_bram, VCC_BRAM_NOMINAL, VCC_MARGIN))

    # Print/Verify AD9361 Product ID/Temperature.
    print(f"\tChecking AD9361 Product ID: \t[{ANSI_COLOR_BLUE}{ad9361_product_id}{ANSI_COLOR_RESET}] ", end="")
    errors += print_result(ad9361_product_id == AD9361_PRODUCT_ID)
    print(f"\tAD9361 Temperature: \t\t[{ANSI_COLOR_BLUE}{ad9361_temp} °C{ANSI_COLOR_RESET}]")

    return errors

# M2SDR RF Test ------------------------------------------------------------------------------------

def m2sdr_rf_autotest():
    print("M2SDR RF Autotest...")
    errors = 0
    for samplerate in SAMPLERATES:
        print(f"\tRF Init @ {samplerate/1e6:3.2f}MSPS...", end="")
        log = subprocess.run(f"cd user && ./m2sdr_rf -samplerate={samplerate}", shell=True, capture_output=True, text=True)
        errors += print_result("AD936x Rev 2 successfully initialized" in log.stdout)
    return errors

# Main ---------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="LiteX M2SDR board Autotest.")
    args = parser.parse_args()

    print("\nLITEX M2SDR AUTOTEST\n" + "-"*40)

    errors = 0

    # PCIe Device Autotest.
    errors += pcie_device_autotest()

    # M2SDR Util Info Autotest.
    errors += m2sdr_util_info_autotest()

    # M2SDR RF Autotest.
    errors += m2sdr_rf_autotest()

    print("\n" + "-"*40)

    if errors:
        print(f"LITEX M2SDR AUTOTEST [{ANSI_COLOR_RED}FAIL{ANSI_COLOR_RESET}]")
    else:
        print(f"LITEX M2SDR AUTOTEST [{ANSI_COLOR_GREEN}PASS{ANSI_COLOR_RESET}]")

if __name__ == '__main__':
    main()
