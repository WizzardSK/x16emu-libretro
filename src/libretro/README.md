# x16emu libretro core

Builds the Commander X16 emulator as a libretro core (RetroArch and other
frontends), without SDL.

    make -f Makefile.libretro            # x16emu_libretro.so / .dylib / .dll
    make -f Makefile.libretro HAVE_ZLIB=0   # no gzip-compressed SD images

## Files

- `libretro.c`: the core. `retro_run()` calls `emulator_loop()`, which returns
  after each VERA frame when built with `__LIBRETRO__`, just like the
  WebAssembly build.
- `shim/SDL.h`: the few SDL pieces the emulation code needs (`SDL_RWops` over
  stdio, timers, scancodes). Window, renderer, audio device and game
  controller code is compiled out with `__LIBRETRO__` in `video.c`,
  `audio.c`, `joystick.c`, `timing.c` and `main.c`; the SDL build is unchanged.
- `debugger_stub.c`: the SDL-drawn debugger is not part of the core.

## Using it

- Put `rom.bin` from an x16emu release in `<system>/x16/rom.bin`.
- Content: `.img` (SD card image, boots `AUTOBOOT.X16`), `.prg` and `.bas`
  (loaded and run at start and after every reset; the content's folder is
  HostFS device 8), `.crt` (cartridge). The core also starts without content,
  into BASIC.
- Input: X16 keyboard through the frontend's keyboard (use Game Focus so the
  frontend's hotkeys do not take the keys), up to four SNES controllers from
  the RetroPads (the RetroPad button order is the SNES shift-register order),
  mouse from the frontend's mouse.
- Audio is output at the VERA rate, 48828 Hz; video is 640x480 at 59.52 Hz.

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

- Save states cover the whole machine: CPU, RAM and banked RAM, cartridge
  memory, VERA (VRAM, registers, FX, scanline position), PSG, PCM FIFO,
  YM2151 (through ymfm's own state), VIAs, I2C/SMC/RTC, SD card SPI state,
  SNES pad latches and the audio resampler. Loading a state and running
  gives the same video and audio, bit for bit, as the original run
  (`savestate_features = "deterministic"`). A state is refused when CPU,
  RAM size or cartridge differ from the running machine. The contents of
  the SD card image are not part of a state, as with a real disk.
- The RTC's 64-byte NVRAM, where the KERNAL keeps its settings, is the
  save RAM; the frontend stores it as the game's `.srm`.
- HostFS no longer adds host time to the emulated clock in this build, so
  a run does not depend on how fast the host serves files.

## Builds

`.github/workflows/libretro.yml` builds Linux (x86_64, aarch64), Windows,
macOS (arm64, x86_64) and Android (`jni/`, arm64-v8a, armeabi-v7a, x86_64,
x86) and publishes them in the `libretro-latest` release.
