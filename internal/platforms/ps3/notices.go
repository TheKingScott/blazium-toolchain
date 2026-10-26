package ps3

// The third-party notices beside the .pkg: the code a PSL1GHT build links
// into EBOOT.BIN and the licence each part comes under. PSL1GHT's libraries
// and start-up code are MIT and newlib's are BSD style; both ask for their
// notices with a binary.

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	guest "github.com/blazium-games/blazium-toolchain/guest/ps3"
)

type noticePart struct {
	title    string
	what     string
	embedded string // guest/ps3/notices
}

var noticeParts = []noticePart{
	{"PSL1GHT", "the PS3 libraries (RSX, GCM, pads, audio, system) and start-up code (MIT licence)", "psl1ght.txt"},
	{"newlib", "the C library (BSD-style licences)", "newlib.txt"},
}

// noticesPath: PENGUIN.pkg -> PENGUIN.THIRD-PARTY-NOTICES.txt
func noticesPath(out string) string {
	return strings.TrimSuffix(out, filepath.Ext(out)) + ".THIRD-PARTY-NOTICES.txt"
}

func buildNotices(out string) ([]byte, error) {
	var b bytes.Buffer
	fmt.Fprintf(&b, "Third-party notices for %s\n\n", filepath.Base(out))
	b.WriteString("This PlayStation 3 game was built with PSL1GHT and ps3toolchain\n")
	b.WriteString("(https://github.com/ps3dev) and contains code from the parts below, under\n")
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
