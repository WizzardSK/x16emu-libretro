// Commander X16 Emulator
// All rights reserved. License: 2-clause BSD
//
// Machine state serialization. Each module has one xxx_state() function that
// visits its variables with STATE_VAR/STATE_ARRAY; the same walk measures,
// saves or loads, so the three can never disagree about the layout.

#ifndef _STATE_H_
#define _STATE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
	X16_STATE_MEASURE,
	X16_STATE_SAVE,
	X16_STATE_LOAD,
} x16_state_mode;

typedef struct {
	x16_state_mode mode;
	uint8_t *data;
	size_t size;
	size_t pos;
	bool overflow;
} x16_state;

static inline void
state_raw(x16_state *s, void *p, size_t n)
{
	if (s->mode != X16_STATE_MEASURE) {
		if (s->pos + n > s->size) {
			s->overflow = true;
			return;
		}
		if (s->mode == X16_STATE_SAVE) {
			memcpy(s->data + s->pos, p, n);
		} else {
			memcpy(p, s->data + s->pos, n);
		}
	}
	s->pos += n;
}

#define STATE_VAR(s, v) state_raw((s), &(v), sizeof(v))
#define STATE_ARRAY(s, a) state_raw((s), (a), sizeof(a))

#ifdef __cplusplus
extern "C" {
#endif
void cpu_state(x16_state *s);
void memory_state(x16_state *s);
void video_state(x16_state *s);
void via_state(x16_state *s);
void i2c_state(x16_state *s);
void smc_state(x16_state *s);
void rtc_state(x16_state *s);
void vera_spi_state(x16_state *s);
void pcm_state(x16_state *s);
void psg_state(x16_state *s);
void sdcard_state(x16_state *s);
void serial_state(x16_state *s);
void joystick_state(x16_state *s);
void audio_state(x16_state *s);
void YM_state(x16_state *s);
#ifdef __cplusplus
}
#endif

#endif
