package ps1

// Ahead-of-time compilation of GDIR (the cooked GDScript) to C for the PS1
// guest. ps1 build --sample gdir, when the export folder next to --out has
// SCRP00.BIN, writes gdir_aot.inc into the guest's sources: C for the
// functions the scripts spend their frames in (aotSelect), up to a byte
// budget, and a table the VM's call() looks up. gdir_vm.cpp includes the
// file when it is there.
//
// The C keeps the interpreter's semantics: the same frame on the VM's
// stack (locals, then operands), the same helpers for everything but the
// common cases, the same checks after anything that can fail. What goes is
// the interpreter's work per op: fetching and decoding the bytecode, the
// dispatch, moving the stack pointer, and copying each local, member and
// constant onto the stack before it is used (gdiraot_gen.go).
//
// The file carries a hash of the program (constants, classes and code); the
// VM compares it with the program it loads and only interprets if they
// differ.

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// GDIR opcodes (guest/ps2/runtime/gdir_ops.h)
const (
	gopNop      = 0
	gopConst    = 1
	gopNil      = 2
	gopTrue     = 3
	gopFalse    = 4
	gopLLoad    = 5
	gopLStore   = 6
	gopMLoad    = 7
	gopMStore   = 8
	gopSelf     = 9
	gopNodeRef  = 10
	gopGetP     = 11
	gopSetP     = 12
	gopAdd      = 13
	gopSub      = 14
	gopMul      = 15
	gopDiv      = 16
	gopMod      = 17
	gopNeg      = 18
	gopEq       = 19
	gopNe       = 20
	gopLt       = 21
	gopLe       = 22
	gopGt       = 23
	gopGe       = 24
	gopNot      = 25
	gopJmp      = 26
	gopJz       = 27
	gopJnz      = 28
	gopJzk      = 29
	gopJnzk     = 30
	gopPop      = 31
	gopDup      = 32
	gopCall     = 33
	gopNative   = 34
	gopRet      = 35
	gopRetNil   = 36
	gopArray    = 37
	gopIndex    = 38
	gopSetIndex = 39
	gopBand     = 40
	gopBor      = 41
	gopIn       = 42
)

// aotBudget: bytecode bytes compiled at most (each comes to about 9 bytes
// of MIPS: 24000 is about 210 KiB of the 2 MiB). BLAZIUM_PS1_AOT_BYTES
// overrides; 0 turns the compiler off.
const aotBudget = 24000

// aotNamedBudget: the same with BLAZIUM_PS1_AOT_FUNCS (only those named)
const aotNamedBudget = 40000

type gdirFunc struct {
	argc, locals int
	code         int // offset into the code section
	end          int // the next function's code (or the section's end)
	name         string
	class        int
}

type gdirClass struct {
	members, noderefs, funcs, first int
	process, physics                int
}

type gdirProgram struct {
	consts  int
	ktype   []int    // each constant's type
	kval    []uint32 // an int's or a float's bits, a bool
	hashed  []byte   // constants to the code's end (what the VM hashes)
	strs    map[int]string
	classes []gdirClass
	funcs   []gdirFunc
	code    []byte
}

