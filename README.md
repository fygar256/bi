# bi

Binary editor like vI - bi

bi is a binary editor with a user interface similar to vi. It started when I posted it on ASCII-net for MS-DOS in December 1991. 33 years have passed since then. Another binary editor with a user interface similar to vi was released for Linux in 1996 by Gerhard Buergmann, called bvi, but I was the first. The first version of bi was written in C, but the currently released version is written in python and Go.

It uses ANSI terminal escape sequences, so it is for ANSI terminals. It should work on ANSI terminals on Linux, Unix, FreeBSD, and POSIX-compliant OSes where python runs. 

Development environment and operation verification was done on ArchLinux and FreeBSD. 

If the terminal background color is white, use any string other than `black` for the -t option, or omit the option if it is black.

The bi is designed to be with high functionality, flexible, lightweight, simple, compact, user-friendly and smoothly responsive. bi can handle original script.

The development environment and operation verification are done on ArchLinux.

This software disributed with MIT license.

## in go high speed version

This is a fast version of 'bi', the Binary editor like vi, written in Go. It doesn't support Python calls or eval() or exec(), but it runs much faster than the Python version, making it ideal for working with large files.

The manual, commands, and operation methods are the same as those for the Python version of bi.

The following explains how to initialize the Go directory:

```
go mod init bi
go get golang.org/x/term
```

To build:

```
go build -o bi bi.go
sudo cp bi /usr/local/bin
```

To run:

```
bi file
```

## in python high functionality version

##### installation

```
git clone http://github.com/fygar256/bi
cd bi
chmod +x bi.py
sudo cp bi.py /usr/local/bin/bi

sudo cp bi.1.gz /usr/share/man/man1/
sudo mandb
```

##### execution
bi file

##### reference of man
man bi

##### Title

vi like binary editor 'bi'

Designed and Programmed by Taisuke Maekawa (fygar256)

Overview
--------

BI is a binary editor designed to mimic the interface of the

UNIX editor 'vi'. The name (BI) is an abbreviation for Binary editor

like vI. For the vi and binary enthusists.

##### Debug cooperation

Pacific Software Development

##### Screenshot

<img alt="image" src="https://github.com/fygar256/bi/blob/main/screenshot.png">

##### Mascot character

<img alt="image" width="200px" height="200px" src="https://github.com/fygar256/bi/blob/main/bigirl.png">
