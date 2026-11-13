// zerosize: zero-sized values that need an alignment of 8 (package zero), laid out after data that leaves
// the address odd (package odd). The stock linker aligns every symbol, size 0 included; hlink once aligned
// each object's block only to its largest alignment class with bytes in it, so zero's temporaries got odd
// addresses (when, as here, a package's only symbols of a kind are zero-sized, its block was not aligned at
// all). link/secdiff.sh checks the addresses; the program prints whether they are 8-aligned.
package main

import (
	"fmt"
	"unsafe"

	"hlinkprogs/zerosize/odd"
	"hlinkprogs/zerosize/zero"
)

func main() {
	al := func(p unsafe.Pointer) bool { return uintptr(p)%8 == 0 }
	fmt.Println(odd.Bump(), al(unsafe.Pointer(zero.P)), al(unsafe.Pointer(zero.Q)))
}
