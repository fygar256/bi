#!/usr/bin/python3
import sys
import tty
import termios
import string
import copy
import os
ESC='\033['
LENONSCR=(20*16)
BOTTOMLN=23
UNKNOWN=0xffffffffffffffffffffffffffffffff
mem=[]
yank=[]
coltab=[0,1,4,5,2,6,3,7]
filename=""
modified=False
newfile=False
homeaddr=0
insmod=False
curx=0
cury=0
mark=[UNKNOWN] * 26

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
    print(f"{ESC}2J",end='')
    esclocate()

def esccolor(col1=7,col2=0):
    print(f"{ESC}3{coltab[col1]}m{ESC}4{coltab[col2]}m",end='')

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

def getln():
    s=""
    while True:
        ch=getch()
        if ch=='\033':
            return s
        elif ch==chr(13):
            return s
        elif ch==chr(0x7f):
            if s!='':
                escleft()
                putch(' ')
                escleft()
                s=s[:len(s)-1]
        else:
            putch(ch)
            s+=ch

def skipspc(s,idx):
    while idx<len(s) and s[idx]==' ':
        idx+=1
    return idx

def print_title():
    global filename,modified,insmod,mem
    esclocate(0,0)
    esccolor(6)
    print(f"bi version 0.98 by T.Maekawa                                               {"ins" if insmod else "ovw"} ")
    esccolor(5)
    print(f"file:[{filename:<40}] length: {len(mem)} bytes {("not " if not modified else "")+"modified"}    ")

def repaint():
    print_title()
    esclocate(0,2)
    esccolor(4)
    print("OFFSET       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F 0123456789ABCDEF  ")
    esccolor(7)
    addr=homeaddr
    for y in range(0x14):
        esccolor(5)
        print(f"{addr+y*16:012X} ",end='')
        esccolor(7)
        for i in range(16):
            a=y*16+i+addr
            print(f"~~ " if a>=len(mem) else f"{mem[a]:02X} ",end='')
        esccolor(6)
        for i in range(16):
            a=y*16+i+addr
            print("~" if a>=len(mem) else (chr(mem[a]) if 0x20<=mem[a]<=0x7f else "."),end='')
        print("")

def genmem(code,length):
    return [code]*length

def insmem(start,mem2):
    global mem,modified
    if start>=len(mem):
        for i in range(start-len(mem)+1):
            mem+=[0]
    if start<len(mem):
        mem1=[]
        mem3=[]
        for j in range(start):
            mem1+=[mem[j]]
        for j in range(len(mem)-start):
            mem3+=[mem[start+j]]
        mem=mem1+mem2+mem3
    modified=True

def delmem(start,end,yf):
    global yank,mem,modified
    length=end-start+1
    if length<=0:
        return
    if start>=len(mem):
        return
    if yf:
        yank=[]
        for j in range(start,end+1):
            if j<len(mem):
                yank+=[mem[j]]

    mem1=[]
    mem2=[]
    for j in range(start):
        mem1+=[mem[j]]
    for j in range(end+1,len(mem)):
        mem2+=[mem[j]]
    mem=mem1+mem2
    modified=True

def yankmem(start,end):
    global yank,mem,modified
    length=end-start+1
    if length<=0:
        return
    if start>=len(mem):
        return
    yank=[]
    cnt=0
    for j in range(start,end+1):
        if j<len(mem):
            cnt+=1
            yank+=[mem[j]]

    stdmm(f"{cnt} bytes yanked.")

def ovwmem(start,mem0):
    global mem,modified

    if mem0==[]:
        return

    if start+len(mem0)>len(mem):
        for j in range(start+len(mem0)-len(mem)):
            mem+=[0]

    for j in range(len(mem0)):
        if j>=len(mem):
            mem+=[mem0[j]]
        else:
            mem[start+j]=mem0[j]
    modified=True

def redmem(start,end):
    global mem
    m=[]
    for i in range(start,end+1):
        if len(mem)>i:
            m+=[mem[i]]
        else:
            m+=[0]
    return m

def cpymem(start,end,dest):
    m=redmem(start,end)
    ovwmem(dest,m)

def movmem(start,end,dest):
    m=redmem(start,end)
    delmem(start,end,True)
    ovwmem(dest-(end-start+1),m)

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
    return mem[addr]

