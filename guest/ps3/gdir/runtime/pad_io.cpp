// MIT. The PS2 guest's pad API (pad_io.h) on the PS3's controllers (libio's
// ioPad, four ports): the DualShock 3 has every PS2 button, so each one is
// itself, the sticks are the sticks, and the face buttons, d-pad and
// shoulders report their pressure as the DualShock 2's do (pad_io_get_pressure,
// libpad's order). Player 1 is the first controller the system assigned.

#include "pad_io.h"

#include "mc_io.h"

#include <io/pad.h>
#include <string.h>

#define PAD_MAX 4

/* the PS2's button bits (libpad's), so the tables match the other guests */
enum {
	B_SELECT = 0x0001, B_L3 = 0x0002, B_R3 = 0x0004, B_START = 0x0008,
	B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080,
	B_L2 = 0x0100, B_R2 = 0x0200, B_L1 = 0x0400, B_R1 = 0x0800,
	B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000,
};

static int s_ready;
static unsigned short s_btns[PAD_MAX];
static unsigned short s_raw[PAD_MAX]; /* the pad's own, as last reported */
static unsigned short s_prev[PAD_MAX];
static float s_lx[PAD_MAX], s_ly[PAD_MAX];
static int s_conn[PAD_MAX];
static int s_press_on[PAD_MAX];
static unsigned char s_pressure[PAD_MAX][12];
static float s_deadzone = 0.2f;
static int s_prev_cross;

#ifdef PS3_AUTOPAD
/* dev: scripted presses for pad 0, "frame+frames=BUTTON,..." with BUTTON one
 * of CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 START SELECT UP DOWN LEFT
 * RIGHT (an emulator with no pad bound still gets through the menus) */
static unsigned short auto_buttons(unsigned frame)
{
	static const char *spec = PS3_AUTOPAD;
	static const struct {
		const char *n;
		unsigned short m;
	} k[] = { { "CROSS", B_CROSS }, { "CIRCLE", B_CIRCLE }, { "SQUARE", B_SQUARE }, { "TRIANGLE", B_TRIANGLE },
		{ "L1", B_L1 }, { "R1", B_R1 }, { "L2", B_L2 }, { "R2", B_R2 }, { "START", B_START }, { "SELECT", B_SELECT },
		{ "UP", B_UP }, { "DOWN", B_DOWN }, { "LEFT", B_LEFT }, { "RIGHT", B_RIGHT } };
	unsigned short out = 0;
	const char *p = spec;
	while (*p) {
		unsigned f0 = 0, n = 0;
		while (*p >= '0' && *p <= '9') {
			f0 = f0 * 10 + (unsigned)(*p++ - '0');
		}
		if (*p == '+') {
			p++;
			while (*p >= '0' && *p <= '9') {
				n = n * 10 + (unsigned)(*p++ - '0');
			}
		}
		if (*p == '=') {
			p++;
		}
		const char *name = p;
		while (*p && *p != ',') {
			p++;
		}
		const size_t len = (size_t)(p - name);
		if (frame >= f0 && frame < f0 + (n ? n : 1)) {
			for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
				if (strlen(k[i].n) == len && strncmp(k[i].n, name, len) == 0) {
					out |= k[i].m;
				}
			}
		}
		if (*p == ',') {
			p++;
		}
	}
	return out;
}
static unsigned s_auto_frame;
#endif

static float stick(unsigned v)
{
	/* 0..255, 128 in the middle; down and right positive, as the PS2's */
	float f = ((float)(int)(v & 0xff) - 128.0f) / 127.0f;
	return f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
}

