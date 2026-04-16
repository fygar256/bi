#!/usr/bin/env python3
import sys
import tty
import termios
import string
import re
import os
import io
import argparse

# ========================================================================
# readline フォールバック実装 (C版の #ifndef HAVE_READLINE から移植)
# ========================================================================
try:
    import readline
    HAVE_READLINE = True
except ImportError:
    HAVE_READLINE = False
    import sys
    print("Warning: readline module not available. Using fallback implementation.", 
          file=sys.stderr)
    
    class ReadlineFallback:
        """
        C版の readline 代替実装を Python に移植
        readlineがない環境（Windowsなど）でも基本的な機能を提供
        """
        def __init__(self):
            self._history = []
        
        def add_history(self, line):
            """履歴に追加（重複は除く）"""
            if line and (not self._history or self._history[-1] != line):
                self._history.append(line)
                # 履歴サイズの制限
                if len(self._history) > 1000:
                    self._history.pop(0)
        
        def clear_history(self):
            """履歴をクリア"""
            self._history = []
        
        def get_history_item(self, index):
            """履歴項目を取得（1-indexed、readline互換）"""
            if 1 <= index <= len(self._history):
                return self._history[index - 1]
            return None
        
        def get_current_history_length(self):
            """現在の履歴数を取得"""
            return len(self._history)

        def set_pre_input_hook(self, hook=None):
            """readline非対応環境用スタブ（何もしない）"""
            pass

        def insert_text(self, text):
            """readline非対応環境用スタブ（何もしない）"""
            pass

        def redisplay(self):
            """readline非対応環境用スタブ（何もしない）"""
            pass
    
    readline = ReadlineFallback()




# ========================================================================
# グローバル変数: @コマンド(exec)や{}式(eval)からアクセス可能
#   mem  -- 編集中ファイルのバイト列 (list[int])
#   cp   -- 現在のカーソル位置 (int, current position)
#   setmem(addr, data) -- memへの書き込みヘルパー
# ========================================================================
mem: list = []
cp: int = 0

def setmem(addr: int, data: int) -> None:
    """グローバルな mem[] にバイト値を書き込む。
    addr がリスト末尾を超える場合は自動的に拡張する。
    BiEditor.call_exec() が実行後に self.memory.mem へ同期する。
    """
    global mem
    while len(mem) <= addr:
        mem.append(0)
    mem[addr] = int(data) & 0xff


# ========================================================================
# パーシャル編集の状態管理 (C版 g_partial 相当)
# ========================================================================
class _PartialState:
    def __init__(self):
        self.active = False       # パーシャルモード有効フラグ
        self.offset = 0           # ファイル内の開始オフセット
        self.length = 0           # 読み込んだバイト数
        self.init_offset = 0      # 起動時コマンドラインで指定したオフセット
        self.init_length = 0      # 起動時コマンドラインで指定した長さ（0=EOFまで）

g_partial = _PartialState()


class Terminal:
    """ターミナル制御を担当するクラス"""
    ESC = '['
    
    def __init__(self, termcol='', get_scripting=None):
        self.termcol = termcol
        self.coltab = [0, 1, 4, 5, 2, 6, 3, 7]
        # () -> bool を返すコールバック。True のときエスケープシーケンスを抑制する
        self.get_scripting = get_scripting
    
    def _scripting(self):
        return self.get_scripting is not None and self.get_scripting()
    
    def nocursor(self):
        if self._scripting(): return
        print(f"{self.ESC}?25l", end='', flush=True)
    
    def dispcursor(self):
        if self._scripting(): return
        print(f"{self.ESC}?25h", end='', flush=True)
    
    def up(self, n=1):
        if self._scripting(): return
        print(f"{self.ESC}{n}A", end='')
    
    def down(self, n=1):
        if self._scripting(): return
        print(f"{self.ESC}{n}B", end='')
    
    def right(self, n=1):
        if self._scripting(): return
        print(f"{self.ESC}{n}C", end='')
    
    def left(self, n=1):
        if self._scripting(): return
        print(f"{self.ESC}{n}D", end='', flush=True)
    
    def locate(self, x=0, y=0):
        if self._scripting(): return
        print(f"{self.ESC}{y+1};{x+1}H", end='', flush=True)
    
    def scrollup(self, n=1):
        if self._scripting(): return
        print(f"{self.ESC}{n}S", end='')
    
    def scrolldown(self, n=1):
        if self._scripting(): return
        print(f"{self.ESC}{n}T", end='')
    
    def clear(self):
        if self._scripting(): return
        if self.termcol == 'color':
            # coltab フルカラーモード
            print("\033[40m", end='')
        elif self.termcol == 'black':
            # 黒地に白: fg=白(37), bg=黒(40) 固定
            print("\033[40m", end='')
        elif self.termcol == 'white':
            # 白地に黒: fg=黒(30), bg=白(47) 固定
            print(f"\033[47m", end='')

        print(f"{self.ESC}2J", end='', flush=True)
        self.locate()
    
    def clraftcur(self):
        if self._scripting(): return
        print(f"{self.ESC}0J", end='', flush=True)
    
    def clrline(self):
        if self._scripting(): return
        print(f"{self.ESC}2K", end='', flush=True)
    
    def color(self, col1=7, col2=0):
        if self._scripting(): return
        if self.termcol == 'color':
            # coltab フルカラーモード（UI要素ごとに色が変わる・従来の挙動）
            print(f"{self.ESC}3{self.coltab[col1]}m{self.ESC}4{self.coltab[col2]}m", end='', flush=True)
        elif self.termcol == 'black':
            # 黒地に白: fg=白(37), bg=黒(40) 固定
            print(f"{self.ESC}37m{self.ESC}40m", end='', flush=True)
        elif self.termcol == 'white':
            # 白地に黒: fg=黒(30), bg=白(47) 固定
            print(f"{self.ESC}30m{self.ESC}47m", end='', flush=True)
        # else: 指定なし → カラーエスケープを出力しない（端末本来の色を維持）
    
    def resetcolor(self):
        if self._scripting(): return
        print(f"{self.ESC}0m", end='')

    def highlight_color(self):
        """検索ヒット箇所のハイライト色 (緑地に明るいシアン・太字)"""
        if self._scripting(): return
        print(f"\x1b[1;96;44m", end='', flush=True)
    
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
        self._diff_log = None   # None=非記録中, list=記録中

    # ------------------------------------------------------------------
    # 差分記録 API
    # ------------------------------------------------------------------
    def begin_diff(self):
        """差分記録を開始する"""
        self._diff_log = []

    def end_diff(self):
        """差分記録を終了し、記録済み差分リストを返す"""
        log = self._diff_log
        self._diff_log = None
        return log if log else []

    def cancel_diff(self):
        """差分記録を破棄して終了する"""
        self._diff_log = None
    
    def __len__(self):
        return len(self.mem)
    
    def readmem(self, addr):
        if addr >= len(self.mem):
            return 0
        return self.mem[addr] & 0xff
    
    def setmem(self, addr, data):
        orig_len = len(self.mem)
        if addr >= len(self.mem):
            for i in range(addr - len(self.mem) + 1):
                self.mem.append(0)
        old_val = self.mem[addr]
        new_val = int(data) & 0xff
        if self._diff_log is not None:
            # ('ovw', addr, old_byte, new_byte, orig_mem_len)
            self._diff_log.append(('ovw', addr, old_val, new_val, orig_len))
        self.mem[addr] = new_val
        self.modified = True
        self.lastchange = True
    
    def insmem(self, start, mem2):
        if self._diff_log is not None:
            self._diff_log.append(('ins', start, list(mem2)))
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
        if length <= 0 or start >= len(self.mem) or end>len(self.mem)-1:
            return False
        
        if self._diff_log is not None:
            self._diff_log.append(('del', start, list(self.mem[start:end+1])))
        
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
        
        if self._diff_log is not None:
            orig_len = len(self.mem)
            # 変更前の該当領域を保存（拡張予定分は 0 で補完）
            old_region = list(self.mem[start:start+len(mem0)])
            while len(old_region) < len(mem0):
                old_region.append(0)
            self._diff_log.append(('ovw_region', start, old_region, list(mem0), orig_len))
        
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
    
    def __init__(self, memory_buffer, display, get_flags=None):
        self.memory = memory_buffer
        self.display = display
        self.get_flags = get_flags  # () -> (scripting, verbose)
        self.smem = []
        self.regexp = False
        self.remem = ''
        self.span = 0
        self.nff = True

    def stdmm(self, s):
        if self.get_flags is not None:
            scripting, verbose = self.get_flags()
        else:
            scripting, verbose = False, False
        self.display.stdmm(s, scripting, verbose)

    def stdmm_wait(self, s):
        """スクリプティング中（-v含む）は常に抑制するメッセージ用"""
        if self.get_flags is not None and self.get_flags()[0]:
            return
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
        
        # 複数のエンコーディングを試行（バイナリセーフ対応）
        encodings = ['utf-8', 'latin-1', 'cp1252', 'shift-jis', 'euc-jp']
        
        for encoding in encodings:
            try:
                # まず文字列としてマッチを試みる
                ms = byte_data.decode(encoding, errors='strict')
                
                try:
                    f = re.match(self.remem, ms)
                except re.error:
                    # 正規表現パターンエラー
                    return -1
                
                if f:
                    start, end = f.span()
                    matched_str = ms[start:end]
                    
                    # マッチした文字列を同じエンコーディングでバイト列に戻す
                    try:
                        matched_bytes = matched_str.encode(encoding)
                        self.span = len(matched_bytes)
                        return 1
                    except (UnicodeEncodeError, UnicodeDecodeError):
                        # このエンコーディングでは正確に変換できない
                        continue
                
            except (UnicodeDecodeError, LookupError):
                # このエンコーディングでデコードできない場合は次を試す
                continue
        
        # すべてのエンコーディングで失敗した場合、バイト列として直接マッチを試みる
        try:
            # パターンをバイト列として扱う（latin-1は1バイト=1文字なので安全）
            pattern_bytes = self.remem.encode('latin-1')
            f = re.match(pattern_bytes, byte_data)
            
            if f:
                start, end = f.span()
                self.span = end - start
                return 1
        except (re.error, UnicodeEncodeError, UnicodeDecodeError):
            pass
        
        return 0
    
    def searchnext(self, fp, mem_len):
        if mem_len == 0:
            self.clrmm()
            return None
        curpos = fp
        start = fp
        wrapped = False
        if not self.regexp and not self.smem:
            return False
        self.stdmm_wait("Wait.")
        while True:
            f = self.hitre(curpos) if self.regexp else self.hit(curpos)
            
            if f == 1:
                if not wrapped:
                    self.clrmm()
                return curpos
            elif f < 0:
                if not wrapped:
                    self.clrmm()
                return None
            
            curpos += 1
            
            if curpos >= mem_len:
                if self.nff:
                    if not wrapped:
                        self.stdmm_wait("Search reached BOTTOM, wrap around to TOP.")
                        wrapped = True
                    curpos = 0
                else:
                    self.clrmm()
                    return None
            
            if curpos == start:
                self.clrmm()
                return None
    
    def searchlast(self, fp, mem_len):
        if mem_len == 0:
            self.clrmm()
            return None
        wrapped = False
        if fp < 0:
            fp = mem_len - 1
        curpos = fp
        start = fp
        if not self.regexp and not self.smem:
            return False
        if not wrapped:
            self.stdmm_wait("Wait.")
        while True:
            f = self.hitre(curpos) if self.regexp else self.hit(curpos)
            
            if f == 1:
                if not wrapped:
                    self.clrmm()
                return curpos
            elif f < 0:
                if not wrapped:
                    self.clrmm()
                return None
            
            curpos -= 1
            if curpos < 0:
                self.stdmm_wait("Search reached TOP, wrap around to BOTTOM.")
                wrapped = True
                curpos = mem_len - 1
            
            if curpos == start:
                self.clrmm()
                return None

    def search_all(self, mem_len, max_results=10000):
        """全てのマッチ箇所を検索して返す"""
        matches = []
        if mem_len == 0:
            return matches
        if not self.regexp and not self.smem:
            return matches
        
        self.stdmm_wait("Searching all matches...")
        curpos = 0
        
        while curpos < mem_len and len(matches) < max_results:
            f = self.hitre(curpos) if self.regexp else self.hit(curpos)
            
            if f == 1:
                # マッチした位置と長さを保存
                match_len = self.span if self.regexp else len(self.smem)
                matches.append((curpos, match_len))
                # 次の検索位置は現在のマッチの後から
                curpos += max(1, match_len)
            elif f < 0:
                break
            else:
                curpos += 1
        
        self.clrmm()
        return matches


