// MIT. Fail-soft pads for PCSX2. BUILD_FOR_PCSX2 on IOP reset. rom0: X modules
// (XSIO2MAN, XPADMAN, XMTAPMAN) for up to four pads with a multitap.

#include "pad_io.h"
#include "mc_io.h"

#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <string.h>
#include <tamtypes.h>

#ifndef BUILD_FOR_PCSX2
#define BUILD_FOR_PCSX2 1
#endif

#if defined(__has_include)
#if __has_include("irx_flags.h")
#include "irx_flags.h"
#endif
#if __has_include(<libpad.h>)
#include <libpad.h>
#include <libmtap.h>
#define BLAZIUM_PS2_HAS_LIBPAD 1
#endif
#if __has_include(<iopcontrol.h>)
#include <iopcontrol.h>
#define BLAZIUM_PS2_HAS_IOPCONTROL 1
#endif
#if __has_include(<sbv_patches.h>)
#include <sbv_patches.h>
#define BLAZIUM_PS2_HAS_SBV 1
#endif
#endif

#ifdef BLAZIUM_PS2_HAS_IRX_FILEXIO
extern "C" {
extern const unsigned char irx_fileXio[];
extern const unsigned int size_irx_fileXio;
}
#endif
#ifdef BLAZIUM_PS2_HAS_IRX_IOMANX
extern "C" {
extern const unsigned char irx_iomanX[];
extern const unsigned int size_irx_iomanX;
}
#endif

#ifdef BLAZIUM_PS2_HAS_LIBPAD
/* Up to four pads, in player order: port 1 (its four multitap slots when a
 * multitap is in it), then port 2 the same way. Player 1's pad also keeps the
 * single-pad state the actions, sticks, pressure and rumble read. */
#define PAD_MAX 4
struct PadSlot {
	char buf[256] __attribute__((aligned(64)));
	int port;
	int slot;
	int open;
	int mode_set;
	unsigned short btns;
	unsigned short prev;
	float lx; /* left stick across, -1..1, 0 inside the dead zone */
};
static PadSlot s_pads[PAD_MAX] __attribute__((aligned(64)));
static int s_pad_n;
static int s_mtap; /* 1 when XMTAPMAN is up */
static int s_mtap_seen[2];
static unsigned s_poll_n;
static int s_ready;
static int s_prev_cross;
static int s_has_act;
static int s_has_press;
static unsigned char s_pressure[12];
static unsigned short s_btns;
static unsigned short s_prev_btns;
static float s_lx;
static float s_ly;
static float s_rx;
static float s_ry;
static float s_deadzone;

static void pad_try_dualshock(PadSlot *p, int first)
{
	int state = padGetState(p->port, p->slot);
	if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
		return;
	}
	if (p->mode_set) {
		return;
	}
	padSetMainMode(p->port, p->slot, 1, 3);
	if (first && padInfoAct(p->port, p->slot, -1, 0)) {
		char align[6] = {0, 1, (char)0xff, (char)0xff, (char)0xff, (char)0xff};
		if (padSetActAlign(p->port, p->slot, align) != 0) {
			s_has_act = 1;
		}
	}
	if (first && padInfoPressMode(p->port, p->slot)) {
		if (padEnterPressMode(p->port, p->slot) != 0) {
			s_has_press = 1;
		}
	}
	p->mode_set = 1;
}

static int load_rom_or_buf(const char *rom, const unsigned char *buf, unsigned sz)
{
	int ret = SifLoadModule(rom, 0, NULL);
	if (ret >= 0) {
		return 1;
	}
	if (buf && sz) {
		ret = SifExecModuleBuffer((void *)buf, (int)sz, 0, NULL, NULL);
		return ret >= 0;
	}
	return 0;
}

/* Which pads exist: port 1's slots, then port 2's, four at most. A port with
 * a multitap has four slots, one without has one. Pads already open keep
 * their places. */
