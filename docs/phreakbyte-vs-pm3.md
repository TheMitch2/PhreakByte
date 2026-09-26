# Phreakbyte ChameleonUltra Fork vs Proxmark3 (Iceman)

An honest capability comparison. The two devices are built on fundamentally
different RF architectures, so "parity" is stated per task as **Full**,
**Partial**, or **N/A** with the reason b
ash: syntax error: unexpected "("
Phreakbyte fork matches Proxmark3 it is on **commands, workflows, and file
formats**[span_1](start_span)[span_1](end_span); where it does not, it is almost always the **RF layer**, which the
ChameleonUltra hardware cannot reach[span_2](start_span)[span_2](end_span).

| Feature / Capability | Phreakbyte ChameleonUltra Fork | Proxmark3 (Iceman) | Parity |
|---|---|---|---|
| **File interoperability** | Reads/writes Proxmark3 `mfc v2` and `mfdes v1` JSON both directions, plus `.eml`, `.dic`, `.key`, `.dfc`, `.dfcb`, and PM3 `.trace`[span_3](start_span)[span_3](end_span). | Native, and a wider set (EMV JSON, `.mct`, iCESERE, etc.)[span_4](start_span)[span_4](end_span). | **Full for MFC/DESFire dumps + keys.** PM3 supports more container types overall[span_5](start_span)[span_5](end_span). |
| **MIFARE Classic recovery** | Full attack chain: check-keys (`fchk`), darkside, nested, hardnested, static-nested (backdoor), and `autopwn` that chains them, propagates keys, resumes from a keyfile, and can dump to `mfc v2` JSON / load straight into a slot[span_6](start_span)[span_6](end_span). | Full recovery suite (darkside, nested, hardnested, staticnested, autopwn)[span_7](start_span)[span_7](end_span). | **Full.** Same algorithms, same workflow[span_8](start_span)[span_8](end_span). |
| **DESFire** | EV1/EV2 emulation with key/auth handling (DES/2TDEA/3TDEA/AES), key-version semantics, and PM3-compatible dump/keys[span_9](start_span)[span_9](end_span). Reader-side: read info, enumerate AIDs/files, check keys, auth-trace[span_10](start_span)[span_10](end_span). | Full read/enumerate/auth/key-dictionary, plus EV3 features (SDM/LRP, originality signatures)[span_11](start_span)[span_11](end_span). | **Partial.** No EV3 (SDM/LRP/originality) and no signature emulation b
ash: syntax error: unexpected "("
| **Other HF protocols** | SEOS (eload/keys), EMV APDU scan/relay, ISO14443-4 (T=CL) handling[span_13](start_span)[span_13](end_span). | Full, plus iCLASS/PICOPASS, FeliCa, Legic, Topaz, and more[span_14](start_span)[span_14](end_span). | **Partial.** CU covers the common auditing set; PM3 covers more HF families[span_15](start_span)[span_15](end_span). |
| **Low Frequency (LF)** | 125 kHz (EM410x, HID Prox, ioProx, PAC, Viking, Indala, Jablotron; IDTECK write/emulate) and 134.2 kHz FDX-B; T55xx read/write; some raw/`--adc` LF capture[span_16](start_span)[span_16](end_span). | Full 125/134.2 kHz decode, raw modulation synthesis, T55xx, and a larger LF protocol set[span_17](start_span)[span_17](end_span). | **Partial.** Common tags covered; PM3 decodes more and synthesizes arbitrary LF waveforms[span_18](start_span)[span_18](end_span). |
| **Raw RF capture & DSP** | Frame/bit-level HF capture via the MFRC522 reader IC and host-side decode; some LF raw/`--adc` sampling[span_19](start_span)[span_19](end_span). **No** HF raw-sample capture, antenna/`hf tune` sampling, or arbitrary waveform synthesis[span_20](start_span)[span_20](end_span). | FPGA + ADC: raw sample-level capture, sample-level sniffing, arbitrary modulation synthesis, antenna tuning[span_21](start_span)[span_21](end_span). | **N/A (hardware).** The Ultra's HF path is a MFRC522 reader IC (framing/decoded frames) plus the nRF52840 NFC peripheral for emulation b
ash: syntax error: unexpected "("
| **Sniffing & relaying** | HF sniffing (passive tap and active) via the MFRC522, exporting PM3 `.trace`; LF sniff; live ISO14443-4 T=CL / EMV APDU relay (`emv apdu`) and a standalone `relay` mode with WTX handling[span_24](start_span)[span_24](end_span). | Sample-level FPGA sniffing and intera
