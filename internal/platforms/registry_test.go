package platforms_test

import (
	"errors"
	"testing"

	"github.com/blazium-games/blazium-toolchain/internal/platforms"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/future"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/n64"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/ps1"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/ps2"
	"github.com/blazium-games/blazium-toolchain/internal/platforms/steam"
)

func TestListIncludesPS1AndPlanned(t *testing.T) {
	platforms.Register(ps1.New())
	platforms.Register(ps2.New())
	platforms.Register(n64.New())
	platforms.Register(steam.New())
	future.Register()
	list := platforms.List()
	ids := map[string]platforms.Status{}
	for _, info := range list {
		ids[info.ID] = info.Status
	}
	if ids["ps1"] != platforms.StatusSupported {
		t.Fatalf("ps1 status %q", ids["ps1"])
	}
	if ids["ps2"] != platforms.StatusSupported {
		t.Fatalf("ps2 status %q", ids["ps2"])
	}
	if ids["n64"] != platforms.StatusSupported {
		t.Fatalf("n64 status %q", ids["n64"])
	}
	if ids["steam"] != platforms.StatusSupported {
		t.Fatalf("steam status %q", ids["steam"])
	}
	for _, id := range []string{"ps4"} {
		if ids[id] != platforms.StatusPlanned {
			t.Fatalf("%s want planned, got %q", id, ids[id])
		}
	}
}

func TestLookupPlanned(t *testing.T) {
	future.Register()
	_, err := platforms.Lookup("ps4")
	if !errors.Is(err, platforms.ErrPlanned) {
		t.Fatalf("got %v", err)
	}
}

func TestLookupUnknown(t *testing.T) {
	_, err := platforms.Lookup("dreamcast")
	if !errors.Is(err, platforms.ErrUnknownPlatform) {
		t.Fatalf("got %v", err)
	}
}

func TestLookupN64Supported(t *testing.T) {
	platforms.Register(n64.New())
	p, err := platforms.Lookup("n64")
	if err != nil {
		t.Fatal(err)
	}
	if p.Info().Status != platforms.StatusSupported {
		t.Fatalf("%+v", p.Info())
	}
}
