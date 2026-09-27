# Standalone Modes

Standalone modes let the ChameleonUltra work **without a host** — no laptop, no
app, just the device and its two buttons. You configure a mode over the CLI
once, **arm** it, then walk away. The device runs the mode on button presses,
saves results to flash, and you pull them back at the bench later.

Every built-in mode is **safe by default**: none write to a target card or to
your emulation slots. A mode that would write anything must declare it, and the
framework refuses to arm such a mode unless you pass `--opt-in`.

## How every mode works (the pattern)

1. **Select** the mode: `standalone set-mode <name>`
2. **Configure** it (options are per-mode): `standalone config <name> [flags]`
3. **Arm** it: `standalone trigger` (from the CLI), or press **both buttons,
   long** on the device.
4. **Walk away.** Press **both buttons, short** to run one capture/action.
5. **Disarm** to save results: `standalone disarm`, or **both buttons, long**.
6. **Retrieve**: `standalone get-result` (over USB/BLE).

`standalone status` shows the current mode, armed state, and stored-result
count. `standalone ls` lists the modes. `standalone clear-result` discards
stored results.

## Buttons while armed (chord = press both together)

| Chord | Meaning |
|-------|---------|
| **Both, short** | Run the mode's action (one capture / advance slot). |
| **Both, long** | Arm ⇄ disarm. |
| **Both, very-long** | Discard all stored results for this mode. |

## LED feedback (all modes)

The slot LEDs signal state and confirm each action landed — watch them before
you walk away:

| LED pattern | Meaning |
|-------------|---------|
| Mode-colour **sweep → solid** | Armed. |
| Reverse sweep → **all off** | Disarmed. |
| **Green triple-flash** | Action succeeded (a capture landed). |
| **Red triple-flash** | Action failed. |
| **Red double-flash** | Refused — mode needs `--opt-in`. |
| Brief mode-colour **wave** | A long operation started / finished. |

---

## `authtrace` — reader-side auth capture  *(Ultra only)*

CU acts as a **reader**. Each press runs one full HF14A authentication against a
real card and records every frame (REQA…ATS, auth, `nt`, `nr‖ar`, `at`).

```
standalone set-mode authtrace
standalone config authtrace --key-type A --block 4 --key FFFFFFFFFFFF --timeout 1000
standalone trigger                 # arm  (or: both-buttons long on device)
#  → walk to the card, press both-short per auth attempt (green flash = captured)
standalone disarm                  # or both-buttons long — saves results
standalone get-result              # pull the traces
```

**Config** (16-byte blob; the CLI sets it via the flags): `--key-type A|B`,
`--block <0-255>` (e.g. `4` = first non-MAD sector key A), `--key <12 hex>`
(candidate sector key), `--timeout <100-30000>` (per-session tag-poll, ms).
**Buttons:** BOTH_SHORT = run one auth session · BOTH_LONG = arm/disarm ·
BOTH_VLONG = discard all sessions.
No `--opt-in` needed (never writes target memory or slots).

## `emultrace` — card-side auth capture  *(Lite + Ultra)*

CU emulates a **card** and captures the exchange when a real reader
authenticates to it. A new REQA (7-bit `0x26`) arriving mid-capture marks a
session boundary; everything before it commits as one session. Only sessions
with enough trace data commit (isolated/back-to-back REQAs are filtered), and an
idle gap auto-commits the current one. Same wire format as `authtrace`; feeds
mfkey32v2.

```
standalone set-mode emul-trace
standalone trigger                 # arm — then present CU to the reader
#  captures happen automatically as the reader authenticates
standalone disarm
standalone get-result
```

**Config:** none. **Buttons:** BOTH_LONG = arm/disarm. Sessions commit
automatically (on the next REQA or after the idle timeout) — no short-press
capture. **Works on Lite and Ultra** (emulation only).

## `hf14a-tap-sniff` — passive tap  *(Ultra only)*

CU stays **silent** and listens: NFCT captures the reader→card downlink, RC522
captures the card→reader uplink from the shared coil. Never interferes with the
transaction.

```
standalone set-mode hf14a-tap-sniff
standalone config hf14a-tap-sniff --timeout 2000
standalone trigger                 # arm
#  → position CU by the live reader/card, press both-short to capture a session
standalone disarm
standalone get-result
```

**Config** (8-byte blob): `--timeout <100-30000>` (per-capture listen duration,
ms). **Buttons:** BOTH_SHORT = capture one session · BOTH_LONG = arm/disarm ·
BOTH_VLONG = discard all sessions. No `--opt-in` needed.

## `relay` — two-device BLE relay  *(Ultra only, needs two CUs)*

A transparent Bluetooth relay between **two** ChameleonUltras. Roles are
assigned automatically by BLE MAC: the **lower MAC becomes CARD** (NFCT, faces
the reader), the **higher MAC becomes READER** (RC522, faces the card).

