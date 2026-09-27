"""cli_core — shared foundation for the Chameleon CLI command modules.

Imports, module-level helpers, base *Unit classes, and the CLITree group
definitions (root, hw, hf, hf_mf, ...). Every cli_* command module imports
the groups and bases from here and registers its commands by decorator side
effect. Split out of the former monolithic chameleon_cli_unit.py."""

import binascii
import glob
import math
import os
import tempfile
import re
import subprocess
import argparse
import timeit
import sys
import time
import serial.tools.list_ports
import threading
import random
import struct
import queue
import pm3_trace
import chameleon_ndef as ndef
from enum import Enum
from multiprocessing import Pool, cpu_count
from typing import ClassVar, Union
from pathlib import Path
from platform import uname
from datetime import datetime
import hardnested_utils
from fdxb_country import describe_country_code

from chameleon_dfc import DfcCredential, DfcError
import chameleon_pm3
import chameleon_com
import chameleon_cmd
import chameleon_dfu
from chameleon_utils import (
    ArgumentParserNoExit,
    ArgsParserError,
    UnexpectedResponseError,
    execute_tool,
    tqdm_if_exists,
    print_key_table,
    odd_parity_byte,
    default_cwd
)

from chameleon_utils import CLITree
from chameleon_utils import CR, CG, CB, CC, CY, C0, color_string
from chameleon_utils import print_mem_dump
from chameleon_enum import Command, Status, SlotNumber, TagSenseType, TagSpecificType
from chameleon_enum import (
    MifareClassicWriteMode,
    MifareClassicPrngType,
    MifareClassicDarksideStatus,
    MfcKeyType,
)
from chameleon_enum import MifareUltralightWriteMode
from chameleon_enum import (
    AnimationMode,
    ButtonPressFunction,
    ButtonType,
    MfcValueBlockOperator,
)
from chameleon_enum import HIDFormat
from chameleon_enum import StandaloneMode, StandaloneState, StandaloneFlag
from crypto1 import Crypto1

# NXP IDs based on https://www.nxp.com/docs/en/application-note/AN10833.pdf
type_id_SAK_dict = {
    0x00: "MIFARE Ultralight Classic/C/EV1/Nano | NTAG 2xx",
    0x08: "MIFARE Classic 1K | Plus SE 1K | Plug S 2K | Plus X 2K",
    0x09: "MIFARE Mini 0.3k",
    0x10: "MIFARE Plus 2K",
    0x11: "MIFARE Plus 4K",
    0x18: "MIFARE Classic 4K | Plus S 4K | Plus X 4K",
    0x19: "MIFARE Classic 2K",
    0x20: "MIFARE Plus EV1/EV2 | DESFire EV1/EV2/EV3 | DESFire Light | NTAG 4xx | "
    "MIFARE Plus S 2/4K | MIFARE Plus X 2/4K | MIFARE Plus SE 1K",
    0x28: "SmartMX with MIFARE Classic 1K",
    0x38: "SmartMX with MIFARE Classic 4K",
}


def load_key_file(import_key, keys):
    """
    Load binary key file and append its content to the provided set of keys.
    Each key is 6 bytes concatenated.
    """
    with open(import_key.name, "rb") as file:
        data = file.read()
    for i in range(0, len(data), 6):
        key = data[i:i+6]
        if len(key) == 6:
            keys.add(key)
    return keys


def load_dic_file(import_dic, keys):
    """
    Load dictionary file and append its content to the provided set of keys.
    Each key is a 12-char hex string on a new line.
    """
    with open(import_dic.name, "r") as file:
        for line in file:
            line = line.strip()
            if line:
                keys.add(bytes.fromhex(line))
    return keys


