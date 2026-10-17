// MIT. Memory card saves for PS2.save_slot / load_slot / memcard_present.
//
// The card in slot 1 holds one directory per game, with the BIOS browser's
// icon.sys and icon, and one file per save slot. The directory name, the
// title the browser shows and the icon come from MCSAVE.BIN, which the
// export writes next to the packs (cdrom0:\DATA\ or host:); without it a
// save still works, under a generic name with no icon.

#ifndef BLAZIUM_PS2_MC_IO_H
#define BLAZIUM_PS2_MC_IO_H

#ifdef __cplusplus
extern "C" {
#endif

// Load the card modules (XMCMAN, XMCSERV) and start libmc. pad_io_init calls
// it at boot, after the pad modules. 1 when the card can be used.
int mc_io_boot(void);

// 1 when a formatted PS2 memory card is in slot 1.
int mc_io_present(void);

// Write n bytes as save slot `slot` (0..3). 1 on success.
int mc_io_save(int slot, const unsigned char *data, unsigned n);

// Read save slot `slot` into data (at most maxn bytes; the rest of data is
// zeroed). Bytes read, -1 if the card has no such save, -2 if there is no
// usable card.
int mc_io_load(int slot, unsigned char *data, unsigned maxn);

#ifdef __cplusplus
}
#endif

#endif
