// MIT. The PS2 guest's pad API (pad_io.h) on the Switch (libnx's pad):
// player 1 is the console in handheld mode or the controller set as player
// 1, players 2 to 4 the controllers set as players 2 to 4 (Pro
// Controllers, or Joy-Con pairs). The shared runtime asks for PlayStation
// buttons:
//
//	          cross  circle square triangle L1      R1      select start
//	Switch    A      B      Y      X        L / ZL  R / ZR  -      +
//
// The d-pad is the d-pad, the left stick the left stick, and pushed well
// over it also works the d-pad's buttons in menus (steering keeps its
// travel).
//
// In a race that is cross to drive, square to brake, R1 to drift and
// circle (or L1) for the item; in menus cross accepts and circle goes back.
// HOME is the system's (the HOME Menu; closing the software from there).

#include "pad_io.h"

#include "mc_io.h"

#include <stdlib.h>
#include <string.h>
#include <switch.h>

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

#ifdef SWITCH_AUTOPAD
/* dev: scripted presses for pad 0, "frame+frames=BUTTON,..." with BUTTON one
 * of CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 START SELECT UP DOWN LEFT
 * RIGHT (an emulator with no pad bound still gets through the menus) */
static unsigned short auto_buttons(unsigned frame)
{
	static const char *spec = SWITCH_AUTOPAD;
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

static float clamp1(float f)
{
	return f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
}

/* the longer of two sticks (up positive in, down positive out) */
static void take_stick(float x, float y, float *lx, float *ly)
{
	x = clamp1(x);
	y = clamp1(y);
	if (x * x + y * y > *lx * *lx + *ly * *ly) {
		*lx = x;
		*ly = -y;
	}
}

static PadState s_pad[PAD_MAX];

/* player i: the controller set as that player (player 1: handheld too) */
static void read_pad(int i)
{
	s_prev[i] = s_btns[i];
	unsigned short b = 0;
	float lx = 0.0f, ly = 0.0f;
	int conn = 0;

	PadState *p = &s_pad[i];
	padUpdate(p);
	if (padIsConnected(p)) {
		conn = 1;
		const u64 h = padGetButtons(p);
		if (h & HidNpadButton_A) b |= B_CROSS;
		if (h & HidNpadButton_B) b |= B_CIRCLE;
		if (h & HidNpadButton_Y) b |= B_SQUARE;
		if (h & HidNpadButton_X) b |= B_TRIANGLE;
		if (h & (HidNpadButton_L | HidNpadButton_ZL)) b |= B_L1;
		if (h & (HidNpadButton_R | HidNpadButton_ZR)) b |= B_R1;
		if (h & HidNpadButton_Plus) b |= B_START;
		if (h & HidNpadButton_Minus) b |= B_SELECT;
		if (h & HidNpadButton_StickL) b |= B_L3;
		if (h & HidNpadButton_StickR) b |= B_R3;
		if (h & HidNpadButton_Up) b |= B_UP;
		if (h & HidNpadButton_Down) b |= B_DOWN;
		if (h & HidNpadButton_Left) b |= B_LEFT;
		if (h & HidNpadButton_Right) b |= B_RIGHT;
		const HidAnalogStickState st = padGetStickPos(p, 0);
		take_stick((float)st.x / (float)JOYSTICK_MAX, (float)st.y / (float)JOYSTICK_MAX, &lx, &ly);
	}

	s_conn[i] = conn;
	if (!conn) {
		s_btns[i] = 0;
		s_dpad[i] = 0;
		s_sdir[i] = 0;
		s_lx[i] = s_ly[i] = 0.0f;
		memset(s_pressure[i], 0, sizeof(s_pressure[i]));
		return;
	}
	s_dpad[i] = b & (B_UP | B_DOWN | B_LEFT | B_RIGHT);
	s_lx[i] = lx;
	s_ly[i] = ly;
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
	s_pressure[i][8] = (b & B_L1) ? 255 : 0;
	s_pressure[i][9] = (b & B_R1) ? 255 : 0;
	s_pressure[i][6] = (b & B_CROSS) ? 255 : 0;
	s_pressure[i][7] = (b & B_SQUARE) ? 255 : 0;
}

/* rumble: player 1's two motors, handheld, as a Pro Controller and as a
 * pair of Joy-Con (each style has handles of its own) */
static HidVibrationDeviceHandle s_vib[3][2];
static int s_vib_ok[3];

int pad_io_init(void)
{
	if (s_ready) {
		return 1;
	}
	padConfigureInput(PAD_MAX, HidNpadStyleSet_NpadStandard);
	padInitialize(&s_pad[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
	padInitialize(&s_pad[1], HidNpadIdType_No2);
	padInitialize(&s_pad[2], HidNpadIdType_No3);
	padInitialize(&s_pad[3], HidNpadIdType_No4);
	s_vib_ok[0] = R_SUCCEEDED(hidInitializeVibrationDevices(s_vib[0], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld));
	s_vib_ok[1] = R_SUCCEEDED(hidInitializeVibrationDevices(s_vib[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey));
	s_vib_ok[2] = R_SUCCEEDED(hidInitializeVibrationDevices(s_vib[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual));
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
	if (!s_ready) {
		pad_io_init();
	}
	for (int i = 0; i < PAD_MAX; i++) {
		read_pad(i);
	}
#ifdef SWITCH_AUTOPAD
	{
		/* SWITCH_AUTOPADS pads (1 if unset) all get the script */
#ifndef SWITCH_AUTOPADS
#define SWITCH_AUTOPADS 1
#endif
		const unsigned short ab = auto_buttons(s_auto_frame++);
		for (int i = 0; i < SWITCH_AUTOPADS && i < PAD_MAX; i++) {
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
	/* the small motor's buzz high, the large one's rumble low */
	HidVibrationValue v[2];
	memset(v, 0, sizeof(v));
	for (int k = 0; k < 2; k++) {
		v[k].amp_low = large > 0 ? (float)(large > 255 ? 255 : large) / 255.0f * 0.8f : 0.0f;
		v[k].freq_low = 160.0f;
		v[k].amp_high = small_on ? 0.5f : 0.0f;
		v[k].freq_high = 320.0f;
	}
	/* the style player 1 is: handheld, a Pro Controller or Joy-Con */
	const u32 style = padGetStyleSet(&s_pad[0]);
	const int d = (style & HidNpadStyleTag_NpadHandheld) ? 0 : (style & HidNpadStyleTag_NpadJoyDual) ? 2 : 1;
	if (s_vib_ok[d]) {
		hidSendVibrationValues(s_vib[d], v, 2);
	}
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
