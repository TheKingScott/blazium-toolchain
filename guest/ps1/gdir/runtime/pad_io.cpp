// MIT. The PS2 guest's pad API (pad_io.h) on the PS1 BIOS pad driver: two
// ports, digital or analog (DualShock) pads. The button bits are the same as
// the PS2's, so the shared runtime's actions map one to one.

#include "pad_io.h"

#include <psxapi.h>
#include <psxpad.h>
#include <string.h>

#define PAD_MAX 2

static uint8_t s_buf[2][34];
static int s_ready;
static unsigned short s_btns[PAD_MAX];
static unsigned short s_prev[PAD_MAX];
static float s_lx[PAD_MAX], s_ly[PAD_MAX], s_rx, s_ry;
static float s_deadzone = 0.2f;
static int s_prev_cross;

static void read_pad(int i)
{
	const PADTYPE *p = (const PADTYPE *)s_buf[i];
	s_prev[i] = s_btns[i];
	if (p->stat != 0) {
		s_btns[i] = 0;
		s_lx[i] = s_ly[i] = 0.0f;
		return;
	}
	s_btns[i] = (unsigned short)~p->btn;
	s_lx[i] = s_ly[i] = 0.0f;
	if (p->type == PAD_ID_ANALOG || p->type == PAD_ID_ANALOG_STICK) {
		s_lx[i] = ((int)p->ls_x - 128) / 128.0f;
		s_ly[i] = ((int)p->ls_y - 128) / 128.0f;
		if (i == 0) {
			s_rx = ((int)p->rs_x - 128) / 128.0f;
			s_ry = ((int)p->rs_y - 128) / 128.0f;
		}
	}
}

int pad_io_init(void)
{
	memset(s_buf, 0xff, sizeof(s_buf));
	InitPAD(s_buf[0], 34, s_buf[1], 34);
	StartPAD();
	ChangeClearPAD(0);
	s_ready = 1;
	return 1;
}

void pad_io_poll(float *yaw, float *dolly, int *cross_down)
{
	if (yaw) {
		*yaw = 0.0f;
	}
	if (dolly) {
		*dolly = 0.0f;
	}
	for (int i = 0; i < PAD_MAX; i++) {
		read_pad(i);
	}
	const int cross = (s_btns[0] & PAD_CROSS) ? 1 : 0;
	if (cross_down) {
		*cross_down = cross && !s_prev_cross;
	}
	s_prev_cross = cross;
}

void pad_io_set_rumble(int small_on, int large)
{
	(void)small_on;
	(void)large;
}

int pad_io_get_pressure(int button)
{
	(void)button;
	return 0;
}

static unsigned short action_mask(int action)
{
	static const unsigned short k[] = {
		PAD_LEFT, PAD_RIGHT, PAD_UP, PAD_DOWN, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE,
		PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_START, PAD_SELECT,
	};
	return (action >= 0 && action < (int)(sizeof(k) / sizeof(k[0]))) ? k[action] : 0;
}

int pad_io_pressed(int action)
{
	const unsigned short m = action_mask(action);
	return (s_ready && m && (s_btns[0] & m)) ? 1 : 0;
}

int pad_io_just_pressed(int action)
{
	const unsigned short m = action_mask(action);
	return (s_ready && m && (s_btns[0] & m) && !(s_prev[0] & m)) ? 1 : 0;
}

int pad_io_just_released(int action)
{
	const unsigned short m = action_mask(action);
	return (s_ready && m && !(s_btns[0] & m) && (s_prev[0] & m)) ? 1 : 0;
}

void pad_io_stick(int stick, float *x, float *y)
{
	float sx = 0.0f, sy = 0.0f;
	if (stick == 1) {
		sx = s_rx;
		sy = s_ry;
	} else {
		sx = s_lx[0];
		sy = s_ly[0];
		if (s_btns[0] & PAD_LEFT) {
			sx = -1.0f;
		}
		if (s_btns[0] & PAD_RIGHT) {
			sx = 1.0f;
		}
		if (s_btns[0] & PAD_UP) {
			sy = -1.0f;
		}
		if (s_btns[0] & PAD_DOWN) {
			sy = 1.0f;
		}
	}
	if (sx * sx + sy * sy < s_deadzone * s_deadzone) {
		sx = sy = 0.0f;
	}
	if (x) {
		*x = sx;
	}
	if (y) {
		*y = sy;
	}
}

void pad_io_set_deadzone(float zone)
{
	s_deadzone = zone < 0.0f ? 0.0f : (zone > 0.95f ? 0.95f : zone);
}

float pad_io_get_deadzone(void)
{
	return s_deadzone;
}

static unsigned short button_mask(const char *name)
{
	static const struct {
		const char *name;
		unsigned short mask;
	} k[] = {
		{ "cross", PAD_CROSS }, { "circle", PAD_CIRCLE }, { "square", PAD_SQUARE }, { "triangle", PAD_TRIANGLE },
		{ "l1", PAD_L1 }, { "r1", PAD_R1 }, { "l2", PAD_L2 }, { "r2", PAD_R2 }, { "l3", PAD_L3 }, { "r3", PAD_R3 },
		{ "start", PAD_START }, { "select", PAD_SELECT },
		{ "up", PAD_UP }, { "down", PAD_DOWN }, { "left", PAD_LEFT }, { "right", PAD_RIGHT },
	};
	for (unsigned i = 0; name && i < sizeof(k) / sizeof(k[0]); i++) {
		if (strcmp(name, k[i].name) == 0) {
			return k[i].mask;
		}
	}
	return 0;
}

int pad_io_count(void)
{
	int n = 0;
	for (int i = 0; i < PAD_MAX; i++) {
		if (((const PADTYPE *)s_buf[i])->stat == 0) {
			n = i + 1;
		}
	}
	return n;
}

int pad_io_pad_pressed(int pad, const char *button)
{
	const unsigned short m = button_mask(button);
	return (pad >= 0 && pad < PAD_MAX && m && (s_btns[pad] & m)) ? 1 : 0;
}

int pad_io_pad_just_pressed(int pad, const char *button)
{
	const unsigned short m = button_mask(button);
	return (pad >= 0 && pad < PAD_MAX && m && (s_btns[pad] & m) && !(s_prev[pad] & m)) ? 1 : 0;
}

float pad_io_pad_axis(int pad)
{
	if (pad < 0 || pad >= PAD_MAX) {
		return 0.0f;
	}
	if (s_btns[pad] & PAD_LEFT) {
		return -1.0f;
	}
	if (s_btns[pad] & PAD_RIGHT) {
		return 1.0f;
	}
	const float x = s_lx[pad];
	return (x > s_deadzone || x < -s_deadzone) ? x : 0.0f;
}
