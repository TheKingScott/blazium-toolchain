package ps1

// The C for one compiled function (gdiraot.go). A virtual stack follows
// the bytecode: a constant, local or member pushed is only noted, and its
// value read where an op uses it (a local's int added in place, a
// member's vector's .x read straight off it); a slot is written to the
// frame (materialized) only when something needs it there: a call, a
// native, a helper's slow path, a jump (every jump target sees the whole
// operand stack in the frame).

import (
	"fmt"
	"strings"
)

const (
	vsMat    = iota // in the frame, S[d]
	vsConst         // constant k (k < 0: -1 nil, -2 true, -3 false)
	vsLocal         // L[k]
	vsMember        // mb[k]
)

// Val types (gdir_vm.cpp)
const (
	vtNil = iota
	vtBool
	vtInt
	vtFloat
	vtStr
	vtVec2
	vtVec3
	vtNode
	vtArr
	vtKarr
	vtUnknown = -1
)

type vslot struct {
	kind, k int
	known   int // a materialized slot's type + 1, 0 if not known
}

type aotGen struct {
	pr *gdirProgram
	b  *strings.Builder
	vs []vslot
}

func (g *aotGen) line(f string, a ...any) { fmt.Fprintf(g.b, "\t"+f+"\n", a...) }

// the constant's type, value bits
func (g *aotGen) kinfo(k int) (int, uint32) {
	switch k {
	case -1:
		return vtNil, 0
	case -2:
		return vtBool, 1
	case -3:
		return vtBool, 0
	}
	if k < 0 || k >= len(g.pr.ktype) {
		return vtNil, 0
	}
	t := g.pr.ktype[k]
	switch t {
	case 0:
		return vtNil, 0
	case 1:
		return vtBool, g.pr.kval[k]
	case 2:
		return vtInt, g.pr.kval[k]
	case 3:
		return vtFloat, g.pr.kval[k]
	case 4:
		return vtStr, 0
	case 5:
		return vtVec2, 0
	case 6:
		return vtVec3, 0
	case 7:
		return vtKarr, 0
	}
	return vtNil, 0
}

// a slot's value as a Val lvalue
func (g *aotGen) ref(d int) string {
	s := g.vs[d]
	switch s.kind {
	case vsLocal:
		return fmt.Sprintf("L[%d]", s.k)
	case vsMember:
		return fmt.Sprintf("mb[%d]", s.k)
	case vsConst:
		switch s.k {
		case -1:
			return "k_vnil"
		case -2:
			return "k_vtrue"
		case -3:
			return "k_vfalse"
		}
		if t, _ := g.kinfo(s.k); t == vtNil && (s.k < 0 || s.k >= len(g.pr.ktype)) {
			return "k_vnil"
		}
		return fmt.Sprintf("s_k[%d]", s.k)
	}
	return fmt.Sprintf("S[%d]", d)
}

// the static type of a slot, or vtUnknown
func (g *aotGen) stype(d int) int {
	if s := g.vs[d]; s.kind == vsConst {
		t, _ := g.kinfo(s.k)
		return t
	}
	return vtUnknown
}

func (g *aotGen) texpr(d int) string { return g.ref(d) + ".t" }

func (g *aotGen) iexpr(d int) string {
	if s := g.vs[d]; s.kind == vsConst {
		t, v := g.kinfo(s.k)
		if t == vtInt || t == vtBool {
			return fmt.Sprintf("%d", int32(v))
		}
	}
	return g.ref(d) + ".i"
}

// write the value of slot d to the lvalue dst
func (g *aotGen) store(dst string, d int) {
	s := g.vs[d]
	if s.kind == vsConst {
		t, v := g.kinfo(s.k)
		switch t {
		case vtNil:
			g.line("A_ZERO(%s);", dst)
			return
		case vtBool:
			g.line("%s.t = T_BOOL; %s.i = %d;", dst, dst, v)
			return
		case vtInt:
			g.line("%s.t = T_INT; %s.i = %d;", dst, dst, int32(v))
			return
		case vtFloat:
			g.line("%s.t = T_FLOAT; ((vword *)&%s)[1] = 0x%08xu;", dst, dst, v)
			return
		}
	}
	src := g.ref(d)
	if src != dst {
		g.line("vcopy(&%s, &%s);", dst, src)
	}
}

