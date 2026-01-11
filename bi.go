// Complete Go port of bi.py (a terminal hex editor).
// This implementation aims to cover the features present in the original Python:
// - Terminal raw-mode single-key editing (getch), escape sequence handling
// - Insert / overwrite editing of bytes (hex input), yank/paste, marks
// - Search: byte-sequence search and UTF-8 / regexp search
// - Script execution mode (run commands from a .bi script)
// - Command-line ":" style commands (write, read, move, copy, shift/rotate etc.)
// - File read/write, partial writes, and scripting verbosity
// - Display: hex dump with ASCII/UTF-8 rendering, cursor, colors via ANSI sequences
//
// Build:
//   go get golang.org/x/term
//   go build -o bi bi.go
//
// Notes & Limitations:
// - The original Python used readline history and pre-input hooks; this port keeps a minimal
//   history buffer (in memory) but does not integrate a full readline editing experience.
// - Python's eval() used for { ... } expressions is replaced by Go's numeric parsing only.
// - Arbitrary Python exec() inside the program is replaced with a shell invocation for safety.
// - Behavior should be close to original; some tiny differences may exist due to language/runtime.
package main

import (
	"bufio"
	"bytes"
	"flag"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"unicode/utf8"

	"golang.org/x/term"
)

const ESC = "\033["
const LENONSCR = 19 * 16
const BOTTOMLN = 22
const RELEN = 128

// LARGE sentinel for UNKNOWN which won't be clamped by v<0 checks.
const UNKNOWN = int64(1 << 62)

var (
	mem           []byte
	yank          []byte
	coltab        = []int{0, 1, 4, 5, 2, 6, 3, 7}
	filename      string
	termcol       = "black"
	lastchange    = false
	modified      = false
	newfile       = false
	homeaddr      int64
	utf8mode      = false
	insmod        = false
	curx          = 0
	cury          = 0
	mark          [26]int64
	smem          []byte
	regexpMode    = false
	repsw         = 0
	remem         string
	span          = 0
	nff           = true
	verbose       = false
	scriptingflag = false
	stack         []interface{}
	cp            int64
	histories     = map[string][]string{
		"command": {},
		"search":  {},
	}
	reObj *regexp.Regexp
)

func init() {
	for i := 0; i < 26; i++ {
		mark[i] = UNKNOWN
	}
}

// Terminal control helpers
func escnocursor()  { fmt.Printf("%s?25l", ESC) }
func escdispcursor() { fmt.Printf("%s?25h", ESC) }
func escup(n int)    { fmt.Printf("%s%dA", ESC, n) }
func escdown(n int)  { fmt.Printf("%s%dB", ESC, n) }
func escright(n int) { fmt.Printf("%s%dC", ESC, n) }
func escleft(n int)  { fmt.Printf("%s%dD", ESC, n) }
func esclocate(x, y int) {
	fmt.Printf("%s%d;%dH", ESC, y+1, x+1)
}
func escscrollup(n int)   { fmt.Printf("%s%dS", ESC, n) }
func escscrolldown(n int) { fmt.Printf("%s%dT", ESC, n) }
func escclear() {
	fmt.Printf("%s2J", ESC)
	esclocate(0, 0)
}
func escclraftcur() { fmt.Printf("%s0J", ESC) }
func escclrline()   { fmt.Printf("%s2K", ESC) }

func esccolor(col1, col2 int) {
	if termcol == "black" {
		fmt.Printf("%s3%dm%s4%dm", ESC, coltab[col1], ESC, coltab[col2])
	} else {
		fmt.Printf("%s3%dm%s4%dm", ESC, coltab[0], ESC, coltab[7])
	}
}
func escresetcolor() { fmt.Printf("%s0m", ESC) }

// Raw-mode single char input
func getch() (rune, error) {
	oldState, err := term.MakeRaw(int(syscall.Stdin))
	if err != nil {
		return 0, err
	}
	defer term.Restore(int(syscall.Stdin), oldState)

	var b [4]byte
	n, err := os.Stdin.Read(b[:1])
	if err != nil || n == 0 {
		return 0, err
	}
	r := rune(b[0])
	// If it's escape, try to read additional bytes for arrow sequences (non-blocking isn't trivial)
	if r == 0x1b {
		// read two more bytes if available
		os.Stdin.Read(b[1:2])
		os.Stdin.Read(b[2:3])
	}
	return r, nil
}

func getchByte() (byte, error) {
	oldState, err := term.MakeRaw(int(syscall.Stdin))
	if err != nil {
		return 0, err
	}
	defer term.Restore(int(syscall.Stdin), oldState)

	var b [1]byte
	n, err := os.Stdin.Read(b[:])
	if err != nil || n == 0 {
		return 0, err
	}
	return b[0], nil
}

// I/O helpers
func putch(c string) { fmt.Print(c) }

func getln(prompt, mode string) string {
	if mode != "search" {
		mode = "command"
	}
	// set/restore history is simplified: we keep an in-memory list, but no readline editing
	fmt.Print(prompt)
	reader := bufio.NewReader(os.Stdin)
	line, err := reader.ReadString('\n')
	if err != nil && err != io.EOF {
		return ""
	}
	text := strings.TrimRight(line, "\r\n")
	// store to history
	histories[mode] = append(histories[mode], text)
	return text
}

func skipspc(s string, idx int) int {
	for idx < len(s) && s[idx] == ' ' {
		idx++
	}
	return idx
}

