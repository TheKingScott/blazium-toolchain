package wiiu

// The third-party notices with the game: the code a devkitPPC and wut
// build links into the RPX and the licence each part comes under. wut's
// libraries and start-up code are under the zlib licence and newlib's BSD
// style; their notices go with a binary.

import (
	"bytes"
	"fmt"
	"path/filepath"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/wiiu"
)

type noticePart struct {
	title    string
	what     string
	embedded string // guest/wiiu/notices
}

var noticeParts = []noticePart{
	{"wut", "the Wii U libraries (GX2, VPAD, KPAD, AX, ProcUI, libwhb) and start-up code (zlib licence)", "wut.txt"},
	{"newlib", "the C library (BSD-style licences)", "newlib.txt"},
}

// noticesPath: PENGUIN.zip -> PENGUIN.THIRD-PARTY-NOTICES.txt
func noticesPath(out string) string {
	return strings.TrimSuffix(out, filepath.Ext(out)) + ".THIRD-PARTY-NOTICES.txt"
}

func buildNotices(out string) ([]byte, error) {
	var b bytes.Buffer
	fmt.Fprintf(&b, "Third-party notices for %s\n\n", filepath.Base(out))
	b.WriteString("This Wii U game was built with devkitPPC and wut\n")
	b.WriteString("(https://github.com/devkitPro/wut) and contains code from the parts below, under\n")
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
