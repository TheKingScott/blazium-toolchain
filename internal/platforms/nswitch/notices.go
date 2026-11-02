package nswitch

// The third-party notices with the game: the code a devkitA64 and libnx
// build links into the .nro and the licence each part comes under. libnx's
// libraries and start-up code are under the ISC licence, deko3d under zlib
// and newlib's BSD style; their notices go with a binary.

import (
	"bytes"
	"fmt"
	"path/filepath"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/switch"
)

type noticePart struct {
	title    string
	what     string
	embedded string // guest/switch/notices
}

var noticeParts = []noticePart{
	{"libnx", "the Switch libraries (applet, HID, audout, file systems, threads) and start-up code (ISC licence)", "libnx.txt"},
	{"deko3d", "the Switch GPU library (zlib licence)", "deko3d.txt"},
	{"newlib", "the C library (BSD-style licences)", "newlib.txt"},
}

// noticesPath: PENGUIN.zip -> PENGUIN.THIRD-PARTY-NOTICES.txt
func noticesPath(out string) string {
	return strings.TrimSuffix(out, filepath.Ext(out)) + ".THIRD-PARTY-NOTICES.txt"
}

func buildNotices(out string) ([]byte, error) {
	var b bytes.Buffer
	fmt.Fprintf(&b, "Third-party notices for %s\n\n", filepath.Base(out))
	b.WriteString("This Switch game was built with devkitA64, libnx and deko3d\n")
	b.WriteString("(https://github.com/switchbrew/libnx) and contains code from the parts below, under\n")
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