func parseGDIRProgram(scrp []byte) (*gdirProgram, error) {
	le := binary.LittleEndian
	if len(scrp) < 12 || string(scrp[:4]) != "SCRP" || le.Uint16(scrp[6:])&1 == 0 {
		return nil, nil
	}
	tape := int(le.Uint32(scrp[8:]))
	if 12+tape+12 > len(scrp) {
		return nil, fmt.Errorf("SCRP: tape runs past the end")
	}
	g := scrp[12+tape:]
	if string(g[:4]) != "GDIR" {
		return nil, nil
	}
	r := &gdirReader{b: g, p: 12}
	pr := &gdirProgram{strs: map[int]string{}}
	pr.consts = r.u32()
	pr.ktype = make([]int, pr.consts)
	pr.kval = make([]uint32, pr.consts)
	for i := 0; i < pr.consts && r.err == nil; i++ {
		t := r.u8()
		pr.ktype[i] = t
		switch t {
		case 0:
		case 1:
			pr.kval[i] = uint32(r.u8())
		case 2, 3:
			pr.kval[i] = uint32(r.u32())
		case 4:
			n := r.u16()
			s := r.take(n + 1)
			if s != nil {
				pr.strs[i] = string(s[:n])
			}
		case 5:
			r.skip(8)
		case 6:
			r.skip(12)
		case 7:
			r.skip(r.u16() * 2)
		default:
			r.fail("unknown constant type")
		}
	}
	cn := r.u16()
	for c := 0; c < cn && r.err == nil; c++ {
		k := gdirClass{members: r.u16(), noderefs: r.u16(), funcs: r.u16()}
		r.skip(2) // init
		r.skip(2) // ready
		k.process = int(int16(r.u16()))
		k.physics = int(int16(r.u16()))
		k.first = len(pr.funcs)
		pr.classes = append(pr.classes, k)
		for f := 0; f < k.funcs && r.err == nil; f++ {
			fn := gdirFunc{argc: r.u8(), locals: r.u8(), code: r.u32(), class: c}
			fn.name = pr.strs[r.u16()]
			pr.funcs = append(pr.funcs, fn)
		}
	}
	n := r.u32()
	pr.code = r.take(n)
	if r.err != nil {
		return nil, r.err
	}
	pr.hashed = g[12:r.p]
	starts := make([]int, 0, len(pr.funcs))
	for _, f := range pr.funcs {
		starts = append(starts, f.code)
	}
	sort.Ints(starts)
	for i := range pr.funcs {
		end := len(pr.code)
		j := sort.SearchInts(starts, pr.funcs[i].code+1)
		if j < len(starts) {
			end = starts[j]
		}
		pr.funcs[i].end = end
	}
	return pr, nil
}

// codeHash: FNV-1a over the program from its constants to its code's end
// (as gdir_vm.cpp's aot_bind).
func (pr *gdirProgram) codeHash() uint32 {
	h := uint32(2166136261)
	for _, b := range pr.hashed {
		h ^= uint32(b)
		h *= 16777619
	}
	return h
}

type gdirOp struct {
	pc, op, len int
	a, b        int // operands
	depth       int // operand stack depth before the op
}

