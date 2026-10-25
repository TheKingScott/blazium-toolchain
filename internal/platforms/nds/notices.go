package nds

// The third-party notices beside the .nds: the code a BlocksDS build links
// into the ROM and the licence each part comes under. libnds, its FatFs and
// the ARM7 core are zlib, ISC (maxmod) and MIT (DSWiFi); picolibc's are BSD
// style and ask for their notices with a binary; BlocksDS's start-up code
// and linker scripts are MPL-2.0, which asks that the ROM say where their
// source is. The installed BlocksDS's own licence files are used when they
// are there, else the copies in guest/nds/notices.

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/nds"
)

type noticePart struct {
	title     string
	what      string
	installed string // under BLOCKSDS ("" none)
	embedded  string // guest/nds/notices
}

var noticeParts = []noticePart{
	{"libnds", "the DS library (zlib licence)", "libs/libnds/licenses/LICENSE", "libnds.txt"},
	{"FatFs", "the FAT filesystem in libnds (SD card and NitroFS)", "libs/libnds/licenses/LICENSE.FatFs", "fatfs.txt"},
	{"BlocksDS start-up code and linker scripts", "MPL-2.0. Their source: https://github.com/blocksds/sdk/tree/master/sys/crts", "sys/crts/COPYING.mpl2", "mpl-2.0.txt"},
	{"maxmod", "the sound system in the ARM7 core (ISC licence)", "libs/maxmod/COPYING", "maxmod.txt"},
	{"DSWiFi", "the Wi-Fi library in the ARM7 core (MIT licence)", "libs/dswifi/COPYING", "dswifi.txt"},
	{"picolibc", "the C library (BSD-style licences)", "", "picolibc.txt"},
}

// noticesPath: PENGUIN.nds -> PENGUIN.THIRD-PARTY-NOTICES.txt
func noticesPath(rom string) string {
	return strings.TrimSuffix(rom, filepath.Ext(rom)) + ".THIRD-PARTY-NOTICES.txt"
}

func buildNotices(rom, blocksds string) ([]byte, error) {
	var b bytes.Buffer
	fmt.Fprintf(&b, "Third-party notices for %s\n\n", filepath.Base(rom))
	b.WriteString("This Nintendo DS ROM was built with BlocksDS (https://github.com/blocksds/sdk)\n")
	b.WriteString("and contains code from the parts below, under these licences. The GCC\n")
	b.WriteString("runtime libraries (libgcc, libstdc++) are under the GCC Runtime Library\n")
	b.WriteString("Exception and need no notice.\n")
	for _, p := range noticeParts {
		var text []byte
		if p.installed != "" && blocksds != "" {
			if raw, err := os.ReadFile(filepath.Join(blocksds, filepath.FromSlash(p.installed))); err == nil {
				text = raw
			}
		}
		if text == nil {
			raw, err := guest.Notice(p.embedded)
			if err != nil {
				return nil, err
			}
			text = raw
		}
		fmt.Fprintf(&b, "\n\n==============================================================================\n%s: %s\n==============================================================================\n\n", p.title, p.what)
		b.Write(bytes.TrimRight(text, "\n"))
		b.WriteString("\n")
	}
	return b.Bytes(), nil
}

func writeNotices(rom, blocksds string) (string, error) {
	text, err := buildNotices(rom, blocksds)
	if err != nil {
		return "", err
	}
	path := noticesPath(rom)
	return path, os.WriteFile(path, text, 0o644)
}
