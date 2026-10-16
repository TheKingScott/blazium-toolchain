// MIT. libpad + rom0 or embedded IRX. Digital + sticks nudge the P4 camera.

#ifndef BLAZIUM_PS2_PAD_IO_H
#define BLAZIUM_PS2_PAD_IO_H

#ifdef __cplusplus
extern "C" {
#endif

int pad_io_init(void);
void pad_io_poll(float *yaw, float *dolly, int *cross_down);
void pad_io_set_rumble(int small_on, int large);
int pad_io_get_pressure(int button);

// PS1-compatible action ids: 0 left, 1 right, 2 up, 3 down, 4 accept, 5 cancel,
// 6 square, 7 triangle, 8 L1, 9 R1, 10 L2, 11 R2, 12 start, 13 select.
int pad_io_pressed(int action);
int pad_io_just_pressed(int action);
int pad_io_just_released(int action);
void pad_io_stick(int stick, float *x, float *y);
void pad_io_set_deadzone(float zone);
float pad_io_get_deadzone(void);

#ifdef __cplusplus
}
#endif

#endif