def setmem(addr,data):
    global mem
    if addr>=len(mem):
        for i in range(addr-len(mem)+1):
            mem+=[0]
    mem[addr]=data
def clrmm():
    esclocate(0,BOTTOMLN)
    esccolor(6)
    print(" "*79,end='')

def stdmm(s):
    clrmm()
    esccolor(6)
    esclocate(0,BOTTOMLN)
    print(s,end='')

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

def invoke_shell(s):
    esccolor(7)
    print()
    s=line[1:].lstrip()
    os.system(s.lstrip())
    esccolor(4)
    print("[ Hit any key to return ]",end='',flush=True)
    getch()
    escclear()

def get_value(s,idx):
    global mem
    v=UNKNOWN
    idx=skipspc(s,idx)
    if idx>=len(s):
        return UNKNOWN,UNKNOWN
    if s[idx]=='$':
        idx+=1
        v=len(mem)-1
    elif s[idx]=='^':
        idx+=1
        v=0
    elif s[idx]=='.':
        idx+=1
        v=fpos()
    elif s[idx]=='\'' and 'a'<=s[idx+1]<='z':
        v=mark[ord(s[idx+1])-ord('a')]
        if v==UNKNOWN:
            stdmm("Unknown mark.")
            return UNKNOWN,UNKNOWN
        else:
            idx+=2
    elif s[idx].lower() in '0123456789abcdef':
        x=0
        while idx<len(s) and s[idx].lower() in '0123456789abcdef':
            x=16*x+int(s[idx].lower(),16)
            idx+=1
        v=x
    elif s[idx]=='#':
        x=0
        idx+=1
        while idx<len(s) and s[idx] in '0123456789':
            x=10*x+int(s[idx])
            idx+=1
        v=x
    return v,idx

def commandline():
    esclocate(0,BOTTOMLN)
    esccolor(7)
    putch(':')
    line=getln()
    if line=='':
        return -1
    if line=='q':
        if modified:
            stdmm("No write since last change. To overriding quit, use 'q!'.")
            return -1
        return 0
    elif line=='q!':
        return 0
    elif line=='wq':
        return 1
    elif line=='wq!':
        return 1
    elif line[0]=='w':
        if len(line)>=2:
            s=line[1:].lstrip()
            writefile(s)
        return -1
    elif line[0]=='!':
        if len(line)>=2:
            invoke_shell(line[1:])
            return -1
    idx=skipspc(line,0)

    x,idx=get_value(line,idx)
    if x==UNKNOWN:
        x=fpos()
    x2=x

    idx=skipspc(line,idx)

    if idx==len(line) and not x==UNKNOWN:
        jump(x)
        return -1

    if idx<len(line) and (line[idx]=='r' or line[idx]=='R'):
        ch=line[idx]
        idx+=1 
        if idx>=len(line):
            stdmm("File name not specified.")
            return -1
        fn=line[idx:].lstrip()
        if fn=="":
            stdmm("File name not specified.")
        else:
            try:
                f=open(fn,"rb")
                data=list(f.read())
                f.close()
            except:
                data=[]
                stdmm("File read error.")

        if ch=='r':
            ovwmem(x,data)
        elif ch=='R':
            insmem(x,data)

        return -1

    if idx<len(line) and (line[idx]=='s' or line[idx]=='S'):
        ch=line[idx]
        idx+=1
        l=[ord(c) for c in line[idx:]]
        if ch=='s':
            ovwmem(x,l)
        elif ch=='S':
            insmem(x,l)
        return -1

    if idx<len(line) and line[idx]=='d':
        length,idx=get_value(line,idx+1)

        if length==UNKNOWN:
            length=1

        idx=skipspc(line,idx)

        delmem(x,x+length-1,True)
        return -1
        stdmm("Unrecognized command.")
        return -1
        

    if idx<len(line) and (line[idx]=='i' or line[idx]=='f'):
        ch=line[idx]
        length,idx=get_value(line,idx+1)

        if length==UNKNOWN:
            length=1

        code=0x00
        if idx<len(line) and line[idx]==',':
            code,idx=get_value(line,idx+1)
            if code==UNKNOWN:
                code=0x00

        data=genmem(code,length)

        if ch=='i':
            insmem(x,data)
        elif ch=='f':
            ovwmem(x,data)
        return -1

    if idx<len(line) and line[idx]==',':
        x2,idx=get_value(line,idx+1)

    idx=skipspc(line,idx)

    if idx<len(line) and line[idx]=='d':
        delmem(x,x2,True)
        return -1
    elif idx<len(line) and line[idx]=='y':
        yankmem(x,x2)
        return -1
    elif idx<len(line) and line[idx]=='w':
        idx+=1
        fn=line[idx:].lstrip()
        wrtfile(x,x2,fn)
        return -1

    if idx<len(line) and line[idx]=='f' or line[idx]=='m' or line[idx]=='c':
        ch=line[idx]
        x3,idx=get_value(line,idx+1)
        if x3==UNKNOWN:
            stdmm("Invalid parameter.")
            return -1
        if ch=='f':
            data=[x3]*(x2-x+1)
            ovwmem(x,data)
            return -1
        elif ch=='c':
            cpymem(x,x2,x3)
            return -1
        elif ch=='m':
            movmem(x,x2,x3)
            return -1

    stdmm("Unrecognized command.")
    return -1

