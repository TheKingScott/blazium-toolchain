// MIT. Fail-soft pad for PCSX2. BUILD_FOR_PCSX2 on IOP reset. Prefer rom0: modules.

#include "pad_io.h"

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
static char s_pad_buf[256] __attribute__((aligned(64)));
static int s_ready;
static int s_prev_cross;
static int s_mode_set;
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

static void pad_try_dualshock(void)
{
	int state = padGetState(0, 0);
	if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
		return;
	}
	if (s_mode_set) {
		return;
	}
	padSetMainMode(0, 0, 1, 3);
	if (padInfoAct(0, 0, -1, 0)) {
		char align[6] = {0, 1, (char)0xff, (char)0xff, (char)0xff, (char)0xff};
		if (padSetActAlign(0, 0, align) != 0) {
			s_has_act = 1;
		}
	}
	if (padInfoPressMode(0, 0)) {
		if (padEnterPressMode(0, 0) != 0) {
			s_has_press = 1;
		}
	}
	s_mode_set = 1;
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

int pad_io_init(void)
{
	s_ready = 0;
	s_prev_cross = 0;
	s_mode_set = 0;
	s_has_act = 0;
	s_has_press = 0;
	s_btns = 0;
	s_prev_btns = 0;
	s_lx = s_ly = s_rx = s_ry = 0.0f;
	s_deadzone = 0.2f;
	memset(s_pressure, 0, sizeof(s_pressure));
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
	SifLoadModule("rom0:SIO2MAN", 0, NULL);
	if (SifLoadModule("rom0:PADMAN", 0, NULL) < 0) {
		return 0;
	}
	padInit(0);
	if (padPortOpen(0, 0, s_pad_buf) == 0) {
		return 0;
	}
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
	if (cross_down) {
		*cross_down = 0;
	}
	if (!s_ready) {
		return;
	}
	pad_try_dualshock();
	struct padButtonStatus buttons;
	int state = padGetState(0, 0);
	if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
		return;
	}
	if (padRead(0, 0, &buttons) == 0) {
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
	const unsigned short btns = ~buttons.btns;
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
	padSetActDirect(0, 0, act);
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
#endif
