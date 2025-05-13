#!/usr/bin/python3
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
ESC='\033['
LENONSCR=(20*16)
BOTTOMLN=23
RELEN=128
UNKNOWN=0xffffffffffffffffffffffffffffffff
mem=[]
yank=[]
coltab=[0,1,4,5,2,6,3,7]
filename=""
lastchange=False
modified=False
newfile=False
homeaddr=0
utf8=False
insmod=False
curx=0
cury=0
mark=[UNKNOWN] * 26
smem=[]
regexp=False
repsw=0
remem=''
span=0
nff=True
verbose=False
scriptingflag=False
stack=[]
cp=0
histories = {
    'command': [],
    'search': []
}


def printhexs(s):
    for i,b in enumerate(s):
        print(f"s[{i}]: {ord(b):02x} ",end='',flush=True)

def escnocursor():
    print(f"{ESC}?25l",end='',flush=True)
    return

def escdispcursor():
    print(f"{ESC}?25h",end='',flush=True)
    return

def escup(n=1):
    print(f"{ESC}{n}A",end='')

def escdown(n=1):
    print(f"{ESC}{n}B",end='')

def escright(n=1):
    print(f"{ESC}{n}C",end='')

def escleft(n=1):
    print(f"{ESC}{n}D",end='',flush=True)

def esclocate(x=0,y=0):
    print(f"{ESC}{y+1};{x+1}H",end='',flush=True)

def escscrollup(n=1):
    print(f"{ESC}{n}S",end='')

def escscrolldown(n=1):
    print(f"{ESC}{n}T",end='')

def escclear():
    print(f"{ESC}2J",end='',flush=True)
    esclocate()

def escclraftcur():
    print(f"{ESC}0J",end='',flush=True)

def escclrline():
    print(f"{ESC}2K",end='',flush=True)

def esccolor(col1=7,col2=0):
    print(f"{ESC}3{coltab[col1]}m{ESC}4{coltab[col2]}m",end='',flush=True)

def escresetcolor():
    print(f"{ESC}0m",end='')

def getch():
    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    tty.setraw(sys.stdin.fileno())
    ch = sys.stdin.read(1)
    termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
    return ch

def putch(c):
    print(c,end='',flush=True)

def get_history_list():
    """現在の履歴をリストとして取得"""
    return [readline.get_history_item(i) for i in range(1, readline.get_current_history_length() + 1)]

def set_history_list(mode):
    """履歴をリストから設定"""
    history_items=histories[mode]
    readline.clear_history()
    for item in history_items:
        readline.add_history(item)

def getln(s="",mode="command"):
    mode="search" if mode=="search" else "command"
    histories[mode]  # 空でも準備しておく
    set_history_list(mode)
    try:
        user_input = input(s)
    except:
        user_input= ""

    histories[mode] = get_history_list()

    return user_input

def skipspc(s,idx):
    while idx<len(s):
        if s[idx]==' ':
            idx+=1
        else:
            break
    return idx

def print_title():
    global filename,modified,insmod,mem,repsw,utf8
    esclocate(0,0)
    esccolor(6)
    print(f"bi version 3.4.4 by T.Maekawa                   utf8mode:{"off" if not utf8 else repsw}     {"insert   " if insmod else "overwrite"}   ")
    esccolor(5)
    print(f"file:[{filename:<35}] length:{len(mem)} bytes [{("not " if not modified else "")+"modified"}]    ")

def printchar(a):
    global utf8
    if a>=len(mem):
        print("~",end='',flush=True)
        return 1
    if utf8:
        if mem[a]<0x80 or 0x80<=mem[a]<=0xbf or 0xf8<=mem[a]<=0xff:
            print(chr(mem[a]&0xff) if 0x20<=mem[a]<=0x7e else '.',end='')
            return 1
        elif 0xc0<=mem[a]<=0xdf:
            m=[readmem(a+repsw),readmem(a+1+repsw)]
            try:
                ch=bytes(m).decode('utf-8')
                print(f"{ch}",end='',flush=True)
                return 2 
            except:
                print(".",end='')
                return 1
        elif 0xe0<=mem[a]<=0xef:
            m=[readmem(a+repsw),readmem(a+1+repsw),readmem(a+2+repsw)]
            try:
                ch=bytes(m).decode('utf-8')
                print(f"{ch} ",end='',flush=True)
                return 3
            except:
                print(".",end='')
                return 1
        elif 0xf0<=mem[a]<=0xf7:
            m=[readmem(a+repsw),readmem(a+1+repsw),readmem(a+2+repsw),readmem(a+3+repsw)]
            try:
                ch=bytes(m).decode('utf-8')
                print(f"{ch}  ",end='',flush=True)
                return 4
            except:
                print(".",end='')
                return 1

    else:
        print(chr(mem[a]&0xff) if 0x20<=mem[a]<=0x7e else '.',end='')
        return 1

