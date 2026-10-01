"""
canary_cmd.py - human-readable names for the first byte of an ISO14443-A frame
as recorded by nfc_canary (the `cmd` field of log records / BLE events).

Pure Python, no dependencies, so both chameleon_cli_unit.py and the standalone
canary_listen.py can import it.

The firmware only keeps byte 0 of the LAST frame seen at the deepest level, so
this is a best-effort label, not a full decode. Ambiguous bytes list every
plausible meaning. Bytes with no defined meaning are most often a Crypto1
encrypted frame (a MIFARE Classic reader mid-authentication).
"""

_NAMES = {
    # POLL (7-bit short frames)
    0x26: "REQA",
    0x52: "WUPA",
    # SELECT / anticollision
    0x93: "SEL CL1",
    0x95: "SEL CL2",
    0x97: "SEL CL3",
    # ISO14443-3/4
    0x50: "HLTA",
    0xE0: "RATS",
    # Magic-card (Gen1) backdoor wake-up
    0x40: "magic wake (Gen1, 7-bit)",
    0x43: "magic wake (Gen1, step 2)",
    # MIFARE Classic
    0x60: "AUTH-A (Classic) / GET_VERSION (UL/NTAG)",
    0x61: "AUTH-B (Classic)",
    0xA0: "WRITE (Classic)",
    0xC0: "DECREMENT",
    0xC1: "INCREMENT",
    0xC2: "RESTORE (Classic) / ISO-DEP DESELECT",
    0xB0: "TRANSFER",
    # Ultralight / NTAG
    0x30: "READ",
    0x3A: "FAST_READ",
    0x39: "READ_CNT",
    0x3C: "READ_SIG",
    0x1A: "AUTH (UL-C)",
    0x1B: "PWD_AUTH (NTAG/UL EV1)",
    0xA2: "WRITE (UL/NTAG)",
}


def canary_cmd_name(level: str, cmd: int) -> str:
    """level is the level name ('field'|'poll'|'select'|'engage')."""
    if level == 'field':
        return '-'
    name = _NAMES.get(cmd)
    if name:
        return name
    # ISO-DEP (14443-4) blocks, identified by PCB bit patterns
    if cmd & 0xE2 == 0x02:
        return "ISO-DEP I-block (APDU)"
    if cmd & 0xE6 == 0xA2:
        return "ISO-DEP R(ACK)"
    if cmd & 0xE6 == 0xB2:
        return "ISO-DEP R(NAK)"
    if cmd & 0xF7 == 0xC2:
        return "ISO-DEP DESELECT"
    if cmd & 0xF7 == 0xF2:
        return "ISO-DEP WTX"
    if cmd & 0xF0 == 0xD0:
        return "PPS"
    return "unknown (encrypted?)"
