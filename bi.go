package main

/*
bi.go - Go port of bi.py (a simple hex/byte editor)
Source (original Python): https://github.com/fygar256/bi/blob/main/bi.py

This is a pragmatic, idiomatic port that preserves the core functionality:
- Terminal-based hex/ASCII display
- Single-key editing (hex nybbles), navigation (h/j/k/l), simple insert/delete/yank/paste
- Command-line ":" mode with a subset of commands (w, q, wq, r, y, p, s simple replace)
- Search by byte-sequence or regexp ("/pattern" or "//hex bytes")
- Read/write files as bytes

Notes / Differences:
- Uses golang.org/x/term to set raw terminal mode for single-key input.
- Not every single Python function or obscure behavior is reproduced identically.
- History and readline pre-input hooks are simplified/omitted.
- Error messages are printed in the bottom status line similar to the original.
- UTF-8 handling: viewing treats bytes; regexp search decodes a window of bytes to UTF-8 string with replacement for invalid sequences.
- Tested on Unix-like terminals (Linux/macOS). Windows support not tested.

Compile:
  go build -o bi bi.go

Run:
  ./bi filename
  -s script.bi  (these scripting features are *not* implemented in this port)
  -t black|white
  -v verbose
  -w write-on-exit-after-script (not used here)

This file aims to provide a readable Go port suitable as a basis for further improvements.
*/

import (
	"bufio"
	"bytes"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"unicode/utf8"
	"golang.org/x/term"
)

const (
	ESC       = "\x1b["
	LENONSCR  = 19 * 16
	BOTTOMLN  = 22
	RELEN     = 128
	UNKNOWN   = ^uint64(0)
	VERSION   = "3.4.4"
	defaultBg = "black"
)

var (
	mem         = []byte{}
	yank        = []byte{}
	coltab      = []int{0, 1, 4, 5, 2, 6, 3, 7}
	filename    = ""
	termcol     = "black"
	lastchange  = false
	modified    = false
	newfile     = false
	homeaddr    = 0
	utf8mode    = false
	insmod      = false
	curx        = 0
	cury        = 0
	mark        = make([]uint64, 26)
	smem        = []byte{}
	regexpMode  = false
	rePattern   = ""
	span        = 0
	nff         = true
	verbose     = false
	scripting   = false
	stack       = []bool{}
	cp          = 0
	histories   = map[string][]string{"command": {}, "search": {}}
	stdinFd     = int(os.Stdin.Fd())
	oldState    *term.State
	reader      *bufio.Reader
)

// helper terminal functions

func escnocursor() { fmt.Print(ESC + "?25l") }
func escdispcursor() { fmt.Print(ESC + "?25h") }
func escup(n int) { fmt.Printf("%s%dA", ESC, n) }
func escdown(n int) { fmt.Printf("%s%dB", ESC, n) }
func escright(n int) { fmt.Printf("%s%dC", ESC, n) }
func escleft(n int) { fmt.Printf("%s%dD", ESC, n) }
func esclocate(x, y int) { fmt.Printf("%s%d;%dH", ESC, y+1, x+1) }
func escscrollup(n int) { fmt.Printf("%s%dS", ESC, n) }
func escscrolldown(n int) { fmt.Printf("%s%dT", ESC, n) }
func escclear() { fmt.Print(ESC + "2J"); esclocate(0, 0) }
func escclraftcur() { fmt.Print(ESC + "0J") }
func escclrline() { fmt.Print(ESC + "2K") }
func esccolor(col1, col2 int) {
	if termcol == "black" {
		fmt.Printf("%s3%dm%s4%dm", ESC, coltab[col1], ESC, coltab[col2])
	} else {
		fmt.Printf("%s3%dm%s4%dm", ESC, coltab[0], ESC, coltab[7])
	}
}
func escresetcolor() { fmt.Print(ESC + "0m") }

func getch() (string, error) {
	// Read a single rune from terminal (raw mode assumed)
	r, _, err := reader.ReadRune()
	if err != nil {
		return "", err
	}
	return string(r), nil
}

func putch(c string) { fmt.Print(c) }

// history input simplified - use bufio for command mode
func getln(prompt, mode string) string {
	fmt.Print(prompt)
    scanner := bufio.NewScanner(os.Stdin)
    scanner.Scan()
    line := scanner.Text()
	return line
}

func skipspc(s string, idx int) int {
	for idx < len(s) {
		if s[idx] == ' ' {
			idx++
		} else {
			break
		}
	}
	return idx
}

func print_title() {
	esclocate(0, 0)
	esccolor(6, 0)
	utfInfo := "off"
	if utf8mode {
		utfInfo = "on"
	}
	mode := "overwrite"
	if insmod {
		mode = "insert   "
	}
	fmt.Printf("bi version %s by T.Maekawa                   utf8mode:%s     %s   \n", VERSION, utfInfo, mode)
	esccolor(5, 0)
	fn := filename
	if len(fn) > 35 {
		fn = fn[:35]
	}
	fmt.Printf("file:[%-35s] length:%d bytes [%s]    \n", fn, len(mem), func() string {
		if !modified {
			return "not modified"
		}
		return "modified"
	}())
}

