# Phreakbyte ChameleonUltra Fork vs Proxmark3 (Iceman)

An honest capability comparison. The two devices are built on fundamentally
different RF architectures, so "parity" is stated per task as **Full**,
**Partial**, or **N/A** with the reason — not as a percentage. Where the
Phreakbyte fork matches Proxmark3 it is on **commands, workflows, and file
formats**; where it does not, it is almost always the **RF layer**, which the
ChameleonUltra hardware cannot reach.

| Feature / Capability | Phreakbyte ChameleonUltra Fork | Proxmark3 (Iceman) | Parity |
|---|---|---|---|
| **File interoperability** | Reads/writes Proxmark3 `mfc v2` and `mfdes v1` JSON both directions, plus `.eml`, `.dic`, `.key`, `.dfc`, `.dfcb`, and PM3 `.trace`. | Native, and a wider set (EMV JSON, `.mct`, iCESERE, etc.). | **Full for MFC/DESFire dumps + keys.** PM3 supports more container types overall. |
| **MIFARE Classic recovery** | Full attack chain: check-keys (`fchk`), darkside, nested, hardnested, static-nested (backdoor), and `autopwn` that chains them, propagates keys, resumes from a keyfile, and can dump to `mfc v2` JSON / load straight into a slot. | Full recovery suite (darkside, nested, hardnested, staticnested, autopwn). | **Full.** Same algorithms, same workflow. |
| **DESFire** | EV1/EV2 emulation with key/auth handling (DES/2TDEA/3TDEA/AES), key-version semantics, and PM3-compatible dump/keys. Reader-side: read info, enumerate AIDs/files, check keys, auth-trace. | Full read/enumerate/auth/key-dictionary, plus EV3 features (SDM/LRP, originality signatures). | **Partial.** No EV3 (SDM/LRP/originality) and no signature emulation — gated on purpose, not zero-filled. |
| **Other HF protocols** | SEOS (eload/keys), EMV APDU scan/relay, ISO14443-4 (T=CL) handling. | Full, plus iCLASS/PICOPASS, FeliCa, Legic, Topaz, and more. | **Partial.** CU covers the common auditing set; PM3 covers more HF families. |
| **Low Frequency (LF)** | 125 kHz (EM410x, HID Prox, ioProx, PAC, Viking, Indala, Jablotron; IDTECK write/emulate) and 134.2 kHz FDX-B; T55xx read/write; some raw/`--adc` LF capture. | Full 125/134.2 kHz decode, raw modulation synthesis, T55xx, and a larger LF protocol set. | **Partial.** Common tags covered; PM3 decodes more and synthesizes arbitrary LF waveforms. |
| **Raw RF capture & DSP** | Frame/bit-level HF capture via the MFRC522 reader IC and host-side decode; some LF raw/`--adc` sampling. **No** HF raw-sample capture, antenna/`hf tune` sampling, or arbitrary waveform synthesis. | FPGA + ADC: raw sample-level capture, sample-level sniffing, arbitrary modulation synthesis, antenna tuning. | **N/A (hardware).** The Ultra's HF path is a MFRC522 reader IC (framing/decoded frames) plus the nRF52840 NFC peripheral for emulation — there is no ADC/FPGA to pull raw sample windows from. Sample-level RF work is out of scope. |
| **Sniffing & relaying** | HF sniffing (passive tap and active) via the MFRC522, exporting PM3 `.trace`; LF sniff; live ISO14443-4 T=CL / EMV APDU relay (`emv apdu`) and a standalone `relay` mode with WTX handling. | Sample-level FPGA sniffing and interactive card/reader relay. | **Partial.** The Ultra sniffs HF at the frame/protocol level through the MFRC522 (enough for most audits); PM3 sniffs at the raw-sample level. Relay is comparable at the protocol level. |
| **Field portability & emulation** | 8 HF + 8 LF slots, host-less standalone modes (`authtrace`, `emul_trace`, `hf14a_tap_sniff`, `relay` with BLE card/reader roles, `slot_cycle`), BLE 5.0, battery powered, pocketable. (Ultra/DevKit carry the MFRC522 for HF read/write/sniff; the Lite omits it and emulates only.) | Powerful but typically tethered; more limited standalone/emulation profile. | **CU advantage.** This is where the ChameleonUltra clearly wins. |

## Bottom line

For **everyday RFID auditing** — cracking and cloning MIFARE Classic,
emulating and key-handling DESFire EV1/EV2, common LF tags, protocol-level
sniffing and relaying, and moving dumps to/from a Proxmark3 — the Phreakbyte
fork gives you **command, workflow, and file-format parity** in a
battery-powered, multi-slot, standalone pocket device.

The real, unavoidable difference is the **RF front end**. Proxmark3 pairs an
FPGA with an ADC, so it can capture and synthesize raw RF at the sample level —
raw-sample sniffing, arbitrary waveform generation, antenna analysis, and the
long tail of exotic HF/LF protocols that depend on that. The ChameleonUltra
handles HF two ways: the nRF52840's built-in NFC peripheral for card
**emulation**, and a dedicated **MFRC522** reader IC for HF **read, write, and
sniffing** (passive and active) — the MFRC522 is what the Ultra and DevKit have
and the Lite does not. LF uses a discrete analog front end. But the MFRC522 is a
framing/reader IC that yields decoded ISO14443-A frames, not raw subcarrier
samples: there is no ADC/FPGA, so PM3-style sample-level capture and arbitrary
waveform synthesis remain out of scope.

So the honest positioning isn't "98% of a Proxmark3." It's: **a Proxmark3-
interoperable pocket auditor** — matching PM3 on the protocol/workflow layer for
the common jobs, trading the FPGA's raw-RF ceiling for portability, multi-slot
emulation, and true standalone operation.