class Display:
    """画面表示クラス"""
    # クラス定数はフォールバック用の最小値として残す
    _MIN_DATA_ROWS = 3   # データ行の最小数
    _HEADER_ROWS   = 3   # タイトル2行 + ヘッダー1行
    _FOOTER_ROWS   = 2   # メッセージ行 + カーソル情報行

    def __init__(self, terminal, memory_buffer):
        self.term = terminal
        self.memory = memory_buffer
        self.homeaddr = 0
        self.curx = 0
        self.cury = 0
        self.utf8 = False
        self.insmod = False
        # 複数のハイライト範囲をリストで管理 [(pos, len), ...]
        self.highlight_ranges = []
        # 画面サイズに応じた行数を初期化
        self.update_screen_size()

    def update_screen_size(self):
        """端末サイズを取得して BOTTOMLN / LENONSCR を再計算する。
        取得できない場合はデフォルト値 (BOTTOMLN=22) を使用する。"""
        try:
            rows = os.get_terminal_size().lines
        except OSError:
            rows = 24          # フォールバック
        # BOTTOMLN = 最終データ行の次 (メッセージ行)
        # レイアウト: 0,1=タイトル  2=ヘッダー  3..BOTTOMLN-1=データ  BOTTOMLN=メッセージ  BOTTOMLN+1=情報
        data_rows = max(self._MIN_DATA_ROWS, rows - self._HEADER_ROWS - self._FOOTER_ROWS)
        self.BOTTOMLN  = self._HEADER_ROWS + data_rows   # == data_rows + 3
        self.LENONSCR  = data_rows * 16
    
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
    
    def is_highlighted(self, addr):
        """指定アドレスがハイライト範囲に含まれるか判定"""
        for pos, length in self.highlight_ranges:
            if pos <= addr < pos + length:
                return True
        return False
    
    def printchar(self, a):
        if a >= len(self.memory.mem):
            print("~", end='', flush=True)
            return 1
        
        if self.utf8:
            if self.memory.mem[a] < 0x80 or 0x80 <= self.memory.mem[a] <= 0xbf or 0xf8 <= self.memory.mem[a] <= 0xff:
                print(chr(self.memory.mem[a] & 0xff) if 0x20 <= self.memory.mem[a] <= 0x7e else '.', end='')
                return 1
            elif 0xc0 <= self.memory.mem[a] <= 0xdf:
                m = [self.memory.readmem(a), self.memory.readmem(a + 1)]
                try:
                    ch = bytes(m).decode('utf-8')
                    print(f"{ch}", end='', flush=True)
                    return 2
                except:
                    print(".", end='')
                    return 1
            elif 0xe0 <= self.memory.mem[a] <= 0xef:
                m = [self.memory.readmem(a), self.memory.readmem(a + 1), self.memory.readmem(a + 2)]
                try:
                    ch = bytes(m).decode('utf-8')
                    print(f"{ch} ", end='', flush=True)
                    return 3
                except:
                    print(".", end='')
                    return 1
            elif 0xf0 <= self.memory.mem[a] <= 0xf7:
                m = [self.memory.readmem(a), self.memory.readmem(a + 1), 
                     self.memory.readmem(a + 2), self.memory.readmem(a + 3 )]
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
        print(f'bi Py version 3.5.1 by Taisuke Maekawa          utf8mode:{"off" if not self.utf8 else "on "}     {"insert   " if self.insmod else "overwrite"}   ')
        self.term.color(5)
        if len(filename) > 35:
            fn = filename[0:35]
        else:
            fn = filename
        print(f'file:[{fn:<35}] length:{len(self.memory.mem)} bytes [{("not " if not self.memory.modified else "")+"modified"}]    ')
    
    def repaint(self, filename):
        self.update_screen_size()   # リサイズに追従
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
            print(f"{(addr + y * 16 + g_partial.offset) & 0xffffffffffff:012X} ", end='')
            self.term.color(7)
            for i in range(16):
                a = y * 16 + i + addr
                in_hl = (self.highlight_ranges and self.is_highlighted(a))
                if in_hl:
                    self.term.highlight_color()
                    print(f"~~" if a >= len(self.memory.mem) else f"{self.memory.mem[a] & 0xff:02X}", end='')
                    self.term.resetcolor()
                    self.term.color(7)
                    print(" ", end='')
                else:
                    self.term.color(7)
                    print(f"~~ " if a >= len(self.memory.mem) else f"{self.memory.mem[a] & 0xff:02X} ", end='')
            self.term.color(7)
            self.term.color(6)
            a = y * 16 + addr
            by = 0
            while by < 16:
                in_hl = (self.highlight_ranges and self.is_highlighted(a))
                if in_hl:
                    self.term.highlight_color()
                    c = self.printchar(a)
                    self.term.resetcolor()
                    self.term.color(6)
                else:
                    self.term.color(6)
                    c = self.printchar(a)
                a += c
                by += c
            print("  ", end='', flush=True)
        self.term.color(0)
        self.term.dispcursor()
    
    def printdata(self):
        addr = self.fpos()
        file_addr = addr + g_partial.offset  # 実ファイル上のアドレス
        a = self.memory.readmem(addr)
        # カーソル位置のバイト詳細（実ファイルアドレスで表示）
        self.term.locate(0, self.BOTTOMLN+1)
        self.term.clrline()          # \n なしで行をクリア（末尾改行によるスクロール防止）
        self.term.locate(0, self.BOTTOMLN+1)
        self.term.color(6)
        s = '.'
        if a < 0x20:
            s = '^' + chr(a + ord('@'))
        elif a >= 0x7e:
            s = '.'
        else:
            s = "'" + chr(a) + "'"
        if addr < len(self.memory.mem):
            print(f"{file_addr:012X} : 0x{a:02X} 0b{a:08b} 0o{a:03o} {a} {s}      ", end='', flush=True)
        else:
            print(f"{file_addr:012X} : ~~                                                   ", end='', flush=True)

        # 行23(BOTTOMLN): PARTIAL ステータス常時表示
        self.term.locate(0, self.BOTTOMLN+1)
        if g_partial.active:
            self.term.color(6)
            print(
                f" PARTIAL  file_offset:0x{g_partial.offset:012X}"
                f"  length:0x{g_partial.length:X}({g_partial.length}) bytes   ",
                end='', flush=True
            )
    
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
        if idx >= len(s):
            return self.UNKNOWN, idx
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
    def readfile_partial(self, fn, offset, max_len=0):
        """パーシャルリード: fn の offset から max_len バイト(0=EOF まで)を読む"""
        global g_partial
        try:
            f = open(fn, "rb")
        except OSError:
            self.newfile = True
            self.memory.mem = []
            g_partial.active = True
            g_partial.offset = offset
            g_partial.length = 0
            return True, "<new file>"
        try:
            fsize = f.seek(0, 2)
        except OSError:
            f.close()
            return False, f"Partial read error: cannot seek '{fn}'."
        if fsize <= offset:
            f.close()
            return False, f"Partial read error: offset 0x{offset:X} exceeds file size (0x{fsize:X})."
        available = fsize - offset
        read_len = available if (max_len == 0 or max_len > available) else max_len
        try:
            f.seek(offset)
            data = f.read(read_len)
            f.close()
        except OSError:
            try: f.close()
            except: pass
            return False, f"Partial read error: I/O error reading '{fn}'."
        actually_read = len(data)
        self.memory.mem = list(data)
        g_partial.active = True
        g_partial.offset = offset
        g_partial.length = actually_read
        self.newfile = False
        self.memory.modified = False
        self.memory.lastchange = False
        if actually_read != read_len:
            return True, f"Partial read warning: requested {read_len} bytes but only {actually_read} bytes read."
        return True, f"Partial load: offset=0x{offset:X}, {actually_read} bytes read."

    def writefile_partial(self, fn):
        """パーシャルライト: g_partial.offset から上書き（テール保護あり）

        ファイル構造:
            [ヘッダ: 0 .. offset-1]
            [パーシャル領域(旧): offset .. offset+g_partial.length-1]
            [テール: offset+g_partial.length .. EOF]

        旧実装の問題:
            offset に新データを書くだけでテールを保護しなかった。
            ・新データが旧領域より短い場合 → テール前に旧データが残留して破損
            ・新データが旧領域より長い場合 → テールを上書きして破損

        修正方針:
            ① テール (offset+g_partial.length 以降) を読み退ける
            ② offset に新データを書く
            ③ テールをその直後に書く
            ④ truncate して余剰バイトを除去
        """
        global g_partial
        if not g_partial.active:
            return self.writefile(fn)
        self.memory.regulate_mem()
        try:
            f = open(fn, "r+b")
        except OSError:
            # ファイルが存在しない場合は新規作成
            try:
                f = open(fn, "wb")
                if g_partial.offset > 0:
                    f.write(b'\x00' * g_partial.offset)
                f.write(bytes(self.memory.mem))
                f.close()
                return True, f"Partial write: offset=0x{g_partial.offset:X}, {len(self.memory.mem)} bytes written (new file)."
            except OSError:
                return False, f"Partial write error: cannot create '{fn}'."
        try:
            # ① テールを読み退ける
            tail_start = g_partial.offset + g_partial.length
            f.seek(0, 2)                  # ファイル末尾
            file_size = f.tell()
            tail = b''
            if tail_start < file_size:
                f.seek(tail_start)
                tail = f.read()

            # ② 新データを書く
            f.seek(g_partial.offset)
            written = f.write(bytes(self.memory.mem))

            # ③ テールを書き戻す
            if tail:
                f.write(tail)

            # ④ 余剰バイトを除去
            f.truncate()
            f.close()
        except OSError:
            try: f.close()
            except: pass
            return False, f"Partial write error: I/O error while writing '{fn}'."
        if written != len(self.memory.mem):
            return False, f"Partial write error: wrote {written}/{len(self.memory.mem)} bytes to '{fn}'."
        return True, f"Partial write: offset=0x{g_partial.offset:X}, {written} bytes written."