def repaint():
    print_title()
    escnocursor()
    esclocate(0,2)
    esccolor(4)
    print("OFFSET       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F 0123456789ABCDEF ")
    esccolor(7)
    addr=homeaddr
    for y in range(0x14):
        esccolor(5)
        esclocate(0,3+y)
        print(f"{(addr+y*16)&0xffffffffffff:012X} ",end='')
        esccolor(7)
        for i in range(16):
            a=y*16+i+addr
            print(f"~~ " if a>=len(mem) else f"{mem[a]&0xff:02X} ",end='')
        esccolor(6)
        a=y*16+addr
        by=0
        while by<16:
            c=printchar(a)
            a+=c
            by+=c
        print("  ",end='',flush=True)
    esccolor(0)
    escdispcursor()

def insmem(start,mem2):
    global mem,lastchange,modified
    if start>=len(mem):
        for i in range(start-len(mem)):
            mem+=[0]
        mem=mem+mem2
        modified=True
        lastchange=True
        return

    mem1=[]
    mem3=[]
    for j in range(start):
        mem1+=[mem[j]&0xff]
    for j in range(len(mem)-start):
        mem3+=[mem[start+j]&0xff]
    mem=mem1+mem2+mem3
    modified=True
    lastchange=True

def delmem(start,end,yf):
    global yank,mem,modified,lastchange
    length=end-start+1
    if length<=0 or start>=len(mem):
        stderr("Invalid range.")
        return
    if yf:
        yankmem(start,end)

    mem1=[]
    mem2=[]
    for j in range(start):
        mem1+=[mem[j]&0xff]
    for j in range(end+1,len(mem)):
        mem2+=[mem[j]&0xff]
    mem=mem1+mem2
    lastchange=True
    modified=True

def yankmem(start,end):
    global yank,mem
    length=end-start+1
    if length<=0 or start>=len(mem):
        stderr("Invalid range.")
        return
    yank=[]
    cnt=0
    for j in range(start,end+1):
        if j<len(mem):
            cnt+=1
            yank+=[mem[j]&0xff]

    stdmm(f"{cnt} bytes yanked.")

def ovwmem(start,mem0):
    global mem,modified,lastchange

    if mem0==[]:
        return

    if start+len(mem0)>=len(mem):
        for j in range(start+len(mem0)-len(mem)):
            mem+=[0]

    for j in range(len(mem0)):
        if j>=len(mem):
            mem+=[mem0[j]&0xff]
        else:
            mem[start+j]=mem0[j]&0xff
    lastchange=True
    modified=True

def redmem(start,end):
    global mem
    m=[]
    for i in range(start,end+1):
        if len(mem)>i:
            m+=[mem[i]&0xff]
        else:
            m+=[0]
    return m

def cpymem(start,end,dest):
    m=redmem(start,end)
    ovwmem(dest,m)

def movmem(start,end,dest):
    global mem
    if start<=dest<=end:
        return end+1
    l=len(mem)
    if start>=l:
        return dest
    m=redmem(start,end)
    delmem(start,end,True)
    if dest>l:
        ovwmem(dest,m)
        xp=dest+len(m)
    else:
        if dest>start:
            insmem(dest-(end-start+1),m)
            xp= dest-(end-start)+len(m)-1
        else:
            insmem(dest,m)
            xp= dest+len(m)
    stdmm(f"{end-start+1} bytes moved.")
    return xp

def scrup():
    global homeaddr
    if homeaddr>=16:
        homeaddr-=16

def scrdown():
    global homeaddr
    homeaddr+=16