// Printing and display
func print_title() {
	esclocate(0, 0)
	esccolor(6, 0)
	mode := "overwrite"
	if insmod {
		mode = "insert   "
	}
	utfStr := "off"
	if utf8mode {
		utfStr = fmt.Sprintf("%d", repsw)
	}
	fmt.Printf("bi version 3.4.4 by T.Maekawa                   utf8mode:%s     %s   \n", utfStr, mode)
	esccolor(5, 0)
	fn := filename
	if len(fn) > 35 {
		fn = fn[:35]
	}
	mod := "not modified"
	if modified {
		mod = "modified"
	}
	fmt.Printf("file:[%-35s] length:%d bytes [%s]    \n", fn, len(mem), mod)
}

func printchar(a int64) int {
	if a >= int64(len(mem)) {
		fmt.Print("~")
		return 1
	}
	if utf8mode {
		b := mem[a]
		if b < 0x80 || (0x80 <= b && b <= 0xbf) || (0xf8 <= b && b <= 0xff) {
			if 0x20 <= b && b <= 0x7e {
				fmt.Printf("%c", b)
			} else {
				fmt.Print(".")
			}
			return 1
		} else if 0xc0 <= b && b <= 0xdf {
			if a+1 < int64(len(mem)) {
				s := mem[a : a+2]
				if utf8.Valid(s) {
					fmt.Print(string(s))
					return 2
				}
			}
			fmt.Print(".")
			return 1
		} else if 0xe0 <= b && b <= 0xef {
			if a+2 < int64(len(mem)) {
				s := mem[a : a+3]
				if utf8.Valid(s) {
					fmt.Print(string(s)+" ")
					return 3
				}
			}
			fmt.Print(".")
			return 1
		} else if 0xf0 <= b && b <= 0xf7 {
			if a+3 < int64(len(mem)) {
				s := mem[a : a+4]
				if utf8.Valid(s) {
					fmt.Print(string(s)+"  ")
					return 4
				}
			}
			fmt.Print(".")
			return 1
		}
	}
	// ascii fallback
	ch := mem[a]
	if 0x20 <= ch && ch <= 0x7e {
		fmt.Printf("%c", ch)
	} else {
		fmt.Print(".")
	}
	return 1
}

func repaint() {
	print_title()
	escnocursor()
	esclocate(0, 2)
	esccolor(4, 0)
	fmt.Print("OFFSET       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F 0123456789ABCDEF ")
	esccolor(7, 0)
	addr := homeaddr
	for y := 0; y < LENONSCR/16; y++ {
		esccolor(5, 0)
		esclocate(0, 3+y)
		fmt.Printf("%012X ", (addr+int64(y*16))&0xffffffffffff)
		esccolor(7, 0)
		for i := 0; i < 16; i++ {
			a := int64(y*16 + i)
			pos := a + addr
			if pos >= int64(len(mem)) {
				fmt.Print("~~ ")
			} else {
				fmt.Printf("%02X ", mem[pos]&0xff)
			}
		}
		esccolor(6, 0)
		a := addr + int64(y*16)
		by := 0
		for by < 16 {
			c := printchar(a)
			a += int64(c)
			by += c
		}
		fmt.Print("  ")
	}
	esccolor(0, 0)
	escdispcursor()
}

// Memory ops
func insmem(start int64, mem2 []byte) {
	if start >= int64(len(mem)) {
		if start > int64(len(mem)) {
			// pad with zeros
			mem = append(mem, make([]byte, int(start)-len(mem))...)
		}
		mem = append(mem, mem2...)
		modified = true
		lastchange = true
		return
	}
	mem = append(mem[:start], append(mem2, mem[start:]...)...)
	modified = true
	lastchange = true
}

func delmem(start, end int64, yf bool) {
	length := end - start + 1
	if length <= 0 || start >= int64(len(mem)) {
		stderr("Invalid range.")
		return
	}
	if yf {
		yankmem(start, end)
	}
	if start < 0 {
		start = 0
	}
	if end >= int64(len(mem)) {
		end = int64(len(mem) - 1)
	}
	mem = append(mem[:start], mem[end+1:]...)
	lastchange = true
	modified = true
}

func yankmem(start, end int64) {
	length := end - start + 1
	if length <= 0 || start >= int64(len(mem)) {
		stderr("Invalid range.")
		return
	}
	yank = []byte{}
	cnt := 0
	for j := start; j <= end; j++ {
		if j < int64(len(mem)) {
			cnt++
			yank = append(yank, mem[j]&0xff)
		}
	}
	stdmm(fmt.Sprintf("%d bytes yanked.", cnt))
}

func ovwmem(start int64, mem0 []byte) {
	if len(mem0) == 0 {
		return
	}
	endNeeded := int(start) + len(mem0)
	if endNeeded > len(mem) {
		// extend mem
		if endNeeded > cap(mem) {
			// no-op; append will grow
		}
		padding := endNeeded - len(mem)
		if padding > 0 {
			mem = append(mem, make([]byte, padding)...)
		}
	}
	for j := 0; j < len(mem0); j++ {
		pos := start + int64(j)
		mem[pos] = mem0[j] & 0xff
	}
	lastchange = true
	modified = true
}

func redmem(start, end int64) []byte {
	m := []byte{}
	for i := start; i <= end; i++ {
		if i >= 0 && i < int64(len(mem)) {
			m = append(m, mem[i]&0xff)
		} else {
			m = append(m, 0)
		}
	}
	return m
}

func cpymem(start, end, dest int64) {
	ovwmem(dest, redmem(start, end))
}

