// MIT. The PS2 guest's pad API (pad_io.h) on the Wii: in each of the four
// players' slots a Wii Remote (with a Nunchuk or a Classic Controller, or
// on its own held sideways) and the GameCube controller in that port, as
// one pad. The shared runtime asks for PlayStation buttons:
//
//	                 cross  circle square triangle L1  R1  select start
//	Remote sideways  2      1      A      -        -   B   -      +
//	Remote+Nunchuk   A      B      C      -        -   Z   -      +
//	Classic          a      b      y      x        L   R/ZR -     +
//	GameCube         A      B      X      Y        L   R   Z      Start
//
// (ZL is L1 too.) The d-pads are the d-pad (a sideways Remote's turned to
// match); the Nunchuk's, the Classic's left and the GameCube's control
// stick are the left stick, and pushed well over they also work the
// d-pad's buttons in menus (steering keeps their travel).
//
// In a race that is cross to drive, square to brake, R1 to drift and
// circle (or L1) for the item; in menus cross accepts and circle goes
// back. HOME (Remote or Classic) leaves the game for the loader, as the
// console's RESET does; POWER turns the Wii off (wii_pad.h).

#include "pad_io.h"

#include "mc_io.h"
#include "wii_pad.h"

#include <gccore.h>
#include <wiiuse/wpad.h>
#include <stdlib.h>
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

#ifdef WII_AUTOPAD
/* dev: scripted presses for pad 0, "frame+frames=BUTTON,..." with BUTTON one
 * of CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 START SELECT UP DOWN LEFT
 * RIGHT (an emulator with no pad bound still gets through the menus) */
static unsigned short auto_buttons(unsigned frame)
{
	static const char *spec = WII_AUTOPAD;
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
	/* the GameCube stick reaches about 100 each way; up is positive (the
	 * PS's is down) */
	float f = (float)v / 96.0f;
	return f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
}

/* a Wii joystick (Nunchuk, Classic) -1..1 each way, up positive */
static float js_axis(int pos, int mn, int ctr, int mx)
{
	float f = 0.0f;
	if (pos > ctr && mx > ctr) {
		f = (float)(pos - ctr) / (float)(mx - ctr);
	} else if (pos < ctr && ctr > mn) {
		f = -(float)(ctr - pos) / (float)(ctr - mn);
	}
	return f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
}

static volatile int s_quit; /* HOME or RESET: back to the loader */
static volatile int s_power; /* POWER (the console's or a Wii Remote's): off */
static int s_wpad_ok;

static void on_reset(u32 irq, void *ctx)
{
	(void)irq;
	(void)ctx;
	s_quit = 1;
}

/* The callbacks run in the interrupt: they only ask, and main turns the
 * Wii off (wii_pad_leave), where shutting down can wait for the IOS. */
static void on_power(void)
{
	s_power = 1;
}

static void on_wpad_power(s32 chan)
{
	(void)chan;
	s_power = 1;
}

int pad_io_quit(void)
{
	return s_power ? 2 : (s_quit ? 1 : 0);
}

void wii_pad_leave(void)
{
	if (s_wpad_ok) {
		WPAD_Shutdown();
	}
	if (s_power) {
		SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	}
	exit(0);
}