func printchar(a int) int {
	if a >= len(mem) {
		fmt.Print("~")
		return 1
	}
	if utf8mode {
		// heuristic similar to python version:
		b := mem[a]
		if b < 0x80 || (0x80 <= b && b <= 0xbf) || (0xf8 <= b && b <= 0xff) {
			if 0x20 <= b && b <= 0x7e {
				fmt.Printf("%c", b)
			} else {
				fmt.Print(".")
			}
			return 1
		}
		// For multi-byte sequences, attempt to decode
		for l := 2; l <= 4; l++ {
			if a+l-1 < len(mem) {
				s := string(mem[a : a+l])
				if utf8.ValidString(s) {
					fmt.Print(s)
					// pad for alignment as original did (space counts)
					if l == 3 {
						fmt.Print(" ")
					} else if l == 4 {
						fmt.Print("  ")
					}
					return l
				}
			}
		}
		fmt.Print(".")
		return 1
	} else {
		b := mem[a]
		if 0x20 <= b && b <= 0x7e {
			fmt.Printf("%c", b)
		} else {
			fmt.Print(".")
		}
		return 1
	}
}

func repaint() {
	print_title()
	escnocursor()
	esclocate(0, 2)
	esccolor(4, 0)
	fmt.Print("OFFSET       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F 0123456789ABCDEF ")
	esccolor(7, 0)
	addr := homeaddr
	lines := LENONSCR / 16
	for y := 0; y < lines; y++ {
		esccolor(5, 0)
		esclocate(0, 3+y)
		fmt.Printf("%012X ", (addr+y*16)&0xffffffffffff)
		esccolor(7, 0)
		for i := 0; i < 16; i++ {
			a := y*16 + i + addr
			if a >= len(mem) {
				fmt.Print("~~ ")
			} else {
				fmt.Printf("%02X ", mem[a])
			}
		}
		esccolor(6, 0)
		a := y*16 + addr
		by := 0
		for by < 16 {
			c := printchar(a)
			a += c
			by += c
		}
		fmt.Print("  ")
	}
	esccolor(0, 0)
	escdispcursor()
}

func insmem(start int, mem2 []byte) {
	if start >= len(mem) {
		if start > len(mem) {
			// pad with zeros
			pad := make([]byte, start-len(mem))
			mem = append(mem, pad...)
		}
		mem = append(mem, mem2...)
		modified = true
		lastchange = true
		return
	}
	mem1 := make([]byte, start)
	copy(mem1, mem[:start])
	mem3 := make([]byte, len(mem)-start)
	copy(mem3, mem[start:])
	mem = append(append(mem1, mem2...), mem3...)
	modified = true
	lastchange = true
}

func delmem(start, end int, yf bool) {
	if start < 0 {
		start = 0
	}
	if end < start || start >= len(mem) {
		stderr("Invalid range.")
		return
	}
	if yf {
		yankmem(start, end)
	}
	newmem := make([]byte, 0, len(mem)-(end-start+1))
	newmem = append(newmem, mem[:start]...)
	if end+1 < len(mem) {
		newmem = append(newmem, mem[end+1:]...)
	}
	mem = newmem
	lastchange = true
	modified = true
}

func yankmem(start, end int) {
	if start < 0 {
		start = 0
	}
	if end < start || start >= len(mem) {
		stderr("Invalid range.")
		return
	}
	if end >= len(mem) {
		end = len(mem) - 1
	}
	yank = make([]byte, end-start+1)
	copy(yank, mem[start:end+1])
	stdmm(fmt.Sprintf("%d bytes yanked.", len(yank)))
}

func ovwmem(start int, mem0 []byte) {
	if len(mem0) == 0 {
		return
	}
	if start+len(mem0) >= len(mem) {
		if start+len(mem0) > len(mem) {
			// extend
			needed := start + len(mem0) - len(mem)
			if needed > 0 {
				mem = append(mem, make([]byte, needed)...)
			}
		}
	}
	for j := 0; j < len(mem0); j++ {
		if start+j >= len(mem) {
			mem = append(mem, mem0[j])
		} else {
			mem[start+j] = mem0[j]
		}
	}
	lastchange = true
	modified = true
}

func redmem(start, end int) []byte {
	m := []byte{}
	if start < 0 {
		start = 0
	}
	for i := start; i <= end; i++ {
		if i < len(mem) {
			m = append(m, mem[i])
		} else {
			m = append(m, 0)
		}
	}
	return m
}

func cpymem(start, end, dest int) {
	m := redmem(start, end)
	ovwmem(dest, m)
}