static void pad_assign(void)
{
	int ports[PAD_MAX], slots[PAD_MAX], n = 0;
	for (int port = 0; port < 2 && n < PAD_MAX; port++) {
		const int nslot = s_mtap_seen[port] ? 4 : 1;
		for (int slot = 0; slot < nslot && n < PAD_MAX; slot++) {
			ports[n] = port;
			slots[n] = slot;
			n++;
		}
	}
	for (int i = 0; i < n; i++) {
		PadSlot *p = &s_pads[i];
		if (p->open && p->port == ports[i] && p->slot == slots[i]) {
			continue;
		}
		if (p->open) {
			padPortClose(p->port, p->slot);
		}
		p->port = ports[i];
		p->slot = slots[i];
		p->mode_set = 0;
		p->btns = 0;
		p->prev = 0;
		p->open = padPortOpen(p->port, p->slot, p->buf) != 0;
	}
	s_pad_n = n;
}

int pad_io_init(void)
{
	s_ready = 0;
	s_prev_cross = 0;
	s_has_act = 0;
	s_has_press = 0;
	s_btns = 0;
	s_prev_btns = 0;
	s_lx = s_ly = s_rx = s_ry = 0.0f;
	s_deadzone = 0.2f;
	s_pad_n = 0;
	s_mtap = 0;
	s_mtap_seen[0] = s_mtap_seen[1] = 0;
	s_poll_n = 0;
	memset(s_pressure, 0, sizeof(s_pressure));
	for (int i = 0; i < PAD_MAX; i++) {
		s_pads[i].open = 0;
	}
	SifInitRpc(0);
#ifdef BLAZIUM_PS2_HAS_IOPCONTROL
#if BUILD_FOR_PCSX2
	while (!SifIopReset("", 0)) {
	}
#else
	while (!SifIopReset("rom0:UDNL rom0:EELOADCNF", 0)) {
	}
#endif
	while (!SifIopSync()) {
	}
	SifInitRpc(0);
#endif
#ifdef BLAZIUM_PS2_HAS_SBV
	sbv_patch_enable_lmb();
	sbv_patch_disable_prefix_check();
#endif
#ifdef BLAZIUM_PS2_HAS_IRX_IOMANX
	load_rom_or_buf("rom0:IOMANX", irx_iomanX, size_irx_iomanX);
#else
	SifLoadModule("rom0:IOMANX", 0, NULL);
#endif
#ifdef BLAZIUM_PS2_HAS_IRX_FILEXIO
	load_rom_or_buf("host:fileXio.irx", irx_fileXio, size_irx_fileXio);
#endif
	/* The X modules, all at boot and in the order the BIOS's own browser
	 * uses: the SIO2 driver, the multitap, the pads, then the memory card
	 * (mc_io). Loading the card modules later, while XPADMAN and XMTAPMAN
	 * are polling the SIO2 bus, brought PCSX2 down. */
	SifLoadModule("rom0:XSIO2MAN", 0, NULL);
	const int mtap_ok = SifLoadModule("rom0:XMTAPMAN", 0, NULL) >= 0;
	if (SifLoadModule("rom0:XPADMAN", 0, NULL) < 0) {
		mc_io_boot();
		return 0;
	}
	mc_io_boot();
	if (mtap_ok) {
		mtapInit();
		s_mtap = 1;
		mtapPortOpen(0);
		mtapPortOpen(1);
	}
	padInit(0);
	pad_assign();
	if (!s_pads[0].open) {
		return 0;
	}
	s_ready = 1;
	return 1;
}

