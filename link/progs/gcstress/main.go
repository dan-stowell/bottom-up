// gcstress: fmt, reflect, maps, goroutines, defer, panic and recover with the collector running all the
// time. Wrong GC bitmaps for globals, wrong stack maps or wrong type data show up as a crash or a wrong sum.
package main

import (
	"fmt"
	"maps"
	"os"
	"reflect"
	"runtime"
	"slices"
	"sort"
	"strings"
	"sync"
)

type node struct {
	left, right *node
	key         int
	val         string
	tags        []string
}

type shape interface {
	area() float64
	name() string
}

type rect struct{ w, h float64 }
type circle struct{ r float64 }

func (r rect) area() float64    { return r.w * r.h }
func (r rect) name() string     { return "rect" }
func (c *circle) area() float64 { return 3 * c.r * c.r }
func (c *circle) name() string  { return "circle" }

// globals with pointers: data (initialised) and bss (zero), scalars in between
var (
	roots   [160]*node
	pad1    [3]int64
	byName  = map[string]*node{"seed": {key: -1, val: "seed"}}
	pad2    uint8
	shapes  = []shape{rect{2, 3}, &circle{1}}
	lastErr error
	table   [16]struct {
		n    int
		p    *node
		s    string
		next *[4]*node
	}
	counter int64
	mu      sync.Mutex
)

func insert(n *node, k int) *node {
	if n == nil {
		return &node{key: k, val: fmt.Sprint("v", k), tags: []string{strings.Repeat("t", k%7)}}
	}
	if k < n.key {
		n.left = insert(n.left, k)
	} else {
		n.right = insert(n.right, k)
	}
	return n
}

func sum(n *node) (s, c int) {
	if n == nil {
		return 0, 0
	}
	ls, lc := sum(n.left)
	rs, rc := sum(n.right)
	return ls + rs + n.key + len(n.val) + len(n.tags[0]), lc + rc + 1
}

func risky(i int) (r int, err error) {
	defer func() {
		if x := recover(); x != nil {
			err = fmt.Errorf("recovered: %v", x)
		}
	}()
	var a []int
	if i%3 == 0 {
		return a[i], nil // index out of range
	}
	if i%3 == 1 {
		var m map[string]int
		m["x"] = i // nil map
	}
	var p *node
	return p.key, nil // nil dereference
}

func main() {
	var wg sync.WaitGroup
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			var local *node
			for i := 0; i < 20000; i++ {
				local = insert(local, (i*7919+g*104729)%10007)
				if i%1000 == 0 {
					mu.Lock()
					roots[g*20+i/1000] = local
					byName[fmt.Sprintf("g%d-%d", g, i)] = local
					if i/1000 < 2 {
						e := &table[g*2+i/1000]
						e.p, e.next, e.s = local, &[4]*node{local, nil, local, nil}, fmt.Sprint(g, i)
					}
					counter++
					mu.Unlock()
					local = nil
				}
			}
		}(g)
	}
	done := make(chan bool)
	go func() {
		for i := 0; i < 50; i++ {
			runtime.GC()
			_ = make([]byte, 1<<20)
		}
		done <- true
	}()
	wg.Wait()
	<-done
	runtime.GC()

	total, count := 0, 0
	for _, r := range roots {
		s, c := sum(r)
		total, count = total+s, count+c
	}
	fmt.Println("roots:", total, count, "counter:", counter, "map:", len(byName))
	keys := slices.Sorted(maps.Keys(byName))
	fmt.Println("keys:", len(keys), keys[0], keys[len(keys)-1])
	ts, tc := 0, 0
	for i := range table {
		s, c := sum(table[i].p)
		ts, tc = ts+s+len(table[i].s), tc+c
		if table[i].next != nil && table[i].next[0] != table[i].p {
			fmt.Println("BAD table", i)
		}
	}
	fmt.Println("table:", ts, tc)

	var errs []string
	for i := 0; i < 6; i++ {
		_, err := risky(i)
		lastErr = err
		errs = append(errs, err.Error())
	}
	sort.Strings(errs)
	fmt.Println(strings.Join(errs, "\n"))

	for _, s := range shapes {
		v := reflect.ValueOf(s)
		fmt.Printf("%s %.1f %v %d methods, kind %v\n", s.name(), s.area(), v.Type(), v.Type().NumMethod(), v.Kind())
	}
	t := reflect.TypeOf(table)
	fmt.Println(t, t.Elem().NumField(), t.Elem().Field(3).Type, reflect.TypeOf(byName).Elem().Elem().Field(4).Type)
	fmt.Printf("%v %+v %#v %T %q %x\n", pad1, rect{1, 2}, circle{3}, lastErr, "q", 255)
	_ = pad2
	if len(os.Args) > 1 {
		panic("asked to panic: " + os.Args[1])
	}
}
