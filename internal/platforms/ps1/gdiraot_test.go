package ps1

import (
	"encoding/binary"
	"fmt"
	"strings"
	"testing"
)

// aotProgram: a SCRP with a GDIR program of the given classes (each with
// _process(delta) and _helper(x) below) and no packs.
//
//	_process(delta): v = delta * 0.5; while v < 1: v = _helper(v)
//	_helper(x):      return x + 1
func aotProgram(classes int) []byte {
	le := binary.LittleEndian
	var code []byte
	op := func(b ...byte) { code = append(code, b...) }
	u16 := func(v int) []byte { return le.AppendUint16(nil, uint16(v)) }
	u32 := func(v int) []byte { return le.AppendUint32(nil, uint32(v)) }
	var starts []int
	for c := 0; c < classes; c++ {
		starts = append(starts, len(code))
		base := len(code)
		op(gopLLoad, 0)
		op(append([]byte{gopConst}, u16(1)...)...)
		op(gopMul)
		op(gopLStore, 1)
		loop := len(code)
		op(gopLLoad, 1)
		op(append([]byte{gopConst}, u16(0)...)...)
		op(gopLt)
		jz := len(code)
		op(append([]byte{gopJz}, u32(0)...)...)
		op(gopLLoad, 1)
		op(append(append([]byte{gopCall}, u16(1)...), 1)...)
		op(gopLStore, 1)
		op(append([]byte{gopJmp}, u32(loop)...)...)
		le.PutUint32(code[jz+1:], uint32(len(code)))
		op(gopRetNil)
		_ = base
		starts = append(starts, len(code))
		op(gopLLoad, 0)
		op(append([]byte{gopConst}, u16(0)...)...)
		op(gopAdd)
		op(gopRet)
	}
	var g []byte
	g = append(g, "GDIR"...)
	g = le.AppendUint16(g, 1)
	g = le.AppendUint16(g, 0)
	g = le.AppendUint32(g, 0) // total, below
	g = le.AppendUint32(g, 4)
	g = append(g, 2)
	g = le.AppendUint32(g, 1) // 0: int 1
	g = append(g, 3)
	g = le.AppendUint32(g, 0x3f000000) // 1: float 0.5
	for _, s := range []string{"_process", "_helper"} {
		g = append(g, 4)
		g = le.AppendUint16(g, uint16(len(s)))
		g = append(append(g, s...), 0)
	}
	g = le.AppendUint16(g, uint16(classes))
	for c := 0; c < classes; c++ {
		g = append(g, 0, 0, 0, 0, 2, 0)                         // members, noderefs, funcs
		g = append(g, 0xff, 0xff, 0xff, 0xff, 0, 0, 0xff, 0xff) // init, ready, process 0, physics
		g = append(g, 1, 2)
		g = le.AppendUint32(g, uint32(starts[2*c]))
		g = le.AppendUint16(g, 2)
		g = append(g, 1, 1)
		g = le.AppendUint32(g, uint32(starts[2*c+1]))
		g = le.AppendUint16(g, 3)
	}
	g = le.AppendUint32(g, uint32(len(code)))
	g = append(g, code...)
	g = le.AppendUint16(g, 0) // packs
	le.PutUint32(g[8:], uint32(len(g)))
	var s []byte
	s = append(s, "SCRP"...)
	s = le.AppendUint16(s, 1)
	s = le.AppendUint16(s, 1)
	s = le.AppendUint32(s, 0)
	return append(s, g...)
}

func TestAOTProgram(t *testing.T) {
	pr, err := parseGDIRProgram(aotProgram(1))
	if err != nil || pr == nil {
		t.Fatal(err)
	}
	if len(pr.funcs) != 2 || pr.funcs[0].name != "_process" || pr.funcs[1].end != len(pr.code) {
		t.Fatalf("funcs %+v", pr.funcs)
	}
	ops, maxd, ok := pr.decode(pr.funcs[0])
	if !ok || maxd != 2 || len(ops) != 13 {
		t.Fatalf("decode: ok %v depth %d ops %d", ok, maxd, len(ops))
	}
	sel := pr.aotSelect(1000)
	if len(sel) != 2 {
		t.Fatalf("selected %v", sel)
	}
	c := pr.aotC(sel)
	for _, want := range []string{
		fmt.Sprintf("#define GDIR_AOT_HASH 0x%08xu", pr.codeHash()),
		"static AOT_ATTR Val aot_0(", "static AOT_ATTR Val aot_1(",
		"A_CALL(0, 1, 1);",                   // the argument in the frame first
		"A_BACK();",                          // the loop's jump back is counted
		"aot_bin(15, &S[0], &L[0], &s_k[1])", // delta * 0.5 read in place
		"{ 0, aot_0 },", "{ 1, aot_1 },",
	} {
		if !strings.Contains(c, want) {
			t.Errorf("missing %q in:\n%s", want, c)
		}
	}
	// the int compare with a constant is inline, and takes the jump
	if !strings.Contains(c, "c_ = L[1].i < 1;") || !strings.Contains(c, "if (c_ == 0) goto l") {
		t.Errorf("compare and jump not fused:\n%s", c)
	}
	// budget 0: nothing
	if s := pr.aotSelect(0); len(s) != 0 {
		t.Fatalf("budget 0 selected %v", s)
	}
}

func TestAOTSharedCode(t *testing.T) {
	pr, err := parseGDIRProgram(aotProgram(3))
	if err != nil || pr == nil {
		t.Fatal(err)
	}
	sel := pr.aotSelect(1000)
	if len(sel) != 2 || len(sel[0]) != 3 || len(sel[1]) != 3 {
		t.Fatalf("three classes of the same script: %v", sel)
	}
	c := pr.aotC(sel)
	if strings.Count(c, "static AOT_ATTR Val") != 2 || !strings.Contains(c, "{ 4, aot_0 },") {
		t.Fatalf("shared code not shared:\n%s", c)
	}
	// a name list picks those alone
	t.Setenv("BLAZIUM_PS1_AOT_FUNCS", "_helper")
	if s := pr.aotSelect(1000); len(s) != 1 || pr.funcs[s[0][0]].name != "_helper" {
		t.Fatalf("named: %v", s)
	}
}

func TestAOTDecodeRejects(t *testing.T) {
	pr, _ := parseGDIRProgram(aotProgram(1))
	// a jump to where the stack is deeper: not compiled
	code := append([]byte(nil), pr.code...)
	pr.code = code
	f := pr.funcs[1]
	// _helper: LLOAD 0 (2) CONST 0 (3) ADD (1) RET -> make it jump back to
	// its start with one value on the stack
	pr.code = append(code[:f.code+5:f.code+5], append([]byte{gopJmp}, binary.LittleEndian.AppendUint32(nil, uint32(f.code))...)...)
	pr.funcs[1].end = len(pr.code)
	if _, _, ok := pr.decode(pr.funcs[1]); ok {
		t.Fatal("decoded a function whose stack depth differs at a jump target")
	}
}
