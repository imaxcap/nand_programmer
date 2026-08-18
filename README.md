# NANDO command-line NAND programmer

NANDO is an STM32-based parallel NAND programmer. The host application is a
small C++17 command-line tool for Linux and Windows; it has no Qt or Boost
dependency.

The existing STM32 firmware, USB CDC protocol, NAND HAL, bootloader, and board
design are retained. File processing and ECC/layout conversion run on the
host.

## Commands

The program supports both one-shot commands and an interactive REPL:

```text
$ nandprog --device /dev/ttyACM0
nand> probe
nand> id
nand> info
nand> read dump.bin 0 0x100000
nand> read.raw dump.raw 0 1024
nand> erase 0 0x20000
nand> write image.bin 0
nand> write.raw image.raw 0
nand> write.qpic kernel.bin 0x100000 --ecc bch8
nand> verify image.bin 0
```

Available operations:

- `id`: print every NAND ID byte returned by the firmware and report whether
  the first five bytes match the CSV. It does not require a database match.
- `probe [chip-name]`: read the firmware version and NAND ID, match the CSV
  database, and configure the programmer. Supplying a name forces a database
  entry for otherwise unknown IDs.
- `info`: show the active ID and database geometry.
- `smem [--refresh]`: scan the first 4MB of NAND at 64KB block boundaries to discover
  and print the Qualcomm SMEM / MIBIB partition table (partition names, start blocks,
  block counts, offsets, and sizes).
- `flash FILE <PARTITION|OFFSET> [--ecc bch4|bch8]`: erase the target partition or
  offset range and flash the input file in Qualcomm QPIC layout in one step.
  If a partition name is supplied (e.g. `0:APPSBL` or `APPSBL`), the offset and
  partition size are automatically resolved from the SMEM partition table and checked
  against the file size. If `--ecc` is omitted, the BCH algorithm (BCH4 vs BCH8)
  is auto-detected based on page and OOB size.
- `read FILE [OFFSET] [LENGTH]`: read data bytes. Offset and length must be
  data-page aligned.
- `read.raw FILE [START-PAGE] [PAGE-COUNT]`: read physical `data+OOB` pages
  without bad-block skipping.
- `erase all` or `erase OFFSET LENGTH`: immediately erase block-aligned ranges.
  There is no confirmation prompt or `--yes` option.
- `write FILE [OFFSET]`: write data pages, padding the final page with `0xff`.
- `write.raw FILE [START-PAGE]`: write complete `data+OOB` pages verbatim.
- `write.qpic FILE [NAND-OFFSET] [--ecc bch4|bch8]`: generate a Qualcomm QPIC
  x8 page/OOB layout on the PC and write it as raw pages. If `--ecc` is omitted,
  the ECC mode is auto-detected from the chip geometry (BCH8 if `oob_size >= (page_size / 512) * 20`,
  otherwise BCH4). `NAND-OFFSET` is a data-space byte offset, defaults to zero,
  and must be data-page aligned.
- `read.qpic FILE <PARTITION|OFFSET> [LENGTH] [--ecc bch4|bch8]`: read physical raw pages
  from the specified partition name or byte offset and automatically de-interleave the
  QPIC Codewords, saving a clean flat binary file.
- `verify.qpic FILE [PARTITION|OFFSET] [--ecc bch4|bch8]`: stream-read raw NAND pages,
  de-interleave QPIC Codewords in real time, and compare against a flat image file.
- `debug [on|off]`: toggle verbose transport and protocol trace logging in REPL.
- `verify FILE [OFFSET] [--raw]`: stream-compare NAND content against a file.

Numbers accept decimal, `0x` hexadecimal, and `K`, `M`, or `G` binary suffixes.

Standard `write`, `write.raw`, and `write.qpic` do not erase NAND automatically.
Use the `flash` command or an explicit `erase` before writing.

## Raw image contract

`write.raw` requires the file size to be an exact multiple of
`data_page_size + oob_size`. It does not pad, truncate, calculate ECC, change
OOB bytes, or skip bad blocks. Hardware/on-die ECC is requested off through the
existing protocol flag.

The unchanged firmware 3.5.x can acknowledge write-end before it checks the
last NAND busy/status result. The CLI warns about this limitation and
`verify FILE START-PAGE --raw` is recommended after raw writes. A future
firmware update can close that acknowledgement race without changing the wire
protocol.

## Qualcomm QPIC Interleaved Architecture & Codeword Logic

Qualcomm Parallel Interface Controller (QPIC) NAND controllers (used in IPQ40xx / IPQ60xx / IPQ807x / IPQ50xx such as Xiaomi AX5 / AX6 / AX1800) do **not** use the traditional flat NAND layout (where 2048 data bytes are contiguous and followed by 64 OOB bytes).

