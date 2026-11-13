// Package zero's only zeroed data are zero-sized static temporaries that need an alignment of 8
// (..stmp_N: size 0, Align 8).
package zero

var (
	P = &[0]uint64{}
	Q = &struct{ _ [0]int64 }{}
)
