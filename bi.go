//go:build linux

// bi - Binary editor like vI
// Go port of bi C version 3.5.1 by Taisuke Maekawa
// Complete compatible translation from bi.c

package main
import (
    "unsafe"
	"bufio"
	"fmt"
	"math"
	"os"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"syscall"
)

// ============================================================
// Constants
// ============================================================

const (
	LENONSCR    = (256+3*16)
	BOTTOMLN    = 22
	RELEN       = 128
	MAX_UNDO    = 100
	FCMP_SPAN   = 10
	FCMP_MAXN   = 8192
)

const (
    TCGETS = 0x5401
    TCSETS = 0x5402
)

const UNKNOWN = uint64(math.MaxUint64)

// ============================================================
// Partial state (global)
// ============================================================

type PartialState struct {
	active     bool
	offset     uint64
	length     uint64
	initOffset uint64
	initLength uint64
}

var gPartial PartialState

// ============================================================
// Data structures
// ============================================================

type Match struct {
	pos uint64
	len uint64
}

type DiffOp int

const (
	DIFF_OVW        DiffOp = iota
	DIFF_OVW_REGION DiffOp = iota
	DIFF_INS        DiffOp = iota
	DIFF_DEL        DiffOp = iota
)

type DiffEntry struct {
	op         DiffOp
	pos        uint64
	origMemLen uint64
	oldByte    byte
	newByte    byte
	oldData    []byte
	newData    []byte
}

type DiffState struct {
	log              []DiffEntry
	markBefore       [26]uint64
	markAfter        [26]uint64
	modifiedBefore   bool
	lastchangeBefore bool
	cursorBefore     uint64
	cursorAfter      uint64
}

// ============================================================
// Terminal
// ============================================================

type Terminal struct {
	termcol string
	coltab  [8]int
	editor  *BiEditor
}

func newTerminal(termcol string, ed *BiEditor) Terminal {
	t := Terminal{termcol: termcol, editor: ed}
	t.coltab = [8]int{0, 1, 4, 5, 2, 6, 3, 7}
	return t
}

func (t *Terminal) isScripting() bool {
	return t.editor != nil && t.editor.scriptingflag
}

func (t *Terminal) nocursor() {
	if t.isScripting() {
		return
	}
	fmt.Print("\x1b[?25l")
	flushOut()
}

func (t *Terminal) dispcursor() {
	if t.isScripting() {
		return
	}
	fmt.Print("\x1b[?25h")
	flushOut()
}

func (t *Terminal) locate(x, y int) {
	if t.isScripting() {
		return
	}
	fmt.Printf("\x1b[%d;%dH", y+1, x+1)
	flushOut()
}

func (t *Terminal) clear() {
	if t.isScripting() {
		return
	}
	fmt.Print("\x1b[2J")
	flushOut()
	t.locate(0, 0)
}

func (t *Terminal) clrline() {
	if t.isScripting() {
		return
	}
	fmt.Print("\x1b[2K")
	flushOut()
}

func (t *Terminal) color(col1, col2 int) {
	if t.isScripting() {
		return
	}
	if t.termcol == "black" {
		fmt.Printf("\x1b[3%dm\x1b[4%dm", t.coltab[col1], t.coltab[col2])
	} else {
		fmt.Printf("\x1b[3%dm\x1b[4%dm", t.coltab[0], t.coltab[7])
	}
	flushOut()
}

func (t *Terminal) resetcolor() {
	if t.isScripting() {
		return
	}
	fmt.Print("\x1b[0m")
}

func (t *Terminal) highlightColor() {
	if t.isScripting() {
		return
	}
	fmt.Print("\x1b[1;96;44m")
	flushOut()
}

// flushOut forces stdout flush (matches fflush(stdout) in C)
func flushOut() {
	// Go's os.Stdout writes are unbuffered; this is a no-op placeholder
	// kept for structural symmetry with C source
}

// ============================================================
// Terminal raw mode helpers
// ============================================================
// FreeBSD 用の ioctl 定数 (sys/ttycom.h)
const (
	TIOCGETA = 0x402c7413
	TIOCSETA = 0x802c7414
)

// ...

// ============================================================
// Terminal raw mode helpers (FreeBSD 対応版)
// ============================================================

func tcgetattr(fd int, t *syscall.Termios) error {
	_, _, err := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), uintptr(TIOCGETA), uintptr(unsafe.Pointer(t)))
	if err != 0 {
		return err
	}
	return nil
}

func tcsetattr(fd int, t *syscall.Termios) error {
	_, _, err := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), uintptr(TIOCSETA), uintptr(unsafe.Pointer(t)))
	if err != 0 {
		return err
	}
	return nil
}

func termGetch() int {
	fd := int(os.Stdin.Fd())
	var old syscall.Termios
	if err := tcgetattr(fd, &old); err != nil {
		return -1
	}
	raw := old
	
	// FreeBSD のフラグ操作 (uint32 にキャストしてビット演算)
	raw.Iflag &^= uint32(syscall.IGNBRK | syscall.BRKINT | syscall.PARMRK |
		syscall.ISTRIP | syscall.INLCR | syscall.IGNCR | syscall.ICRNL | syscall.IXON)
	raw.Oflag &^= uint32(syscall.OPOST)
	raw.Lflag &^= uint32(syscall.ECHO | syscall.ECHONL | syscall.ICANON | syscall.ISIG | syscall.IEXTEN)
	raw.Cflag &^= uint32(syscall.CSIZE | syscall.PARENB)
	raw.Cflag |= uint32(syscall.CS8)
	
	// FreeBSD の Termios 構造体 Cc フィールドへのアクセス
	raw.Cc[syscall.VMIN] = 1
	raw.Cc[syscall.VTIME] = 0
	
	tcsetattr(fd, &raw)
	defer tcsetattr(fd, &old)

	buf := make([]byte, 1)
	n, _ := os.Stdin.Read(buf)
	if n == 0 {
		return -1
	}
	return int(buf[0])
}

// ============================================================
// History + Readline
// ============================================================

type HistoryStore struct {
	entries []string
	maxSize int
}

func newHistoryStore(maxSize int) HistoryStore {
	return HistoryStore{maxSize: maxSize}
}

func (h *HistoryStore) add(entry string) {
	if entry == "" {
		return
	}
	if len(h.entries) >= h.maxSize {
		h.entries = h.entries[1:]
	}
	h.entries = append(h.entries, entry)
}

type HistoryManager struct {
	cmdHist    HistoryStore
	searchHist HistoryStore
}

func newHistoryManager() HistoryManager {
	return HistoryManager{
		cmdHist:    newHistoryStore(1000),
		searchHist: newHistoryStore(1000),
	}
}

// readlineWithHistory reads a line with history navigation support
func readlineWithHistory(prompt string, hist *HistoryStore) string {
	fd := int(os.Stdin.Fd())
	var old syscall.Termios
	if err := tcgetattr(fd, &old); err != nil {
		// Fallback: read without raw mode
		fmt.Print(prompt)
		sc := bufio.NewScanner(os.Stdin)
		if sc.Scan() {
			return sc.Text()
		}
		return ""
	}
	raw := old
	raw.Lflag &^= syscall.ICANON | syscall.ECHO
	raw.Cc[syscall.VMIN] = 1
	raw.Cc[syscall.VTIME] = 0
	tcsetattr(fd, &raw)
	defer tcsetattr(fd, &old)

	fmt.Print(prompt)

	line := []byte{}
	curPos := 0
	histPos := len(hist.entries)
	savedLine := []byte{}
	savedPos := 0

	redraw := func() {
		fmt.Print("\r\x1b[K")
		fmt.Print(prompt)
		fmt.Print(string(line))
		if curPos < len(line) {
			fmt.Printf("\x1b[%dD", len(line)-curPos)
		}
	}

	for {
		buf := make([]byte, 4)
		n, _ := os.Stdin.Read(buf[:1])
		if n == 0 {
			break
		}
		ch := buf[0]

		switch ch {
		case 13, 10: // Enter
			fmt.Println()
			result := string(line)
			if result != "" {
				hist.add(result)
			}
			return result
		case 127, 8: // Backspace
			if curPos > 0 {
				line = append(line[:curPos-1], line[curPos:]...)
				curPos--
				redraw()
			}
		case 27: // ESC
			n2, _ := os.Stdin.Read(buf[:1])
			if n2 == 0 {
				break
			}
			if buf[0] == '[' {
				n3, _ := os.Stdin.Read(buf[:1])
				if n3 == 0 {
					break
				}
				switch buf[0] {
				case 'A': // Up arrow
					if histPos > 0 {
						if histPos == len(hist.entries) {
							savedLine = append([]byte{}, line...)
							savedPos = curPos
						}
						histPos--
						line = []byte(hist.entries[histPos])
						curPos = len(line)
						redraw()
					}
				case 'B': // Down arrow
					if histPos < len(hist.entries) {
						histPos++
						if histPos == len(hist.entries) {
							line = append([]byte{}, savedLine...)
							curPos = savedPos
						} else {
							line = []byte(hist.entries[histPos])
							curPos = len(line)
						}
						redraw()
					}
				case 'C': // Right
					if curPos < len(line) {
						curPos++
						fmt.Print("\x1b[C")
					}
				case 'D': // Left
					if curPos > 0 {
						curPos--
						fmt.Print("\x1b[D")
					}
				case '3': // DEL key (^[[3~)
					os.Stdin.Read(buf[:1])
					if curPos < len(line) {
						line = append(line[:curPos], line[curPos+1:]...)
						redraw()
					}
				}
			}
		case 1: // Ctrl+A
			curPos = 0
			redraw()
		case 5: // Ctrl+E
			curPos = len(line)
			redraw()
		case 21: // Ctrl+U
			line = []byte{}
			curPos = 0
			redraw()
		case 3: // Ctrl+C
			fmt.Println()
			return ""
		default:
			if ch >= 32 {
				line = append(line, 0)
				copy(line[curPos+1:], line[curPos:])
				line[curPos] = ch
				curPos++
				redraw()
			}
		}
	}
	return string(line)
}

func historyGetln(hist *HistoryManager, prompt, mode string) string {
	if mode == "search" {
		return readlineWithHistory(prompt, &hist.searchHist)
	}
	return readlineWithHistory(prompt, &hist.cmdHist)
}

// ============================================================
// MemoryBuffer
// ============================================================

type MemoryBuffer struct {
	mem          []byte
	yank         []byte
	mark         [26]uint64
	modified     bool
	lastchange   bool
	trackingDiff bool
	currentDiff  []DiffEntry
}

func newMemoryBuffer() MemoryBuffer {
	m := MemoryBuffer{}
	for i := range m.mark {
		m.mark[i] = UNKNOWN
	}
	return m
}

func (m *MemoryBuffer) read(addr uint64) byte {
	if addr >= uint64(len(m.mem)) {
		return 0
	}
	return m.mem[addr]
}

func (m *MemoryBuffer) set(addr uint64, data byte) {
	origLen := uint64(len(m.mem))
	for addr >= uint64(len(m.mem)) {
		m.mem = append(m.mem, 0)
	}
	if m.trackingDiff {
		e := DiffEntry{
			op:         DIFF_OVW,
			pos:        addr,
			origMemLen: origLen,
			oldByte:    m.mem[addr],
			newByte:    data & 0xFF,
		}
		m.currentDiff = append(m.currentDiff, e)
	}
	m.mem[addr] = data & 0xFF
	m.modified = true
	m.lastchange = true
}

func (m *MemoryBuffer) insert(start uint64, data []byte) {
	if len(data) == 0 {
		return
	}
	if m.trackingDiff {
		e := DiffEntry{
			op:         DIFF_INS,
			pos:        start,
			origMemLen: uint64(len(m.mem)),
			newData:    append([]byte{}, data...),
		}
		m.currentDiff = append(m.currentDiff, e)
	}
	pos := int(start)
	if pos > len(m.mem) {
		pos = len(m.mem)
	}
	m.mem = append(m.mem, make([]byte, len(data))...)
	copy(m.mem[pos+len(data):], m.mem[pos:])
	copy(m.mem[pos:], data)
	m.modified = true
	m.lastchange = true
}

func (m *MemoryBuffer) deleteRange(start, end uint64, doYank bool) bool {
	if len(m.mem) == 0 || start >= uint64(len(m.mem)) ||
		end >= uint64(len(m.mem)) || start > end {
		return false
	}
	if m.trackingDiff {
		e := DiffEntry{
			op:         DIFF_DEL,
			pos:        start,
			origMemLen: uint64(len(m.mem)),
			oldData:    append([]byte{}, m.mem[start:end+1]...),
		}
		m.currentDiff = append(m.currentDiff, e)
	}
	if doYank {
		m.yank = append([]byte{}, m.mem[start:end+1]...)
	}
	m.mem = append(m.mem[:start], m.mem[end+1:]...)
	m.lastchange = true
	m.modified = true
	return true
}

