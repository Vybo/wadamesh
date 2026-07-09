# Vendored Faux86-remake core (IBM PC-XT / 8086 emulator)

Source: https://github.com/moononournation/Faux86-remake (ESP32-S3 fork of
ArnoldUK/Faux86-remake, which is based on jhhoward/Faux86, based on Mike
Chambers' Fake86). Fetched 2026-07-10.

License: **GPL-2.0-or-later** (per every source-file header: "version 2 of the
License, or (at your option) any later version"). Compatible with wadamesh's
GPL-3.0-or-later. `COPYING` (GPL-2 text) retained.

The whole `src/` core is vendored. Platform frontends are NOT compiled — they are
excluded in platformio.ini's T-Deck `build_src_filter`:
- `console.cpp`   — terminal frontend
- `netcard.cpp` / `packet.cpp` — NE2000 networking (needs host sockets/libpcap)

Local modifications:
- `Config.h`: `#define NETWORKING_OLDCARD` commented out — no host sockets/libpcap
  on the ESP32 (the emulated NE2000 stays, just no real comms).

Compiled T-Deck-only (needs PSRAM for the 1MB machine + SD + input); the V4 build
does not compile this dir and `DosBox` is a stub there.

The wadamesh host lives OUTSIDE this dir in `../DosBox.{h,cpp}` — it implements
Faux86's HostSystemInterface (framebuffer/timer/audio/disk) and `Faux86::log()`.
SCAFFOLD status: the core compiles + links; the host is a stub (blit/audio/disk
are no-ops, no VM is run yet). Real player wiring is the next step.
