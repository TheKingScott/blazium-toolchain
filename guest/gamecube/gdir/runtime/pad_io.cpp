// MIT. The PS2 guest's pad API (pad_io.h) on GameCube controllers (libogc's
// PAD, all four ports). The shared runtime asks for PlayStation buttons;
// each is one GameCube button:
//
//	cross A, circle B, square X, triangle Y, L1 L, R1 R, select Z,
//	start Start, the d-pad as it is, the control stick as the left stick
//	(L2 and R2 have none; R1 drifts as R2 does).
//
// The control stick also works the d-pad's buttons once pushed well over
// (menus, choices): the GameCube's main input is the stick, and Dolphin's
// keyboard puts the arrow keys on it. Steering keeps the stick's travel.
//
// In a race that is A to drive, X to brake, R to drift and B or L for the
// item; in menus A accepts and B goes back; Z quits a race to the menu.
// L and R also report their analogue travel as the DualShock 2's pressure.

#include "pad_io.h"

#include "mc_io.h"

#include <gccore.h>
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
static unsigned short s_dpad[PAD_MAX];  /* the d-pad itself, without the stick */
static unsigned short s_sdir[PAD_MAX];  /* the stick's directions as d-pad buttons */
static unsigned short s_prev[PAD_MAX];
static float s_lx[PAD_MAX], s_ly[PAD_MAX];
static int s_conn[PAD_MAX];
static unsigned char s_pressure[PAD_MAX][12];
static float s_deadzone = 0.2f;
static int s_prev_cross;

#ifdef GC_AUTOPAD
/* dev: scripted presses for pad 0, "frame+frames=BUTTON,..." with BUTTON one
 * of CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 START SELECT UP DOWN LEFT
 * RIGHT (an emulator with no pad bound still gets through the menus) */
static unsigned short auto_buttons(unsigned frame)
{
	static const char *spec = GC_AUTOPAD;
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

static float axis(int v)
{
	/* the stick reaches about 100 each way; up is positive (the PS's is down) */
	float f = (float)v / 96.0f;
	return f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
}

static void read_pad(int i, u32 connected)
{
	s_prev[i] = s_btns[i];
	s_conn[i] = (connected >> i) & 1;
	if (!s_conn[i]) {
		s_btns[i] = 0;
		s_dpad[i] = 0;
		s_sdir[i] = 0;
		s_lx[i] = s_ly[i] = 0.0f;
		return;
	}
	const u16 h = PAD_ButtonsHeld(i);
	unsigned short b = 0;
	if (h & PAD_BUTTON_A) b |= B_CROSS;
	if (h & PAD_BUTTON_B) b |= B_CIRCLE;
	if (h & PAD_BUTTON_X) b |= B_SQUARE;
	if (h & PAD_BUTTON_Y) b |= B_TRIANGLE;
	if (h & PAD_TRIGGER_L) b |= B_L1;
	if (h & PAD_TRIGGER_R) b |= B_R1;
	if (h & PAD_TRIGGER_Z) b |= B_SELECT;
	if (h & PAD_BUTTON_START) b |= B_START;
	if (h & PAD_BUTTON_UP) b |= B_UP;
	if (h & PAD_BUTTON_DOWN) b |= B_DOWN;
	if (h & PAD_BUTTON_LEFT) b |= B_LEFT;
	if (h & PAD_BUTTON_RIGHT) b |= B_RIGHT;
	/* the analogue triggers press L1 / R1 most of the way down too */
	const int tl = PAD_TriggerL(i), tr = PAD_TriggerR(i);
	if (tl > 170) b |= B_L1;
	if (tr > 170) b |= B_R1;
	s_dpad[i] = b & (B_UP | B_DOWN | B_LEFT | B_RIGHT);
	s_lx[i] = axis(PAD_StickX(i));
	s_ly[i] = -axis(PAD_StickY(i));
	/* the stick as d-pad buttons: on past 0.6, off under 0.4 */
	const float dir[4] = { -s_ly[i], s_ly[i], -s_lx[i], s_lx[i] };
	const unsigned short bit[4] = { B_UP, B_DOWN, B_LEFT, B_RIGHT };
	for (int k = 0; k < 4; k++) {
		if (dir[k] > 0.6f) {
			s_sdir[i] |= bit[k];
		} else if (dir[k] < 0.4f) {
			s_sdir[i] &= (unsigned short)~bit[k];
		}
	}
	b |= s_sdir[i];
	s_btns[i] = b;
	memset(s_pressure[i], 0, sizeof(s_pressure[i]));
	s_pressure[i][8] = (unsigned char)tl;
	s_pressure[i][9] = (unsigned char)tr;
	s_pressure[i][6] = (b & B_CROSS) ? 255 : 0;
	s_pressure[i][7] = (b & B_SQUARE) ? 255 : 0;
}

int pad_io_init(void)
{
	PAD_Init();
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
	const u32 connected = PAD_ScanPads();
	for (int i = 0; i < PAD_MAX; i++) {
		read_pad(i, connected);
	}
#ifdef GC_AUTOPAD
	{
		/* GC_AUTOPADS pads (1 if unset) all get the script */
#ifndef GC_AUTOPADS
#define GC_AUTOPADS 1
#endif
		const unsigned short ab = auto_buttons(s_auto_frame++);
		for (int i = 0; i < GC_AUTOPADS && i < PAD_MAX; i++) {
			s_btns[i] |= ab;
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
	PAD_ControlMotor(0, (small_on || large > 0) ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
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
	/* held: the stick's directions are left out, so Input.get_axis and
	 * get_action_strength read its travel, not full lock (the presses,
	 * just_pressed, keep them for menus) */
	const unsigned short m = action_mask(action);
	const unsigned short held = (unsigned short)(s_btns[0] & ~(s_sdir[0] & ~s_dpad[0]));
	return (s_ready && m && (held & m)) ? 1 : 0;
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
		/* the d-pad itself (not the stick's own directions, which would
		 * turn its travel into full lock) */
		if (s_dpad[0] & B_LEFT) {
			sx = -1.0f;
		}
		if (s_dpad[0] & B_RIGHT) {
			sx = 1.0f;
		}
		if (s_dpad[0] & B_UP) {
			sy = -1.0f;
		}
		if (s_dpad[0] & B_DOWN) {
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
	if (s_dpad[pad] & B_LEFT) {
		return -1.0f;
	}
	if (s_dpad[pad] & B_RIGHT) {
		return 1.0f;
	}
	const float x = s_lx[pad];
	return (x > s_deadzone || x < -s_deadzone) ? x : 0.0f;
}