func (m *MemoryBuffer) yank_(start, end uint64) uint64 {
	if len(m.mem) == 0 || start >= uint64(len(m.mem)) {
		return 0
	}
	if end >= uint64(len(m.mem)) {
		end = uint64(len(m.mem)) - 1
	}
	m.yank = append([]byte{}, m.mem[start:end+1]...)
	return uint64(len(m.yank))
}

func (m *MemoryBuffer) overwrite(start uint64, data []byte) {
	if len(data) == 0 {
		return
	}
	if m.trackingDiff {
		e := DiffEntry{
			op:         DIFF_OVW_REGION,
			pos:        start,
			origMemLen: uint64(len(m.mem)),
		}
		for i := uint64(0); i < uint64(len(data)); i++ {
			var old byte
			if start+i < uint64(len(m.mem)) {
				old = m.mem[start+i]
			}
			e.oldData = append(e.oldData, old)
		}
		e.newData = append([]byte{}, data...)
		m.currentDiff = append(m.currentDiff, e)
	}
	for start+uint64(len(data)) > uint64(len(m.mem)) {
		m.mem = append(m.mem, 0)
	}
	copy(m.mem[start:], data)
	m.lastchange = true
	m.modified = true
}

// ============================================================
// Diff apply (undo / redo)
// ============================================================

func applyDiffInverse(m *MemoryBuffer, log []DiffEntry) {
	for i := len(log) - 1; i >= 0; i-- {
		e := &log[i]
		switch e.op {
		case DIFF_OVW:
			for uint64(len(m.mem)) <= e.pos {
				m.mem = append(m.mem, 0)
			}
			m.mem[e.pos] = e.oldByte
			if e.origMemLen < uint64(len(m.mem)) {
				m.mem = m.mem[:e.origMemLen]
			}
		case DIFF_OVW_REGION:
			for j := 0; j < len(e.oldData); j++ {
				pos := e.pos + uint64(j)
				if pos < uint64(len(m.mem)) {
					m.mem[pos] = e.oldData[j]
				}
			}
			if e.origMemLen < uint64(len(m.mem)) {
				m.mem = m.mem[:e.origMemLen]
			}
		case DIFF_INS:
			if len(e.newData) > 0 && e.pos < uint64(len(m.mem)) &&
				e.pos+uint64(len(e.newData))-1 < uint64(len(m.mem)) {
				m.mem = append(m.mem[:e.pos], m.mem[e.pos+uint64(len(e.newData)):]...)
			}
		case DIFF_DEL:
			pos := int(e.pos)
			if pos > len(m.mem) {
				pos = len(m.mem)
			}
			m.mem = append(m.mem, make([]byte, len(e.oldData))...)
			copy(m.mem[pos+len(e.oldData):], m.mem[pos:])
			copy(m.mem[pos:], e.oldData)
		}
	}
}

func applyDiffForward(m *MemoryBuffer, log []DiffEntry) {
	for _, e := range log {
		switch e.op {
		case DIFF_OVW:
			for uint64(len(m.mem)) <= e.pos {
				m.mem = append(m.mem, 0)
			}
			m.mem[e.pos] = e.newByte
		case DIFF_OVW_REGION:
			for uint64(len(m.mem)) < e.pos+uint64(len(e.newData)) {
				m.mem = append(m.mem, 0)
			}
			copy(m.mem[e.pos:], e.newData)
		case DIFF_INS:
			pos := int(e.pos)
			if pos > len(m.mem) {
				pos = len(m.mem)
			}
			m.mem = append(m.mem, make([]byte, len(e.newData))...)
			copy(m.mem[pos+len(e.newData):], m.mem[pos:])
			copy(m.mem[pos:], e.newData)
		case DIFF_DEL:
			if len(e.oldData) > 0 && e.pos < uint64(len(m.mem)) &&
				e.pos+uint64(len(e.oldData))-1 < uint64(len(m.mem)) {
				m.mem = append(m.mem[:e.pos], m.mem[e.pos+uint64(len(e.oldData)):]...)
			}
		}
	}
}

// ============================================================
// Display
// ============================================================

type Display struct {
	term            *Terminal
	memory          *MemoryBuffer
	homeaddr        uint64
	curx            int
	cury            int
	utf8            bool
	repsw           int
	insmod          bool
	highlightRanges []Match
}

func newDisplay(term *Terminal, mem *MemoryBuffer) Display {
	return Display{term: term, memory: mem}
}

func (d *Display) fpos() uint64 {
	return d.homeaddr + uint64(d.curx/2) + uint64(d.cury)*16
}

func (d *Display) jump(addr uint64) {
	if addr < d.homeaddr || addr >= d.homeaddr+LENONSCR {
		d.homeaddr = addr & ^uint64(0xFF)
	}
	i := addr - d.homeaddr
	d.curx = int(i&0xF) * 2
	d.cury = int(i / 16)
}

func (d *Display) isHighlighted(addr uint64) bool {
	for _, m := range d.highlightRanges {
		if addr >= m.pos && addr < m.pos+m.len {
			return true
		}
	}
	return false
}

// printchar prints one character of ASCII/UTF-8 section; returns bytes consumed
func (d *Display) printchar(a uint64) int {
	if a >= uint64(len(d.memory.mem)) {
		fmt.Print("~")
		return 1
	}
	b := d.memory.mem[a]
	if d.utf8 {
		if b < 0x80 || (b >= 0x80 && b <= 0xBF) || b >= 0xF8 {
			if b >= 0x20 && b <= 0x7E {
				fmt.Printf("%c", b)
			} else {
				fmt.Print(".")
			}
			return 1
		} else if b >= 0xC0 && b <= 0xDF {
			if a+1 < uint64(len(d.memory.mem)) {
				b2 := d.memory.mem[a+1]
				if (b2 & 0xC0) == 0x80 {
					fmt.Printf("%c%c", b, b2)
					return 2
				}
			}
			fmt.Print(".")
			return 1
		} else if b >= 0xE0 && b <= 0xEF {
			if a+2 < uint64(len(d.memory.mem)) {
				b2 := d.memory.mem[a+1]
				b3 := d.memory.mem[a+2]
				if (b2&0xC0) == 0x80 && (b3&0xC0) == 0x80 {
					fmt.Printf("%c%c%c ", b, b2, b3)
					return 3
				}
			}
			fmt.Print(".")
			return 1
		} else if b >= 0xF0 && b <= 0xF7 {
			if a+3 < uint64(len(d.memory.mem)) {
				b2 := d.memory.mem[a+1]
				b3 := d.memory.mem[a+2]
				b4 := d.memory.mem[a+3]
				if (b2&0xC0) == 0x80 && (b3&0xC0) == 0x80 && (b4&0xC0) == 0x80 {
					fmt.Printf("%c%c%c%c  ", b, b2, b3, b4)
					return 4
				}
			}
			fmt.Print(".")
			return 1
		}
	}
	if b >= 0x20 && b <= 0x7E {
		fmt.Printf("%c", b)
	} else {
		fmt.Print(".")
	}
	return 1
}

func (d *Display) repaint(filename string) {
	d.term.locate(0, 0)
	d.term.color(6, 0)
	utf8str := "off"
	if d.utf8 {
		utf8str = "on "
	}
	insstr := "overwrite"
	if d.insmod {
		insstr = "insert   "
	}
	fmt.Printf("bi Go version 3.5.1 by Taisuke Maekawa           utf8mode:%s     %s   ",
		utf8str, insstr)

	d.term.color(5, 0)
	fn := filename
	if len(fn) > 35 {
		fn = fn[:35]
	}
	modstr := "not "
	if d.memory.modified {
		modstr = ""
	}
	fmt.Printf("\nfile:[%-35s] length:%d bytes [%smodified]    ",
		fn, len(d.memory.mem), modstr)

	d.term.nocursor()
	d.term.locate(0, 2)
	d.term.color(4, 0)
	fmt.Print("OFFSET       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F 0123456789ABCDEF ")

	d.term.color(7, 0)
	for y := 0; y < LENONSCR/16; y++ {
		d.term.color(5, 0)
		d.term.locate(0, 3+y)
		fmt.Printf("%012X ", d.homeaddr+uint64(y)*16+gPartial.offset)

		// Hex part
		for i := 0; i < 16; i++ {
			a := uint64(y*16+i) + d.homeaddr
			inHL := len(d.highlightRanges) > 0 && d.isHighlighted(a)
			if inHL {
				d.term.highlightColor()
				if a >= uint64(len(d.memory.mem)) {
					fmt.Print("~~")
				} else {
					fmt.Printf("%02X", d.memory.mem[a])
				}
				d.term.resetcolor()
				d.term.color(7, 0)
				fmt.Print(" ")
			} else {
				d.term.color(7, 0)
				if a >= uint64(len(d.memory.mem)) {
					fmt.Print("~~ ")
				} else {
					fmt.Printf("%02X ", d.memory.mem[a])
				}
			}
		}

		// ASCII/UTF-8 part
		d.term.color(6, 0)
		if d.utf8 {
			col := 0
			i := 0
			for i < 16 && col < 16 {
				a := uint64(y*16+i) + d.homeaddr
				inHL := len(d.highlightRanges) > 0 && d.isHighlighted(a)
				if inHL {
					d.term.highlightColor()
				}
				l := d.printchar(a)
				if inHL {
					d.term.resetcolor()
					d.term.color(6, 0)
				}
				i += l
				col++
			}
			for col < 16 {
				fmt.Print(" ")
				col++
			}
		} else {
			for i := 0; i < 16; i++ {
				a := uint64(y*16+i) + d.homeaddr
				inHL := len(d.highlightRanges) > 0 && d.isHighlighted(a)
				if inHL {
					d.term.highlightColor()
				}
				d.printchar(a)
				if inHL {
					d.term.resetcolor()
					d.term.color(6, 0)
				}
			}
		}
		fmt.Print(" ")
	}
}

func (d *Display) printdata() {
	addr := d.fpos()
	fileAddr := addr + gPartial.offset
	a := d.memory.read(addr)

	d.term.locate(0, 23)
	d.term.color(6, 0)
	fmt.Printf("%80s", "")
	d.term.locate(0, 23)
	s := "."
	if a < 0x20 {
		s = fmt.Sprintf("^%c", a+'@')
	} else if a >= 0x7E {
		s = "."
	} else {
		s = fmt.Sprintf("'%c'", a)
	}
	if addr < uint64(len(d.memory.mem)) {
		fmt.Printf("%012X : 0x%02X 0b", fileAddr, a)
		for i := 7; i >= 0; i-- {
			fmt.Printf("%d", (a>>uint(i))&1)
		}
		fmt.Printf(" 0o%03o %d %s      ", a, a, s)
	} else {
		fmt.Printf("%012X : ~~                                                   ", fileAddr)
	}

	d.term.locate(0, 23)
	if gPartial.active {
		d.term.color(6, 0)
		fmt.Printf(" PARTIAL  file_offset:0x%012X  length:0x%X(%d) bytes   ",
			gPartial.offset, gPartial.length, gPartial.length)
	}
	flushOut()
}

func (d *Display) clrmm() {
	d.term.locate(0, BOTTOMLN)
	d.term.color(6, 0)
	d.term.clrline()
}

func (d *Display) stdmm(msg string, scripting, verbose bool) {
	if scripting {
		if verbose {
			fmt.Println(msg)
		}
	} else {
		d.clrmm()
		d.term.color(4, 0)
		d.term.locate(0, BOTTOMLN)
		fmt.Printf(" %s", msg)
		flushOut()
	}
}

func (d *Display) stdmmWait(msg string, scripting, verbose bool) {
	if scripting && !verbose {
		return
	}
	if scripting && verbose {
		fmt.Println(msg)
	} else {
		d.clrmm()
		d.term.color(4, 0)
		d.term.locate(0, BOTTOMLN)
		fmt.Printf(" %s", msg)
		flushOut()
	}
}

func (d *Display) stderr_(msg string, scripting, verbose bool) {
	if scripting {
		fmt.Fprintln(os.Stderr, msg)
	} else {
		d.clrmm()
		d.term.color(3, 0)
		d.term.locate(0, BOTTOMLN)
		fmt.Printf(" %s", msg)
		flushOut()
	}
}

// ============================================================
// Parser
// ============================================================

type Parser struct {
	memory  *MemoryBuffer
	display *Display
}

func newParser(mem *MemoryBuffer, disp *Display) Parser {
	return Parser{memory: mem, display: disp}
}

func parserSkipSpc(s string, idx int) int {
	for idx < len(s) && s[idx] == ' ' {
		idx++
	}
	return idx
}

