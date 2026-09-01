import importlib.util
from pathlib import Path


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"


def _load_script(name):
    path = SCRIPTS / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


eth_phy_prbs = _load_script("eth_phy_prbs")
sfp_eeprom = _load_script("sfp_eeprom")

control_word = eth_phy_prbs.control_word
decode_status = eth_phy_prbs.decode_status
checksum = sfp_eeprom.checksum
identity = sfp_eeprom.identity
text_field = sfp_eeprom.text_field


def test_sfp_identity_and_checksums():
    data = bytearray(96)
    data[0] = 0x03
    data[2] = 0x22
    data[11] = 0x06
    data[12] = 103
    data[20:36] = b"6COM            "
    data[37:40] = bytes.fromhex("001b21")
    data[40:56] = b"6C-SFP-10G-T    "
    data[56:60] = b"A1  "
    data[68:84] = b"TEST1234        "
    data[84:92] = b"260827  "
    data[63] = sum(data[0:63]) & 0xff
    data[95] = sum(data[64:95]) & 0xff

    fields = identity(data)
    assert fields["vendor"] == "6COM"
    assert fields["part_number"] == "6C-SFP-10G-T"
    assert fields["nominal_bitrate_mbd"] == 10300
    assert checksum(data, 0, 62, 63)[0]
    assert checksum(data, 64, 94, 95)[0]


def test_sfp_text_field_replaces_non_printable_bytes():
    assert text_field(b"ABC\x00DEF ", 0, 8) == "ABC.DEF"


def test_eth_phy_control_word_layout():
    assert control_word("near-pma", "prbs31", "prbs31") == (
        0b010 | (0b100 << 4) | (0b100 << 8)
    )
    assert control_word("normal", "prbs7", "prbs15", True, True) == (
        (0b001 << 4) | (0b010 << 8) | (1 << 12) | (1 << 13)
    )


def test_eth_phy_status_decode():
    status = decode_status((1 << 0) | (1 << 1) | (1 << 4) | (1 << 7))
    assert status["qpll_lock"]
    assert status["rx_cdr_lock"]
    assert status["byte_aligned"]
    assert status["rx_prbs_error"]
    assert not status["pcs_link_up"]