def check_tools():
    missing_tools = []

    for tool in (
        "staticnested",
        "nested",
        "darkside",
        "mfkey32v2",
        "mfkey64",
        "staticnested_1nt",
        "staticnested_2x1nt_rf08s",
        "staticnested_2x1nt_rf08s_1key",
    ):
        if any(default_cwd.glob(f"{tool}*")):
            continue
        else:
            missing_tools.append(tool)

    if missing_tools:
        missing_tool_str = ", ".join(missing_tools)
        warn_str = f"Note: optional Mifare tools not found: {missing_tool_str}. Mifare attack commands will not work."
        print(color_string((CY, warn_str)))


class BaseCLIUnit:
    def __init__(self):
        # new a device command transfer and receiver instance(Send cmd and receive response)
        self._device_com: Union[chameleon_com.ChameleonCom, None] = None
        self._device_cmd: Union[chameleon_cmd.ChameleonCMD, None] = None

    @property
    def device_com(self) -> chameleon_com.ChameleonCom:
        assert self._device_com is not None
        return self._device_com

    @device_com.setter
    def device_com(self, com):
        self._device_com = com
        self._device_cmd = chameleon_cmd.ChameleonCMD(self._device_com)

    @property
    def cmd(self) -> chameleon_cmd.ChameleonCMD:
        assert self._device_cmd is not None
        return self._device_cmd

    def args_parser(self) -> ArgumentParserNoExit:
        """
            CMD unit args.

        :return:
        """
        raise NotImplementedError("Please implement this")

    def before_exec(self, args: argparse.Namespace):
        """
            Call a function before exec cmd.

        :return: function references
        """
        return True

    def on_exec(self, args: argparse.Namespace):
        """
            Call a function on cmd match.

        :return: function references
        """
        raise NotImplementedError("Please implement this")

    def after_exec(self, args: argparse.Namespace):
        """
            Call a function after exec cmd.

        :return: function references
        """
        return True

    @staticmethod
    def sub_process(cmd, cwd=default_cwd):
        class ShadowProcess:
            def __init__(self):
                self.output = ""
                self.time_start = timeit.default_timer()
                self._process = subprocess.Popen(
                    cmd,
                    cwd=cwd,
                    shell=True,
                    stderr=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                )
                threading.Thread(target=self.thread_read_output).start()

            def thread_read_output(self):
                while self._process.poll() is None:
                    assert self._process.stdout is not None
                    data = self._process.stdout.read(1024)
                    if len(data) > 0:
                        self.output += data.decode(encoding="utf-8")

            def get_time_distance(self, ms=True):
                if ms:
                    return round((timeit.default_timer() - self.time_start) * 1000, 2)
                else:
                    return round(timeit.default_timer() - self.time_start, 2)

            def is_running(self):
                return self._process.poll() is None

            def is_timeout(self, timeout_ms):
                time_distance = self.get_time_distance()
                if time_distance > timeout_ms:
                    return True
                return False

            def get_output_sync(self):
                return self.output

            def get_ret_code(self):
                return self._process.poll()

            def stop_process(self):
                # noinspection PyBroadException
                try:
                    self._process.kill()
                except Exception:
                    pass

            def get_process(self):
                return self._process

            def wait_process(self):
                return self._process.wait()

        return ShadowProcess()


class DeviceRequiredUnit(BaseCLIUnit):
    """
    Make sure of device online
    """

    def before_exec(self, args: argparse.Namespace):
        ret = self.device_com.isOpen()
        if ret:
            return True
        else:
            print("Please connect to chameleon device first (use 'hw connect').")
            return False


class ReaderRequiredUnit(DeviceRequiredUnit):
    """
    Make sure of device enter to reader mode.
    """

    def before_exec(self, args: argparse.Namespace):
        if not super().before_exec(args):
            return False

        if self.cmd.is_device_reader_mode():
            return True

        self.cmd.set_device_reader_mode(True)
        print("Switch to {  Tag Reader  } mode successfully.")
        return True