func isHexDigit(c byte) bool {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}

func hexVal(c byte) uint64 {
	if c >= '0' && c <= '9' {
		return uint64(c - '0')
	}
	if c >= 'a' && c <= 'f' {
		return uint64(c-'a') + 10
	}
	return uint64(c-'A') + 10
}

func (p *Parser) getValue(s string, idx *int) uint64 {
	if *idx >= len(s) {
		return UNKNOWN
	}
	*idx = parserSkipSpc(s, *idx)
	if *idx >= len(s) {
		return UNKNOWN
	}
	ch := s[*idx]
	var v uint64

	switch {
	case ch == '$':
		(*idx)++
		if len(p.memory.mem) > 0 {
			v = uint64(len(p.memory.mem)) - 1
		}
	case ch == '{':
		(*idx)++
		end := strings.Index(s[*idx:], "}")
		if end < 0 {
			return UNKNOWN
		}
		expr := s[*idx : *idx+end]
		*idx += end + 1
		if expr == "" {
			return UNKNOWN
		}
		// Evaluate via python3
		pyCode := fmt.Sprintf("import sys\nmem=[]\ncp=%d\nprint(int(%s))\n",
			p.display.fpos(), expr)
		tmpf, err := os.CreateTemp("", "bi_eval_*.py")
		if err != nil {
			return UNKNOWN
		}
		tmpf.WriteString(pyCode)
		tmpf.Close()
		out, err2 := exec.Command("python3", tmpf.Name()).Output()
		os.Remove(tmpf.Name())
		if err2 != nil {
			return UNKNOWN
		}
		n, err3 := strconv.ParseUint(strings.TrimSpace(string(out)), 10, 64)
		if err3 != nil {
			return UNKNOWN
		}
		v = n
	case ch == '.':
		(*idx)++
		v = p.display.fpos()
	case ch == '\'' && *idx+1 < len(s) && s[*idx+1] >= 'a' && s[*idx+1] <= 'z':
		(*idx)++
		mk := p.memory.mark[s[*idx]-'a']
		if mk == UNKNOWN {
			(*idx)--
			return UNKNOWN
		}
		v = mk
		(*idx)++
	case ch == '\'' && *idx+1 < len(s):
		return UNKNOWN
	case isHexDigit(ch):
		for *idx < len(s) && isHexDigit(s[*idx]) {
			v = 16*v + hexVal(s[*idx])
			(*idx)++
		}
	case ch == '%':
		(*idx)++
		if *idx >= len(s) || s[*idx] < '0' || s[*idx] > '9' {
			return UNKNOWN
		}
		for *idx < len(s) && s[*idx] >= '0' && s[*idx] <= '9' {
			v = 10*v + uint64(s[*idx]-'0')
			(*idx)++
		}
	default:
		return UNKNOWN
	}

	if int64(v) < 0 {
		v = 0
	}
	return v
}

func (p *Parser) expression(s string, idx *int) uint64 {
	x := p.getValue(s, idx)
	if x == UNKNOWN {
		return UNKNOWN
	}
	*idx = parserSkipSpc(s, *idx)
	if *idx < len(s) && s[*idx] == '+' {
		*idx = parserSkipSpc(s, *idx+1)
		y := p.getValue(s, idx)
		if y == UNKNOWN {
			return UNKNOWN
		}
		x += y
	} else if *idx < len(s) && s[*idx] == '-' {
		*idx = parserSkipSpc(s, *idx+1)
		y := p.getValue(s, idx)
		if y == UNKNOWN {
			return UNKNOWN
		}
		if x < y {
			x = 0
		} else {
			x -= y
		}
	}
	return x
}

func parserGetRestr(s string, idx int, result *string) int {
	var b strings.Builder
	for idx < len(s) {
		if s[idx] == '/' {
			break
		}
		if s[idx] == '\\' && idx+1 < len(s) && s[idx+1] == '\\' {
			b.WriteByte('\\')
			b.WriteByte('\\')
			idx += 2
		} else if s[idx] == '\\' && idx+1 < len(s) && s[idx+1] == '/' {
			b.WriteByte('/')
			idx += 2
		} else if s[idx] == '\\' && idx+1 >= len(s) {
			idx++
			break
		} else {
			b.WriteByte(s[idx])
			idx++
		}
	}
	*result = b.String()
	return idx
}

func (p *Parser) getHexs(s string, idx int, result *[]byte) int {
	*result = nil
	idx = parserSkipSpc(s, idx)
	if idx+1 < len(s) && s[idx] == '/' && s[idx+1] == '/' {
		idx += 2
	}
	idx = parserSkipSpc(s, idx)
	startIdx := idx
	for idx < len(s) {
		v := p.expression(s, &idx)
		if v == UNKNOWN {
			if idx == startIdx {
				break
			}
			*result = nil
			break
		}
		*result = append(*result, byte(v&0xFF))
		startIdx = idx
	}
	return idx
}

func parserComment(s string) string {
	var b strings.Builder
	idx := 0
	for idx < len(s) {
		if s[idx] == '#' {
			break
		}
		if s[idx] == '\\' && idx+1 < len(s) && s[idx+1] == '#' {
			b.WriteByte('#')
			idx += 2
		} else if s[idx] == '\\' && idx+1 < len(s) && s[idx+1] == 'n' {
			b.WriteByte('\n')
			idx += 2
		} else {
			b.WriteByte(s[idx])
			idx++
		}
	}
	return b.String()
}

// ============================================================
// SearchEngine
// ============================================================

type SearchEngine struct {
	memory  *MemoryBuffer
	display *Display
	editor  *BiEditor
	smem    []byte
	isRegex bool
	remem   string
	span    uint64
	nff     bool
}

func newSearchEngine(mem *MemoryBuffer, disp *Display, ed *BiEditor) SearchEngine {
	return SearchEngine{
		memory:  mem,
		display: disp,
		editor:  ed,
		nff:     true,
	}
}

func (se *SearchEngine) hit(addr uint64) int {
	for i := 0; i < len(se.smem); i++ {
		pos := addr + uint64(i)
		if pos < uint64(len(se.memory.mem)) && se.memory.mem[pos] == se.smem[i] {
			continue
		}
		return 0
	}
	return 1
}

func (se *SearchEngine) hitRe(addr uint64) int {
	if se.remem == "" {
		return -1
	}
	if addr >= uint64(len(se.memory.mem)) {
		return -1
	}
	end := addr + RELEN
	if end > uint64(len(se.memory.mem)) {
		end = uint64(len(se.memory.mem))
	}
	if end == addr {
		return -1
	}
	// Replace null bytes with space for regex matching
	sub := make([]byte, end-addr)
	for i := range sub {
		b := se.memory.mem[addr+uint64(i)]
		if b == 0 {
			sub[i] = ' '
		} else {
			sub[i] = b
		}
	}
	re, err := regexp.Compile(se.remem)
	if err != nil {
		return -1
	}
	loc := re.FindIndex(sub)
	if loc == nil || loc[0] != 0 {
		return 0
	}
	se.span = uint64(loc[1] - loc[0])
	return 1
}

func (se *SearchEngine) searchNext(fp, memLen uint64) uint64 {
	if memLen == 0 {
		se.display.clrmm()
		return UNKNOWN
	}
	if !se.isRegex && len(se.smem) == 0 {
		return UNKNOWN
	}
	se.display.stdmmWait("Wait.", se.editor.scriptingflag, se.editor.verbose)
	curpos := fp
	start := fp
	wrapped := false
	for {
		var f int
		if se.isRegex {
			f = se.hitRe(curpos)
		} else {
			f = se.hit(curpos)
		}
		if f == 1 {
			if !wrapped {
				se.display.clrmm()
			}
			return curpos
		} else if f < 0 {
			if !wrapped {
				se.display.clrmm()
			}
			return UNKNOWN
		}
		curpos++
		if curpos >= memLen {
			if se.nff {
				if !wrapped {
					se.display.stdmmWait("Search reached BOTTOM, wrap around to TOP",
						se.editor.scriptingflag, se.editor.verbose)
					wrapped = true
				}
				curpos = 0
			} else {
				se.display.clrmm()
				return UNKNOWN
			}
		}
		if curpos == start {
			return UNKNOWN
		}
	}
}

func (se *SearchEngine) searchLast(fp, memLen uint64) uint64 {
	if memLen == 0 {
		return UNKNOWN
	}
	if !se.isRegex && len(se.smem) == 0 {
		return UNKNOWN
	}
	wrapped := false
	if fp >= memLen {
		fp = memLen - 1
		se.display.stdmmWait("Search reached TOP, wrap around to BOTTOM",
			se.editor.scriptingflag, se.editor.verbose)
		wrapped = true
	}
	curpos := fp
	start := fp
	if !wrapped {
		se.display.stdmmWait("Wait.", se.editor.scriptingflag, se.editor.verbose)
	}
	for {
		var f int
		if se.isRegex {
			f = se.hitRe(curpos)
		} else {
			f = se.hit(curpos)
		}
		if f == 1 {
			if !wrapped {
				se.display.clrmm()
			}
			return curpos
		} else if f < 0 {
			if !wrapped {
				se.display.clrmm()
			}
			return UNKNOWN
		}
		if curpos == 0 {
			if memLen > 0 {
				curpos = memLen - 1
			}
		} else {
			curpos--
		}
		if curpos == start {
			return UNKNOWN
		}
	}
}

func (se *SearchEngine) searchAll(memLen uint64, matches *[]Match) {
	*matches = nil
	if !se.isRegex && len(se.smem) == 0 {
		return
	}
	se.display.stdmmWait("Wait.", se.editor.scriptingflag, se.editor.verbose)
	curpos := uint64(0)
	maxResults := uint64(10000)
	for curpos < memLen && uint64(len(*matches)) < maxResults {
		var f int
		if se.isRegex {
			f = se.hitRe(curpos)
		} else {
			f = se.hit(curpos)
		}
		if f == 1 {
			mlen := se.span
			if !se.isRegex {
				mlen = uint64(len(se.smem))
			}
			*matches = append(*matches, Match{pos: curpos, len: mlen})
			if mlen > 0 {
				curpos += mlen
			} else {
				curpos++
			}
		} else if f < 0 {
			break
		} else {
			curpos++
		}
	}
	se.display.clrmm()
}

func (se *SearchEngine) searchNextNoLoop(fp uint64) uint64 {
	if !se.isRegex && len(se.smem) == 0 {
		return UNKNOWN
	}
	curpos := fp
	for curpos < uint64(len(se.memory.mem)) {
		var f int
		if se.isRegex {
			f = se.hitRe(curpos)
		} else {
			f = se.hit(curpos)
		}
		if f == 1 {
			return curpos
		} else if f < 0 {
			return UNKNOWN
		}
		curpos++
	}
	return UNKNOWN
}

// ============================================================
// FileManager
// ============================================================

type FileManager struct {
	memory   *MemoryBuffer
	filename string
	newfile  bool
}

func newFileManager(mem *MemoryBuffer) FileManager {
	return FileManager{memory: mem}
}

func (fm *FileManager) readFile(filename, msg *string) bool {
	f, err := os.Open(*filename)
	if err != nil {
		fm.newfile = true
		fm.memory.mem = nil
		*msg = "<new file>"
		return true
	}
	defer f.Close()
	fm.newfile = false
	data, err2 := os.ReadFile(*filename)
	if err2 != nil {
		*msg = "File read error."
		return false
	}
	fm.memory.mem = append([]byte{}, data...)
	*msg = ""
	return true
}

func (fm *FileManager) writeFile(filename string, msg *string) bool {
	err := os.WriteFile(filename, fm.memory.mem, 0644)
	if err != nil {
		*msg = "Permission denied."
		return false
	}
	*msg = "File written."
	return true
}

func (fm *FileManager) readFilePartial(filename string, offset, maxLen uint64, msg *string) bool {
	f, err := os.Open(filename)
	if err != nil {
		fm.newfile = true
		fm.memory.mem = nil
		gPartial.active = true
		gPartial.offset = offset
		gPartial.length = 0
		*msg = "<new file>"
		return true
	}
	defer f.Close()

	info, err2 := f.Stat()
	if err2 != nil || uint64(info.Size()) <= offset {
		*msg = fmt.Sprintf("Offset 0x%X exceeds file size.", offset)
		return false
	}

	available := uint64(info.Size()) - offset
	readLen := available
	if maxLen != 0 && maxLen < available {
		readLen = maxLen
	}

	f.Seek(int64(offset), 0)
	buf := make([]byte, readLen)
	n, _ := f.Read(buf)
	buf = buf[:n]

	fm.memory.mem = append([]byte{}, buf...)
	gPartial.active = true
	gPartial.offset = offset
	gPartial.length = uint64(n)
	fm.newfile = false
	fm.memory.modified = false
	fm.memory.lastchange = false
	*msg = fmt.Sprintf("Partial load: offset=0x%X, %d bytes read.", offset, n)
	return true
}

