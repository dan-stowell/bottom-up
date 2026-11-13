// Package a imports b and c; its initialisation must run after theirs.
package a

import (
	"hlinkprogs/initorder/b"
	"hlinkprogs/initorder/c"
)

var Log = append(append([]string{}, c.Log...), "a.var after "+b.Name+" and "+c.Name)

func init() { Log = append(Log, "a.init1") }
func init() { Log = append(Log, "a.init2") }
