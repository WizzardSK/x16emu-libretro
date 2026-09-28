// Commander X16 Emulator - libretro core
// All rights reserved. License: 2-clause BSD
//
// Drives the emulator one video frame per retro_run(): emulator_loop() returns
// after each completed VERA frame when built with __LIBRETRO__, the same way
// the WebAssembly build yields to the browser.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <strings.h>

#include "libretro.h"
#include "../glue.h"
#include "../audio.h"
#include "../cartridge.h"
#include "../files.h"
#include "../i2c.h"
#include "../ieee.h"
#include "../joystick.h"
#include "../keyboard.h"
#include "../memory.h"
#include "../rtc.h"
#include "../sdcard.h"
#include "../timing.h"
#include "../video.h"
#include "../wav_recorder.h"
#include "../cpu/fake6502.h"
#include "../version.h"

// From main.c
extern SDL_RWops *prg_file;
extern bool run_after_load;
extern char *paste_text;
extern char paste_text_data[65536];
extern char *cartridge_path;
extern bool headless;
extern bool prg_done;
void *emulator_loop(void *param);
void machine_reset(void);
void main_shutdown(void);

// The VERA dot clock is 25 MHz and a frame is 800 x 525 dots
#define X16_FPS (25000000.0 / (800 * 525))

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static char system_dir[PATH_MAX];
static char content_dir[PATH_MAX];
static char content_path[PATH_MAX];
static bool game_loaded;
static bool supports_bitmasks;
static int16_t audio_out[4096 * 2];

static void
fallback_log(enum retro_log_level level, const char *fmt, ...)
{
	va_list va;
	(void)level;
	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
}

// ---- Keyboard ---------------------------------------------------------------
// The frontend reports RETROK_* key codes; keyboard.c speaks SDL scancodes,
// which it turns into PS/2 key numbers for the SMC.

static SDL_Scancode
scancode_from_retrok(unsigned k)
{
	if (k >= RETROK_a && k <= RETROK_z) {
		return (SDL_Scancode)(SDL_SCANCODE_A + (k - RETROK_a));
	}
	if (k >= RETROK_1 && k <= RETROK_9) {
		return (SDL_Scancode)(SDL_SCANCODE_1 + (k - RETROK_1));
	}
	if (k >= RETROK_F1 && k <= RETROK_F12) {
		return (SDL_Scancode)(SDL_SCANCODE_F1 + (k - RETROK_F1));
	}
	if (k >= RETROK_KP1 && k <= RETROK_KP9) {
		return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (k - RETROK_KP1));
	}
	switch (k) {
		case RETROK_0: return SDL_SCANCODE_0;
		case RETROK_RETURN: return SDL_SCANCODE_RETURN;
		case RETROK_ESCAPE: return SDL_SCANCODE_ESCAPE;
		case RETROK_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
		case RETROK_TAB: return SDL_SCANCODE_TAB;
		case RETROK_SPACE: return SDL_SCANCODE_SPACE;
		case RETROK_MINUS: return SDL_SCANCODE_MINUS;
		case RETROK_EQUALS: return SDL_SCANCODE_EQUALS;
		case RETROK_LEFTBRACKET: return SDL_SCANCODE_LEFTBRACKET;
		case RETROK_RIGHTBRACKET: return SDL_SCANCODE_RIGHTBRACKET;
		case RETROK_BACKSLASH: return SDL_SCANCODE_BACKSLASH;
		case RETROK_SEMICOLON: return SDL_SCANCODE_SEMICOLON;
		case RETROK_QUOTE: return SDL_SCANCODE_APOSTROPHE;
		case RETROK_BACKQUOTE: return SDL_SCANCODE_GRAVE;
		case RETROK_COMMA: return SDL_SCANCODE_COMMA;
		case RETROK_PERIOD: return SDL_SCANCODE_PERIOD;
		case RETROK_SLASH: return SDL_SCANCODE_SLASH;
		case RETROK_CAPSLOCK: return SDL_SCANCODE_CAPSLOCK;
		case RETROK_PRINT: return SDL_SCANCODE_PRINTSCREEN;
		case RETROK_SCROLLOCK: return SDL_SCANCODE_SCROLLLOCK;
		case RETROK_PAUSE: return SDL_SCANCODE_PAUSE;
		case RETROK_INSERT: return SDL_SCANCODE_INSERT;
		case RETROK_HOME: return SDL_SCANCODE_HOME;
		case RETROK_PAGEUP: return SDL_SCANCODE_PAGEUP;
		case RETROK_DELETE: return SDL_SCANCODE_DELETE;
		case RETROK_END: return SDL_SCANCODE_END;
		case RETROK_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
		case RETROK_RIGHT: return SDL_SCANCODE_RIGHT;
		case RETROK_LEFT: return SDL_SCANCODE_LEFT;
		case RETROK_DOWN: return SDL_SCANCODE_DOWN;
		case RETROK_UP: return SDL_SCANCODE_UP;
		case RETROK_NUMLOCK: return SDL_SCANCODE_NUMLOCKCLEAR;
		case RETROK_KP_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
		case RETROK_KP_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
		case RETROK_KP_MINUS: return SDL_SCANCODE_KP_MINUS;
		case RETROK_KP_PLUS: return SDL_SCANCODE_KP_PLUS;
		case RETROK_KP_ENTER: return SDL_SCANCODE_KP_ENTER;
		case RETROK_KP0: return SDL_SCANCODE_KP_0;
		case RETROK_KP_PERIOD: return SDL_SCANCODE_KP_PERIOD;
		case RETROK_LCTRL: return SDL_SCANCODE_LCTRL;
		case RETROK_LSHIFT: return SDL_SCANCODE_LSHIFT;
		case RETROK_LALT: return SDL_SCANCODE_LALT;
		case RETROK_LSUPER: return SDL_SCANCODE_LGUI;
		case RETROK_RCTRL: return SDL_SCANCODE_RCTRL;
		case RETROK_RSHIFT: return SDL_SCANCODE_RSHIFT;
		case RETROK_RALT: return SDL_SCANCODE_RALT;
		case RETROK_RSUPER: return SDL_SCANCODE_RGUI;
		case RETROK_MENU: return SDL_SCANCODE_APPLICATION;
		case RETROK_OEM_102: return SDL_SCANCODE_NONUSBACKSLASH;
		default: return SDL_SCANCODE_UNKNOWN;
	}
}