func (fm *FileManager) writeFilePartial(filename string, msg *string) bool {
	if !gPartial.active {
		return fm.writeFile(filename, msg)
	}
	f, err := os.OpenFile(filename, os.O_RDWR, 0644)
	if err != nil {
		// New file
		f2, err2 := os.Create(filename)
		if err2 != nil {
			*msg = "Permission denied."
			return false
		}
		// Zero-fill up to offset
		if gPartial.offset > 0 {
			zeros := make([]byte, gPartial.offset)
			f2.Write(zeros)
		}
		f2.Write(fm.memory.mem)
		f2.Close()
		*msg = fmt.Sprintf("Partial write: offset=0x%X, %d bytes written (new file).",
			gPartial.offset, len(fm.memory.mem))
		return true
	}
	defer f.Close()
	f.Seek(int64(gPartial.offset), 0)
	n, _ := f.Write(fm.memory.mem)
	*msg = fmt.Sprintf("Partial write: offset=0x%X, %d bytes written.", gPartial.offset, n)
	return true
}

// ============================================================
// BiEditor
// ============================================================

type BiEditor struct {
	scriptingflag bool
	verbose       bool
	term          Terminal
	memory        MemoryBuffer
	display       Display
	parser        Parser
	history       HistoryManager
	search        SearchEngine
	filemgr       FileManager
	undoStack     []DiffState
	redoStack     []DiffState
	diffActive    bool
	diffModSnap   bool
	diffLcSnap    bool
	diffMarkSnap  [26]uint64
	diffCurSnap   uint64
	cp            uint64
}

func newEditor(termcol string) *BiEditor {
	ed := &BiEditor{}
	ed.term = newTerminal(termcol, ed)
	ed.memory = newMemoryBuffer()
	ed.display = newDisplay(&ed.term, &ed.memory)
	ed.parser = newParser(&ed.memory, &ed.display)
	ed.history = newHistoryManager()
	ed.search = newSearchEngine(&ed.memory, &ed.display, ed)
	ed.filemgr = newFileManager(&ed.memory)
	return ed
}

func (ed *BiEditor) saveUndoState() {
	if ed.scriptingflag {
		return
	}
	if ed.diffActive {
		ed.commitUndo()
	}
	copy(ed.diffMarkSnap[:], ed.memory.mark[:])
	ed.diffModSnap = ed.memory.modified
	ed.diffLcSnap = ed.memory.lastchange
	ed.diffCurSnap = ed.display.fpos()
	ed.diffActive = true
	ed.memory.trackingDiff = true
	ed.memory.currentDiff = nil
}

func (ed *BiEditor) commitUndo() {
	if !ed.diffActive {
		return
	}
	log := ed.memory.currentDiff
	ed.memory.currentDiff = nil
	ed.memory.trackingDiff = false
	ed.diffActive = false

	if len(log) == 0 {
		return
	}
	state := DiffState{
		log:              log,
		modifiedBefore:   ed.diffModSnap,
		lastchangeBefore: ed.diffLcSnap,
		cursorBefore:     ed.diffCurSnap,
		cursorAfter:      ed.display.fpos(),
	}
	copy(state.markBefore[:], ed.diffMarkSnap[:])
	copy(state.markAfter[:], ed.memory.mark[:])
	ed.undoStack = append(ed.undoStack, state)
	if len(ed.undoStack) > MAX_UNDO {
		ed.undoStack = ed.undoStack[1:]
	}
	ed.redoStack = nil
}

func (ed *BiEditor) decUndo() bool {
	if !ed.diffActive {
		return false
	}
	ed.memory.currentDiff = nil
	ed.memory.trackingDiff = false
	ed.diffActive = false
	return true
}

func (ed *BiEditor) undo() bool {
	if len(ed.undoStack) == 0 {
		ed.display.stdmm("No more undo.", ed.scriptingflag, ed.verbose)
		return false
	}
	state := ed.undoStack[len(ed.undoStack)-1]
	ed.undoStack = ed.undoStack[:len(ed.undoStack)-1]
	state.cursorAfter = ed.display.fpos()
	ed.redoStack = append(ed.redoStack, state)

	applyDiffInverse(&ed.memory, ed.redoStack[len(ed.redoStack)-1].log)
	copy(ed.memory.mark[:], state.markBefore[:])
	ed.memory.modified = state.modifiedBefore
	ed.memory.lastchange = state.lastchangeBefore

	target := state.cursorBefore
	memLen := uint64(len(ed.memory.mem))
	if memLen == 0 {
		target = 0
	} else if target >= memLen {
		target = memLen - 1
	}
	ed.display.jump(target)
	ed.display.stdmm(fmt.Sprintf("Undo. (%d more)", len(ed.undoStack)),
		ed.scriptingflag, ed.verbose)
	return true
}

func (ed *BiEditor) redo() bool {
	if len(ed.redoStack) == 0 {
		ed.display.stdmm("No more redo.", ed.scriptingflag, ed.verbose)
		return false
	}
	state := ed.redoStack[len(ed.redoStack)-1]
	ed.redoStack = ed.redoStack[:len(ed.redoStack)-1]
	state.cursorBefore = ed.display.fpos()
	ed.undoStack = append(ed.undoStack, state)

	applyDiffForward(&ed.memory, ed.undoStack[len(ed.undoStack)-1].log)
	copy(ed.memory.mark[:], state.markAfter[:])
	ed.memory.modified = true
	ed.memory.lastchange = true

	target := state.cursorAfter
	memLen := uint64(len(ed.memory.mem))
	if memLen == 0 {
		target = 0
	} else if target >= memLen {
		target = memLen - 1
	}
	ed.display.jump(target)
	ed.display.stdmm(fmt.Sprintf("Redo. (%d more)", len(ed.redoStack)),
		ed.scriptingflag, ed.verbose)
	return true
}

// ============================================================
// Interactive editor (fedit)
// ============================================================

func (ed *BiEditor) fedit() {
	stroke := false
	ed.display.repsw = 0

	for {
		ed.cp = ed.display.fpos()
		ed.display.repaint(ed.filemgr.filename)
		ed.display.printdata()

		// Cursor position
		cx := ed.display.curx/2*3 + 13 + (ed.display.curx & 1)
		cy := ed.display.cury + 3
		ed.term.locate(cx, cy)
		ed.term.dispcursor()
		flushOut()

		ch := termGetch()
		ed.display.clrmm()
		ed.search.nff = true

		// ESC sequence
		if ch == 27 {
			c2 := termGetch()
			if c2 == '[' {
				c3 := termGetch()
				switch c3 {
				case 'A':
					ch = 'k'
				case 'B':
					ch = 'j'
				case 'C':
					ch = 'l'
				case 'D':
					ch = 'h'
				case '2':
					ch = 'i'
				default:
					continue
				}
			} else {
				// ESC alone: clear highlight
				ed.display.highlightRanges = nil
				continue
			}
		}

		// Search n / N
		if ch == 'n' {
			pos := ed.search.searchNext(ed.display.fpos()+1, uint64(len(ed.memory.mem)))
			if pos != UNKNOWN {
				if len(ed.display.highlightRanges) == 0 {
					ed.search.searchAll(uint64(len(ed.memory.mem)), &ed.display.highlightRanges)
				}
				ed.display.jump(pos)
			} else {
				ed.display.stdmm("Not found.", ed.scriptingflag, ed.verbose)
			}
			continue
		} else if ch == 'N' {
			var fprev uint64
			if ed.display.fpos() == 0 {
				fprev = UNKNOWN
			} else {
				fprev = ed.display.fpos() - 1
			}
			pos := ed.search.searchLast(fprev, uint64(len(ed.memory.mem)))
			if pos != UNKNOWN {
				if len(ed.display.highlightRanges) == 0 {
					ed.search.searchAll(uint64(len(ed.memory.mem)), &ed.display.highlightRanges)
				}
				ed.display.jump(pos)
			} else {
				ed.display.stdmm("Not found.", ed.scriptingflag, ed.verbose)
			}
			continue
		}

		// Undo/Redo
		if ch == 'u' {
			ed.undo()
			continue
		} else if ch == 18 || ch == 'U' { // Ctrl+R or U
			ed.redo()
			continue
		}

		// Scroll
		if ch == 2 { // Ctrl+B
			if ed.display.homeaddr >= 256 {
				ed.display.homeaddr -= 256
			} else {
				ed.display.homeaddr = 0
			}
			continue
		} else if ch == 12 { // Ctrl+L
			ed.term.clear()
			continue
		} else if ch == 6 { // Ctrl+F
			ed.display.homeaddr += 256
			continue
		} else if ch == 21 { // Ctrl+U
			if ed.display.homeaddr >= 128 {
				ed.display.homeaddr -= 128
			} else {
				ed.display.homeaddr = 0
			}
			continue
		} else if ch == 4 { // Ctrl+D
			ed.display.homeaddr += 128
			continue
		}

		// Cursor movement
		if ch == '^' {
			ed.display.curx = 0
			continue
		} else if ch == '$' {
			ed.display.curx = 30
			continue
		} else if ch == 'j' {
			if ed.display.cury < LENONSCR/16-1 {
				ed.display.cury++
			} else {
				ed.display.homeaddr += 16
			}
			continue
		} else if ch == 'k' {
			if ed.display.cury > 0 {
				ed.display.cury--
			} else if ed.display.homeaddr >= 16 {
				ed.display.homeaddr -= 16
			}
			continue
		} else if ch == 'h' {
			if ed.display.curx > 0 {
				ed.display.curx--
			} else if ed.display.fpos() != 0 {
				ed.display.curx = 31
				if ed.display.cury > 0 {
					ed.display.cury--
				} else if ed.display.homeaddr >= 16 {
					ed.display.homeaddr -= 16
				}
			}
			continue
		} else if ch == 'l' {
			if ed.display.curx < 31 {
				ed.display.curx++
			} else {
				ed.display.curx = 0
				if ed.display.cury < LENONSCR/16-1 {
					ed.display.cury++
				} else {
					ed.display.homeaddr += 16
				}
			}
			continue
		}

		// Search / command
		if ch == '/' {
			ed.term.locate(0, BOTTOMLN)
			ed.term.color(7, 0)
			input := historyGetln(&ed.history, "/", "search")
			if input != "" {
				line := "/" + input
				ed.handleSearchInput(line)
			}
			continue
		}

		// UTF-8 toggle
		if ch == 25 { // Ctrl+Y
			ed.display.utf8 = !ed.display.utf8
			if ed.display.repsw == 0 {
				ed.display.repsw = 1
			} else {
				ed.display.repsw = 0
			}
			ed.term.clear()
			continue
		}

		// Save + quit (Z)
		if ch == 'Z' {
			var msg string
			var success bool
			if gPartial.active {
				success = ed.filemgr.writeFilePartial(ed.filemgr.filename, &msg)
			} else {
				success = ed.filemgr.writeFile(ed.filemgr.filename, &msg)
			}
			ed.memory.lastchange = false
			if !success {
				ed.display.stderr_(msg, ed.scriptingflag, ed.verbose)
			}
			return
		}

		// Quit (q)
		if ch == 'q' {
			if ed.memory.lastchange {
				ed.display.stdmm("No write since last change. To overriding quit, use 'q!'.",
					ed.scriptingflag, ed.verbose)
			} else {
				return
			}
			continue
		}

		// Mark display (M)
		if ch == 'M' {
			ed.term.locate(0, BOTTOMLN)
			ed.term.color(7, 0)
			for i := 0; i < 26; i++ {
				mk := ed.memory.mark[i]
				c := byte('a' + i)
				if mk == UNKNOWN {
					fmt.Printf("%c = unknown         ", c)
				} else {
					fmt.Printf("%c = %012X    ", c, mk)
				}
				if (i+1)%3 == 0 {
					fmt.Println()
				}
			}
			ed.term.color(4, 0)
			fmt.Print("[ hit any key ]")
			flushOut()
			termGetch()
			ed.term.clear()
			continue
		}

		// Mark set (m)
		if ch == 'm' {
			ch2 := termGetch()
			if ch2 >= 'a' && ch2 <= 'z' {
				ed.memory.mark[ch2-'a'] = ed.display.fpos()
				ed.display.stdmm(
					fmt.Sprintf("Mark '%c' set at %X", ch2, ed.display.fpos()),
					ed.scriptingflag, ed.verbose)
			}
			continue
		}

		// Jump to mark (')
		if ch == '\'' {
			ch2 := termGetch()
			if ch2 >= 'a' && ch2 <= 'z' {
				mkv := ed.memory.mark[ch2-'a']
				if mkv != UNKNOWN {
					ed.display.jump(mkv)
				} else {
					ed.display.stdmm(fmt.Sprintf("Mark '%c' not set", ch2),
						ed.scriptingflag, ed.verbose)
				}
			}
			continue
		}

		// Yank (y) / Paste (p/P)
		if ch == 'y' {
			ch2 := termGetch()
			if ch2 == 'y' {
				ed.memory.yank_(ed.display.fpos(), ed.display.fpos()+15)
				ed.display.stdmm(fmt.Sprintf("%d bytes yanked.", len(ed.memory.yank)),
					ed.scriptingflag, ed.verbose)
			}
			continue
		}
		if ch == 'p' {
			if len(ed.memory.yank) > 0 {
				ed.saveUndoState()
				ed.memory.overwrite(ed.display.fpos(), ed.memory.yank)
				ed.commitUndo()
				ed.display.stdmm(fmt.Sprintf("%d bytes Pasted.", len(ed.memory.yank)),
					ed.scriptingflag, ed.verbose)
				ed.display.jump(ed.display.fpos() + uint64(len(ed.memory.yank)))
			}
			continue
		} else if ch == 'P' {
			if len(ed.memory.yank) > 0 {
				ed.saveUndoState()
				ed.display.highlightRanges = nil
				ed.memory.insert(ed.display.fpos(), ed.memory.yank)
				ed.commitUndo()
				ed.display.stdmm(fmt.Sprintf("%d bytes Pasted (insert).", len(ed.memory.yank)),
					ed.scriptingflag, ed.verbose)
				ed.display.jump(ed.display.fpos() + uint64(len(ed.memory.yank)))
			}
			continue
		}

		// Insert mode toggle (i)
		if ch == 'i' {
			ed.display.insmod = !ed.display.insmod
			stroke = false
			continue
		}

		// Hex digit editing
		if isHexDigit(byte(ch)) {
			addr := ed.display.fpos()
			c := hexVal(byte(ch))
			sh := uint(0)
			mask := byte(0xF0)
			if (ed.display.curx & 1) != 0 {
				sh = 0
				mask = 0xF0
			} else {
				sh = 4
				mask = 0x0F
			}

			if ed.display.insmod {
				if !stroke && addr < uint64(len(ed.memory.mem)) {
					ed.saveUndoState()
					ed.display.highlightRanges = nil
					b := byte(c) << sh
					ed.memory.insert(addr, []byte{b})
				} else {
					if !stroke {
						ed.saveUndoState()
					}
					ed.memory.set(addr, (ed.memory.read(addr)&mask)|(byte(c)<<sh))
				}
				if (ed.display.curx & 1) != 0 {
					stroke = false
				} else {
					stroke = !stroke
				}
				if !stroke {
					ed.commitUndo()
				}
			} else {
				if (ed.display.curx & 1) == 0 {
					ed.saveUndoState()
				}
				ed.memory.set(addr, (ed.memory.read(addr)&mask)|(byte(c)<<sh))
				if (ed.display.curx & 1) == 1 {
					ed.commitUndo()
				}
			}

			if ed.display.curx < 31 {
				ed.display.curx++
			} else {
				ed.display.curx = 0
				if ed.display.cury < LENONSCR/16-1 {
					ed.display.cury++
				} else {
					ed.display.homeaddr += 16
				}
			}
			continue
		}

		// Delete (x)
		if ch == 'x' {
			ed.saveUndoState()
			if ed.memory.deleteRange(ed.display.fpos(), ed.display.fpos(), false) {
				ed.commitUndo()
				ed.display.highlightRanges = nil
			} else {
				ed.display.stderr_("Invalid range.", ed.scriptingflag, ed.verbose)
				ed.decUndo()
			}
			continue
		}

		// Command mode (:)
		if ch == ':' {
			beforeLen := len(ed.memory.mem)
			line := historyGetln(&ed.history, ":", "command")
			f := ed.commandLine(line)
			if len(ed.memory.mem) != beforeLen {
				ed.display.highlightRanges = nil
			}
			if f == 1 || f == 0 {
				return
			}
		}
	}
}

