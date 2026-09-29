#!/usr/bin/env python3
"""Cross-check: the Python credential codec must emit bytes the engine accepts.

For every *.dfc example the codec can model (EV1/EV2), encode it with
chameleon_dfc and decode the resulting .dfcb with the real engine (via the
dfc_decode_probe harness). A mismatch — codec says "fine", engine says
"malformed" — is exactly the tag-order class of bug the engine-only C suite
cannot see. Run:  python3 test_codec_roundtrip.py /path/to/probe /path/to/*.dfc
"""

import chameleon_dfc as D
import subprocess
import sys
import tempfile
import os
import glob

SCRIPT_DIR = os.environ.get(
    "DFC_SCRIPT_DIR",
    os.path.join(os.path.dirname(__file__), "../../../../../../../software/script"),
)
sys.path.insert(0, os.path.abspath(SCRIPT_DIR))


def probe(path_probe, blob):
    with tempfile.NamedTemporaryFile(suffix=".dfcb", delete=False) as t:
        t.write(blob)
        name = t.name
    try:
        r = subprocess.run([path_probe, name], capture_output=True, text=True)
        return r.returncode, r.stdout.strip()
    finally:
        os.unlink(name)


def main():
    if len(sys.argv) < 3:
        print("usage: test_codec_roundtrip.py <probe> <fixture.dfc> [more.dfc ...]")
        return 2
    path_probe, fixtures = sys.argv[1], sys.argv[2:]
    # expand globs
    files = []
    for f in fixtures:
        files += glob.glob(f)
    fails = 0
    for f in sorted(set(files)):
        text = open(f).read()
        try:
            cred = D.DfcCredential.parse_text(text)
        except D.DfcError as e:
            print(f"  skip  {os.path.basename(f)}: codec cannot model ({e})")
            continue
        blob = cred.to_wire()
        code, name = probe(path_probe, blob)
        ok = code == 0
        print(
            f"  {'PASS' if ok else 'FAIL'}  {os.path.basename(f)}: "
            f"codec->{len(blob)}B, engine={name}"
        )
        if not ok:
            fails += 1
    print(f"\n{'ALL PASS' if fails == 0 else str(fails) + ' FAILED'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
