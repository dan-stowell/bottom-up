// Package c has no imports of ours: it initialises first.
package c

import "os"

var Log = []string{"c.var"}

var Name = func() string {
	Log = append(Log, "c.name args="+string(rune('0'+len(os.Args))))
	return "c"
}()

var table = map[string]int{"x": 1, "y": 2} // a map initialiser the linker may drop if table is dead

func init() { Log = append(Log, "c.init") }
