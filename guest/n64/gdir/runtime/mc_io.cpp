// MIT. Saves for PS2.save_slot / load_slot / memcard_present on the N64:
// the cartridge's EEPROM (libdragon's eepromfs), where the emulator or the
// cart has one. Each save slot is one file, "/slotN", of "BLZS", the byte
// count and the bytes. Without an EEPROM there is no card.

#include "mc_io.h"

#include <libdragon.h>
#include <stdio.h>
#include <string.h>

#define MC_SLOTS 4
#define MC_BYTES 112 /* per slot (104 of save; eepfs adds a checksum): four slots fit a 4 Kbit EEPROM */

static int s_ok;
static uint8_t s_buf[MC_BYTES];

int mc_io_boot(void)
{
	const eeprom_type_t t = eeprom_present();
	if (t == EEPROM_NONE) {
		printf("N64 save: no EEPROM on the cart\n");
		return 0;
	}
	static const eepfs_entry_t files[MC_SLOTS] = {
		{ "/slot0", MC_BYTES, true, false }, { "/slot1", MC_BYTES, true, false }, { "/slot2", MC_BYTES, true, false }, { "/slot3", MC_BYTES, true, false },
	};
	const int r = eepfs_init(files, MC_SLOTS);
	if (r != EEPFS_ESUCCESS) {
		printf("N64 save: eepfs_init failed (%d)\n", r);
		return 0;
	}
	if (!eepfs_verify_signature()) {
		eepfs_wipe();
	}
	s_ok = 1;
	printf("N64 save: %s EEPROM, %d slots\n", t == EEPROM_16K ? "16 Kbit" : "4 Kbit", MC_SLOTS);
	return 1;
}

int mc_io_present(void)
{
	return s_ok;
}

static void path_for(int slot, char *out)
{
	snprintf(out, 8, "/slot%d", slot & 3);
}

int mc_io_save(int slot, const unsigned char *data, unsigned n)
{
	if (!s_ok || n > MC_BYTES - 8) {
		return 0;
	}
	char path[8];
	path_for(slot, path);
	memset(s_buf, 0, sizeof(s_buf));
	memcpy(s_buf, "BLZS", 4);
	s_buf[4] = (uint8_t)n;
	s_buf[5] = (uint8_t)(n >> 8);
	memcpy(s_buf + 8, data, n);
	return eepfs_write(path, s_buf, sizeof(s_buf)) == EEPFS_ESUCCESS ? 1 : 0;
}

int mc_io_load(int slot, unsigned char *data, unsigned maxn)
{
	memset(data, 0, maxn);
	if (!s_ok) {
		return -2;
	}
	char path[8];
	path_for(slot, path);
	if (eepfs_read(path, s_buf, sizeof(s_buf)) != EEPFS_ESUCCESS || memcmp(s_buf, "BLZS", 4) != 0) {
		return -1;
	}
	unsigned n = (unsigned)s_buf[4] | ((unsigned)s_buf[5] << 8);
	if (n > MC_BYTES - 8) {
		return -1;
	}
	if (n > maxn) {
		n = maxn;
	}
	memcpy(data, s_buf + 8, n);
	return (int)n;
}