class BiEditor:
    """バイナリエディタのメインクラス"""
    def __init__(self, termcol=''):
        self.scriptingflag = False
        self.verbose = False
        self.term = Terminal(termcol, get_scripting=lambda: self.scriptingflag)
        self.memory = MemoryBuffer()
        self.display = Display(self.term, self.memory)
        self.parser = Parser(self.memory, self.display)
        self.history = HistoryManager()
        self.search = SearchEngine(self.memory, self.display,
                                   get_flags=lambda: (self.scriptingflag, self.verbose))
        self.filemgr = FileManager(self.memory)
        
        self.stack = []
        self.cp = 0
        self.endian = 'little'  # エンディアン ('little' or 'big')
        
        # Undo/Redo機能用（差分方式）
        self.undo_stack = []  # 各エントリ: {'diff': [...], 'mark_before': [...], ...}
        self.redo_stack = []
        self.max_undo_levels = 100  # 最大undo回数
        self._undo_mark_snapshot = None    # begin_undo() 時点の mark スナップショット
        self._undo_meta_snapshot = None    # begin_undo() 時点の modified/lastchange
        self._undo_cursor_snapshot = None  # begin_undo() 時点のカーソル位置
    
    def stdmm(self, s):
        self.display.stdmm(s, self.scriptingflag, self.verbose)

    def stdmm_wait(self, s):
        """スクリプティング中（-v含む）は常に抑制するメッセージ用"""
        if self.scriptingflag:
            return
        self.display.stdmm(s, False, False)

    # ------------------------------------------------------------------
    # 差分 undo/redo ヘルパー
    # ------------------------------------------------------------------
    def _apply_diff_inverse(self, diff_log):
        """差分リストを逆順に逆適用する（undo 用）"""
        for entry in reversed(diff_log):
            op = entry[0]
            if op == 'ovw':
                # ('ovw', addr, old_byte, new_byte, orig_mem_len)
                _, addr, old_byte, new_byte, orig_len = entry
                # orig_len より短くなっていた場合も考慮して復元
                while len(self.memory.mem) <= addr:
                    self.memory.mem.append(0)
                self.memory.mem[addr] = old_byte
                # mem が拡張されていたなら縮める
                if orig_len < len(self.memory.mem):
                    del self.memory.mem[orig_len:]
            elif op == 'ovw_region':
                # ('ovw_region', start, old_region, new_region, orig_len)
                _, start, old_region, new_region, orig_len = entry
                for i, v in enumerate(old_region):
                    if start + i < len(self.memory.mem):
                        self.memory.mem[start + i] = v
                if orig_len < len(self.memory.mem):
                    del self.memory.mem[orig_len:]
            elif op == 'ins':
                # ('ins', start, data) → undo は削除
                _, start, data = entry
                del self.memory.mem[start:start + len(data)]
            elif op == 'del':
                # ('del', start, data) → undo は挿入
                _, start, data = entry
                self.memory.mem[start:start] = data

    def _apply_diff_forward(self, diff_log):
        """差分リストを順方向に適用する（redo 用）"""
        for entry in diff_log:
            op = entry[0]
            if op == 'ovw':
                _, addr, old_byte, new_byte, orig_len = entry
                while len(self.memory.mem) <= addr:
                    self.memory.mem.append(0)
                self.memory.mem[addr] = new_byte
            elif op == 'ovw_region':
                _, start, old_region, new_region, orig_len = entry
                # new_region に合わせて拡張
                while len(self.memory.mem) < start + len(new_region):
                    self.memory.mem.append(0)
                for i, v in enumerate(new_region):
                    self.memory.mem[start + i] = v
            elif op == 'ins':
                _, start, data = entry
                self.memory.mem[start:start] = data
            elif op == 'del':
                _, start, data = entry
                del self.memory.mem[start:start + len(data)]

    def save_undo_state(self):
        """操作前に呼び出す: 差分記録を開始し mark/meta/カーソル位置をスナップショット"""
        if self.scriptingflag:
            return
        # 前回の記録が完了していない場合は先に確定させる
        if self.memory._diff_log is not None:
            self.commit_undo()
        self._undo_mark_snapshot = list(self.memory.mark)
        self._undo_meta_snapshot = (self.memory.modified, self.memory.lastchange)
        self._undo_cursor_snapshot = self.display.fpos()  # 操作前のカーソル位置を保存
        self.memory.begin_diff()

    def commit_undo(self):
        """操作後に呼び出す: 記録した差分をスタックに積む"""
        if self.scriptingflag or self._undo_mark_snapshot is None:
            self.memory.cancel_diff()
            return
        diff_log = self.memory.end_diff()
        if not diff_log:
            self._undo_mark_snapshot = None
            self._undo_meta_snapshot = None
            self._undo_cursor_snapshot = None
            return
        state = {
            'diff': diff_log,
            'mark_before': self._undo_mark_snapshot,
            'mark_after': list(self.memory.mark),
            'modified_before': self._undo_meta_snapshot[0],
            'lastchange_before': self._undo_meta_snapshot[1],
            'cursor_before': self._undo_cursor_snapshot,  # 操作前のカーソル位置
            'cursor_after': self.display.fpos(),           # 操作後のカーソル位置
        }
        self._undo_mark_snapshot = None
        self._undo_meta_snapshot = None
        self._undo_cursor_snapshot = None
        self.undo_stack.append(state)
        if len(self.undo_stack) > self.max_undo_levels:
            self.undo_stack.pop(0)
        self.redo_stack = []

    def dec_undo(self):
        """操作が失敗したとき: 今回の差分記録を破棄する"""
        self.memory.cancel_diff()
        self._undo_mark_snapshot = None
        self._undo_meta_snapshot = None

    def undo(self):
        """差分を逆適用して undo を実行"""
        if not self.undo_stack:
            self.stdmm("No more undo.")
            return False

        state = self.undo_stack.pop()

        # undo を押した瞬間の現在位置を cursor_after に上書き保存する。
        # こうすることで、次に redo したとき「undo を押す直前の場所」に戻れる。
        state['cursor_after'] = self.display.fpos()

        # 同じ state を redo スタックに積む（forward diff を保持）
        self.redo_stack.append(state)

        # 差分を逆適用
        self._apply_diff_inverse(state['diff'])
        self.memory.mark = list(state['mark_before'])
        self.memory.modified = state['modified_before']
        self.memory.lastchange = state['lastchange_before']

        # カーソル位置は変えず、範囲外の場合のみクランプする
        mem_len = len(self.memory.mem)
        cur = self.display.fpos()
        if mem_len == 0:
            self.display.jump(0)
        elif cur >= mem_len:
            self.display.jump(mem_len - 1)

        self.stdmm(f"Undo. ({len(self.undo_stack)} more)")
        return True

    def redo(self):
        """差分を順適用して redo を実行"""
        if not self.redo_stack:
            self.stdmm("No more redo.")
            return False

        state = self.redo_stack.pop()

        # redo を押した瞬間の現在位置を cursor_before に上書き保存する。
        # こうすることで、次に undo したとき「redo を押す直前の場所」に戻れる。
        state['cursor_before'] = self.display.fpos()

        # 同じ state を undo スタックに戻す
        self.undo_stack.append(state)

        # 差分を順適用
        self._apply_diff_forward(state['diff'])
        self.memory.mark = list(state['mark_after'])
        self.memory.modified = True
        self.memory.lastchange = True

        # カーソル位置は変えず、範囲外の場合のみクランプする
        mem_len = len(self.memory.mem)
        cur = self.display.fpos()
        if mem_len == 0:
            self.display.jump(0)
        elif cur >= mem_len:
            self.display.jump(mem_len - 1)

        self.stdmm(f"Redo. ({len(self.redo_stack)} more)")
        return True
    
    def stderr(self, s):
        self.display.stderr(s, self.scriptingflag, self.verbose)
    
    def disp_marks(self):
        j = 0
        self.term.locate(0, self.display.BOTTOMLN)
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
            self.term.locate(0, self.display.BOTTOMLN)
            print(msg, end='', flush=True)
            Terminal.getch()
            self.term.locate(0, self.display.BOTTOMLN + 1)
            print(" " * 80, end='', flush=True)
    
    def call_exec(self, line):
        if len(line) <= 1:
            self.stderr("No python code specified.")
            return
        line = line[1:]

        # exec() 実行前にグローバルを最新状態に合わせる
        global mem, cp
        mem = self.memory.mem
        cp  = self.cp

        try:
            if self.scriptingflag:
                exec(line, globals())
            else:
                self.display.clrmm()
                self.term.color(7)
                self.term.locate(0, self.display.BOTTOMLN)
                exec(line, globals())
                self.term.color(4)
                self.term.clrline()
                print("[ Hit a key ]", end='', flush=True)
                Terminal.getch()
                self.term.clear()
                self.display.repaint(self.filemgr.filename)
        except:
            self.stderr("python exec() error.")
            return

        # exec() がグローバルの mem を差し替えた場合も含めて書き戻す
        self.memory.mem = mem
        self.memory.modified   = True
        self.memory.lastchange = True

    def fedit(self):
        """フルスクリーンエディタモード"""
        stroke = False
        ch = ''
        
        while True:
            self.cp = self.display.fpos()
            # グローバル変数を最新状態に同期 (@exec / {}eval から参照可能)
            global mem, cp
            mem = self.memory.mem
            cp  = self.cp
            self.display.repaint(self.filemgr.filename)
            self.display.printdata()
            self.term.locate(self.display.curx // 2 * 3 + 13 + (self.display.curx & 1), self.display.cury + 3)
            ch = Terminal.getch()
            self.display.clrmm()
            self.search.nff = True
            
            # エスケープシーケンス処理
            if ch == chr(27):
                c2 = Terminal.getch()
                if c2 == chr(91):  # '[' - エスケープシーケンス
                    c3 = Terminal.getch()
                    if c3 == 'A':
                        ch = 'k'
                    elif c3 == 'B':
                        ch = 'j'
                    elif c3 == 'C':
                        ch = 'l'
                    elif c3 == 'D':
                        ch = 'h'
                    elif c3 == chr(50):
                        ch = 'i'
                else:
                    # ESC単独 - ハイライトをクリア
                    self.display.highlight_ranges = []
                    continue
            
            # 検索コマンド
            if ch == 'n':
                pos = self.search.searchnext(self.display.fpos() + 1, len(self.memory))
                if pos is not None and pos is not False:
                    # ハイライト範囲が空の場合、全マッチを再検索してハイライト
                    if not self.display.highlight_ranges:
                        matches = self.search.search_all(len(self.memory))
                        if matches:
                            self.display.highlight_ranges = matches
                    self.display.jump(pos)
                elif pos is None:
                    self.stdmm("Not found.")
                continue
            elif ch == 'N':
                pos = self.search.searchlast(self.display.fpos() - 1, len(self.memory))
                if pos is not None and pos is not False:
                    # ハイライト範囲が空の場合、全マッチを再検索してハイライト
                    if not self.display.highlight_ranges:
                        matches = self.search.search_all(len(self.memory))
                        if matches:
                            self.display.highlight_ranges = matches
                    self.display.jump(pos)
                elif pos is None:
                    self.stdmm("Not found.")
                continue
            
            # Undo/Redo
            elif ch == 'u':
                self.undo()
                continue
            elif ch == chr(18) or ch=='U':  # Ctrl+R
                self.redo()
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
                self.dec_undo()
                if self.display.cury < self.display.LENONSCR // 16 - 1:
                    self.display.cury += 1
                else:
                    self.display.scrdown()
                continue
            elif ch == 'k':
                self.dec_undo()
                if self.display.cury > 0:
                    self.display.cury -= 1
                else:
                    self.display.scrup()
                continue
            elif ch == 'h':
                self.dec_undo()
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
                self.dec_undo()
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
                self.display.repaint(self.filemgr.filename)
                continue
            
            # ファイル操作 (Z: :wq! 相当、パーシャル対応、失敗でも終了)
            elif ch == 'Z':
                if g_partial.active:
                    success, msg = self.filemgr.writefile_partial(self.filemgr.filename)
                else:
                    success, msg = self.filemgr.writefile(self.filemgr.filename)
                self.memory.lastchange = False
                if not success:
                    self.stderr(msg)
                return True
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
                if y:
                    self.save_undo_state()
                    self.memory.ovwmem(self.display.fpos(), y)
                    self.commit_undo()
                    self.stdmm(f"{len(y)} bytes pasted.")
                    self.display.jump(self.display.fpos() + len(y))
                continue
            elif ch == 'P':
                y = list(self.memory.yank)
                if y:  # yankバッファが空でない場合のみハイライトをクリア
                    self.save_undo_state()
                    self.display.highlight_ranges = []
                    self.memory.insmem(self.display.fpos(), y)
                    self.commit_undo()
                    self.stdmm(f"{len(y)} bytes pasted (insert).")
                    self.display.jump(self.display.fpos() + len(y))
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
                        # 実際に挿入が行われるのでundo保存とハイライトをクリア
                        self.save_undo_state()
                        self.display.highlight_ranges = []
                        self.memory.insmem(addr, [c << sh])
                    else:
                        if not stroke:
                            self.save_undo_state()
                        self.memory.setmem(addr, self.memory.readmem(addr) & mask | c << sh)
                    stroke = (not stroke) if not self.display.curx & 1 else False
                    if not stroke:
                        self.commit_undo()  # 1バイト分の入力完了でコミット
                else:
                    if (self.display.curx & 1) == 0:  # 上位ニブルの入力時のみ記録開始
                        self.save_undo_state()
                    self.memory.setmem(addr, self.memory.readmem(addr) & mask | c << sh)
                    if (self.display.curx & 1) == 1:  # 下位ニブル完了でコミット
                        self.commit_undo()
                self.display.inccurx()
            elif ch == 'x':
                # 削除が成功した場合のみundo保存とハイライトをクリア
                self.save_undo_state()
                if self.memory.delmem(self.display.fpos(), self.display.fpos(), False, self.memory.yankmem):
                    self.commit_undo()
                    self.display.highlight_ranges = []
                else:
                    self.stderr("Invalid range.")
                    self.dec_undo()
            elif ch == ':':
                self.display.disp_curpos()
                # コマンド実行前のファイル長を保存
                before_len = len(self.memory.mem)
                f = self.commandln()
                # コマンド実行後にファイル長が変わった場合のみハイライトをクリア
                if len(self.memory.mem) != before_len:
                    self.display.highlight_ranges = []
                self.display.erase_curpos()
                if f == 1:
                    return True
                elif f == 0:
                    return False
    
    def do_search(self):
        """検索実行"""
        self.display.disp_curpos()
        self.term.locate(0, self.display.BOTTOMLN)
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
        """文字列検索 - 全てのマッチをハイライト"""
        if s != "":
            self.search.regexp = True
            self.search.remem = s
            
            # 全てのマッチ箇所を検索
            matches = self.search.search_all(len(self.memory))
            
            if matches:
                # ハイライト範囲を設定
                self.display.highlight_ranges = matches
                # 最初のマッチ位置にジャンプ
                first_pos = matches[0][0]
                self.display.jump(first_pos)
                self.stdmm(f"Found {len(matches)} match(es)")
                return True
            else:
                self.display.highlight_ranges = []
                self.stdmm("Not found")
        return False
    
    def searchhex(self, sm):
        """16進検索 - 全てのマッチをハイライト"""
        self.search.remem = ''
        self.search.regexp = False
        if sm:
            self.search.smem = sm
            
            # 全てのマッチ箇所を検索
            matches = self.search.search_all(len(self.memory))
            
            if matches:
                # ハイライト範囲を設定
                self.display.highlight_ranges = matches
                # 最初のマッチ位置にジャンプ
                first_pos = matches[0][0]
                self.display.jump(first_pos)
                self.stdmm(f"Found {len(matches)} match(es)")
                return True
            else:
                self.display.highlight_ranges = []
                self.stdmm("Not found")
        return False
    
    def commandln(self):
        """コマンドライン入力"""
        self.term.locate(0, self.display.BOTTOMLN)
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
        # グローバル変数を最新状態に同期 (@exec / {}eval から参照可能)
        global mem, cp
        mem = self.memory.mem
        cp  = self.cp
        line = self.parser.comment(line)
        if line == '':
            return -1
        
        # エンディアン切り替え
        if line[0] == '_':
            if line == '_big':
                self.endian = 'big'
                self.stdmm("Switched to big endian.")
            elif line == '_little':
                self.endian = 'little'
                self.stdmm("Switched to little endian.")
            else:
                self.stderr("Unknown command. Use '_big' or '_little'.")
            return -1

        # 型付き数値表示 (?s/?i/?l/?q/?f/?d/?Q/?us/?ui/?ul) — 範囲なし版はここから parse_range_command へ
        if line[0] == '?' and len(line) >= 2 and (
                line[1] in 'silqfdQ' or line[1:] in ('us', 'ui', 'ul')):
            return self.parse_range_command(line)

        # 終了コマンド
        if line == 'q':
            if self.memory.lastchange:
                self.stderr("No write since last change. To overriding quit, use 'q!'.")
                return -1
            return 0
        elif line == 'q!':
            return 0
        elif line == 'wq' or line == 'wq!':
            if g_partial.active:
                success, msg = self.filemgr.writefile_partial(self.filemgr.filename)
            else:
                success, msg = self.filemgr.writefile(self.filemgr.filename)
            if success:
                self.memory.lastchange = False
                self.stdmm("File written and quit.")
                return 0
            else:
                self.stderr(msg)
                return -1
        
        # Undo/Redo
        elif line == 'u' or line == 'undo':
            self.undo()
            return -1
        elif line == 'U' or line == 'redo':
            self.redo()
            return -1

        # ファイル書き込み
        elif line[0] == 'w':
            # :wp [file] — 明示的パーシャルライト
            if len(line) >= 2 and line[1] == 'p':
                fname = line[2:].lstrip() or self.filemgr.filename
                success, msg = self.filemgr.writefile_partial(fname)
                if success:
                    self.memory.lastchange = False
                    self.stdmm(msg)
                else:
                    self.stderr(msg)
                return -1
            # :w / :w filename
            fname_specified = len(line) >= 2 and line[1:].lstrip() != ''
            if fname_specified:
                success, msg = self.filemgr.writefile(line[1:].lstrip())
            elif g_partial.active:
                success, msg = self.filemgr.writefile_partial(self.filemgr.filename)
                if success:
                    self.memory.lastchange = False
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
        elif line[0] == 'r' and len(line)>=2 and line[1]=='p':
            # :rp — 起動時コマンドラインで指定した範囲を再ロード
            success, msg = self.filemgr.readfile_partial(
                self.filemgr.filename,
                g_partial.init_offset,
                g_partial.init_length)
            if success:
                self.display.jump(0)
                self.display.highlight_ranges = []
                self.stdmm(msg)
            else:
                self.stderr(msg)
            return -1
        elif line[0] == 'r' and len(line)<2:
            if g_partial.active:
                success, msg = self.filemgr.readfile_partial(
                    self.filemgr.filename,
                    g_partial.offset, g_partial.length)
            else:
                success, msg = self.filemgr.readfile(self.filemgr.filename)
            if success:
                self.display.jump(0)
                self.display.highlight_ranges = []
                self.stdmm(msg if msg else "Original file read.")
            else:
                self.stderr(msg)
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
            if not self.search.regexp and not self.search.smem:
                self.stderr("No data to search.")
                return -1
            pos = self.search.searchnext(self.display.fpos() + 1, len(self.memory))
            if pos is not None:
                if not self.display.highlight_ranges:
                    matches = self.search.search_all(len(self.memory))
                    if matches:
                        self.display.highlight_ranges = matches
                self.display.jump(pos)
            return -1
        elif line[0] == 'N':
            if not self.search.regexp and not self.search.smem:
                self.stderr("No data to search.")
                return -1
            pos = self.search.searchlast(self.display.fpos() - 1, len(self.memory))
            if pos is not None:
                if not self.display.highlight_ranges:
                    matches = self.search.search_all(len(self.memory))
                    if matches:
                        self.display.highlight_ranges = matches
                self.display.jump(pos)
            return -1
        
        # 特殊コマンド
        elif line[0] == '@':
            self.call_exec(line)
            self.display.jump(cp)
            return -1
        elif line[0] == '!':
            if len(line) >= 2:
                self.invoke_shell(line[1:])
            else:
                self.stderr("No shell command specified.")
            return -1
        elif line[0] == '?':
            if len(line) >= 2:
                v, _ = self.parser.expression(line[1:], 0)
                if v == Parser.UNKNOWN:
                    self.stderr("Invalid expression.")
                else:
                    self.printvalue(line[1:])
            else:
                self.stderr("Invalid expression.")
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
                xf2 = True
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

        # パーシャル編集中: ユーザー入力アドレスはファイル絶対値
        # → バッファ相対インデックス (addr - g_partial.offset) に変換
        # ただし rp コマンドは絶対アドレスをそのまま使うので変換しない
        next_cmd = line[idx:idx+2] if idx + 1 < len(line) else line[idx:idx+1]
        if g_partial.active and g_partial.offset > 0 and next_cmd != 'rp':
            if xf:
                x  = max(0, x  - g_partial.offset)
            if xf2:
                x2 = max(0, x2 - g_partial.offset)
            elif xf:           # x2 = x と連動していたケース
                x2 = x

        if idx == len(line):
            self.display.jump(x)
            return -1
        
        # 各種コマンドの処理
        return self.execute_command(line, idx, x, x2, xf, xf2)
    
    def cmd_typed_display(self, x, x2, xf, xf2, type_char):
        """型付き数値表示コマンド (?s/?i/?l/?q/?f/?d/?Q)"""
        import struct, ctypes

        size_map = {'s': 2, 'i': 4, 'l': 8, 'q': 16, 'f': 4, 'd': 8, 'Q': 16,
                    'us': 2, 'ui': 4, 'ul': 8}
        label_map = {'s': 'int16', 'i': 'int32', 'l': 'int64', 'q': 'int128',
                     'f': 'float32', 'd': 'float64', 'Q': 'float128',
                     'us': 'uint16', 'ui': 'uint32', 'ul': 'uint64'}
        size = size_map[type_char]
        be = (self.endian == 'big')
        endian_ch = '>' if be else '<'
        mem_len = len(self.memory.mem)

        start = int(x)
        end   = int(x2) if xf2 else start

        lines_out = []
        pos = start
        while pos <= end:
            if len(self.memory.mem)>pos+size:
                raw = bytes([self.memory.readmem(pos + i) for i in range(size)])
                try:
                    if type_char == 's':
                        val = struct.unpack(endian_ch + 'h', raw)[0]
                        s = str(val)
                    elif type_char == 'i':
                        val = struct.unpack(endian_ch + 'i', raw)[0]
                        s = str(val)
                    elif type_char == 'l':
                        val = struct.unpack(endian_ch + 'q', raw)[0]
                        s = str(val)
                    elif type_char == 'q':
                        val = int.from_bytes(raw, 'big' if be else 'little', signed=True)
                        s = str(val)
                    elif type_char == 'f':
                        val = struct.unpack(endian_ch + 'f', raw)[0]
                        s = repr(val)
                    elif type_char == 'd':
                        val = struct.unpack(endian_ch + 'd', raw)[0]
                        s = repr(val)
                    elif type_char == 'Q':
                        # 128-bit float: ctypes long double (platform dependent)
                        if be:
                            raw = raw[::-1]
                        buf = (ctypes.c_ubyte * 16)(*raw)
                        ld = ctypes.cast(buf, ctypes.POINTER(ctypes.c_longdouble)).contents.value
                        s = repr(ld)
                    elif type_char == 'us':
                        val = struct.unpack(endian_ch + 'H', raw)[0]
                        s = str(val)
                    elif type_char == 'ui':
                        val = struct.unpack(endian_ch + 'I', raw)[0]
                        s = str(val)
                    elif type_char == 'ul':
                        val = struct.unpack(endian_ch + 'Q', raw)[0]
                        s = str(val)
                except Exception as e:
                    s = f'(error: {e})'
            else:
                s='~~~~~~~~'
            lines_out.append(f"{pos:08X}: ({label_map[type_char]}) {s}")
            pos += size
            if pos > end and end != start:
                break

        output = '\n'.join(lines_out)

        if self.scriptingflag:
            if self.verbose:
                print(output)
        else:
            self.display.clrmm()
            self.term.color(6)
            self.term.locate(0, self.display.BOTTOMLN)
            # 複数行は1行にまとめて表示、長ければスクロール
            if len(lines_out) == 1:
                print(lines_out[0], end='', flush=True)
            else:
                self.term.locate(0,self.display.BOTTOMLN+1)
                # 複数: 画面下に表示してキー待ち
                print("", flush=True)
                for ln in lines_out:
                    print(ln)
                self.term.color(4)
                print("[ Hit any key ]", end='', flush=True)
                Terminal.getch()
                self.term.clear()
                return -1
            Terminal.getch()
            self.term.locate(0, self.display.BOTTOMLN)
            print(" " * 80, end='', flush=True)
        return -1

    def execute_command(self, line, idx, x, x2, xf, xf2):
        """個別コマンドの実行"""
        # 型付き数値表示 (?s/?i/?l/?q/?f/?d/?Q/?us/?ui/?ul)
        if idx < len(line) and line[idx] == '?':
            rest = line[idx + 1:]
            if rest in ('s', 'i', 'l', 'q', 'f', 'd', 'Q', 'us', 'ui', 'ul'):
                return self.cmd_typed_display(x, x2, xf, xf2, rest)

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
     # ========================================================================
     # [start],[end] w <file>   --- 指定範囲のエリアを別ファイルに書き出す
     # ========================================================================
        if idx < len(line) and line[idx] == 'w':
            idx += 1
            fn = line[idx:].lstrip()          # ファイル名（スペースも含む）
            if not fn:
                self.stderr("Filename required for range write (ex: 100,1ff w dump.bin)")
                return -1

            # 範囲チェック
            if not xf or not xf2 or x > x2:
                self.stderr("Invalid range.")
                return -1
            if x >= len(self.memory.mem):
                self.stderr("Range start is beyond end of buffer.")
                return -1
            if x2 >= len(self.memory.mem):
                x2 = len(self.memory.mem) - 1

            # 書き出し（FileManager.wrtfile をそのまま利用）
            success, msg = self.filemgr.wrtfile(x, x2, fn)
            if success:
                self.stdmm(f"{x2 - x + 1} bytes written to '{fn}'")
            else:
                self.stderr(msg or "Write error.")
            return -1
        
        # paste
        if idx < len(line) and line[idx] == 'p':
            y = list(self.memory.yank)
            if y:
                self.save_undo_state()
                self.memory.ovwmem(x, y)
                self.commit_undo()
                self.stdmm(f"{len(y)} bytes pasted.")
                self.display.jump(x + len(y))
            else:
                self.stderr("Yank buffer empty.")
            return -1
        
        if idx < len(line) and line[idx] == 'P':
            y = list(self.memory.yank)
            if y:
                self.save_undo_state()
                self.memory.insmem(x, y)
                self.commit_undo()
                self.stdmm(f"{len(y)} bytes pasted (insert).")
                self.display.jump(x + len(y))
            else:
                self.stderr("Yank buffer empty.")
            return -1
        
        # mark
        if idx + 1 < len(line) and line[idx] == 'm':
            if 'a' <= line[idx + 1] <= 'z':
                self.memory.mark[ord(line[idx + 1]) - ord('a')] = x
            else:
                self.stderr("Invalid mark character (use 'ma' to 'mz').")
            return -1
        
        # partial read with new offset: [offset] rp  /  [offset,end] rp
        # x, x2 は絶対アドレスのまま（parse_range_command での変換をスキップ済み）
        if idx + 1 < len(line) and line[idx] == 'r' and line[idx + 1] == 'p':
            abs_off  = x if xf else g_partial.init_offset
            load_len = (x2 - abs_off + 1) if xf2 else g_partial.init_length
            success, msg = self.filemgr.readfile_partial(
                self.filemgr.filename, abs_off, load_len)
            if success:
                g_partial.init_offset = abs_off
                g_partial.init_length = load_len
                self.display.jump(0)
                self.display.highlight_ranges = []
                self.stdmm(msg)
            else:
                self.stderr(msg)
            return -1

        # read file — ファイル名省略時はカレントファイルを使う
        if idx < len(line) and (line[idx] == 'r' or line[idx] == 'R'):
            ch = line[idx]
            idx += 1
            fn = line[idx:].lstrip() if idx < len(line) else ''
            if fn == '':
                fn = self.filemgr.filename  # 省略時はカレントファイル
            data = []
            read_error = False
            try:
                f = open(fn, "rb")
                data = list(f.read())
                f.close()
            except:
                self.stderr("File read error.")
                read_error = True
            if data:
                self.save_undo_state()
                if ch == 'r':
                    self.memory.ovwmem(x, data)
                elif ch == 'R':
                    self.memory.insmem(x, data)
                self.commit_undo()
                self.display.jump(x + len(data))
            elif not read_error:
                self.stderr("File specified is empty.")
            return -1
        
        if idx < len(line):
            ch = line[idx]
        else:
            ch = ''
        
        # delete
        if ch == 'd':
            self.save_undo_state()
            if self.memory.delmem(x, x2, True, self.memory.yankmem):
                self.commit_undo()
                self.stdmm(f"{x2 - x + 1} bytes deleted.")
                self.display.jump(x)
            else:
                self.dec_undo()
                self.stderr("Invalid range.")
            return -1

        # substitute
        elif ch == 's':
            self.scommand(x, x2, xf, xf2, line, idx + 1)
            return -1
        
        # not
        if idx < len(line) and line[idx] == '~':
            self.save_undo_state()
            self.openot(x, x2)
            self.commit_undo()
            self.display.jump(x2 + 1)
            return -1
        
        # その他の複雑なコマンド
        if idx < len(line) and line[idx] in "IivCc&|^<>f":
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
            
            self.save_undo_state()
            self.shift_rotate(x, x2, times, bit, multibyte, ch)
            self.commit_undo()
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
                    self.save_undo_state()
                    data = m * ((x2 - x + 1) // len(m)) + m[0:((x2 - x + 1) % len(m))]
                    self.memory.ovwmem(x, data)
                    self.commit_undo()
                    self.stdmm(f"{len(data)} bytes filled.")
                    self.display.jump(x + len(data))
                else:
                    self.stderr("No data specified.")
                return -1
            
            if ch == 'I' and xf2:
                self.stderr("Invalid syntax.")
                return -1
            
            data = m * length
            if data:
                self.save_undo_state()
                if ch == 'i':
                    self.memory.ovwmem(x, data)
                    self.commit_undo()
                    self.stdmm(f"{len(data)} bytes overwritten.")
                else:
                    self.memory.insmem(x, data)
                    self.commit_undo()
                    self.stdmm(f"{len(data)} bytes inserted.")
                self.display.jump(x + len(data))
            else:
                self.stderr("No data specified.")
            return -1
        
        # 残りのコマンドは第3引数が必要
        x3, idx = self.parser.expression(line, idx)
        if x3 == Parser.UNKNOWN:
            self.stderr("Invalid parameter.")
            return -1
        
        # copy/Copy
        if ch == 'c':
            # パーシャル編集中: x3 もバッファ相対に変換
            if g_partial.active and g_partial.offset > 0:
                x3 = max(0, x3 - g_partial.offset)
            self.save_undo_state()
            self.memory.yankmem(x, x2)
            m = self.memory.redmem(x, x2)
            self.memory.ovwmem(x3, m)
            self.commit_undo()
            self.stdmm(f"{x2 - x + 1} bytes copied.")
            self.display.jump(x3 + (x2 - x + 1))
            return -1
        elif ch == 'C':
            # パーシャル編集中: x3 もバッファ相対に変換
            if g_partial.active and g_partial.offset > 0:
                x3 = max(0, x3 - g_partial.offset)
            self.save_undo_state()
            m = self.memory.redmem(x, x2)
            self.memory.yankmem(x, x2)
            self.memory.insmem(x3, m)
            self.commit_undo()
            self.stdmm(f"{x2 - x + 1} bytes inserted.")
            self.display.jump(x3 + len(m))
            return -1
        
        # move
        elif ch == 'v':
            # パーシャル編集中: x3 もバッファ相対に変換
            if g_partial.active and g_partial.offset > 0:
                x3 = max(0, x3 - g_partial.offset)
            self.save_undo_state()
            xp = self.movmem(x, x2, x3)
            self.commit_undo()
            self.display.jump(xp)
            return -1
        
        # ビット演算
        elif ch == '&':
            self.save_undo_state()
            self.opeand(x, x2, x3)
            self.commit_undo()
            self.display.jump(x2 + 1)
            return -1
        elif ch == '|':
            self.save_undo_state()
            self.opeor(x, x2, x3)
            self.commit_undo()
            self.display.jump(x2 + 1)
            return -1
        elif ch == '^':
            self.save_undo_state()
            self.opexor(x, x2, x3)
            self.commit_undo()
            self.display.jump(x2 + 1)
            return -1

        # ====================================================================
        # [start],[end] f [start2]  — 2領域バイト比較（バンド幅±10 の LCS diff）
        # Region1: mem[start..end]
        # Region2: mem[start2 .. start2+(end-start)] (同長)
        # 表示: 8バイト/行、一致=白、差異=赤
        # ====================================================================
        elif ch == 'f':
            if not xf or not xf2 or x > x2:
                self.stderr("Invalid range. Usage: start,end f start2")
                return -1
            if self.scripting and not self.verbose:
                return -1
            FCMP_SPAN = 10
            FCMP_MAXN = 8192

            n1 = int(x2 - x + 1)
            if n1 > FCMP_MAXN:
                n1 = FCMP_MAXN
                self.stdmm("  Note: comparison truncated to 8192 bytes.")

            n2 = n1

            # 範囲外バイト用に安全なバッファを確保（OOB は 0 で埋める）
            mem_size = len(self.memory.mem)
            s1 = [self.memory.mem[int(x) + i] if int(x) + i < mem_size else 0 for i in range(n1)]
            s2 = [self.memory.mem[int(x3) + i] if int(x3) + i < mem_size else 0 for i in range(n2)]

            span = FCMP_SPAN
            bw   = 2 * span + 1  # バンド幅

            # dp[i*bw + d], dir[i*bw + d]
            dp  = [0] * ((n1 + 1) * bw)
            dirb = [0] * ((n1 + 1) * bw)  # 1=上 2=左 3=斜め一致 4=斜め不一致

            # 境界初期化
            for jj in range(1, min(span, n2) + 1):
                dirb[0 * bw + jj + span] = 2
            for ii in range(1, min(span, n1) + 1):
                dirb[ii * bw + span - ii] = 1

            # DP 充填
            for ii in range(1, n1 + 1):
                jlo = max(1, ii - span)
                jhi = min(n2, ii + span)
                for jj in range(jlo, jhi + 1):
                    d   = jj - ii + span
                    cur = ii * bw + d
                    best = -1
                    bdir = 0

                    # 斜め (ii-1, jj-1)
                    if ii >= 1 and jj >= 1:
                        dd = jj - 1 - (ii - 1) + span
                        if 0 <= dd < bw:
                            prev = dp[(ii - 1) * bw + dd]
                            if s1[ii - 1] == s2[jj - 1]:
                                v = prev + 1
                                if v > best:
                                    best = v; bdir = 3
                            else:
                                if prev > best:
                                    best = prev; bdir = 4

                    # 上 (ii-1, jj)
                    dd = d + 1
                    if 0 <= dd < bw:
                        v = dp[(ii - 1) * bw + dd]
                        if v > best:
                            best = v; bdir = 1

                    # 左 (ii, jj-1)
                    if jj >= 1:
                        dd = d - 1
                        if 0 <= dd < bw:
                            v = dp[ii * bw + dd]
                            if v > best:
                                best = v; bdir = 2

                    if best < 0:
                        best = 0
                    dp[cur]   = best
                    dirb[cur] = bdir

            # トレースバック
            align_a = []
            align_b = []
            ci, cj = n1, n2
            while ci > 0 or cj > 0:
                in_band = (abs(ci - cj) <= span)
                if not in_band or ci == 0:
                    # バンド外または s1 使い切り → s2 を消費
                    if cj > 0:
                        align_a.append(-1)
                        align_b.append(s2[cj - 1])
                        cj -= 1
                    else:
                        # s2 も使い切り → s1 を消費（残りを削除扱い）
                        align_a.append(s1[ci - 1])
                        align_b.append(-1)
                        ci -= 1
                elif cj == 0:
                    align_a.append(s1[ci - 1])
                    align_b.append(-1)
                    ci -= 1
                else:
                    d  = cj - ci + span
                    dv = dirb[ci * bw + d]
                    if dv == 3 or dv == 4:  # 斜め
                        align_a.append(s1[ci - 1])
                        align_b.append(s2[cj - 1])
                        ci -= 1; cj -= 1
                    elif dv == 1:           # 上: s1 削除
                        align_a.append(s1[ci - 1])
                        align_b.append(-1)
                        ci -= 1
                    else:                   # 左: s2 挿入
                        align_a.append(-1)
                        align_b.append(s2[cj - 1])
                        cj -= 1

            # 逆順に並べ直す
            align_a.reverse()
            align_b.reverse()
            np_ = len(align_a)

            # 表示: 8ペア/行
            def _fmt_addr(a):
                if a < 0:
                    return f"-{(-a):012X}"
                return f"{a:012X}"
            addr1_base = int(x) + g_partial.offset
            addr2_base = int(x3) + g_partial.offset
            self.term.color(5)
            print(f" R1-addr      Region1 ({_fmt_addr(addr1_base)})   R2-addr      Region2 ({_fmt_addr(addr2_base)})")

            any_diff = False
            off1 = 0
            off2 = 0

            rs = 0
            while rs < np_:
                re = min(rs + 8, np_)

                # 範囲外フラグを事前計算（最大8エントリ）
                oob_a = [False] * 8
                oob_b = [False] * 8
                to1, to2 = off1, off2
                for k in range(rs, re):
                    ki = k - rs
                    if align_a[k] >= 0:
                        oob_a[ki] = (int(x) + to1 >= mem_size)
                        to1 += 1
                    if align_b[k] >= 0:
                        oob_b[ki] = (int(x3) + to2 >= mem_size)
                        to2 += 1

                row_diff = any(
                    align_a[k] != align_b[k] or oob_a[k - rs] != oob_b[k - rs]
                    for k in range(rs, re))
                if row_diff:
                    any_diff = True

                row_off1 = off1
                row_off2 = off2

                r1_abs = addr1_base + row_off1
                r2_abs = addr2_base + row_off2
                print(f" {_fmt_addr(r1_abs)} ", end='')

                # Region1
                for k in range(rs, rs + 8):
                    if k < re:
                        ki = k - rs
                        diff = (align_a[k] != align_b[k] or oob_a[ki] != oob_b[ki])
                        if diff:
                            self.term.color(1)
                        if align_a[k] < 0:
                            print("-- ", end='')
                        elif oob_a[ki]:
                            print("~~ ", end='')
                        else:
                            print(f"{align_a[k]:02X} ", end='')
                        if diff:
                            self.term.color(7)
                    else:
                        print("   ", end='')

                print(f" {_fmt_addr(r2_abs)} ", end='')

                # Region2
                for k in range(rs, rs + 8):
                    if k < re:
                        ki = k - rs
                        diff = (align_a[k] != align_b[k] or oob_a[ki] != oob_b[ki])
                        if diff:
                            self.term.color(1)
                        if align_b[k] < 0:
                            print("-- ", end='')
                        elif oob_b[ki]:
                            print("~~ ", end='')
                        else:
                            print(f"{align_b[k]:02X} ", end='')
                        if diff:
                            self.term.color(7)
                    else:
                        print("   ", end='')

                print("*") if row_diff else print()
                sys.stdout.flush()

                for k in range(rs, re):
                    if align_a[k] >= 0:
                        off1 += 1
                    if align_b[k] >= 0:
                        off2 += 1

                rs += 8

            print("\x1b[0m", end='', flush=True)

            if not self.scriptingflag:
                self.term.color(4)
                if not any_diff:
                    msg = "  Identical. [ hit a key ]"
                else:
                    msg = "  Differences found. [ hit a key ]"
                print(msg, end='', flush=True)
                Terminal.getch()
                self.term.clear()
                self.display.repaint(self.filemgr.filename)

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
        be = (self.endian == 'big')
        if be:
            # ビッグエンディアン: x が MSB
            for i in range(x, x2 + 1):
                v = (v << 8) | self.memory.readmem(i)
        else:
            # リトルエンディアン: x が LSB
            for i in range(x2, x - 1, -1):
                v = (v << 8) | self.memory.readmem(i)
        return v
    
    def put_multibyte_value(self, x, x2, v):
        be = (self.endian == 'big')
        if be:
            # ビッグエンディアン: x2 から x へ LSB → MSB の順で書き戻す
            for i in range(x2, x - 1, -1):
                self.memory.setmem(i, v & 0xff)
                v >>= 8
        else:
            # リトルエンディアン: x から x2 へ LSB → MSB の順で書き戻す
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
        """置換コマンド
        undo 記録はこの関数が自前で管理する。
        呼び出し元で save_undo_state / commit_undo を呼ぶ必要はない。
        """
        self.save_undo_state()   # undo 差分記録を開始
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
                # span はマッチ実行後に hitre() が更新するため、ここでは 1 以上の仮値を設定して
                # 「検索対象あり」チェックだけ通す。実際の削除幅は searchnextnoloop 後に使う self.search.span で決まる。
                self.search.span = max(1, len(m))
            elif idx < len(line) and line[idx] == '/':
                self.search.smem, idx = self.parser.get_hexs(line, idx + 1)
                self.search.regexp = False
                self.search.remem = ''
                self.search.span = len(self.search.smem)
            else:
                self.dec_undo()  # 変更なしのままキャンセル
                self.stderr("Invalid syntax.")
                return
        
        if self.search.span == 0:
            self.dec_undo()  # 変更なしのままキャンセル
            self.stderr("Specify search object.")
            return
        
        n, idx = self.parser.get_str_or_hexs(line, idx)
        
        i = start
        cnt = 0
        self.display.jump(i)
        
        while True:
            f = self.searchnextnoloop(self.display.fpos())
            
            i = self.display.fpos()
            
            if f < 0:
                # 検索エラー: それまでの置換分をコミット（0 件なら commit_undo が自動で no-op にする）
                self.commit_undo()
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
                self.commit_undo()   # 全置換完了でコミット
                self.stdmm(f"  {cnt} times replaced.")
                return
    
    def searchnextnoloop(self, fp):
        """ループしない検索"""
        cur_pos = fp
        
        if not self.search.regexp and not self.search.smem:
            return 0
        self.stdmm_wait("Wait.")
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
        
        line = f.readline()
        
        while line:
            if self.verbose:
                print(line,end='')
            line=line.strip()
            flag = self.commandline(line)
            if flag == 0:
                f.close()
                return 0
            elif flag == 1:
                f.close()
                return 1
            line = f.readline()
        
        f.close()
        return 0


def main():
    """メイン関数"""
    ap = argparse.ArgumentParser(
        usage='%(prog)s [-h] [options] <file> [options]',
        description='Binary editor. Options can appear before or after <file>.'
    )
    ap.add_argument('file', help='file to edit')
    ap.add_argument('-s', '--script', type=str, default='', metavar='script.bi',
                    help='bi script file')
    ap.add_argument('-t', '--termcolor', type=str, default='',
                    help="color scheme: 'black' (white on black), 'white' (black on white), 'color' (multi-color mode); omit to use terminal default")
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='verbose when processing script')
    ap.add_argument('-w', '--write', action='store_true',
                    help='write file when exiting script')
    ap.add_argument('-o', '--offset', type=lambda x: int(x, 16), default=None,
                    metavar='OFFSET', help='partial edit: start offset (hex)')
    ap.add_argument('-l', '--length', type=lambda x: int(x, 16), default=None,
                    metavar='LENGTH', help='partial edit: length in bytes (hex)')
    ap.add_argument('-e', '--end', type=lambda x: int(x, 16), default=None,
                    metavar='END', help='partial edit: end offset inclusive (hex)')
    args = ap.parse_args()

    # パーシャルモードの判定・長さ計算
    partial_mode = False
    partial_offset = args.offset if args.offset is not None else 0
    partial_length = 0  # 0 = EOF まで

    if args.offset is not None:
        partial_mode = True
    if args.length is not None:
        partial_length = args.length
        partial_mode = True
    if args.end is not None:
        if args.end < partial_offset:
            print(f"Error: -e value (0x{args.end:X}) is less than -o value (0x{partial_offset:X}).",
                  file=sys.stderr)
            return
        partial_length = args.end - partial_offset + 1
        partial_mode = True

    # 起動時パーシャル範囲を g_partial に保存（:rp コマンドで再ロードに使用）
    g_partial.init_offset = partial_offset
    g_partial.init_length = partial_length

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
    if partial_mode:
        success, msg = editor.filemgr.readfile_partial(args.file, partial_offset, partial_length)
    else:
        success, msg = editor.filemgr.readfile(args.file)

    if not success:
        print(msg, file=sys.stderr)
        return
    elif msg:
        editor.stdmm(msg)

    # スクリプト実行またはインタラクティブモード
    if args.script:
        try:
            editor.scripting(args.script)
            if not editor.memory.lastchange:
                print('Nothing done.')
            if args.write and editor.memory.lastchange:
                if g_partial.active:
                    ok, wmsg = editor.filemgr.writefile_partial(args.file)
                else:
                    ok, wmsg = editor.filemgr.writefile(args.file)
                if ok:
                    if editor.verbose:
                        print(wmsg)
                else:
                    print(wmsg, file=sys.stderr)
        except Exception as exc:
            editor.filemgr.writefile("file.save")
            editor.stderr(f"Some error occured ({exc}). memory saved to file.save.")
    else:
        try:
            editor.fedit()
        except Exception as exc:
            editor.filemgr.writefile("file.save")
            editor.stderr(f"Some error occured ({exc}). memory saved to file.save.")

    # 終了処理
    editor.term.color(7)
    editor.term.dispcursor()
    editor.term.locate(0, editor.display.BOTTOMLN+1)


if __name__ == "__main__":
    main()
