// Command sources prints the Wii U guest's SourcesHash (for
// guest/wiiu/prebuilt/SOURCES; scripts/wiiu-guest-prebuilt.sh runs it).
package main

import (
	"fmt"
	"os"

	guest "github.com/blazium-games/blazium-toolchain/guest/wiiu"
)

func main() {
	h, err := guest.SourcesHash()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println(h)
}