class SlotIndexArgsUnit(DeviceRequiredUnit):
    @staticmethod
    def add_slot_args(parser: ArgumentParserNoExit, mandatory=False):
        slot_choices = [x.value for x in SlotNumber]
        help_str = f"Slot Index: {slot_choices} Default: active slot"

        parser.add_argument(
            "-s",
            "--slot",
            type=int,
            required=mandatory,
            help=help_str,
            metavar="<1-8>",
            choices=slot_choices,
        )
        return parser


class SlotIndexArgsAndGoUnit(SlotIndexArgsUnit):
    def before_exec(self, args: argparse.Namespace):
        if super().before_exec(args):
            self.prev_slot_num = SlotNumber.from_fw(self.cmd.get_active_slot())
            if args.slot is not None:
                self.slot_num = args.slot
                if self.slot_num != self.prev_slot_num:
                    self.cmd.set_active_slot(self.slot_num)
            else:
                self.slot_num = self.prev_slot_num
            return True
        return False

    def after_exec(self, args: argparse.Namespace):
        if self.prev_slot_num != self.slot_num:
            self.cmd.set_active_slot(self.prev_slot_num)


class SenseTypeArgsUnit(DeviceRequiredUnit):
    @staticmethod
    def add_sense_type_args(parser: ArgumentParserNoExit):
        sense_group = parser.add_mutually_exclusive_group(required=True)
        sense_group.add_argument("--hf", action="store_true", help="HF type")
        sense_group.add_argument("--lf", action="store_true", help="LF type")
        return parser


class MF1AuthArgsUnit(ReaderRequiredUnit):
    def args_parser(self) -> ArgumentParserNoExit:
        parser = ArgumentParserNoExit()
        parser.add_argument(
            "--blk",
            "--block",
            type=int,
            required=True,
            metavar="<dec>",
            help="The block where the key of the card is known",
        )
        type_group = parser.add_mutually_exclusive_group()
        type_group.add_argument(
            "-a", "-A", action="store_true", help="Known key is A key (default)"
        )
        type_group.add_argument(
            "-b", "-B", action="store_true", help="Known key is B key"
        )
        parser.add_argument(
            "-k",
            "--key",
            type=str,
            required=True,
            metavar="<hex>",
            help="tag sector key",
        )
        return parser

    def get_param(self, args):
        class Param:
            def __init__(self):
                self.block = args.blk
                self.type = MfcKeyType.B if args.b else MfcKeyType.A
                key: str = args.key
                if not re.match(r"^[a-fA-F0-9]{12}$", key):
                    raise ArgsParserError("key must include 12 HEX symbols")
                self.key: bytearray = bytearray.fromhex(key)

        return Param()