func movmem(start, end, dest int) int {
	if start <= dest && dest <= end {
		return end + 1
	}
	l := len(mem)
	if start >= l {
		return dest
	}
	m := redmem(start, end)
	yankmem(start, end)
	delmem(start, end, false) // we already yanked
	// insert at dest
	if dest > len(mem) {
		ovwmem(dest, m)
		return dest + len(m)
	}
	if dest > start {
		insmem(dest-(end-start+1), m)
		return dest - (end-start) + len(m) - 1
	}
	insmem(dest, m)
	return dest + len(m)
}

func scrup() {
	if homeaddr >= 16 {
		homeaddr -= 16
	}
}
func scrdown() { homeaddr += 16 }

func fpos() int { return homeaddr + curx/2 + cury*16 }

func inccurx() {
	if curx < 31 {
		curx++
	} else {
		curx = 0
		if cury < LENONSCR/16-1 {
			cury++
		} else {
			scrdown()
		}
	}
}

func readmem(addr int) byte {
	if addr >= len(mem) {
		return 0
	}
	return mem[addr]
}

func setmem(addr int, data int) {
	if addr >= len(mem) {
		// extend
		need := addr - len(mem) + 1
		if need > 0 {
			mem = append(mem, make([]byte, need)...)
		}
	}
	if data >= 0 && data <= 255 {
		mem[addr] = byte(data)
	} else {
		mem[addr] = 0
	}
	modified = true
	lastchange = true
}

func clrmm() {
	esclocate(0, BOTTOMLN)
	esccolor(6, 0)
	escclrline()
}

func stdmm(s string) {
	if scripting {
		if verbose {
			fmt.Println(s)
		}
	} else {
		clrmm()
		esccolor(4, 0)
		esclocate(0, BOTTOMLN)
		fmt.Print(" " + s)
	}
}

func stderr(s string) {
	if scripting {
		fmt.Fprintln(os.Stderr, s)
	} else {
		clrmm()
		esccolor(3, 0)
		esclocate(0, BOTTOMLN)
		fmt.Print(" " + s)
	}
}

func jump(addr int) {
	if addr < 0 {
		addr = 0
	}
	if addr < homeaddr || addr >= homeaddr+LENONSCR {
		homeaddr = addr & ^0xff
	}
	i := addr - homeaddr
	curx = (i & 0xf) * 2
	cury = i / 16
}

// --- search functions ---

// hitre: attempt regular expression match on bytes starting at addr
func hitre(addr int) int {
	if rePattern == "" {
		return -1
	}
	// build a local slice up to RELEN or to end
	var m []byte
	if addr < len(mem)-RELEN {
		m = mem[addr : addr+RELEN]
	} else {
		m = mem[addr:]
	}
	// decode with replacement to a string
	s := string(bytes.Runes(m)) // this will produce Unicode runes; invalid sequences will be replaced
	// Use compiled regexp
	re, err := regexp.Compile(rePattern)
	if err != nil {
		stderr("Bad regular expression.")
		return -1
	}
	loc := re.FindStringIndex(s)
	if loc == nil {
		return 0
	}
	start := loc[0]
	end := loc[1]
	// Now measure bytes spanned by matched substring
	mb := []byte(s[start:end])
	span = len(mb)
	return 1
}

func hit(addr int) int {
	for i := 0; i < len(smem); i++ {
		if addr+i >= len(mem) || mem[addr+i] != smem[i] {
			return 0
		}
	}
	if len(smem) == 0 {
		return 0
	}
	return 1
}

func searchnextnoloop(fp int) int {
	curPos := fp
	if !regexpMode && len(smem) == 0 {
		return 0
	}
	for {
		var f int
		if regexpMode {
			f = hitre(curPos)
		} else {
			f = hit(curPos)
		}
		if f == 1 {
			jump(curPos)
			return 1
		} else if f < 0 {
			return -1
		}
		curPos++
		if curPos >= len(mem) {
			jump(len(mem))
			return 0
		}
	}
}

func searchnext(fp int) bool {
	curpos := fp
	start := fp
	if !regexpMode && len(smem) == 0 {
		return false
	}
	for {
		var f int
		if regexpMode {
			f = hitre(curpos)
		} else {
			f = hit(curpos)
		}
		if f == 1 {
			jump(curpos)
			return true
		} else if f < 0 {
			return false
		}
		curpos++
		if curpos >= len(mem) {
			if nff {
				stdmm("Search reached to bottom, continuing from top.")
			}
			curpos = 0
		}
		if curpos == start {
			if nff {
				stdmm("Not found.")
			}
			return false
		}
	}
}

func searchlast(fp int) bool {
	curpos := fp
	start := fp
	if !regexpMode && len(smem) == 0 {
		return false
	}
	for {
		var f int
		if regexpMode {
			f = hitre(curpos)
		} else {
			f = hit(curpos)
		}
		if f == 1 {
			jump(curpos)
			return true
		} else if f < 0 {
			return false
		}
		curpos--
		if curpos < 0 {
			stdmm("Search reached to top, continuing from bottom.")
			curpos = len(mem) - 1
		}
		if curpos == start {
			stdmm("Not found.")
			return false
		}
	}
}