func (g *aotGen) mat(d int) {
	if g.vs[d].kind != vsMat {
		known := 0
		if t := g.stype(d); t != vtUnknown {
			known = t + 1
		}
		g.store(fmt.Sprintf("S[%d]", d), d)
		g.vs[d] = vslot{known: known}
	}
}

func (g *aotGen) matAll() {
	for d := range g.vs {
		g.mat(d)
	}
}

// before a slot of kind/k is written: the slots that read it, to the frame
func (g *aotGen) matReaders(kind, k int) {
	for d, s := range g.vs {
		if s.kind == kind && s.k == k {
			g.mat(d)
		}
	}
}

func (g *aotGen) push(s vslot) { g.vs = append(g.vs, s) }
func (g *aotGen) pop(n int)    { g.vs = g.vs[:len(g.vs)-n] }
func (g *aotGen) top() int     { return len(g.vs) - 1 }

// a slot's truth: C, or the constant's ("1"/"0")
func (g *aotGen) truth(d int) string {
	if t := g.stype(d); t != vtUnknown {
		_, v := g.kinfo(g.vs[d].k)
		switch t {
		case vtNil:
			return "0"
		case vtBool, vtInt:
			if v != 0 {
				return "1"
			}
			return "0"
		case vtFloat:
			if v&0x7fffffff != 0 {
				return "1"
			}
			return "0"
		}
	}
	r := g.ref(d)
	if s := g.vs[d]; s.kind == vsMat && s.known == vtBool+1 {
		return fmt.Sprintf("(%s.i != 0)", r)
	}
	return fmt.Sprintf("(%s.t == T_BOOL ? %s.i != 0 : op_truthy(&%s))", r, r, r)
}

func (g *aotGen) jumpIf(cond string, want bool, to int) {
	switch {
	case cond == "1" && want, cond == "0" && !want:
		g.line("goto l%d;", to)
	case cond == "1" || cond == "0":
	case want:
		g.line("if (%s) goto l%d;", cond, to)
	default:
		g.line("if (!%s) goto l%d;", cond, to)
	}
}

var aotCop = map[int]string{gopAdd: "+", gopSub: "-", gopMul: "*", gopLt: "<", gopLe: "<=", gopGt: ">", gopGe: ">=", gopEq: "==", gopNe: "!="}

// the int-int test of slots a, b ("" when one can't be an int)
func (g *aotGen) bothInt(a, b int) string {
	var c []string
	for _, d := range []int{a, b} {
		switch g.stype(d) {
		case vtInt:
		case vtUnknown:
			c = append(c, g.texpr(d)+" == T_INT")
		default:
			return ""
		}
	}
	if len(c) == 0 {
		return "1"
	}
	return strings.Join(c, " && ")
}

// the fast test of == / != (the interpreter's: one of int, bool, node)
func (g *aotGen) eqFast(a, b int) string {
	ta, tb := g.stype(a), g.stype(b)
	switch {
	case ta != vtUnknown && tb != vtUnknown:
		return "" // two constants: rare, the helper
	case tb != vtUnknown:
		if tb == vtInt || tb == vtBool {
			return fmt.Sprintf("%s == %d", g.texpr(a), tb)
		}
		return ""
	case ta != vtUnknown:
		if ta == vtInt || ta == vtBool {
			return fmt.Sprintf("%s == %d", g.texpr(b), ta)
		}
		return ""
	}
	return fmt.Sprintf("%s == %s && (%s == T_INT || %s == T_BOOL || %s == T_NODE)", g.texpr(a), g.texpr(b), g.texpr(a), g.texpr(a), g.texpr(a))
}

// the slow path's operands to the frame (in its own block: the virtual
// stack keeps them pending for the fast path's sake)
func (g *aotGen) matIn(ds ...int) {
	saved := append([]vslot(nil), g.vs...)
	for _, d := range ds {
		g.mat(d)
	}
	g.vs = saved
}

