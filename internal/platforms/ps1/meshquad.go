package ps1

// Triangles to quads for the PS1 renderer, done once when the disc is
// built: pairQuads joins two triangles that share an edge (same texture,
// the same colour and UV on the shared corners, flat enough and convex)
// into one quad, one GPU primitive with four corners. A quad costs the
// renderer about what a triangle does, and a track is mostly quads.

import (
	"math"
)

type tri struct {
	v    [3]ps2Vert
	orig int
}

func posKey(v ps2Vert) [3]uint32 {
	return [3]uint32{math.Float32bits(v.x), math.Float32bits(v.y), math.Float32bits(v.z)}
}

// prim: a triangle (d < 0) or a quad a,b,c,d drawn as a,b,c + b,c,d (the
// PS1 GPU's order), corners as source vertices with their UVs.
type prim struct {
	v    [4]ps2Vert
	quad bool
}

func sameCorner(a, b ps2Vert) bool {
	return posKey(a) == posKey(b) && a.r == b.r && a.g == b.g && a.b == b.b && a.u == b.u && a.v == b.v
}

func normal(t tri) [3]float64 {
	ux, uy, uz := float64(t.v[1].x-t.v[0].x), float64(t.v[1].y-t.v[0].y), float64(t.v[1].z-t.v[0].z)
	vx, vy, vz := float64(t.v[2].x-t.v[0].x), float64(t.v[2].y-t.v[0].y), float64(t.v[2].z-t.v[0].z)
	n := [3]float64{uy*vz - uz*vy, uz*vx - ux*vz, ux*vy - uy*vx}
	l := math.Sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2])
	if l == 0 {
		return n
	}
	return [3]float64{n[0] / l, n[1] / l, n[2] / l}
}

// asQuad: t1 and t2 as one quad (t1's winding), or false.
func asQuad(t1, t2 tri) (prim, bool) {
	if t1.v[0].tex != t2.v[0].tex {
		return prim{}, false
	}
	n1, n2 := normal(t1), normal(t2)
	if n1[0]*n2[0]+n1[1]*n2[1]+n1[2]*n2[2] < 0.94 { // within ~20 degrees
		return prim{}, false
	}
	for k := 0; k < 3; k++ {
		q, r := t1.v[(k+1)%3], t1.v[(k+2)%3] // edge q->r in t1, p = t1.v[k]
		for j := 0; j < 3; j++ {
			// t2 has the edge the other way round: r->q
			if sameCorner(t2.v[j], r) && sameCorner(t2.v[(j+1)%3], q) {
				p, s := t1.v[k], t2.v[(j+2)%3]
				// convex: p and s either side of q-r, and q, r either side of p-s
				if !opposite(q, r, p, s, n1) || !opposite(p, s, q, r, n1) {
					return prim{}, false
				}
				return prim{v: [4]ps2Vert{p, q, r, s}, quad: true}, true
			}
		}
	}
	return prim{}, false
}

// a and b on opposite sides of the line e0-e1 (in the plane with normal n)
func opposite(e0, e1, a, b ps2Vert, n [3]float64) bool {
	side := func(x ps2Vert) float64 {
		ex, ey, ez := float64(e1.x-e0.x), float64(e1.y-e0.y), float64(e1.z-e0.z)
		px, py, pz := float64(x.x-e0.x), float64(x.y-e0.y), float64(x.z-e0.z)
		cx, cy, cz := ey*pz-ez*py, ez*px-ex*pz, ex*py-ey*px
		return cx*n[0] + cy*n[1] + cz*n[2]
	}
	sa, sb := side(a), side(b)
	return (sa > 1e-6 && sb < -1e-6) || (sa < -1e-6 && sb > 1e-6)
}

// pairQuads: the triangles as primitives, neighbours (within a few places
// in the list) joined into quads where they can be.
func pairQuads(ts []tri) []prim {
	used := make([]bool, len(ts))
	var out []prim
	for i := range ts {
		if used[i] {
			continue
		}
		used[i] = true
		joined := false
		for j := i + 1; j < len(ts) && j <= i+8; j++ {
			if used[j] {
				continue
			}
			if q, ok := asQuad(ts[i], ts[j]); ok {
				used[j] = true
				out = append(out, q)
				joined = true
				break
			}
		}
		if !joined {
			out = append(out, prim{v: [4]ps2Vert{ts[i].v[0], ts[i].v[1], ts[i].v[2]}})
		}
	}
	return out
}
