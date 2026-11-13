// ifaces: type switches, interface conversions (itabs made by the compiler and by the runtime), reflection
// over method sets (method pruning: reflect.Method keeps exported methods; without it unused methods are
// dropped), types reflect has to find by string (typelinks: SliceOf, MapOf, FuncOf, ...), encoding/json.
package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"reflect"
	"sort"
	"strings"
)

type Animal interface {
	Sound() string
	Legs() int
}

type Named interface{ Name() string }

type Dog struct{ name string }
type Bird struct {
	Species string `json:"species"`
	Wings   int    `json:"wings,omitempty"`
}
type Fish struct{}

func (d Dog) Sound() string    { return "woof" }
func (d Dog) Legs() int        { return 4 }
func (d Dog) Name() string     { return d.name }
func (d *Dog) Rename(n string) { d.name = n }
func (d Dog) Unused() string   { return "never called directly: " + d.name }
func (b *Bird) Sound() string  { return "tweet" }
func (b *Bird) Legs() int      { return 2 }
func (b Bird) String() string  { return "bird:" + b.Species }
func (Fish) Sound() string     { return "" }
func (Fish) Legs() int         { return 0 }

type Stack[T any] struct{ items []T }

func (s *Stack[T]) Push(x T) { s.items = append(s.items, x) }
func (s *Stack[T]) Pop() T {
	x := s.items[len(s.items)-1]
	s.items = s.items[:len(s.items)-1]
	return x
}

type writerFunc func([]byte) (int, error)

func (f writerFunc) Write(p []byte) (int, error) { return f(p) }

func describe(x any) string {
	switch v := x.(type) {
	case nil:
		return "nil"
	case int, int64:
		return fmt.Sprint("int ", v)
	case string:
		return "string " + v
	case Animal:
		s := fmt.Sprint("animal ", v.Sound(), " ", v.Legs())
		if n, ok := v.(Named); ok {
			s += " named " + n.Name()
		}
		if st, ok := v.(fmt.Stringer); ok {
			s += " stringer " + st.String()
		}
		return s
	case error:
		return "error " + v.Error()
	case []any:
		var parts []string
		for _, e := range v {
			parts = append(parts, describe(e))
		}
		return "[" + strings.Join(parts, ", ") + "]"
	case func() int:
		return fmt.Sprint("func ", v())
	case fmt.Stringer:
		return "stringer " + v.String()
	}
	return fmt.Sprintf("other %T", x)
}

func main() {
	d := Dog{"rex"}
	things := []any{nil, 1, int64(2), "s", d, &d, &Bird{Species: "wren"}, Fish{}, io.EOF, []any{1, Fish{}},
		func() int { return 42 }, reflect.TypeOf(0), struct{ X, Y int }{1, 2}, map[string][]int{"a": {1}}}
	for _, x := range things {
		fmt.Println(describe(x))
	}

	// method sets through reflection
	for _, x := range []any{d, &d, &Bird{}, Fish{}, &Stack[int]{}, writerFunc(nil), os.Stdout} {
		t := reflect.TypeOf(x)
		var ms []string
		for i := 0; i < t.NumMethod(); i++ {
			ms = append(ms, t.Method(i).Name)
		}
		if len(ms) > 6 {
			ms = append(ms[:6], fmt.Sprint("... ", t.NumMethod()))
		}
		fmt.Println(t, ms)
	}
	m := reflect.ValueOf(&d).MethodByName("Rename")
	m.Call([]reflect.Value{reflect.ValueOf("max")})
	fmt.Println(d.Name(), reflect.ValueOf(d).MethodByName("Unused").Call(nil)[0])
	var a Animal = &Bird{Species: "owl"}
	fmt.Println(reflect.TypeOf(&a).Elem().NumMethod(), reflect.TypeOf(a).Implements(reflect.TypeOf((*fmt.Stringer)(nil)).Elem()))

	// types reflect builds or finds by their string
	ts := []reflect.Type{
		reflect.SliceOf(reflect.TypeOf(d)),
		reflect.MapOf(reflect.TypeOf(""), reflect.TypeOf([]int(nil))),
		reflect.PointerTo(reflect.TypeOf(Fish{})),
		reflect.ChanOf(reflect.BothDir, reflect.TypeOf(0)),
		reflect.FuncOf([]reflect.Type{reflect.TypeOf(0)}, []reflect.Type{reflect.TypeOf("")}, false),
		reflect.ArrayOf(3, reflect.TypeOf(byte(0))),
		reflect.StructOf([]reflect.StructField{{Name: "A", Type: reflect.TypeOf(0)}, {Name: "B", Type: reflect.TypeOf("")}}),
	}
	var existing any = map[string][]int{}
	fmt.Println(ts[1] == reflect.TypeOf(existing), ts[0] == reflect.TypeOf([]Dog(nil)))
	for _, t := range ts {
		v := reflect.New(t).Elem()
		fmt.Println(t, t.Kind(), t.Size(), v.IsZero())
	}
	mv := reflect.MakeMap(ts[1])
	mv.SetMapIndex(reflect.ValueOf("k"), reflect.ValueOf([]int{1, 2, 3}))
	fmt.Println(mv.Interface().(map[string][]int)["k"])

	// generics and a function type with a method
	s := &Stack[Animal]{}
	s.Push(d)
	s.Push(&Bird{Species: "tit"})
	fmt.Println(s.Pop().Sound(), s.Pop().Sound())
	var sb strings.Builder
	var w io.Writer = writerFunc(func(p []byte) (int, error) { return sb.Write(p) })
	fmt.Fprintf(w, "%v|%d|%s", []Animal{d, Fish{}}, 7, &Bird{Species: "jay"})
	fmt.Println(sb.String())

	// encoding/json: reflection over fields, tags, Stringer and Marshaler lookups
	b, err := json.Marshal(map[string]any{"birds": []*Bird{{"wren", 2}, {Species: "kiwi"}}, "n": 1.5, "ok": true})
	fmt.Println(string(b), err)
	var back struct {
		Birds []Bird
		N     float64
	}
	fmt.Println(json.Unmarshal(b, &back), back.Birds, back.N)
	keys := []string{}
	for _, f := range reflect.VisibleFields(reflect.TypeOf(back)) {
		keys = append(keys, f.Name+":"+f.Type.String())
	}
	sort.Strings(keys)
	fmt.Println(keys)
}
