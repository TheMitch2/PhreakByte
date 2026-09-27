# Codec ↔ engine cross-check

Proves the Python credential codec (`software/script/chameleon_dfc.py`) emits
`.dfcb` bytes the firmware DER engine accepts. The engine-only C suite cannot
see host encoder bugs (e.g. a v6 tag emitted out of DER order); this closes that
gap.

## Build the probe
```
cc -I ../../src -I ../../port -I ../../port/host \
   -DDFC_BUILD_PROFILE=DFC_PROFILE_FULL_EV2 \
   dfc_decode_probe.c \
   ../../src/dfc_der.c ../../src/dfc_credential.c ../../src/dfc_common.c \
   ../../src/dfc_command.c ../../src/dfc_ev2.c ../../src/dfc_profile.c \
   <crypto-backend-srcs> -o probe
```
(For decode-only testing the crypto functions are never called; link a stub
providing `dfc_crypto_*`, `dfc_ev2_*`, `dfc_random_fill`, `dfc_assert_fail`, or
just link the real backend.)

## Run
```
python3 test_codec_roundtrip.py ./probe /path/to/*.dfc
```
Each fixture the codec can model (EV1/EV2) is encoded and decoded by the engine;
`engine=Ok` is a pass. A `FAIL` means the codec produced bytes the device would
reject — the class of bug that shipped the v6 auth-command tag-order regression.
