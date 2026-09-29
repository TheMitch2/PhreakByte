# Recovering a bricked ChameleonUltra (stuck on red LED after revert-to-stock)

## Symptom

After a **revert-to-stock**, a battery-powered Ultra powers on with **only the
red LED** ,  no white/other LEDs, and it does **not** enumerate over USB. It looks
dead but the power light is on.

## Cause

On HV-mode boards the core runs off the nRF52840's `REGOUT0` regulator setting
(battery → VDDH → REG0 → VDD). If `REGOUT0` is left blank or set to **1.8 V**,
the core voltage is too low to boot ,  the chip powers on but can't run any
firmware. An older revert path could leave `REGOUT0` at 1.8 V, producing exactly
this state.

Firmware built after this fix forces `REGOUT0 = 3.3 V` during revert, so **new
reverts won't brick**. A unit already in this state can't run firmware to fix
itself, so it must be recovered over **SWD** (a debug probe: J-Link, or a
CMSIS-DAP such as a Pi Pico running picoprobe, or a Black Magic Probe).

## Fix over SWD

You only need to write the correct `REGOUT0` value and reset. `REGOUT0` lives in
the UICR at `0x10001304`; `0xFFFFFFFD` selects 3.3 V (VOUT field = 5, all other
bits left erased).

### With nRF Command Line Tools (nrfjprog)

```sh
# 1) Read it back first (optional ,  a bricked unit usually shows 0xFFFFFFFF or ...FFF7 = 1.8V)
nrfjprog -f NRF52 --memrd 0x10001304 --n 4

# 2) Erase the UICR, then write REGOUT0 = 3.3V.
#    UICR can only be changed after an erase; --eraseuicr does that.
nrfjprog -f NRF52 --eraseuicr
nrfjprog -f NRF52 --memwr 0x10001304 --val 0xFFFFFFFD

# 3) Reset ,  REGOUT0 only takes effect after a reset.
nrfjprog -f NRF52 --reset
```

The device should now boot to the (stock) bootloader over USB and be flashable
again.

> Note: `--eraseuicr` also clears the bootloader-address word (`0x10001014`). If,
> after this, the unit doesn't reach the bootloader, do a full recover and
> re-flash the firmware:
> ```sh
> nrfjprog -f NRF52 --recover
> # then flash a normal (non-recovery) firmware package as usual
> ```

### With pyOCD (open-source, works with CMSIS-DAP / picoprobe)

```sh
pyocd erase --chip -t nrf52840        # clears UICR too
pyocd cmd -t nrf52840 -c "write32 0x10001304 0xFFFFFFFD"
pyocd reset -t nrf52840
```

### With OpenOCD (Black Magic Probe / generic SWD)

```
# in the OpenOCD/gdb session, after halting:
nrf52 uicr_write 0x10001304 0xFFFFFFFD   # or: mww 0x10001304 0xFFFFFFFD after an erase
reset
```

## Prevention

- Build/flash the **revert-to-stock** image from firmware that includes the
  `REGOUT0 = 3.3 V` fix (both `bl_updater.c` and the pre-revert guard in
  `app_main.c`). Reverts from that image self-heal the voltage and won't brick.
- Dev boards that feed VDD directly (not through REG0) are unaffected either way.