// decode: the function's ops with the operand depth before each, or false
// if a place is reached at two depths or anything is out of the ordinary.
func (pr *gdirProgram) decode(f gdirFunc) ([]gdirOp, int, bool) {
	code := pr.code
	depth := map[int]int{}
	ops := map[int]gdirOp{}
	type work struct{ pc, d int }
	stack := []work{{f.code, 0}}
	maxd := 0
	for len(stack) > 0 {
		w := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		pc, d := w.pc, w.d
		for {
			if pc < f.code || pc >= f.end {
				return nil, 0, false
			}
			if od, ok := depth[pc]; ok {
				if od != d {
					return nil, 0, false
				}
				break
			}
			depth[pc] = d
			o := gdirOp{pc: pc, op: int(code[pc]), len: 1, depth: d}
			u16 := func(at int) int { return int(binary.LittleEndian.Uint16(code[at:])) }
			u32 := func(at int) int { return int(binary.LittleEndian.Uint32(code[at:])) }
			eff, stop, branch, keep := 0, false, false, false
			need := func(n int) bool { return pc+n <= f.end }
			switch o.op {
			case gopNop:
			case gopConst:
				o.len, eff = 3, 1
				if !need(3) {
					return nil, 0, false
				}
				o.a = u16(pc + 1)
			case gopNil, gopTrue, gopFalse, gopSelf, gopDup:
				eff = 1
			case gopLLoad, gopMLoad, gopNodeRef:
				o.len, eff = 2, 1
				if !need(2) {
					return nil, 0, false
				}
				o.a = int(code[pc+1])
			case gopLStore, gopMStore, gopSetP:
				o.len, eff = 2, -1
				if !need(2) {
					return nil, 0, false
				}
				o.a = int(code[pc+1])
			case gopGetP:
				o.len = 2
				if !need(2) {
					return nil, 0, false
				}
				o.a = int(code[pc+1])
			case gopAdd, gopSub, gopMul, gopDiv, gopMod, gopBand, gopBor,
				gopEq, gopNe, gopLt, gopLe, gopGt, gopGe, gopIn, gopIndex, gopPop:
				eff = -1
			case gopNeg, gopNot:
			case gopSetIndex:
				eff = -3
			case gopJmp:
				o.len, stop, branch = 5, true, true
			case gopJz, gopJnz:
				o.len, eff, branch = 5, -1, true
			case gopJzk, gopJnzk:
				o.len, eff, branch, keep = 5, -1, true, true
			case gopCall:
				o.len = 4
				if !need(4) {
					return nil, 0, false
				}
				o.a, o.b = u16(pc+1), int(code[pc+3])
				eff = 1 - o.b
			case gopNative:
				o.len = 3
				if !need(3) {
					return nil, 0, false
				}
				o.a, o.b = int(code[pc+1]), int(code[pc+2])
				eff = 1 - o.b
			case gopArray:
				o.len = 2
				if !need(2) {
					return nil, 0, false
				}
				o.a = int(code[pc+1])
				eff = 1 - o.a
			case gopRet:
				eff, stop = -1, true
			case gopRetNil:
				stop = true
			default:
				return nil, 0, false
			}
			if !need(o.len) {
				return nil, 0, false
			}
			if branch {
				o.a = u32(pc + 1)
			}
			// the operands an op reads must be on the stack
			pops := map[int]int{gopLStore: 1, gopMStore: 1, gopSetP: 2, gopGetP: 1, gopNeg: 1, gopNot: 1,
				gopJz: 1, gopJnz: 1, gopJzk: 1, gopJnzk: 1, gopPop: 1, gopDup: 1, gopRet: 1, gopSetIndex: 3}
			switch o.op {
			case gopAdd, gopSub, gopMul, gopDiv, gopMod, gopBand, gopBor, gopEq, gopNe, gopLt, gopLe, gopGt, gopGe, gopIn, gopIndex:
				pops[o.op] = 2
			case gopCall, gopNative:
				pops[o.op] = o.b
			case gopArray:
				pops[o.op] = o.a
			}
			if d < pops[o.op] {
				return nil, 0, false
			}
			ops[pc] = o
			if branch {
				td := d + eff
				if keep {
					td = d
				}
				stack = append(stack, work{o.a, td})
			}
			if stop {
				break
			}
			d += eff
			if d > maxd {
				maxd = d
			}
			if d > 200 {
				return nil, 0, false
			}
			pc += o.len
		}
	}
	var out []gdirOp
	for _, o := range ops {
		out = append(out, o)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].pc < out[j].pc })
	return out, maxd, true
}

