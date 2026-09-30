# Reverting to stock firmware

You do **not** need to replace the bootloader to run stock ChameleonUltra
firmware, and this fork no longer ships an in-field bootloader-swap ("recovery
UF2") path — it was removed because swapping the bootloader from a running
application is unreliable on the nRF52840 and could leave a device unresponsive
until power was physically removed.

The important fact: **this fork's bootloader is a superset of the stock one.**
It lives at the stock address (`0xF3000`, 44 KB) and provides both UF2
drag-and-drop *and* the stock Nordic CDC serial / BLE DFU transports. So stock
firmware installs straight through it — no bootloader change required.

## Revert to stock firmware (no SWD, no tools beyond nrfutil)

1. Download the stock application from the upstream release you want:
   <https://github.com/RfidResearchGroup/ChameleonUltra/releases>
   (the application DFU zip, e.g. `*-app-*.zip`).

2. Put the device into **serial DFU**: with the device unplugged, hold
   **button B** and plug in USB. It enumerates as the Nordic DFU device
   (USB `1915:521f`).

3. Flash the stock app over serial DFU:
   ```sh
   nrfutil device program --firmware <stock-app-dfu>.zip --traits nordicDfu
   ```
   (or the upstream ChameleonUltra desktop/GUI updater, or the Android/iOS app —
   any stock DFU flasher works, because the transport is the stock one.)

4. The device reboots into stock firmware. Done.

You keep this fork's bootloader (UF2 + serial DFU). Nothing about running stock
firmware requires removing it, and there is no user-visible downside to leaving
it in place — it behaves as the stock bootloader plus UF2.

## If you genuinely need the *stock bootloader* back

Restoring the literal stock bootloader is an **SWD** operation — the same way it
was installed at the factory. It is intentionally not offered as an in-field
flash, because installing a bootloader at its linked address from a running
image cannot be done reliably without a debugger. Use a debug probe (J-Link,
CMSIS-DAP / picoprobe, Black Magic Probe) and program the upstream `sd_bl` /
bootloader image, then the stock app. This is only needed for development or
resale; ordinary use never requires it.
