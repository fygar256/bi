#!/usr/bin/env python3
import sys
import tty
import termios
import string
import copy
import re
import os
import io
import argparse
import readline


class Terminal:
    """ターミナル制御を担当するクラス"""
    ESC = '\033['
    
    def __init__(self, termcol='black'):
        self.termcol = termcol
        self.coltab = [0, 1, 4, 5, 2, 6, 3, 7]
    
    def nocursor(self):
        print(f"{self.ESC}?25l", end='', flush=True)
    
    def dispcursor(self):
        print(f"{self.ESC}?25h", end='', flush=True)
    
    def up(self, n=1):
        print(f"{self.ESC}{n}A", end='')
    
    def down(self, n=1):
        print(f"{self.ESC}{n}B", end='')
    
    def right(self, n=1):
        print(f"{self.ESC}{n}C", end='')
    
    def left(self, n=1):
        print(f"{self.ESC}{n}D", end='', flush=True)
    
    def locate(self, x=0, y=0):
        print(f"{self.ESC}{y+1};{x+1}H", end='', flush=True)
    
    def scrollup(self, n=1):
        print(f"{self.ESC}{n}S", end='')
    
    def scrolldown(self, n=1):
        print(f"{self.ESC}{n}T", end='')
    
    def clear(self):
        print(f"{self.ESC}2J", end='', flush=True)
        self.locate()
    
    def clraftcur(self):
        print(f"{self.ESC}0J", end='', flush=True)
    
    def clrline(self):
        print(f"{self.ESC}2K", end='', flush=True)
    
    def color(self, col1=7, col2=0):
        if self.termcol == 'black':
            print(f"{self.ESC}3{self.coltab[col1]}m{self.ESC}4{self.coltab[col2]}m", end='', flush=True)
        else:
            print(f"{self.ESC}3{self.coltab[0]}m{self.ESC}4{self.coltab[7]}m", end='', flush=True)
    
    def resetcolor(self):
        print(f"{self.ESC}0m", end='')
    
    @staticmethod
    def getch():
        fd = sys.stdin.fileno()
        old_settings = termios.tcgetattr(fd)
        tty.setraw(sys.stdin.fileno())
        ch = sys.stdin.read(1)
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
        return ch


class HistoryManager:
    """コマンド履歴管理クラス"""
    def __init__(self):
        self.histories = {
            'command': [],
            'search': []
        }
    
    def get_history_list(self):
        return [readline.get_history_item(i) for i in range(1, readline.get_current_history_length() + 1)]
    
    def set_history_list(self, mode):
        history_items = self.histories[mode]
        readline.clear_history()
        for item in history_items:
            readline.add_history(item)
    
    def getln(self, s="", mode="command"):
        mode = "search" if mode == "search" else "command"
        self.set_history_list(mode)
        try:
            user_input = input(s)
        except:
            user_input = ""
        
        self.histories[mode] = self.get_history_list()
        return user_input


class MemoryBuffer:
    """メモリバッファ管理クラス"""
    UNKNOWN = 0xffffffffffffffffffffffffffffffff
    
    def __init__(self):
        self.mem = []
        self.yank = []
        self.mark = [self.UNKNOWN] * 26
        self.modified = False
        self.lastchange = False
    
    def __len__(self):
        return len(self.mem)
    
    def readmem(self, addr):
        if addr >= len(self.mem):
            return 0
        return self.mem[addr] & 0xff
    
    def setmem(self, addr, data):
        if addr >= len(self.mem):
            for i in range(addr - len(self.mem) + 1):
                self.mem.append(0)
        
        if isinstance(data, int) and 0 <= data <= 255:
            self.mem[addr] = data
        else:
            self.mem[addr] = 0
        
        self.modified = True
        self.lastchange = True
    
    def insmem(self, start, mem2):
        if start >= len(self.mem):
            for i in range(start - len(self.mem)):
                self.mem.append(0)
            self.mem = self.mem + mem2
            self.modified = True
            self.lastchange = True
            return
        
        mem1 = self.mem[:start]
        mem3 = self.mem[start:]
        self.mem = mem1 + mem2 + mem3
        self.modified = True
        self.lastchange = True
    
    def delmem(self, start, end, yf, yankmem_func):
        length = end - start + 1
        if length <= 0 or start >= len(self.mem):
            return False
        
        if yf:
            yankmem_func(start, end)
        
        self.mem = self.mem[:start] + self.mem[end+1:]
        self.lastchange = True
        self.modified = True
        return True
    
    def yankmem(self, start, end):
        length = end - start + 1
        if length <= 0 or start >= len(self.mem):
            return 0
        
        self.yank = []
        cnt = 0
        for j in range(start, end + 1):
            if j < len(self.mem):
                cnt += 1
                self.yank.append(self.mem[j] & 0xff)
        return cnt
    
    def ovwmem(self, start, mem0):
        if not mem0:
            return
        
        if start + len(mem0) >= len(self.mem):
            for j in range(start + len(mem0) - len(self.mem)):
                self.mem.append(0)
        
        for j in range(len(mem0)):
            if start + j >= len(self.mem):
                self.mem.append(mem0[j] & 0xff)
            else:
                self.mem[start + j] = mem0[j] & 0xff
        
        self.lastchange = True
        self.modified = True
    
    def redmem(self, start, end):
        m = []
        for i in range(start, end + 1):
            if len(self.mem) > i:
                m.append(self.mem[i] & 0xff)
            else:
                m.append(0)
        return m
    
    def regulate_mem(self):
        for i in range(len(self.mem)):
            try:
                self.mem[i] = self.mem[i] & 0xff
            except:
                self.mem[i] = 0