// aotSelect: what to compile. The functions run every frame (reached from
// a _process or _physics_process), each weighed by the ops it runs a frame
// (how often it is called times its ops; in a loop, 8 times, per loop),
// functions with the same code (the same script in several classes) taken
// together as one, the most ops per byte first, up to budget bytes of
// bytecode. Each group's first function is the one compiled; the others
// share it. BLAZIUM_PS1_AOT_FUNCS names functions (a profile's hottest,
// from a GDIR_STATS build) to take instead.
func (pr *gdirProgram) aotSelect(budget int) [][]int {
	n := len(pr.funcs)
	weight := make([]float64, n)
	ops := make([][]gdirOp, n)
	ok := make([]bool, n)
	for i := range pr.funcs {
		ops[i], _, ok[i] = pr.decode(pr.funcs[i])
	}
	// calls out of each function, with how many times each runs a call
	type edge struct {
		to   int
		mult float64
	}
	calls := make([][]edge, n)
	own := make([]float64, n) // ops a call runs, a loop's body 8 times
	for i := range pr.funcs {
		if !ok[i] {
			continue
		}
		first := pr.classes[pr.funcs[i].class].first
		for _, o := range ops[i] {
			m := 1.0
			for _, j := range ops[i] {
				if j.op >= gopJmp && j.op <= gopJnzk && j.a <= o.pc && j.pc >= o.pc && m < 512 {
					m *= 8 // in a loop
				}
			}
			own[i] += m
			if o.op != gopCall {
				continue
			}
			if t := first + o.a; t >= 0 && t < n {
				calls[i] = append(calls[i], edge{t, m})
			}
		}
	}
	// spread each frame's runs from the roots, nearest first (a call
	// cycle is followed a few rounds, then left)
	type item struct {
		f int
		w float64
		d int
	}
	var queue []item
	for _, k := range pr.classes {
		for _, r := range []int{k.process, k.physics} {
			if r >= 0 && k.first+r < n {
				queue = append(queue, item{k.first + r, 1, 0})
			}
		}
	}
	for len(queue) > 0 {
		it := queue[0]
		queue = queue[1:]
		weight[it.f] += it.w
		if it.d >= 12 {
			continue
		}
		for _, e := range calls[it.f] {
			w := it.w * e.mult
			if w > 1e6 {
				w = 1e6
			}
			queue = append(queue, item{e.to, w, it.d + 1})
		}
	}
	// the same code: one group
	type group struct {
		fns  []int
		w    float64
		size int
	}
	byKey := map[string]*group{}
	var groups []*group
	for i, f := range pr.funcs {
		if weight[i] == 0 || !ok[i] {
			continue
		}
		key := fmt.Sprintf("%d/%d/", f.argc, f.locals) + relCode(pr.code[f.code:f.end], f.code)
		g := byKey[key]
		if g == nil {
			g = &group{size: f.end - f.code}
			byKey[key] = g
			groups = append(groups, g)
		}
		g.fns = append(g.fns, i)
		g.w += weight[i] * own[i]
	}
	// BLAZIUM_PS1_AOT_FUNCS: names to take first (a profile's hottest)
	want := map[string]int{}
	for i, nm := range aotNames(os.Getenv("BLAZIUM_PS1_AOT_FUNCS")) {
		if _, dup := want[nm]; !dup {
			want[nm] = i + 1
		}
	}
	rank := func(g *group) int {
		best := 0
		for _, fi := range g.fns {
			if r := want[pr.funcs[fi].name]; r > 0 && (best == 0 || r < best) {
				best = r
			}
		}
		return best
	}
	sort.SliceStable(groups, func(a, b int) bool {
		ra, rb := rank(groups[a]), rank(groups[b])
		if (ra > 0) != (rb > 0) {
			return ra > 0
		}
		if ra > 0 {
			return ra < rb
		}
		return groups[a].w/float64(groups[a].size) > groups[b].w/float64(groups[b].size)
	})
	var out [][]int
	used := 0
	for _, g := range groups {
		if len(want) > 0 && rank(g) == 0 {
			continue
		}
		if used+g.size > budget {
			continue
		}
		used += g.size
		out = append(out, g.fns)
	}
	sort.Slice(out, func(a, b int) bool { return out[a][0] < out[b][0] })
	return out
}

// aotNames: BLAZIUM_PS1_AOT_FUNCS, names separated by commas, or a file of
// them (one or more a line, # comments)
func aotNames(v string) []string {
	v = strings.TrimSpace(v)
	if v == "" {
		return nil
	}
	if b, err := os.ReadFile(v); err == nil {
		var lines []string
		for _, ln := range strings.Split(string(b), "\n") {
			if i := strings.IndexByte(ln, '#'); i >= 0 {
				ln = ln[:i]
			}
			lines = append(lines, ln)
		}
		v = strings.Join(lines, ",")
	}
	var out []string
	for _, f := range strings.FieldsFunc(v, func(r rune) bool { return r == ',' || r == ' ' || r == '\t' || r == '\r' }) {
		out = append(out, f)
	}
	return out
}