func (pr *gdirProgram) aotFunc(b *strings.Builder, fi int, names string) {
	f := pr.funcs[fi]
	ops, _, _ := pr.decode(f)
	targets := map[int]bool{}
	for _, o := range ops {
		switch o.op {
		case gopJmp, gopJz, gopJnz, gopJzk, gopJnzk:
			targets[o.a] = true
		}
	}
	g := &aotGen{pr: pr, b: b}
	fmt.Fprintf(b, "/* %s */\nstatic AOT_ATTR Val aot_%d(unsigned inst, Val *L, Val *stk, unsigned *spv, Val *mb)\n{\n", names, fi)
	g.line("AOT_PROLOGUE(%d);", f.locals)
	back := func(o gdirOp) {
		if o.a <= o.pc {
			g.line("A_BACK();")
		}
	}
	live := true // the previous op falls through
	for i := 0; i < len(ops); i++ {
		o := ops[i]
		if targets[o.pc] {
			if live {
				g.matAll()
			}
			fmt.Fprintf(b, "l%d:\n", o.pc)
			live = true
			g.vs = make([]vslot, o.depth)
		} else if !live || len(g.vs) != o.depth {
			g.vs = make([]vslot, o.depth)
			live = true
		}
		d := o.depth
		switch o.op {
		case gopNop:
		case gopConst:
			g.push(vslot{kind: vsConst, k: o.a})
		case gopNil:
			g.push(vslot{kind: vsConst, k: -1})
		case gopTrue:
			g.push(vslot{kind: vsConst, k: -2})
		case gopFalse:
			g.push(vslot{kind: vsConst, k: -3})
		case gopLLoad:
			g.push(vslot{kind: vsLocal, k: o.a})
		case gopMLoad:
			g.push(vslot{kind: vsMember, k: o.a})
		case gopLStore:
			t := g.top()
			if s := g.vs[t]; !(s.kind == vsLocal && s.k == o.a) {
				g.pop(1)
				g.matReaders(vsLocal, o.a)
				g.push(s)
				g.store(fmt.Sprintf("L[%d]", o.a), t)
			}
			g.pop(1)
		case gopMStore:
			t := g.top()
			s := g.vs[t]
			g.pop(1)
			g.matReaders(vsMember, o.a)
			g.push(s)
			if st := g.stype(t); st != vtUnknown && st != vtStr {
				g.store(fmt.Sprintf("mb[%d]", o.a), t)
			} else {
				g.mat(t)
				g.line("A_MSTORE(%d, %d);", t, o.a)
			}
			g.pop(1)
		case gopSelf:
			g.line("A_SELF(%d);", d)
			g.push(vslot{})
		case gopNodeRef:
			g.line("A_NODEREF(%d, %d);", d, o.a)
			g.push(vslot{})
		case gopGetP:
			t := g.top()
			// GP_X/Y/Z 6-8, GP_R/G/B 12-14 (gdir_ops.h)
			k := map[int]int{6: 0, 7: 1, 8: 2, 12: 0, 13: 1, 14: 2}
			if kk, ok := k[o.a]; ok && g.stype(t) == vtUnknown {
				r := g.ref(t)
				cond := fmt.Sprintf("%s.t == T_VEC3", r)
				if kk < 2 {
					cond = fmt.Sprintf("(%s.t == T_VEC3 || %s.t == T_VEC2)", r, r)
				}
				g.line("if (%s) {", cond)
				g.line("\t((vword *)&S[%d])[1] = ((const vword *)&%s)[%d];", t, r, 1+kk)
				g.line("\tS[%d].t = T_FLOAT;", t)
				g.line("} else {")
				g.line("\tif (aot_getp(&S[%d], &%s, %d)) goto done;", t, r, o.a)
				g.line("}")
				g.vs[t] = vslot{}
			} else {
				g.line("A_SYNC(%d);", t+1)
				g.line("if (aot_getp(&S[%d], &%s, %d)) goto done;", t, g.ref(t), o.a)
				g.vs[t] = vslot{}
			}
		case gopAdd, gopSub, gopMul:
			a, bb := d-2, d-1
			// ints inline only when one side is an int constant (a
			// counter, an index); the rest is mostly floats: the helper
			fast := ""
			if g.stype(a) == vtInt || g.stype(bb) == vtInt {
				fast = g.bothInt(a, bb)
			}
			if fast != "" {
				g.line("if (%s) {", fast)
				g.line("\tconst int r_ = %s %s %s;", g.iexpr(a), aotCop[o.op], g.iexpr(bb))
				g.line("\tS[%d].i = r_;", a)
				g.line("\tS[%d].t = T_INT;", a)
				g.line("} else {")
			}
			g.line("\tif (aot_bin(%d, &S[%d], &%s, &%s)) goto done;", o.op, a, g.ref(a), g.ref(bb))
			if fast != "" {
				g.line("}")
			}
			g.pop(2)
			g.push(vslot{})
		case gopLt, gopLe, gopGt, gopGe, gopEq, gopNe:
			a, bb := d-2, d-1
			eq := o.op == gopEq || o.op == gopNe
			fast := ""
			if g.stype(a) != vtUnknown || g.stype(bb) != vtUnknown {
				if eq {
					fast = g.eqFast(a, bb)
				} else if g.stype(a) == vtInt || g.stype(bb) == vtInt {
					fast = g.bothInt(a, bb)
				}
			}
			// a compare a jump takes (the jump not a jump's target)
			var j *gdirOp
			if i+1 < len(ops) && !targets[ops[i+1].pc] && (ops[i+1].op == gopJz || ops[i+1].op == gopJnz) {
				j = &ops[i+1]
			}
			dst := fmt.Sprintf("&S[%d]", a)
			if j != nil {
				dst = "0"
			}
			g.line("{")
			g.line("\tint c_;")
			if fast != "" {
				g.line("\tif (%s) {", fast)
				g.line("\t\tc_ = %s %s %s;", g.iexpr(a), aotCop[o.op], g.iexpr(bb))
				if j == nil {
					g.line("\t\tS[%d].i = c_;", a)
					g.line("\t\tS[%d].t = T_BOOL;", a)
				}
				g.line("\t} else {")
			}
			g.line("\tc_ = aot_rel(%d, %s, &%s, &%s);", o.op, dst, g.ref(a), g.ref(bb))
			g.line("\tif (c_ < 0) goto done;")
			if fast != "" {
				g.line("\t}")
			}
			g.pop(2)
			if j != nil {
				g.matAll()
				back(*j)
				want := 0
				if j.op == gopJnz {
					want = 1
				}
				g.line("\tif (c_ == %d) goto l%d;", want, j.a)
				i++
			} else {
				g.line("\t(void)c_;")
				g.push(vslot{known: vtBool + 1})
			}
			g.line("}")
		case gopJz, gopJnz:
			t := g.top()
			cond := g.truth(t)
			if strings.Contains(cond, "S[") || strings.Contains(cond, "L[") || strings.Contains(cond, "mb[") || strings.Contains(cond, "s_k[") {
				// read it before the others are written to the frame
				g.line("{")
				g.line("\tconst int t_ = %s;", cond)
				g.pop(1)
				g.matAll()
				back(o)
				g.jumpIf("t_", o.op == gopJnz, o.a)
				g.line("}")
			} else {
				g.pop(1)
				g.matAll()
				if (cond == "1") == (o.op == gopJnz) {
					back(o)
				}
				g.jumpIf(cond, o.op == gopJnz, o.a)
			}
		case gopJzk, gopJnzk:
			g.matAll()
			back(o)
			g.jumpIf(g.truth(d-1), o.op == gopJnzk, o.a)
			g.pop(1)
		case gopJmp:
			g.matAll()
			back(o)
			g.line("goto l%d;", o.a)
			live = false
		case gopNot:
			// a run of nots (and the jump that takes it): one truth
			flip := true
			for i+1 < len(ops) && ops[i+1].op == gopNot && !targets[ops[i+1].pc] {
				flip = !flip
				i++
			}
			t := g.top()
			cond := g.truth(t)
			if i+1 < len(ops) && !targets[ops[i+1].pc] && (ops[i+1].op == gopJz || ops[i+1].op == gopJnz) {
				j := ops[i+1]
				i++
				want := j.op == gopJnz
				if flip {
					want = !want
				}
				if cond == "1" || cond == "0" {
					g.pop(1)
					g.matAll()
					if (cond == "1") == want {
						back(j)
					}
					g.jumpIf(cond, want, j.a)
				} else {
					g.line("{")
					g.line("\tconst int t_ = %s;", cond)
					g.pop(1)
					g.matAll()
					back(j)
					g.jumpIf("t_", want, j.a)
					g.line("}")
				}
				break
			}
			switch {
			case cond == "1" && flip, cond == "0" && !flip:
				g.vs[t] = vslot{kind: vsConst, k: -3}
			case cond == "0" || cond == "1":
				g.vs[t] = vslot{kind: vsConst, k: -2}
			default:
				g.line("{")
				g.line("\tconst int t_ = %s;", cond)
				g.line("\tS[%d].t = T_BOOL;", t)
				if flip {
					g.line("\tS[%d].i = !t_;", t)
				} else {
					g.line("\tS[%d].i = t_;", t)
				}
				g.line("}")
				g.vs[t] = vslot{known: vtBool + 1}
			}
		case gopNeg:
			g.mat(d - 1)
			g.line("A_NEG(%d);", d-1)
		case gopPop:
			g.pop(1)
		case gopDup:
			t := g.top()
			if g.vs[t].kind != vsMat {
				g.push(g.vs[t])
			} else {
				g.line("A_DUP(%d);", d)
				g.push(vslot{})
			}
		case gopIndex:
			a, bb := d-2, d-1
			g.line("A_SYNC(%d);", a+2)
			g.line("if (aot_index(&S[%d], &%s, &%s)) goto done;", a, g.ref(a), g.ref(bb))
			g.pop(2)
			g.push(vslot{})
		case gopRet:
			g.line("ret = %s;", g.ref(d-1))
			g.line("goto done;")
			live = false
		case gopRetNil:
			g.line("goto done;")
			live = false
		case gopNative:
			// float() int() abs() len() Vector3() Color(): in place
			type fast struct {
				fn    string
				known int
			}
			f, ok := map[[2]int]fast{
				{3, 1}: {"aot_float", vtFloat + 1}, {2, 1}: {"aot_int", vtInt + 1}, {4, 1}: {"aot_abs", 0},
				{46, 1}: {"aot_len", vtInt + 1}, {39, 1}: {"aot_len", vtInt + 1},
				{28, 3}: {"aot_vec3", vtVec3 + 1}, {29, 3}: {"aot_vec3", vtVec3 + 1},
			}[[2]int{o.a, o.b}]
			if !ok {
				g.matAll()
				g.line("A_NATIVE(%d, %d, %d);", d-o.b, o.a, o.b)
				g.pop(o.b)
				g.push(vslot{})
				break
			}
			var args []string
			for k := d - o.b; k < d; k++ {
				args = append(args, "&"+g.ref(k))
			}
			g.line("%s(&S[%d], %s);", f.fn, d-o.b, strings.Join(args, ", "))
			g.pop(o.b)
			g.push(vslot{known: f.known})
		default:
			// everything else: the whole stack to the frame, then the op
			g.matAll()
			switch o.op {
			case gopSetP:
				g.line("A_SETP(%d, %d);", d-2, o.a)
				g.pop(2)
				g.push(vslot{})
			case gopDiv, gopMod, gopBand, gopBor:
				g.line("A_ARITH(%d, %d);", d-2, o.op)
				g.pop(1)
			case gopIn:
				g.line("A_IN(%d);", d-2)
				g.pop(1)
			case gopCall:
				g.line("A_CALL(%d, %d, %d);", d-o.b, o.a, o.b)
				g.pop(o.b)
				g.push(vslot{})
			case gopArray:
				g.line("A_ARRAY(%d, %d);", d-o.a, o.a)
				g.pop(o.a)
				g.push(vslot{})
			case gopSetIndex:
				g.line("A_SETINDEX(%d);", d-3)
				g.pop(3)
			}
		}
	}
	g.line("AOT_EPILOGUE();")
	fmt.Fprintf(b, "}\n\n")
}
