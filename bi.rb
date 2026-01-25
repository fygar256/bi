#!/usr/bin/env ruby
# coding: utf-8

require 'io/console'
require 'readline'
require 'optparse'

class Terminal
  ESC = "\033["
  
  attr_accessor :termcol, :coltab
  
  def initialize(termcol = 'black')
    @termcol = termcol
    @coltab = [0, 1, 4, 5, 2, 6, 3, 7]
  end
  
  def nocursor
    print "#{ESC}?25l"
    $stdout.flush
  end
  
  def dispcursor
    print "#{ESC}?25h"
    $stdout.flush
  end
  
  def up(n = 1)
    print "#{ESC}#{n}A"
  end
  
  def down(n = 1)
    print "#{ESC}#{n}B"
  end
  
  def right(n = 1)
    print "#{ESC}#{n}C"
  end
  
  def left(n = 1)
    print "#{ESC}#{n}D"
    $stdout.flush
  end
  
  def locate(x = 0, y = 0)
    print "#{ESC}#{y+1};#{x+1}H"
    $stdout.flush
  end
  
  def scrollup(n = 1)
    print "#{ESC}#{n}S"
  end
  
  def scrolldown(n = 1)
    print "#{ESC}#{n}T"
  end
  
  def clear
    print "#{ESC}2J"
    $stdout.flush
    locate
  end
  
  def clraftcur
    print "#{ESC}0J"
    $stdout.flush
  end
  
  def clrline
    print "#{ESC}2K"
    $stdout.flush
  end
  
  def color(col1 = 7, col2 = 0)
    if @termcol == 'black'
      print "#{ESC}3#{@coltab[col1]}m#{ESC}4#{@coltab[col2]}m"
    else
      print "#{ESC}3#{@coltab[0]}m#{ESC}4#{@coltab[7]}m"
    end
    $stdout.flush
  end
  
  def resetcolor
    print "#{ESC}0m"
  end
  
  def self.getch
    $stdin.getch
  end
end

class HistoryManager
  def initialize
    @histories = {
      'command' => [],
      'search' => []
    }
  end
  
  def get_history_list
    list = []
    (1..Readline::HISTORY.length).each do |i|
      list << Readline::HISTORY[i - 1]
    end
    list
  end
  
  def set_history_list(mode)
    history_items = @histories[mode]
    Readline::HISTORY.clear
    history_items.each do |item|
      Readline::HISTORY << item
    end
  end
  
  def getln(s = "", mode = "command")
    mode = mode == "search" ? "search" : "command"
    set_history_list(mode)
    
    begin
      user_input = Readline.readline(s, true)
      user_input ||= ""
    rescue
      user_input = ""
    end
    
    @histories[mode] = get_history_list
    user_input
  end
end

class MemoryBuffer
  UNKNOWN = 0xffffffffffffffffffffffffffffffff
  
  attr_accessor :mem, :yank, :mark, :modified, :lastchange
  
  def initialize
    @mem = []
    @yank = []
    @mark = Array.new(26, UNKNOWN)
    @modified = false
    @lastchange = false
  end
  
  def length
    @mem.length
  end
  
  def readmem(addr)
    return 0 if addr >= @mem.length
    @mem[addr] & 0xff
  end
  
  def setmem(addr, data)
    if addr >= @mem.length
      (addr - @mem.length + 1).times { @mem << 0 }
    end
    
    if data.is_a?(Integer) && data >= 0 && data <= 255
      @mem[addr] = data
    else
      @mem[addr] = 0
    end
    
    @modified = true
    @lastchange = true
  end
  
  def insmem(start, mem2)
    if start >= @mem.length
      (start - @mem.length).times { @mem << 0 }
      @mem = @mem + mem2
      @modified = true
      @lastchange = true
      return
    end
    
    mem1 = @mem[0...start]
    mem3 = @mem[start..-1]
    @mem = mem1 + mem2 + mem3
    @modified = true
    @lastchange = true
  end
  
  def delmem(start, _end, yf, yankmem_func)
    length = _end - start + 1
    return false if length <= 0 || start >= @mem.length
    
    yankmem_func.call(start, _end) if yf
    
    @mem = @mem[0...start] + @mem[(_end + 1)..-1].to_a
    @lastchange = true
    @modified = true
    true
  end
  
  def yankmem(start, _end)
    length = _end - start + 1
    return 0 if length <= 0 || start >= @mem.length
    
    @yank = []
    cnt = 0
    (start.._end).each do |j|
      if j < @mem.length
        cnt += 1
        @yank << (@mem[j] & 0xff)
      end
    end
    cnt
  end
  
  def ovwmem(start, mem0)
    return if mem0.nil? || mem0.empty?
    
    if start + mem0.length >= @mem.length
      (start + mem0.length - @mem.length).times { @mem << 0 }
    end
    
    mem0.each_with_index do |val, j|
      if start + j >= @mem.length
        @mem << (val & 0xff)
      else
        @mem[start + j] = val & 0xff
      end
    end
    
    @lastchange = true
    @modified = true
  end
  
  def redmem(start, _end)
    m = []
    (start.._end).each do |i|
      if @mem.length > i
        m << (@mem[i] & 0xff)
      else
        m << 0
      end
    end
    m
  end
  
  def regulate_mem
    @mem.each_with_index do |val, i|
      begin
        @mem[i] = val & 0xff
      rescue
        @mem[i] = 0
      end
    end
  end
end