func (ed *BiEditor) handleSearchInput(line string) {
	if len(line) > 2 && line[0] == '/' && line[1] == '/' {
		// Hex search
		var sm []byte
		ed.parser.getHexs(line, 2, &sm)
		if len(sm) > 0 {
			ed.search.smem = sm
			ed.search.isRegex = false
			ed.search.remem = ""
			ed.display.highlightRanges = nil
			ed.search.searchAll(uint64(len(ed.memory.mem)), &ed.display.highlightRanges)
			if len(ed.display.highlightRanges) > 0 {
				ed.display.jump(ed.display.highlightRanges[0].pos)
				ed.display.stdmm(fmt.Sprintf("Found %d match(es)", len(ed.display.highlightRanges)),
					ed.scriptingflag, ed.verbose)
			} else {
				ed.display.stdmm("Not found", ed.scriptingflag, ed.verbose)
			}
		}
	} else if len(line) > 1 && line[0] == '/' {
		// Regex search
		pattern := line[1:]
		if len(pattern) > 0 && pattern[len(pattern)-1] == '/' {
			pattern = pattern[:len(pattern)-1]
		}
		if pattern != "" {
			ed.search.remem = pattern
			ed.search.isRegex = true
			ed.search.smem = nil
			ed.display.highlightRanges = nil
			ed.search.searchAll(uint64(len(ed.memory.mem)), &ed.display.highlightRanges)
			if len(ed.display.highlightRanges) > 0 {
				ed.display.jump(ed.display.highlightRanges[0].pos)
				ed.display.stdmm(fmt.Sprintf("Found %d match(es)", len(ed.display.highlightRanges)),
					ed.scriptingflag, ed.verbose)
			} else {
				ed.display.stdmm("Not found", ed.scriptingflag, ed.verbose)
			}
		}
	}
}

// ============================================================
// Command line processing
// ============================================================

// commandLine processes a command string; returns 0=quit, 1=quit!, -1=continue
func (ed *BiEditor) commandLine(line string) int {
	ed.cp = ed.display.fpos()
	parsedLine := parserComment(line)
	if parsedLine == "" {
		return -1
	}

	// Quit commands
	if parsedLine == "q" {
		if ed.memory.lastchange {
			ed.display.stderr_("No write since last change. To overriding quit, use 'q!'.",
				ed.scriptingflag, ed.verbose)
			return -1
		}
		return 0
	}
	if parsedLine == "q!" {
		return 0
	}
	if parsedLine == "wq" || parsedLine == "wq!" {
		var msg string
		var success bool
		if gPartial.active {
			success = ed.filemgr.writeFilePartial(ed.filemgr.filename, &msg)
		} else {
			success = ed.filemgr.writeFile(ed.filemgr.filename, &msg)
		}
		if success {
			ed.memory.lastchange = false
			ed.display.stdmm("File written and quit.", ed.scriptingflag, ed.verbose)
			return 0
		}
		return -1
	}

	// Undo/Redo
	if parsedLine == "u" || parsedLine == "undo" {
		ed.undo()
		return -1
	}
	if parsedLine == "U" || parsedLine == "redo" {
		ed.redo()
		return -1
	}

	// Write
	if len(parsedLine) >= 1 && parsedLine[0] == 'w' {
		if len(parsedLine) >= 2 && parsedLine[1] == 'p' {
			// :wp partial write
			var msg string
			fname := ed.filemgr.filename
			after := strings.TrimLeft(parsedLine[2:], " ")
			if after != "" {
				fname = after
			}
			success := ed.filemgr.writeFilePartial(fname, &msg)
			if success {
				ed.memory.lastchange = false
				ed.display.stdmm(msg, ed.scriptingflag, ed.verbose)
			} else {
				ed.display.stderr_(msg, ed.scriptingflag, ed.verbose)
			}
			return -1
		}
		var msg string
		fname := ed.filemgr.filename
		rest := strings.TrimLeft(parsedLine[1:], " ")
		fnameSpecified := rest != ""
		if fnameSpecified {
			fname = rest
		}
		var success bool
		if !fnameSpecified && gPartial.active {
			success = ed.filemgr.writeFilePartial(fname, &msg)
		} else {
			success = ed.filemgr.writeFile(fname, &msg)
		}
		if !fnameSpecified && success {
			ed.memory.lastchange = false
		}
		if msg != "" {
			if success {
				ed.display.stdmm(msg, ed.scriptingflag, ed.verbose)
			} else {
				ed.display.stderr_(msg, ed.scriptingflag, ed.verbose)
			}
		}
		return -1
	}

	// Python execution (@)
	if len(parsedLine) >= 1 && parsedLine[0] == '@' {
		pyCode := strings.TrimLeft(parsedLine[1:], " ")
		if pyCode == "" {
			ed.display.stderr_("Syntax error: No Python code specified.",
				ed.scriptingflag, ed.verbose)
			return -1
		}
		tmpf, err := os.CreateTemp("", "bi_python_*.py")
		if err != nil {
			ed.display.stderr_("Cannot create temporary file.", ed.scriptingflag, ed.verbose)
			return -1
		}
		tmpf.WriteString(pyCode + "\n")
		tmpf.Close()
		if !ed.scriptingflag {
			ed.display.clrmm()
			ed.term.color(7, 0)
			ed.term.locate(0, BOTTOMLN)
		}
		cmd := exec.Command("python3", tmpf.Name())
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		cmd.Run()
		os.Remove(tmpf.Name())
		if !ed.scriptingflag {
			ed.term.color(4, 0)
			fmt.Print("[ Hit a key ]")
			flushOut()
			termGetch()
			ed.term.clear()
		}
		return -1
	}

	// Shell command (!)
	if len(parsedLine) >= 1 && parsedLine[0] == '!' {
		shellCmd := strings.TrimLeft(parsedLine[1:], " ")
		if shellCmd == "" {
			ed.display.stderr_("Syntax error: No shell command specified.",
				ed.scriptingflag, ed.verbose)
			return -1
		}
		if !ed.scriptingflag {
			ed.term.color(7, 0)
			fmt.Println()
			flushOut()
		}
		cmd := exec.Command("sh", "-c", shellCmd)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		cmd.Run()
		if !ed.scriptingflag {
			ed.term.color(4, 0)
			fmt.Print("[ Hit any key to return ]")
			flushOut()
			termGetch()
			ed.term.clear()
		}
		return -1
	}

	// ? - evaluate expression
	if len(parsedLine) >= 1 && parsedLine[0] == '?' {
		if len(parsedLine) >= 2 {
			idx := 1
			v := ed.parser.expression(parsedLine, &idx)
			if v == UNKNOWN {
				ed.display.stderr_("Syntax error: Invalid expression.",
					ed.scriptingflag, ed.verbose)
				return -1
			}
			s := "."
			if v < 0x20 {
				s = fmt.Sprintf("^%c", v+'@')
			} else if v >= 0x7E {
				s = "."
			} else {
				s = fmt.Sprintf("'%c'", rune(v))
			}
			msg := fmt.Sprintf("d%d  x%016X  o%024o %s", v, v, v, s)
			ed.display.stdmm(msg, ed.scriptingflag, ed.verbose)
			if !ed.scriptingflag {
				ed.term.locate(0, BOTTOMLN+1)
				ed.term.color(6, 0)
				fmt.Print("b")
				for i := 63; i >= 0; i-- {
					if i%4 == 3 && i != 63 {
						fmt.Print(" ")
					}
					fmt.Printf("%d", (v>>uint(i))&1)
				}
				flushOut()
				termGetch()
				ed.term.locate(0, BOTTOMLN+1)
				fmt.Printf("%80s", "")
				flushOut()
			} else {
				fmt.Print("b")
				for i := 63; i >= 0; i-- {
					if i%4 == 3 {
						fmt.Print(" ")
					}
					fmt.Printf("%d", (v>>uint(i))&1)
				}
				fmt.Println()
			}
		}
		return -1
	}

	// Search (/)
	if len(parsedLine) >= 1 && parsedLine[0] == '/' {
		ed.handleSearchInput(parsedLine)
		return -1
	}

	// File reload (r/rp)
	if len(parsedLine) >= 1 && parsedLine[0] == 'r' {
		if len(parsedLine) >= 2 && parsedLine[1] == 'p' {
			// :rp - reload with initial partial settings
			var msg string
			success := ed.filemgr.readFilePartial(ed.filemgr.filename,
				gPartial.initOffset, gPartial.initLength, &msg)
			if success {
				ed.display.jump(0)
				ed.display.highlightRanges = nil
				ed.display.stdmm(msg, ed.scriptingflag, ed.verbose)
			} else {
				ed.display.stderr_(msg, ed.scriptingflag, ed.verbose)
			}
			return -1
		}
		if len(parsedLine) == 1 {
			// :r - reload
			var msg string
			var success bool
			if gPartial.active {
				success = ed.filemgr.readFilePartial(ed.filemgr.filename,
					gPartial.offset, gPartial.length, &msg)
			} else {
				fn := ed.filemgr.filename
				success = ed.filemgr.readFile(&fn, &msg)
			}
			ed.display.jump(0)
			ed.display.highlightRanges = nil
			if success {
				displayMsg := msg
				if displayMsg == "" {
					displayMsg = "Original file read."
				}
				ed.display.stdmm(displayMsg, ed.scriptingflag, ed.verbose)
			} else {
				ed.display.stderr_(msg, ed.scriptingflag, ed.verbose)
			}
			return -1
		}
		// :r filename
		rest := strings.TrimLeft(parsedLine[1:], " ")
		if rest != "" {
			ed.filemgr.filename = rest
		}
		var msg string
		var success bool
		if gPartial.active {
			success = ed.filemgr.readFilePartial(ed.filemgr.filename,
				gPartial.offset, gPartial.length, &msg)
		} else {
			fn := ed.filemgr.filename
			success = ed.filemgr.readFile(&fn, &msg)
		}
		ed.display.jump(0)
		ed.display.highlightRanges = nil
		displayMsg := msg
		if displayMsg == "" {
			displayMsg = "Original file read."
		}
		if success {
			ed.display.stdmm(displayMsg, ed.scriptingflag, ed.verbose)
		} else {
			ed.display.stderr_(msg, ed.scriptingflag, ed.verbose)
		}
		return -1
	}

	// Script execution (T/t)
	if len(parsedLine) >= 1 && (parsedLine[0] == 'T' || parsedLine[0] == 't') {
		scriptFile := strings.TrimLeft(parsedLine[1:], " ")
		if scriptFile == "" {
			ed.display.stderr_("Syntax error: No script file specified.",
				ed.scriptingflag, ed.verbose)
			return -1
		}
		oldVerbose := ed.verbose
		oldScripting := ed.scriptingflag
		ed.verbose = parsedLine[0] == 'T'
		fmt.Println()
		result := ed.scripting(scriptFile)
		ed.verbose = oldVerbose
		ed.scriptingflag = oldScripting
		if result == 0 || result == 1 {
			return result
		}
		return -1
	}

	// Search n/N commands
	if parsedLine == "n" || (len(parsedLine) == 1 && parsedLine[0] == 'n') {
		pos := ed.search.searchNext(ed.display.fpos()+1, uint64(len(ed.memory.mem)))
		if pos != UNKNOWN {
			if len(ed.display.highlightRanges) == 0 {
				ed.search.searchAll(uint64(len(ed.memory.mem)), &ed.display.highlightRanges)
			}
			ed.display.jump(pos)
		}
		return -1
	}
	if len(parsedLine) == 1 && parsedLine[0] == 'N' {
		var fprev uint64
		if ed.display.fpos() == 0 {
			fprev = UNKNOWN
		} else {
			fprev = ed.display.fpos() - 1
		}
		pos := ed.search.searchLast(fprev, uint64(len(ed.memory.mem)))
		if pos != UNKNOWN {
			if len(ed.display.highlightRanges) == 0 {
				ed.search.searchAll(uint64(len(ed.memory.mem)), &ed.display.highlightRanges)
			}
			ed.display.jump(pos)
		}
		return -1
	}

	// Range commands: parse [x][,x2] cmd ...
	idx := parserSkipSpc(parsedLine, 0)
	x := ed.parser.expression(parsedLine, &idx)
	xf := false
	xf2 := false
	x2 := x

	if x == UNKNOWN {
		x = ed.display.fpos()
	} else {
		xf = true
	}

	idx = parserSkipSpc(parsedLine, idx)
	if idx < len(parsedLine) && parsedLine[idx] == ',' {
		idx = parserSkipSpc(parsedLine, idx+1)
		if idx < len(parsedLine) && parsedLine[idx] == '*' {
			idx = parserSkipSpc(parsedLine, idx+1)
			t := ed.parser.expression(parsedLine, &idx)
			if t == UNKNOWN {
				t = 1
			}
			x2 = x + t - 1
			xf2 = true
		} else {
			t := ed.parser.expression(parsedLine, &idx)
			if t != UNKNOWN {
				x2 = t
				xf2 = true
			}
		}
	}

	if x2 < x {
		x2 = x
	}
	idx = parserSkipSpc(parsedLine, idx)

	// Partial mode: convert absolute addresses to buffer-relative
	nc := ""
	if idx < len(parsedLine) {
		nc = parsedLine[idx:]
	}
	isRp := len(nc) >= 2 && nc[0] == 'r' && nc[1] == 'p'
	if gPartial.active && gPartial.offset > 0 && !isRp {
		if xf {
			if x >= gPartial.offset {
				x -= gPartial.offset
			} else {
				ed.display.stderr_("Invalid range.", ed.scriptingflag, ed.verbose)
				return -1
			}
		}
		if xf2 {
			if x2 >= gPartial.offset {
				x2 -= gPartial.offset
			} else {
				ed.display.stderr_("Invalid range.", ed.scriptingflag, ed.verbose)
				return -1
			}
		} else if xf {
			x2 = x
		}
	}

	// Just a jump if no command follows
	if idx >= len(parsedLine) || parsedLine[idx] == 0 {
		ed.display.jump(x)
		return -1
	}

	return ed.executeCommand(parsedLine, idx, x, x2, xf, xf2)
}