static void read_pad(int i, const padInfo *info)
{
	s_prev[i] = s_btns[i];
	s_conn[i] = info->status[i] ? 1 : 0;
	if (!s_conn[i]) {
		s_raw[i] = 0;
		s_btns[i] = 0;
		s_lx[i] = s_ly[i] = 0.0f;
		s_press_on[i] = 0;
		return;
	}
	if (!s_press_on[i]) {
		ioPadSetPressMode((u32)i, PAD_PRESS_MODE_ON);
		s_press_on[i] = 1;
	}
	padData d;
	memset(&d, 0, sizeof(d));
	if (ioPadGetData((u32)i, &d) != 0 || d.len <= 0) {
		s_btns[i] = s_raw[i]; /* nothing new: as it was */
		return;
	}
	unsigned short b = 0;
	if (d.BTN_CROSS) b |= B_CROSS;
	if (d.BTN_CIRCLE) b |= B_CIRCLE;
	if (d.BTN_SQUARE) b |= B_SQUARE;
	if (d.BTN_TRIANGLE) b |= B_TRIANGLE;
	if (d.BTN_L1) b |= B_L1;
	if (d.BTN_R1) b |= B_R1;
	if (d.BTN_L2) b |= B_L2;
	if (d.BTN_R2) b |= B_R2;
	if (d.BTN_L3) b |= B_L3;
	if (d.BTN_R3) b |= B_R3;
	if (d.BTN_START) b |= B_START;
	if (d.BTN_SELECT) b |= B_SELECT;
	if (d.BTN_UP) b |= B_UP;
	if (d.BTN_DOWN) b |= B_DOWN;
	if (d.BTN_LEFT) b |= B_LEFT;
	if (d.BTN_RIGHT) b |= B_RIGHT;
	s_raw[i] = b;
	s_btns[i] = b;
	s_lx[i] = stick(d.ANA_L_H);
	s_ly[i] = stick(d.ANA_L_V);
	/* libpad's pressure order: right left up down triangle circle cross
	 * square l1 r1 l2 r2 */
	if (d.len >= 24) {
		const unsigned p[12] = { d.PRE_RIGHT, d.PRE_LEFT, d.PRE_UP, d.PRE_DOWN, d.PRE_TRIANGLE, d.PRE_CIRCLE,
			d.PRE_CROSS, d.PRE_SQUARE, d.PRE_L1, d.PRE_R1, d.PRE_L2, d.PRE_R2 };
		for (int k = 0; k < 12; k++) {
			s_pressure[i][k] = (unsigned char)(p[k] > 255 ? 255 : p[k]);
		}
	}
}

int pad_io_init(void)
{
	ioPadInit(7);
	mc_io_boot();
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
	padInfo info;
	memset(&info, 0, sizeof(info));
	ioPadGetInfo(&info);
	for (int i = 0; i < PAD_MAX; i++) {
		read_pad(i, &info);
	}
#ifdef PS3_AUTOPAD
	{
		/* PS3_AUTOPADS pads (1 if unset) all get the script */
#ifndef PS3_AUTOPADS
#define PS3_AUTOPADS 1
#endif
		const unsigned short ab = auto_buttons(s_auto_frame++);
		for (int i = 0; i < PS3_AUTOPADS && i < PAD_MAX; i++) {
			s_btns[i] = (unsigned short)(s_conn[i] ? (s_btns[i] | ab) : ab);
			s_conn[i] = 1;
		}
	}
#endif
	const int cross = (s_btns[0] & B_CROSS) ? 1 : 0;
	if (cross_down) {
		*cross_down = cross && !s_prev_cross;
	}
	s_prev_cross = cross;
}

void pad_io_set_rumble(int small_on, int large)
{
	if (!s_ready || !s_conn[0]) {
		return;
	}
	padActParam act;
	memset(&act, 0, sizeof(act));
	act.small_motor = small_on ? 1 : 0;
	act.large_motor = (u8)(large < 0 ? 0 : (large > 255 ? 255 : large));
	ioPadSetActDirect(0, &act);
}

int pad_io_get_pressure(int button)
{
	if (!s_ready || button < 0 || button >= 12) {
		return 0;
	}
	return (int)s_pressure[0][button];
}

static unsigned short action_mask(int action)
{
	static const unsigned short k[] = {
		B_LEFT, B_RIGHT, B_UP, B_DOWN, B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE,
		B_L1, B_R1, B_L2, B_R2, B_START, B_SELECT,
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

void pad_io_stick(int stick_id, float *x, float *y)
{
	float sx = 0.0f, sy = 0.0f;
	if (stick_id == 0) {
		sx = s_lx[0];
		sy = s_ly[0];
		if (s_btns[0] & B_LEFT) {
			sx = -1.0f;
		}
		if (s_btns[0] & B_RIGHT) {
			sx = 1.0f;
		}
		if (s_btns[0] & B_UP) {
			sy = -1.0f;
		}
		if (s_btns[0] & B_DOWN) {
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
		{ "cross", B_CROSS }, { "circle", B_CIRCLE }, { "square", B_SQUARE }, { "triangle", B_TRIANGLE },
		{ "l1", B_L1 }, { "r1", B_R1 }, { "l2", B_L2 }, { "r2", B_R2 }, { "l3", B_L3 }, { "r3", B_R3 },
		{ "start", B_START }, { "select", B_SELECT },
		{ "up", B_UP }, { "down", B_DOWN }, { "left", B_LEFT }, { "right", B_RIGHT },
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
		if (s_conn[i]) {
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
	if (s_btns[pad] & B_LEFT) {
		return -1.0f;
	}
	if (s_btns[pad] & B_RIGHT) {
		return 1.0f;
	}
	const float x = s_lx[pad];
	return (x > s_deadzone || x < -s_deadzone) ? x : 0.0f;
}
