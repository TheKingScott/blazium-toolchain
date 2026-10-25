// MIT. The PS2 guest's pad API (pad_io.h) on the DS's buttons (libnds's
// keys; one player). The shared runtime asks for PlayStation buttons; each
// is one DS button, by place on the face as the PS has them:
//
//	cross A, circle B, square Y, triangle X, L1 L, R1 R,
//	start Start, select Select, the d-pad as it is (and as the left stick).
//
// In a race that is A to drive, Y to brake, R to drift and B or L for the
// item; in menus A accepts and B goes back.

#include "pad_io.h"

#include "mc_io.h"

#include <nds.h>
#include <string.h>

#define PAD_MAX 1

/* the PS2's button bits (libpad's), so the tables match the other guests */
enum {
	B_SELECT = 0x0001, B_L3 = 0x0002, B_R3 = 0x0004, B_START = 0x0008,
	B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080,
	B_L2 = 0x0100, B_R2 = 0x0200, B_L1 = 0x0400, B_R1 = 0x0800,
	B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000,
};

static int s_ready;
static unsigned short s_btns[PAD_MAX];
static unsigned short s_prev[PAD_MAX];
static float s_lx[PAD_MAX], s_ly[PAD_MAX];
static int s_conn[PAD_MAX];
static float s_deadzone = 0.2f;
static int s_prev_cross;

#ifdef NDS_AUTOPAD
/* dev: scripted presses, "frame+frames=BUTTON,..." with BUTTON one of A B X
 * Y L R START SELECT UP DOWN LEFT RIGHT (an emulator with no keys bound still
 * gets through the menus) */
static unsigned short auto_buttons(unsigned frame)
{
	static const char *spec = NDS_AUTOPAD;
	static const struct {
		const char *n;
		unsigned short m;
	} k[] = { { "A", B_CROSS }, { "B", B_CIRCLE }, { "Y", B_SQUARE }, { "X", B_TRIANGLE }, { "L", B_L1 }, { "R", B_R1 },
		{ "START", B_START }, { "SELECT", B_SELECT }, { "UP", B_UP }, { "DOWN", B_DOWN }, { "LEFT", B_LEFT }, { "RIGHT", B_RIGHT } };
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

static void read_pad(int i)
{
	s_prev[i] = s_btns[i];
	s_conn[i] = 1;
	const uint32_t k = keysHeld();
	unsigned short b = 0;
	if (k & KEY_A) b |= B_CROSS;
	if (k & KEY_B) b |= B_CIRCLE;
	if (k & KEY_Y) b |= B_SQUARE;
	if (k & KEY_X) b |= B_TRIANGLE;
	if (k & KEY_L) b |= B_L1;
	if (k & KEY_R) b |= B_R1;
	if (k & KEY_START) b |= B_START;
	if (k & KEY_SELECT) b |= B_SELECT;
	if (k & KEY_UP) b |= B_UP;
	if (k & KEY_DOWN) b |= B_DOWN;
	if (k & KEY_LEFT) b |= B_LEFT;
	if (k & KEY_RIGHT) b |= B_RIGHT;
	s_btns[i] = b;
	/* no stick: pad_io_stick reads the d-pad */
	s_lx[i] = s_ly[i] = 0.0f;
}

int pad_io_init(void)
{
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
	scanKeys();
	for (int i = 0; i < PAD_MAX; i++) {
		read_pad(i);
	}
#ifdef NDS_AUTOPAD
	s_btns[0] |= auto_buttons(s_auto_frame++);
#endif
	const int cross = (s_btns[0] & B_CROSS) ? 1 : 0;
	if (cross_down) {
		*cross_down = cross && !s_prev_cross;
	}
	s_prev_cross = cross;
}

void pad_io_set_rumble(int small_on, int large)
{
	(void)small_on; /* no rumble pak */
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

void pad_io_stick(int stick, float *x, float *y)
{
	float sx = 0.0f, sy = 0.0f;
	if (stick == 0) {
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