func (ed *BiEditor) executeCommand(line string, idx int, x, x2 uint64, xf, xf2 bool) int {
	// yank (y)
	if line[idx] == 'y' {
		idx++
		if !xf && !xf2 {
			var m []byte
			ed.parser.getHexs(line, idx, &m)
			ed.memory.yank = m
		} else {
			ed.memory.yank_(x, x2)
		}
		ed.display.stdmm(fmt.Sprintf("%d bytes yanked.", len(ed.memory.yank)),
			ed.scriptingflag, ed.verbose)
		return -1
	}

	// range write (w filename)
	if line[idx] == 'w' {
		idx++
		idx = parserSkipSpc(line, idx)
		if idx >= len(line) {
			ed.display.stderr_("Filename required for range write (ex: 100,1ff w dump.bin)",
				ed.scriptingflag, ed.verbose)
			return -1
		}
		fname := strings.TrimLeft(line[idx:], " ")
		if !xf || !xf2 || x > x2 {
			ed.display.stderr_("Invalid range.", ed.scriptingflag, ed.verbose)
			return -1
		}
		if x >= uint64(len(ed.memory.mem)) {
			ed.display.stderr_("Range start is beyond end of buffer.", ed.scriptingflag, ed.verbose)
			return -1
		}
		if x2 >= uint64(len(ed.memory.mem)) {
			x2 = uint64(len(ed.memory.mem)) - 1
		}
		data := append([]byte{}, ed.memory.mem[x:x2+1]...)
		err := os.WriteFile(fname, data, 0644)
		if err != nil {
			ed.display.stderr_("Cannot open output file.", ed.scriptingflag, ed.verbose)
			return -1
		}
		ed.display.stdmm(fmt.Sprintf("%d bytes written to '%s'", len(data), fname),
			ed.scriptingflag, ed.verbose)
		return -1
	}

	// paste (p/P)
	if line[idx] == 'p' {
		if len(ed.memory.yank) > 0 {
			ed.saveUndoState()
			ed.memory.overwrite(x, ed.memory.yank)
			ed.commitUndo()
			ed.display.stdmm(fmt.Sprintf("%d bytes Pasted.", len(ed.memory.yank)),
				ed.scriptingflag, ed.verbose)
			ed.display.jump(x + uint64(len(ed.memory.yank)))
		}
		return -1
	}
	if line[idx] == 'P' {
		if len(ed.memory.yank) > 0 {
			ed.saveUndoState()
			ed.memory.insert(x, ed.memory.yank)
			ed.commitUndo()
			ed.display.stdmm(fmt.Sprintf("%d bytes Pasted (insert).", len(ed.memory.yank)),
				ed.scriptingflag, ed.verbose)
			ed.display.jump(x + uint64(len(ed.memory.yank)))
		}
		return -1
	}

	// mark (m)
	if line[idx] == 'm' {
		if idx+1 < len(line) && line[idx+1] >= 'a' && line[idx+1] <= 'z' {
			ed.memory.mark[line[idx+1]-'a'] = x
			return -1
		} else if idx+1 < len(line) {
			ed.display.stderr_("Syntax error: Invalid mark character (use 'ma' to 'mz').",
				ed.scriptingflag, ed.verbose)
			return -1
		}
	}

	// partial read (rp)
	if idx+1 < len(line) && line[idx] == 'r' && line[idx+1] == 'p' {
		absOff := gPartial.initOffset
		loadLen := gPartial.initLength
		if xf {
			absOff = x
		}
		if xf2 {
			if x2 >= absOff {
				loadLen = x2 - absOff + 1
			} else {
				loadLen = 0
			}
		}
		var msg string
		success := ed.filemgr.readFilePartial(ed.filemgr.filename, absOff, loadLen, &msg)
		if success {
			gPartial.initOffset = absOff
			gPartial.initLength = loadLen
			ed.display.jump(0)
			ed.display.highlightRanges = nil
			ed.display.stdmm(msg, ed.scriptingflag, ed.verbose)
		} else {
			ed.display.stderr_(msg, ed.scriptingflag, ed.verbose)
		}
		return -1
	}

	// read file (r/R)
	if line[idx] == 'r' || line[idx] == 'R' {
		cmd := line[idx]
		idx++
		idx = parserSkipSpc(line, idx)
		fname := ed.filemgr.filename
		if idx < len(line) {
			fname = line[idx:]
		}
		f, err := os.Open(fname)
		if err != nil {
			ed.display.stderr_("File read error.", ed.scriptingflag, ed.verbose)
			return -1
		}
		defer f.Close()
		info, _ := f.Stat()
		if info.Size() > 0 {
			buf := make([]byte, info.Size())
			n, _ := f.Read(buf)
			buf = buf[:n]
			ed.saveUndoState()
			if cmd == 'r' {
				ed.memory.overwrite(x, buf)
			} else {
				ed.memory.insert(x, buf)
			}
			ed.commitUndo()
			ed.display.stdmm(fmt.Sprintf("%d bytes read from %s", n, fname),
				ed.scriptingflag, ed.verbose)
			ed.display.jump(x + uint64(n))
		}
		return -1
	}

	// delete (d)
	if line[idx] == 'd' {
		ed.saveUndoState()
		if ed.memory.deleteRange(x, x2, true) {
			ed.commitUndo()
			ed.display.stdmm(fmt.Sprintf("%d bytes deleted.", x2-x+1),
				ed.scriptingflag, ed.verbose)
			ed.display.jump(x)
		} else {
			ed.display.stderr_("Invalid range.", ed.scriptingflag, ed.verbose)
			ed.decUndo()
		}
		return -1
	}

	// insert/overwrite (i/I)
	if line[idx] == 'i' || line[idx] == 'I' {
		cmd := line[idx]
		idx++
		idx = parserSkipSpc(line, idx)

		var pattern []byte
		isRepeat := false
		var repeatCount uint64 = 1

		if idx < len(line) && line[idx] == '/' {
			var str string
			idx = parserGetRestr(line, idx+1, &str)
			pattern = []byte(str)
		} else {
			idx = ed.parser.getHexs(line, idx, &pattern)
		}

		if len(pattern) == 0 {
			ed.display.stderr_("No data specified.", ed.scriptingflag, ed.verbose)
			return -1
		}

		idx = parserSkipSpc(line, idx)
		if idx < len(line) && line[idx] == '*' {
			idx++
			idx = parserSkipSpc(line, idx)
			n := ed.parser.expression(line, &idx)
			if n != UNKNOWN && n > 0 {
				isRepeat = true
				repeatCount = n
			} else {
				ed.display.stderr_("Invalid repeat count.", ed.scriptingflag, ed.verbose)
				return -1
			}
		}

		if xf && xf2 {
			if x > x2 {
				ed.display.stderr_("Invalid range (start > end).", ed.scriptingflag, ed.verbose)
				return -1
			}
			if x >= uint64(len(ed.memory.mem)) {
				ed.display.stderr_("Invalid range.", ed.scriptingflag, ed.verbose)
				return -1
			}
			if x2 >= uint64(len(ed.memory.mem)) {
				x2 = uint64(len(ed.memory.mem)) - 1
			}
		} else {
			x = ed.display.fpos()
			x2 = x
		}

		ed.saveUndoState()

		if cmd == 'I' { // insert
			var dataToInsert []byte
			if isRepeat {
				for r := uint64(0); r < repeatCount; r++ {
					dataToInsert = append(dataToInsert, pattern...)
				}
			} else if xf && xf2 {
				rangeLen := x2 - x + 1
				full := rangeLen / uint64(len(pattern))
				rem := rangeLen % uint64(len(pattern))
				for r := uint64(0); r < full; r++ {
					dataToInsert = append(dataToInsert, pattern...)
				}
				dataToInsert = append(dataToInsert, pattern[:rem]...)
			} else {
				dataToInsert = append(dataToInsert, pattern...)
			}
			ed.memory.insert(x, dataToInsert)
			ed.commitUndo()
			ed.display.jump(x + uint64(len(dataToInsert)))
			ed.display.stdmm(fmt.Sprintf("%d bytes inserted.", len(dataToInsert)),
				ed.scriptingflag, ed.verbose)
		} else { // overwrite
			var rangeLen uint64
			if !xf && !xf2 {
				rangeLen = uint64(len(pattern)) * repeatCount
			} else if xf && xf2 {
				rangeLen = x2 - x + 1
			} else if xf && !xf2 {
				rangeLen = uint64(len(pattern)) * repeatCount
			}
			var dataToWrite []byte
			if isRepeat {
				for r := uint64(0); r < repeatCount; r++ {
					dataToWrite = append(dataToWrite, pattern...)
				}
				if uint64(len(dataToWrite)) > rangeLen {
					dataToWrite = dataToWrite[:rangeLen]
				}
			} else {
				full := rangeLen / uint64(len(pattern))
				rem := rangeLen % uint64(len(pattern))
				for r := uint64(0); r < full; r++ {
					dataToWrite = append(dataToWrite, pattern...)
				}
				dataToWrite = append(dataToWrite, pattern[:rem]...)
			}
			ed.memory.overwrite(x, dataToWrite)
			ed.commitUndo()
			ed.display.jump(x + uint64(len(dataToWrite)))
			ed.display.stdmm(fmt.Sprintf("%d bytes overwritten.", len(dataToWrite)),
				ed.scriptingflag, ed.verbose)
		}
		return -1
	}

	// substitute (s)
	if line[idx] == 's' {
		ed.saveUndoState()
		r := ed.sCommand(x, x2, xf, xf2, line, idx+1)
		ed.commitUndo()
		return r
	}

	// NOT (~)
	if line[idx] == '~' {
		ed.saveUndoState()
		ed.opNot(x, x2)
		ed.commitUndo()
		ed.display.jump(x2 + 1)
		return -1
	}

	// Shift/Rotate (<, >)
	if line[idx] == '<' || line[idx] == '>' {
		dir := line[idx]
		idx++
		multibyte := false
		if idx < len(line) && line[idx] == dir {
			multibyte = true
			idx++
		}
		times := 1
		idx = parserSkipSpc(line, idx)
		t := ed.parser.expression(line, &idx)
		if t != UNKNOWN {
			times = int(t)
		}
		bit := -1
		idx = parserSkipSpc(line, idx)
		if idx < len(line) && line[idx] == ',' {
			idx++
			b := ed.parser.expression(line, &idx)
			if b != UNKNOWN {
				bit = int(b)
			}
		}
		ed.saveUndoState()
		ed.shiftRotate(x, x2, times, bit, multibyte, dir)
		ed.commitUndo()
		return -1
	}

	// Commands with 3rd argument: c, C, v, &, |, ^, f
	cmd := byte(0)
	cmdIdx := idx
	for cmdIdx < len(line) {
		ch := line[cmdIdx]
		if ch == 'c' || ch == 'C' || ch == 'v' || ch == '&' ||
			ch == '|' || ch == '^' || ch == 'f' {
			cmd = ch
			idx = cmdIdx + 1
			break
		}
		cmdIdx++
	}

	if cmd == 0 {
		if idx < len(line) && line[idx] != 0 && line[idx] != ' ' {
			ed.display.stderr_("Unrecognized command.", ed.scriptingflag, ed.verbose)
		}
		return -1
	}

	idx = parserSkipSpc(line, idx)
	x3 := ed.parser.expression(line, &idx)
	if x3 == UNKNOWN {
		ed.display.stderr_("Invalid parameter.", ed.scriptingflag, ed.verbose)
		return -1
	}

	// Partial offset adjustment for x3
	if cmd == 'c' || cmd == 'C' || cmd == 'v' || cmd == 'f' {
		if gPartial.active && gPartial.offset > 0 {
			if x3 >= gPartial.offset {
				x3 -= gPartial.offset
			} else {
				ed.display.stderr_("Invalid range.", ed.scriptingflag, ed.verbose)
				return -1
			}
		}
	}

	// copy/Copy (c/C)
	if cmd == 'c' || cmd == 'C' {
		ed.saveUndoState()
		var m []byte
		for i := x; i <= x2 && i < uint64(len(ed.memory.mem)); i++ {
			m = append(m, ed.memory.mem[i])
		}
		ed.memory.yank = append([]byte{}, m...)
		if cmd == 'c' {
			ed.memory.overwrite(x3, m)
			ed.commitUndo()
			ed.display.stdmm(fmt.Sprintf("%d bytes copied.", x2-x+1),
				ed.scriptingflag, ed.verbose)
			ed.display.jump(x3 + uint64(len(m)))
		} else {
			ed.memory.insert(x3, m)
			ed.commitUndo()
			ed.display.stdmm(fmt.Sprintf("%d bytes inserted.", len(m)),
				ed.scriptingflag, ed.verbose)
			ed.display.jump(x3 + uint64(len(m)))
		}
		return -1
	}

	// move (v)
	if cmd == 'v' {
		ed.saveUndoState()
		xp := ed.movMem(x, x2, x3)
		ed.commitUndo()
		ed.display.jump(xp)
		return -1
	}

	// bit operations
	if cmd == '&' {
		ed.saveUndoState()
		ed.opAnd(x, x2, x3)
		ed.commitUndo()
		ed.display.jump(x2 + 1)
		return -1
	}
	if cmd == '|' {
		ed.saveUndoState()
		ed.opOr(x, x2, x3)
		ed.commitUndo()
		ed.display.jump(x2 + 1)
		return -1
	}
	if cmd == '^' {
		ed.saveUndoState()
		ed.opXor(x, x2, x3)
		ed.commitUndo()
		ed.display.jump(x2 + 1)
		return -1
	}

	// diff compare (f)
	if cmd == 'f' {
		if !xf || !xf2 || x > x2 {
			ed.display.stderr_("Invalid range. Usage: start,end f start2",
				ed.scriptingflag, ed.verbose)
			return -1
		}
		ed.diffCompare(x, x2, x3)
		return -1
	}

	ed.display.stderr_("Unrecognized command.", ed.scriptingflag, ed.verbose)
	return -1
}

