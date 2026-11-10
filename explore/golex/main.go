// golex: the reference. Tokenise a Go source file with go/scanner and write the token stream as
// little-endian dword pairs (byte offset, token kind), or time the scan.
//   golex dump FILE > tokens      golex time FILE [reps]
package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"fmt"
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

func main() {
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
	switch os.Args[1] {
	case "dump":
		w := bufio.NewWriterSize(os.Stdout, 1<<20)
		var b [8]byte
		_, errs := scan(src, func(off int, tok token.Token) {
			binary.LittleEndian.PutUint32(b[0:], uint32(off))
			binary.LittleEndian.PutUint32(b[4:], uint32(tok))
			w.Write(b[:])
		})
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
