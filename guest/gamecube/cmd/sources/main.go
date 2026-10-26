// Command sources prints the GameCube guest's SourcesHash (for
// guest/gamecube/prebuilt/SOURCES; scripts/gamecube-guest-prebuilt.sh runs it).
package main

import (
	"fmt"
	"os"

	guest "github.com/blazium-games/blazium-toolchain/guest/gamecube"
)

func main() {
	h, err := guest.SourcesHash()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Println(h)
}