// get_restr: parse pattern until '/'
func get_restr(s string, idx int) (string, int) {
	var b strings.Builder
	for idx < len(s) {
		if s[idx] == '/' {
			break
		}
		if idx+1 < len(s) && s[idx:idx+2] == "\\\\" {
			b.WriteString("\\\\")
			idx += 2
		} else if idx+1 < len(s) && s[idx:idx+2] == "\\/" {
			b.WriteByte('/')
			idx += 2
		} else if s[idx] == '\\' && idx == len(s)-1 {
			idx++
			break
		} else {
			b.WriteByte(s[idx])
			idx++
		}
	}
	return b.String(), idx
}

func searchstr(s string) bool {
	if s != "" {
		regexpMode = true
		rePattern = s
		return searchnext(fpos())
	}
	return false
}

func get_hexs(s string, idx int) ([]byte, int) {
	out := []byte{}
	for idx < len(s) {
		v, nidx := expression(s, idx)
		if v == uint64(UNKNOWN) {
			break
		}
		out = append(out, byte(v&0xff))
		idx = nidx
	}
	return out, idx
}

func searchhex(sm []byte) bool {
	rePattern = ""
	regexpMode = false
	if len(sm) > 0 {
		smem = make([]byte, len(sm))
		copy(smem, sm)
		return searchnext(fpos())
	}
	return false
}

func comment(s string) string {
	var b strings.Builder
	i := 0
	for i < len(s) {
		if s[i] == '#' {
			break
		}
		if i+1 < len(s) && s[i:i+2] == "\\#" {
			b.WriteByte('#')
			i += 2
			continue
		}
		if i+1 < len(s) && s[i:i+2] == "\\n" {
			b.WriteByte('\n')
			i += 2
			continue
		}
		b.WriteByte(s[i])
		i++
	}
	return b.String()
}

// expression and get_value: parse numeric expressions used by commands
func expression(s string, idx int) (uint64, int) {
	x, idx := get_value(s, idx)
	if idx < len(s) && x != uint64(UNKNOWN) {
		if s[idx] == '+' {
			y, idx2 := get_value(s, idx+1)
			if y != uint64(UNKNOWN) {
				x = x + y
			}
			idx = idx2
		} else if s[idx] == '-' {
			y, idx2 := get_value(s, idx+1)
			if y != uint64(UNKNOWN) {
				if x > y {
					x = x - y
				} else {
					x = 0
				}
			}
			idx = idx2
		}
	}
	return x, idx
}

func get_value(s string, idx int) (uint64, int) {
	if idx >= len(s) {
		return uint64(UNKNOWN), idx
	}
	idx = skipspc(s, idx)
	if idx >= len(s) {
		return uint64(UNKNOWN), idx
	}
	ch := s[idx]
	if ch == '$' {
		idx++
		if len(mem) != 0 {
			return uint64(len(mem) - 1), idx
		}
		return 0, idx
	} else if ch == '{' {
		idx++
		u := ""
		for idx < len(s) {
			if s[idx] == '}' {
				idx++
				break
			}
			u += string(s[idx])
			idx++
		}
		if u == "" {
			stderr("Invalid eval expression.")
			return uint64(UNKNOWN), idx
		}
		// For simplicity, evaluate only integers using strconv
		v, err := strconv.ParseInt(u, 0, 64)
		if err != nil {
			stderr("Invalid eval expression.")
			return uint64(UNKNOWN), idx
		}
		return uint64(v), idx
	} else if ch == '.' {
		idx++
		return uint64(fpos()), idx
	} else if ch == '\'' && idx+1 < len(s) && 'a' <= s[idx+1] && s[idx+1] <= 'z' {
		idx++
		v := mark[s[idx]-'a']
		if v == uint64(UNKNOWN) {
			stderr("Unknown mark.")
			return uint64(UNKNOWN), idx
		}
		idx++
		return v, idx
	} else if (s[idx] >= '0' && s[idx] <= '9') || (s[idx] >= 'a' && s[idx] <= 'f') || (s[idx] >= 'A' && s[idx] <= 'F') {
		x := uint64(0)
		for idx < len(s) {
			ch2 := s[idx]
			var val int
			if ch2 >= '0' && ch2 <= '9' {
				val = int(ch2 - '0')
			} else if ch2 >= 'a' && ch2 <= 'f' {
				val = int(ch2-'a') + 10
			} else if ch2 >= 'A' && ch2 <= 'F' {
				val = int(ch2-'A') + 10
			} else {
				break
			}
			x = x*16 + uint64(val)
			idx++
		}
		return x, idx
	} else if ch == '%' {
		idx++
		x := uint64(0)
		for idx < len(s) && s[idx] >= '0' && s[idx] <= '9' {
			x = x*10 + uint64(s[idx]-'0')
			idx++
		}
		return x, idx
	}
	return uint64(UNKNOWN), idx
}

