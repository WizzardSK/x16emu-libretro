# x16emu libretro core

Builds the Commander X16 emulator as a libretro core (RetroArch and other
frontends), without SDL.

    make -f Makefile.libretro                      # x16emu_libretro.so / .dylib / .dll
    make -f Makefile.libretro platform=win CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++
    make -f Makefile.libretro platform=osx arch=arm64
    make -f Makefile.libretro platform=ios-arm64   # x16emu_libretro_ios.dylib
    make -f Makefile.libretro platform=tvos-arm64  # x16emu_libretro_tvos.dylib
    ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk
    make -f Makefile.libretro HAVE_ZLIB=0          # no gzip-compressed SD images

The source list is in `Makefile.common`, shared by `Makefile.libretro` and
`jni/Android.mk`. zlib is on by default except on Windows.

## Files

- `libretro.c`: the core. `retro_run()` calls `emulator_loop()`, which returns
  after each VERA frame when built with `__LIBRETRO__`, just like the
  WebAssembly build.
- `shim/SDL.h`: the few SDL pieces the emulation code needs (`SDL_RWops` over
  stdio, timers, scancodes). Window, renderer, audio device and game
  controller code is compiled out with `__LIBRETRO__` in `video.c`,
  `audio.c`, `joystick.c`, `timing.c` and `main.c`; the SDL build is unchanged.
- `debugger_stub.c`: the SDL-drawn debugger is not part of the core.
- `../state.h`: save-state walk used by every emulation module.
- `x16emu_libretro.info` (repository root): the core info file, in the
  format of [libretro-core-info](https://github.com/libretro/libretro-core-info);
  its name has to match the core's file name.

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

| Target | GitHub Actions | libretro GitLab |
|---|---|---|
| Linux x86_64, i686, aarch64 | yes | yes |
| Windows x86_64, i686 | yes (MinGW) | yes |
| macOS | universal (arm64 + x86_64) | x86_64, arm64 |
| iOS arm64, tvOS arm64 | yes | yes |
| Android armeabi-v7a, arm64-v8a, x86_64, x86 | yes | yes |

- `.github/workflows/libretro.yml` builds every target on each push to the
  `libretro` branch and publishes the zips (core plus `.info`) in the
  [libretro-latest release](../../../../releases/tag/libretro-latest).
- `.gitlab-ci.yml` uses the `libretro-infrastructure/ci-templates`, as the
  other libretro cores do, for building on libretro's buildbot.