class SearchEngine:
    """検索エンジンクラス"""
    RELEN = 128
    
    def __init__(self, memory_buffer, display):
        self.memory = memory_buffer
        self.display = display
        self.smem = []
        self.regexp = False
        self.remem = ''
        self.span = 0
        self.nff = True

    def stdmm(self, s):
        self.display.stdmm(s, False, False)

    def clrmm(self):
        self.display.clrmm()
    
    def hit(self, addr):
        for i in range(len(self.smem)):
            if addr + i < len(self.memory.mem) and self.memory.mem[addr + i] == self.smem[i]:
                continue
            else:
                return 0
        return 1
    
    def hitre(self, addr):
        if not self.remem:
            return -1
        
        self.span = 0
        m = []
        
        if addr < len(self.memory.mem) - self.RELEN:
            m = self.memory.mem[addr:addr + self.RELEN]
        else:
            m = self.memory.mem[addr:]
        
        byte_data = bytes(m)
        try:
            ms = byte_data.decode('utf-8', errors='replace')
        except:
            return -1
        
        try:
            f = re.match(self.remem, ms)
        except:
            return -1
        
        if f:
            start, end = f.span()
            self.span = end - start
            matched_str = ms[start:end]
            try:
                matched_bytes = matched_str.encode('utf-8')
            except:
                return -1
            
            self.span = len(matched_bytes)
            return 1
        else:
            return 0
    
    def searchnext(self, fp, mem_len):
        curpos = fp
        start = fp
        if not self.regexp and not self.smem:
            return False
        
        self.stdmm("Wait.")
        while True:
            f = self.hitre(curpos) if self.regexp else self.hit(curpos)
            
            if f == 1:
                self.clrmm()
                return curpos
            elif f < 0:
                self.clrmm()
                return None
            
            curpos += 1
            
            if curpos >= mem_len:
                if self.nff:
                    curpos = 0
                else:
                    self.clrmm()
                    return None
            
            if curpos == start:
                self.clrmm()
                return None
    
    def searchlast(self, fp, mem_len):
        curpos = fp
        start = fp
        if not self.regexp and not self.smem:
            return False
        
        self.stdmm("Wait.")
        while True:
            f = self.hitre(curpos) if self.regexp else self.hit(curpos)
            
            if f == 1:
                self.clrmm()
                return curpos
            elif f < 0:
                self.clrmm()
                return None
            
            curpos -= 1
            if curpos < 0:
                curpos = mem_len - 1
            
            if curpos == start:
                self.clrmm()
                return None


