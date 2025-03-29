#!/usr/bin/python3
import sys
import tty
import termios
import string
ESC='\033['
mem=[]
coltab=[0,1,4,5,2,3,6,7]
filename=""
modified=False
homeaddr=0
insmod=False
curx=0
cury=0

def getch():
    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    tty.setraw(sys.stdin.fileno())
    ch = sys.stdin.read(1)
    termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
    return ch

def putch(c):
    print(c,end='',flush=True)

def escup(n=1):
    print(f"{ESC}{n}A",end='')

def escdown(n=1):
    print(f"{ESC}{n}B",end='')

def escright(n=1):
    print(f"{ESC}{n}C",end='')

def escleft(n=1):
    print(f"{ESC}{n}D",end='')

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
    print("OFFSET       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F ASCII            ")
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

def insmem(start,end,code):
    global mem,modified
    length=end-start+1
    if length<=0:
        return
    if end>=len(mem):
        for i in range(end-len(mem)+1):
            mem+=[code]
    if start<len(mem):
        mem1=[]
        mem2=[]
        mem3=[]
        for j in range(start):
            mem1+=[mem[j]]
        for j in range(length):
            mem2+=[code]
        for j in range(len(mem)-start):
            mem3+=[mem[start+j]]
        mem=mem1+mem2+mem3
    modified=True

def delmem(start,end):
    global mem,modified
    length=end-start+1
    if length<=0:
        return
    if start>=len(mem):
        return
    mem1=[]
    mem2=[]
    for j in range(start):
        mem1=[mem[j]]
    for j in range(len(mem)-end):
        mem2=[mem[j]]
    mem=mem1+mem2
    modified=True

def ovwmem(start,mem0):
    global mem,modified
    for j in range(len(mem0)):
        if j>=len(mem):
            mem+=[mem0[j]]
        else:
            mem[start+j]=mem[j]
    modified=True

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

def fedit():
    global modified,insmod,homeaddr,curx,cury
    stroke=False
    while True:
        repaint()
        esclocate( curx//2*3+13+(curx&1),cury+3)
        ch=getch()
        if ch==chr(2):
            if homeaddr>=256:
                homeaddr-=256
        elif ch==chr(6):
            homeaddr+=256
        elif ch==chr(0x15):
            if homeaddr>=128:
                homeaddr-=128
        elif ch==chr(4):
            homeaddr+=128
        elif ch=='^':
            curx=0
        elif ch=='$':
            curx=30
        elif ch=='j':
            if cury<19:
                cury+=1
            else:
                scrdown()
        elif ch=='k':
            if cury>0:
                cury-=1
            else:
                scrup()
        elif ch=='h':
            if curx>0:
                curx-=1
            else:
                curx=31
                if cury>0:
                    cury-=1
                else:
                    scrup()
        elif ch=='l':
            inccurx()
        elif ch=='Z':
            return
        elif ch=='i':
            insmod=not insmod
            stroke=False
        elif ch==chr(12):
            repaint()
        elif ch==chr(3):
            return
        elif ch in string.hexdigits:
            addr=fpos()
            c=int("0x"+ch,16)
            sh=4 if not curx&1 else 0
            mask=0xf if not curx&1 else 0xf0
            if insmod:
                if not stroke:
                    insmem(addr,addr,c<<sh)
                else:
                    mem[addr]=mem[addr]&mask|c<<sh
                stroke=(not stroke) if not curx&1 else False
            else:
                mem[addr]=mem[addr]&mask|c<<sh
                modified=True
            inccurx()

def readfile(fn):
    global mem
    f=open(fn,"rb")
    mem=list(f.read())
    f.close()

def writefile(fn):
    global mem
    f=open(fn,"wb")
    f.write(bytes(mem))
    f.close()

def main():
    global filename
    filename=sys.argv[1]
    readfile(filename)
    fedit()
    writefile(filename)

if __name__=="__main__":
    main()
    exit(0)