// ============================================================
// Editing operations
// ============================================================

func (ed *BiEditor) opAnd(x, x2, x3 uint64) {
	for i := x; i <= x2 && i < uint64(len(ed.memory.mem)); i++ {
		ed.memory.set(i, ed.memory.read(i)&byte(x3&0xFF))
	}
	ed.display.stdmm(fmt.Sprintf("%d bytes anded.", x2-x+1), ed.scriptingflag, ed.verbose)
}

func (ed *BiEditor) opOr(x, x2, x3 uint64) {
	for i := x; i <= x2 && i < uint64(len(ed.memory.mem)); i++ {
		ed.memory.set(i, ed.memory.read(i)|byte(x3&0xFF))
	}
	ed.display.stdmm(fmt.Sprintf("%d bytes ored.", x2-x+1), ed.scriptingflag, ed.verbose)
}

func (ed *BiEditor) opXor(x, x2, x3 uint64) {
	for i := x; i <= x2 && i < uint64(len(ed.memory.mem)); i++ {
		ed.memory.set(i, ed.memory.read(i)^byte(x3&0xFF))
	}
	ed.display.stdmm(fmt.Sprintf("%d bytes xored.", x2-x+1), ed.scriptingflag, ed.verbose)
}

func (ed *BiEditor) opNot(x, x2 uint64) {
	for i := x; i <= x2 && i < uint64(len(ed.memory.mem)); i++ {
		ed.memory.set(i, ^ed.memory.read(i)&0xFF)
	}
	ed.display.stdmm(fmt.Sprintf("%d bytes noted.", x2-x+1), ed.scriptingflag, ed.verbose)
}

func (ed *BiEditor) movMem(start, end, dest uint64) uint64 {
	if start <= dest && dest <= end {
		return end + 1
	}
	memLen := uint64(len(ed.memory.mem))
	if start >= memLen {
		return dest
	}
	var m []byte
	for i := start; i <= end && i < memLen; i++ {
		m = append(m, ed.memory.mem[i])
	}
	ed.memory.deleteRange(start, end, true)
	var xp uint64
	if dest > memLen {
		ed.memory.overwrite(dest, m)
		xp = dest + uint64(len(m))
	} else {
		if dest > start {
			ed.memory.insert(dest-(end-start+1), m)
			xp = dest - (end - start) + uint64(len(m)) - 1
		} else {
			ed.memory.insert(dest, m)
			xp = dest + uint64(len(m))
		}
	}
	ed.display.stdmm(fmt.Sprintf("%d bytes moved.", end-start+1), ed.scriptingflag, ed.verbose)
	return xp
}

func (ed *BiEditor) shiftRotate(x, x2 uint64, times, bit int, multibyte bool, direction byte) {
	for t := 0; t < times; t++ {
		if !multibyte {
			if bit != 0 && bit != 1 {
				// Rotate
				if direction == '<' {
					for i := x; i <= x2 && i < uint64(len(ed.memory.mem)); i++ {
						m := ed.memory.read(i)
						c := (m & 0x80) >> 7
						ed.memory.set(i, (m<<1)|c)
					}
				} else {
					for i := x; i <= x2 && i < uint64(len(ed.memory.mem)); i++ {
						m := ed.memory.read(i)
						c := (m & 0x01) << 7
						ed.memory.set(i, (m>>1)|c)
					}
				}
			} else {
				// Shift
				carry := byte(bit & 1)
				if direction == '<' {
					for i := x; i <= x2 && i < uint64(len(ed.memory.mem)); i++ {
						ed.memory.set(i, (ed.memory.read(i)<<1)|carry)
					}
				} else {
					for i := x; i <= x2 && i < uint64(len(ed.memory.mem)); i++ {
						ed.memory.set(i, (ed.memory.read(i)>>1)|(carry<<7))
					}
				}
			}
		} else {
			// Multibyte
			ln := x2 - x + 1
			if ln == 0 || x >= uint64(len(ed.memory.mem)) {
				continue
			}
			var v uint64
			for i := x2; ; i-- {
				if i < uint64(len(ed.memory.mem)) {
					v = (v << 8) | uint64(ed.memory.read(i))
				}
				if i == x {
					break
				}
			}
			if bit != 0 && bit != 1 {
				if direction == '<' {
					c := uint64(0)
					if v&(1<<(ln*8-1)) != 0 {
						c = 1
					}
					v = (v << 1) | c
				} else {
					c := v & 1
					v = (v >> 1) | (c << (ln*8 - 1))
				}
			} else {
				carry := uint64(bit & 1)
				if direction == '<' {
					v = (v << 1) | carry
				} else {
					v = (v >> 1) | (carry << (ln*8 - 1))
				}
			}
			for i := x; i <= x2 && i < uint64(len(ed.memory.mem)); i++ {
				ed.memory.set(i, byte(v&0xFF))
				v >>= 8
			}
		}
	}
}