class Display:
    """画面表示クラス"""
    LENONSCR = 19 * 16
    BOTTOMLN = 22
    
    def __init__(self, terminal, memory_buffer):
        self.term = terminal
        self.memory = memory_buffer
        self.homeaddr = 0
        self.curx = 0
        self.cury = 0
        self.utf8 = False
        self.repsw = 0
        self.insmod = False
    
    def fpos(self):
        return self.homeaddr + self.curx // 2 + self.cury * 16
    
    def jump(self, addr):
        if addr < self.homeaddr or addr >= self.homeaddr + self.LENONSCR:
            self.homeaddr = addr & ~(0xff)
        i = addr - self.homeaddr
        self.curx = (i & 0xf) * 2
        self.cury = (i // 16)
    
    def scrup(self):
        if self.homeaddr >= 16:
            self.homeaddr -= 16
    
    def scrdown(self):
        self.homeaddr += 16
    
    def inccurx(self):
        if self.curx < 31:
            self.curx += 1
        else:
            self.curx = 0
            if self.cury < self.LENONSCR // 16 - 1:
                self.cury += 1
            else:
                self.scrdown()
    
    def printchar(self, a):
        if a >= len(self.memory.mem):
            print("~", end='', flush=True)
            return 1
        
        if self.utf8:
            if self.memory.mem[a] < 0x80 or 0x80 <= self.memory.mem[a] <= 0xbf or 0xf8 <= self.memory.mem[a] <= 0xff:
                print(chr(self.memory.mem[a] & 0xff) if 0x20 <= self.memory.mem[a] <= 0x7e else '.', end='')
                return 1
            elif 0xc0 <= self.memory.mem[a] <= 0xdf:
                m = [self.memory.readmem(a + self.repsw), self.memory.readmem(a + 1 + self.repsw)]
                try:
                    ch = bytes(m).decode('utf-8')
                    print(f"{ch}", end='', flush=True)
                    return 2
                except:
                    print(".", end='')
                    return 1
            elif 0xe0 <= self.memory.mem[a] <= 0xef:
                m = [self.memory.readmem(a + self.repsw), self.memory.readmem(a + 1 + self.repsw), self.memory.readmem(a + 2 + self.repsw)]
                try:
                    ch = bytes(m).decode('utf-8')
                    print(f"{ch} ", end='', flush=True)
                    return 3
                except:
                    print(".", end='')
                    return 1
            elif 0xf0 <= self.memory.mem[a] <= 0xf7:
                m = [self.memory.readmem(a + self.repsw), self.memory.readmem(a + 1 + self.repsw), 
                     self.memory.readmem(a + 2 + self.repsw), self.memory.readmem(a + 3 + self.repsw)]
                try:
                    ch = bytes(m).decode('utf-8')
                    print(f"{ch}  ", end='', flush=True)
                    return 4
                except:
                    print(".", end='')
                    return 1
        else:
            print(chr(self.memory.mem[a] & 0xff) if 0x20 <= self.memory.mem[a] <= 0x7e else '.', end='')
            return 1
    
    def print_title(self, filename):
        self.term.locate(0, 0)
        self.term.color(6)
        print(f'bi version 3.4.4 by T.Maekawa                   utf8mode:{"off" if not self.utf8 else self.repsw}     {"insert   " if self.insmod else "overwrite"}   ')
        self.term.color(5)
        if len(filename) > 35:
            fn = filename[0:35]
        else:
            fn = filename
        print(f'file:[{fn:<35}] length:{len(self.memory.mem)} bytes [{("not " if not self.memory.modified else "")+"modified"}]    ')
    
    def repaint(self, filename):
        self.print_title(filename)
        self.term.nocursor()
        self.term.locate(0, 2)
        self.term.color(4)
        print("OFFSET       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F 0123456789ABCDEF ")
        self.term.color(7)
        addr = self.homeaddr
        for y in range(self.LENONSCR // 16):
            self.term.color(5)
            self.term.locate(0, 3 + y)
            print(f"{(addr + y * 16) & 0xffffffffffff:012X} ", end='')
            self.term.color(7)
            for i in range(16):
                a = y * 16 + i + addr
                print(f"~~ " if a >= len(self.memory.mem) else f"{self.memory.mem[a] & 0xff:02X} ", end='')
            self.term.color(6)
            a = y * 16 + addr
            by = 0
            while by < 16:
                c = self.printchar(a)
                a += c
                by += c
            print("  ", end='', flush=True)
        self.term.color(0)
        self.term.dispcursor()
    
    def printdata(self):
        addr = self.fpos()
        a = self.memory.readmem(addr)
        self.term.locate(0, 23)
        self.term.color(6)
        s = '.'
        if a < 0x20:
            s = '^' + chr(a + ord('@'))
        elif a >= 0x7e:
            s = '.'
        else:
            s = "'" + chr(a) + "'"
        if addr < len(self.memory.mem):
            print(f"{addr:012X} : 0x{a:02X} 0b{a:08b} 0o{a:03o} {a} {s}      ", end='', flush=True)
        else:
            print(f"{addr:012X} : ~~                                                   ", end='', flush=True)
    
    def disp_curpos(self):
        self.term.color(4)
        self.term.locate(self.curx // 2 * 3 + 12, self.cury + 3)
        print("[", end='', flush=True)
        self.term.locate(self.curx // 2 * 3 + 15, self.cury + 3)
        print("]", end='', flush=True)
    
    def erase_curpos(self):
        self.term.color(7)
        self.term.locate(self.curx // 2 * 3 + 12, self.cury + 3)
        print(" ", end='', flush=True)
        self.term.locate(self.curx // 2 * 3 + 15, self.cury + 3)
        print(" ", end='', flush=True)
    
    def clrmm(self):
        self.term.locate(0, self.BOTTOMLN)
        self.term.color(6)
        self.term.clrline()
    
    def stdmm(self, s, scripting, verbose):
        if scripting:
            if verbose:
                print(s)
        else:
            self.clrmm()
            self.term.color(4)
            self.term.locate(0, self.BOTTOMLN)
            print(" " + s, end='', flush=True)
    
    def stderr(self, s, scripting, verbose):
        if scripting:
            print(s, file=sys.stderr)
        else:
            self.clrmm()
            self.term.color(3)
            self.term.locate(0, self.BOTTOMLN)
            print(" " + s, end='', flush=True)


class Parser:
    """コマンドパーサークラス"""
    UNKNOWN = 0xffffffffffffffffffffffffffffffff
    
    def __init__(self, memory_buffer, display):
        self.memory = memory_buffer
        self.display = display
    
    @staticmethod
    def skipspc(s, idx):
        while idx < len(s):
            if s[idx] == ' ':
                idx += 1
            else:
                break
        return idx
    
    def get_value(self, s, idx):
        if idx >= len(s):
            return self.UNKNOWN, idx
        idx = self.skipspc(s, idx)
        ch = s[idx]
        
        if ch == '$':
            idx += 1
            v = len(self.memory.mem) - 1 if len(self.memory.mem) != 0 else 0
        elif ch == '{':
            idx += 1
            u = ''
            while idx < len(s):
                if s[idx] == '}':
                    idx += 1
                    break
                u += s[idx]
                idx += 1
            else:
                return self.UNKNOWN, idx
            
            try:
                v = int(eval(u))
            except:
                return self.UNKNOWN, idx
        elif ch == '.':
            idx += 1
            v = self.display.fpos()
        elif ch == "'" and len(s) > idx + 1 and 'a' <= s[idx + 1] <= 'z':
            idx += 1
            v = self.memory.mark[ord(s[idx]) - ord('a')]
            if v == self.UNKNOWN:
                return self.UNKNOWN, idx - 1
            else:
                idx += 1
        elif idx < len(s) and s[idx] in '0123456789abcdefABCDEF':
            x = 0
            while idx < len(s) and s[idx] in '0123456789abcdefABCDEF':
                x = 16 * x + int("0x" + s[idx], 16)
                idx += 1
            v = x
        elif ch == '%':
            x = 0
            idx += 1
            while idx < len(s) and s[idx] in '0123456789':
                x = 10 * x + int(s[idx])
                idx += 1
            v = x
        else:
            v = self.UNKNOWN
        
        if v < 0:
            v = 0
        return v, idx
    
    def expression(self, s, idx):
        x, idx = self.get_value(s, idx)
        if len(s) > idx and x != self.UNKNOWN and s[idx] == '+':
            y, idx = self.get_value(s, idx + 1)
            x = x + y
        elif len(s) > idx and x != self.UNKNOWN and s[idx] == '-':
            y, idx = self.get_value(s, idx + 1)
            x = x - y
            if x < 0:
                x = 0
        return x, idx
    
    @staticmethod
    def get_restr(s, idx):
        m = ''
        while idx < len(s):
            if s[idx] == '/':
                break
            
            if idx + 1 < len(s) and s[idx:idx+2] == "\\\\":
                m += '\\\\'
                idx += 2
            elif idx + 1 < len(s) and s[idx:idx+2] == chr(0x5c) + '/':
                m += '/'
                idx += 2
            elif s[idx] == '\\' and len(s) - 1 == idx:
                idx += 1
                break
            else:
                m += s[idx]
                idx += 1
        return m, idx
    
    def get_hexs(self, s, idx):
        m = []
        while idx < len(s):
            v, idx = self.expression(s, idx)
            if v == self.UNKNOWN:
                break
            m.append(v & 0xff)
        return m, idx
    
    def get_str_or_hexs(self, line, idx):
        idx = self.skipspc(line, idx)
        if idx < len(line) and line[idx] == '/':
            idx += 1
            if idx < len(line) and line[idx] == '/':
                m, idx = self.get_hexs(line, idx + 1)
            else:
                s, idx = self.get_restr(line, idx)
                try:
                    bseq = s.encode('utf-8')
                except:
                    return [], idx
                m = list(bseq)
        else:
            m = []
        return m, idx
    
    def get_str(self, line, idx):
        s, idx = self.get_restr(line, idx)
        try:
            bseq = s.encode('utf-8')
        except:
            return [], idx
        m = list(bseq)
        return m, idx
    
    @staticmethod
    def comment(s):
        idx = 0
        m = ''
        while idx < len(s):
            if s[idx] == '#':
                break
            
            if idx + 1 < len(s) and s[idx:idx+2] == chr(0x5c) + '#':
                m += '#'
                idx += 2
            
            elif idx + 1 < len(s) and s[idx:idx+2] == chr(0x5c) + 'n':
                m += '\n'
                idx += 2
            else:
                m += s[idx]
                idx += 1
        
        return m


class FileManager:
    """ファイル入出力管理クラス"""
    def __init__(self, memory_buffer):
        self.memory = memory_buffer
        self.filename = ""
        self.newfile = False
    
    def readfile(self, fn):
        try:
            f = open(fn, "rb")
        except:
            self.newfile = True
            self.memory.mem = []
            return True, "<new file>"
        else:
            self.newfile = False
            try:
                self.memory.mem = list(f.read())
                f.close()
                return True, None
            except MemoryError:
                f.close()
                return False, "Memory overflow."
    
    def writefile(self, fn):
        self.memory.regulate_mem()
        try:
            f = open(fn, "wb")
            f.write(bytes(self.memory.mem))
            f.close()
            return True, "File written."
        except:
            return False, "Permission denied."
    
    def wrtfile(self, start, end, fn):
        self.memory.regulate_mem()
        try:
            f = open(fn, "wb")
            for i in range(start, end + 1):
                if i < len(self.memory.mem):
                    f.write(bytes([self.memory.mem[i]]))
                else:
                    f.write(bytes([0]))
            f.close()
            return True, None
        except:
            return False, "Permission denied."


class BiEditor:
    """バイナリエディタのメインクラス"""
    def __init__(self, termcol='black'):
        self.term = Terminal(termcol)
        self.memory = MemoryBuffer()
        self.display = Display(self.term, self.memory)
        self.parser = Parser(self.memory, self.display)
        self.history = HistoryManager()
        self.search = SearchEngine(self.memory, self.display)
        self.filemgr = FileManager(self.memory)
        
        self.verbose = False
        self.scriptingflag = False
        self.stack = []
        self.cp = 0
    
    def stdmm(self, s):
        self.display.stdmm(s, self.scriptingflag, self.verbose)
    
    def stderr(self, s):
        self.display.stderr(s, self.scriptingflag, self.verbose)
    
    def disp_marks(self):
        j = 0
        self.term.locate(0, Display.BOTTOMLN)
        self.term.color(7)
        for i in 'abcdefghijklmnopqrstuvwxyz':
            m = self.memory.mark[j]
            if m == MemoryBuffer.UNKNOWN:
                print(f"{i} = unknown         ", end='')
            else:
                print(f"{i} = {self.memory.mark[j]:012X}    ", end='')
            j += 1
            if j % 3 == 0:
                print()
        self.term.color(4)
        print("[ hit any key ]")
        Terminal.getch()
        self.term.clear()
    
    def invoke_shell(self, line):
        self.term.color(7)
        print()
        os.system(line.lstrip())
        self.term.color(4)
        print("[ Hit any key to return ]", end='', flush=True)
        Terminal.getch()
        self.term.clear()
    
    def printvalue(self, s):
        v, idx = self.parser.expression(s, 0)
        if v == Parser.UNKNOWN:
            return
        
        s = ' . '
        if v < 0x20:
            s = '^' + chr(v + ord('@')) + ' '
        elif v >= 0x7e:
            s = ' . '
        else:
            s = "'" + chr(v) + "'"
        
        x = f"{v:016X}"
        spaced_hex = ' '.join([x[i:i+4] for i in range(0, 16, 4)])
        o = f"{v:024o}"
        spaced_oct = ' '.join([o[i:i+4] for i in range(0, 24, 4)])
        b = f"{v:064b}"
        spaced_bin = ' '.join([b[i:i+4] for i in range(0, 64, 4)])
        
        msg = f"d{v:>10}  x{spaced_hex}  o{spaced_oct} {s}\nb{spaced_bin}"
        
        if self.scriptingflag:
            if self.verbose:
                print(msg)
        else:
            self.display.clrmm()
            self.term.color(6)
            self.term.locate(0, Display.BOTTOMLN)
            print(msg, end='', flush=True)
            Terminal.getch()
            self.term.locate(0, Display.BOTTOMLN + 1)
            print(" " * 80, end='', flush=True)
    
    def call_exec(self, line):
        if len(line) <= 1:
            return
        line = line[1:]
        try:
            if self.scriptingflag:
                exec(line, globals())
            else:
                self.display.clrmm()
                self.term.color(7)
                self.term.locate(0, Display.BOTTOMLN)
                exec(line, globals())
                self.term.color(4)
                self.term.clrline()
                print("[ Hit a key ]", end='', flush=True)
                Terminal.getch()
                self.term.clear()
                self.display.repaint(self.filemgr.filename)
        except:
            self.stderr("python exec() error.")

    def fedit(self):
        """フルスクリーンエディタモード"""
        stroke = False
        ch = ''
        self.display.repsw = 0
        
        while True:
            self.cp = self.display.fpos()
            self.display.repaint(self.filemgr.filename)
            self.display.printdata()
            self.term.locate(self.display.curx // 2 * 3 + 13 + (self.display.curx & 1), self.display.cury + 3)
            ch = Terminal.getch()
            self.display.clrmm()
            self.search.nff = True
            
            # エスケープシーケンス処理
            if ch == chr(27):
                c2 = Terminal.getch()
                c3 = Terminal.getch()
                if c3 == 'A':
                    ch = 'k'
                elif c3 == 'B':
                    ch = 'j'
                elif c3 == 'C':
                    ch = 'l'
                elif c3 == 'D':
                    ch = 'h'
                elif c2 == chr(91) and c3 == chr(50):
                    ch = 'i'
            
            # 検索コマンド
            if ch == 'n':
                pos = self.search.searchnext(self.display.fpos() + 1, len(self.memory))
                if pos is not None and pos is not False:
                    self.display.jump(pos)
                elif pos is None:
                    self.stdmm("Not found.")
                continue
            elif ch == 'N':
                pos = self.search.searchlast(self.display.fpos() - 1, len(self.memory))
                if pos is not None and pos is not False:
                    self.display.jump(pos)
                elif pos is None:
                    self.stdmm("Not found.")
                continue
            
            # スクロールコマンド
            elif ch == chr(2):  # Ctrl+B
                if self.display.homeaddr >= 256:
                    self.display.homeaddr -= 256
                else:
                    self.display.homeaddr = 0
                continue
            elif ch == chr(6):  # Ctrl+F
                self.display.homeaddr += 256
                continue
            elif ch == chr(0x15):  # Ctrl+U
                if self.display.homeaddr >= 128:
                    self.display.homeaddr -= 128
                else:
                    self.display.homeaddr = 0
                continue
            elif ch == chr(4):  # Ctrl+D
                self.display.homeaddr += 128
                continue
            
            # カーソル移動
            elif ch == '^':
                self.display.curx = 0
                continue
            elif ch == '$':
                self.display.curx = 30
                continue
            elif ch == 'j':
                if self.display.cury < Display.LENONSCR // 16 - 1:
                    self.display.cury += 1
                else:
                    self.display.scrdown()
                continue
            elif ch == 'k':
                if self.display.cury > 0:
                    self.display.cury -= 1
                else:
                    self.display.scrup()
                continue
            elif ch == 'h':
                if self.display.curx > 0:
                    self.display.curx -= 1
                else:
                    if self.display.fpos() != 0:
                        self.display.curx = 31
                        if self.display.cury > 0:
                            self.display.cury -= 1
                        else:
                            self.display.scrup()
                continue
            elif ch == 'l':
                self.display.inccurx()
                continue
            
            # 表示モード切替
            elif ch == chr(25):  # Ctrl+Y
                self.display.utf8 = not self.display.utf8
                self.term.clear()
                self.display.repaint(self.filemgr.filename)
                continue
            elif ch == chr(12):  # Ctrl+L
                self.term.clear()
                self.display.repsw = (self.display.repsw + (1 if self.display.utf8 else 0)) % 4
                self.display.repaint(self.filemgr.filename)
                continue
            
            # ファイル操作
            elif ch == 'Z':
                success, msg = self.filemgr.writefile(self.filemgr.filename)
                if success:
                    return True
                else:
                    self.stderr(msg)
                    continue
            elif ch == 'q':
                if self.memory.lastchange:
                    self.stdmm("No write since last change. To overriding quit, use 'q!'.")
                    continue
                return False
            
            # マーク操作
            elif ch == 'M':
                self.disp_marks()
                continue
            elif ch == 'm':
                ch = Terminal.getch().lower()
                if 'a' <= ch <= 'z':
                    self.memory.mark[ord(ch) - ord('a')] = self.display.fpos()
                continue
            
            # 検索
            elif ch == '/':
                self.do_search()
                continue
            elif ch == "'":
                ch = Terminal.getch().lower()
                if 'a' <= ch <= 'z':
                    mark_pos = self.memory.mark[ord(ch) - ord('a')]
                    if mark_pos != MemoryBuffer.UNKNOWN:
                        self.display.jump(mark_pos)
                continue
            
            # ヤンク・ペースト
            elif ch == 'p':
                y = list(self.memory.yank)
                self.memory.ovwmem(self.display.fpos(), y)
                self.display.jump(self.display.fpos() + len(y))
                continue
            elif ch == 'P':
                y = list(self.memory.yank)
                self.memory.insmem(self.display.fpos(), y)
                self.display.jump(self.display.fpos() + len(self.memory.yank))
                continue
            
            # 編集モード
            if ch == 'i':
                self.display.insmod = not self.display.insmod
                stroke = False
            elif ch in string.hexdigits:
                addr = self.display.fpos()
                c = int("0x" + ch, 16)
                sh = 4 if not self.display.curx & 1 else 0
                mask = 0xf if not self.display.curx & 1 else 0xf0
                if self.display.insmod:
                    if not stroke and addr < len(self.memory.mem):
                        self.memory.insmem(addr, [c << sh])
                    else:
                        self.memory.setmem(addr, self.memory.readmem(addr) & mask | c << sh)
                    stroke = (not stroke) if not self.display.curx & 1 else False
                else:
                    self.memory.setmem(addr, self.memory.readmem(addr) & mask | c << sh)
                self.display.inccurx()
            elif ch == 'x':
                self.memory.delmem(self.display.fpos(), self.display.fpos(), False, self.memory.yankmem)
            elif ch == ':':
                self.display.disp_curpos()
                f = self.commandln()
                self.display.erase_curpos()
                if f == 1:
                    return True
                elif f == 0:
                    return False
    
    def do_search(self):
        """検索実行"""
        self.display.disp_curpos()
        self.term.locate(0, Display.BOTTOMLN)
        self.term.color(7)
        readline.set_pre_input_hook(lambda: (readline.insert_text('/'), readline.redisplay()))
        
        s = self.history.getln("", "search")
        self.searchsub(self.parser.comment(s))
        self.display.erase_curpos()
    
    def searchsub(self, line):
        """検索サブルーチン"""
        if len(line) > 2 and line[0:2] == '//':
            sm, idx = self.parser.get_hexs(line, 2)
            self.searchhex(sm)
        elif len(line) > 1 and line[0] == '/':
            m, idx = self.parser.get_restr(line, 1)
            self.searchstr(m)
    
    def searchstr(self, s):
        """文字列検索"""
        if s != "":
            self.search.regexp = True
            self.search.remem = s
            pos = self.search.searchnext(self.display.fpos(), len(self.memory))
            if pos is not None and pos is not False:
                self.display.jump(pos)
                return True
        return False
    
    def searchhex(self, sm):
        """16進検索"""
        self.search.remem = ''
        self.search.regexp = False
        if sm:
            self.search.smem = sm
            pos = self.search.searchnext(self.display.fpos(), len(self.memory))
            if pos is not None and pos is not False:
                self.display.jump(pos)
                return True
        return False
    
    def commandln(self):
        """コマンドライン入力"""
        self.term.locate(0, Display.BOTTOMLN)
        self.term.color(7)
        readline.set_pre_input_hook(lambda: (readline.insert_text(''), readline.redisplay()))
        line = self.history.getln(':', "command").lstrip()
        return self.commandline(line)
    
    def commandline(self, line):
        """コマンド実行"""
        try:
            return self.commandline_(line)
        except MemoryError:
            self.stderr("Memory overflow.")
            return -1
    
    def commandline_(self, line):
        """コマンド処理メイン"""
        self.cp = self.display.fpos()
        line = self.parser.comment(line)
        if line == '':
            return -1
        
        # 終了コマンド
        if line == 'q':
            if self.memory.lastchange:
                self.stderr("No write since last change. To overriding quit, use 'q!'.")
                return -1
            return 0
        elif line == 'q!':
            return 0
        elif line == 'wq' or line == 'wq!':
            success, msg = self.filemgr.writefile(self.filemgr.filename)
            if success:
                self.memory.lastchange = False
                return 0
            else:
                return -1
        
        # ファイル書き込み
        elif line[0] == 'w':
            if len(line) >= 2:
                s = line[1:].lstrip()
                success, msg = self.filemgr.writefile(s)
            else:
                success, msg = self.filemgr.writefile(self.filemgr.filename)
                if success:
                    self.memory.lastchange = False
            if msg:
                if success:
                    self.stdmm(msg)
                else:
                    self.stderr(msg)
            return -1
        
        # ファイル読み込み
        elif line[0] == 'r':
            if len(line) < 2:
                success, msg = self.filemgr.readfile(self.filemgr.filename)
                if msg:
                    self.stdmm(msg if success else msg)
                else:
                    self.stdmm("Original file read.")
                return -1
        
        # スクリプト実行
        elif line[0] == 'T' or line[0] == 't':
            if len(line) >= 2:
                s = line[1:].lstrip()
                self.stack.append(self.scriptingflag)
                self.stack.append(self.verbose)
                self.verbose = True if line[0] == 'T' else False
                print("")
                self.scripting(s)
                if self.verbose:
                    self.stdmm("[ Hit any key ]")
                    Terminal.getch()
                self.verbose = self.stack.pop()
                self.scriptingflag = self.stack.pop()
                self.term.clear()
                return -1
            else:
                self.stderr("Specify script file name.")
                return -1
        
        # 検索
        elif line[0] == 'n':
            pos = self.search.searchnext(self.display.fpos() + 1, len(self.memory))
            if pos is not None and pos is not False:
                self.display.jump(pos)
            return -1
        elif line[0] == 'N':
            pos = self.search.searchlast(self.display.fpos() - 1, len(self.memory))
            if pos is not None and pos is not False:
                self.display.jump(pos)
            return -1
        
        # 特殊コマンド
        elif line[0] == '@':
            self.call_exec(line)
            return -1
        elif line[0] == '!':
            if len(line) >= 2:
                self.invoke_shell(line[1:])
            return -1
        elif line[0] == '?':
            self.printvalue(line[1:])
            return -1
        elif line[0] == '/':
            self.searchsub(line)
            return -1
        
        # アドレス範囲コマンドのパース
        return self.parse_range_command(line)
    
    def parse_range_command(self, line):
        """範囲指定コマンドのパース"""
        idx = self.parser.skipspc(line, 0)
        
        x, idx = self.parser.expression(line, idx)
        xf = False
        xf2 = False
        if x == Parser.UNKNOWN:
            x = self.display.fpos()
        else:
            xf = True
        x2 = x
        
        idx = self.parser.skipspc(line, idx)
        if idx < len(line) and line[idx] == ',':
            idx = self.parser.skipspc(line, idx + 1)
            if idx < len(line) and line[idx] == '*':
                idx = self.parser.skipspc(line, idx + 1)
                t, idx = self.parser.expression(line, idx)
                if t == Parser.UNKNOWN:
                    t = 1
                x2 = x + t - 1
            else:
                t, idx = self.parser.expression(line, idx)
                if t == Parser.UNKNOWN:
                    x2 = x
                else:
                    x2 = t
                    xf2 = True
        else:
            x2 = x
        
        if x2 < x:
            x2 = x
        
        idx = self.parser.skipspc(line, idx)
        
        if idx == len(line):
            self.display.jump(x)
            return -1
        
        # 各種コマンドの処理
        return self.execute_command(line, idx, x, x2, xf, xf2)
    
    def execute_command(self, line, idx, x, x2, xf, xf2):
        """個別コマンドの実行"""
        # yank
        if idx < len(line) and line[idx] == 'y':
            idx += 1
            if not xf and not xf2:
                m, idx = self.parser.get_str_or_hexs(line, idx)
                self.memory.yank = list(m)
            else:
                cnt = self.memory.yankmem(x, x2)
            
            self.stdmm(f"{len(self.memory.yank)} bytes yanked.")
            return -1
        
        # paste
        if idx < len(line) and line[idx] == 'p':
            y = list(self.memory.yank)
            self.memory.ovwmem(x, y)
            self.display.jump(x + len(y))
            return -1
        
        if idx < len(line) and line[idx] == 'P':
            y = list(self.memory.yank)
            self.memory.insmem(x, y)
            self.display.jump(x + len(self.memory.yank))
            return -1
        
        # mark
        if idx + 1 < len(line) and line[idx] == 'm':
            if 'a' <= line[idx + 1] <= 'z':
                self.memory.mark[ord(line[idx + 1]) - ord('a')] = x
            return -1
        
        # read file
        if idx < len(line) and (line[idx] == 'r' or line[idx] == 'R'):
            ch = line[idx]
            idx += 1
            if idx >= len(line):
                self.stderr("File name not specified.")
                return -1
            fn = line[idx:].lstrip()
            if fn == "":
                self.stderr("File name not specified.")
            else:
                try:
                    f = open(fn, "rb")
                    data = list(f.read())
                    f.close()
                except:
                    data = []
                    self.stderr("File read error.")
            
            if ch == 'r':
                self.memory.ovwmem(x, data)
            elif ch == 'R':
                self.memory.insmem(x, data)
            
            self.display.jump(x + len(data))
            return -1
        
        if idx < len(line):
            ch = line[idx]
        else:
            ch = ''
        
        # delete
        if ch == 'd':
            if self.memory.delmem(x, x2, True, self.memory.yankmem):
                self.stdmm(f"{x2 - x + 1} bytes deleted.")
                self.display.jump(x)
            return -1
        
        # write file
        elif ch == 'w':
            idx += 1
            fn = line[idx:].lstrip()
            success, msg = self.filemgr.wrtfile(x, x2, fn)
            if msg:
                self.stderr(msg)
            return -1
        
        # substitute
        elif ch == 's':
            self.scommand(x, x2, xf, xf2, line, idx + 1)
            return -1
        
        # not
        if idx < len(line) and line[idx] == '~':
            self.openot(x, x2)
            self.display.jump(x2 + 1)
            return -1
        
        # その他の複雑なコマンド
        if idx < len(line) and line[idx] in "IivCc&|^<>":
            return self.execute_complex_command(line, idx, x, x2, xf, xf2)
        
        self.stderr("Unrecognized command.")
        return -1
    
    def execute_complex_command(self, line, idx, x, x2, xf, xf2):
        """複雑なコマンドの実行（シフト、ビット演算など）"""
        ch = line[idx]
        idx += 1
        
        # シフト・ローテート
        if ch in '<>':
            multibyte = False
            if idx < len(line) and line[idx] == ch:
                idx += 1
                multibyte = True
            
            times, idx = self.parser.expression(line, idx)
            if times == Parser.UNKNOWN:
                times = 1
            
            if idx < len(line) and line[idx] == ',':
                bit, idx = self.parser.expression(line, idx + 1)
            else:
                bit = Parser.UNKNOWN
            
            self.shift_rotate(x, x2, times, bit, multibyte, ch)
            return -1
        
        # insert/Insert
        if ch == 'i' or ch == 'I':
            idx = self.parser.skipspc(line, idx)
            if idx < len(line) and line[idx] == '/':
                m, idx = self.parser.get_str(line, idx + 1)
            else:
                m, idx = self.parser.get_hexs(line, idx)
            
            if idx < len(line) and line[idx] == '*':
                idx += 1
                length, idx = self.parser.expression(line, idx)
            else:
                length = 1
            
            # fill mode for 'i' with range
            if ch == 'i' and xf2:
                if len(m):
                    data = m * ((x2 - x + 1) // len(m)) + m[0:((x2 - x + 1) % len(m))]
                    self.memory.ovwmem(x, data)
                    self.stdmm(f"{len(data)} bytes filled.")
                    self.display.jump(x + len(data))
                else:
                    self.stderr("Invalid syntax.")
                return -1
            
            if ch == 'I' and xf2:
                self.stderr("Invalid syntax.")
                return -1
            
            data = m * length
            if ch == 'i':
                self.memory.ovwmem(x, data)
                self.stdmm(f"{len(data)} bytes overwritten.")
            else:
                self.memory.insmem(x, data)
                self.stdmm(f"{len(data)} bytes inserted.")
            
            self.display.jump(x + len(data))
            return -1
        
        # 残りのコマンドは第3引数が必要
        x3, idx = self.parser.expression(line, idx)
        if x3 == Parser.UNKNOWN:
            self.stderr("Invalid parameter.")
            return -1
        
        # copy/Copy
        if ch == 'c':
            self.memory.yankmem(x, x2)
            m = self.memory.redmem(x, x2)
            self.memory.ovwmem(x3, m)
            self.stdmm(f"{x2 - x + 1} bytes copied.")
            self.display.jump(x3 + (x2 - x + 1))
            return -1
        elif ch == 'C':
            m = self.memory.redmem(x, x2)
            self.memory.yankmem(x, x2)
            self.memory.insmem(x3, m)
            self.stdmm(f"{x2 - x + 1} bytes inserted.")
            self.display.jump(x3 + len(m))
            return -1
        
        # move
        elif ch == 'v':
            xp = self.movmem(x, x2, x3)
            self.display.jump(xp)
            return -1
        
        # ビット演算
        elif ch == '&':
            self.opeand(x, x2, x3)
            self.display.jump(x2 + 1)
            return -1
        elif ch == '|':
            self.opeor(x, x2, x3)
            self.display.jump(x2 + 1)
            return -1
        elif ch == '^':
            self.opexor(x, x2, x3)
            self.display.jump(x2 + 1)
            return -1
        
        return -1
    
    # 各種操作メソッド
    def opeand(self, x, x2, x3):
        for i in range(x, x2 + 1):
            self.memory.setmem(i, self.memory.readmem(i) & (x3 & 0xff))
        self.stdmm(f"{x2 - x + 1} bytes anded.")
    
    def opeor(self, x, x2, x3):
        for i in range(x, x2 + 1):
            self.memory.setmem(i, self.memory.readmem(i) | (x3 & 0xff))
        self.stdmm(f"{x2 - x + 1} bytes ored.")
    
    def opexor(self, x, x2, x3):
        for i in range(x, x2 + 1):
            self.memory.setmem(i, self.memory.readmem(i) ^ (x3 & 0xff))
        self.stdmm(f"{x2 - x + 1} bytes xored.")
    
    def openot(self, x, x2):
        for i in range(x, x2 + 1):
            self.memory.setmem(i, (~(self.memory.readmem(i)) & 0xff))
        self.stdmm(f"{x2 - x + 1} bytes noted.")
    
    def movmem(self, start, end, dest):
        if start <= dest <= end:
            return end + 1
        l = len(self.memory.mem)
        if start >= l:
            return dest
        m = self.memory.redmem(start, end)
        self.memory.delmem(start, end, True, self.memory.yankmem)
        if dest > l:
            self.memory.ovwmem(dest, m)
            xp = dest + len(m)
        else:
            if dest > start:
                self.memory.insmem(dest - (end - start + 1), m)
                xp = dest - (end - start) + len(m) - 1
            else:
                self.memory.insmem(dest, m)
                xp = dest + len(m)
        self.stdmm(f"{end - start + 1} bytes moved.")
        return xp
    
    def shift_rotate(self, x, x2, times, bit, multibyte, direction):
        """シフト・ローテート操作"""
        for i in range(times):
            if not multibyte:
                if bit != 0 and bit != 1:
                    if direction == '<':
                        self.left_rotate_byte(x, x2)
                    else:
                        self.right_rotate_byte(x, x2)
                else:
                    if direction == '<':
                        self.left_shift_byte(x, x2, bit & 1)
                    else:
                        self.right_shift_byte(x, x2, bit & 1)
            else:
                if bit != 0 and bit != 1:
                    if direction == '<':
                        self.left_rotate_multibyte(x, x2)
                    else:
                        self.right_rotate_multibyte(x, x2)
                else:
                    if direction == '<':
                        self.left_shift_multibyte(x, x2, bit & 1)
                    else:
                        self.right_shift_multibyte(x, x2, bit & 1)
    
    def left_shift_byte(self, x, x2, c):
        for i in range(x, x2 + 1):
            self.memory.setmem(i, (self.memory.readmem(i) << 1) | (c & 1))
    
    def right_shift_byte(self, x, x2, c):
        for i in range(x, x2 + 1):
            self.memory.setmem(i, (self.memory.readmem(i) >> 1) | ((c & 1) << 7))
    
    def left_rotate_byte(self, x, x2):
        for i in range(x, x2 + 1):
            m = self.memory.readmem(i)
            c = (m & 0x80) >> 7
            self.memory.setmem(i, (m << 1) | c)
    
    def right_rotate_byte(self, x, x2):
        for i in range(x, x2 + 1):
            m = self.memory.readmem(i)
            c = (m & 0x01) << 7
            self.memory.setmem(i, (m >> 1) | c)
    
    def get_multibyte_value(self, x, x2):
        v = 0
        for i in range(x2, x - 1, -1):
            v = (v << 8) | self.memory.readmem(i)
        return v
    
    def put_multibyte_value(self, x, x2, v):
        for i in range(x, x2 + 1):
            self.memory.setmem(i, v & 0xff)
            v >>= 8
    
    def left_shift_multibyte(self, x, x2, c):
        v = self.get_multibyte_value(x, x2)
        self.put_multibyte_value(x, x2, (v << 1) | c)
    
    def right_shift_multibyte(self, x, x2, c):
        v = self.get_multibyte_value(x, x2)
        self.put_multibyte_value(x, x2, (v >> 1) | (c << ((x2 - x) * 8 + 7)))
    
    def left_rotate_multibyte(self, x, x2):
        v = self.get_multibyte_value(x, x2)
        c = 1 if v & (1 << ((x2 - x) * 8 + 7)) else 0
        self.put_multibyte_value(x, x2, (v << 1) | c)
    
    def right_rotate_multibyte(self, x, x2):
        v = self.get_multibyte_value(x, x2)
        c = 1 if v & 0x1 else 0
        self.put_multibyte_value(x, x2, (v >> 1) | (c << ((x2 - x) * 8 + 7)))

    def scommand(self, start, end, xf, xf2, line, idx):
        """置換コマンド"""
        self.search.nff = False
        pos = self.display.fpos()
        
        idx = self.parser.skipspc(line, idx)
        if not xf and not xf2:
            start = 0
            end = len(self.memory.mem) - 1
        
        m = ''
        if idx < len(line) and line[idx] == '/':
            idx += 1
            if idx < len(line) and line[idx] != '/':
                m, idx = self.parser.get_restr(line, idx)
                self.search.regexp = True
                self.search.remem = m
                self.search.span = len(m)
            elif idx < len(line) and line[idx] == '/':
                self.search.smem, idx = self.parser.get_hexs(line, idx + 1)
                self.search.regexp = False
                self.search.remem = ''
                self.search.span = len(self.search.smem)
            else:
                self.stderr(f"Invalid syntax.")
                return
        
        if self.search.span == 0:
            self.stderr(f"Specify search object.")
            return
        
        n, idx = self.parser.get_str_or_hexs(line, idx)
        
        i = start
        cnt = 0
        self.display.jump(i)
        
        while True:
            f = self.searchnextnoloop(self.display.fpos())
            
            i = self.display.fpos()
            
            if f < 0:
                return
            elif i <= end and f == 1:
                self.memory.delmem(i, i + self.search.span - 1, False, self.memory.yankmem)
                self.memory.insmem(i, n)
                pos = i + len(n)
                cnt += 1
                i = pos
                self.display.jump(i)
            else:
                self.display.jump(pos)
                self.stdmm(f"  {cnt} times replaced.")
                return
    
    def searchnextnoloop(self, fp):
        """ループしない検索"""
        cur_pos = fp
        
        if not self.search.regexp and not self.search.smem:
            return 0
        self.stdmm("Wait.")
        
        while True:
            if self.search.regexp:
                f = self.search.hitre(cur_pos)
            else:
                f = self.search.hit(cur_pos)
            
            if f == 1:
                self.display.jump(cur_pos)
                self.display.clrmm()
                return 1
            
            elif f < 0:
                self.display.clrmm()
                return -1
            
            cur_pos += 1
            
            if cur_pos >= len(self.memory.mem):
                self.display.jump(len(self.memory.mem))
                self.display.clrmm()
                return 0
    
    def scripting(self, scriptfile):
        """スクリプト実行"""
        try:
            f = open(scriptfile, "rt")
        except:
            self.stderr("Script file open error.")
            return False
        
        line = f.readline().strip()
        flag = -1
        self.scriptingflag = True
        
        while line:
            if self.verbose:
                print(line)
            flag = self.commandline(line)
            if flag == 0:
                f.close()
                return 0
            elif flag == 1:
                f.close()
                return 1
            line = f.readline().strip()
        
        f.close()
        return 0


def main():
    """メイン関数"""
    parser = argparse.ArgumentParser()
    parser.add_argument('file', help='file to edit')
    parser.add_argument('-s', '--script', type=str, default='', metavar='script.bi', help='bi script file')
    parser.add_argument('-t', '--termcolor', type=str, default='black', help='background color of terminal. default is \'black\' the others are white.')
    parser.add_argument('-v', '--verbose', action='store_true', help='verbose when processing script')
    parser.add_argument('-w', '--write', action='store_true', help='write file when exiting script')
    args = parser.parse_args()
    
    # エディタの初期化
    editor = BiEditor(termcol=args.termcolor)
    editor.filemgr.filename = args.file
    editor.verbose = args.verbose
    
    # 画面クリア（スクリプトモード以外）
    if not args.script:
        editor.term.clear()
    else:
        editor.scriptingflag = True
    
    # ファイル読み込み
    success, msg = editor.filemgr.readfile(args.file)
    if not success:
        print(msg, file=sys.stderr)
        return
    elif msg:
        editor.stdmm(msg)
    
    # スクリプト実行またはインタラクティブモード
    if args.script:
        try:
            f = editor.scripting(args.script)
            if args.write and editor.memory.lastchange:
                editor.filemgr.writefile(args.file)
        except:
            editor.filemgr.writefile("file.save")
            editor.stderr("Some error occured. memory saved to file.save.")
    else:
        try:
            editor.fedit()
        except:
            editor.filemgr.writefile("file.save")
            editor.stderr("Some error occured. memory saved to file.save.")
    
    # 終了処理
    editor.term.color(7)
    editor.term.dispcursor()
    editor.term.locate(0, 23)


if __name__ == "__main__":
    main()
