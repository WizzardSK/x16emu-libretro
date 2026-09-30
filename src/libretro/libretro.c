// Commander X16 Emulator - libretro core
// All rights reserved. License: 2-clause BSD
//
// Drives the emulator one video frame per retro_run(): emulator_loop() returns
// after each completed VERA frame when built with __LIBRETRO__, the same way
// the WebAssembly build yields to the browser.

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "miniz.h"

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
#include "../state.h"
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
extern char *keymaps[];
extern bool set_system_time;
extern bool enable_midline;
extern bool ym2151_irq_support;
extern uint8_t keymap;
extern int instruction_counter;
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
static char save_dir[PATH_MAX];
static char zip_dir[PATH_MAX];
static char zip_pick[PATH_MAX];
static char content_dir[PATH_MAX];
static char content_path[PATH_MAX];
static bool game_loaded;
static bool supports_bitmasks;
static int16_t audio_out[4096 * 2];
static bool mouse_enabled = true;
static size_t state_size;

#define NUM_KEYMAPS 28

// ---- Core options -------------------------------------------------------------

static struct retro_core_option_v2_category option_cats[] = {
	{ "system", "System", "CPU, memory and clock of the emulated machine." },
	{ "input", "Input", "Keyboard layout and mouse." },
	{ "video_audio", "Video & Audio", "Accuracy settings for VERA and the YM2151." },
	{ NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition option_defs[] = {
	{ "x16_cpu_type", "CPU (Restart)", "CPU", "65C02 as shipped, or the 65C816 upgrade.", NULL, "system",
		{ { "65C02", NULL }, { "65C816", NULL }, { NULL, NULL } }, "65C02" },
	{ "x16_cpu_speed", "CPU Speed (Restart)", "CPU Speed", "8 MHz is the real machine; faster clocks run software faster.", NULL, "system",
		{ { "8", "8 MHz" }, { "1", "1 MHz" }, { "2", "2 MHz" }, { "4", "4 MHz" }, { "10", "10 MHz" }, { "12", "12 MHz" },
		  { "16", "16 MHz" }, { "20", "20 MHz" }, { "32", "32 MHz" }, { "40", "40 MHz" }, { NULL, NULL } }, "8" },
	{ "x16_ram_size", "Banked RAM (Restart)", "Banked RAM", "High RAM at $A000-$BFFF. 512 KB is the standard machine, 2 MB the maximum.", NULL, "system",
		{ { "512", "512 KB" }, { "64", "64 KB" }, { "128", "128 KB" }, { "256", "256 KB" }, { "1024", "1 MB" },
		  { "1536", "1.5 MB" }, { "2048", "2 MB" }, { NULL, NULL } }, "512" },
	{ "x16_rtc_host_time", "Set Clock from Host (Restart)", "Set Clock from Host", "Start the real-time clock at the host's local time instead of stopped at 2000-01-01.", NULL, "system",
		{ { "enabled", NULL }, { "disabled", NULL }, { NULL, NULL } }, "enabled" },
	{ "x16_keymap", "Keyboard Layout (Restart)", "Keyboard Layout", "The layout the KERNAL uses to read the keyboard; match it to the host keyboard.", NULL, "input",
		{ { NULL, NULL } }, "en-us" },
	{ "x16_mouse", "Mouse", NULL, "Pass the host mouse to the X16.", NULL, "input",
		{ { "enabled", NULL }, { "disabled", NULL }, { NULL, NULL } }, "enabled" },
	{ "x16_midline", "Mid-line Effects", NULL, "Emulate VERA register changes in the middle of a scanline. More accurate, slower.", NULL, "video_audio",
		{ { "disabled", NULL }, { "enabled", NULL }, { NULL, NULL } }, "disabled" },
	{ "x16_ym2151_irq", "YM2151 IRQ", NULL, "Deliver YM2151 timer interrupts to the CPU. Only needed by software that uses them; costs speed.", NULL, "video_audio",
		{ { "disabled", NULL }, { "enabled", NULL }, { NULL, NULL } }, "disabled" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static void
set_core_options(void)
{
	// Keyboard layouts come from the table the KERNAL numbers them by
	for (int i = 0; option_defs[i].key; i++) {
		if (!strcmp(option_defs[i].key, "x16_keymap")) {
			for (int k = 0; k < NUM_KEYMAPS && k < RETRO_NUM_CORE_OPTION_VALUES_MAX - 1; k++) {
				option_defs[i].values[k].value = keymaps[k];
				option_defs[i].values[k].label = NULL;
			}
		}
	}

	unsigned version = 0;
	if (environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
		struct retro_core_options_v2 opts = { option_cats, option_defs };
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts);
		return;
	}

	// Old frontends: "Description; default|other|..."
	static struct retro_variable vars[sizeof(option_defs) / sizeof(option_defs[0])];
	static char descs[sizeof(option_defs) / sizeof(option_defs[0])][1024];
	int n = 0;
	for (int i = 0; option_defs[i].key; i++, n++) {
		const struct retro_core_option_v2_definition *d = &option_defs[i];
		char *out = descs[i];
		size_t len = snprintf(out, sizeof(descs[i]), "%s; %s", d->desc, d->default_value);
		for (int v = 0; d->values[v].value && len < sizeof(descs[i]); v++) {
			if (strcmp(d->values[v].value, d->default_value)) {
				len += snprintf(out + len, sizeof(descs[i]) - len, "|%s", d->values[v].value);
			}
		}
		vars[n].key = d->key;
		vars[n].value = out;
	}
	vars[n].key = NULL;
	vars[n].value = NULL;
	environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
}

static const char *
get_option(const char *key)
{
	struct retro_variable var = { key, NULL };
	return environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) ? var.value : NULL;
}

// Options that take effect at once
static void
apply_runtime_options(void)
{
	const char *v;
	if ((v = get_option("x16_mouse"))) {
		mouse_enabled = strcmp(v, "disabled") != 0;
	}
	if ((v = get_option("x16_midline"))) {
		enable_midline = !strcmp(v, "enabled");
	}
	if ((v = get_option("x16_ym2151_irq"))) {
		ym2151_irq_support = !strcmp(v, "enabled");
	}
}

// Options read when a game is loaded (the machine is built with them)
static void
apply_boot_options(void)
{
	const char *v;
	regs.is65c816 = (v = get_option("x16_cpu_type")) && !strcmp(v, "65C816");
	is_gen2 = false;

	MHZ = 8;
	if ((v = get_option("x16_cpu_speed"))) {
		int mhz = atoi(v);
		if (mhz >= 1 && mhz <= 40) {
			MHZ = (uint8_t)mhz;
		}
	}

	num_banks = 1;
	num_ram_banks = 64;
	if ((v = get_option("x16_ram_size"))) {
		int kb = atoi(v);
		if (kb >= 8 && kb <= 2048 && (kb & 7) == 0) {
			num_ram_banks = kb / 8;
		}
	}

	set_system_time = !(v = get_option("x16_rtc_host_time")) || strcmp(v, "disabled") != 0;

	keymap = 0;
	if ((v = get_option("x16_keymap"))) {
		for (int k = 0; k < NUM_KEYMAPS; k++) {
			if (!strcmp(v, keymaps[k])) {
				keymap = k;
			}
		}
	}

	apply_runtime_options();
}

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

	if (!mouse_enabled) {
		return;
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
	info->valid_extensions = "img|prg|bas|crt|zip";
	info->need_fullpath    = true;   // SD card images are read and written in place
	info->block_extract    = true;
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

	set_core_options();
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
	dir = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir) {
		snprintf(save_dir, sizeof(save_dir), "%s", dir);
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
static void serialize_machine(x16_state *s);

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

// ---- .zip content --------------------------------------------------------------
// X16 software is often a ZIP of a PRG and the data files it loads at run time
// (the frontend's own archive support hands over one file only). The archive is
// extracted once to <save dir>/x16emu/<zip name>/, which then serves as the
// host file system, so files a game writes there (saves, high scores, an SD
// card image) survive to the next start. It is extracted again only when the
// ZIP itself changes.

static bool
make_dir(const char *path)
{
#ifdef _WIN32
	return _mkdir(path) == 0 || errno == EEXIST;
#else
	return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

// Create every directory on the way to path (path itself included)
static bool
make_dirs(const char *path)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/' || *p == '\\') {
			char c = *p;
			*p = 0;
			make_dir(tmp);
			*p = c;
		}
	}
	return make_dir(tmp);
}

// Entry names that would land outside the extraction directory are skipped
static bool
safe_zip_name(const char *name)
{
	if (!name[0] || name[0] == '/' || name[0] == '\\' || strchr(name, ':')) {
		return false;
	}
	for (const char *p = name; *p; ) {
		const char *end = p + strcspn(p, "/\\");
		if (end - p == 2 && p[0] == '.' && p[1] == '.') {
			return false;
		}
		p = *end ? end + 1 : end;
	}
	return true;
}

static bool
extract_zip(const char *zip, const char *dest)
{
	struct stat st;
	char stamp[64] = "", marker[PATH_MAX + 32], old[64] = "";
	if (stat(zip, &st) == 0) {
		snprintf(stamp, sizeof(stamp), "%lld %lld", (long long)st.st_size, (long long)st.st_mtime);
	}
	snprintf(marker, sizeof(marker), "%s/.x16emu-zip", dest);
	FILE *m = fopen(marker, "rb");
	if (m) {
		size_t n = fread(old, 1, sizeof(old) - 1, m);
		old[n] = 0;
		fclose(m);
		if (stamp[0] && !strcmp(old, stamp)) {
			log_cb(RETRO_LOG_INFO, "[x16] using %s, extracted earlier\n", dest);
			return true;
		}
	}

	mz_zip_archive za;
	memset(&za, 0, sizeof(za));
	if (!mz_zip_reader_init_file(&za, zip, 0)) {
		log_cb(RETRO_LOG_ERROR, "[x16] cannot open %s as a ZIP archive\n", zip);
		return false;
	}
	make_dirs(dest);
	bool ok = true;
	mz_uint n = mz_zip_reader_get_num_files(&za);
	for (mz_uint i = 0; i < n && ok; i++) {
		mz_zip_archive_file_stat fs;
		if (!mz_zip_reader_file_stat(&za, i, &fs)) {
			continue;
		}
		if (!safe_zip_name(fs.m_filename)) {
			log_cb(RETRO_LOG_WARN, "[x16] skipping ZIP entry %s\n", fs.m_filename);
			continue;
		}
		char out[PATH_MAX + 512];
		snprintf(out, sizeof(out), "%s/%s", dest, fs.m_filename);
		for (char *c = out + strlen(dest); *c; c++) {
			if (*c == '\\') {
				*c = '/';
			}
		}
		if (mz_zip_reader_is_file_a_directory(&za, i)) {
			make_dirs(out);
			continue;
		}
		char *slash = strrchr(out, '/');
		if (slash) {
			*slash = 0;
			make_dirs(out);
			*slash = '/';
		}
		if (!mz_zip_reader_extract_to_file(&za, i, out, 0)) {
			log_cb(RETRO_LOG_ERROR, "[x16] cannot extract %s\n", fs.m_filename);
			ok = false;
		}
	}
	mz_zip_reader_end(&za);

	if (ok && (m = fopen(marker, "wb"))) {
		fputs(stamp, m);
		fclose(m);
	}
	log_cb(RETRO_LOG_INFO, "[x16] extracted %u entries of %s to %s\n", (unsigned)n, zip, dest);
	return ok;
}

// What to start from an extracted archive, in this order: an SD card image,
// AUTOBOOT.X16 (the KERNAL runs it from the host file system by itself), a PRG
// (preferably one named like the archive), a BASIC listing, a cartridge. A
// single top-level folder holding everything is looked into.
static bool
pick_zip_content(const char *dir, const char *zip_base, char *out, size_t out_size)
{
	char cur[PATH_MAX];
	snprintf(cur, sizeof(cur), "%s", dir);
	size_t bl = strlen(zip_base);
	for (int depth = 0; depth < 4; depth++) {
		DIR *d = opendir(cur);
		if (!d) {
			return false;
		}
		char best[5][256] = { "", "", "", "", "" };
		char named_prg[256] = "";
		char subdir[256] = "";
		int subdirs = 0;
		struct dirent *e;
		while ((e = readdir(d))) {
			const char *name = e->d_name;
			if (name[0] == '.') {
				continue;
			}
			char full[PATH_MAX + 256];
			struct stat st;
			snprintf(full, sizeof(full), "%s/%s", cur, name);
			if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
				subdirs++;
				snprintf(subdir, sizeof(subdir), "%s", name);
				continue;
			}
			int kind = has_extension(name, "img") ? 0
				: !strcasecmp(name, "AUTOBOOT.X16") ? 1
				: has_extension(name, "prg") ? 2
				: has_extension(name, "bas") ? 3
				: has_extension(name, "crt") ? 4 : -1;
			if (kind < 0) {
				continue;
			}
			if (kind == 2 && !strncasecmp(name, zip_base, bl) && name[bl] == '.') {
				snprintf(named_prg, sizeof(named_prg), "%s", name);
			}
			// readdir order is arbitrary; take the alphabetically first
			if (!best[kind][0] || strcasecmp(name, best[kind]) < 0) {
				snprintf(best[kind], sizeof(best[kind]), "%s", name);
			}
		}
		closedir(d);
		if (named_prg[0]) {
			snprintf(best[2], sizeof(best[2]), "%s", named_prg);
		}
		for (int k = 0; k < 5; k++) {
			if (best[k][0]) {
				snprintf(out, out_size, "%s/%s", cur, best[k]);
				return true;
			}
		}
		if (subdirs != 1) {
			return false;
		}
		size_t len = strlen(cur);
		snprintf(cur + len, sizeof(cur) - len, "/%s", subdir);
	}
	return false;
}

