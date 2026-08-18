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

## QPIC image contract

`write.qpic` implements the standard x8 BCH4/BCH8 page layout used by
Qualcomm QPIC NAND controllers (e.g. IPQ807x / IPQ60xx / IPQ50xx such as Xiaomi AX5)
and [qcom-nandc-pagify](https://github.com/ecsv/qcom-nandc-pagify). It uses
516-byte codeword data, a one-byte `0xff` bad-block marker, 7-byte BCH4 or
13-byte BCH8 parity, and the corresponding `0xff` codeword/OOB padding. A
partial final input page is padded with `0x00`, matching the reference tool.

The ECC algorithm (`bch4` vs `bch8`) is automatically determined from the active
NAND chip's `page_size` and `spare_size` (OOB) using the Qualcomm QPIC standard formula:
- Codewords per page = `page_size / 512`
- Required minimum OOB for 8-bit ECC = `(page_size / 512) * 20`
- If `oob_size >= min_oob_for_bch8`, **BCH8** is chosen (e.g., 2KB page with 128B OOB, 4KB page with 224B/256B OOB).
- Otherwise, **BCH4** is chosen (e.g., 2KB page with 64B OOB like standard 128MB SLC NAND on AX5).
- You can still explicitly force an algorithm using `--ecc bch4` or `--ecc bch8`.

The generated data+OOB page is streamed through the existing raw write
protocol with STM32 hardware ECC disabled. Bad blocks are skipped. The current
implementation targets standard x8 parallel NAND geometry; x16, RS/SBL, and
SoC-specific boot-partition fixups are not implemented.

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

## Firmware and hardware

The firmware remains under `firmware/` and is not built by the host CMake
project. The current board exposes a USB CDC serial device and uses the legacy
3.5.0 command protocol.

PCB and adapter designs remain under `kicad/`. The project is licensed under
GPLv3, subject to the third-party firmware library licenses described in the
source tree.

The unchanged firmware reads five NAND ID cycles. The host preserves and
prints any response length, but reading more than five ID bytes requires a
future firmware change.
