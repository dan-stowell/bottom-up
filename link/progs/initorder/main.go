// initorder: package initialisation order across packages (the linker schedules it from R_INITORDER),
// variable initialisers that depend on other packages, several init functions per package.
package main

import (
	"fmt"
	"strings"

	"hlinkprogs/initorder/a"
	"hlinkprogs/initorder/c"
)

var first = len(a.Log)

var words = map[string][]string{"main": {"m1", "m2"}}

func init() { a.Log = append(a.Log, "main.init") }

func main() {
	fmt.Println(strings.Join(c.Log, " "))
	fmt.Println(strings.Join(a.Log, " "))
	fmt.Println(first, words)
}