// sCommand handles the s (substitute) command
func (ed *BiEditor) sCommand(start, end uint64, xf, xf2 bool, line string, idx int) int {
	ed.search.nff = false
	pos := ed.display.fpos()
	idx = parserSkipSpc(line, idx)
	if !xf && !xf2 {
		start = 0
		end = 0
		if len(ed.memory.mem) > 0 {
			end = uint64(len(ed.memory.mem)) - 1
		}
	}

	// Parse search pattern
	if idx < len(line) && line[idx] == '/' {
		idx++
		if idx < len(line) && line[idx] != '/' {
			// Regex pattern
			var pattern string
			idx = parserGetRestr(line, idx, &pattern)
			ed.search.isRegex = true
			ed.search.remem = pattern
			ed.search.span = 0
		} else if idx < len(line) && line[idx] == '/' {
			// Hex pattern
			var sm []byte
			idx = ed.parser.getHexs(line, idx+1, &sm)
			ed.search.smem = sm
			ed.search.isRegex = false
			ed.search.remem = ""
			ed.search.span = uint64(len(sm))
		} else {
			ed.display.stderr_("Invalid syntax.", ed.scriptingflag, ed.verbose)
			return -1
		}
	}

	if !ed.search.isRegex && ed.search.span == 0 {
		ed.display.stderr_("Specify search object.", ed.scriptingflag, ed.verbose)
		return -1
	}

	// Parse replacement
	var replacement []byte
	idx = parserSkipSpc(line, idx)
	if idx < len(line) && line[idx] == '/' {
		idx++
		if idx >= len(line) {
			ed.display.stderr_("Syntax error: Missing replacement pattern.",
				ed.scriptingflag, ed.verbose)
			return -1
		}
		if line[idx] == '/' {
			idx = ed.parser.getHexs(line, idx+1, &replacement)
		} else {
			var str string
			idx = parserGetRestr(line, idx, &str)
			replacement = []byte(str)
		}
	}

	// Perform substitution
	cnt := 0
	ed.display.jump(start)
	for {
		foundPos := ed.search.searchNextNoLoop(ed.display.fpos())
		if foundPos == UNKNOWN {
			break
		}
		ed.display.jump(foundPos)
		i := ed.display.fpos()
		if i <= end {
			var spanLen uint64
			if ed.search.isRegex {
				spanLen = ed.search.span
			} else {
				spanLen = uint64(len(ed.search.smem))
			}
			ed.memory.deleteRange(i, i+spanLen-1, false)
			if len(replacement) > 0 {
				ed.memory.insert(i, replacement)
			}
			pos = i + uint64(len(replacement))
			cnt++
			ed.display.jump(pos)
		} else {
			break
		}
	}
	ed.display.jump(pos)
	ed.display.stdmm(fmt.Sprintf("  %d times replaced.", cnt), ed.scriptingflag, ed.verbose)
	return -1
}

// diffCompare implements the f command (LCS-based region comparison)
func (ed *BiEditor) diffCompare(x, x2, x3 uint64) {
	n1 := x2 - x + 1
	if n1 > FCMP_MAXN {
		n1 = FCMP_MAXN
		ed.display.stdmm("  Note: comparison truncated to 8192 bytes.",
			ed.scriptingflag, ed.verbose)
	}
	n2 := n1
	if x3 >= uint64(len(ed.memory.mem)) {
		n2 = 0
	} else if x3+n2 > uint64(len(ed.memory.mem)) {
		n2 = uint64(len(ed.memory.mem)) - x3
	}

	s1 := ed.memory.mem[x : x+n1]
	var s2 []byte
	if n2 > 0 {
		s2 = ed.memory.mem[x3 : x3+n2]
	}

	span := FCMP_SPAN
	bw := 2*span + 1

	dp := make([]int, (int(n1)+1)*bw)
	dir := make([]byte, (int(n1)+1)*bw)

	// Initialize boundaries
	for jj := 1; jj <= span && uint64(jj) <= n2; jj++ {
		dir[0*bw+jj+span] = 2
	}
	for ii := 1; ii <= span && uint64(ii) <= n1; ii++ {
		dir[ii*bw+span-ii] = 1
	}

	// DP fill
	for ii := 1; ii <= int(n1); ii++ {
		jlo := ii - span
		if jlo < 1 {
			jlo = 1
		}
		jhi := ii + span
		if uint64(jhi) > n2 {
			jhi = int(n2)
		}
		for jj := jlo; jj <= jhi; jj++ {
			d := jj - ii + span
			cur := ii*bw + d
			best := -1
			bdir := byte(0)
			// diagonal
			if ii >= 1 && jj >= 1 {
				dd := jj - 1 - (ii - 1) + span
				if dd >= 0 && dd < bw {
					prev := dp[(ii-1)*bw+dd]
					if uint64(ii-1) < n1 && uint64(jj-1) < n2 && s1[ii-1] == s2[jj-1] {
						v := prev + 1
						if v > best {
							best = v
							bdir = 3
						}
					} else {
						if prev > best {
							best = prev
							bdir = 4
						}
					}
				}
			}
			// up
			{
				dd := d + 1
				if dd >= 0 && dd < bw {
					v := dp[(ii-1)*bw+dd]
					if v > best {
						best = v
						bdir = 1
					}
				}
			}
			// left
			if jj >= 1 {
				dd := d - 1
				if dd >= 0 && dd < bw {
					v := dp[ii*bw+dd]
					if v > best {
						best = v
						bdir = 2
					}
				}
			}
			if best < 0 {
				best = 0
			}
			dp[cur] = best
			dir[cur] = bdir
		}
	}

	// Traceback
	alignA := make([]int, int(n1)+int(n2)+4)
	alignB := make([]int, int(n1)+int(n2)+4)
	np := 0
	ci := int(n1)
	cj := int(n2)

	absInt := func(x int) int {
		if x < 0 {
			return -x
		}
		return x
	}

	for ci > 0 || cj > 0 {
		inBand := absInt(ci-cj) <= span
		if !inBand || ci == 0 {
			if cj > 0 {
				alignA[np] = -1
				alignB[np] = int(s2[cj-1])
				np++
				cj--
			} else {
				alignA[np] = int(s1[ci-1])
				alignB[np] = -1
				np++
				ci--
			}
		} else if cj == 0 {
			alignA[np] = int(s1[ci-1])
			alignB[np] = -1
			np++
			ci--
		} else {
			d := cj - ci + span
			dv := dir[ci*bw+d]
			if dv == 3 || dv == 4 {
				alignA[np] = int(s1[ci-1])
				alignB[np] = int(s2[cj-1])
				np++
				ci--
				cj--
			} else if dv == 1 {
				alignA[np] = int(s1[ci-1])
				alignB[np] = -1
				np++
				ci--
			} else {
				alignA[np] = -1
				alignB[np] = int(s2[cj-1])
				np++
				cj--
			}
		}
	}

	// Reverse
	for i := 0; i < np/2; i++ {
		alignA[i], alignA[np-1-i] = alignA[np-1-i], alignA[i]
		alignB[i], alignB[np-1-i] = alignB[np-1-i], alignB[i]
	}

	// Display
	fmt.Printf("\x1b[0;36m")
	fmt.Printf("  R1-offs R2-offs  Region1 (%-8X)       |   Region2 (%-8X)\n",
		x+gPartial.offset, x3+gPartial.offset)
	fmt.Printf("\x1b[0;37m")
	flushOut()

	anyDiff := false
	off1, off2 := 0, 0

	for rs := 0; rs < np; rs += 8 {
		re := rs + 8
		if re > np {
			re = np
		}
		rowOff1 := off1
		rowOff2 := off2

		for k := rs; k < re; k++ {
			if alignA[k] != alignB[k] {
				anyDiff = true
				break
			}
		}

		fmt.Printf("  +%05X  +%05X   ", rowOff1, rowOff2)

		for k := rs; k < rs+8; k++ {
			if k < re {
				diff := alignA[k] != alignB[k]
				if diff {
					fmt.Print("\x1b[1;31m")
				}
				if alignA[k] < 0 {
					fmt.Print("-- ")
				} else {
					fmt.Printf("%02X ", byte(alignA[k]))
				}
				if diff {
					fmt.Print("\x1b[0;37m")
				}
			} else {
				fmt.Print("   ")
			}
		}
		fmt.Print(" |   ")
		for k := rs; k < rs+8; k++ {
			if k < re {
				diff := alignA[k] != alignB[k]
				if diff {
					fmt.Print("\x1b[1;31m")
				}
				if alignB[k] < 0 {
					fmt.Print("-- ")
				} else {
					fmt.Printf("%02X ", byte(alignB[k]))
				}
				if diff {
					fmt.Print("\x1b[0;37m")
				}
			} else {
				fmt.Print("   ")
			}
		}
		fmt.Println()
		flushOut()

		for k := rs; k < re; k++ {
			if alignA[k] >= 0 {
				off1++
			}
			if alignB[k] >= 0 {
				off2++
			}
		}
	}

	fmt.Print("\x1b[0m")
	flushOut()

	if !ed.scriptingflag {
		fmt.Print("\x1b[0;32m")
		if anyDiff {
			fmt.Print("  Differences found. [ hit a key ]")
		} else {
			fmt.Print("  Identical. [ hit a key ]")
		}
		termGetch()
		ed.term.clear()
	}
}

// ============================================================
// Scripting
// ============================================================

func (ed *BiEditor) scripting(scriptFile string) int {
	f, err := os.Open(scriptFile)
	if err != nil {
		ed.display.stderr_("Script file open error.", ed.scriptingflag, ed.verbose)
		return -1
	}
	defer f.Close()

	ed.scriptingflag = true
	sc := bufio.NewScanner(f)
	flag := -1
	for sc.Scan() {
		line := sc.Text()
		if line == "" {
			continue
		}
		if ed.verbose {
			fmt.Println(line)
		}
		flag = ed.commandLine(line)
		if flag == 0 || flag == 1 {
			return flag
		}
	}
	return 0
}

// ============================================================
// Main
// ============================================================

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr, "Usage: %s [options] <file> [options]\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "  Options can appear before or after <file>.\n")
		fmt.Fprintf(os.Stderr, "Options:\n")
		fmt.Fprintf(os.Stderr, "  -s <script>  Execute script file\n")
		fmt.Fprintf(os.Stderr, "  -t <color>   Terminal color (black/white)\n")
		fmt.Fprintf(os.Stderr, "  -v           Verbose mode (show commands when scripting)\n")
		fmt.Fprintf(os.Stderr, "  -w           Write file when exiting script\n")
		fmt.Fprintf(os.Stderr, "  -o <offset>  Partial edit: start offset (hex)\n")
		fmt.Fprintf(os.Stderr, "  -l <length>  Partial edit: length in bytes (hex)\n")
		fmt.Fprintf(os.Stderr, "  -e <end>     Partial edit: end offset inclusive (hex)\n")
		os.Exit(1)
	}

	var filename string
	var scriptfile string
	termcol := "black"
	verbose := false
	writeOnExit := false
	var partialOffset, partialLength uint64
	partialMode := false
	hasEndOpt := false
	var endOffsetRaw uint64

	parseHex := func(s string) uint64 {
		v, _ := strconv.ParseUint(s, 16, 64)
		return v
	}

	args := os.Args[1:]
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-s":
			if i+1 < len(args) {
				i++
				scriptfile = args[i]
			}
		case "-t":
			if i+1 < len(args) {
				i++
				termcol = args[i]
			}
		case "-v":
			verbose = true
		case "-w":
			writeOnExit = true
		case "-o":
			if i+1 < len(args) {
				i++
				partialOffset = parseHex(args[i])
				partialMode = true
			}
		case "-l":
			if i+1 < len(args) {
				i++
				partialLength = parseHex(args[i])
				partialMode = true
			}
		case "-e":
			if i+1 < len(args) {
				i++
				endOffsetRaw = parseHex(args[i])
				hasEndOpt = true
				partialMode = true
			}
		default:
			if len(args[i]) > 0 && args[i][0] != '-' {
				if filename == "" {
					filename = args[i]
				}
			} else {
				fmt.Fprintf(os.Stderr, "Unknown option: %s\n", args[i])
				os.Exit(1)
			}
		}
	}

	if hasEndOpt {
		if endOffsetRaw >= partialOffset {
			partialLength = endOffsetRaw - partialOffset + 1
		} else {
			fmt.Fprintf(os.Stderr, "Error: -e value (0x%X) is less than -o value (0x%X).\n",
				endOffsetRaw, partialOffset)
			os.Exit(1)
		}
	}

	if filename == "" {
		fmt.Fprintf(os.Stderr, "Error: No filename specified.\n")
		fmt.Fprintf(os.Stderr, "Usage: %s [options] <file> [options]\n", os.Args[0])
		os.Exit(1)
	}

	ed := newEditor(termcol)
	ed.verbose = verbose
	ed.filemgr.filename = filename

	if scriptfile == "" {
		ed.term.clear()
	} else {
		ed.scriptingflag = true
	}

	gPartial.initOffset = partialOffset
	gPartial.initLength = partialLength

	var msg string
	var success bool
	if partialMode {
		success = ed.filemgr.readFilePartial(filename, partialOffset, partialLength, &msg)
	} else {
		success = ed.filemgr.readFile(&filename, &msg)
	}

	if !success {
		fmt.Fprintln(os.Stderr, msg)
		os.Exit(1)
	} else if msg != "" {
		ed.display.stdmm(msg, ed.scriptingflag, ed.verbose)
	}

	if scriptfile != "" {
		result := ed.scripting(scriptfile)
		if writeOnExit && ed.memory.lastchange {
			var wMsg string
			if partialMode || gPartial.active {
				success = ed.filemgr.writeFilePartial(filename, &wMsg)
			} else {
				success = ed.filemgr.writeFile(filename, &wMsg)
			}
			if success && ed.verbose {
				fmt.Println(wMsg)
			}
		}
		os.Exit(result)
	} else {
		ed.fedit()
		ed.term.color(7, 0)
		ed.term.dispcursor()
		ed.term.locate(0, 23)
	}
}
