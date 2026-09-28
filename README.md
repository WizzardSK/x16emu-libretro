# x16emu-libretro

[![Build libretro core](../../actions/workflows/libretro.yml/badge.svg)](../../actions/workflows/libretro.yml)

A libretro core of **x16emu**, the official emulator of the
[Commander X16](https://commanderx16.com), for RetroArch and other libretro
frontends. It is built from the
[X16Community/x16-emulator](https://github.com/X16Community/x16-emulator)
sources without SDL.

It emulates the 65C02 (or 65C816) CPU, the VERA video chip with its PSG and
PCM audio, the YM2151 FM synthesizer, the SD card, the real-time clock, up to
four SNES controllers, the keyboard and the mouse.

## Download

Prebuilt cores are in the
[libretro-latest release](../../releases/tag/libretro-latest), rebuilt on
every push:

| Platform | File |
|---|---|
| Linux x86_64, i686, aarch64 | `x16emu_libretro.so` |
| Windows x86_64, i686 | `x16emu_libretro.dll` |
| macOS (Apple Silicon and Intel) | `x16emu_libretro.dylib` |
| iOS, tvOS | `x16emu_libretro_ios.dylib`, `x16emu_libretro_tvos.dylib` |
| Android armeabi-v7a, arm64-v8a, x86_64, x86 | `x16emu_libretro_android-<abi>.so` |

## Setup

1. Put the core in RetroArch's `cores` folder and `x16emu_libretro.info` in
   its `info` folder.
2. Put `rom.bin`, the KERNAL ROM, in the system folder as
   `system/x16/rom.bin`. It comes with every
   [x16emu release](https://github.com/X16Community/x16-emulator/releases)
   (or build it from [x16-rom](https://github.com/X16Community/x16-rom)).

## Content

| Type | What the core does |
|---|---|
| `.img` | SD card image, read and written in place; the KERNAL runs `AUTOBOOT.X16` from it |
| `.prg` | loaded and run at start and after every reset |
| `.bas` | BASIC listing, typed in and run at start and after every reset |
| `.crt` | cartridge |
| no content | boots into BASIC |

With `.prg`, `.bas` and `.crt`, the folder of the content is the host file
system on device 8, so a program can load its data files.

## Controls

- **Keyboard**: the X16 keyboard. Turn on RetroArch's *Game Focus*
  (Scroll Lock) so its hotkeys do not take the keys.
- **Joypads**: four SNES controllers from RetroPads 1-4. The RetroPad button
  numbering is the SNES order, so B, A, Y, X, L, R, Select and Start map
  one to one.
- **Mouse**: the host mouse (can be turned off).

## Core options

| Option | Values | Applies |
|---|---|---|
| CPU | 65C02, 65C816 | on restart |
| CPU Speed | 1-40 MHz (8 is the real machine) | on restart |
| Banked RAM | 64 KB - 2 MB (512 KB standard) | on restart |
| Set Clock from Host | on / off | on restart |
| Keyboard Layout | the KERNAL's 28 layouts | on restart |
| Mouse | on / off | at once |
| Mid-line Effects | on / off | at once |
| YM2151 IRQ | on / off | at once |

## Saves

- **Save states** cover the whole machine: CPU, RAM and banked RAM,
  cartridge memory, VERA (VRAM, registers, FX, scanline position), PSG, PCM,
  YM2151, VIAs, SMC, RTC, SD card interface, SNES controller latches and the
  audio resampler. They are deterministic: loading a state and running gives
  the same picture and sound, bit for bit, so rewind, run-ahead and netplay
  work. A state is refused when CPU, RAM size or cartridge differ. The
  contents of the SD card image are not part of a state, as with a real
  disk.
- **Save RAM**: the RTC's 64-byte NVRAM, where the KERNAL keeps its settings,
  is saved by the frontend as the game's `.srm`.

## Building

    make -f Makefile.libretro                      # Linux, macOS: x16emu_libretro.so / .dylib
    make -f Makefile.libretro platform=win CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++
    make -f Makefile.libretro platform=osx arch=arm64
    make -f Makefile.libretro platform=ios-arm64
    make -f Makefile.libretro platform=tvos-arm64
    ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk
    make -f Makefile.libretro HAVE_ZLIB=0          # without gzip-compressed SD images

The source list is in `Makefile.common`, shared with `jni/Android.mk`. zlib
is on by default except on Windows.

`.github/workflows/libretro.yml` builds every platform above and publishes
the release; `.gitlab-ci.yml` builds with libretro's
`libretro-infrastructure/ci-templates`, like the other libretro cores.

## How it is put together

- `src/libretro/libretro.c`: the core. `retro_run()` calls `emulator_loop()`,
  which returns after each VERA frame, the way the WebAssembly build of
  x16emu yields to the browser.
- `src/libretro/shim/SDL.h`: the few SDL pieces the emulation code uses
  (file I/O, timers, scancodes). Window, renderer, audio device, game
  controller and debugger code in the shared sources is compiled out with
  `__LIBRETRO__`; it is kept there so changes from upstream x16emu still
  merge.
- `src/state.h`: the save-state walk. Every module has one `xxx_state()`
  function that measures, saves or loads, so the three always agree.
- `x16emu_libretro.info`: the core info, in the format of
  [libretro-core-info](https://github.com/libretro/libretro-core-info).

Unlike the standalone emulator, the core does not add the host time a
host-file-system operation took to the emulated clock: the frontend paces
frames, and host time leaking in would break deterministic save states.

## Emulator I/O registers

As in x16emu, `$9FB0`-`$9FBF` are emulator registers; `$9FBE`/`$9FBF` read
as `"1"`/`"6"` so software can detect the emulator. See the
[x16emu README](https://github.com/X16Community/x16-emulator#emulator-io-registers).

## Links

- Commander X16: [commanderx16.com](https://commanderx16.com),
  [forum](https://cx16forum.com/forum/)
- Upstream emulator:
  [X16Community/x16-emulator](https://github.com/X16Community/x16-emulator)

## License

Copyright (c) 2019-2023 Michael Steil &lt;mist64@mac.com&gt;,
[www.pagetable.com](https://www.pagetable.com/), et al.
All rights reserved. License: 2-clause BSD (see `LICENSE`).

The libretro API header is MIT licensed, the SDL scancode values in
`src/libretro/shim/SDL.h` come from SDL2 (zlib license), ymfm is BSD 3-Clause.
