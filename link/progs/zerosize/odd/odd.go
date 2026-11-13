// Package odd has 3 bytes of zeroed data that needs no alignment, so the package after it starts at an odd
// address.
package odd

var X [3]byte

func Bump() int {
	X[0]++
	return int(X[0])
}