**Setup — do this on BOTH devices:**
```
standalone set-mode relay --opt-in    # relay requires --opt-in
standalone config relay --timeout 3000   # --timeout is reused as the WTX window, ms (500-10000)
standalone trigger                    # arm both; they pair over BLE automatically
```
On arm, each device starts BLE (`ble_relay_start`) and the two pair
automatically. On connect, each device flashes green (success) and then goes
**solid in its role colour**:

| LED (solid) | Role | Faces | Uses |
|-------------|------|-------|------|
| **Blue** | CARD | the real **reader** | NFCT emulation |
| **Green** | READER | the real **card** | RC522 |

So: put the **blue** device on the reader, the **green** device on the card. If
the LEDs aren't blue/green solid yet, the two haven't paired — keep them in BLE
range.

Then present the devices; frames relay automatically and are recorded.
```
standalone disarm                     # saves the session trace
standalone get-result
```
**Buttons:** BOTH_LONG = arm/disarm · **BOTH_VLONG = discard results and
restart pairing** (stops BLE, clears the trace, re-links from scratch — use it if
roles didn't assign or a device dropped). Relay has no short-press action; frames
relay automatically once paired.
**Config flag:** `--timeout <500-10000>` — reused as the WTX
(waiting-time-extension) window in ms, tuned for BLE round-trip latency. Relay
ignores `--block/--key-type/--key`.

## `slot-cycle` — rotate emulation slots  *(Lite + Ultra)*

Rotates the active emulation slot through a chosen set at a fixed interval. No
RF interaction, no writes — the safest mode.

```
standalone set-mode slot-cycle
standalone trigger                 # arm — rotation begins (uses firmware defaults)
```
Defaults are sensible (all slots, start at 0, 3000 ms dwell), so you usually
don't configure it. To override, pass the raw 6-byte config blob with `-d`
(the CLI has no named flags for slot-cycle):
```
#  bytes: version(01) slot_mask start_slot reserved(00) interval_ms(LE)
#  e.g. slots 0-3, start 0, 2000 ms = 01 0F 00 00 D0 07
standalone config slot-cycle -d 010F0000D007
```
Fields (6-byte blob): `version=01`, `slot_mask` (bit N = include slot N),
`start_slot` (must be set in the mask), `reserved=00`, `interval_ms` little-endian
(100-60000, default 3000). **Buttons:** BOTH_SHORT = advance now (resumes if
paused) · BOTH_LONG = arm/disarm · BOTH_VLONG = pause/resume.
---

## `--opt-in` and quiet flags

`set-mode` takes optional flags:
- `--opt-in` — sets `HOST_OPTED_IN`. **Required** for modes that act on real
  targets (e.g. `relay`); arming without it gives a **red double-flash** and a
  "requires --opt-in" refusal.
- `--quiet-buzzer` / `--quiet-led` — silence the buzzer / LEDs for covert use.

## CLI reference (`standalone` group)

| Command | Purpose |
|---------|---------|
| `ls` | List available modes. |
| `status` | Current mode, armed state, stored-result count. |
| `set-mode <name> [--opt-in] [--quiet-buzzer] [--quiet-led]` | Select the active mode. |
| `config <name> [--block --key-type --key --timeout \| -d <hex>]` | Write the mode's config. `authtrace` uses `--block/--key-type/--key/--timeout`; `hf14a-tap-sniff` uses `--timeout`; `relay` reuses `--timeout` as WTX; `slot-cycle` uses raw `-d <hex>`. |
| `trigger` | Arm the active mode from the host. |
| `disarm` | Disarm and save results. |
| `get-result` | Retrieve stored session results. |
| `clear-result` | Discard stored results. |

CLI vs firmware names: the CLI uses hyphens (`emul-trace`, `hf14a-tap-sniff`,
`slot-cycle`); `status` reports the firmware's underscored names (`emul_trace`,
`hf14a_tap_sniff`, `slot_cycle`). They refer to the same modes.

## Result format

`authtrace`, `emultrace`, and `hf14a-tap-sniff` write the **same** record layout,
so host tooling parses them identically:

```
u8  session_num     0-based session index
u8  status          STATUS_HF_* result code
u16 trace_len       length of the trace bytes
u8  trace[trace_len]   verbatim wire trace (same format as CMD 2017)
```

Traces are Proxmark3-decoder-compatible — feed them to mfkey32v2 / mfkey64.
`relay` wraps the same trace bytes with per-session role, UID, ATQA/SAK, and
frame count.

## Writing your own mode

`mode_template.c` is a fully commented reference for the
`standalone_mode_iface_t` contract; `slot_cycle` is the simplest real example.
Key rules: a mode that writes tag memory must set `writes_tag`, one that writes
slots must set `writes_slot` (the framework enforces the opt-in), and
`on_button`/`on_enter`/`on_exit` may block only briefly. See
`CONTRIBUTING STANDALONE.md` and `TESTING.md` here for the full contract.