class HF14AAntiCollArgsUnit(DeviceRequiredUnit):
    @staticmethod
    def add_hf14a_anticoll_args(parser: ArgumentParserNoExit):
        parser.add_argument("--uid", type=str, metavar="<hex>", help="Unique ID")
        parser.add_argument(
            "--atqa", type=str, metavar="<hex>", help="Answer To Request"
        )
        parser.add_argument(
            "--sak", type=str, metavar="<hex>", help="Select AcKnowledge"
        )
        ats_group = parser.add_mutually_exclusive_group()
        ats_group.add_argument(
            "--ats", type=str, metavar="<hex>", help="Answer To Select"
        )
        ats_group.add_argument(
            "--delete-ats", action="store_true", help="Delete Answer To Select"
        )
        return parser

    def update_hf14a_anticoll(self, args, uid, atqa, sak, ats):
        anti_coll_data_changed = False
        change_requested = False
        if args.uid is not None:
            change_requested = True
            uid_str: str = args.uid.strip()
            if re.match(r"[a-fA-F0-9]+", uid_str) is not None:
                new_uid = bytes.fromhex(uid_str)
                if len(new_uid) not in [4, 7, 10]:
                    raise Exception("UID length error")
            else:
                raise Exception("UID must be hex")
            if new_uid != uid:
                uid = new_uid
                anti_coll_data_changed = True
            else:
                print(color_string((CY, "Requested UID already set")))
        if args.atqa is not None:
            change_requested = True
            atqa_str: str = args.atqa.strip()
            if re.match(r"[a-fA-F0-9]{4}", atqa_str) is not None:
                new_atqa = bytes.fromhex(atqa_str)
            else:
                raise Exception("ATQA must be 4-byte hex")
            if new_atqa != atqa:
                atqa = new_atqa
                anti_coll_data_changed = True
            else:
                print(color_string((CY, "Requested ATQA already set")))
        if args.sak is not None:
            change_requested = True
            sak_str: str = args.sak.strip()
            if re.match(r"[a-fA-F0-9]{2}", sak_str) is not None:
                new_sak = bytes.fromhex(sak_str)
            else:
                raise Exception("SAK must be 2-byte hex")
            if new_sak != sak:
                sak = new_sak
                anti_coll_data_changed = True
            else:
                print(color_string((CY, "Requested SAK already set")))
        if (args.ats is not None) or args.delete_ats:
            change_requested = True
            if args.delete_ats:
                new_ats = b""
            else:
                ats_str: str = args.ats.strip()
                if re.match(r"[a-fA-F0-9]+", ats_str) is not None:
                    new_ats = bytes.fromhex(ats_str)
                else:
                    raise Exception("ATS must be hex")
            if new_ats != ats:
                ats = new_ats
                anti_coll_data_changed = True
            else:
                print(color_string((CY, "Requested ATS already set")))
        if anti_coll_data_changed:
            self.cmd.hf14a_set_anti_coll_data(uid, atqa, sak, ats)
        return change_requested, anti_coll_data_changed, uid, atqa, sak, ats


class MFUAuthArgsUnit(ReaderRequiredUnit):
    def args_parser(self) -> ArgumentParserNoExit:
        parser = ArgumentParserNoExit()

        def key_parser(key: str) -> bytes:
            try:
                key = bytes.fromhex(key)
            except ValueError:
                raise ValueError("Key should be a hex string")

            if len(key) not in [4, 16]:
                raise ValueError("Key should either be 4 or 16 bytes long")
            elif len(key) == 16:
                raise ValueError("Ultralight-C authentication isn't supported yet")

            return key

        parser.add_argument(
            "-k",
            "--key",
            type=key_parser,
            metavar="<hex>",
            help="Authentication key (EV1/NTAG 4 bytes).",
        )
        parser.add_argument(
            "-l",
            action="store_true",
            dest="swap_endian",
            help="Swap endianness of the key.",
        )

        return parser

    def get_param(self, args):
        key = args.key

        if key is not None and args.swap_endian:
            key = bytearray(key)
            for i in range(len(key)):
                key[i] = key[len(key) - 1 - i]
            key = bytes(key)

        class Param:
            def __init__(self, key):
                self.key = key

        return Param(key)

    def on_exec(self, args: argparse.Namespace):
        raise NotImplementedError("Please implement this")
















IDTECK_PREAMBLE_HEX = "4944544B"
IDTECK_PREAMBLE_INT = 0x4944544B


def _idteck_compute_checksum(payload_lo3: int) -> int:
    """Compute the IDTECK checksum byte from the low 3 bytes of the 4-byte payload.

    Matches the formula used by Proxmark3 (cmdlfidteck.c): the checksum is the
    sum of the three non-checksum payload bytes, taken modulo 256.
    """
    return ((payload_lo3 >> 16) + (payload_lo3 >> 8) + payload_lo3) & 0xFF