static int pad_read(PadSlot *p, struct padButtonStatus *buttons)
{
	if (!p->open) {
		return 0;
	}
	p->prev = p->btns;
	const int state = padGetState(p->port, p->slot);
	if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
		p->btns = 0;
		p->lx = 0.0f;
		return 0;
	}
	if (padRead(p->port, p->slot, buttons) == 0) {
		return 0;
	}
	p->btns = (unsigned short)~buttons->btns;
	/* analog (DualShock) mode only: a digital pad's stick bytes mean nothing */
	p->lx = 0.0f;
	if ((buttons->mode >> 4) == 7) {
		const float x = ((int)buttons->ljoy_h - 128) / 128.0f;
		if (x > s_deadzone || x < -s_deadzone) {
			p->lx = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
		}
	}
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
	if (cross_down) {
		*cross_down = 0;
	}
	if (!s_ready) {
		return;
	}
	/* a multitap can take a moment to be seen, or be plugged in later */
	if (s_mtap && (s_poll_n++ % 60) == 0) {
		int changed = 0;
		for (int port = 0; port < 2; port++) {
			const int on = mtapGetConnection(port) == 1;
			if (on != s_mtap_seen[port]) {
				s_mtap_seen[port] = on;
				changed = 1;
			}
		}
		if (changed) {
			pad_assign();
		}
	}
	struct padButtonStatus buttons;
	for (int i = 1; i < s_pad_n; i++) {
		pad_try_dualshock(&s_pads[i], 0);
		pad_read(&s_pads[i], &buttons);
	}
	pad_try_dualshock(&s_pads[0], 1);
	if (!pad_read(&s_pads[0], &buttons)) {
		return;
	}
	if (s_has_press) {
		s_pressure[0] = buttons.right_p;
		s_pressure[1] = buttons.left_p;
		s_pressure[2] = buttons.up_p;
		s_pressure[3] = buttons.down_p;
		s_pressure[4] = buttons.triangle_p;
		s_pressure[5] = buttons.circle_p;
		s_pressure[6] = buttons.cross_p;
		s_pressure[7] = buttons.square_p;
		s_pressure[8] = buttons.l1_p;
		s_pressure[9] = buttons.r1_p;
		s_pressure[10] = buttons.l2_p;
		s_pressure[11] = buttons.r2_p;
	}
	s_prev_btns = s_btns;
	const unsigned short btns = s_pads[0].btns;
	s_btns = btns;
	s_lx = ((int)buttons.ljoy_h - 128) / 128.0f;
	s_ly = ((int)buttons.ljoy_v - 128) / 128.0f;
	s_rx = ((int)buttons.rjoy_h - 128) / 128.0f;
	s_ry = ((int)buttons.rjoy_v - 128) / 128.0f;
	if (yaw) {
		if (btns & PAD_LEFT) {
			*yaw -= 0.04f;
		}
		if (btns & PAD_RIGHT) {
			*yaw += 0.04f;
		}
		*yaw += ((int)buttons.rjoy_h - 128) * 0.00035f;
	}
	if (dolly) {
		if (btns & PAD_UP) {
			*dolly -= 0.12f;
		}
		if (btns & PAD_DOWN) {
			*dolly += 0.12f;
		}
		*dolly += ((int)buttons.ljoy_v - 128) * 0.0012f;
	}
	const int cross = (btns & PAD_CROSS) ? 1 : 0;
	if (cross_down) {
		*cross_down = (cross && !s_prev_cross) ? 1 : 0;
	}
	s_prev_cross = cross;
}

void pad_io_set_rumble(int small_on, int large)
{
	if (!s_ready || !s_has_act) {
		return;
	}
	char act[6] = {0, 0, 0, 0, 0, 0};
	act[0] = small_on ? 1 : 0;
	act[1] = (char)(large < 0 ? 0 : (large > 255 ? 255 : large));
	padSetActDirect(s_pads[0].port, s_pads[0].slot, act);
}

int pad_io_get_pressure(int button)
{
	if (!s_has_press || button < 0 || button >= 12) {
		return 0;
	}
	return (int)s_pressure[button];
}

static unsigned short action_mask(int action)
{
	switch (action) {
	case 0:
		return PAD_LEFT;
	case 1:
		return PAD_RIGHT;
	case 2:
		return PAD_UP;
	case 3:
		return PAD_DOWN;
	case 4:
		return PAD_CROSS;
	case 5:
		return PAD_CIRCLE;
	case 6:
		return PAD_SQUARE;
	case 7:
		return PAD_TRIANGLE;
	case 8:
		return PAD_L1;
	case 9:
		return PAD_R1;
	case 10:
		return PAD_L2;
	case 11:
		return PAD_R2;
	case 12:
		return PAD_START;
	case 13:
		return PAD_SELECT;
	default:
		return 0;
	}
}

int pad_io_pressed(int action)
{
	const unsigned short m = action_mask(action);
	return (s_ready && m && (s_btns & m)) ? 1 : 0;
}

int pad_io_just_pressed(int action)
{
	const unsigned short m = action_mask(action);
	return (s_ready && m && (s_btns & m) && !(s_prev_btns & m)) ? 1 : 0;
}

