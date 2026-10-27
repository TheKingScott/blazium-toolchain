// MIT. The Wii guest's pads beyond pad_io.h (pad_io.cpp).
#pragma once

// 1 once HOME (on a Wii Remote or Classic Controller) or the console's
// RESET was pressed, 2 once POWER was (the console's, a Wii Remote's, or
// Dolphin's Stop): main then calls wii_pad_leave.
int pad_io_quit(void);

// Leave the game: the Wii off for POWER, else back to the loader (the
// Homebrew Channel). Does not return.
void wii_pad_leave(void);