Instead, QPIC splits each physical page into multiple self-contained **Codewords (CWs)** of 512 data bytes each:

```text
Codewords Per Page = Page_Size / 512
```

- **2KB Page (2048 bytes)** = **4 Codewords**
- **4KB Page (4096 bytes)** = **8 Codewords**

### 1. Traditional Flat NAND vs Qualcomm QPIC Interleaved Layout

```text
Traditional NAND (Flat 2KB + 64B):
┌────────────────────────────────────────────────────────┬─────────────────────────┐
│              2048-Byte User Data Area                  │  64-Byte Spare/ECC Area │
│  [0 ............................................ 2047] │  [2048 ........... 2111]│
└────────────────────────────────────────────────────────┴─────────────────────────┘

Qualcomm QPIC Interleaved (2KB + 64B, 4 x 528B Codewords):
┌───────────────────────┬───────────────────────┬───────────────────────┬───────────────────────┐
│      Codeword 0       │      Codeword 1       │      Codeword 2       │      Codeword 3       │
│    (Bytes 0 ~ 527)    │   (Bytes 528 ~ 1055)  │  (Bytes 1056 ~ 1583)  │  (Bytes 1584 ~ 2111)  │
├───────────┬───────────┼───────────┬───────────┼───────────┬───────────┼───────────┬───────────┤
│ 512B Data │ 16B OOB   │ 512B Data │ 16B OOB   │ 512B Data │ 16B OOB   │ 512B Data │ 16B OOB   │
│ & Metadata│ & BCH ECC │ & Metadata│ & BCH ECC │ & Metadata│ & BCH ECC │ & Metadata│ & BCH ECC │
└───────────┴───────────┴───────────┴───────────┴───────────┴───────────┴───────────┴───────────┘
```

### 2. Internal Structure of a Single Codeword

Inside each Codeword, the Bad Block Marker (BBM) is positioned at `bbm_pos = Page_Size % Codeword_Size` so that on the physical NAND, the BBM in the final Codeword aligns exactly with physical column `Page_Size` (the first byte of the traditional spare area).

```text
BCH4 Single Codeword (528 Bytes Total):
┌──────────────────────────────┬─────┬──────────────────────────┬──────────────┬──────────────┐
│ Data Part 1 (Pre-BBM)        │ BBM │ Data Part 2 (Post-BBM)   │ BCH4 Parity  │ Pad (0xFF)   │
│ (bbm_pos bytes)              │ 1B  │ (516 - bbm_pos bytes)    │ 7 Bytes      │ 4 Bytes      │
└──────────────────────────────┴─────┴──────────────────────────┴──────────────┴──────────────┘
├──────────── 516-Byte Codeword Data Payload ──────────────────┤
├─────────────────────────────────── 528 Bytes Total ─────────────────────────────────────────┤

BCH8 Single Codeword (532 Bytes Total):
┌──────────────────────────────┬─────┬──────────────────────────┬──────────────┬──────────────┐
│ Data Part 1 (Pre-BBM)        │ BBM │ Data Part 2 (Post-BBM)   │ BCH8 Parity  │ Pad (0xFF)   │
│ (bbm_pos bytes)              │ 1B  │ (516 - bbm_pos bytes)    │ 13 Bytes     │ 2 Bytes      │
└──────────────────────────────┴─────┴──────────────────────────┴──────────────┴──────────────┘
├──────────── 516-Byte Codeword Data Payload ──────────────────┤
├─────────────────────────────────── 532 Bytes Total ─────────────────────────────────────────┤
```

### 3. Geometry & BCH Mode Matrix

The ECC mode is determined automatically by the available OOB space:

```text
Min_OOB_For_BCH8 = (Page_Size / 512) * 20
```

| NAND Geometry | CW Count | Mode | CW Size | BBM Pos (`Page % CW`) | Raw Page Size | Auto ECC Selection Rule |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **2K Page + 64B OOB** | 4 | **BCH4** | 528 B | **464** (`2048 % 528`) | 2112 B | `64B < 80B` $\implies$ **BCH4** |
| **2K Page + 128B OOB**| 4 | **BCH8** | 532 B | **452** (`2048 % 532`) | 2176 B | `128B >= 80B` $\implies$ **BCH8** |
| **4K Page + 128B OOB**| 8 | **BCH4** | 528 B | **400** (`4096 % 528`) | 4224 B | `128B < 160B` $\implies$ **BCH4** |
| **4K Page + 256B OOB**| 8 | **BCH8** | 532 B | **372** (`4096 % 532`) | 4256 B | `256B >= 160B` $\implies$ **BCH8** |