static void
keyboard_event(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers)
{
	(void)character;
	(void)key_modifiers;
	SDL_Scancode sc = scancode_from_retrok(keycode);
	if (sc != SDL_SCANCODE_UNKNOWN) {
		handle_keyboard(down, 0, sc);
	}
}

// ---- Joypads and mouse ------------------------------------------------------

static void
update_input(void)
{
	input_poll_cb();

	for (int port = 0; port < NUM_JOYSTICKS; port++) {
		uint16_t pressed = 0;
		if (supports_bitmasks) {
			pressed = (uint16_t)input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
		} else {
			for (unsigned id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R; id++) {
				if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id)) {
					pressed |= 1 << id;
				}
			}
		}
		// RetroPad ids 0-11 are the SNES shift-register order
		joystick_libretro_set(port, pressed & 0x0fff);
	}

	static bool buttons[3];
	static const unsigned ids[3] = {RETRO_DEVICE_ID_MOUSE_LEFT, RETRO_DEVICE_ID_MOUSE_RIGHT, RETRO_DEVICE_ID_MOUSE_MIDDLE};
	bool changed = false;
	int dx = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
	int dy = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
	if (dx || dy) {
		mouse_move(dx, dy);
		changed = true;
	}
	for (int b = 0; b < 3; b++) {
		bool down = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, ids[b]) != 0;
		if (down != buttons[b]) {
			if (down) {
				mouse_button_down(b);
			} else {
				mouse_button_up(b);
			}
			buttons[b] = down;
			changed = true;
		}
	}
	if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP)) {
		mouse_set_wheel(1);
		changed = true;
	} else if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN)) {
		mouse_set_wheel(-1);
		changed = true;
	}
	if (changed) {
		mouse_send_state();
	}
}

// ---- libretro API -------------------------------------------------------------

unsigned
retro_api_version(void)
{
	return RETRO_API_VERSION;
}

void
retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name     = "Commander X16 (x16emu)";
	info->library_version  = "r" VER;
	info->valid_extensions = "img|prg|bas|crt";
	info->need_fullpath    = true;   // SD card images are read and written in place
	info->block_extract    = false;
}