int pad_io_just_released(int action)
{
	const unsigned short m = action_mask(action);
	return (s_ready && m && !(s_btns & m) && (s_prev_btns & m)) ? 1 : 0;
}

void pad_io_stick(int stick, float *x, float *y)
{
	float sx = 0.0f;
	float sy = 0.0f;
	if (s_ready) {
		if (stick == 1) {
			sx = s_rx;
			sy = s_ry;
		} else {
			sx = s_lx;
			sy = s_ly;
			if (s_btns & PAD_LEFT) {
				sx = -1.0f;
			}
			if (s_btns & PAD_RIGHT) {
				sx = 1.0f;
			}
			if (s_btns & PAD_UP) {
				sy = -1.0f;
			}
			if (s_btns & PAD_DOWN) {
				sy = 1.0f;
			}
		}
	}
	if (s_deadzone > 0.0f) {
		const float mag = sx * sx + sy * sy;
		if (mag < s_deadzone * s_deadzone) {
			sx = 0.0f;
			sy = 0.0f;
		}
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
	if (zone < 0.0f) {
		zone = 0.0f;
	}
	if (zone > 0.95f) {
		zone = 0.95f;
	}
	s_deadzone = zone;
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
	if (!name) {
		return 0;
	}
	for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		if (strcmp(name, k[i].name) == 0) {
			return k[i].mask;
		}
	}
	return 0;
}

int pad_io_count(void)
{
	int n = 0;
	for (int i = 0; i < s_pad_n; i++) {
		if (s_pads[i].open) {
			const int state = padGetState(s_pads[i].port, s_pads[i].slot);
			if (state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) {
				n = i + 1;
			}
		}
	}
	return n;
}

int pad_io_pad_pressed(int pad, const char *button)
{
	const unsigned short m = button_mask(button);
	return (pad >= 0 && pad < s_pad_n && m && (s_pads[pad].btns & m)) ? 1 : 0;
}

int pad_io_pad_just_pressed(int pad, const char *button)
{
	const unsigned short m = button_mask(button);
	return (pad >= 0 && pad < s_pad_n && m && (s_pads[pad].btns & m) && !(s_pads[pad].prev & m)) ? 1 : 0;
}

float pad_io_pad_axis(int pad)
{
	if (pad < 0 || pad >= s_pad_n) {
		return 0.0f;
	}
	const PadSlot *p = &s_pads[pad];
	if (p->btns & PAD_LEFT) {
		return -1.0f;
	}
	if (p->btns & PAD_RIGHT) {
		return 1.0f;
	}
	return p->lx;
}
#else
int pad_io_init(void)
{
	return 0;
}

void pad_io_poll(float *yaw, float *dolly, int *cross_down)
{
	if (yaw) {
		*yaw = 0.0f;
	}
	if (dolly) {
		*dolly = 0.0f;
	}
	if (cross_down) {
		*cross_down = 0;
	}
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

int pad_io_pressed(int action)
{
	(void)action;
	return 0;
}

int pad_io_just_pressed(int action)
{
	(void)action;
	return 0;
}

int pad_io_just_released(int action)
{
	(void)action;
	return 0;
}

void pad_io_stick(int stick, float *x, float *y)
{
	(void)stick;
	if (x) {
		*x = 0.0f;
	}
	if (y) {
		*y = 0.0f;
	}
}

void pad_io_set_deadzone(float zone)
{
	(void)zone;
}

float pad_io_get_deadzone(void)
{
	return 0.2f;
}

int pad_io_count(void)
{
	return 0;
}

int pad_io_pad_pressed(int pad, const char *button)
{
	(void)pad;
	(void)button;
	return 0;
}

int pad_io_pad_just_pressed(int pad, const char *button)
{
	(void)pad;
	(void)button;
	return 0;
}

float pad_io_pad_axis(int pad)
{
	(void)pad;
	return 0.0f;
}

int pad_io_count(void)
{
	return 0;
}

int pad_io_pad_pressed(int pad, const char *button)
{
	(void)pad;
	(void)button;
	return 0;
}

int pad_io_pad_just_pressed(int pad, const char *button)
{
	(void)pad;
	(void)button;
	return 0;
}
#endif