> **Note on 4K + 128B**: BCH8 requires `8 * 20 = 160` bytes of OOB. A 128-byte OOB NAND cannot fit BCH8 parity; therefore, 4K+128B NAND strictly uses **BCH4** (`8 * 16 = 128` bytes).

### 4. QPIC Toolchain Commands Summary

- `smem [--refresh]`: Scans the first 4MB of NAND in 64KB block steps, de-interleaves QPIC codewords in real time, and prints the Qualcomm partition table (`0:SBL1`, `0:MIBIB`, `0:QSEE`, `0:DEVCFG`, `0:RPM`, `0:CDT`, `0:APPSBL`, `rootfs`, etc.).
- `flash FILE <PARTITION|OFFSET> [--ecc bch4|bch8]`: Erases the destination block range (with bad-block skipping) and flashes the image encoded in QPIC layout in one step.
- `read.qpic FILE <PARTITION|OFFSET> [LENGTH]`: Reads physical raw pages, strips QPIC ECC/BBM, and reassembles a 100% clean flat binary image.
- `verify.qpic FILE [PARTITION|OFFSET]`: Stream-reads raw pages and compares against flat binary image files in memory without needing full disk dumps.

## Build

Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The executable and database are placed together under `build/`:

```sh
./build/nandprog --device /dev/ttyACM0 probe
```

Windows with Visual Studio:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Then run:

```powershell
.\build\Release\nandprog.exe --device COM3 probe
```

Installed packages place `nandprog` (or `nandprog.exe`) and
`nando_parallel_chip_db.csv` in the same `bin/` directory. The database search
order is `--db PATH`, beside the executable, the legacy `data/` beside the
executable, the legacy installation `share/nandprog/` directory, and the
source-tree `data/` directory.

## Firmware v3.6.0 & Hardware Acceleration

The STM32F103 firmware (designated v3.6.0) includes major hardware-level optimizations and features:

1. **32-Bit FSMC Unrolled Burst Optimization**:
   - Bulk USB packet processing and 32-bit FSMC write acceleration achieving 4.5~5.5 MB/s write throughput and 5.8 MB/s read throughput.
   - Safe asynchronous polling ensuring full NAND status verification before write acknowledgement.

2. **Native ONFI 1.0 Parameter Page Engine**:
   - Hardware-level `0xEC` parameter page reader with full CRC-16 (`0x8005`, init `0x4F4E`) integrity verification.
   - Automatically detects Micron, Toshiba/Kioxia, Macronix, Winbond, and Spansion ONFI NAND flash geometries (page size, block size, total size, spare size, row/col address cycles) directly on the MCU without requiring PC database entries.

3. **Industrial SSD RDT (Reliability Demonstration Test) Hardware Self-Test**:
   - `test [all | PART|OFF LEN] [--mode chip|block] [--passes N] [--delay DURATION]`:
     - **Full-Chip RDT Spanning Mode (`--mode chip` / default)**: Emulates industrial SSD RDT burn-in workflows. Phase 1 erases and programs PRBS32 pseudo-random patterns + 512B ECC across all blocks simultaneously to stress-test wordline coupling and substrate leakage; Phase 2 reads back, checks parity syndromes, and marks damaged blocks.
     - **Per-Block Immediate Mode (`--mode block` / `-b`)**: Rapid block-by-block erase $\to$ program $\to$ read-verify $\to$ erase sequence.
     - **Charge Retention & Gate Leakage Testing (`test.write` / `test.verify` / `--delay`)**: Write baseline PRBS patterns, disconnect or wait for a retention duration (e.g. `--delay 10m`), and verify cell charge retention.
     - **Automated OOB Bad Block Marking**: Hardware automatically programs `0x00` into byte 0 of spare area on uncorrectable errors and updates the RAM bad block table.

4. **Physical Scrub (`scrub`)**:
   - Unconditional physical block erase command (`0x10`) bypassing all bad-block tables and OOB markers (requires explicit `YES` confirmation).

5. **Live Throughput & Bad Block Progress Output**:
   - Real-time progress bar with instantaneous and overall average speed display, plus smooth bad-block skip reporting without line-wrapping glitches.

## Firmware Build & GitHub Actions CI

The firmware can be built locally using ARM GCC or downloaded directly from GitHub Actions artifacts:

```sh
# Local firmware compilation
make -C firmware -f Makefile.linux TOOLCHAIN=arm-none-eabi-
```

Generated firmware binaries in `firmware/obj/`:
- `nando_fw.bin` / `nando_fw.hex`: Complete unified firmware binary.
- `bootloader_fw.hex`: High-speed USB bootloader.
- `app_fw_1.hex` / `app_fw_2.hex`: Main programmer application images.

PCB and adapter designs remain under `kicad/`. The project is licensed under GPLv3.
