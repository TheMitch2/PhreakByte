# Phreakbyte ChameleonUltra Fork vs Proxmark3 (Iceman)

The two devices overlap heavily on RFID auditing workflows but sit on
different RF architectures, so "parity" is stated per task as **Full**,
**Partial**, or **N/A** with the reason — not as a percentage. Where the
Phreakbyte fork matches Proxmark3 it is on **commands, workflows, and file
formats**, not on the raw-RF layer (see Bottom line).

| Feature / Capability | Phreakbyte ChameleonUltra Fork | Proxmark3 (Iceman) | Parity |
|---|---|---|---|
| **File interoperability** | Reads/writes Proxmark3 `mfc v2` and `mfdes v1` JSON both directions, plus `.eml`, `.dic`, `.key`, `.dfc`, `.dfcb`, and PM3 `.trace`. | Native, and a wider set (EMV JSON, `.mct`, iCESERE, etc.). | **Full for MFC/DESFire dumps + keys.** PM3 supports more container types overall. |
| **Card identification (`hf 14a info`)** | Deep scan prints UID/ATQA/SAK/ATS, guesses type from SAK, detects MIFARE Classic + PRNG, probes for Gen1A/Gen2/Gen3 magic backdoors, and prints a single "what to run next" recommendation (autopwn, `hf mf cview`/`cload`, `gen3uid`, `hf mfu dump`, `hf des chk`). Stays silent rather than guess when SAK/ATS are ambiguous. | `hf 14a info` identifies the tag and prints detected features/fingerprint. | **Full.** Comparable identify, plus guided next-step hints. |
| **MIFARE Classic recovery** | Full attack chain: check-keys (`fchk`), darkside, nested, hardnested, static-nested (backdoor), and `autopwn` that chains them, propagates keys, resumes from a keyfile, and can dump to `mfc v2` JSON / load straight into a slot. Plus emulation-side key capture: simulate a card, log the reader's auth attempts, recover keys with mfkey32/mfkey64. | Full recovery suite (darkside, nested, hardnested, staticnested, autopwn, sim+mfkey). | **Full.** Same algorithms, same workflow. |
| **MIFARE Classic magic cards** | Gen1A full backdoor suite matching PM3's `hf mf c*`: block read/write (`cgetblk`, `csetblk`), UID set (`csetuid`), dump (`cview`), and dump-to-card (`cload`), no keys needed; plus Gen1A/Gen2 emulation modes and clone. Gen3 UID/block-0 write and permanent UID lock (`gen3uid`, `gen3blk`, `gen3freeze`). Read-only `identify_magic_gen` probe for gen1a/gen2/gen3. | Gen1A/Gen2/Gen3/Gen4 (ultimate magic / GTU) read, write, and config. | **Full for Gen1A–Gen3.** Only Gen4 (ultimate magic / GTU) config remains. |
| **DESFire** | EV1/EV2 emulation with full key/auth handling (DES/2TDEA/3TDEA/AES), key-version semantics, v6 credential format incl. Authentication Commands, and PM3-compatible dump/keys. Reader-side: read info, enumerate AIDs/files, check keys, single-call ISO 7816 auth (`readerauth`), and per-frame `auth-trace`. | Full read/enumerate/auth/key-dictionary, plus EV3 features (SDM/LRP, originality signatures). | **Partial.** No EV3 (SDM/LRP/originality) and no signature emulation — gated on purpose, not zero-filled. |
| **MIFARE Ultralight / NTAG** | Full read/write (pages, counters, signature, version), NDEF read/write, dump/eload/esave, UL-C key gen, NFC import, and emulation-side PWD/PACK detection logging. | Full UL/NTAG suite plus tearoff/glitch attacks. | **Partial.** Protocol/workflow full; tearoff needs precise power glitching the CU hardware cannot do. |
| **Other HF protocols** | SEOS (eload/keys), EMV APDU scan/relay, generic ISO14443-4 (T=CL) raw/APDU passthrough (`hf 14a raw`). | Full, plus iCLASS/PICOPASS, FeliCa, Legic, Topaz, ISO15693, and more. | **Partial.** CU covers the common auditing set; PM3 covers more HF families. ISO15693 is N/A (hardware — see below). |
| **Low Frequency (LF)** | 125 kHz (EM410x, HID Prox, ioProx, PAC, Viking, Indala, Jablotron; IDTECK write/emulate only) and 134.2 kHz FDX-B; T55xx detect/read/write/wipe; some raw/`--adc` LF capture. | Full 125/134.2 kHz decode, raw modulation synthesis, T55xx, and a larger LF protocol set. | **Partial.** Common tags covered; PM3 decodes more and synthesizes arbitrary LF waveforms. |
| **Raw RF capture & DSP** | Frame/bit-level HF capture via the MFRC522 reader IC and host-side decode; some LF raw/`--adc` sampling. **No** HF raw-sample capture, antenna/`hf tune` sampling, or arbitrary waveform synthesis. | FPGA + ADC: raw sample-level capture, sample-level sniffing, arbitrary modulation synthesis, antenna tuning. | **N/A (hardware).** The Ultra's HF path is a MFRC522 reader IC (framing/decoded frames) plus the nRF52840 NFC peripheral for emulation — there is no ADC/FPGA to pull raw sample windows from. Sample-level RF work and ISO15693 (needs a different reader IC) are out of scope. |
| **Sniffing & relaying** | HF sniffing — passive tap (`hf 14a sniff --tap`) and active (`hf 14a sniff`) — via the MFRC522, exporting PM3 `.trace`; LF sniff; live ISO14443-4 T=CL / EMV APDU relay (`emv apdu`) and a standalone `relay` mode with WTX handling. Relays ISO14443-4 (DESFire/EMV); MIFARE Classic relay is not possible (no WTX at ISO14443-3 to absorb BLE latency). | Sample-level FPGA sniffing and interactive card/reader relay, incl. MIFARE Classic relay over a low-latency link. | **Partial.** The Ultra sniffs HF at the frame/protocol level through the MFRC522 (enough for most audits); PM3 sniffs at the raw-sample level. T=CL relay is comparable; Crypto1 relay is out of reach over BLE. |
| **Field portability & emulation** | 8 HF + 8 LF slots, host-less standalone modes (`authtrace`, `emul-trace`, `hf14a-tap-sniff`, `relay` with BLE card/reader roles, `slot-cycle`, `autoclone`, `dict_check`, `read-replay`), BLE 5.0, battery powered, pocketable. (Ultra/DevKit carry the MFRC522 for HF read/write/sniff; the Lite omits it and emulates only.) | Powerful but typically tethered; more limited standalone/emulation profile. | **CU advantage.** This is where the ChameleonUltra clearly wins. |