def fedit():
    global yank,modified,insmod,homeaddr,curx,cury
    stroke=False
    while True:
        repaint()
        esclocate( curx//2*3+13+(curx&1),cury+3)
        ch=getch()
        if ch==chr(2):
            if homeaddr>=256:
                homeaddr-=256
            else:
                homeaddr=0
            continue
        elif ch==chr(6):
            homeaddr+=256
            continue
        elif ch==chr(0x15):
            if homeaddr>=128:
                homeaddr-=128
            else:
                homeaddr=0
            continue
        elif ch==chr(4):
            homeaddr+=128
            continue
        elif ch=='^':
            curx=0
            continue
        elif ch=='$':
            curx=30
            continue
        elif ch=='j':
            if cury<19:
                cury+=1
            else:
                scrdown()
            continue
        elif ch=='k':
            if cury>0:
                cury-=1
            else:
                scrup()
            continue
        elif ch=='h':
            if curx>0:
                curx-=1
            else:
                if fpos()!=0:
                    curx=31
                    if cury>0:
                        cury-=1
                    else:
                        scrup()
            continue
        elif ch=='l':
            inccurx()
            continue
        elif ch==chr(12):
            escclear()
            repaint()
            continue
        elif ch=='Z':
            return(True)
        elif ch=='q':
            return(False)
        elif ch=='M':
            disp_marks()
            continue
        elif ch=='m':
            ch=getch().lower()
            if 'a'<=ch<='z':
                mark[ord(ch)-ord('a')]=fpos()
            continue
        elif ch=='\'':
            ch=getch().lower()
            if 'a'<=ch<='z':
                jump(mark[ord(ch)-ord('a')])
            continue
        elif ch=='p':
            y=list(yank)
            ovwmem(fpos(),y)
            jump(fpos()+len(y))
            continue
        elif ch=='P':
            y=list(yank)
            insmem(fpos(),y)
            delmem(len(mem)-1,len(mem)-1,False)
            jump(fpos()+len(yank))
            continue

        clrmm()

        if ch=='i':
            insmod=not insmod
            stroke=False
        elif ch in string.hexdigits:
            addr=fpos()
            c=int("0x"+ch,16)
            sh=4 if not curx&1 else 0
            mask=0xf if not curx&1 else 0xf0
            if insmod:
                if not stroke and addr<len(mem):
                    insmem(addr,[c<<sh])
                else:
                    setmem(addr,readmem(addr)&mask|c<<sh)
                stroke=(not stroke) if not curx&1 else False
            else:
                setmem(addr,readmem(addr)&mask|c<<sh)
                modified=True
            inccurx()
        elif ch=='x':
            delmem(fpos(),fpos(),False)
        elif ch==':':
            f=commandline()
            if f==1:
                return True
            elif f==0:
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
        mem=list(f.read())
        f.close()

def writefile(fn):
    global mem,newfile
    f=open(fn,"wb")
    f.write(bytes(mem))
    f.close()

def wrtfile(start,end,fn):
    global mem
    f=open(fn,"wb")
    for i in range(start,end+1):
        if i<len(mem):
            f.write(bytes([mem[i]]))
        else:
            f.write(bytes([0]))
    f.close()

def main():
    global filename
    if len(sys.argv)<=1:
        print("Usage: bi file")
        return
    filename=sys.argv[1]
    readfile(filename)
    f=fedit()
    if f:
        writefile(filename)

if __name__=="__main__":
    main()
    exit(0)