func movmem(start, end, dest int64) int64 {
	if start <= dest && dest <= end {
		return end + 1
	}
	l := int64(len(mem))
	if start >= l {
		return dest
	}
	m := redmem(start, end)
	yankmem(start, end)
	delmem(start, end, false)
	if dest > l {
		ovwmem(dest, m)
		return dest + int64(len(m))
	} else {
		if dest > start {
			insmem(dest-(end-start+1), m)
			return dest - (end-start) + int64(len(m)) - 1
		} else {
			insmem(dest, m)
			return dest + int64(len(m))
		}
	}
}

// Scrolling & cursor
func scrup() {
	if homeaddr >= 16 {
		homeaddr -= 16
	}
}
func scrdown() { homeaddr += 16 }
func fpos() int64 {
	return homeaddr + int64(curx/2) + int64(cury*16)
}
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

func readmem(addr int64) int {
	if addr >= int64(len(mem)) || addr < 0 {
		return 0
	}
	return int(mem[addr] & 0xff)
}

func setmem(addr int64, data int) {
	if addr >= int64(len(mem)) {
		padding := int(addr) - len(mem) + 1
		if padding > 0 {
			mem = append(mem, make([]byte, padding)...)
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

// message helpers
func clrmm() {
	esclocate(0, BOTTOMLN)
	esccolor(6, 0)
	escclrline()
}

func stdmm(s string) {
	if scriptingflag {
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
	if scriptingflag {
		fmt.Fprintln(os.Stderr, s)
	} else {
		clrmm()
		esccolor(3, 0)
		esclocate(0, BOTTOMLN)
		fmt.Print(" " + s)
	}
}

func jump(addr int64) {
	if addr < homeaddr || addr >= homeaddr+LENONSCR {
		homeaddr = addr &^ 0xff
	}
	i := addr - homeaddr
	curx = int((i & 0xf) * 2)
	cury = int(i / 16)
}

func disp_marks() {
	j := 0
	esclocate(0, BOTTOMLN)
	esccolor(7, 0)
	for i := 0; i < 26; i++ {
		if mark[i] == UNKNOWN {
			fmt.Printf("%c = unknown         ", 'a'+i)
		} else {
			fmt.Printf("%c = %012X    ", 'a'+i, mark[i])
		}
		j++
		if j%3 == 0 {
			fmt.Println()
		}
	}
	esccolor(4, 0)
	fmt.Print("[ hit any key ]")
	getchByte()
	escclear()
}

func invoke_shell(line string) {
	esccolor(7, 0)
	fmt.Println()
	// Use /bin/sh -c semantics
	cmd := exec.Command("/bin/sh", "-c", strings.TrimSpace(line))
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Run()
	esccolor(4, 0)
	fmt.Print("[ Hit any key to return ]")
	getchByte()
	escclear()
}

// expression parsing
func expression(s string, idx int) (int64, int) {
	x, idx := get_value(s, idx)
	if idx < len(s) && x != UNKNOWN && s[idx] == '+' {
		y, idx2 := get_value(s, idx+1)
		x = x + y
		idx = idx2
	} else if idx < len(s) && x != UNKNOWN && s[idx] == '-' {
		y, idx2 := get_value(s, idx+1)
		x = x - y
		if x < 0 {
			x = 0
		}
		idx = idx2
	}
	return x, idx
}

func get_value(s string, idx int) (int64, int) {
	if idx >= len(s) {
		return UNKNOWN, idx
	}
	idx = skipspc(s, idx)
	if idx >= len(s) {
		return UNKNOWN, idx
	}
	ch := s[idx]
	if ch == '$' {
		idx++
		if len(mem) != 0 {
			return int64(len(mem) - 1), idx
		}
		return int64(0), idx
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
			return UNKNOWN, idx
		}
		// try to parse as integer literal in base auto (0x.. 0..)
		v, err := strconv.ParseInt(u, 0, 64)
		if err != nil {
			stderr("Invalid eval expression.")
			return UNKNOWN, idx
		}
		if v < 0 {
			v = 0
		}
		return v, idx
	} else if ch == '.' {
		idx++
		return fpos(), idx
	} else if ch == '\'' && idx+1 < len(s) && s[idx+1] >= 'a' && s[idx+1] <= 'z' {
		idx++
		v := mark[s[idx]-'a']
		if v == UNKNOWN {
			stderr("Unknown mark.")
			return UNKNOWN, idx - 1
		} else {
			idx++
			return v, idx
		}
	} else if strings.IndexAny(string(ch), "0123456789abcdefABCDEF") >= 0 {
		var x int64
		for idx < len(s) && strings.IndexAny(string(s[idx]), "0123456789abcdefABCDEF") >= 0 {
			d := string(s[idx])
			v, _ := strconv.ParseInt(d, 16, 64)
			x = 16*x + v
			idx++
		}
		if x < 0 {
			x = 0
		}
		return x, idx
	} else if ch == '%' {
		idx++
		var x int64
		for idx < len(s) && s[idx] >= '0' && s[idx] <= '9' {
			x = x*10 + int64(s[idx]-'0')
			idx++
		}
		if x < 0 {
			x = 0
		}
		return x, idx
	}
	return UNKNOWN, idx
}

// Search helpers
func searchnextnoloop(fp int64) int {
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
		if curPos >= int64(len(mem)) {
			jump(int64(len(mem)))
			return 0
		}
	}
}

func scommand(start, end int64, xf, xf2 bool, line string, idx int) {
	nff = false
	pos := fpos()
	idx = skipspc(line, idx)
	if !xf && !xf2 {
		start = 0
		end = int64(len(mem) - 1)
	}
	if idx < len(line) && line[idx] == '/' {
		idx++
		if idx < len(line) && line[idx] != '/' {
			m, idx2 := get_restr(line, idx)
			idx = idx2
			regexpMode = true
			remem = m
			span = len(m)
			var err error
			reObj, err = regexp.Compile(remem)
			if err != nil {
				stderr("Bad regular expression.")
				return
			}
		} else if idx < len(line) && line[idx] == '/' {
			var idx2 int
			smem, idx2 = get_hexs(line, idx+1)
			idx = idx2
			regexpMode = false
			remem = ""
			span = len(smem)
		} else {
			stderr("Invalid syntax.")
			return
		}
	}
	if span == 0 {
		stderr("Specify search object.")
		return
	}
	n, idx := get_str_or_hexs(line, idx)
	i := start
	cnt := 0
	jump(i)
	for {
		f := searchnextnoloop(fpos())
		i = fpos()
		if f < 0 {
			return
		} else if i <= end && f == 1 {
			delmem(i, i+int64(span)-1, false)
			insmem(i, n)
			pos = i + int64(len(n))
			cnt++
			i = pos
			jump(i)
		} else {
			jump(pos)
			stdmm(fmt.Sprintf("  %d times replaced.", cnt))
			return
		}
	}
}

func opeand(x, x2 int64, x3 int) {
	for i := x; i <= x2; i++ {
		setmem(i, readmem(i)&(x3&0xff))
	}
	stdmm(fmt.Sprintf("%d bytes anded.", x2-x+1))
}
func opeor(x, x2 int64, x3 int) {
	for i := x; i <= x2; i++ {
		setmem(i, readmem(i)|(x3&0xff))
	}
	stdmm(fmt.Sprintf("%d bytes ored.", x2-x+1))
}
func opexor(x, x2 int64, x3 int) {
	for i := x; i <= x2; i++ {
		setmem(i, readmem(i)^(x3&0xff))
	}
	stdmm(fmt.Sprintf("%d bytes xored.", x2-x+1))
}
func openot(x, x2 int64) {
	for i := x; i <= x2; i++ {
		setmem(i, ^readmem(i)&0xff)
	}
	stdmm(fmt.Sprintf("%d bytes noted.", x2-x+1))
}

func hitre(addr int64) int {
	if remem == "" {
		return -1
	}
	span = 0

	var m []byte
	if addr < int64(len(mem))-RELEN {
		m = mem[addr : addr+RELEN]
	} else if addr < int64(len(mem)) {
		m = mem[addr:]
	} else {
		return 0
	}

	// Python の decode('utf-8','replace') 相当を得るために
	// Go の string(m) を使う（無効バイトは UTF-8 の置換文字に変換される）
	ms := string(m)

	// remem が変更された可能性を考え、reObj を確実に（再）コンパイルする
	if reObj == nil || reObj.String() != remem {
		var err error
		reObj, err = regexp.Compile(remem)
		if err != nil {
			stderr("Bad regular expression.")
			return -1
		}
	}

	// Python の re.match と同等に「先頭マッチのみ」を受け入れる
	loc := reObj.FindStringIndex(ms)
	if loc == nil {
		return 0
	}
	// 先頭以外で見つかった場合はヒットとみなさない（cur_pos からの先頭マッチを逐次試す方式）
	if loc[0] != 0 {
		return 0
	}

	// マッチ長をバイト長で計算して span に設定
	start, end := loc[0], loc[1]
	matched := ms[start:end]
	span = len([]byte(matched))
	return 1
}

func hit(addr int64) int {
	for i := 0; i < len(smem); i++ {
		if addr+int64(i) < int64(len(mem)) && mem[addr+int64(i)] == smem[i] {
			continue
		} else {
			return 0
		}
	}
	return 1
}

func searchnext(fp int64) bool {
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
		if curpos >= int64(len(mem)) {
			if nff {
				stdmm("Search reached to bottom, continuing from top.")
			}
			curpos = 0
			esccolor(0, 0)
		}
		if curpos == start {
			if nff {
				stdmm("Not found.")
			}
			return false
		}
	}
}

func searchlast(fp int64) bool {
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
			esccolor(0, 0)
			curpos = int64(len(mem) - 1)
		}
		if curpos == start {
			stdmm("Not found.")
			return false
		}
	}
}