// relCode: a function's code with its jump targets made relative to its
// start, so the same script in two classes reads the same
func relCode(c []byte, base int) string {
	var b strings.Builder
	for pc := 0; pc < len(c); {
		op := int(c[pc])
		ln := 1
		switch op {
		case gopConst, gopNative:
			ln = 3
		case gopLLoad, gopLStore, gopMLoad, gopMStore, gopNodeRef, gopGetP, gopSetP, gopArray:
			ln = 2
		case gopCall:
			ln = 4
		case gopJmp, gopJz, gopJnz, gopJzk, gopJnzk:
			ln = 5
		}
		if pc+ln > len(c) {
			ln = len(c) - pc
		}
		if ln == 5 {
			to := int(binary.LittleEndian.Uint32(c[pc+1:])) - base
			fmt.Fprintf(&b, "%d:%d,", op, to)
		} else {
			b.Write(c[pc : pc+ln])
		}
		pc += ln
	}
	return b.String()
}

// aotC: gdir_aot.inc, the C for the groups aotSelect chose.
func (pr *gdirProgram) aotC(groups [][]int) string {
	var b strings.Builder
	fmt.Fprintf(&b, "/* gdir_aot.inc: written by blazium-toolchain ps1 build (internal/platforms/ps1/gdiraot.go)\n")
	fmt.Fprintf(&b, " * from SCRP00.BIN: %d function(s) of the program compiled to C. */\n\n", len(groups))
	fmt.Fprintf(&b, "#define GDIR_AOT_HASH 0x%08xu\n#define GDIR_AOT_FUNCS %du\n\n", pr.codeHash(), len(pr.funcs))
	for _, g := range groups {
		var names []string
		for _, x := range g {
			names = append(names, fmt.Sprintf("%s (class %d)", cComment(pr.funcs[x].name), pr.funcs[x].class))
		}
		pr.aotFunc(&b, g[0], strings.Join(names, ", "))
	}
	fmt.Fprintf(&b, "static const struct AotEntry k_aot[] = {\n")
	for _, g := range groups {
		for _, x := range g {
			fmt.Fprintf(&b, "\t{ %d, aot_%d },\n", x, g[0])
		}
	}
	fmt.Fprintf(&b, "};\n")
	return b.String()
}

func cComment(s string) string {
	s = strings.ReplaceAll(s, "*/", "* /")
	return strings.Map(func(r rune) rune {
		if r < 32 || r > 126 {
			return '?'
		}
		return r
	}, s)
}

// writeAOT: gdir_aot.inc into the guest sources (dir) from the export
// folder's SCRP00.BIN, if there is one. Returns how many functions were
// compiled (0: no file written).
func writeAOT(exportDir, guestDir string) (int, int, error) {
	budget := aotBudget
	if len(aotNames(os.Getenv("BLAZIUM_PS1_AOT_FUNCS"))) > 0 {
		budget = aotNamedBudget
	}
	if v := strings.TrimSpace(os.Getenv("BLAZIUM_PS1_AOT_BYTES")); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 {
			return 0, 0, fmt.Errorf("BLAZIUM_PS1_AOT_BYTES: want a byte count, got %q", v)
		}
		budget = n
	}
	if budget == 0 {
		return 0, 0, nil
	}
	path := findCase(exportDir, "SCRP00.BIN")
	if path == "" {
		return 0, 0, nil
	}
	scrp, err := os.ReadFile(path)
	if err != nil {
		return 0, 0, err
	}
	pr, err := parseGDIRProgram(scrp)
	if err != nil || pr == nil {
		return 0, 0, err
	}
	sel := pr.aotSelect(budget)
	if len(sel) == 0 {
		return 0, 0, nil
	}
	bytes := 0
	for _, g := range sel {
		bytes += pr.funcs[g[0]].end - pr.funcs[g[0]].code
	}
	if err := os.WriteFile(filepath.Join(guestDir, "gdir_aot.inc"), []byte(pr.aotC(sel)), 0o644); err != nil {
		return 0, 0, err
	}
	return len(sel), bytes, nil
}
