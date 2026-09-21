"""PTM builds independently of the optional White Rabbit integration."""
import os
from pathlib import Path
import subprocess
import sys


def test_ptm_generates_without_white_rabbit(tmp_path):
    root = Path(__file__).resolve().parents[1]
    script = r'''
import importlib.abc
import importlib.util
import sys
from pathlib import Path

class NoWhiteRabbit(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "litex_wr_nic" or fullname.startswith("litex_wr_nic."):
            raise ImportError("PTM must not depend on White Rabbit")

sys.meta_path.insert(0, NoWhiteRabbit())
spec = importlib.util.spec_from_file_location("m2sdr_target", "litex_m2sdr.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
try:
    module.BaseSoC(with_pcie=False, with_pcie_ptm=True)
except ValueError as error:
    assert "requires PCIe" in str(error)
else:
    raise AssertionError("PTM without PCIe was silently ignored")

soc = module.BaseSoC(with_pcie=True, with_pcie_ptm=True)
from litex.soc.integration.builder import Builder
out = Path(sys.argv[1])
Builder(soc, output_dir=str(out), csr_csv=str(out / "csr.csv"),
        compile_software=False).build(build_name="ptm_test", run=False)
csr = (out / "csr.csv").read_text()
assert "csr_register,ptm_requester_control," in csr
assert "csr_register,ptm_requester_t1_time," in csr
assert "csr_register,ptm_requester_t4_time," in csr
assert not any(name.startswith("litex_wr_nic") for name in sys.modules)
rtl = (out / "gateware/ptm_test.v").read_text()
assert "sniffer_tap pcie_ptm_sniffer_tap(" in rtl
'''
    result = subprocess.run([sys.executable, "-c", script, str(tmp_path / "build")],
        cwd=root, env=dict(os.environ), text=True, capture_output=True)
    assert result.returncode == 0, result.stdout + result.stderr