def _idteck_compose_frame(card_id: int) -> bytes:
    """Compose an 8-byte IDTECK frame from a 24-bit card ID.

    The frame is preamble + [checksum][card_id bytes reversed] where the
    reversal and checksum placement match the layout observed on real IDTECK
    cards (see cmdlfidteck.c in the Proxmark3 client). This helper is exposed
    for future CLI use (e.g. `lf idteck compose --cn`); it is not wired into
    any command yet.
    """
    card_id &= 0xFFFFFF
    # The card ID is stored with bytes reversed in the payload; mirror PM3.
    reversed_id = ((card_id & 0xFF) << 16) | ((card_id >> 8) & 0xFF) << 8 | ((card_id >> 16) & 0xFF)
    chksum = _idteck_compute_checksum(reversed_id)
    payload = (chksum << 24) | reversed_id
    return bytes.fromhex(IDTECK_PREAMBLE_HEX) + payload.to_bytes(4, "big")


def _idteck_frame_info(frame: bytes) -> dict:
    """Parse an 8-byte IDTECK frame into its components.

    Returns a dict with: preamble_hex, preamble_valid, payload_hex, checksum,
    checksum_expected, checksum_valid, card_id (24-bit extracted from payload
    bytes 1-3 with the byte-reversal convention used by PM3).
    """
    if len(frame) != 8:
        raise ValueError("IDTECK frame must be exactly 8 bytes")
    preamble = int.from_bytes(frame[:4], "big")
    payload = int.from_bytes(frame[4:], "big")
    chksum = (payload >> 24) & 0xFF
    lo3 = payload & 0xFFFFFF
    expected = _idteck_compute_checksum(lo3)
    card_id = ((lo3 >> 16) & 0xFF) | ((lo3 >> 8) & 0xFF) << 8 | (lo3 & 0xFF) << 16
    return {
        "preamble_hex": f"{preamble:08X}",
        "preamble_valid": preamble == IDTECK_PREAMBLE_INT,
        "payload_hex": f"{payload:08X}",
        "checksum": chksum,
        "checksum_expected": expected,
        "checksum_valid": chksum == expected,
        "card_id": card_id,
    }




def _fdxb_crc16(data: bytes) -> int:
    """CRC-16 as computed by the firmware's fdxb_crc16 (reflected 0x8408)."""
    crc = 0x0000
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def _fdxb_frame_ok(frame: bytes) -> "tuple[bool, str]":
    """
    Validate a 13-byte destuffed FDX-B frame for writability.

    Returns (True, "") if structurally sound, else (False, reason).

    The 64-bit block in bytes 0-7 is laid out (LSB first):
        bits  0-37  national ID
        bits 38-47  country code
        bit  48     extended-data flag (a.k.a. data-block / application bit)
        bits 49-62  reserved -- MUST be zero on a conformant tag
        bit  63     animal flag

    A frame with nonzero reserved bits is emitted fine by the T55xx but
    rejected as malformed by conformant readers, so the written tag reads
    back as "not found".  That is the hard failure we block here.
    """
    if len(frame) != 13:
        return False, f"expected 13 bytes, got {len(frame)}"

    v = int.from_bytes(frame[0:8], "little")
    reserved = (v >> 49) & ((1 << 14) - 1)
    if reserved != 0:
        return False, (f"reserved bits 49-62 are nonzero (0x{reserved:04x}); "
                       f"a conformant reader will reject this frame as malformed "
                       f"and the written tag will read back as not found")
    return True, ""


def _fdxb_crc16(data: bytes) -> int:
    """CRC-16 as computed by the firmware's fdxb_crc16 (reflected 0x8408)."""
    crc = 0x0000
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def _fdxb_crc_ok(frame: bytes) -> bool:
    """True if bytes 8-9 match the CRC-16 over bytes 0-7."""
    stored = int.from_bytes(frame[8:10], "little")
    return stored == _fdxb_crc16(frame[0:8])