func get_restr(s string, idx int) (string, int) {
	var b strings.Builder
	for idx < len(s) {
		if s[idx] == '/' {
			break
		}
		if idx+1 < len(s) && s[idx:idx+2] == "\\\\" {
			b.WriteString("\\\\")
			idx += 2
		} else if idx+1 < len(s) && s[idx] == '\\' && s[idx+1] == '/' {
			b.WriteByte('/')
			idx += 2
		} else if s[idx] == '\\' && idx+1 == len(s) {
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
		remem = s
		reObj, _ = regexp.Compile(remem)
		return searchnext(fpos())
	}
	return false
}

func searchsub(line string) bool {
	if len(line) > 2 && line[:2] == "//" {
		sm, _ := get_hexs(line, 2)
		return searchhex(sm)
	} else if len(line) > 1 && line[0] == '/' {
		m, _ := get_restr(line, 1)
		return searchstr(m)
	}
	return false
}

func search() {
    disp_curpos()
    esclocate(0, BOTTOMLN)
    esccolor(7, 0)

    // 表示用に先頭のスラッシュを出す（Python の readline pre_input_hook と同等の振る舞い）
    fmt.Print("/")

    // ユーザー入力を取得（ユーザーがスラッシュ以降だけ入力する想定）
    s := getln("", "search")

    // comment() は元の文字列のエスケープや '#' 処理を行うため、
    // searchsub に渡す際に必ず先頭に '/' を付与する
    searchsub("/" + comment(s))

    erase_curpos()
}

func get_hexs(s string, idx int) ([]byte, int) {
	m := []byte{}
	for idx < len(s) {
		v, idx2 := expression(s, idx)
		idx = idx2
		if v == UNKNOWN {
			break
		}
		m = append(m, byte(v&0xff))
	}
	return m, idx
}

func searchhex(sm []byte) bool {
	remem = ""
	regexpMode = false
	if len(sm) > 0 {
		smem = sm
		return searchnext(fpos())
	}
	return false
}

func comment(s string) string {
	idx := 0
	var b strings.Builder
	for idx < len(s) {
		if s[idx] == '#' {
			break
		}
		if idx+1 < len(s) && s[idx:idx+2] == "\\#" {
			b.WriteByte('#')
			idx += 2
			continue
		}
		if idx+1 < len(s) && s[idx:idx+2] == "\\n" {
			b.WriteByte('\n')
			idx += 2
			continue
		}
		b.WriteByte(s[idx])
		idx++
	}
	return b.String()
}

func scripting(scriptfile string) int {
	fh, err := os.Open(scriptfile)
	if err != nil {
		stderr("Script file open error.")
		return -1
	}
	defer fh.Close()
	scanner := bufio.NewScanner(fh)
	scriptingflag = true
	flagv := -1
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if verbose {
			fmt.Println(line)
		}
		flagv = commandline(line)
		if flagv == 0 {
			return 0
		} else if flagv == 1 {
			return 1
		}
	}
	return 0
}