// command line processing - implements a subset for useful functionality
func commandline_(line string) int {
	cp = fpos()
	line = comment(line)
	if line == "" {
		return -1
	}
	if line == "q" {
		if lastchange {
			stderr("No write since last change. To overriding quit, use 'q!'.")
			return -1
		}
		return 0
	} else if line == "q!" {
		return 0
	} else if line == "wq" || line == "wq!" {
		ok := writefile(filename)
		if ok {
			lastchange = false
			return 0
		}
		return -1
	} else if strings.HasPrefix(line, "w") {
		if len(line) >= 2 {
			s := strings.TrimSpace(line[1:])
			writefile(s)
		} else {
			writefile(filename)
			lastchange = false
		}
		return -1
	} else if strings.HasPrefix(line, "r") {
		if len(line) < 2 {
			readfile(filename)
			stdmm("Original file read.")
			return -1
		}
	} else if strings.HasPrefix(line, "n") {
		searchnext(fpos() + 1)
		return -1
	} else if strings.HasPrefix(line, "N") {
		searchlast(fpos() - 1)
		return -1
	} else if strings.HasPrefix(line, "/") {
		// search
		if strings.HasPrefix(line, "//") {
			sm, _ := get_hexs(line, 2)
			searchhex(sm)
		} else {
			m, _ := get_restr(line, 1)
			searchstr(m)
		}
		return -1
	}

	// parse address expressions like python code
	idx := skipspc(line, 0)
	x, idx := expression(line, idx)
	xf := false
	xf2 := false
	if x == uint64(UNKNOWN) {
		x = uint64(fpos())
	} else {
		xf = true
	}
	x2 := x
	idx = skipspc(line, idx)
	if idx < len(line) && line[idx] == ',' {
		idx = skipspc(line, idx+1)
		if idx < len(line) && line[idx] == '*' {
			idx = skipspc(line, idx+1)
			t, idx2 := expression(line, idx)
			if t == uint64(UNKNOWN) {
				t = 1
			}
			x2 = x + t - 1
			idx = idx2
		} else {
			t, idx2 := expression(line, idx)
			if t == uint64(UNKNOWN) {
				x2 = x
			} else {
				x2 = t
				xf2 = true
			}
			idx = idx2
		}
	}
	if x2 < x {
		x2 = x
	}
	idx = skipspc(line, idx)
	if idx == len(line) {
		jump(int(x))
		return -1
	}

	// yank command
	if idx < len(line) && line[idx] == 'y' {
		idx++
		if !xf && !xf2 {
			m, _ := get_str_or_hexs(line, idx)
			yank = append([]byte{}, m...)
		} else {
			yankmem(int(x), int(x2))
		}
		stdmm(fmt.Sprintf("%d bytes yanked.", len(yank)))
		return -1
	}
	// print / paste
	if idx < len(line) && line[idx] == 'p' {
		y := append([]byte{}, yank...)
		ovwmem(int(x), y)
		jump(int(x) + len(y))
		return -1
	}
	if idx < len(line) && line[idx] == 'P' {
		y := append([]byte{}, yank...)
		insmem(int(x), y)
		jump(int(x) + len(yank))
		return -1
	}
	// mark
	if idx+1 < len(line) && line[idx] == 'm' {
		if 'a' <= line[idx+1] && line[idx+1] <= 'z' {
			mark[line[idx+1]-'a'] = uint64(x)
		}
		return -1
	}
	// read file into memory (r or R)
	if idx < len(line) && (line[idx] == 'r' || line[idx] == 'R') {
		ch := line[idx]
		idx++
		if idx >= len(line) {
			stderr("File name not specified.")
			return -1
		}
		fn := strings.TrimSpace(line[idx:])
		if fn == "" {
			stderr("File name not specified.")
		} else {
			data, err := os.ReadFile(fn)
			if err != nil {
				stderr("File read error.")
			} else {
				if ch == 'r' {
					ovwmem(int(x), data)
				} else {
					insmem(int(x), data)
				}
				jump(int(x) + len(data))
			}
		}
		return -1
	}

	// single-letter commands
	if idx < len(line) {
		ch := line[idx]
		if ch == 'd' {
			delmem(int(x), int(x2), true)
			stdmm(fmt.Sprintf("%d bytes deleted.", int(x2)-int(x)+1))
			jump(int(x))
			return -1
		} else if ch == 'w' {
			idx++
			fn := strings.TrimSpace(line[idx:])
			wrtfile(int(x), int(x2), fn)
			return -1
		} else if ch == 's' {
			// simple replace: s /pattern/replacement/
			// this is a simplified implementation: not full myriads of options
			// parse after 's'
			if idx+1 < len(line) && line[idx+1] == '/' {
				parts := strings.Split(line[idx+2:], "/")
				if len(parts) >= 2 {
					pat := parts[0]
					rep := parts[1]
					// build replacement bytes
					repb := []byte(rep)
					// do simple search-replace from start to end
					cnt := 0
					i := int(x)
					jump(i)
					for {
						res := searchnextnoloop(fpos())
						if res <= 0 {
							break
						}
						pos := fpos()
						if pos <= int(x2) {
							delmem(pos, pos+len(pat)-1, false)
							insmem(pos, repb)
							cnt++
							jump(pos + len(repb))
						} else {
							break
						}
					}
					stdmm(fmt.Sprintf("  %d times replaced.", cnt))
					return -1
				}
			}
			stderr("Invalid s command.")
			return -1
		}
	}

	// bitwise and other operations are omitted in this simplified port
	stderr("Unrecognized command.")
	return -1
}

