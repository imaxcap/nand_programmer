# NANDO command-line NAND programmer

NANDO is an STM32-based parallel NAND programmer. The host application is a
small C++17 command-line tool for Linux and Windows; it has no Qt or Boost
dependency.

The existing STM32 firmware, USB CDC protocol, NAND HAL, bootloader, and board
design are retained. File processing and future ECC/layout conversion belong
on the host.

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
nand> verify image.bin 0
```

Available operations:

- `id`: print every NAND ID byte returned by the firmware and report whether
  the first five bytes match the CSV. It does not require a database match.
- `probe [chip-name]`: read the firmware version and NAND ID, match the CSV
  database, and configure the programmer. Supplying a name forces a database
  entry for otherwise unknown IDs.
- `info`: show the active ID and database geometry.
- `read FILE [OFFSET] [LENGTH]`: read data bytes. Offset and length must be
  data-page aligned.
- `read.raw FILE [START-PAGE] [PAGE-COUNT]`: read physical `data+OOB` pages
  without bad-block skipping.
- `erase all [--yes]` or `erase OFFSET LENGTH [--yes]`: erase block-aligned
  ranges. Interactive mode asks for confirmation; one-shot mode requires
  `--yes`.
- `write FILE [OFFSET]`: write data pages, padding the final page with `0xff`.
- `write.raw FILE [START-PAGE]`: write complete `data+OOB` pages verbatim.
- `verify FILE [OFFSET] [--raw]`: stream-compare NAND content against a file.
- `write.qpic`: reserved for future Qualcomm QPIC BCH/layout support.

Numbers accept decimal, `0x` hexadecimal, and `K`, `M`, or `G` binary suffixes.

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

## Build

Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The executable and database are placed under `build/`:

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

The chip database search order is `--db PATH`, `data/` beside the executable,
the installation `share/nandprog/` directory, and the source-tree `data/`
directory.

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
