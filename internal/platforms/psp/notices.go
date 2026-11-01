package psp

// The third-party notices with the game: the code a pspdev build links
// into the EBOOT and the licence each part comes under. PSPSDK's libraries
// and start-up code are under a BSD licence and newlib's BSD style; their
// notices go with a binary.

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/psp"
)

type noticePart struct {
	title    string
	what     string
	embedded string // guest/psp/notices
}

var noticeParts = []noticePart{
	{"PSPSDK", "the PSP libraries (GE, display, controls, audio, kernel) and start-up code (BSD licence)", "pspsdk.txt"},
	{"newlib", "the C library (BSD-style licences)", "newlib.txt"},
}

// noticesPath: PENGUIN.zip -> PENGUIN.THIRD-PARTY-NOTICES.txt
func noticesPath(out string) string {
	return strings.TrimSuffix(out, filepath.Ext(out)) + ".THIRD-PARTY-NOTICES.txt"
}

func buildNotices(out string) ([]byte, error) {
	var b bytes.Buffer
	fmt.Fprintf(&b, "Third-party notices for %s\n\n", filepath.Base(out))
	b.WriteString("This PSP game was built with pspdev and PSPSDK\n")
	b.WriteString("(https://github.com/pspdev) and contains code from the parts below, under\n")
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
