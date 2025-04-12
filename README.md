# bi
Binary editor like vI - bi

This is a binary editor with an interface similar to vi. It started when I uploaded it to ASCII-net for MS-DOS in December 1991. 33 years have passed since then. In 1996, Gerhard Buergmann released bvi, which was created for Linux with an interface similar to vi, but I was the first to do so. The version for MS-DOS is complete. 2nd version of linux is completed on 2025-04-04.

The first version was written in C,this version is written in Python.

```
##### installation
chmod +x bi.py
sudo cp bi.py /usr/bin/bi

##### execution
bi file
```

##### Manual

```

                   vi like binary editor 'bi'

                   Programmed by T.Maekawa (fygar256)

Overview
--------

    BI is a binary editor designed to mimic the interface of the
    UNIX editor 'vi'. The name (BI) is an abbreviation for Binary editor
	like vI. 

★Command Reference

     Commands on edit mode.

   <hex-key>               ----- set data
   hjkl or allow key       ----- move cursor
   ^F ^B                   ----- move by a page ( 256 bytes )
   ^D ^U                   ----- move by half a page ( 128 bytes )
   ^L                      ----- repaint screen.
   ^                       ----- jump to the left end of line
   $                       ----- jump to the right end of line
   m[a-z]                  ----- mark currrent position
   '[a-z]                  ----- jump to marked point
   /<regexp>               ----- search regular expression string
   //xx xx xx ...          ----- search binary data

   n                       ----- search the next 
   N                       ----- search the last
   M                       ----- display marks

   p                       ----- paste yank buffer (overwrite)
   P                       ----- paste yank buffer (insert)
   q                       ----- quit
   x                       ----- delete a byte
   Z                       ----- write and quit

   :                       ----- to command line mode

    ◎On command line mode

   [offset]                ----- jump to the address
   [offset]S<string>       ----- insert string before [offset]
   [offset]s<string>       ----- overwrite string on and after [offset]

   [offset]R<filename>     ----- read file (insert) before [offset]
   [offset]r<filename>     ----- read file (overwrite) on and after [offset]
   [offset] f <len>,<data> ----- fill with <data> (by length)
   <start>,<end> f <xx>    ----- fill with xx (by range)
   [offset]i<len>,<data>   ----- insert data
   [offset] d <len>        ----- delete by length

   <start>,<end> d         ----- delete by range
   <start>,<end>y          ----- yank to yankbuffer
   <start>,<end>m<dest>    ----- move data
   <start>,<end> c <dest>  ----- copy data (data will be yanked)
   <start>,<end> i <dest>  ----- insert data
   u/str                   ----- yank to yank buffer with string
   u//xx xx xx ...         ----- yank to yank buffer with data
@  <start>,<end> a /regexp/str                  ----- replace regexp with str
@  <start>,<end> a /regexp//xx xx xx ...        ----- replace regexp with data
@  <start>,<end> a //xx xx xx .../str           ----- replace data1 with str
@  <start>,<end> a //xx xx xx ...//xx xx xx ... ----- replace data1 with data2
   <start>,<end>w<filename> ---- write data on file
   !<string>               ----- invoke shell
   q                       ----- quit
   q!                      ----- overriding quit
   wq,wq!                  ----- write and quit
   w <filename>            ----- write file
   <CR> without any command or <ESC>   ----- return to on-screen mode


    Regular expression can be used for string search.

      The values enclosed with `[]` can be left out, when
    these commands above take the current position as the value omitted. 

      On command line, you've got to give values in hexadecimal or decimal with
      prefix '#'.

    And you can also give values with '[a-z] as marked position,
    ^ as the top of file, . as the current position, and $ as the bottom
    of file.

    The m command has a bit of a quirk. It deletes data from <start> to <end>
    and moves it to <dest>, but if dest==filesize, the deleted data is moved
    to the end of the data + 1, and if dest>filesize, it fills from the end of
    the file + 1 to dest with 0s, and writes the deleted data from dest.

    the commands that marked with `@` are not implemented yet.

★Attention

    It doesn't support undo command yet.
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
--------------------

      I won't owe any responsibility for the result of application of
    this program. 

```

##### Screenshot

<img alt="image" src="https://github.com/fygar256/bi/blob/main/screenshot-2025-04-11.png">