## Bottom line

For **everyday RFID auditing** — cracking and cloning MIFARE Classic,
emulating and key-handling DESFire EV1/EV2, common LF tags, protocol-level
sniffing and relaying, and moving dumps to/from a Proxmark3 — the Phreakbyte
fork gives you **command, workflow, and file-format parity** in a
battery-powered, multi-slot, standalone pocket device.

The real, unavoidable difference is the **RF front end**. Proxmark3 pairs an
FPGA with an ADC, so it can capture and synthesize raw RF at the sample level —
raw-sample sniffing, arbitrary waveform generation, antenna analysis, and the
long tail of exotic HF/LF protocols that depend on that (including ISO15693,
which needs a reader IC the Ultra does not have). The ChameleonUltra handles HF
two ways: the nRF52840's built-in NFC peripheral for card **emulation**, and a
dedicated **MFRC522** reader IC for HF **read, write, and sniffing** (passive
and active) — the MFRC522 is what the Ultra and DevKit have and the Lite does
not. LF uses a discrete analog front end. But the MFRC522 is a framing/reader IC
that yields decoded ISO14443-A frames, not raw subcarrier samples: there is no
ADC/FPGA, so PM3-style sample-level capture and arbitrary waveform synthesis
remain out of scope.

So the honest positioning isn't "98% of a Proxmark3." It's: **a Proxmark3-
interoperable pocket auditor** — matching PM3 on the protocol/workflow layer for
the common jobs, trading the FPGA's raw-RF ceiling for portability, multi-slot
emulation, and true standalone operation.

---
*Parity reflects the `main` branch command surface at the time of writing.
Regenerate the command reference with `gen_command_md.py` when it drifts.*
