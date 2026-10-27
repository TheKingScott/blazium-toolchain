// MIT. The game's files on the Wii: a Homebrew Channel app folder,
// apps/NAME/ on the SD card (or a USB drive), with boot.dol, meta.xml,
// icon.png and the cooked packs in DATA/ (wii build writes it). libfat
// mounts the card ("sd:") and drives ("usb:"); the shared runtime's
// fopen/fread/fseek then read the packs as on the PS2's CD.

#include "data_io.h"

#include <dirent.h>
#include <fat.h>
#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* The app folder's name, written into the DOL by wii build: "BLZAPPDIR:"
 * then the name, zero-padded (empty here; the build fills it in). Used
 * when the loader passes no argv (Dolphin, some loaders). */
extern "C" {
__attribute__((used)) volatile char blazium_app_dir[64] = "BLZAPPDIR:";
}

static char s_dir[256];

static int has_data(const char *dir)
{
	char p[300];
	snprintf(p, sizeof(p), "%s/DATA/MESH00.BIN", dir);
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int try_dir(const char *dir)
{
	if (!dir || !dir[0] || strlen(dir) >= sizeof(s_dir) || !has_data(dir)) {
		return 0;
	}
	strcpy(s_dir, dir);
	return 1;
}

/* the first apps/ folder on dev ("sd:") with the game's data */
static int scan(const char *dev)
{
	char apps[32];
	snprintf(apps, sizeof(apps), "%s/apps", dev);
	DIR *d = opendir(apps);
	if (!d) {
		return 0;
	}
	int ok = 0;
	struct dirent *e;
	while (!ok && (e = readdir(d)) != 0) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char p[256];
		snprintf(p, sizeof(p), "%s/%s", apps, e->d_name);
		ok = try_dir(p);
	}
	closedir(d);
	return ok;
}

int data_init(int argc, char **argv)
{
	s_dir[0] = 0;
	if (!fatInitDefault()) {
		printf("Wii data: no SD card or USB drive\n");
		return 0;
	}
	/* argv[0]: "sd:/apps/penguin/boot.dol" from the Homebrew Channel */
	if (argc > 0 && argv && argv[0]) {
		char p[256];
		strncpy(p, argv[0], sizeof(p) - 1);
		p[sizeof(p) - 1] = 0;
		char *slash = strrchr(p, '/');
		if (slash) {
			*slash = 0;
			try_dir(p);
		}
	}
	/* the folder name the build wrote */
	if (!s_dir[0]) {
		char name[64];
		for (int i = 0; i < 54; i++) {
			name[i] = blazium_app_dir[10 + i];
		}
		name[53] = 0;
		if (name[0]) {
			const char *devs[] = { "sd:", "usb:" };
			for (int i = 0; i < 2 && !s_dir[0]; i++) {
				char p[128];
				snprintf(p, sizeof(p), "%s/apps/%s", devs[i], name);
				try_dir(p);
			}
		}
	}
	if (!s_dir[0] && !scan("sd:")) {
		scan("usb:");
	}
	printf("Wii data: %s\n", s_dir[0] ? s_dir : "the game's folder is not on the SD card or a USB drive");
	return s_dir[0] ? 1 : 0;
}

const char *data_dir(void)
{
	return s_dir;
}

FILE *data_fopen(const char *name)
{
	if (!s_dir[0] || !name) {
		return 0;
	}
	char p[320];
	snprintf(p, sizeof(p), "%s/DATA/%s", s_dir, name);
	return fopen(p, "rb");
}
