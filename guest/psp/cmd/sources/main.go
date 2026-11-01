// Command sources prints the PSP guest's SourcesHash (for
// guest/psp/prebuilt/SOURCES; scripts/psp-guest-prebuilt.sh runs it).
package main

import (
	"fmt"
	"os"

	guest "github.com/blazium-games/blazium-toolchain/guest/psp"
)

func main() {
	h, err := guest.SourcesHash()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println(h)
}