def _fdxb_build_frame(country: int, national: int, animal: int = 1,
                      extended: int = 0) -> bytes:
    """
    Build a 13-byte destuffed FDX-B frame from logical fields.

    Reserved bits 49-62 are left zero by construction, so a frame built this
    way can never hit the "reserved bits nonzero -> unreadable" footgun.

        bits  0-37  national ID     (<= 274877906943)
        bits 38-47  country code    (<= 1023)
        bit  48     extended flag   (set iff extended != 0)
        bits 49-62  reserved        (always 0 here)
        bit  63     animal flag
        bytes 8-9   CRC-16 over bytes 0-7
        bytes 10-12 extended data   (24 bits, 0 if unused)
    """
    if not 0 <= country <= 0x3FF:
        raise ArgsParserError("country must be 0-1023")
    if not 0 <= national <= ((1 << 38) - 1):
        raise ArgsParserError("national ID must be 0-274877906943 (38-bit field)")
    if not 0 <= extended <= 0xFFFFFF:
        raise ArgsParserError("extended data must be 0-16777215 (24-bit field)")

    v = national & ((1 << 38) - 1)
    v |= (country & 0x3FF) << 38
    v |= (1 if extended else 0) << 48
    v |= (animal & 1) << 63

    head = v.to_bytes(8, "little")
    crc = _fdxb_crc16(head).to_bytes(2, "little")
    ext = (extended & 0xFFFFFF).to_bytes(3, "little")
    return head + crc + ext




class TagTypeArgsUnit(DeviceRequiredUnit):
    @staticmethod
    def add_type_args(parser: ArgumentParserNoExit):
        type_names = [t.name for t in TagSpecificType.list()]
        help_str = "Tag Type: " + ", ".join(type_names)
        parser.add_argument(
            "-t",
            "--type",
            type=str,
            required=True,
            metavar="TAG_TYPE",
            help=help_str,
            choices=type_names,
        )
        return parser

    def args_parser(self) -> ArgumentParserNoExit:
        raise NotImplementedError()

    def on_exec(self, args: argparse.Namespace):
        raise NotImplementedError()


root = CLITree(root=True)
hw = root.subgroup("hw", "Hardware-related commands")
hw_slot = hw.subgroup("slot", "Emulation slots commands")
hw_settings = hw.subgroup("settings", "Chameleon settings commands")

hf = root.subgroup("hf", "High Frequency commands")
hf_14a = hf.subgroup("14a", "ISO14443-a commands")
hf_mf = hf.subgroup("mf", "MIFARE Classic commands")
hf_mfu = hf.subgroup("mfu", "MIFARE Ultralight / NTAG commands")
hf_des = hf.subgroup("des", "MIFARE DESFire commands")
hf_seos = hf.subgroup("seos", "SEOS commands")

lf = root.subgroup("lf", "Low Frequency commands")
lf_em = lf.subgroup("em", "EM commands")
lf_em_4x05 = lf_em.subgroup("4x05", "EM4x05/EM4x69 commands")
lf_indala = lf.subgroup("indala", "Indala commands")
data = root.subgroup('data', 'Data analysis and visualization commands')
emv = root.subgroup('emv', 'EMV contactless payment card commands')
standalone = root.subgroup('standalone', 'Host-less standalone modes')


lf_em_410x = lf_em.subgroup("410x", "EM410x commands")
lf_hid = lf.subgroup("hid", "HID commands")
lf_hid_prox = lf_hid.subgroup("prox", "HID Prox commands")
lf_ioprox = lf.subgroup("ioprox", "ioProx commands")
lf_pac = lf.subgroup("pac", "PAC/Stanley commands")
lf_viking = lf.subgroup("viking", "Viking commands")
lf_jablotron = lf.subgroup("jablotron", "Jablotron commands")
lf_fdxb = lf.subgroup("fdxb", "FDX-B animal tag commands (134.2 kHz)")
lf_generic = lf.subgroup("generic", "Generic commands")
lf_idteck = lf.subgroup("idteck", "IDTECK commands")
lf_t55xx = lf.subgroup("t55xx", "T55xx/T5577 raw block commands")


