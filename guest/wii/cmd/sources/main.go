// Command sources prints the Wii guest's SourcesHash (for
// guest/wii/prebuilt/SOURCES; scripts/wii-guest-prebuilt.sh runs it).
package main

import (
	"fmt"
	"os"

	guest "github.com/blazium-games/blazium-toolchain/guest/wii"
)

func main() {
	h, err := guest.SourcesHash()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println(h)
}
