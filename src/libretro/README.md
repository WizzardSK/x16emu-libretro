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

Not supported yet: save states, core options (CPU speed, RAM size, keymap),
NVRAM saving.
