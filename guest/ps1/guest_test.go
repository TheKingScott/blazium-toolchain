package guest

import (
	"bytes"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestInstallWritesRuntime(t *testing.T) {
	dir := t.TempDir()
	if err := Install(dir); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "CMakeLists.txt")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "main.cpp")); err != nil {
		t.Fatal(err)
	}
	if CookABI != 20 {
		t.Fatalf("CookABI %d", CookABI)
	}
	for _, name := range RuntimeNames {
		if _, err := os.Stat(filepath.Join(dir, name)); err != nil {
			t.Fatalf("missing %s: %v", name, err)
		}
	}
}

func TestEmbeddedRuntimeComplete(t *testing.T) {
	for _, name := range RuntimeNames {
		if _, err := files.ReadFile("runtime/" + name); err != nil {
			t.Fatalf("embed missing %s: %v", name, err)
		}
	}
}

func TestOverlayReplacesAndAdds(t *testing.T) {
	dir := t.TempDir()
	if err := Install(dir); err != nil {
		t.Fatal(err)
	}
	over := t.TempDir()
	if err := os.MkdirAll(filepath.Join(over, "extra"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(over, "main.cpp"), []byte("// overlay main\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(over, "extra", "hooks.cpp"), []byte("// extra hook\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(over, "skip.exe"), []byte("no"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := Overlay(dir, over); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(dir, "main.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "// overlay main\n" {
		t.Fatalf("main.cpp not replaced: %q", got)
	}
	if _, err := os.Stat(filepath.Join(dir, "extra", "hooks.cpp")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "skip.exe")); err == nil {
		t.Fatal("exe should not copy")
	}
	if _, err := os.Stat(filepath.Join(dir, "script_vm.cpp")); err != nil {
		t.Fatal(err)
	}
}

func TestLockstepWithEditorRuntime(t *testing.T) {
	if os.Getenv("BLAZIUM_EDITOR_LOCKSTEP") != "1" {
		t.Skip("set BLAZIUM_EDITOR_LOCKSTEP=1 to compare guest sources with a sibling editor tree")
	}
	_, thisFile, _, ok := runtime.Caller(0)
	if !ok {
		t.Fatal("caller")
	}
	toolchainRoot := filepath.Clean(filepath.Join(filepath.Dir(thisFile), "..", ".."))
	editor := filepath.Join(toolchainRoot, "..", "blazium", "platform", "ps1", "runtime")
	if _, err := os.Stat(filepath.Join(editor, "main.cpp")); err != nil {
		t.Skip("editor runtime tree absent")
	}
	embedded := filepath.Join(filepath.Dir(thisFile), "runtime")
	for _, name := range []string{"main.cpp", "CMakeLists.txt", "script_vm.cpp", "script_vm.h", "fmv_play.cpp", "fmv_play.h"} {
		want, err := os.ReadFile(filepath.Join(editor, name))
		if err != nil {
			t.Fatal(err)
		}
		got, err := os.ReadFile(filepath.Join(embedded, name))
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(want, got) {
			t.Fatalf("%s differs from editor copy at %s", name, editor)
		}
	}
}

func TestInstallGDIR(t *testing.T) {
	dir := t.TempDir()
	if err := InstallGDIR(dir); err != nil {
		t.Fatal(err)
	}
	for _, name := range []string{"CMakeLists.txt", "main.cpp", "gpu_draw.cpp", "ps1_libc.c", "shim/stdio.h"} {
		if _, err := os.Stat(filepath.Join(dir, filepath.FromSlash(name))); err != nil {
			t.Fatalf("missing %s: %v", name, err)
		}
	}
	raw, err := os.ReadFile(filepath.Join(dir, "CMakeLists.txt"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(raw, []byte("psn00bsdk_add_executable(runtime")) {
		t.Fatal("GDIR CMakeLists does not build the runtime target")
	}
}

// The GDIR guest runs the PS2 guest's VM: the shared files must not drift.
func TestGDIRSharedMatchesPS2(t *testing.T) {
	for _, name := range GDIRShared {
		p1, err := gdirFiles.ReadFile("gdir/runtime/" + name)
		if err != nil {
			t.Fatalf("embed missing %s: %v", name, err)
		}
		p2, err := os.ReadFile(filepath.Join("..", "ps2", "runtime", name))
		if err != nil {
			t.Fatalf("ps2 %s: %v", name, err)
		}
		if !bytes.Equal(p1, p2) {
			t.Errorf("guest/ps1/gdir/runtime/%s differs from guest/ps2/runtime/%s (copy it across)", name, name)
		}
	}
}