class SearchEngine
  RELEN = 128
  
  attr_accessor :smem, :regexp, :remem, :span, :nff
  
  def initialize(memory_buffer)
    @memory = memory_buffer
    @smem = []
    @regexp = false
    @remem = ''
    @span = 0
    @nff = true
  end
  
  def hit(addr)
    @smem.each_with_index do |val, i|
      return 0 if addr + i >= @memory.mem.length || @memory.mem[addr + i] != val
    end
    1
  end
  
  def hitre(addr)
    return -1 if @remem.empty?
    
    @span = 0
    m = []
    
    if addr < @memory.mem.length - RELEN
      m = @memory.mem[addr...(addr + RELEN)]
    else
      m = @memory.mem[addr..-1] || []
    end
    
    byte_data = m.pack('C*')
    begin
      ms = byte_data.force_encoding('UTF-8').encode('UTF-8', invalid: :replace, undef: :replace)
    rescue
      return -1
    end
    
    begin
      f = ms.match(/\A#{@remem}/)
    rescue
      return -1
    end
    
    if f
      matched_str = f[0]
      begin
        matched_bytes = matched_str.encode('UTF-8').bytes
      rescue
        return -1
      end
      
      @span = matched_bytes.length
      return 1
    else
      return 0
    end
  end
  
  def searchnext(fp, mem_len)
    curpos = fp
    start = fp
    return false if !@regexp && @smem.empty?
    
    loop do
      f = @regexp ? hitre(curpos) : hit(curpos)
      
      return curpos if f == 1
      return nil if f < 0
      
      curpos += 1
      
      if curpos >= mem_len
        if @nff
          curpos = 0
        else
          return nil
        end
      end
      
      return nil if curpos == start
    end
  end
  
  def searchlast(fp, mem_len)
    curpos = fp
    start = fp
    return false if !@regexp && @smem.empty?
    
    loop do
      f = @regexp ? hitre(curpos) : hit(curpos)
      
      return curpos if f == 1
      return nil if f < 0
      
      curpos -= 1
      curpos = mem_len - 1 if curpos < 0
      
      return nil if curpos == start
    end
  end
end

class Display
  LENONSCR = 19 * 16
  BOTTOMLN = 22
  
  attr_accessor :homeaddr, :curx, :cury, :utf8, :repsw, :insmod
  
  def initialize(terminal, memory_buffer)
    @term = terminal
    @memory = memory_buffer
    @homeaddr = 0
    @curx = 0
    @cury = 0
    @utf8 = false
    @repsw = 0
    @insmod = false
  end
  
  def fpos
    @homeaddr + @curx / 2 + @cury * 16
  end
  
  def jump(addr)
    if addr < @homeaddr || addr >= @homeaddr + LENONSCR
      @homeaddr = addr & ~(0xff)
    end
    i = addr - @homeaddr
    @curx = (i & 0xf) * 2
    @cury = i / 16
  end
  
  def scrup
    @homeaddr -= 16 if @homeaddr >= 16
  end
  
  def scrdown
    @homeaddr += 16
  end
  
  def inccurx
    if @curx < 31
      @curx += 1
    else
      @curx = 0
      if @cury < LENONSCR / 16 - 1
        @cury += 1
      else
        scrdown
      end
    end
  end
  
  def printchar(a)
    if a >= @memory.mem.length
      print "~"
      $stdout.flush
      return 1
    end
    
    if @utf8
      byte_val = @memory.mem[a]
      if byte_val < 0x80 || (0x80 <= byte_val && byte_val <= 0xbf) || (0xf8 <= byte_val && byte_val <= 0xff)
        ch = (byte_val >= 0x20 && byte_val <= 0x7e) ? byte_val.chr : '.'
        print ch
        return 1
      elsif byte_val >= 0xc0 && byte_val <= 0xdf
        m = [@memory.readmem(a + @repsw), @memory.readmem(a + 1 + @repsw)]
        begin
          ch = m.pack('C*').force_encoding('UTF-8')
          print ch
          $stdout.flush
          return 2
        rescue
          print "."
          return 1
        end
      elsif byte_val >= 0xe0 && byte_val <= 0xef
        m = [@memory.readmem(a + @repsw), @memory.readmem(a + 1 + @repsw), @memory.readmem(a + 2 + @repsw)]
        begin
          ch = m.pack('C*').force_encoding('UTF-8')
          print "#{ch} "
          $stdout.flush
          return 3
        rescue
          print "."
          return 1
        end
      elsif byte_val >= 0xf0 && byte_val <= 0xf7
        m = [@memory.readmem(a + @repsw), @memory.readmem(a + 1 + @repsw),
             @memory.readmem(a + 2 + @repsw), @memory.readmem(a + 3 + @repsw)]
        begin
          ch = m.pack('C*').force_encoding('UTF-8')
          print "#{ch}  "
          $stdout.flush
          return 4
        rescue
          print "."
          return 1
        end
      end
    else
      ch = (@memory.mem[a] >= 0x20 && @memory.mem[a] <= 0x7e) ? @memory.mem[a].chr : '.'
      print ch
      return 1
    end
  end
  
  def print_title(filename)
    @term.locate(0, 0)
    @term.color(6)
    utf8_mode = @utf8 ? @repsw : "off"
    mode_str = @insmod ? "insert   " : "overwrite"
    puts "bi ruby version 3.4.4 by T.Maekawa              utf8mode:#{utf8_mode}     #{mode_str}   "
    @term.color(5)
    fn = filename.length > 35 ? filename[0...35] : filename
    modified_str = @memory.modified ? "modified" : "not modified"
    print "file:[#{fn.ljust(35)}] length:#{@memory.mem.length} bytes [#{modified_str}]    "
  end
  
  def repaint(filename)
    print_title(filename)
    @term.nocursor
    @term.locate(0, 2)
    @term.color(4)
    print "OFFSET       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F 0123456789ABCDEF "
    @term.color(7)
    addr = @homeaddr
    
    (LENONSCR / 16).times do |y|
      @term.color(5)
      @term.locate(0, 3 + y)
      print "%012X " % ((addr + y * 16) & 0xffffffffffff)
      @term.color(7)
      
      16.times do |i|
        a = y * 16 + i + addr
        if a >= @memory.mem.length
          print "~~ "
        else
          print "%02X " % (@memory.mem[a] & 0xff)
        end
      end
      
      @term.color(6)
      a = y * 16 + addr
      by = 0
      while by < 16
        c = printchar(a)
        a += c
        by += c
      end
      print "  "
      $stdout.flush
    end
    
    @term.color(0)
    @term.dispcursor
  end
  
  def printdata
    addr = fpos
    a = @memory.readmem(addr)
    @term.locate(0, 23)
    @term.color(6)
    
    s = '.'
    if a < 0x20
      s = '^' + (a + '@'.ord).chr
    elsif a >= 0x7e
      s = '.'
    else
      s = "'" + a.chr + "'"
    end
    
    if addr < @memory.mem.length
      print "%012X : 0x%02X 0b%08b 0o%03o %d %s      " % [addr, a, a, a, a, s]
    else
      print "%012X : ~~                                                   " % addr
    end
    $stdout.flush
  end
  
  def disp_curpos
    @term.color(4)
    @term.locate(@curx / 2 * 3 + 12, @cury + 3)
    print "["
    $stdout.flush
    @term.locate(@curx / 2 * 3 + 15, @cury + 3)
    print "]"
    $stdout.flush
  end
  
  def erase_curpos
    @term.color(7)
    @term.locate(@curx / 2 * 3 + 12, @cury + 3)
    print " "
    $stdout.flush
    @term.locate(@curx / 2 * 3 + 15, @cury + 3)
    print " "
    $stdout.flush
  end
  
  def clrmm
    @term.locate(0, BOTTOMLN)
    @term.color(6)
    @term.clrline
  end
  
  def stdmm(s, scripting, verbose)
    if scripting
      puts s if verbose
    else
      clrmm
      @term.color(4)
      @term.locate(0, BOTTOMLN)
      print " #{s}"
      $stdout.flush
    end
  end
  
  def stderr(s, scripting, verbose)
    if scripting
      $stderr.puts s
    else
      clrmm
      @term.color(3)
      @term.locate(0, BOTTOMLN)
      print " #{s}"
      $stdout.flush
    end
  end
end

class Parser
  UNKNOWN = 0xffffffffffffffffffffffffffffffff
  
  def initialize(memory_buffer, display)
    @memory = memory_buffer
    @display = display
  end
  
  def self.skipspc(s, idx)
    while idx < s.length && s[idx] == ' '
      idx += 1
    end
    idx
  end
  
  def skipspc(s, idx)
    Parser.skipspc(s, idx)
  end
  
  def get_value(s, idx)
    return [UNKNOWN, idx] if idx >= s.length
    idx = skipspc(s, idx)
    ch = s[idx]
    
    if ch == '$'
      idx += 1
      v = @memory.mem.length != 0 ? @memory.mem.length - 1 : 0
    elsif ch == '{'
      idx += 1
      u = ''
      while idx < s.length
        if s[idx] == '}'
          idx += 1
          break
        end
        u += s[idx]
        idx += 1
      end
      return [UNKNOWN, idx] unless s[idx - 1] == '}'
      
      begin
        v = eval(u).to_i
      rescue
        return [UNKNOWN, idx]
      end
    elsif ch == '.'
      idx += 1
      v = @display.fpos
    elsif ch == "'" && s.length > idx + 1 && s[idx + 1] >= 'a' && s[idx + 1] <= 'z'
      idx += 1
      v = @memory.mark[s[idx].ord - 'a'.ord]
      if v == UNKNOWN
        return [UNKNOWN, idx - 1]
      else
        idx += 1
      end
    elsif idx < s.length && s[idx].match?(/[0-9a-fA-F]/)
      x = 0
      while idx < s.length && s[idx].match?(/[0-9a-fA-F]/)
        x = 16 * x + s[idx].to_i(16)
        idx += 1
      end
      v = x
    elsif ch == '%'
      x = 0
      idx += 1
      while idx < s.length && s[idx].match?(/[0-9]/)
        x = 10 * x + s[idx].to_i
        idx += 1
      end
      v = x
    else
      v = UNKNOWN
    end
    
    v = 0 if v < 0
    [v, idx]
  end
  
  def expression(s, idx)
    x, idx = get_value(s, idx)
    if s.length > idx && x != UNKNOWN && s[idx] == '+'
      y, idx = get_value(s, idx + 1)
      x = x + y
    elsif s.length > idx && x != UNKNOWN && s[idx] == '-'
      y, idx = get_value(s, idx + 1)
      x = x - y
      x = 0 if x < 0
    end
    [x, idx]
  end
  
  def self.get_restr(s, idx)
    m = ''
    while idx < s.length
      break if s[idx] == '/'
      
      if idx + 1 < s.length && s[idx..idx+1] == "\\\\"
        m += '\\\\'
        idx += 2
      elsif idx + 1 < s.length && s[idx..idx+1] == "\\/"
        m += '/'
        idx += 2
      elsif s[idx] == '\\' && s.length - 1 == idx
        idx += 1
        break
      else
        m += s[idx]
        idx += 1
      end
    end
    [m, idx]
  end
  
  def get_restr(s, idx)
    Parser.get_restr(s, idx)
  end
  
  def get_hexs(s, idx)
    m = []
    while idx < s.length
      v, idx = expression(s, idx)
      break if v == UNKNOWN
      m << (v & 0xff)
    end
    [m, idx]
  end
  
  def get_str_or_hexs(line, idx)
    idx = skipspc(line, idx)
    if idx < line.length && line[idx] == '/'
      idx += 1
      if idx < line.length && line[idx] == '/'
        m, idx = get_hexs(line, idx + 1)
      else
        s, idx = get_restr(line, idx)
        begin
          bseq = s.encode('UTF-8').bytes
        rescue
          return [[], idx]
        end
        m = bseq
      end
    else
      m = []
    end
    [m, idx]
  end
  
  def get_str(line, idx)
    s, idx = get_restr(line, idx)
    begin
      bseq = s.encode('UTF-8').bytes
    rescue
      return [[], idx]
    end
    m = bseq
    [m, idx]
  end
  
  def self.comment(s)
    idx = 0
    m = ''
    while idx < s.length
      break if s[idx] == '#'
      
      if idx + 1 < s.length && s[idx..idx+1] == "\\#"
        m += '#'
        idx += 2
      elsif idx + 1 < s.length && s[idx..idx+1] == "\\n"
        m += "\n"
        idx += 2
      else
        m += s[idx]
        idx += 1
      end
    end
    m
  end
  
  def comment(s)
    Parser.comment(s)
  end
end

class FileManager
  attr_accessor :filename, :newfile
  
  def initialize(memory_buffer)
    @memory = memory_buffer
    @filename = ""
    @newfile = false
  end
  
  def readfile(fn)
    begin
      f = File.open(fn, "rb")
    rescue
      @newfile = true
      @memory.mem = []
      return [true, "<new file>"]
    else
      @newfile = false
      begin
        @memory.mem = f.read.bytes
        f.close
        return [true, nil]
      rescue
        f.close
        return [false, "Memory overflow."]
      end
    end
  end
  
  def writefile(fn)
    @memory.regulate_mem
    begin
      f = File.open(fn, "wb")
      f.write(@memory.mem.pack('C*'))
      f.close
      return [true, "File written."]
    rescue
      return [false, "Permission denied."]
    end
  end
  
  def wrtfile(start, _end, fn)
    @memory.regulate_mem
    begin
      f = File.open(fn, "wb")
      (start.._end).each do |i|
        if i < @memory.mem.length
          f.write([@memory.mem[i]].pack('C'))
        else
          f.write([0].pack('C'))
        end
      end
      f.close
      return [true, nil]
    rescue
      return [false, "Permission denied."]
    end
  end
end

class BiEditor
  def initialize(termcol = 'black')
    @term = Terminal.new(termcol)
    @memory = MemoryBuffer.new
    @display = Display.new(@term, @memory)
    @parser = Parser.new(@memory, @display)
    @history = HistoryManager.new
    @search = SearchEngine.new(@memory)
    @filemgr = FileManager.new(@memory)
    
    @verbose = false
    @scriptingflag = false
    @stack = []
    @cp = 0
  end
  
  attr_accessor :verbose, :scriptingflag, :filemgr, :term, :memory, :display
  
  def stdmm(s)
    @display.stdmm(s, @scriptingflag, @verbose)
  end
  
  def stderr(s)
    @display.stderr(s, @scriptingflag, @verbose)
  end
  
  def disp_marks
    j = 0
    @term.locate(0, Display::BOTTOMLN)
    @term.color(7)
    
    ('a'..'z').each do |i|
      m = @memory.mark[j]
      if m == MemoryBuffer::UNKNOWN
        print "#{i} = unknown         "
      else
        print "#{i} = %012X    " % @memory.mark[j]
      end
      j += 1
      puts if j % 3 == 0
    end
    
    @term.color(4)
    puts "[ hit any key ]"
    Terminal.getch
    @term.clear
  end
  
  def invoke_shell(line)
    @term.color(7)
    puts
    system(line.lstrip)
    @term.color(4)
    print "[ Hit any key to return ]"
    $stdout.flush
    Terminal.getch
    @term.clear
  end
  
  def printvalue(s)
    v, idx = @parser.expression(s, 0)
    return if v == Parser::UNKNOWN
    
    s = ' . '
    if v < 0x20
      s = '^' + (v + '@'.ord).chr + ' '
    elsif v >= 0x7e
      s = ' . '
    else
      s = "'" + v.chr + "'"
    end
    
    x = "%016X" % v
    spaced_hex = x.scan(/.{4}/).join(' ')
    o = "%024o" % v
    spaced_oct = o.scan(/.{4}/).join(' ')
    b = "%064b" % v
    spaced_bin = b.scan(/.{4}/).join(' ')
    
    msg = "d%10d  x#{spaced_hex}  o#{spaced_oct} #{s}\nb#{spaced_bin}" % v
    
    if @scriptingflag
      puts msg if @verbose
    else
      @display.clrmm
      @term.color(6)
      @term.locate(0, Display::BOTTOMLN)
      print msg
      $stdout.flush
      Terminal.getch
      @term.locate(0, Display::BOTTOMLN + 1)
      print " " * 80
      $stdout.flush
    end
  end
  
  def call_exec(line)
    return if line.length <= 1
    line = line[1..-1]
    
    begin
      if @scriptingflag
        eval(line)
      else
        @display.clrmm
        @term.color(7)
        @term.locate(0, Display::BOTTOMLN)
        eval(line)
        @term.color(4)
        @term.clrline
        print "[ Hit a key ]"
        $stdout.flush
        Terminal.getch
        @term.clear
        @display.repaint(@filemgr.filename)
      end
    rescue
      stderr("ruby eval() error.")
    end
  end
  
  def fedit
    stroke = false
    ch = ''
    @display.repsw = 0
    
    loop do
      @cp = @display.fpos
      @display.repaint(@filemgr.filename)
      @display.printdata
      @term.locate(@display.curx / 2 * 3 + 13 + (@display.curx & 1), @display.cury + 3)
      ch = Terminal.getch
      @display.clrmm
      @search.nff = true
      
      # エスケープシーケンス処理
      if ch == "\e"
        c2 = Terminal.getch
        c3 = Terminal.getch
        if c3 == 'A'
          ch = 'k'
        elsif c3 == 'B'
          ch = 'j'
        elsif c3 == 'C'
          ch = 'l'
        elsif c3 == 'D'
          ch = 'h'
        elsif c2 == '[' && c3 == '2'
          ch = 'i'
        end
      end
      
      # 検索コマンド
      if ch == 'n'
        pos = @search.searchnext(@display.fpos + 1, @memory.length)
        if pos && pos != false
          @display.jump(pos)
        elsif pos.nil?
          stdmm("Not found.")
        end
        next
      elsif ch == 'N'
        pos = @search.searchlast(@display.fpos - 1, @memory.length)
        if pos && pos != false
          @display.jump(pos)
        elsif pos.nil?
          stdmm("Not found.")
        end
        next
      
      # スクロールコマンド
      elsif ch == "\x02"  # Ctrl+B
        if @display.homeaddr >= 256
          @display.homeaddr -= 256
        else
          @display.homeaddr = 0
        end
        next
      elsif ch == "\x06"  # Ctrl+F
        @display.homeaddr += 256
        next
      elsif ch == "\x15"  # Ctrl+U
        if @display.homeaddr >= 128
          @display.homeaddr -= 128
        else
          @display.homeaddr = 0
        end
        next
      elsif ch == "\x04"  # Ctrl+D
        @display.homeaddr += 128
        next
      
      # カーソル移動
      elsif ch == '^'
        @display.curx = 0
        next
      elsif ch == '$'
        @display.curx = 30
        next
      elsif ch == 'j'
        if @display.cury < Display::LENONSCR / 16 - 1
          @display.cury += 1
        else
          @display.scrdown
        end
        next
      elsif ch == 'k'
        if @display.cury > 0
          @display.cury -= 1
        else
          @display.scrup
        end
        next
      elsif ch == 'h'
        if @display.curx > 0
          @display.curx -= 1
        else
          if @display.fpos != 0
            @display.curx = 31
            if @display.cury > 0
              @display.cury -= 1
            else
              @display.scrup
            end
          end
        end
        next
      elsif ch == 'l'
        @display.inccurx
        next
      
      # 表示モード切替
      elsif ch == "\x19"  # Ctrl+Y
        @display.utf8 = !@display.utf8
        @term.clear
        @display.repaint(@filemgr.filename)
        next
      elsif ch == "\x0c"  # Ctrl+L
        @term.clear
        @display.repsw = (@display.repsw + (@display.utf8 ? 1 : 0)) % 4
        @display.repaint(@filemgr.filename)
        next
      
      # ファイル操作
      elsif ch == 'Z'
        success, msg = @filemgr.writefile(@filemgr.filename)
        if success
          return true
        else
          stderr(msg)
          next
        end
      elsif ch == 'q'
        if @memory.lastchange
          stdmm("No write since last change. To overriding quit, use 'q!'.")
          next
        end
        return false
      
      # マーク操作
      elsif ch == 'M'
        disp_marks
        next
      elsif ch == 'm'
        ch = Terminal.getch.downcase
        if ch >= 'a' && ch <= 'z'
          @memory.mark[ch.ord - 'a'.ord] = @display.fpos
        end
        next
      
      # 検索
      elsif ch == '/'
        do_search
        next
      elsif ch == "'"
        ch = Terminal.getch.downcase
        if ch >= 'a' && ch <= 'z'
          mark_pos = @memory.mark[ch.ord - 'a'.ord]
          if mark_pos != MemoryBuffer::UNKNOWN
            @display.jump(mark_pos)
          end
        end
        next
      
      # ヤンク・ペースト
      elsif ch == 'p'
        y = @memory.yank.dup
        @memory.ovwmem(@display.fpos, y)
        @display.jump(@display.fpos + y.length)
        next
      elsif ch == 'P'
        y = @memory.yank.dup
        @memory.insmem(@display.fpos, y)
        @display.jump(@display.fpos + @memory.yank.length)
        next
      
      # 編集モード
      elsif ch == 'i'
        @display.insmod = !@display.insmod
        stroke = false
      elsif ch.match?(/[0-9a-fA-F]/)
        addr = @display.fpos
        c = ch.to_i(16)
        sh = (@display.curx & 1) == 0 ? 4 : 0
        mask = (@display.curx & 1) == 0 ? 0xf : 0xf0
        if @display.insmod
          if !stroke && addr < @memory.mem.length
            @memory.insmem(addr, [c << sh])
          else
            @memory.setmem(addr, @memory.readmem(addr) & mask | c << sh)
          end
          stroke = ((@display.curx & 1) == 0) ? !stroke : false
        else
          @memory.setmem(addr, @memory.readmem(addr) & mask | c << sh)
        end
        @display.inccurx
      elsif ch == 'x'
        @memory.delmem(@display.fpos, @display.fpos, false, method(:yankmem_dummy))
      elsif ch == ':'
        @display.disp_curpos
        f = commandln
        @display.erase_curpos
        return true if f == 1
        return false if f == 0
      end
    end
  end
  
  def yankmem_dummy(start, _end)
    # ダミーメソッド
  end
  
  def do_search
    @display.disp_curpos
    @term.locate(0, Display::BOTTOMLN)
    @term.color(7)
    Readline.pre_input_hook = proc { Readline.insert_text('/'); Readline.redisplay }
    
    s = @history.getln("", "search")
    searchsub(@parser.comment(s))
    @display.erase_curpos
  end
  
  def searchsub(line)
    if line.length > 2 && line[0..1] == '//'
      sm, idx = @parser.get_hexs(line, 2)
      searchhex(sm)
    elsif line.length > 1 && line[0] == '/'
      m, idx = @parser.get_restr(line, 1)
      searchstr(m)
    end
  end
  
  def searchstr(s)
    if s != ""
      @search.regexp = true
      @search.remem = s
      pos = @search.searchnext(@display.fpos, @memory.length)
      if pos && pos != false
        @display.jump(pos)
        return true
      end
    end
    false
  end
  
  def searchhex(sm)
    @search.remem = ''
    @search.regexp = false
    if sm && !sm.empty?
      @search.smem = sm
      pos = @search.searchnext(@display.fpos, @memory.length)
      if pos && pos != false
        @display.jump(pos)
        return true
      end
    end
    false
  end
  
  def commandln
    @term.locate(0, Display::BOTTOMLN)
    @term.color(7)
    Readline.pre_input_hook = proc { Readline.insert_text(''); Readline.redisplay }
    line = @history.getln(':', "command").lstrip
    commandline(line)
  end
  
  def commandline(line)
    begin
      commandline_(line)
    rescue
      stderr("Memory overflow.")
      return -1
    end
  end
  
  def commandline_(line)
    @cp = @display.fpos
    line = @parser.comment(line)
    return -1 if line == ''
    
    # 終了コマンド
    if line == 'q'
      if @memory.lastchange
        stderr("No write since last change. To overriding quit, use 'q!'.")
        return -1
      end
      return 0
    elsif line == 'q!'
      return 0
    elsif line == 'wq' || line == 'wq!'
      success, msg = @filemgr.writefile(@filemgr.filename)
      if success
        @memory.lastchange = false
        return 0
      else
        return -1
      end
    
    # ファイル書き込み
    elsif line[0] == 'w'
      if line.length >= 2
        s = line[1..-1].lstrip
        success, msg = @filemgr.writefile(s)
      else
        success, msg = @filemgr.writefile(@filemgr.filename)
        @memory.lastchange = false if success
      end
      if msg
        if success
          stdmm(msg)
        else
          stderr(msg)
        end
      end
      return -1
    
    # ファイル読み込み
    elsif line[0] == 'r'
      if line.length < 2
        success, msg = @filemgr.readfile(@filemgr.filename)
        if msg
          stdmm(msg)
        else
          stdmm("Original file read.")
        end
        return -1
      end
    
    # スクリプト実行
    elsif line[0] == 'T' || line[0] == 't'
      if line.length >= 2
        s = line[1..-1].lstrip
        @stack.push(@scriptingflag)
        @stack.push(@verbose)
        @verbose = line[0] == 'T'
        puts ""
        scripting(s)
        if @verbose
          stdmm("[ Hit any key ]")
          Terminal.getch
        end
        @verbose = @stack.pop
        @scriptingflag = @stack.pop
        @term.clear
        return -1
      else
        stderr("Specify script file name.")
        return -1
      end
    
    # 検索
    elsif line[0] == 'n'
      pos = @search.searchnext(@display.fpos + 1, @memory.length)
      @display.jump(pos) if pos && pos != false
      return -1
    elsif line[0] == 'N'
      pos = @search.searchlast(@display.fpos - 1, @memory.length)
      @display.jump(pos) if pos && pos != false
      return -1
    
    # 特殊コマンド
    elsif line[0] == '@'
      call_exec(line)
      return -1
    elsif line[0] == '!'
      invoke_shell(line[1..-1]) if line.length >= 2
      return -1
    elsif line[0] == '?'
      printvalue(line[1..-1])
      return -1
    elsif line[0] == '/'
      searchsub(line)
      return -1
    end
    
    # アドレス範囲コマンドのパース
    parse_range_command(line)
  end
  
  def parse_range_command(line)
    idx = @parser.skipspc(line, 0)
    
    x, idx = @parser.expression(line, idx)
    xf = false
    xf2 = false
    if x == Parser::UNKNOWN
      x = @display.fpos
    else
      xf = true
    end
    x2 = x
    
    idx = @parser.skipspc(line, idx)
    if idx < line.length && line[idx] == ','
      idx = @parser.skipspc(line, idx + 1)
      if idx < line.length && line[idx] == '*'
        idx = @parser.skipspc(line, idx + 1)
        t, idx = @parser.expression(line, idx)
        t = 1 if t == Parser::UNKNOWN
        x2 = x + t - 1
      else
        t, idx = @parser.expression(line, idx)
        if t == Parser::UNKNOWN
          x2 = x
        else
          x2 = t
          xf2 = true
        end
      end
    else
      x2 = x
    end
    
    x2 = x if x2 < x
    
    idx = @parser.skipspc(line, idx)
    
    if idx == line.length
      @display.jump(x)
      return -1
    end
    
    # 各種コマンドの処理
    execute_command(line, idx, x, x2, xf, xf2)
  end
  
  def execute_command(line, idx, x, x2, xf, xf2)
    # yank
    if idx < line.length && line[idx] == 'y'
      idx += 1
      if !xf && !xf2
        m, idx = @parser.get_str_or_hexs(line, idx)
        @memory.yank = m.dup
      else
        cnt = @memory.yankmem(x, x2)
      end
      
      stdmm("#{@memory.yank.length} bytes yanked.")
      return -1
    end
    
    # paste
    if idx < line.length && line[idx] == 'p'
      y = @memory.yank.dup
      @memory.ovwmem(x, y)
      @display.jump(x + y.length)
      return -1
    end
    
    if idx < line.length && line[idx] == 'P'
      y = @memory.yank.dup
      @memory.insmem(x, y)
      @display.jump(x + @memory.yank.length)
      return -1
    end
    
    # mark
    if idx + 1 < line.length && line[idx] == 'm'
      if line[idx + 1] >= 'a' && line[idx + 1] <= 'z'
        @memory.mark[line[idx + 1].ord - 'a'.ord] = x
      end
      return -1
    end
    
    # read file
    if idx < line.length && (line[idx] == 'r' || line[idx] == 'R')
      ch = line[idx]
      idx += 1
      if idx >= line.length
        stderr("File name not specified.")
        return -1
      end
      fn = line[idx..-1].lstrip
      if fn == ""
        stderr("File name not specified.")
      else
        begin
          f = File.open(fn, "rb")
          data = f.read.bytes
          f.close
        rescue
          data = []
          stderr("File read error.")
        end
      end
      
      if ch == 'r'
        @memory.ovwmem(x, data)
      elsif ch == 'R'
        @memory.insmem(x, data)
      end
      
      @display.jump(x + data.length)
      return -1
    end
    
    if idx < line.length
      ch = line[idx]
    else
      ch = ''
    end
    
    # delete
    if ch == 'd'
      if @memory.delmem(x, x2, true, @memory.method(:yankmem))
        stdmm("#{x2 - x + 1} bytes deleted.")
        @display.jump(x)
      end
      return -1
    
    # write file
    elsif ch == 'w'
      idx += 1
      fn = line[idx..-1].lstrip
      success, msg = @filemgr.wrtfile(x, x2, fn)
      stderr(msg) if msg
      return -1
    
    # substitute
    elsif ch == 's'
      scommand(x, x2, xf, xf2, line, idx + 1)
      return -1
    end
    
    # not
    if idx < line.length && line[idx] == '~'
      openot(x, x2)
      @display.jump(x2 + 1)
      return -1
    end
    
    # その他の複雑なコマンド
    if idx < line.length && "fIivCc&|^<>".include?(line[idx])
      return execute_complex_command(line, idx, x, x2, xf, xf2)
    end
    
    stderr("Unrecognized command.")
    -1
  end
  
  def execute_complex_command(line, idx, x, x2, xf, xf2)
    ch = line[idx]
    idx += 1
    
    # シフト・ローテート
    if ch == '<' || ch == '>'
      multibyte = false
      if idx < line.length && line[idx] == ch
        idx += 1
        multibyte = true
      end
      
      times, idx = @parser.expression(line, idx)
      times = 1 if times == Parser::UNKNOWN
      
      if idx < line.length && line[idx] == ','
        bit, idx = @parser.expression(line, idx + 1)
      else
        bit = Parser::UNKNOWN
      end
      
      shift_rotate(x, x2, times, bit, multibyte, ch)
      return -1
    end
    
    # insert/Insert
    if ch == 'i' || ch == 'I'
      idx = @parser.skipspc(line, idx)
      if idx < line.length && line[idx] == '/'
        m, idx = @parser.get_str(line, idx + 1)
      else
        m, idx = @parser.get_hexs(line, idx)
      end
      
      if idx < line.length && line[idx] == '*'
        idx += 1
        length, idx = @parser.expression(line, idx)
      else
        length = 1
      end
      
      # fill mode for 'i' with range
      if ch == 'i' && xf2
        if m.length > 0
          data = m * ((x2 - x + 1) / m.length) + m[0...((x2 - x + 1) % m.length)]
          @memory.ovwmem(x, data)
          stdmm("#{data.length} bytes filled.")
          @display.jump(x + data.length)
        else
          stderr("Invalid syntax.")
        end
        return -1
      end
      
      if ch == 'I' && xf2
        stderr("Invalid syntax.")
        return -1
      end
      
      data = m * length
      if ch == 'i'
        @memory.ovwmem(x, data)
        stdmm("#{data.length} bytes overwritten.")
      else
        @memory.insmem(x, data)
        stdmm("#{data.length} bytes inserted.")
      end
      
      @display.jump(x + data.length)
      return -1
    end
    
    # 残りのコマンドは第3引数が必要
    x3, idx = @parser.expression(line, idx)
    if x3 == Parser::UNKNOWN
      stderr("Invalid parameter.")
      return -1
    end
    
    # copy/Copy
    if ch == 'c'
      @memory.yankmem(x, x2)
      m = @memory.redmem(x, x2)
      @memory.ovwmem(x3, m)
      stdmm("#{x2 - x + 1} bytes copied.")
      @display.jump(x3 + (x2 - x + 1))
      return -1
    elsif ch == 'C'
      m = @memory.redmem(x, x2)
      @memory.yankmem(x, x2)
      @memory.insmem(x3, m)
      stdmm("#{x2 - x + 1} bytes inserted.")
      @display.jump(x3 + m.length)
      return -1
    
    # move
    elsif ch == 'v'
      xp = movmem(x, x2, x3)
      @display.jump(xp)
      return -1
    
    # ビット演算
    elsif ch == '&'
      opeand(x, x2, x3)
      @display.jump(x2 + 1)
      return -1
    elsif ch == '|'
      opeor(x, x2, x3)
      @display.jump(x2 + 1)
      return -1
    elsif ch == '^'
      opexor(x, x2, x3)
      @display.jump(x2 + 1)
      return -1
    end
    
    -1
  end
  
  # 各種操作メソッド
  def opeand(x, x2, x3)
    (x..x2).each do |i|
      @memory.setmem(i, @memory.readmem(i) & (x3 & 0xff))
    end
    stdmm("#{x2 - x + 1} bytes anded.")
  end
  
  def opeor(x, x2, x3)
    (x..x2).each do |i|
      @memory.setmem(i, @memory.readmem(i) | (x3 & 0xff))
    end
    stdmm("#{x2 - x + 1} bytes ored.")
  end
  
  def opexor(x, x2, x3)
    (x..x2).each do |i|
      @memory.setmem(i, @memory.readmem(i) ^ (x3 & 0xff))
    end
    stdmm("#{x2 - x + 1} bytes xored.")
  end
  
  def openot(x, x2)
    (x..x2).each do |i|
      @memory.setmem(i, (~@memory.readmem(i)) & 0xff)
    end
    stdmm("#{x2 - x + 1} bytes noted.")
  end
  
  def movmem(start, _end, dest)
    return _end + 1 if start <= dest && dest <= _end
    l = @memory.mem.length
    return dest if start >= l
    m = @memory.redmem(start, _end)
    @memory.delmem(start, _end, true, @memory.method(:yankmem))
    if dest > l
      @memory.ovwmem(dest, m)
      xp = dest + m.length
    else
      if dest > start
        @memory.insmem(dest - (_end - start + 1), m)
        xp = dest - (_end - start) + m.length - 1
      else
        @memory.insmem(dest, m)
        xp = dest + m.length
      end
    end
    stdmm("#{_end - start + 1} bytes moved.")
    xp
  end
  
  def shift_rotate(x, x2, times, bit, multibyte, direction)
    times.times do
      if !multibyte
        if bit != 0 && bit != 1
          if direction == '<'
            left_rotate_byte(x, x2)
          else
            right_rotate_byte(x, x2)
          end
        else
          if direction == '<'
            left_shift_byte(x, x2, bit & 1)
          else
            right_shift_byte(x, x2, bit & 1)
          end
        end
      else
        if bit != 0 && bit != 1
          if direction == '<'
            left_rotate_multibyte(x, x2)
          else
            right_rotate_multibyte(x, x2)
          end
        else
          if direction == '<'
            left_shift_multibyte(x, x2, bit & 1)
          else
            right_shift_multibyte(x, x2, bit & 1)
          end
        end
      end
    end
  end
  
  def left_shift_byte(x, x2, c)
    (x..x2).each do |i|
      @memory.setmem(i, (@memory.readmem(i) << 1) | (c & 1))
    end
  end
  
  def right_shift_byte(x, x2, c)
    (x..x2).each do |i|
      @memory.setmem(i, (@memory.readmem(i) >> 1) | ((c & 1) << 7))
    end
  end
  
  def left_rotate_byte(x, x2)
    (x..x2).each do |i|
      m = @memory.readmem(i)
      c = (m & 0x80) >> 7
      @memory.setmem(i, (m << 1) | c)
    end
  end
  
  def right_rotate_byte(x, x2)
    (x..x2).each do |i|
      m = @memory.readmem(i)
      c = (m & 0x01) << 7
      @memory.setmem(i, (m >> 1) | c)
    end
  end
  
  def get_multibyte_value(x, x2)
    v = 0
    x2.downto(x) do |i|
      v = (v << 8) | @memory.readmem(i)
    end
    v
  end
  
  def put_multibyte_value(x, x2, v)
    (x..x2).each do |i|
      @memory.setmem(i, v & 0xff)
      v >>= 8
    end
  end
  
  def left_shift_multibyte(x, x2, c)
    v = get_multibyte_value(x, x2)
    put_multibyte_value(x, x2, (v << 1) | c)
  end
  
  def right_shift_multibyte(x, x2, c)
    v = get_multibyte_value(x, x2)
    put_multibyte_value(x, x2, (v >> 1) | (c << ((x2 - x) * 8 + 7)))
  end
  
  def left_rotate_multibyte(x, x2)
    v = get_multibyte_value(x, x2)
    c = (v & (1 << ((x2 - x) * 8 + 7))) != 0 ? 1 : 0
    put_multibyte_value(x, x2, (v << 1) | c)
  end
  
  def right_rotate_multibyte(x, x2)
    v = get_multibyte_value(x, x2)
    c = (v & 0x1) != 0 ? 1 : 0
    put_multibyte_value(x, x2, (v >> 1) | (c << ((x2 - x) * 8 + 7)))
  end
  
  def scommand(start, _end, xf, xf2, line, idx)
    @search.nff = false
    pos = @display.fpos
    
    idx = @parser.skipspc(line, idx)
    if !xf && !xf2
      start = 0
      _end = @memory.mem.length - 1
    end
    
    m = ''
    if idx < line.length && line[idx] == '/'
      idx += 1
      if idx < line.length && line[idx] != '/'
        m, idx = @parser.get_restr(line, idx)
        @search.regexp = true
        @search.remem = m
        @search.span = m.length
      elsif idx < line.length && line[idx] == '/'
        @search.smem, idx = @parser.get_hexs(line, idx + 1)
        @search.regexp = false
        @search.remem = ''
        @search.span = @search.smem.length
      else
        stderr("Invalid syntax.")
        return
      end
    end
    
    if @search.span == 0
      stderr("Specify search object.")
      return
    end
    
    n, idx = @parser.get_str_or_hexs(line, idx)
    
    i = start
    cnt = 0
    @display.jump(i)
    
    loop do
      f = searchnextnoloop(@display.fpos)
      
      i = @display.fpos
      
      if f < 0
        return
      elsif i <= _end && f == 1
        @memory.delmem(i, i + @search.span - 1, false, @memory.method(:yankmem))
        @memory.insmem(i, n)
        pos = i + n.length
        cnt += 1
        i = pos
        @display.jump(i)
      else
        @display.jump(pos)
        stdmm("  #{cnt} times replaced.")
        return
      end
    end
  end
  
  def searchnextnoloop(fp)
    cur_pos = fp
    
    return 0 if !@search.regexp && @search.smem.empty?
    
    loop do
      if @search.regexp
        f = @search.hitre(cur_pos)
      else
        f = @search.hit(cur_pos)
      end
      
      if f == 1
        @display.jump(cur_pos)
        return 1
      elsif f < 0
        return -1
      end
      
      cur_pos += 1
      
      if cur_pos >= @memory.mem.length
        @display.jump(@memory.mem.length)
        return 0
      end
    end
  end
  
  def scripting(scriptfile)
    begin
      f = File.open(scriptfile, "rt")
    rescue
      stderr("Script file open error.")
      return false
    end
    
    flag = -1
    @scriptingflag = true
    
    f.each_line do |line|
      line = line.strip
      next if line.empty?
      
      puts line if @verbose
      flag = commandline(line)
      if flag == 0
        f.close
        return 0
      elsif flag == 1
        f.close
        return 1
      end
    end
    
    f.close
    0
  end
end

# メイン関数
def main
  options = {}
  OptionParser.new do |opts|
    opts.banner = "Usage: bi.rb [options] file"
    
    opts.on("-s", "--script SCRIPT", "bi script file") do |s|
      options[:script] = s
    end
    
    opts.on("-t", "--termcolor COLOR", "background color of terminal. default is 'black' the others are white.") do |t|
      options[:termcolor] = t
    end
    
    opts.on("-v", "--verbose", "verbose when processing script") do
      options[:verbose] = true
    end
    
    opts.on("-w", "--write", "write file when exiting script") do
      options[:write] = true
    end
  end.parse!
  
  if ARGV.empty?
    $stderr.puts "Error: file argument is required"
    exit 1
  end
  
  filename = ARGV[0]
  
  # エディタの初期化
  editor = BiEditor.new(options[:termcolor] || 'black')
  editor.filemgr.filename = filename
  editor.verbose = options[:verbose] || false
  
  # 画面クリア(スクリプトモード以外)
  if !options[:script]
    editor.term.clear
  else
    editor.scriptingflag = true
  end
  
  # ファイル読み込み
  success, msg = editor.filemgr.readfile(filename)
  if !success
    $stderr.puts msg
    return
  elsif msg
    editor.stdmm(msg)
  end
  
  # スクリプト実行またはインタラクティブモード
  if options[:script]
    begin
      f = editor.scripting(options[:script])
      if options[:write] && editor.memory.lastchange
        editor.filemgr.writefile(filename)
      end
    rescue
      editor.filemgr.writefile("file.save")
      editor.stderr("Some error occured. memory saved to file.save.")
    end
  else
    begin
      editor.fedit
    rescue
      editor.filemgr.writefile("file.save")
      editor.stderr("Some error occured. memory saved to file.save.")
    end
  end
  
  # 終了処理
  editor.term.color(7)
  editor.term.dispcursor
  editor.term.locate(0, 23)
end

if __FILE__ == $0
  main
end
