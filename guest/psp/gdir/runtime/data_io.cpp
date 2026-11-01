// MIT. The game's files on the PSP: a game folder, PSP/GAME/NAME/ on the
// Memory Stick (ms0:, or the PSP Go's internal ef0:), with EBOOT.PBP and
// the cooked packs in DATA/ (psp build writes it). The shared runtime's
// fopen/fread/fseek then read the packs as on the PS2's CD.

#include "data_io.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* The game folder's name, written into the EBOOT by psp build:
 * "BLZAPPDIR:" then the name, zero-padded (empty here; the build fills it
 * in). Used when argv[0] does not lead to the data. */
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

/* the first PSP/GAME folder on dev ("ms0:") with the game's data */
static int scan(const char *dev)
{
	char apps[32];
	snprintf(apps, sizeof(apps), "%s/PSP/GAME", dev);
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
	/* argv[0]: "ms0:/PSP/GAME/PENGUIN/EBOOT.PBP" */
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
			const char *devs[] = { "ms0:", "ef0:" };
			for (int i = 0; i < 2 && !s_dir[0]; i++) {
				char p[128];
				snprintf(p, sizeof(p), "%s/PSP/GAME/%s", devs[i], name);
				try_dir(p);
			}
		}
	}
	if (!s_dir[0] && !scan("ms0:")) {
		scan("ef0:");
	}
	printf("PSP data: %s\n", s_dir[0] ? s_dir : "the game's folder is not on the Memory Stick");
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
