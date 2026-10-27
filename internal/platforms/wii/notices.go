package wii

// The third-party notices with the app: the code a devkitPPC build links
// into boot.dol and the licence each part comes under. libogc's libraries
// and start-up code are under a zlib-style licence (with parts under a
// BSD-style one), its wiiuse under the GPL version 3 with a linking
// exception, libfat's under a BSD licence and newlib's BSD style; their
// notices go with a binary.

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/wii"
)

type noticePart struct {
	title    string
	what     string
	embedded string // guest/wii/notices
}

var noticeParts = []noticePart{
	{"libogc", "the Wii libraries (GX, video, pads, audio, Bluetooth) and start-up code", "libogc.txt"},
	{"wiiuse", "the Wii Remote library (Wii Remotes, Nunchuks, Classic Controllers)", "wiiuse.txt"},
	{"libfat", "the FAT file system (SD card, USB drives)", "libfat.txt"},
	{"newlib", "the C library (BSD-style licences)", "newlib.txt"},
}

// noticesPath: PENGUIN.zip -> PENGUIN.THIRD-PARTY-NOTICES.txt
func noticesPath(out string) string {
	return strings.TrimSuffix(out, filepath.Ext(out)) + ".THIRD-PARTY-NOTICES.txt"
}

func buildNotices(out string) ([]byte, error) {
	var b bytes.Buffer
	fmt.Fprintf(&b, "Third-party notices for %s\n\n", filepath.Base(out))
	b.WriteString("This Wii game was built with devkitPPC, libogc and libfat\n")
	b.WriteString("(https://github.com/devkitPro) and contains code from the parts below, under\n")
	b.WriteString("these licences. The GCC runtime libraries (libgcc, libstdc++) are under the\n")
	b.WriteString("GCC Runtime Library Exception and need no notice.\n")
	for _, p := range noticeParts {
		text, err := guest.Notice(p.embedded)
		if err != nil {
			return nil, err
		}
		fmt.Fprintf(&b, "\n\n==============================================================================\n%s: %s\n==============================================================================\n\n", p.title, p.what)
		b.Write(bytes.TrimRight(text, "\n"))
		b.WriteString("\n")
	}
	return b.Bytes(), nil
}

func writeNotices(out string) (string, error) {
	text, err := buildNotices(out)
	if err != nil {
		return "", err
	}
	path := noticesPath(out)
	return path, os.WriteFile(path, text, 0o644)
}
