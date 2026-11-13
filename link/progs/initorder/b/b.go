// Package b imports c.
package b

import "hlinkprogs/initorder/c"

var Name = func() string {
	c.Log = append(c.Log, "b.var")
	return "b"
}()

func init() { c.Log = append(c.Log, "b.init") }
