# bi
Binary editor like vI - bi

bi is a binary editor with a user interface similar to vi. It started when I posted it on ASCII-net for MS-DOS in December 1991. 33 years have passed since then. Another binary editor with a user interface similar to vi was released for Linux in 1996 by Gerhard Buergmann, called bvi, but I was the first. The first version of bi was written in C, but the currently released version is written in python.

It uses ANSI terminal escape sequences, so it is for ANSI terminals. I think it will run on python and on Linux, FreeBSD, unix, and POSIX-compliant OS with ANSI terminal.

The bi is designed to be with high functionality, flexible, lightweight, simple, compact, user-friendly and smoothly responsive. bi can handle original script.

The development environment and operation verification are done on ArchLinux.

This software disributed with MIT license.

```
##### installation (Other)
git clone http://github.com/fygar256/bi
cd bi
chmod +x bi.py
sudo cp bi.py /usr/bin/bi

sudo cp bi.1 /usr/share/man/man1/
sudo mandb

##### execution
bi file

##### reference of man
man bi

##### Document

                   vi like binary editor 'bi'

           Designed and Programmed by Taisuke Maekawa (fygar256)
                   

Overview
--------

    BI is a binary editor designed to mimic the interface of the
    UNIX editor 'vi'. The name (BI) is an abbreviation for Binary editor
	like vI. For the vi and binary enthusiasts.

Command Reference

     Commands on edit mode.

   <hex-key>               ----- set data
   hjkl or arrow key       ----- move cursor
   ^F ^B                   ----- move by a page ( 256 bytes )
   ^D ^U                   ----- move by half a page ( 128 bytes )
   ^L                      ----- repaint screen.
   ^Y                      ----- toggle utf-8 display enable/disable
   ^                       ----- jump to the left end of line
   $                       ----- jump to the right end of line
   m[a-z]                  ----- mark currrent position
   '[a-z]                  ----- jump to marked point

   n                       ----- search the next
   N                       ----- search the last
   M                       ----- display marks

   p                       ----- paste yank buffer (overwrite)
   P                       ----- paste yank buffer (insert)
   q                       ----- quit
   x                       ----- delete a byte
   Z                       ----- write and quit

   /                       ----- to command line search mode
   :                       ----- to command line mode

On command line mode

   #                       ----- comment. will be ignored after '#'
   /<regexp>               ----- search regular expression string
   //xx xx xx ...          ----- search binary data
   !<string>               ----- invoke shell
   @<string>               ----- call python exec()
   ?<value>                ----- print value in decimal,hex,octal,ASCII,binary
   q                       ----- quit
   q!                      ----- overriding quit
   wq,wq!                  ----- write and quit
   r                       ----- read original file
   w                       ----- write data to original file
   w <filename>            ----- write data to filename
   t <filename>            ----- scripting with filename in silence mode
   T <filename>            ----- scripting with filename in verbose mode
   n                       ----- search the next
   N                       ----- search the last
   [offset]                ----- jump to the address
   [offset]m[a-z]          ----- mark position
   [offset]O<string>       ----- insert string and jump to end of string+1
   [offset]o<string>       ----- overwrite string and jump to end of str+1
   [offset]R<filename>     ----- read file and insert at [offset]
   [offset]r<filename>     ----- read file (overwrite) on and after [offset]
   [offset] p              ----- paste yank buffer (overwrite)
   [offset] P              ----- paste yank buffer (insert)
   [offset]i xx xx xx ... *<length>  ----- overwrite data xx repeatedly length times
   [offset]I xx xx xx ... *<length>  ----- insert data xx repeatedly length times

   y/str                   ----- yank to yank buffer with string
   y//xx xx xx ...         ----- yank to yank buffer with data
   <start>,<end> d         ----- delete by range (data will be yanked)
   <start>,<end> C <dest>  ----- insert data to <dest> (data will be yanked)
   <start>,<end> c <dest>  ----- copy data (data will be yanked)
   [start,end] v <dest>    ----- move data
   [start,end] s /regexp/str            ----- replace regexp with str
   [start,end] s /regexp//xx xx xx ...  ----- replace regexp with data
   [start,end] s //xx xx xx .../str     ----- replace data1 with str
   [start,end] s //xx xx ...//xx xx ... ----- replace data1 with data2
   [start],[end] y         ----- yank to yankbuffer
   [start,end]|<data>      ----- bitwise or with data
   [start,end]&<data>      ----- bitwise and with data
   [start,end]^<data>      ----- bitwise xor with data
   [start,end]~            ----- bitwise not with data
   [start,end]<[[times],[01]]  - left shift with bit 0,1 or rotate by byte
   [start,end]>[[times],[01]]  - right shift with bit 0,1 or rotate by byte
   [start,end]<<[[times],[01]] - left shift with bit 0,1 or rotate by multibyte
   [start,end]>>[[times],[01]] - right shift with bit 0,1 or rotate by multibyte
   <start>,<end> f xx xx xx ... - fill with data by range
   <start>,<end>w<filename> ---- write data on file
   <CR> without any command or <ESC>   ----- return to on-screen mode

Remarks

    Regular expression can be used for string search.
    '/' can be escaped with escape character '\' in regular expression.

    Comment can be written in command with '#'. You have to write command
    including semicolon with escape character '\'.

    The values enclosed with `[]` can be left out, when these commands
    above take the current position as the value omitted.
    But when the values such as start and end are omitted in 's' command,
    's' command will affect entire file.

    The value <end> can be passed with '*<length>' as <end>=<start>+<length>-1.

    On command line, you've got to give values by simple expression as
    followings.

        <expression> := <factor> | <factor> [+|-] <factor>

    factor is a number in hexadecimal or decimal with prefix '#'.
    And you can also give values with '[a-z] as marked position,
    0 as the top of file, . as the current position, and $ as the bottom
    of file. Python eval() expression is what is enclosed with '{}'.
    You can give values like this {0xff^0x55}.
    Also, you can give the value of file as mem[x] in {}, x can be specified
    to current position 'cp'.

    The v command has a bit of a quirk. It deletes data from <start> to <end>
    and moves it to <dest>, but if dest==filesize, the deleted data is moved
    to the end of the data + 1, and if dest>filesize, it fills from the end of
    the file + 1 to dest with 0s, and writes the deleted data from dest.

    ^L switches multi-byte boundary from 0 to 3. if utf-8 multi-byte characters
    are not seen, press ^L several times.

    ^Y toggles utf-8 display mode. Initial value is disable. When the screen
    is disturbed by utf-8 encode error, you can disable it with pressing ^Y.

Scripting functionality

    bi has Scripting functionality.
    bi sctipt is named 'file.bi'. The command line synopsis of specificaton
    of script file is like that: 'bi [-v] -s file.bi targetfile'

Python exec
    The python exec() is Turing complete therefore, with '@'
    command, everything is able to be written.

    For example, in new file situation, production for the file including
    0~255 repeated 4times of 1024 bytes can be written like this:

    :@for i in range(1024):\n    setmem(i,i%256) # 0~255 repeat 4 times

    If you import modules, they will be located bi's global space.

Speed up
    If you need for bi to be higher speed, you can compile bi.py with
    pyinstaller like this: pyinstaller --onefile bi.py

★Attention

    It doesn't support undo command yet.

Danger
    With '@' command, if you rewrite global variable of bi with 'global <var>'
    and '<var>=<some value>', it might destroy bi system and cause python
    system error. So be careful when you rewrite global variable of bi.

------ HISTORY -----
1991-12-4 A sector was lost on a floppy disk used to back up a hard disk.
1991-12-5 Reluctantly repaired the file using DUMP and a C program.
1991-12-6 Started creating a file editor. This is what they mean by "hastily".
1991-12-7 Coding.
1991-12-8 Coding.
1991-12-9 Finished for now.
1991-12-10 Added error checking when the disk is full.
1991-12-20? Posted on ASCII-pcs junk.test, but deleted immediately by maintenance.
1992-01-18 Distinguished between q, q!, and wq. Renamed to bi.
1992-01-23 Rewrote the documentation version 0.9999
1992-02-05 Fixed a bug in wq version 0.99992
1992-02-10 Fixed a bug in memory allocation errors version 0.99998
Added ^D,^U commands
--- linux version is as follows.
2025-03-29 version 1.98
2025-03-30 version 1.989 Added a little debugging, x command, M command,
' command, and m command.
2025-03-31 version 1.9893 Added command line commands. Added !,w,q,wq,wq!,
w<file> commands.
2025-04-01 version 1.98951 y, d, p, P commands added
2025-04-02 version 1.98953 r, R commands added
2025-04-02 version 1.98955 i, f commands added
2025-04-02 version 1.9896 c, m commands added
2025-04-03 version 1.9897 s, S, w commands added
2025-04-03 version 1.9899 /, ?,N, n commands added
2025-04-03 linux version is basically complete.
2025-04-04 Bug fixes. Complete. version 2.0
2025-04-11 version 2.1 a little adjustment.
2025-04-12 version 2.2 regular expression support.
2025-04-12 version 2.3 u command added
2025-04-12 version 2.4 change '?' to '//' for uniform notation
2025-04-13 version 2.5 a command added
2025-04-13 version 2.5.5 change 'u' to 'y' for uniform notation
2025-04-13 version 2.5.7 Bug fixed of shell invoke
2025-04-13 version 2.6.0 &,^,|,~ command added. adjustment to get start,end parameters
2025-04-13 version 2.6.9 change 'm' to 'v' for scripting notation in the future and adjustment of search commands
2025-04-14 version 2.7.0 added scripting function.
2025-04-14 version 2.7.3 a little adjustment
2025-04-14 version 2.8.0 added simple expression functionality and add a little adjustment
2025-04-14 version 2.8.3 added '%' prefix to pass <end> value in command line parameter and added a little adjustment
2025-04-14 version 2.8.5 added rotate and shift command.
2025-04-15 version 2.9.1 Bug fixes of multibyte shift and rotate.
2025-04-15 version 2.9.5 Bug fixes of comment.
2025-04-15 version 2.9.6 a little adjustment and 'o' command added.
2025-04-15 version 2.9.7 a little adjustment and 'O' command added.
2025-04-16 version 3.0.0 Bug fixes of flags to write file (lastchange,modified)
2025-04-19 version 3.0.1 Bug fixes of commandline parsing and a little adjustment.
2025-04-19 version 3.0.2 Bug fixes of 'i' command.
2025-04-20 version 3.0.3 added permission check when writing file.
2025-04-20 version 3.0.4 change command name 'a' to 's' and Store/Insert data commans renamed to 'o' and 'O'
2025-04-20 version 3.0.5 Bug fixes.
2025-04-21 version 3.0.6 regexp bug fixes.
2025-04-21 version 3.0.8 get regexp search high-speed.
2025-04-23 version 3.0.9 'd' command bug fix.
2025-04-24 version 3.1.0 added data print functionality on bottom line.
2025-04-24 version 3.2.0 utf-8 multi-byte (up to 3) manipulation added for Japanese.
2025-04-26 version 3.3.0 added checking memory overflow error.
2025-04-26 version 3.3.1 separated message into error message and standard message.
2025-04-26 version 3.4.0 make it can give a factor python eval() expression.
2025-04-26 version 3.4.1 added functionality of print a value.
2025-05-01 version 3.4.3 added reference of cp and mem[] as current position and editing file and '@' command.
2025-05-04 version 3.4.3.5 adjustment to uniform notation to python.
2025-05-04 version 3.4.4 utf8 manipulation (full) added.
2025-05-09 version 3.4.4.7 stable.
--------------------

      I don't take any responsibility for the result of application of
    this program.

```

##### Debug cooperation

Pacific Software Development

##### Screenshot

<img alt="image" src="https://github.com/fygar256/bi/blob/main/screenshot.png">

##### Mascot character

<img alt="image" width="200px" height="200px" src="https://github.com/fygar256/bi/blob/main/bigirl.png">
