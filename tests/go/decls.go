// Declarations in every shape the declaration index must follow. Parsed, never compiled.
package p

import (
	alias "x/y"
	. "z"
)

import solo "w"

var a, b, c = 1, f(2, 3), [...]int{4, 5}
var _ = func() int { var inner, more int; type local struct{}; return inner + more }()
var (
	d    = map[string][]int{"a": {1, 2}, "b": {3}}
	e, f func(x, y int) (r, s int)
	g    struct{ h, i int; j func(k int) }
	l    = 1 +
		2
	_, m = <-ch, m2
)
var n interface{ o(p, q int); r() }
var s = t[u, v]{}
const w = iota
const (
	x, y = iota, iota << 1
	z
	aa, bb
	cc = "(" + ")" + "{" + `}` + '(' /* ( */ // (
)
type dd int
type ee = dd
type ff[K comparable, V any] struct{ k K; v V }
type (
	gg struct {
		hh, ii int
		jj     func(kk, ll int) (mm, nn int)
	}
	oo[T any] interface{ pp(T) T; ~int | ~string }
	qq        = func(rr int) (ss, tt int)
	uu        [len(vv)]int
)
type ww func(xx, yy int)
func zz()                              {}
func ab[T any, U interface{ ac() }](ad T, ae ...U) (af, ag T) { return }
func (ah ai) aj(ak, al int) (am int)   { type an int; const ao, ap = 1, 2; var aq = func(ar int) {}; return }
func (*as) at()                        {}
func (au av[aw, ax]) ay()              {}
func (az *ba[bb2]) bc() (bd, be func(bf int) (bg int)) { return }
func _()                               {}
func init()                            { go func() { defer func() {}() }(); if x := (struct{ y int }{1}); x.y > 0 {} }
var bh = [...]struct{ bi, bj int }{{1, 2}, {3, 4}}
var bk, bl = func(bm, bn int) int { return bm }, 5
type bo struct{}; type bp int; var bq, br int; const bs = 1; func bt() {}
var (
	bu int; bv, bw string
)
const bx, by, bz = 1, 2, 3
func noBody(x int) int
func noBodyStruct() struct{ a int }
func resultStruct() struct{ a int } { return struct{ a int }{1} }
func resultIface() interface{ m() } { return nil }
func resultArr() [2]struct{} { var z [2]struct{}; return z }
var lit = func() struct{} { return struct{}{} }
var lit2, lit3 = func() {}, func() int { return 1 }
type fn func() struct{}
func (r struct{ a int }) odd() {}
func lastOne() { x := map[string]struct{}{"a": {}}; _ = x }