/* player i: the Wii Remote in slot i and the GameCube controller in port i */
static void read_pad(int i, u32 gc_connected)
{
	s_prev[i] = s_btns[i];
	unsigned short b = 0;
	float lx = 0.0f, ly = 0.0f;
	int tl = 0, tr = 0;
	int conn = 0;

	/* the GameCube controller */
	if ((gc_connected >> i) & 1) {
		conn = 1;
		const u16 h = PAD_ButtonsHeld(i);
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
		tl = PAD_TriggerL(i);
		tr = PAD_TriggerR(i);
		if (tl > 170) b |= B_L1;
		if (tr > 170) b |= B_R1;
		lx = axis(PAD_StickX(i));
		ly = -axis(PAD_StickY(i));
	}

	/* the Wii Remote and what is plugged into it */
	u32 type = 0;
	WPADData *wd = s_wpad_ok && WPAD_Probe(i, &type) == WPAD_ERR_NONE ? WPAD_Data(i) : 0;
	if (wd && wd->err == WPAD_ERR_NONE) {
		conn = 1;
		const u32 h = wd->btns_h;
		if (h & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) {
			s_quit = 1;
		}
		if (h & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) b |= B_START;
		if (h & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) b |= B_SELECT;
		if (wd->exp.type == WPAD_EXP_CLASSIC) {
			const classic_ctrl_t *c = &wd->exp.classic;
			if (h & WPAD_CLASSIC_BUTTON_A) b |= B_CROSS;
			if (h & WPAD_CLASSIC_BUTTON_B) b |= B_CIRCLE;
			if (h & WPAD_CLASSIC_BUTTON_Y) b |= B_SQUARE;
			if (h & WPAD_CLASSIC_BUTTON_X) b |= B_TRIANGLE;
			if (h & (WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL)) b |= B_L1;
			if (h & (WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_ZR)) b |= B_R1;
			if (h & WPAD_CLASSIC_BUTTON_UP) b |= B_UP;
			if (h & WPAD_CLASSIC_BUTTON_DOWN) b |= B_DOWN;
			if (h & WPAD_CLASSIC_BUTTON_LEFT) b |= B_LEFT;
			if (h & WPAD_CLASSIC_BUTTON_RIGHT) b |= B_RIGHT;
			const float x = js_axis(c->ljs.pos.x, c->ljs.min.x, c->ljs.center.x, c->ljs.max.x);
			const float y = js_axis(c->ljs.pos.y, c->ljs.min.y, c->ljs.center.y, c->ljs.max.y);
			if (x * x + y * y > lx * lx + ly * ly) {
				lx = x;
				ly = -y;
			}
			const int cl = (int)(c->l_shoulder * 255.0f), cr = (int)(c->r_shoulder * 255.0f);
			tl = cl > tl ? cl : tl;
			tr = cr > tr ? cr : tr;
		} else if (wd->exp.type == WPAD_EXP_NUNCHUK) {
			/* the Remote upright in the right hand, the Nunchuk in the left */
			const nunchuk_t *n = &wd->exp.nunchuk;
			if (h & WPAD_BUTTON_A) b |= B_CROSS;
			if (h & WPAD_BUTTON_B) b |= B_CIRCLE;
			if (h & WPAD_NUNCHUK_BUTTON_C) b |= B_SQUARE;
			if (h & WPAD_NUNCHUK_BUTTON_Z) b |= B_R1;
			if (h & WPAD_BUTTON_UP) b |= B_UP;
			if (h & WPAD_BUTTON_DOWN) b |= B_DOWN;
			if (h & WPAD_BUTTON_LEFT) b |= B_LEFT;
			if (h & WPAD_BUTTON_RIGHT) b |= B_RIGHT;
			const float x = js_axis(n->js.pos.x, n->js.min.x, n->js.center.x, n->js.max.x);
			const float y = js_axis(n->js.pos.y, n->js.min.y, n->js.center.y, n->js.max.y);
			if (x * x + y * y > lx * lx + ly * ly) {
				lx = x;
				ly = -y;
			}
		} else {
			/* sideways, the d-pad on the left: its up is the Remote's left */
			if (h & WPAD_BUTTON_2) b |= B_CROSS;
			if (h & WPAD_BUTTON_1) b |= B_CIRCLE;
			if (h & WPAD_BUTTON_A) b |= B_SQUARE;
			if (h & WPAD_BUTTON_B) b |= B_R1;
			if (h & WPAD_BUTTON_LEFT) b |= B_UP;
			if (h & WPAD_BUTTON_RIGHT) b |= B_DOWN;
			if (h & WPAD_BUTTON_DOWN) b |= B_RIGHT;
			if (h & WPAD_BUTTON_UP) b |= B_LEFT;
		}
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
	s_pressure[i][8] = (unsigned char)tl;
	s_pressure[i][9] = (unsigned char)tr;
	s_pressure[i][6] = (b & B_CROSS) ? 255 : 0;
	s_pressure[i][7] = (b & B_SQUARE) ? 255 : 0;
}

int pad_io_init(void)
{
	if (s_ready) {
		return 1;
	}
	PAD_Init();
	s_wpad_ok = WPAD_Init() == WPAD_ERR_NONE;
	if (s_wpad_ok) {
		WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC);
		WPAD_SetPowerButtonCallback(on_wpad_power);
	}
	SYS_SetResetCallback(on_reset);
	SYS_SetPowerCallback(on_power);
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
	const u32 connected = PAD_ScanPads();
	if (s_wpad_ok) {
		WPAD_ScanPads();
	}
	for (int i = 0; i < PAD_MAX; i++) {
		read_pad(i, connected);
	}
#ifdef WII_AUTOPAD
	{
		/* WII_AUTOPADS pads (1 if unset) all get the script */
#ifndef WII_AUTOPADS
#define WII_AUTOPADS 1
#endif
		const unsigned short ab = auto_buttons(s_auto_frame++);
		for (int i = 0; i < WII_AUTOPADS && i < PAD_MAX; i++) {
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
	const int on = small_on || large > 0;
	PAD_ControlMotor(0, on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
	if (s_wpad_ok) {
		WPAD_Rumble(0, on);
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