// Bitwise and rotate/shift functions
func left_shift_byte(x, x2 int64, c int) {
	for i := x; i <= x2; i++ {
		setmem(i, (readmem(i)<<1)|(c&1))
	}
}
func right_shift_byte(x, x2 int64, c int) {
	for i := x; i <= x2; i++ {
		setmem(i, (readmem(i)>>1)|((c&1)<<7))
	}
}
func left_rotate_byte(x, x2 int64) {
	for i := x; i <= x2; i++ {
		m := readmem(i)
		c := (m & 0x80) >> 7
		setmem(i, (m<<1)|c)
	}
}
func right_rotate_byte(x, x2 int64) {
	for i := x; i <= x2; i++ {
		m := readmem(i)
		c := (m & 0x01) << 7
		setmem(i, (m>>1)|c)
	}
}

func get_multibyte_value(x, x2 int64) int64 {
	v := int64(0)
	for i := x2; i >= x; i-- {
		v = (v << 8) | int64(readmem(i))
	}
	return v
}
func put_multibyte_value(x, x2 int64, v int64) {
	for i := x; i <= x2; i++ {
		setmem(i, int(v&0xff))
		v >>= 8
	}
}
func left_shift_multibyte(x, x2 int64, c int) {
	v := get_multibyte_value(x, x2)
	put_multibyte_value(x, x2, (v<<1)|int64(c))
}
func right_shift_multibyte(x, x2 int64, c int) {
	v := get_multibyte_value(x, x2)
	put_multibyte_value(x, x2, (v>>1)|(int64(c)<<((x2-x)*8+7)))
}
func left_rotate_multibyte(x, x2 int64) {
	v := get_multibyte_value(x, x2)
	c := 0
	if v&(1<<((x2-x)*8+7)) != 0 {
		c = 1
	}
	put_multibyte_value(x, x2, (v<<1)|int64(c))
}
func right_rotate_multibyte(x, x2 int64) {
	v := get_multibyte_value(x, x2)
	c := 0
	if v&0x1 != 0 {
		c = 1
	}
	put_multibyte_value(x, x2, (v>>1)|(int64(c)<<((x2-x)*8+7)))
}

func shift_rotate(x, x2 int64, times, bit int64, multibyte bool, direction byte) {
	for i := int64(0); i < times; i++ {
		if !multibyte {
			if bit != 0 && bit != 1 {
				if direction == '<' {
					left_rotate_byte(x, x2)
				} else {
					right_rotate_byte(x, x2)
				}
			} else {
				if direction == '<' {
					left_shift_byte(x, x2, int(bit&1))
				} else {
					right_shift_byte(x, x2, int(bit&1))
				}
			}
		} else {
			if bit != 0 && bit != 1 {
				if direction == '<' {
					left_rotate_multibyte(x, x2)
				} else {
					right_rotate_multibyte(x, x2)
				}
			} else {
				if direction == '<' {
					left_shift_multibyte(x, x2, int(bit&1))
				} else {
					right_shift_multibyte(x, x2, int(bit&1))
				}
			}
		}
	}
}

// string/hex input parsing
func get_str_or_hexs(line string, idx int) ([]byte, int) {
	idx = skipspc(line, idx)
	if idx < len(line) && line[idx] == '/' {
		idx++
		if idx < len(line) && line[idx] == '/' {
			return get_hexs(line, idx+1)
		}
		s, idx2 := get_restr(line, idx)
		return []byte(s), idx2
	}
	return []byte{}, idx
}
func get_str(line string, idx int) ([]byte, int) {
	s, idx2 := get_restr(line, idx)
	return []byte(s), idx2
}