void
retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->geometry.base_width   = X16_SCREEN_WIDTH;
	info->geometry.base_height  = X16_SCREEN_HEIGHT;
	info->geometry.max_width    = X16_SCREEN_WIDTH;
	info->geometry.max_height   = X16_SCREEN_HEIGHT;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps            = X16_FPS;
	info->timing.sample_rate    = AUDIO_SAMPLERATE;
}

void
retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;

	bool no_game = true;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

	static const struct retro_controller_description pads[] = {
		{ "SNES Controller", RETRO_DEVICE_JOYPAD },
	};
	static const struct retro_controller_info ports[] = {
		{ pads, 1 }, { pads, 1 }, { pads, 1 }, { pads, 1 }, { NULL, 0 },
	};
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

void
retro_init(void)
{
	struct retro_log_callback logging;
	log_cb = environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) ? logging.log : fallback_log;

	const char *dir = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir) {
		snprintf(system_dir, sizeof(system_dir), "%s", dir);
	}

	supports_bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);

	struct retro_keyboard_callback kb = { keyboard_event };
	environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb);
}

void
retro_deinit(void)
{
}

static bool has_extension(const char *path, const char *ext);

// Queue the PRG or BASIC program so the KERNAL loads and runs it once BASIC
// is up, after power-on and after every reset
static bool
queue_program(void)
{
	extern bool prg_consumed;
	// Once the KERNAL has opened the PRG, the file belongs to an IEEE channel
	// (closed with it, or by machine_reset); only an unopened one is ours
	if (prg_file && !prg_consumed) {
		SDL_RWclose(prg_file);
	}
	prg_file = NULL;
	prg_done = false;
	prg_consumed = false;
	paste_text = NULL;

	if (has_extension(content_path, "prg")) {
		prg_file = SDL_RWFromFile(content_path, "rb");
		run_after_load = true;
		return prg_file != NULL;
	}
	if (has_extension(content_path, "bas")) {
		FILE *f = fopen(content_path, "rb");
		if (!f) {
			return false;
		}
		size_t len = fread(paste_text_data, 1, sizeof(paste_text_data) - 6, f);
		fclose(f);
		strcpy(paste_text_data + len, "\rRUN\r");
		paste_text = paste_text_data;
	}
	return true;
}

static bool
has_extension(const char *path, const char *ext)
{
	size_t lp = strlen(path), le = strlen(ext);
	return lp > le && path[lp - le - 1] == '.' && !strcasecmp(path + lp - le, ext);
}

// The KERNAL ROM (rom.bin from the x16-rom release) is looked for in the
// frontend's system directory.
static bool
load_rom(void)
{
	static const char *names[] = { "x16/rom.bin", "commanderx16/rom.bin", "rom.bin" };
	char path[PATH_MAX + 32];
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", system_dir, names[i]);
		FILE *f = fopen(path, "rb");
		if (!f) {
			continue;
		}
		size_t n = fread(ROM, 1, ROM_SIZE, f);
		fclose(f);
		log_cb(RETRO_LOG_INFO, "[x16] KERNAL ROM %s (%u bytes)\n", path, (unsigned)n);
		return n > 0;
	}
	log_cb(RETRO_LOG_ERROR, "[x16] rom.bin not found: put it in %s/x16/ (from an x16-rom release)\n", system_dir);
	return false;
}

