// golex: the reference. Tokenise a Go source file with go/scanner and write the token stream as
// all byte offsets (little-endian dwords) followed by all token kinds (bytes), or time the scan.
//   golex dump FILE > tokens      golex time FILE [reps]      golex lines FILE OKFILE > tokens
//   golex pcorpus LIST > corpus   golex decls LIST > declarations   golex declstime LIST
package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"fmt"
	"go/ast"
	"go/parser"
	"go/scanner"
	"go/token"
	"os"
	"strconv"
	"time"
	"unicode/utf8"
)

func scan(src []byte, emit func(off int, tok token.Token)) (n int, errs int) {
	fset := token.NewFileSet()
	file := fset.AddFile("", fset.Base(), len(src))
	var s scanner.Scanner
	s.Init(file, src, func(pos token.Position, msg string) { errs++ }, 0)
	for {
		pos, tok, _ := s.Scan()
		if tok == token.EOF {
			break
		}
		n++
		if emit != nil {
			emit(int(pos)-file.Base(), tok)
		}
	}
	return
}

// normal: a file as the corpus holds it, or nil if it is not one for the corpus
func normal(name string, parse bool) ([]byte, *ast.File) {
	b, err := os.ReadFile(name)
	if err != nil || bytes.IndexByte(b, 0) >= 0 || bytes.IndexByte(b, '\r') >= 0 || bytes.HasPrefix(b, []byte("\xef\xbb\xbf")) || !utf8.Valid(b) {
		return nil, nil
	}
	if _, errs := scan(b, nil); errs > 0 {
		return nil, nil
	}
	if len(b) > 0 && b[len(b)-1] != '\n' {
		b = append(b, '\n')
	}
	if !parse {
		return b, nil
	}
	f, err := parser.ParseFile(token.NewFileSet(), "", b, parser.SkipObjectResolution)
	if err != nil {
		return nil, nil
	}
	return b, f
}

// decls: the package-level declarations of the files in LIST that parse, as the offsets of their names in
// the concatenation `pcorpus` writes (dwords), then one byte each: 1 func, 2 method, 3 var, 4 const, 5 type,
// 6 the path of an import, 7 the name in a package clause (so: where each file starts, and what it needs)
func decls(list []byte, timeOnly bool) {
	var offs []uint32
	var kinds []byte
	base := 0
	var parse time.Duration
	for _, name := range bytes.Fields(list) {
		b, _ := normal(string(name), false)
		if b == nil {
			continue
		}
		t := time.Now()
		fset := token.NewFileSet()
		f, err := parser.ParseFile(fset, "", b, parser.SkipObjectResolution)
		parse += time.Since(t)
		if err != nil {
			continue
		}
		at := func(id *ast.Ident, k byte) {
			offs = append(offs, uint32(base+fset.Position(id.Pos()).Offset))
			kinds = append(kinds, k)
		}
		at(f.Name, 7)
		for _, d := range f.Decls {
			switch d := d.(type) {
			case *ast.FuncDecl:
				if d.Recv != nil {
					at(d.Name, 2)
				} else {
					at(d.Name, 1)
				}
			case *ast.GenDecl:
				for _, s := range d.Specs {
					switch s := s.(type) {
					case *ast.ValueSpec:
						for _, n := range s.Names {
							if d.Tok == token.CONST {
								at(n, 4)
							} else {
								at(n, 3)
							}
						}
					case *ast.TypeSpec:
						at(s.Name, 5)
					case *ast.ImportSpec:
						offs = append(offs, uint32(base+fset.Position(s.Path.Pos()).Offset))
						kinds = append(kinds, 6)
					}
				}
			}
		}
		base += len(b)
	}
	if timeOnly {
		fmt.Printf("%d bytes, %d declarations, go/parser: %.1f ms\n", base, len(offs), float64(parse.Microseconds())/1e3)
		return
	}
	w := bufio.NewWriterSize(os.Stdout, 1<<20)
	binary.Write(w, binary.LittleEndian, offs)
	binary.Write(w, binary.LittleEndian, kinds)
	w.Flush()
}