func printvalue(s string) {
	v, _ := expression(s, 0)
	if v == UNKNOWN {
		return
	}
	vis := " . "
	if v < 0x20 {
		vis = fmt.Sprintf("^%c ", rune(v+int64('@')))
	} else if v >= 0x7e {
		vis = " . "
	} else {
		vis = fmt.Sprintf("'%c'", rune(v))
	}
	x := fmt.Sprintf("%016X", v)
	spacedHex := strings.Join(splitEvery(x, 4), " ")
	o := fmt.Sprintf("%024o", v)
	spacedOct := strings.Join(splitEvery(o, 4), " ")
	b := fmt.Sprintf("%064b", v)
	spacedBin := strings.Join(splitEvery(b, 4), " ")
	msg := fmt.Sprintf("d%10d  x%s  o%s %s\nb%s", v, spacedHex, spacedOct, vis, spacedBin)
	if scriptingflag {
		if verbose {
			fmt.Println(msg)
		}
	} else {
		clrmm()
		esccolor(6, 0)
		esclocate(0, BOTTOMLN)
		fmt.Print(msg)
		getchByte()
		esclocate(0, BOTTOMLN+1)
		fmt.Print(strings.Repeat(" ", 80))
	}
}

func splitEvery(s string, n int) []string {
	out := []string{}
	for i := 0; i < len(s); i += n {
		end := i + n
		if end > len(s) {
			end = len(s)
		}
		out = append(out, s[i:end])
	}
	return out
}

func call_exec(line string) {
	if len(line) <= 1 {
		return
	}
	line = line[1:]
	// For safety, run shell commands rather than executing Go code dynamically.
	invoke_shell(line)
}

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
		if writefile(filename) {
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
	} else if strings.HasPrefix(line, "T") || strings.HasPrefix(line, "t") {
		if len(line) >= 2 {
			s := strings.TrimSpace(line[1:])
			stack = append(stack, scriptingflag)
			stack = append(stack, verbose)
			if line[0] == 'T' {
				verbose = true
			} else {
				verbose = false
			}
			fmt.Println("")
			scripting(s)
			if verbose {
				stdmm("[ Hit any key ]")
				getchByte()
			}
			// restore
			if len(stack) >= 2 {
				verbose = stack[len(stack)-1].(bool)
				stack = stack[:len(stack)-1]
				scriptingflag = stack[len(stack)-1].(bool)
				stack = stack[:len(stack)-1]
			}
			escclear()
			return -1
		} else {
			stderr("Specify script file name.")
			return -1
		}
	} else if strings.HasPrefix(line, "n") {
		searchnext(fpos() + 1)
		return -1
	} else if strings.HasPrefix(line, "N") {
		searchlast(fpos() - 1)
		return -1
	} else if strings.HasPrefix(line, "@") {
		call_exec(line)
		return -1
	} else if strings.HasPrefix(line, "!") {
		if len(line) >= 2 {
			invoke_shell(line[1:])
			return -1
		}
		return -1
	} else if strings.HasPrefix(line, "?") {
		printvalue(line[1:])
		return -1
	} else if strings.HasPrefix(line, "/") {
		searchsub(line)
		return -1
	}
	idx := skipspc(line, 0)
	x, idx := expression(line, idx)
	xf := false
	xf2 := false
	if x == UNKNOWN {
		x = fpos()
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
			idx = idx2
			if t == UNKNOWN {
				t = 1
			}
			x2 = x + t - 1
		} else {
			t, idx2 := expression(line, idx)
			idx = idx2
			if t == UNKNOWN {
				x2 = x
			} else {
				x2 = t
				xf2 = true
			}
		}
	} else {
		x2 = x
	}
	if x2 < x {
		x2 = x
	}
	idx = skipspc(line, idx)
	if idx == len(line) {
		jump(x)
		return -1
	}
	if idx < len(line) && line[idx] == 'y' {
		idx++
		if !xf && !xf2 {
			m, _ := get_str_or_hexs(line, idx)
			yank = append([]byte{}, m...)
		} else {
			yankmem(x, x2)
		}
		stdmm(fmt.Sprintf("%d bytes yanked.", len(yank)))
		return -1
	}
	if idx < len(line) && line[idx] == 'p' {
		y := append([]byte{}, yank...)
		ovwmem(x, y)
		jump(x + int64(len(y)))
		return -1
	}
	if idx < len(line) && line[idx] == 'P' {
		y := append([]byte{}, yank...)
		insmem(x, y)
		jump(x + int64(len(yank)))
		return -1
	}
	if idx+1 < len(line) && line[idx] == 'm' {
		if 'a' <= line[idx+1] && line[idx+1] <= 'z' {
			mark[line[idx+1]-'a'] = x
		}
		return -1
	}
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
			data, err := ioutil.ReadFile(fn)
			if err != nil {
				data = []byte{}
				stderr("File read error.")
			}
			if ch == 'r' {
				ovwmem(x, data)
			} else {
				insmem(x, data)
			}
			jump(x + int64(len(data)))
			return -1
		}
	}
	var ch byte = 0
	if idx < len(line) {
		ch = line[idx]
	}
	if ch == 'd' {
		delmem(x, x2, true)
		stdmm(fmt.Sprintf("%d bytes deleted.", x2-x+1))
		jump(x)
		return -1
	} else if ch == 'w' {
		idx++
		fn := strings.TrimSpace(line[idx:])
		wrtfile(x, x2, fn)
		return -1
	} else if ch == 's' {
		scommand(x, x2, xf, xf2, line, idx+1)
		return -1
	}
	if idx < len(line) && line[idx] == '~' {
		idx++
		openot(x, x2)
		jump(x2 + 1)
		return -1
	}
	if idx < len(line) && strings.ContainsRune("fIivCc&|^<>", rune(line[idx])) {
		ch := line[idx]
		idx++
		if ch == '<' || ch == '>' {
			multibyte := false
			if idx < len(line) && line[idx] == ch {
				idx++
				multibyte = true
			}
			times, idx2 := expression(line, idx)
			idx = idx2
			if times == UNKNOWN {
				times = 1
			}
			bit := UNKNOWN
			if idx < len(line) && line[idx] == ',' {
				bit, idx = expression(line, idx+1)
			}
			shift_rotate(x, x2, times, bit, multibyte, byte(ch))
			return -1
		}
		if ch == 'i' {
			idx = skipspc(line, idx)
			var m []byte
			if idx < len(line) && line[idx] == '/' {
				m, idx = get_str(line, idx+1)
			} else {
				m, idx = get_hexs(line, idx)
			}
			if xf2 {
				if len(m) > 0 {
					total := int(x2 - x + 1)
					rep := total / len(m)
					rem := total % len(m)
					data := bytes.Repeat(m, rep)
					data = append(data, m[:rem]...)
					ovwmem(x, data)
					stdmm(fmt.Sprintf("%d bytes filled.", len(data)))
					jump(x + int64(len(data)))
				} else {
					stderr("Invalid syntax.")
				}
				return -1
			}
			length := int64(1)
			if idx < len(line) && line[idx] == '*' {
				idx++
				length, idx = expression(line, idx)
			}
			data := bytes.Repeat(m, int(length))
			ovwmem(x, data)
			stdmm(fmt.Sprintf("%d bytes overwritten.", len(data)))
			jump(x + int64(len(data)))
			return -1
		}
		if ch == 'I' {
			idx = skipspc(line, idx)
			var m []byte
			if idx < len(line) && line[idx] == '/' {
				m, idx = get_str(line, idx+1)
			} else {
				m, idx = get_hexs(line, idx)
			}
			if idx < len(line) && line[idx] == '*' {
				idx++
				_, idx = expression(line, idx)
			}
			if xf2 {
				stderr("Invalid syntax.")
				return -1
			}
			data := bytes.Repeat(m, 1)
			insmem(x, data)
			stdmm(fmt.Sprintf("%d bytes inserted.", len(data)))
			jump(x + int64(len(data)))
			return -1
		}
		x3, idx2 := expression(line, idx)
		idx = idx2
		if x3 == UNKNOWN {
			stderr("Invalid parameter.")
			return -1
		}
		switch ch {
		case 'c':
			yankmem(x, x2)
			cpymem(x, x2, x3)
			stdmm(fmt.Sprintf("%d bytes copied.", x2-x+1))
			jump(x3 + (x2 - x + 1))
			return -1
		case 'C':
			mm := redmem(x, x2)
			yankmem(x, x2)
			insmem(x3, mm)
			stdmm(fmt.Sprintf("%d bytes inserted.", x2-x+1))
			jump(x3 + int64(len(mm)))
			return -1
		case 'v':
			xp := movmem(x, x2, x3)
			jump(xp)
			return -1
		case '&':
			opeand(x, x2, int(x3))
			jump(x2 + 1)
			return -1
		case '|':
			opeor(x, x2, int(x3))
			jump(x2 + 1)
			return -1
		case '^':
			opexor(x, x2, int(x3))
			jump(x2 + 1)
			return -1
		}
	}
	stderr("Unrecognized command.")
	return -1
}