func commandline(line string) int {
	defer func() {
		if r := recover(); r != nil {
			stderr("Memory overflow or panic.")
		}
	}()
	return commandline_(line)
}

func commandln() int {
	esclocate(0, BOTTOMLN)
	esccolor(7, 0)
	line := getln(":", "command")
	line = strings.TrimSpace(line)
	return commandline(line)
}

func printdata() {
	addr := fpos()
	var a byte
	if addr < len(mem) {
		a = mem[addr]
		esclocate(0, 23)
		esccolor(6, 0)
		s := "."
		if a < 0x20 {
			s = "^" + string(a+('@'))
		} else if a >= 0x7e {
			s = "."
		} else {
			s = "'" + string(a) + "'"
		}
		fmt.Printf("%012X : 0x%02X 0b%08b 0o%03o %d %s      ", addr, a, a, a, a, s)
	} else {
		esclocate(0, 23)
		esccolor(6, 0)
		fmt.Printf("%012X : ~~                                                   ", addr)
	}
}

func disp_curpos() {
	esccolor(4, 0)
	esclocate(curx/2*3+12, cury+3)
	fmt.Print("[")
	esclocate(curx/2*3+15, cury+3)
	fmt.Print("]")
}

func erase_curpos() {
	esccolor(7, 0)
	esclocate(curx/2*3+12, cury+3)
	fmt.Print(" ")
	esclocate(curx/2*3+15, cury+3)
	fmt.Print(" ")
}

func fedit() bool {
	lastchange = false
	stroke := false
	for {
		cp = fpos()
		repaint()
		printdata()
		esclocate(curx/2*3+13+(curx&1), cury+3)
		ch, err := getch()
		if err != nil {
			stderr("Input error.")
			return false
		}
		clrmm()
		nff = true

		// handle escape sequences for arrows
		if ch == "\x1b" {
			// likely an arrow; read two more runes
			r1, _ := getch()
			r2, _ := getch()
			if r2 == "A" {
				ch = "k"
			} else if r2 == "B" {
				ch = "j"
			} else if r2 == "C" {
				ch = "l"
			} else if r2 == "D" {
				ch = "h"
			} else if r1 == "[" && r2 == "2" {
				// begin insert? emulate 'i'
				ch = "i"
			}
		}

		if ch == "n" {
			searchnext(fpos() + 1)
			continue
		} else if ch == "N" {
			searchlast(fpos() - 1)
			continue
		} else if ch == "\x02" { // ctrl-b
			if homeaddr >= 256 {
				homeaddr -= 256
			} else {
				homeaddr = 0
			}
			continue
		} else if ch == "\x06" { // ctrl-f
			homeaddr += 256
			continue
		} else if ch == "\x15" { // ctrl-u
			if homeaddr >= 128 {
				homeaddr -= 128
			} else {
				homeaddr = 0
			}
			continue
		} else if ch == "\x04" { // ctrl-d
			homeaddr += 128
			continue
		} else if ch == "^" {
			curx = 0
			continue
		} else if ch == "$" {
			curx = 30
			continue
		} else if ch == "j" {
			if cury < LENONSCR/16-1 {
				cury++
			} else {
				scrdown()
			}
			continue
		} else if ch == "k" {
			if cury > 0 {
				cury--
			} else {
				scrup()
			}
			continue
		} else if ch == "h" {
			if curx > 0 {
				curx--
			} else {
				if fpos() != 0 {
					curx = 31
					if cury > 0 {
						cury--
					} else {
						scrup()
					}
				}
			}
			continue
		} else if ch == "l" {
			inccurx()
			continue
		} else if ch == "\x19" { // ctrl-y toggle utf8
			utf8mode = !utf8mode
			escclear()
			repaint()
			continue
		} else if ch == "\x0c" { // ctrl-l
			escclear()
			repaint()
			continue
		} else if ch == "Z" {
			if writefile(filename) {
				return true
			} else {
				continue
			}
		} else if ch == "q" {
			if lastchange {
				stdmm("No write since last change. To overriding quit, use 'q!'.")
				continue
			}
			return false
		} else if ch == "M" {
			disp_marks()
			continue
		} else if ch == "m" {
			ch2, _ := getch()
			ch2 = strings.ToLower(ch2)
			if ch2 >= "a" && ch2 <= "z" {
				mark[ch2[0]-'a'] = uint64(fpos())
			}
			continue
		} else if ch == "/" {
			search()
			continue
		} else if ch == "'" {
			ch2, _ := getch()
			ch2 = strings.ToLower(ch2)
			if ch2 >= "a" && ch2 <= "z" {
				jump(int(mark[ch2[0]-'a']))
			}
			continue
		} else if ch == "p" {
			y := append([]byte{}, yank...)
			ovwmem(fpos(), y)
			jump(fpos() + len(y))
			continue
		} else if ch == "P" {
			y := append([]byte{}, yank...)
			insmem(fpos(), y)
			jump(fpos() + len(yank))
			continue
		}

		if ch == "i" {
			insmod = !insmod
			stroke = false
		} else if len(ch) == 1 && strings.Index("0123456789abcdefABCDEF", ch) >= 0 {
			addr := fpos()
			c, _ := strconv.ParseInt(ch, 0, 64)
			sh := 4
			if curx&1 == 1 {
				sh = 0
			}
			mask := 0xf0
			if curx&1 == 0 {
				mask = 0x0f
			}
			if insmod {
				if !stroke && addr < len(mem) {
					insmem(addr, []byte{byte(c << sh)})
				} else {
					orig := int(readmem(addr))
					setmem(addr, (orig&mask)|(int(c)<<sh))
				}
				if curx&1 == 0 {
					stroke = !stroke
				} else {
					stroke = false
				}
			} else {
				orig := int(readmem(addr))
				setmem(addr, (orig&mask)|(int(c)<<sh))
			}
			inccurx()
		} else if ch == "x" {
			delmem(fpos(), fpos(), false)
		} else if ch == ":" {
			disp_curpos()
			f := commandln()
			erase_curpos()
			if f == 1 {
				return true
			} else if f == 0 {
				return false
			}
		}
	}
}