func main() {
	if os.Args[1] == "decls" || os.Args[1] == "declstime" { // golex decls LIST > declarations
		list, _ := os.ReadFile(os.Args[2])
		decls(list, os.Args[1] == "declstime")
		return
	}
	if os.Args[1] == "pcorpus" { // golex pcorpus LIST > corpus.go : like corpus, but only files go/parser accepts
		list, _ := os.ReadFile(os.Args[2])
		w := bufio.NewWriterSize(os.Stdout, 1<<20)
		n := 0
		for _, name := range bytes.Fields(list) {
			if b, f := normal(string(name), true); f != nil {
				w.Write(b)
				n++
			}
		}
		w.Flush()
		fmt.Fprintln(os.Stderr, n, "files")
		return
	}
	if os.Args[1] == "corpus" { // golex corpus LIST > corpus.go : the listed files that scan without error, concatenated
		list, _ := os.ReadFile(os.Args[2])
		w := bufio.NewWriterSize(os.Stdout, 1<<20)
		n := 0
		for _, name := range bytes.Fields(list) {
			b, err := os.ReadFile(string(name))
			if err != nil || bytes.IndexByte(b, 0) >= 0 || bytes.IndexByte(b, '\r') >= 0 || bytes.HasPrefix(b, []byte("\xef\xbb\xbf")) || !utf8.Valid(b) {
				continue
			}
			if _, errs := scan(b, nil); errs > 0 {
				continue
			}
			if len(b) > 0 && b[len(b)-1] != '\n' {
				b = append(b, '\n')
			}
			w.Write(b)
			n++
		}
		w.Flush()
		fmt.Fprintln(os.Stderr, n, "files")
		return
	}
	src, err := os.ReadFile(os.Args[2])
	if err != nil {
		panic(err)
	}
	if os.Args[1] == "lines" { // golex lines FILE OKFILE > tokens : every line is a source of its own; those that scan
		// without error are written to OKFILE, and stdout gets the token dump of OKFILE (for fuzzing)
		var ok []byte
		var offs []uint32
		var kinds []byte
		for _, line := range bytes.Split(bytes.TrimSuffix(src, []byte("\n")), []byte("\n")) {
			one := append(append([]byte{}, line...), '\n')
			if _, errs := scan(one, nil); errs > 0 {
				continue
			}
			base := len(ok)
			scan(one, func(off int, tok token.Token) {
				offs = append(offs, uint32(base+off))
				kinds = append(kinds, byte(tok))
			})
			ok = append(ok, one...)
		}
		os.WriteFile(os.Args[3], ok, 0o644)
		w := bufio.NewWriterSize(os.Stdout, 1<<20)
		binary.Write(w, binary.LittleEndian, offs)
		binary.Write(w, binary.LittleEndian, kinds)
		w.Flush()
		return
	}
	switch os.Args[1] {
	case "dump": // every token's offset as a little-endian dword, then every token's kind as a byte
		var offs []uint32
		var kinds []byte
		_, errs := scan(src, func(off int, tok token.Token) {
			offs = append(offs, uint32(off))
			kinds = append(kinds, byte(tok))
		})
		w := bufio.NewWriterSize(os.Stdout, 1<<20)
		binary.Write(w, binary.LittleEndian, offs)
		binary.Write(w, binary.LittleEndian, kinds)
		w.Flush()
		fmt.Fprintln(os.Stderr, "scanner errors:", errs)
	case "time":
		reps := 5
		if len(os.Args) > 3 {
			reps, _ = strconv.Atoi(os.Args[3])
		}
		best := time.Duration(1 << 62)
		n := 0
		for i := 0; i < reps; i++ {
			t := time.Now()
			n, _ = scan(src, nil)
			if d := time.Since(t); d < best {
				best = d
			}
		}
		fmt.Printf("%d bytes, %d tokens, best of %d: %.1f ms (%.0f MB/s, %.1f ns per token)\n", len(src), n, reps, float64(best.Microseconds())/1e3, float64(len(src))/best.Seconds()/1e6, float64(best.Nanoseconds())/float64(n))
	case "kinds":
		for t := token.Token(0); t < 100; t++ {
			fmt.Println(int(t), t.String())
		}
	}
}