func commandline(line string) int {
	defer func() {
		if r := recover(); r != nil {
			stderr("Memory overflow.")
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
	a := readmem(addr)
	esclocate(0, 23)
	esccolor(6, 0)
	s := "."
	if a < 0x20 {
		s = fmt.Sprintf("^%c", rune(a+int('@')))
	} else if a >= 0x7e {
		s = "."
	} else {
		s = fmt.Sprintf("'%c'", rune(a))
	}
	if addr < int64(len(mem)) {
		fmt.Printf("%012X : 0x%02X 0b%08b 0o%03o %d %s      ", addr, a, a, a, a, s)
	} else {
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
	stroke := false
	var ch byte
	repsw = 0
	for {
		cp = fpos()
		repaint()
		printdata()
		esclocate(curx/2*3+13+(curx&1), cury+3)
		b, err := getchByte()
		if err != nil {
			return false
		}
		ch = b
		clrmm()
		nff = true
		// arrow handling
		if ch == 0x1b {
			// read two more bytes if present
			b2, _ := getchByte()
			b3, _ := getchByte()
			if b3 == 'A' {
				ch = 'k'
			} else if b3 == 'B' {
				ch = 'j'
			} else if b3 == 'C' {
				ch = 'l'
			} else if b3 == 'D' {
				ch = 'h'
			} else if b2 == '[' && b3 == '2' {
				ch = 'i'
			}
		}

		if ch == 'n' {
			searchnext(fpos() + 1)
			continue
		} else if ch == 'N' {
			searchlast(fpos() - 1)
			continue
		} else if ch == 0x02 { // ctrl-b
			if homeaddr >= 256 {
				homeaddr -= 256
			} else {
				homeaddr = 0
			}
			continue
		} else if ch == 0x06 { // ctrl-f
			homeaddr += 256
			continue
		} else if ch == 0x15 { // ctrl-u
			if homeaddr >= 128 {
				homeaddr -= 128
			} else {
				homeaddr = 0
			}
			continue
		} else if ch == 0x04 { // ctrl-d
			homeaddr += 128
			continue
		} else if ch == '^' {
			curx = 0
			continue
		} else if ch == '$' {
			curx = 30
			continue
		} else if ch == 'j' {
			if cury < LENONSCR/16-1 {
				cury++
			} else {
				scrdown()
			}
			continue
		} else if ch == 'k' {
			if cury > 0 {
				cury--
			} else {
				scrup()
			}
			continue
		} else if ch == 'h' {
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
		} else if ch == 'l' {
			inccurx()
			continue
		} else if ch == 0x19 { // ctrl-y toggle utf8
			utf8mode = !utf8mode
			escclear()
			repaint()
			continue
		} else if ch == 0x0c { // ctrl-l
			escclear()
			if utf8mode {
				repsw = (repsw + 1) % 4
			}
			repaint()
			continue
		} else if ch == 'Z' {
			if writefile(filename) {
				return true
			}
			continue
		} else if ch == 'q' {
			if lastchange {
				stdmm("No write since last change. To overriding quit, use 'q!'.")
				continue
			}
			return false
		} else if ch == 'M' {
			disp_marks()
			continue
		} else if ch == 'm' {
			c, _ := getchByte()
			c = byte(strings.ToLower(string(c))[0])
			if 'a' <= c && c <= 'z' {
				mark[c-'a'] = fpos()
			}
			continue
		} else if ch == '/' {
			search()
			continue
		} else if ch == '\'' {
			c, _ := getchByte()
			c = byte(strings.ToLower(string(c))[0])
			if 'a' <= c && c <= 'z' {
				jump(mark[c-'a'])
			}
			continue
		} else if ch == 'p' {
			y := append([]byte{}, yank...)
			ovwmem(fpos(), y)
			jump(fpos() + int64(len(y)))
			continue
		} else if ch == 'P' {
			y := append([]byte{}, yank...)
			insmem(fpos(), y)
			jump(fpos() + int64(len(yank)))
			continue
		}

		if ch == 'i' {
			insmod = !insmod
			stroke = false
		} else if strings.ContainsRune("0123456789abcdefABCDEF", rune(ch)) {
			addr := fpos()
			cval, _ := strconv.ParseInt(string(ch), 16, 64)
			sh := 4
			if curx&1 == 1 {
				sh = 0
			}
			mask := 0x0f
			if curx&1 == 1 {
				mask = 0xf0
			}
			if insmod {
				if !stroke && addr < int64(len(mem)) {
					insmem(addr, []byte{byte(cval << int64(sh))})
				} else {
					setmem(addr, (readmem(addr) & mask) | int(cval)<<sh)
				}
				if curx&1 == 0 {
					stroke = !stroke
				} else {
					stroke = false
				}
			} else {
				setmem(addr, (readmem(addr) & mask) | int(cval)<<sh)
			}
			inccurx()
		} else if ch == 'x' {
			delmem(fpos(), fpos(), false)
		} else if ch == ':' {
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

func readfile(fn string) bool {
	data, err := ioutil.ReadFile(fn)
	if err != nil {
		newfile = true
		stdmm("<new file>")
		mem = []byte{}
		return true
	}
	newfile = false
	mem = append([]byte{}, data...)
	return true
}

func regulate_mem() {
	for i := range mem {
		mem[i] = mem[i] & 0xff
	}
}

func writefile(fn string) bool {
	regulate_mem()
	err := ioutil.WriteFile(fn, mem, 0644)
	if err != nil {
		stderr("Permission denied.")
		return false
	}
	stdmm("File written.")
	return true
}

func wrtfile(start, end int64, fn string) bool {
	regulate_mem()
	f, err := os.Create(fn)
	if err != nil {
		stderr("Permission denied.")
		return false
	}
	defer f.Close()
	for i := start; i <= end; i++ {
		if i >= 0 && i < int64(len(mem)) {
			f.Write([]byte{mem[i]})
		} else {
			f.Write([]byte{0})
		}
	}
	return true
}

func main() {
	// CLI flags similar to original
	script := flag.String("s", "", "bi script file")
	termcolor := flag.String("t", "black", "background color of terminal. default is 'black' the others are white.")
	verboseFlag := flag.Bool("v", false, "verbose when processing script")
	writeFlag := flag.Bool("w", false, "write file when exiting script")
	flag.Parse()
	if flag.NArg() < 1 {
		fmt.Println("Usage: bi [options] file")
		flag.PrintDefaults()
		os.Exit(1)
	}
	filename = flag.Arg(0)
	termcol = *termcolor
	if *script == "" {
		escclear()
	} else {
		scriptingflag = true
	}
	verbose = *verboseFlag
	wrtflg := *writeFlag
	if !readfile(filename) {
		return
	}
	if *script != "" {
		defer func() {
			if r := recover(); r != nil {
				writefile("file.save")
				stderr("Some error occured. memory saved to file.save.")
			}
		}()
		_ = scripting(*script)
		if wrtflg && lastchange {
			writefile(filename)
		}
	} else {
		defer func() {
			if r := recover(); r != nil {
				writefile("file.save")
				stderr("Some error occured. memory saved to file.save.")
			}
		}()
		_ = fedit()
	}
	esccolor(7, 0)
	escdispcursor()
	esclocate(0, 23)
}
