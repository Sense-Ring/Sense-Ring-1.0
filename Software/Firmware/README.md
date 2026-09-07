# SenseRing firmware

Firmware for the SenseRing 1.0: an nRF52832 that measures heart rate and blood
oxygen from the finger, stores every reading to on-board flash, and hands the
log to a phone over Bluetooth.

The ring never decides what a reading means. It measures, records *why* it
refused when it could not measure, and buffers until something collects.

## How it works, in five lines

- Two LEDs and a photodiode (MAX30102) sample the finger at 25 Hz.
- Every ~2 minutes the ring wakes, fills a 15-second window, and reports five
  readings from it, then puts the LEDs down. That is ~26% duty on a 31 mAh cell.
- Autocorrelation over the window gives the pulse period and a 0–1000
  confidence; the red/IR ratio gives SpO₂.
- Each reading becomes a 16-byte record in a flash ring buffer — about **4 days**
  of readings, 14,592 records — carrying its own confidence, perfusion, contact
  state, step delta and movement bucket.
- A client subscribes over BLE, replays the backlog, and acknowledges it. Only
  then does the ring erase.

An accelerometer (BMA530) runs continuously at ~15 µA: it decides when the hand
is still enough to be worth measuring, and tags every record with how much it
moved.

**`src/ARCHITECTURE.md` is the real documentation** — one section per source
file, explaining the reasoning rather than restating the code. Start at §0 if
embedded work is new to you.

## Hardware

Board `sensering_v29`, defined in-tree under `boards/`. The schematic this
firmware was written against is `.sense/SenseRing v49.pdf`, and it is the
authority whenever it and a document disagree — the version numbers differ, and
the schematic is the one that matches the pinout. The repository's own hardware
sources live in [`Hardware/`](../../Hardware/).

| | |
|---|---|
| MCU | nRF52832 (QFAA), 512 KB flash / 64 KB RAM, no DC/DC inductor |
| PPG | MAX30102, I²C `0x57`, on a switched VLED+ rail |
| IMU | BMA530, I²C `0x18` |
| PMIC | BQ25180, 31 mAh LiPo |
| Rail | XC6504 1.8 V LDO |
| Console | **RTT only — the board has no UART** |

## Building

The toolchain is the nRF Connect SDK (Zephyr). Built against **NCS v3.3.1**.

```
west build -b sensering_v29 -- -DBOARD_ROOT=<path to this repo>
```

`build_env.bat` wraps that for a Windows-side NCS install — edit the two NCS
paths at the top; it works out the repo location itself. It exists because `west` and the toolchain live on the
Windows side, so builds have to run through `cmd.exe` rather than from a WSL
shell with exported variables — bash environment variables do not propagate to
Windows processes.

A clean build currently produces:

```
FLASH:  209 232 B / 248 KB   82.4%
RAM:     43 428 B /  64 KB   66.3%
```

The `mcuboot` and `image-0` partitions remain in the map and the app links over
them; the former OTA slots were reclaimed for the vitals log, so **there is no
OTA path** — flashing needs a J-Link.

```
west flash          # J-Link, nRF52832_xxAA at 1000 kHz
```

## Tests

The pure-arithmetic halves run on a host compiler in about a second, with no
Zephyr, board or NCS toolchain:

```
cd tests/host
make            # everything      — 63,377 checks
make vitals     # the rate estimator, against synthetic PPG
make flash      # the log's ring arithmetic, against a modelled NOR part
make sweep      # candidate BPM floors side by side
```

The flash model only ever clears bits, exactly like the part, so writing into a
slot nobody erased first fails the way hardware fails. Neither target is wired
into `CMakeLists.txt` — deliberately, because a test that needs the full
toolchain is a test nobody runs. Nothing covers the duty cycle or the BLE
protocol; both need a kernel.

`tests/bench/` holds bring-up modes that are kept but not compiled — an I²C bus
scan and IMU probe, for a board revision that fails to come up.

## Bluetooth

Standard Heart Rate (`0x180D`) and Battery services, plus a custom service at
`f1a00001-9c1b-4d3e-a7b2-5e8c6d9f0a11`:

| Characteristic | Purpose |
|---|---|
| `…0002` | live vitals package — byte-identical to the flash record |
| `…0003` | live motion — timestamp, x/y/z, step total |
| `…0004` | control — set the clock, request a replay, acknowledge one |
| `…0005` | history — the framed, resumable backlog replay |
| `…0006` | status — cell level and step total |

The custom characteristics are encryption-gated and bonds persist. The ring
decides who may pair: only with no bond yet, or inside a two-minute window
opened by a power-on boot.

Live streaming is **off** by default (`BLE_LIVE_STREAM` in `main.c`) — the radio
carries nothing per reading, and data leaves in page-sized batches.

## Licence

This firmware is MIT — see [`LICENSE`](LICENSE). The SenseRing 1.0 repository as
a whole is GPLv3; MIT is compatible with that, so a combined work still
distributes under the GPL.

## Layout

```
src/            firmware, one .c per subsystem, plus ARCHITECTURE.md
boards/         the sensering_v29 board definition and DTS
tests/host/     host-compiled tests for vitals.c and flash_store.c
tests/bench/    uncompiled bring-up helpers
prj.conf        Kconfig, every line commented with why
.sense/         schematic
LICENSE         MIT
```