def fpos():
    global homeaddr,curx,cury
    return(homeaddr+curx//2+cury*16)

def inccurx():
    global curx,cury
    if curx<31:
        curx+=1
    else:
        curx=0
        if cury<19:
            cury+=1
        else:
            scrdown()

def readmem(addr):
    global mem
    if addr>=len(mem):
        return 0
    return (mem[addr]&0xff)

def setmem(addr,data):
    global mem,modified,lastchange

    if addr>=len(mem):
        for i in range(addr-len(mem)+1):
            mem+=[0]

    if isinstance(data,int) and 0<=data<=255:
        mem[addr]=data
    else:
        mem[addr]=0

    modified=True
    lastchange=True

def clrmm():
    esclocate(0,BOTTOMLN)
    esccolor(6)
    escclrline()

def stdmm(s):
    global scriptingflag,verbose
    if scriptingflag:
        if verbose:
            print(s)
    else:
        clrmm()
        esccolor(4)
        esclocate(0,BOTTOMLN)
        print(s,end='',flush=True)

def stderr(s):
    global scriptingflag,verbose
    if scriptingflag:
        print(s,file=sys.stderr)
    else:
        clrmm()
        esccolor(3)
        esclocate(0,BOTTOMLN)
        print(s)


def jump(addr):
    global homeaddr,curx,cury
    if addr < homeaddr or addr>=homeaddr+LENONSCR:
        homeaddr=addr & ~(0xff)
    i=addr-homeaddr
    curx=(i&0xf)*2
    cury=(i//16)

def disp_marks():
    j=0
    esclocate(0,BOTTOMLN)
    esccolor(7)
    for i in 'abcdefghijklmnopqrstuvwxyz':
        m=mark[j]
        if m==UNKNOWN:
            print(f"{i} = unknown         ",end='')
        else:
            print(f"{i} = {mark[j]:012X}    ",end='')
        j+=1
        if j%3==0:
            print()
    esccolor(4)
    print("[ hit any key ]")
    getch()
    escclear()

def invoke_shell(line):
    esccolor(7)
    print()
    os.system(line.lstrip())
    esccolor(4)
    print("[ Hit any key to return ]",end='',flush=True)
    getch()
    escclear()

def expression(s,idx):
    x,idx=get_value(s,idx)
    if len(s)>idx and x!=UNKNOWN and s[idx]=='+':
        y,idx=get_value(s,idx+1)
        x=x+y
    elif len(s)>idx and x!=UNKNOWN and s[idx]=='-':
        y,idx=get_value(s,idx+1)
        x=x-y
        if x<0:
            x=0
    return x,idx

def get_value(s,idx):
    if idx>=len(s):
        return UNKNOWN,idx
    idx=skipspc(s,idx)
    ch=s[idx]
    if ch=='$':
        idx+=1
        if len(mem)!=0:
            v=len(mem)-1
        else:
            v=0
    elif ch=='{':
        idx+=1
        u=''
        while idx<len(s):
            if s[idx]=='}':
                idx+=1
                break
            u+=s[idx]
            idx+=1
        else:
            stderr("Invalid eval expression.")
            return UNKNOWN,idx

        try:
            v=int(eval(u))
        except:
            stderr("Invalid eval expression.")
            return UNKNOWN,idx

    elif ch=='.':
        idx+=1
        v=fpos()
    elif ch=='\'' and len(s)>idx+1 and 'a'<=s[idx+1]<='z':
        idx+=1
        v=mark[ord(s[idx])-ord('a')]
        if v==UNKNOWN:
            stderr("Unknown mark.")
            return UNKNOWN,idx-1
        else:
            idx+=1
    elif idx<len(s) and s[idx] in '0123456789abcdefABCDEF':
        x=0
        while idx<len(s) and s[idx] in '0123456789abcdefABCDEF':
            x=16*x+int("0x"+s[idx],16)
            idx+=1
        v=x
    elif ch=='%':
        x=0
        idx+=1
        while idx<len(s) and s[idx] in '0123456789':
            x=10*x+int(s[idx])
            idx+=1
        v=x
    else:
        v=UNKNOWN
    if v<0:
        v=0
    return v,idx

def searchnextnoloop(fp):
    global smem,nff
    cur_pos=fp

    if regexp==False and not smem:
        return False
    while True:
        if regexp:
            f=hitre(cur_pos)
        else:
            f=hit(cur_pos)

        if f:
            jump(cur_pos)
            return True

        cur_pos+=1

        if cur_pos>=len(mem):
            jump(len(mem))
            return False


def scommand(start,end,xf,xf2,line,idx):
    global span,nff,regexp,remem,smem
    nff=False
    pos=fpos()

    idx=skipspc(line,idx)
    if not xf and not xf2:
        start=0
        end=len(mem)-1
    f=False

    m=''
    hs=[]
    re_=False
    if idx<len(line) and line[idx]=='/':
        idx+=1
        f=True
        if idx<len(line) and line[idx]!='/':
            m,idx=get_restr(line,idx)
            regexp=True
            remem=m
            span=len(m)
        elif idx<len(line) and line[idx]=='/':
            smem,idx=get_hexs(line,idx+1)
            regexp=False
            remem=''
            span=len(smem)
        else:
            stderr(f"Invalid syntax.")
            return

    if span==0:
        stderr(f"Specify search object.")
        return

    n,idx=get_str_or_hexs(line,idx)

    i=start
    cnt=0
    jump(i)

    while True:

        f=searchnextnoloop(fpos())

        i=fpos()

        if i<=end and f==True:
            delmem(i,i+span-1,False)
            insmem(i,n)
            pos=i+len(n)
            cnt+=1
            i=pos
            jump(i)
        else:
            jump(pos)
            stdmm(f"  {cnt} times replaced.")
            return

def opeand(x,x2,x3):
    for i in range(x,x2+1):
        setmem(i,readmem(i)&(x3&0xff))
    stdmm(f"{x2-x+1} bytes anded.")
    return
            
def opeor(x,x2,x3):
    for i in range(x,x2+1):
        setmem(i,readmem(i)|(x3&0xff))
    stdmm(f"{x2-x+1} bytes ored.")
    return
            
def opexor(x,x2,x3):
    for i in range(x,x2+1):
        setmem(i,readmem(i)^(x3&0xff))
    stdmm(f"{x2-x+1} bytes xored.")
    return
            
def openot(x,x2):
    for i in range(x,x2+1):
        setmem(i,(~(readmem(i))&0xff))
    stdmm(f"{x2-x+1} bytes noted.")
    return
            
def hitre(addr):
    global span, remem, mem

    if not remem:
        return False

    span = 0
    m = []

    if addr < len(mem) - RELEN:
        m = mem[addr:addr + RELEN]
    else:
        m = mem[addr:]

    byte_data = bytes(m)
    try:
        ms = byte_data.decode('utf-8', errors='replace')
    except:
        stderr("Unicode decode error")
        return False

    try:
        f = re.match(remem, ms)
    except:
        stderr("Bad regular expression.")
        return False

    if f:
        start, end = f.span()
        span = end - start
        matched_str = ms[start:end]
        try:
            matched_bytes = matched_str.encode('utf-8')
        except:
            stderr("Unicode encode error.")
            return False

        span=len(matched_bytes)
        return True
    else:
        return False

def hit(addr):
    global smem,mem
    for i in range(len(smem)):
        if addr+i<len(mem) and mem[addr+i]==smem[i]:
            continue
        else:
            return False
    return True

def searchnext(fp):
    global smem,nff
    curpos=fp
    start=fp
    if regexp==False and not smem:
        return False
    while True:
        if regexp:
            f=hitre(curpos)
        else:
            f=hit(curpos)

        if f:
            jump(curpos)
            return True

        curpos+=1

        if curpos>=len(mem):
            if nff:
                stdmm("Search reached to bottom, continuing from top.")
            curpos=0
            esccolor(0)

        if curpos==start:
            if nff:
                stdmm("Not found.")
            return False

def searchlast(fp):
    curpos=fp
    start=fp
    if regexp==False and not smem:
        return False
    while True:
        if regexp:
            f=hitre(curpos)
        else:
            f=hit(curpos)

        if f:
            jump(curpos)
            return True

        curpos-=1
        if curpos<0:
            stdmm("Search reached to top, continuing from bottom.")
            esccolor(0)
            curpos=len(mem)-1

        if curpos==start:
            stdmm("Not found.")
            return False

def get_restr(s, idx):
    m = ''
    while idx < len(s):
        if s[idx] == '/':
            break

        if idx+1<len(s) and s[idx:idx+2]=="\\\\":
            m+='\\\\'
            idx+=2
        elif idx+1<len(s) and s[idx:idx+2]==chr(0x5c)+'/':
            m+='/'
            idx+=2
        elif s[idx]=='\\' and len(s)-1==idx:
            idx+=1
            break
        else:
            m+=s[idx]
            idx+=1
    return m, idx

def searchstr(s):
    global regexp,remem
    if s!="":
        regexp=True
        remem=s
        return(searchnext(fpos()))
    return False


def searchsub(line):
    if len(line)>2 and line[0:2]=='//':
        sm,idx=get_hexs(line,2)
        return searchhex(sm)
    elif len(line)>1 and line[0]=='/':
        m,idx=get_restr(line,1)
        return searchstr(m)

def search_pre_input_hook():
    readline.insert_text('/')
    readline.redisplay()

def no_pre_input_hook():
    readline.insert_text('')
    readline.redisplay()

def search():
    disp_curpos()
    esclocate(0,BOTTOMLN)
    esccolor(7)
    readline.set_pre_input_hook(search_pre_input_hook)
    
    s=getln("","search")
    searchsub(comment(s))
    erase_curpos()

def get_hexs(s,idx):
    m=[]
    while idx<len(s):
        v,idx=expression(s,idx)
        if v==UNKNOWN:
            break
        m+=[v&0xff]
    return m,idx

def searchhex(sm):
    global smem,remem,regexp
    remem=''
    regexp=False
    if sm:
        smem=sm
        return(searchnext(fpos()))
    return False

def comment(s):
    idx=0
    m = ''
    while idx < len(s):
        if s[idx] == '#':
            break

        if idx+1<len(s) and s[idx:idx+2]==chr(0x5c)+'#':
            m+='#'
            idx+=2

        if idx+1<len(s) and s[idx:idx+2]==chr(0x5c)+'n':
            m+='\n'
            idx+=2
        else:
            m+=s[idx]
            idx+=1

    return m

def scripting(scriptfile):
    global scriptingflag,verbose
    try:
        f=open(scriptfile,"rt")
    except:
        stderr("Script file open error.")
        return False
    line=f.readline().strip()
    flag=-1
    scriptingflag=True
    while line:
        if verbose:
            print(line)
        flag=commandline(line)
        if flag==0:
            f.close()
            return 0
        elif flag==1:
            f.close()
            return 1
        line=f.readline().strip()
    f.close()
    return 0

def left_shift_byte(x,x2,c):
    for i in range(x,x2+1):
        setmem(i,(readmem(i)<<1)|(c&1))
    return

def right_shift_byte(x,x2,c):
    for i in range(x,x2+1):
        setmem(i,(readmem(i)>>1)|((c&1)<<7))
    return

def left_rotate_byte(x,x2):
    for i in range(x,x2+1):
        m=readmem(i)
        c=(m&0x80)>>7
        setmem(i,(m<<1)|c)
    return

def right_rotate_byte(x,x2):
    for i in range(x,x2+1):
        m=readmem(i)
        c=(m&0x01)<<7
        setmem(i,(m>>1)|c)
    return

def get_multibyte_value(x,x2):
    v=0
    for i in range(x2,x-1,-1):
        v=(v<<8)|readmem(i)
    return v

def put_multibyte_value(x,x2,v):
    for i in range(x,x2+1):
        setmem(i,v&0xff)
        v>>=8
    return
    
def left_shift_multibyte(x,x2,c):
    v=get_multibyte_value(x,x2)
    put_multibyte_value(x,x2,(v<<1)|c)
    return

def right_shift_multibyte(x,x2,c):
    v=get_multibyte_value(x,x2)
    put_multibyte_value(x,x2,(v>>1)|(c<<((x2-x)*8+7)))
    return

def left_rotate_multibyte(x,x2):
    v=get_multibyte_value(x,x2)
    c=1 if v&(1<<((x2-x)*8+7)) else 0
    put_multibyte_value(x,x2,(v<<1)|c)
    return

def right_rotate_multibyte(x,x2):
    v=get_multibyte_value(x,x2)
    c=1 if v&0x1 else 0
    put_multibyte_value(x,x2,(v>>1)|(c<<((x2-x)*8+7)))
    return

def shift_rotate(x,x2,times,bit,multibyte,direction):
    for i in range(times):
        if not multibyte:
            if bit!=0 and bit!=1:
                if direction=='<':
                    left_rotate_byte(x,x2)
                else:
                    right_rotate_byte(x,x2)
            else:
                if direction=='<':
                    left_shift_byte(x,x2,bit&1)
                else:
                    right_shift_byte(x,x2,bit&1)
        else:
            if bit!=0 and bit!=1:
                if direction=='<':
                    left_rotate_multibyte(x,x2)
                else:
                    right_rotate_multibyte(x,x2)
            else:
                if direction=='<':
                    left_shift_multibyte(x,x2,bit&1)
                else:
                    right_shift_multibyte(x,x2,bit&1)
    return

def get_str_or_hexs(line,idx):
    idx=skipspc(line,idx)
    if idx<len(line) and line[idx]=='/':
        idx+=1
        if idx<len(line) and line[idx]=='/':
            m,idx=get_hexs(line,idx+1)
        else:
            s,idx=get_restr(line,idx)
            try:
                bseq=s.encode('utf-8')
            except:
                stderr("Unicode encode error.")
                return [],idx
            m=list(bseq)
    else:
        m=[]
    return m,idx

def get_str(line,idx):
    s,idx=get_restr(line,idx)
    try:
        bseq=s.encode('utf-8')
    except:
        stderr("Unicode encode error.")
        return [],idx
    m=list(bseq)
    return m,idx

def printvalue(s):
    global scriptingflag,verbose
    v,idx=expression(s,0)
    if v==UNKNOWN:
        return
        
    s=' . '
    if v<0x20:
        s='^'+chr(v+ord('@'))+' '
    elif v>=0x7e:
        s=' . '
    else:
        s='\''+chr(v)+'\''

    x=f"{v:016X}"
    spaced_hex = ' '.join([x[i:i+4] for i in range(0, 16, 4)])
    o=f"{v:024o}"
    spaced_oct = ' '.join([o[i:i+4] for i in range(0, 24, 4)])
    b=f"{v:064b}"
    spaced_bin = ' '.join([b[i:i+4] for i in range(0, 64, 4)])
    
    msg=f"d{v:>10}  x{spaced_hex}  o{spaced_oct} {s}\nb{spaced_bin}"

    if scriptingflag:
        if verbose:
            print(msg)
    else:
        clrmm()
        esccolor(6)
        esclocate(0,BOTTOMLN)
        print(msg,end='',flush=True)
        getch()
        esclocate(0,BOTTOMLN+1)
        print(" "*80,end='',flush=True)

def call_exec(line):
    global scriptingflag
    if len(line)<=1:
        return
    line=line[1:]
    try:
        if scriptingflag:
            exec(line,globals())
        else:
            clrmm()
            esccolor(7)
            esclocate(0,BOTTOMLN)
            exec(line,globals())
            esccolor(4)
            escclrline()
            print("[ Hit a key ]",end='',flush=True)
            getch()
            escclear()
            repaint()

    except:
        stderr("python exec() error.")

    finally:
        return

def commandline_(line):
    global lastchange,yank,filename,stack,verbose,scriptingflag,cp

    cp=fpos()
    line=comment(line)
    if line=='':
        return -1
    if line=='q':
        if lastchange:
            stderr("No write since last change. To overriding quit, use 'q!'.")
            return -1
        return 0
    elif line=='q!':
        return 0
    elif line=='wq' or line=='wq!':
        f=writefile(filename)
        if f:
            lastchange=False
            return 0
        else:
            return -1
    elif line[0]=='w':
        if len(line)>=2:
            s=line[1:].lstrip()
            writefile(s)
        else:
            writefile(filename)
            lastchange=False
        return -1
    elif line[0]=='r':
        if len(line)<2:
            readfile(filename)
            stdmm("Original file read.")
            return -1
    elif line[0]=='T' or line[0]=='t':
        if len(line)>=2:
            s=line[1:].lstrip()
            stack+=[scriptingflag]
            stack+=[verbose]
            verbose=True if line[0]=='T' else False
            print("")
            scripting(s)
            if verbose:
                stdmm("[ Hit any key ]")
                getch()
            verbose=stack[len(stack)-1]
            stack=stack[0:len(stack)-1]
            scriptingflag=stack[len(stack)-1]
            stack=stack[0:len(stack)-1]
            escclear()
            return -1
        else:
            stderr("Specify script file name.")
            return -1
    elif line[0]=='n':
        searchnext(fpos()+1)
        return -1
    elif line[0]=='N':
        searchlast(fpos()-1)
        return -1
    elif line[0]=='@':
        call_exec(line)
        return -1
    elif line[0]=='!':
        if len(line)>=2:
            invoke_shell(line[1:])
            return -1
        return -1
    elif line[0]=='?':
        printvalue(line[1:])
        return -1
    elif line[0]=='/':
        searchsub(line)
        return -1
    idx=skipspc(line,0)

    x,idx=expression(line,idx)
    xf=False
    xf2=False
    if x==UNKNOWN:
        x=fpos()
    else:
        xf=True
    x2=x

    idx=skipspc(line,idx)
    if idx<len(line) and line[idx]==',':
        idx=skipspc(line,idx+1)
        if idx<len(line) and line[idx]=='*':
            idx=skipspc(line,idx+1)
            t,idx=expression(line,idx)
            if t==UNKNOWN:
                t=1
            x2=x+t-1
        else:
            t,idx=expression(line,idx)
            if t==UNKNOWN:
                x2=x
            else:
                x2=t
                xf2=True
    else:
        x2=x

    if x2<x:
        x2=x

    idx=skipspc(line,idx)

    if idx==len(line):
        jump(x)
        return -1
    
    if idx<len(line) and line[idx]=='y':
        idx+=1
        if not xf and not xf2:
            m,idx=get_str_or_hexs(line,idx)
            yank=list(m)
        else:
            yankmem(x,x2)

        stdmm(f"{len(yank)} bytes yanked.")
        return -1

    if idx<len(line) and line[idx] == 'p':
        y = list(yank)
        ovwmem(x, y)
        jump(x + len(y))
        return -1

    if idx<len(line) and line[idx] == 'P':
        y = list(yank)
        insmem(x, y)
        jump(x + len(yank))
        return -1

    if idx+1<len(line) and line[idx]=='m':
        if 'a'<=line[idx+1]<='z':
            mark[ord(line[idx+1])-ord('a')]=x
        return -1

    if idx<len(line) and (line[idx]=='r' or line[idx]=='R'):
        ch=line[idx]
        idx+=1 
        if idx>=len(line):
            stderr("File name not specified.")
            return -1
        fn=line[idx:].lstrip()
        if fn=="":
            stderr("File name not specified.")
        else:
            try:
                f=open(fn,"rb")
                data=list(f.read())
                f.close()
            except:
                data=[]
                stderr("File read error.")

        if ch=='r':
            ovwmem(x,data)
        elif ch=='R':
            insmem(x,data)

        jump(x+len(data))
        return -1

    if idx<len(line) and line[idx] in 'oO':
        ch=line[idx]
        idx+=1
        l,idx=get_str(line,idx)
        if ch=='o':
            ovwmem(x,l)
            stdmm(f"{len(l)} bytes stored.")
        elif ch=='O':
            insmem(x,l)
            stdmm(f"{len(l)} bytes inserted.")
        jump(x+len(l))
        return -1

    if idx<len(line):
        ch=line[idx]
    else:
        ch=''

    if ch=='d':
        delmem(x,x2,True)
        stdmm(f"{x2-x+1} bytes deleted.")
        jump(x)
        return -1
    elif ch=='w':
        idx+=1
        fn=line[idx:].lstrip()
        wrtfile(x,x2,fn)
        return -1
    elif ch=='s':
        scommand(x,x2,xf,xf2,line,idx+1)
        return -1

    if idx<len(line) and line[idx]=='~':
        ch=line[idx]
        idx+=1
        openot(x,x2)
        jump(x2+1)
        return -1

    if idx<len(line) and line[idx] in "fIivCc&|^<>":
        ch=line[idx]
        idx+=1
        if ch in '<>':
            if idx<len(line) and line[idx]==ch:
                idx+=1
                multibyte=True
            else:
                multibyte=False

            times,idx=expression(line,idx)

            if times==UNKNOWN:
                times=1

            if idx<len(line) and line[idx]==',':
                bit,idx=expression(line,idx+1)
            else:
                bit=UNKNOWN

            shift_rotate(x,x2,times,bit,multibyte,ch)
            return -1

        if ch in 'f':
            m,idx=get_hexs(line,idx)
            if len(m):
                data=m*((x2-x+1)//len(m))+m[0:((x2-x+1)%len(m))]
                ovwmem(x,data)
                stdmm(f"{len(data)} bytes filled.")
                jump(x+len(data))
            else:
                stderr("Invalid syntax.")
            return -1

        if ch=='i':
            m,idx=get_hexs(line,idx)
            if idx<len(line) and line[idx]=='*':
                idx+=1
                length,idx=expression(line,idx)
            else:
                length=1

            if xf2:
                stderr("Invalid syntax.")
                return -1

            data=m*length
            ovwmem(x,data)
            stdmm(f"{len(data)} bytes overwritten.")
            jump(x+len(data))

            return -1

        if ch=='I':
            m,idx=get_hexs(line,idx)
            if idx<len(line) and line[idx]=='*':
                idx+=1
                length,idx=expression(line,idx)
            else:
                length=1

            if xf2:
                stderr("Invalid syntax.")
                return -1

            data=m*length
            insmem(x,data)
            stdmm(f"{len(data)} bytes inserted.")
            jump(x+len(data))
            return -1

        x3,idx=expression(line,idx)
        if x3==UNKNOWN:
            stderr("Invalid parameter.")
            return -1

        if ch=='c':
            yankmem(x,x2)
            cpymem(x,x2,x3)
            stdmm(f"{x2-x+1} bytes copied.")
            jump(x3+(x2-x+1))
            return -1
        elif ch=='C':
            m=redmem(x,x2)
            yankmem(x,x2)
            insmem(x3,m)
            stdmm(f"{x2-x+1} bytes inserted.")
            jump(x3+len(m))
            return -1
        elif ch=='v':
            xp=movmem(x,x2,x3)
            jump(xp)
            return -1
        elif ch=='&':
            opeand(x,x2,x3)
            jump(x2+1)
            return -1
        elif ch=='|':
            opeor(x,x2,x3)
            jump(x2+1)
            return -1
        elif ch=='^':
            opexor(x,x2,x3)
            jump(x2+1)
            return -1
    stderr("Unrecognized command.")
    return -1

def commandline(line):
    try:
        return commandline_(line)
    except MemoryError:
        stderr("Memory overflow.")

def commandln():
    esclocate(0,BOTTOMLN)
    esccolor(7)
    readline.set_pre_input_hook(no_pre_input_hook)
    line=getln(':',"command").lstrip()
    return commandline(line)

def printdata():
    addr=fpos()
    a=readmem(addr)
    esclocate(0,24)
    esccolor(6)
    s='.'
    if a<0x20:
        s='^'+chr(a+ord('@'))
    elif a>=0x7e:
        s='.'
    else:
        s='\''+chr(a)+'\''
    if addr<len(mem):
        print(f"{addr:012X} : 0x{a:02X} 0b{a:08b} 0o{a:03o} {a} {s}      ",end='',flush=True)
    else:
        print(f"{addr:012X} : ~~                                                   ",end='',flush=True)

def disp_curpos():
    esccolor(4)
    esclocate(curx // 2 * 3 + 12 , cury + 3)
    print("[",end='',flush=True)
    esclocate(curx // 2 * 3 + 15 , cury + 3)
    print("]",end='',flush=True)

def erase_curpos():
    esccolor(7)
    esclocate(curx // 2 * 3 + 12 , cury + 3)
    print(" ",end='',flush=True)
    esclocate(curx // 2 * 3 + 15 , cury + 3)
    print(" ",end='',flush=True)

def fedit():
    global nff,yank,lastchange,modified,insmod,homeaddr,curx,cury,repsw,utf8,cp
    stroke = False
    ch = ''
    repsw=0
    while True:
        cp=fpos()
        repaint()
        printdata()
        esclocate(curx // 2 * 3 + 13 + (curx & 1), cury + 3)
        ch = getch()
        clrmm()
        nff = True

        if ch == chr(27):
            c2 = getch()
            c3 = getch()
            if c3 == 'A':
                ch = 'k'
            elif c3 == 'B':
                ch = 'j'
            elif c3 == 'C':
                ch = 'l'
            elif c3 == 'D':
                ch = 'h'
            elif c2==chr(91) and c3==chr(50):
                ch='i'

        if ch == 'n':
            searchnext(fpos()+1)
            continue
        elif ch == 'N':
            searchlast(fpos()-1)
            continue

        elif ch == chr(2):
            if homeaddr >= 256:
                homeaddr -= 256
            else:
                homeaddr = 0
            continue
        elif ch == chr(6):
            homeaddr += 256
            continue
        elif ch == chr(0x15):
            if homeaddr >= 128:
                homeaddr -= 128
            else:
                homeaddr = 0
            continue
        elif ch == chr(4):
            homeaddr += 128
            continue
        elif ch == '^':
            curx = 0
            continue
        elif ch == '$':
            curx = 30
            continue
        elif ch == 'j':
            if cury < 19:
                cury += 1
            else:
                scrdown()
            continue
        elif ch == 'k':
            if cury > 0:
                cury -= 1
            else:
                scrup()
            continue
        elif ch == 'h':
            if curx > 0:
                curx -= 1
            else:
                if fpos() != 0:
                    curx = 31
                    if cury > 0:
                        cury -= 1
                    else:
                        scrup()
            continue
        elif ch == 'l':
            inccurx()
            continue
        elif ch==chr(25):
            utf8=not utf8
            escclear()
            repaint()
            continue
        elif ch == chr(12):
            escclear()
            repsw=(repsw+(1 if utf8 else 0))%4
            repaint()
            continue
        elif ch == 'Z':
            if writefile(filename):
                return True
            else:
                continue
        elif ch == 'q':
            if lastchange:
                stdmm("No write since last change. To overriding quit, use 'q!'.")
                continue
            return False
        elif ch == 'M':
            disp_marks()
            continue
        elif ch == 'm':
            ch = getch().lower()
            if 'a' <= ch <= 'z':
                mark[ord(ch) - ord('a')] = fpos()
            continue
        elif ch == '/':
            search()
            continue
        elif ch == '\'':
            ch = getch().lower()
            if 'a' <= ch <= 'z':
                jump(mark[ord(ch) - ord('a')])
            continue
        elif ch == 'p':
            y = list(yank)
            ovwmem(fpos(), y)
            jump(fpos() + len(y))
            continue
        elif ch == 'P':
            y = list(yank)
            insmem(fpos(), y)
            jump(fpos() + len(yank))
            continue

        if ch == 'i':
            insmod = not insmod
            stroke = False
        elif ch in string.hexdigits:
            addr = fpos()
            c = int("0x" + ch, 16)
            sh = 4 if not curx & 1 else 0
            mask = 0xf if not curx & 1 else 0xf0
            if insmod:
                if not stroke and addr < len(mem):
                    insmem(addr, [c << sh])
                else:
                    setmem(addr, readmem(addr) & mask | c << sh)
                stroke = (not stroke) if not curx & 1 else False
            else:
                setmem(addr, readmem(addr) & mask | c << sh)
            inccurx()
        elif ch == 'x':
            delmem(fpos(), fpos(), False)
        elif ch == ':':
            disp_curpos()
            f = commandln()
            erase_curpos()
            if f == 1:
                return True
            elif f == 0:
                return False

def readfile(fn):
    global mem,newfile
    try:
        f=open(fn,"rb")
    except:
        newfile=True
        stdmm("<new file>")
        mem=[]
    else:
        newfile=False
        try:
            mem=list(f.read())
            f.close()
            return True
        except MemoryError:
            stderr("Memory overflow.")
            f.close()
            return False
    return True

def regulate_mem():
    global mem
    for i in range(len(mem)):
        try:
            mem[i]=mem[i]&0xff
        except:
            mem[i]=0

def writefile(fn):
    global mem
    regulate_mem()
    try:
        f=open(fn,"wb")
        f.write(bytes(mem))
        f.close()
        stdmm("File written.")
        return True
    except:
        stderr("Permission denied.")
        return False

def wrtfile(start,end,fn):
    global mem
    regulate_mem()
    try:
        f=open(fn,"wb")
        for i in range(start,end+1):
            if i<len(mem):
                f.write(bytes([mem[i]]))
            else:
                f.write(bytes([0]))
        f.close()
        return True
    except:
        stderr("Permission denied.")
        return False

def main():
    global filename,verbose,scriptingflag
    parser = argparse.ArgumentParser()
    parser.add_argument('file', help='file to edit')
    parser.add_argument('-s', '--script', type=str, default='', metavar='script.bi', help='bi script file')
    parser.add_argument('-v', '--verbose', action='store_true', help='verbose when processing script')
    parser.add_argument('-w', '--write', action='store_true', help='write file when exiting script')
    args = parser.parse_args()
    filename=args.file
    script=args.script
    if not script:
        escclear()
    else:
        scriptingflag=True
    verbose=args.verbose
    wrtflg=args.write
    if not readfile(filename):
        return

    if script:
        try:
            f=scripting(script)
            if wrtflg and lastchange:
                writefile(filename)
        except:
            writefile("file.save")
            stderr("Some error occured. memory saved to file.save.")
    else:
        try:
            fedit()
        except:
            writefile("file.save")
            stderr("Some error occured. memory saved to file.save.")

    esccolor(7)
    escdispcursor()
if __name__=="__main__":
    main()

