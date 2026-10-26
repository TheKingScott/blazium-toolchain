// Command sources prints the PS3 guest's SourcesHash (for
// guest/ps3/prebuilt/SOURCES; scripts/ps3-guest-prebuilt.sh runs it).
package main

import (
	"fmt"
	"os"

	guest "github.com/blazium-games/blazium-toolchain/guest/ps3"
)

func main() {
	h, err := guest.SourcesHash()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println(h)
}
