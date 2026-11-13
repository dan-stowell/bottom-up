// traceback: runtime.Callers, FuncForPC, file and line, and a panic's traceback through closures, methods,
// deferred calls and goroutines. The pclntab must be right for the text to match the stock-linked binary's.
package main

import (
	"errors"
	"fmt"
	"os"
	"runtime"
	"runtime/debug"
	"strings"
)

type T struct{ n int }

func (t *T) method(depth int) []uintptr {
	if depth > 0 {
		return t.method(depth - 1)
	}
	pcs := make([]uintptr, 32)
	return pcs[:runtime.Callers(0, pcs)]
}

func (t T) value() string {
	_, file, line, _ := runtime.Caller(0)
	return fmt.Sprintf("%s:%d", file[strings.LastIndex(file, "/")+1:], line)
}

//go:noinline
func generic[E any](x E, f func(E) string) string { return f(x) }

func frames(pcs []uintptr) {
	fr := runtime.CallersFrames(pcs)
	for {
		f, more := fr.Next()
		file := f.File[strings.LastIndex(f.File, "/")+1:]
		fmt.Printf("  %s %s:%d\n", f.Function, file, f.Line)
		if !more {
			break
		}
	}
}

func deferred() (s string) {
	defer func() {
		r := recover()
		st := string(debug.Stack())
		n := 0
		for _, l := range strings.Split(st, "\n") {
			if strings.HasPrefix(l, "main.") || strings.HasPrefix(l, "panic(") || strings.HasPrefix(l, "runtime.") {
				if i := strings.LastIndex(l, "("); i > 0 {
					l = l[:i]
				}
				s += l + ";"
				n++
			}
		}
		s += fmt.Sprint(" recovered: ", r, " frames: ", n)
	}()
	var e *T
	func() {
		_ = e.n
	}()
	return
}

func deep(n int, ch chan int) {
	if n == 0 {
		var m map[int]int
		if len(os.Args) > 1 {
			m[1] = 2 // crash with a full traceback
		}
		ch <- 1
		return
	}
	defer func() {}()
	deep(n-1, ch)
}

func main() {
	t := &T{1}
	frames(t.method(3))
	fmt.Println(t.value(), T{2}.value())
	fmt.Println(generic(7, func(i int) string {
		pc, _, line, _ := runtime.Caller(0)
		return fmt.Sprint(runtime.FuncForPC(pc).Name(), " line ", line, " arg ", i)
	}))
	f := runtime.FuncForPC(reflectPC(main))
	file, line := f.FileLine(f.Entry())
	fmt.Println(f.Name(), file[strings.LastIndex(file, "/")+1:], line)
	fmt.Println(deferred())
	err := errors.Join(errors.New("a"), fmt.Errorf("b: %w", os.ErrNotExist))
	fmt.Println(err, errors.Is(err, os.ErrNotExist))
	ch := make(chan int)
	go deep(5, ch)
	<-ch
	fmt.Println("done")
}

func reflectPC(f func()) uintptr {
	pcs := make([]uintptr, 1)
	runtime.Callers(2, pcs)
	_ = f
	return pcs[0]
}