// Where a content path names a ZIP archive, the length of the archive's own
// path; 0 otherwise. Frontends name a file picked inside an archive as
// "archive.zip#dir/file".
static size_t
zip_path_length(const char *path)
{
	for (const char *p = path; (p = strchr(p, '.')); p++) {
		if (!strncasecmp(p, ".zip", 4) && (!p[4] || p[4] == '#')) {
			return p + 4 - path;
		}
	}
	return 0;
}

// Turns a ZIP content path into the file to start; NULL when nothing usable
// is inside
static const char *
open_zip_content(const char *content)
{
	char zip[PATH_MAX];
	size_t len = zip_path_length(content);
	snprintf(zip, sizeof(zip), "%.*s", (int)len, content);
	const char *inner = content[len] == '#' ? content + len + 1 : NULL;

	const char *base = strrchr(zip, '/');
#ifdef _WIN32
	const char *bbase = strrchr(zip, '\\');
	if (bbase > base) {
		base = bbase;
	}
#endif
	base = base ? base + 1 : zip;
	char name[256];
	snprintf(name, sizeof(name), "%s", base);
	name[strlen(name) - 4] = 0;

	const char *root = save_dir[0] ? save_dir : system_dir;
	snprintf(zip_dir, sizeof(zip_dir), "%s/x16emu/%s", root, name);
	if (!extract_zip(zip, zip_dir)) {
		return NULL;
	}
	if (inner && *inner && safe_zip_name(inner)) {
		struct stat st;
		snprintf(zip_pick, sizeof(zip_pick), "%s/%s", zip_dir, inner);
		for (char *c = zip_pick + strlen(zip_dir); *c; c++) {
			if (*c == '\\') {
				*c = '/';
			}
		}
		if (stat(zip_pick, &st) == 0 && !S_ISDIR(st.st_mode)) {
			log_cb(RETRO_LOG_INFO, "[x16] starting %s\n", zip_pick);
			return zip_pick;
		}
		log_cb(RETRO_LOG_WARN, "[x16] %s is not in %s\n", inner, zip);
	}
	if (!pick_zip_content(zip_dir, name, zip_pick, sizeof(zip_pick))) {
		log_cb(RETRO_LOG_ERROR, "[x16] %s holds no .img, AUTOBOOT.X16, .prg, .bas or .crt\n", zip);
		return NULL;
	}
	log_cb(RETRO_LOG_INFO, "[x16] starting %s\n", zip_pick);
	return zip_pick;
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

	apply_boot_options();

	content_path[0] = 0;
	headless = false;
	using_hostfs = true;
	prg_file = NULL;
	prg_done = false;
	run_after_load = false;
	paste_text = NULL;
	cartridge_path = NULL;

	const char *path = game ? game->path : NULL;
	if (path && zip_path_length(path) && !(path = open_zip_content(path))) {
		return false;
	}
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

		if (!strcasecmp(path + strlen(content_dir) + 1, "AUTOBOOT.X16")) {
			// Nothing to queue: the KERNAL loads and runs AUTOBOOT.X16 from
			// the host file system on its own
		} else if (has_extension(path, "img")) {
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
	rtc_init(set_system_time);
	machine_reset();
	timing_init();

	x16_state measure = { X16_STATE_MEASURE };
	serialize_machine(&measure);
	state_size = measure.pos;

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
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		apply_runtime_options();
	}

	update_input();

	emulator_loop(NULL);

	video_cb(video_get_framebuffer(), X16_SCREEN_WIDTH, X16_SCREEN_HEIGHT, X16_SCREEN_WIDTH * 4);

	size_t frames;
	while ((frames = audio_libretro_read(audio_out, sizeof(audio_out) / sizeof(audio_out[0]) / 2)) > 0) {
		audio_batch_cb(audio_out, frames);
	}
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

// ---- Save states ----------------------------------------------------------------
// Version 1: header, then every module's state in a fixed order

#define X16_STATE_MAGIC 0x53363158u   // "X16S"
#define X16_STATE_VERSION 1u

static void
serialize_header(x16_state *s)
{
	uint32_t magic = X16_STATE_MAGIC, version = X16_STATE_VERSION;
	uint16_t ram_banks = num_ram_banks, banks = num_banks;
	uint8_t cpu816 = regs.is65c816, cart = CART != NULL;
	STATE_VAR(s, magic);
	STATE_VAR(s, version);
	STATE_VAR(s, ram_banks);
	STATE_VAR(s, banks);
	STATE_VAR(s, cpu816);
	STATE_VAR(s, cart);
}

static void
serialize_machine(x16_state *s)
{
	serialize_header(s);
	cpu_state(s);
	memory_state(s);
	video_state(s);
	via_state(s);
	i2c_state(s);
	smc_state(s);
	rtc_state(s);
	vera_spi_state(s);
	pcm_state(s);
	psg_state(s);
	sdcard_state(s);
	serial_state(s);
	joystick_state(s);
	audio_state(s);
	YM_state(s);
	STATE_VAR(s, instruction_counter);
	STATE_VAR(s, MHZ);
}

size_t
retro_serialize_size(void)
{
	return state_size;
}

bool
retro_serialize(void *data, size_t size)
{
	if (!game_loaded || size < state_size) {
		return false;
	}
	x16_state s = { X16_STATE_SAVE, (uint8_t *)data, size };
	serialize_machine(&s);
	return !s.overflow;
}

bool
retro_unserialize(const void *data, size_t size)
{
	if (!game_loaded || size < state_size) {
		return false;
	}
	// Check the header against this machine before touching anything
	x16_state check = { X16_STATE_SAVE, (uint8_t *)malloc(state_size), state_size };
	serialize_header(&check);
	bool same = !memcmp(check.data, data, check.pos);
	free(check.data);
	if (!same) {
		log_cb(RETRO_LOG_ERROR, "[x16] save state is from a different machine configuration (CPU, RAM, cartridge)\n");
		return false;
	}
	x16_state s = { X16_STATE_LOAD, (uint8_t *)data, size };
	serialize_machine(&s);
	return !s.overflow;
}
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }

// The RTC's battery-backed NVRAM (KERNAL settings) is the save RAM, which
// the frontend keeps in a .srm file next to each game's saves
void *
retro_get_memory_data(unsigned id)
{
	switch (id) {
		case RETRO_MEMORY_SAVE_RAM: return nvram;
		case RETRO_MEMORY_SYSTEM_RAM: return RAM;
		default: return NULL;
	}
}

size_t
retro_get_memory_size(unsigned id)
{
	switch (id) {
		case RETRO_MEMORY_SAVE_RAM: return sizeof(nvram);
		case RETRO_MEMORY_SYSTEM_RAM: return RAM ? 0xa000 : 0;
		default: return 0;
	}
}
