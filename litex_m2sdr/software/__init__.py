import os
import subprocess

# Generic PCIe Utilities ---------------------------------------------------------------------------

def get_pcie_device_ids(vendor, device):
    try:
        lspci_output = subprocess.check_output(["lspci", "-d", f"{vendor}:{device}"]).decode()
        # Split output into lines and extract device IDs (first field of each line)
        device_ids = [line.split()[0] for line in lspci_output.strip().split('\n') if line]
        return device_ids
    except subprocess.CalledProcessError:
        return []

def remove_pcie_device(device_id, driver="litepcie"):
    if not device_id:
        return
    subprocess.run(f"echo 1 | sudo tee /sys/bus/pci/devices/0000:{device_id}/remove > /dev/null", shell=True)

def rescan_pcie_bus():
    subprocess.run("echo 1 | sudo tee /sys/bus/pci/rescan > /dev/null", shell=True)

# LitePCIe Software Generation ---------------------------------------------------------------------

def generate_litepcie_software_headers(soc, dst):
    from litex.build import tools
    from litex.soc.integration.export import get_csr_header, get_soc_header, get_mem_header

    csr_header = get_csr_header(soc.csr_regions, soc.constants, with_csr_base_define=False, with_access_functions=False)
    tools.write_to_file(os.path.join(dst, "csr.h"), csr_header)
    soc_header = get_soc_header(soc.constants, with_access_functions=False)
    tools.write_to_file(os.path.join(dst, "soc.h"), soc_header)
    mem_header = get_mem_header(soc.mem_regions)
    tools.write_to_file(os.path.join(dst, "mem.h"), mem_header)

def generate_litepcie_software(soc, dst, use_litepcie_software=False):
    from litepcie.software import copy_litepcie_software

    if use_litepcie_software:
        cdir = os.path.abspath(os.path.dirname(__file__))
        os.system(f"cp {cdir}/__init__.py {cdir}/__init__.py.orig")
        copy_litepcie_software(dst)
        os.system(f"cp {cdir}/__init__.py.orig {cdir}/__init__.py")
    generate_litepcie_software_headers(soc, os.path.join(dst, "kernel"))