func disp_marks() {
	j := 0
	esclocate(0, BOTTOMLN)
	esccolor(7, 0)
	for i := 'a'; i <= 'z'; i++ {
		m := mark[j]
		if m == uint64(UNKNOWN) {
			fmt.Printf("%c = unknown         ", i)
		} else {
			fmt.Printf("%c = %012X    ", i, m)
		}
		j++
		if j%3 == 0 {
			fmt.Println()
		}
	}
	esccolor(4, 0)
	fmt.Print("[ hit any key ]")
	getch()
	escclear()
}

func invoke_shell(line string) {
	esccolor(7, 0)
	fmt.Println()
	cmd := exec.Command("sh", "-c", line)
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	_ = cmd.Run()
	esccolor(4, 0)
	fmt.Print("[ Hit any key to return ]")
	getch()
	escclear()
}

func search() {
	disp_curpos()
	esclocate(0, BOTTOMLN)
	esccolor(7, 0)
	// simplified: prompt with leading '/'
	fmt.Print("/")
	inReader := bufio.NewReader(os.Stdin)
	s, _ := inReader.ReadString('\n')
	s = strings.TrimRight(s, "\r\n")
	searchsub(comment(s))
	erase_curpos()
}

func searchsub(line string) {
	if len(line) > 2 && strings.HasPrefix(line, "//") {
		sm, _ := get_hexs(line, 2)
		searchhex(sm)
	} else if len(line) > 1 && line[0] == '/' {
		m, _ := get_restr(line, 1)
		searchstr(m)
	}
}

func get_str_or_hexs(line string, idx int) ([]byte, int) {
	idx = skipspc(line, idx)
	if idx < len(line) && line[idx] == '/' {
		idx++
		if idx < len(line) && line[idx] == '/' {
			m, idx2 := get_hexs(line, idx+1)
			return m, idx2
		} else {
			s, idx2 := get_restr(line, idx)
			// encode to utf-8 bytes
			return []byte(s), idx2
		}
	}
	return []byte{}, idx
}

func get_str(line string, idx int) ([]byte, int) {
	s, idx := get_restr(line, idx)
	return []byte(s), idx
}