bool
retro_load_game(const struct retro_game_info *game)
{
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
		log_cb(RETRO_LOG_ERROR, "[x16] XRGB8888 is not supported\n");
		return false;
	}

	static struct retro_input_descriptor desc[4 * 13 + 1];
	static const struct { unsigned id; const char *name; } buttons[] = {
		{ RETRO_DEVICE_ID_JOYPAD_UP, "Up" }, { RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },
		{ RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" }, { RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
		{ RETRO_DEVICE_ID_JOYPAD_B, "B" }, { RETRO_DEVICE_ID_JOYPAD_A, "A" },
		{ RETRO_DEVICE_ID_JOYPAD_Y, "Y" }, { RETRO_DEVICE_ID_JOYPAD_X, "X" },
		{ RETRO_DEVICE_ID_JOYPAD_L, "L" }, { RETRO_DEVICE_ID_JOYPAD_R, "R" },
		{ RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" }, { RETRO_DEVICE_ID_JOYPAD_START, "Start" },
	};
	int n = 0;
	for (unsigned port = 0; port < NUM_JOYSTICKS; port++) {
		for (size_t b = 0; b < sizeof(buttons) / sizeof(buttons[0]); b++) {
			desc[n++] = (struct retro_input_descriptor){ port, RETRO_DEVICE_JOYPAD, 0, buttons[b].id, buttons[b].name };
		}
	}
	desc[n] = (struct retro_input_descriptor){ 0 };
	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

	if (!load_rom()) {
		return false;
	}

	content_path[0] = 0;
	headless = false;
	using_hostfs = true;
	prg_file = NULL;
	prg_done = false;
	run_after_load = false;
	paste_text = NULL;
	cartridge_path = NULL;

	const char *path = game ? game->path : NULL;
	if (path) {
		snprintf(content_dir, sizeof(content_dir), "%s", path);
		char *slash = strrchr(content_dir, '/');
#ifdef _WIN32
		char *bslash = strrchr(content_dir, '\\');
		if (bslash > slash) {
			slash = bslash;
		}
#endif
		if (slash) {
			*slash = 0;
		} else {
			strcpy(content_dir, ".");
		}
		// HostFS (device 8) serves the files next to the content, so a PRG
		// can load its data files
		fsroot_path = (uint8_t *)content_dir;
		startin_path = (uint8_t *)content_dir;

		if (has_extension(path, "img")) {
			// Boot from the SD card image; the KERNAL runs AUTOBOOT.X16 if present
			sdcard_set_path(path);
			using_hostfs = false;
		} else if (has_extension(path, "prg") || has_extension(path, "bas")) {
			snprintf(content_path, sizeof(content_path), "%s", path);
			if (!queue_program()) {
				log_cb(RETRO_LOG_ERROR, "[x16] cannot open %s\n", path);
				return false;
			}
		} else if (has_extension(path, "crt")) {
			cartridge_path = (char *)path;
			if (!cartridge_load(path, true)) {
				log_cb(RETRO_LOG_ERROR, "[x16] cannot load cartridge %s\n", path);
				cartridge_path = NULL;
				return false;
			}
		}
	}

	audio_init(NULL, 32);
	video_init(1, 1.0f, "best", false, 1.0f);
	wav_recorder_set_path(NULL);
	memory_init();
	joystick_init();
	rtc_init(false);
	machine_reset();
	timing_init();

	game_loaded = true;
	return true;
}

bool
retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
	(void)type;
	(void)info;
	(void)num;
	return false;
}

void
retro_unload_game(void)
{
	if (!game_loaded) {
		return;
	}
	// The SD card file is one of the files main_shutdown() closes, so it has
	// to be detached first
	sdcard_detach();
	main_shutdown();
	// A PRG the KERNAL has opened belongs to its IEEE channel, which
	// ieee_libretro_release() closes; one it never opened is closed here
	extern bool prg_consumed;
	if (prg_file && !prg_consumed) {
		SDL_RWclose(prg_file);
	}
	prg_file = NULL;
	ieee_libretro_release();
	free(RAM);
	free(BRAM);
	RAM = BRAM = NULL;
	game_loaded = false;
}

void
retro_reset(void)
{
	machine_reset();
	if (content_path[0]) {
		queue_program();
	}
}

void
retro_run(void)
{
	update_input();

	emulator_loop(NULL);

	video_cb(video_get_framebuffer(), X16_SCREEN_WIDTH, X16_SCREEN_HEIGHT, X16_SCREEN_WIDTH * 4);

	size_t frames;
	while ((frames = audio_libretro_read(audio_out, sizeof(audio_out) / sizeof(audio_out[0]) / 2)) > 0) {
		audio_batch_cb(audio_out, frames);
	}
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

// Save states are not supported yet
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }

void *
retro_get_memory_data(unsigned id)
{
	return id == RETRO_MEMORY_SYSTEM_RAM ? RAM : NULL;
}

size_t
retro_get_memory_size(unsigned id)
{
	return id == RETRO_MEMORY_SYSTEM_RAM ? 0xa000 : 0;
}