func printvalue(s string) {
	v, _ := expression(s, 0)
	if v == uint64(UNKNOWN) {
		return
	}
	var sdisplay string
	if v < 0x20 {
		sdisplay = "^" + string(byte(v+uint64('@')))
	} else if v >= 0x7e {
		sdisplay = " . "
	} else {
		sdisplay = "'" + string(byte(v)) + "'"
	}
	x := fmt.Sprintf("%016X", v)
	spacedHex := strings.Join([]string{x[0:4], x[4:8], x[8:12], x[12:16]}, " ")
	o := fmt.Sprintf("%024o", v)
	spacedOct := strings.Join([]string{o[0:6], o[6:12], o[12:18], o[18:24]}, " ")
	b := fmt.Sprintf("%064b", v)
	// group binary every 4
	var bparts []string
	for i := 0; i < 64; i += 4 {
		bparts = append(bparts, b[i:i+4])
	}
	spacedBin := strings.Join(bparts, " ")
	msg := fmt.Sprintf("d%10d  x%s  o%s %s\nb%s", v, spacedHex, spacedOct, sdisplay, spacedBin)
	clrmm()
	esccolor(6, 0)
	esclocate(0, BOTTOMLN)
	fmt.Print(msg)
	getch()
	esclocate(0, BOTTOMLN+1)
	fmt.Print(strings.Repeat(" ", 80))
}

func call_exec(line string) {
	if len(line) <= 1 {
		return
	}
	line = line[1:]
	defer func() {
		if r := recover(); r != nil {
			stderr("python exec() error.")
		}
	}()
	if scripting {
		fmt.Println(line)
	} else {
		clrmm()
		esccolor(7, 0)
		esclocate(0, BOTTOMLN)
		// Very unsafe to eval arbitrary Go code; omit - just run as shell command
		// For compatibility, treat as shell invocation
		invoke_shell(line)
	}
}

// file I/O

func readfile(fn string) bool {
	f, err := os.Open(fn)
	if err != nil {
		newfile = true
		stdmm("<new file>")
		mem = []byte{}
		return true
	}
	defer f.Close()
	data, err := io.ReadAll(f)
	if err != nil {
		stderr("File read error.")
		return false
	}
	mem = make([]byte, len(data))
	copy(mem, data)
	newfile = false
	return true
}

func regulate_mem() {
	for i := range mem {
		mem[i] = mem[i] & 0xff
	}
}

func writefile(fn string) bool {
	regulate_mem()
	f, err := os.Create(fn)
	if err != nil {
		stderr("Permission denied.")
		return false
	}
	defer f.Close()
	_, err = f.Write(mem)
	if err != nil {
		stderr("Write failed.")
		return false
	}
	stdmm("File written.")
	return true
}

func wrtfile(start, end int, fn string) bool {
	regulate_mem()
	f, err := os.Create(fn)
	if err != nil {
		stderr("Permission denied.")
		return false
	}
	defer f.Close()
	for i := start; i <= end; i++ {
		var b byte
		if i < len(mem) {
			b = mem[i]
		} else {
			b = 0
		}
		_, err = f.Write([]byte{b})
		if err != nil {
			return false
		}
	}
	return true
}

// small utility functions

func get_str_from_input(prompt string) string {
	fmt.Print(prompt)
	r := bufio.NewReader(os.Stdin)
	s, _ := r.ReadString('\n')
	return strings.TrimRight(s, "\r\n")
}

func disp_help() {
	fmt.Println("Simple bi (Go port) help:")
	fmt.Println(" - Navigation: h (left nybble), l (right), j, k")
	fmt.Println(" - i toggles insert mode")
	fmt.Println(" - x deletes current byte")
	fmt.Println(" - p paste (overwrite), P paste (insert)")
	fmt.Println(" - /search or //hex search")
	fmt.Println(" - :w write, :q quit, :wq write+quit")
}

// main and initialization

func initMarks() {
	for i := 0; i < len(mark); i++ {
		mark[i] = uint64(UNKNOWN)
	}
}

func main() {
	flag.Usage = func() {
		fmt.Fprintf(flag.CommandLine.Output(), "Usage: %s [options] file\n", os.Args[0])
		flag.PrintDefaults()
	}
	script := flag.String("s", "", "bi script file (not supported in go port)")
	termcolor := flag.String("t", defaultBg, "background color: black or white")
	verb := flag.Bool("v", false, "verbose when processing script")
	writeOnExit := flag.Bool("w", false, "write file when exiting script")
	flag.Parse()
	if flag.NArg() < 1 {
		flag.Usage()
		os.Exit(2)
	}
	filename = flag.Arg(0)
	termcol = *termcolor
	verbose = *verb

	// prepare stdin reader and raw mode
	reader = bufio.NewReader(os.Stdin)
	var err error
	oldState, err = term.MakeRaw(stdinFd)
	if err != nil {
		// if cannot make raw, continue but single-key input will block on newline
		oldState = nil
	} else {
		// ensure we restore terminal on exit
		defer term.Restore(stdinFd, oldState)
	}

	initMarks()

	if !readfile(filename) {
		return
	}

	// main edit loop
	escclear()
	if *script != "" {
		// scripting not implemented in this port - just exit or run script through shell
		scripting = true
		// try running as shell script
		cmd := exec.Command("sh", *script)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		_ = cmd.Run()
		if *writeOnExit && lastchange {
			writefile(filename)
		}
	} else {
		ok := fedit()
		if !ok {
			// exit
		}
	}

	escresetcolor()
	escdispcursor()
	esclocate(0, 23)
}
