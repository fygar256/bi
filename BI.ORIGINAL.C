/*
 *	BI the binary file editor for VI and binary enthsiasts
 *
 *		programmed and copyright (c) by T.Maekawa (GAR)
 *						Dec 20th 1991
 */

#include	<stdio.h>
#include	<ctype.h>
#include	<dos.h>
#include	<io.h>
#include	<string.h>

/* original library */
#include	<usr/p.h>
#include	<usr/print.h>
#include	<usr/escseq.h>

#define	WTOP		3
#define	NLINE		20
#define	LENONSCR	(NLINE*16)
#define	BOTTOM		23
#define	UNMARKED	-1l
#define	MAXLENGTH	0x7fffffff
#define	UNASSIGNED	-1l
#define	ERR_NOWKSPACE	"No enough space on working disk"
#define	ERR_NOPREV	"No previous data to look for."
#define	ERR_UNRECO	"Unrecognized command."
#define	ERR_FLEXIST	"File exists - use \"w!\" to overwrite."

static	char		rcs_id[]="$Id: bi.c 1.1 93/04/07 13:56:40 Perky_Jean Exp Locker: Perky_Jean $";
static	FILE		*fp;
static	char		srcfilename[ 100 ];
static	char		filename[ 20 ];
static	int		drive = 0;
static	int		handle;
static	long		flen;
static	int		newfile;
static	int		modified = 0;
static	long		fpoint;
static	int		df;
static	int		curx = 0;
static	int		cury = 0;
static	long		mark[ 26 ];
static	char		tempfn[] = "$$TEMP$$.BI";
static	int		yanked = 0;
static	long		yanklen = 0;
static	char		yankbuffer[ 16384 ];
static	char		yankfn[] = "$$YANK$$.BI";
static	FILE		*yankfp;
static	unsigned char	searchbuff[ 256 ];
static	int		searchlen = 0;
static	int		searchmf;

get_a_char()
{
	cursor(1);
	return(charin());
}

main(int argc,char *argv[])
{
	char		buff[100 ];
	int		rv;
	long		l;

	if ( argc!=2 ) {
		fprintf(stderr,"Usage : bi file");
		exit(-1);
		}
	strcpy( srcfilename,argv[1] );
	fullpath( srcfilename );
	newfile = !exist(srcfilename) ;
	tmpnam( filename );
	create(filename);
	if (!newfile)
		move(srcfilename,filename);
	fp = fopen(filename,"r+b");
	handle = fileno( fp );
	drive = drvnum( filename );
	fpoint = 0;
	cursor(0);
	printmode(0);
	cls();
	color(7);
	rv = fedit();
	l = filelength(handle);
	fclose( fp );
	if (rv && modified) {
		sprintf(buff,"\"%s\" %ld byte(s).",srcfilename,l);
		stdmm(buff);
		move(filename,srcfilename);
		}
	unlink(filename);
	unlink(yankfn);
	cursor(1);
	elocate(0,BOTTOM);
}

title()
{
	locate(0,0);
	color(5);
	print("bi Version 0.99998 by T.Maekawa (GAR)");
}

scrdown()
{
	char	buff[17];
	int	leng;
	eclrline(BOTTOM);
	if (fpoint>=16) {
		textwindow(0,WTOP,79,WTOP+NLINE-1);
		textscroll(1);
		textwindow(0,0,79,24);
		fseek( fp,fpoint-16,0 );
		leng = fread( buff,1,17,fp );
		locate( 0,WTOP );
		if (leng>16) leng = 16;
		hexdmp( fpoint-16,buff,leng,1 );
		fpoint-=16;
		}
}

scrup()
{
	char	buff[17];
	int	leng;
	eclrline(BOTTOM);
	textwindow(0,WTOP,79,WTOP+NLINE-1);
	textscroll(0);
	textwindow(0,0,79,24);
	fseek( fp,fpoint+NLINE*16,0 );
	leng = fread( buff,1,17,fp );
	locate( 0,WTOP+NLINE-1 );
	if (leng>16) leng = 16;
	hexdmp( fpoint+NLINE*16,buff,leng,1 );
	fpoint+=16;
}

disp_len()
{
	color(4);
	locate(48,1);
	print("%10ld",flen);
	color(7);
}
repaint()
{
	cprint(CLS);
	disp();
}

disp()
{
	int	leng;
	char	buff[ 20*16 ];
	color(4);
	locate(0,1);
	print("file [%-30s]  length : %10ld   %s modified   ",
			srcfilename,flen,modified?"":"not");
	color(7);
	locate(0,2);
	color(6);
	print("OFFSET   +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F  0123456789ABCDEF");
	fseek( fp,fpoint,0 );
	leng = fread( buff,1,sizeof(buff), fp );
	color(7);
	locate(0,3);
	hexdmp( fpoint,buff,leng,20 );
}

long position() { return( fpoint+ curx/2 + cury*16 ); }

fedit()
{
	int		rv,c,d;
	long		value,wp,idx,flentmp;
	int		i,leng,tmp;
	char		buff[16];

	for(i=0;i<26;i++) mark[i] = UNMARKED;
	cprint(CLS);
	title();
	curx = 0;
	cury = 0;
	if (newfile) stdm( "< New File >");

	df = 1;
	while(1) {
		clearerr(fp);
		flen = filelength(handle);
		disp_len();
		if (df) disp();
		curset( curx,cury );
		do  c = get_a_char(); while( c==0x1b);
		if (c=='Z') return(1);
		df = 0;
		if (keyinp(6)&bit6)	c = 2;
		if (keyinp(6)&bit7)	c = 6;
		if (keyinp(7)&bit2)	c = 'k';
		if (keyinp(7)&bit3)	c = 'h';
		if (keyinp(7)&bit4)	c = 'l';
		if (keyinp(7)&bit5)	c = 'j';
		if (isxdigit(c)) {
			tmp = modified;
			modified = 1;
			wp = position();
			fillblank( wp,0 );
			if (wp>flen) disp();
			if (keyinp(0)&bit0) {
				errm("Aborted.");
				}
			else {
				c -= isdigit(c)?'0':'a'-10;
				fseek(fp,wp,0);
				d = fgetc( fp ) & 0xff;
				fseek(fp,wp,0);
				if (curx&1)  fputc(d & 0xf0 | c,fp );
				else	fputc( d&0xf | c<<4,fp );
				locate(0,cury+3);
				fseek( fp,wp&~0xf,0 );
				leng = fread( buff,1,16,fp );
				hexdmp( wp&~0xf,buff,leng,1 );
				c = 'l';
				disp_len();
				df = tmp==0;
				}
			}
		switch (c) {
		case	2:			/* ^B */
			fpoint &= 0xffffff00;
			if( fpoint>=256 )   fpoint -=256;
			jump( fpoint );
			break;
		case	6:			/* ^F */
			fpoint = (fpoint & 0xffffff00)+256;
			jump(fpoint);
			break;
		case	0x15:			/* ^U */
			fpoint &= 0xffffff80;
			if( fpoint>=128 )   fpoint -=128;
			jump( fpoint );
			break;
		case	4:			/* ^D */
			fpoint = fpoint & 0xffffff80 + 128;
			jump(fpoint);
			break;
		case	7:
			eclrline(BOTTOM);
			locate(0,BOTTOM);
			print("\"%s\" %ld byte(s) of %ld -- %d%% -- ",
				srcfilename, position(),filelength(handle),
				filelength(handle)?
				(int)((position()+1l)*100l/filelength(handle)):
				-1);
			break;
		case	12:
			df = 1;
			fclose(fp);
			fp = fopen(filename,"rb+");
			handle = fileno(fp);
			break;
		case	'$':
			jump( (position()&~0xf) +0xf );
			break;
		case	'^':
			jump( position()&~0xf );
			break;
		case	8:
		case	'h':
			if (curx>0) --curx;
			else	{
				curx = 31;
				if ( cury>0 )	cury--;
				else	scrdown();
				}
			break;
		case	'l':
			if (curx<31) ++curx;
			else	{
				curx = 0;
				if ( cury<19 )  ++cury;
				else	scrup();
				}
			break;
		case	'j':
			if ( cury <19 )  ++cury;
			else	scrup();
			break;
		case	'k':
			if ( cury > 0 )  --cury;
			else	scrdown();
			break;
		case	'm':
			c = tolower(get_a_char());
			if ( c<'a' || c>'z' ) {
				print("%c",7);
				break;
				}
			mark[ c-'a' ] = position();
			break;
		case	'\'':
			c = tolower( get_a_char());
			if (c<'a' || c>'z' ) {
				print("%c",7);
				break;
				}
			value = mark[ c-'a' ];
			if (value == UNMARKED) errm( "Unknown Mark" );
			else	jump( value );
			break;
		case	'P':
			pastei(position());
			break;
		case	'p':
			paste(position());
			break;
		case	'n':
			searchforward();
			break;
		case	'N':
			searchreverse();
			break;
		case	'y':
			if (mark[0]!=UNMARKED) {
				yank(mark[0],position()-mark[0]+1);
				}
			break;
		case	'/':
			disp_curpos();
			search('/');
			searchforward();
			era_curpos();
			break;
		case	'?':
			disp_curpos();
			search('?');
			searchreverse();
			era_curpos();
			break;
		case	':':
			disp_curpos();
			rv = commandline();
			if (rv!=-1) return(rv);
			era_curpos();
			break;
		case	'M':
			disp_marks();
			break;
			
		}
	}
}

disp_marks()
{
	int		i;
	eclrline(BOTTOM);
	locate(0,BOTTOM);
	for(i=0;i<26;i++) {
		print("%c : ",i+'a');
		if (mark[i]==UNMARKED) print("unknown     ");
		else	print("%08lx    ",mark[i]);
		}
	print("\n");
	print("yank buffer length : %08lx\n",yanklen);
	stdm("[ Hit return to continue ]");
	get_a_char();
	repaint();
}


curset(int x,int y)
{
	locate( x/2*3+9+(x&1),y+3 );
}

static	int	svx,svy;

disp_curpos()
{
	curset(curx&~1,cury);
	cprint(CUR_LEFT);
	cprint( '<' );
	cprint(CUR_RIGHT);
	cprint(CUR_RIGHT);
	cprint( '>' );
	svx = curx;
	svy = cury;
}
era_curpos()
{
	curset(svx&~1,svy);
	cprint(CUR_LEFT);
	cprint( ' ' );
	cprint(CUR_RIGHT);
	cprint(CUR_RIGHT);
	cprint( ' ' );
}

stdmm( char *s )
{
	eclrline(BOTTOM);
	locate(0,BOTTOM);
	color(7);
	print("%s",s);
}
stdmv( long v,char *s)
{
	eclrline(BOTTOM);
	locate(0,BOTTOM);
	color(5+AT_REVERSE);
	print("%ld%s",v,s);
	color(7);
}

stdm( char *s )
{
	eclrline(BOTTOM);
	locate(0,BOTTOM);
	color(5+AT_REVERSE);
	print("%s",s);
	color(7);
}

errm(char *s)
{
	eclrline(BOTTOM);
	locate(0,BOTTOM);
	color(4+AT_REVERSE);
	print("%s",s);
	color(7);
}

jump( long value )
{
	int		i;
	long		p;
	if ( value < fpoint || value >fpoint+LENONSCR-1) fpoint = value &~0xff;
	i = value-fpoint;
	curx = (i&0xf)*2;
	cury = i/16;
	df = 1;
}

getparam( char **pp,long *value )
{
	long		v;
	int		d;
	char		*p;

	if ( (v = getvalp(pp))!=0xffffffff ) {
		*value = v;
		return(1);
		}
	p = *pp;
	skipspc(&p);
	d =*p;
	switch(d) {
	case	'\'':
		if ( (v=mark[ tolower(*++p)-'a' ])==UNMARKED ) {
			errm("Unknown Mark");
			return(0);
			}
		*value = v;
		*pp=++p;
		return(1);
	case	'$':
		*value = flen-1;
		*pp=++p;
		return(1);
	case	'^':
		*value = 0;
		*pp= ++p;
		return(1);
	case	'.':
		*value = position();
		*pp= ++p;
		return(1);
	default	:
		*value = UNASSIGNED;
		return(1);
	}
}

search(int c)
{
	char	*p,buff[ 256 ];
	long	d;

	locate( 0,BOTTOM );
	eclraftcur();
	print("%c",c);
	cursor(1);
	linein( buff );
	p = buff;
	if (*p=='\0') return;
	if (*p==c) {
		++p;
		searchlen = 0;
		while( (d=gethexp(&p))!=UNASSIGNED ) {
			searchbuff[searchlen++]=(char )d;
			}
		searchmf = 0;
		}
	else	{
		strcpy( searchbuff,buff );
		searchlen = strlen( buff );
		searchmf = 1;
		}
}

disp_search()
{
	char	buff[ 100 ];
	char	buff2[ 100 ];
	int	i;

	if (searchmf)	{
		sprintf(buff,"Searching string: %s",searchbuff);
		stdm(buff);
		}
	else	{
		for (i=0;i<searchlen && i<16 ;i++) {
			sprintf(&buff[i*3],"%02x ",(unsigned int)searchbuff[i]);
			}
		sprintf(buff2,"Searching hex numbers : %s",buff);
		stdm( buff2 );
		}
}
			
searchreverse()
{
	long	basepos,pos;
	int		c,i;

	if (searchlen ==0 ) {
		errm(ERR_NOPREV);
		return;
		}
	disp_search();
	pos = position();
	if (pos == 0 || pos >=flen) pos = flen-1;
	else		pos--;
	basepos = pos;
	do {
	if (keyinp(0)&bit0) {
		errm("Aborted.");
		return;
		}
	fseek(fp,pos,0);
	i = 0;
	while( i<searchlen ) {
		c = searchbuff[ i ] &0xff;
		if (fgetc(fp)!=c) break;
		i++;
		}
	if (i == searchlen) {
		jump(pos);
		eclrline(BOTTOM);
		return;
		}
	if (pos == 0) pos = flen-1;
	else		pos--;
	} while( basepos != pos);
	errm("Fail");
}

searchforward()
{
	long		basepos,pos;
	int		c,i;

	if (searchlen ==0 ) {
		errm(ERR_NOPREV);
		return;
		}
	disp_search();
	pos = position();
	if (++pos >= flen ) pos = 0;
	basepos = pos;
	do {
	if (keyinp(0)&bit0) {
		errm("Aborted.");
		return;
		}
	fseek(fp,pos,0);
	i = 0;
	while( i<searchlen ) {
		c = searchbuff[ i ] &0xff;
		if (fgetc(fp)!=c) break;
		i++;
		}
	if (i == searchlen) {
		jump(pos);
		eclrline(BOTTOM);
		return;
		}
	if (++pos == flen) pos = 0;
	} while( basepos != pos);
	errm("Fail");
}

commandline()
{
	char		*p,buff[ 256 ];
	int		d;
	long		len,l,value1,value2,value3;

	locate( 0,BOTTOM );
	eclraftcur();
	cprint(':');
	cursor(1);
	linein( buff );
	p = buff;
	skipspc(&p);
	switch(*p) {
		case	'!':
			print("\n");
			system( p+1 );
			print("\n");
			stdm("[ Hit return to continue ]");
			get_a_char();
			repaint();
			return(-1);
		case	'q':
			return(quit(p+1));
		case	'w':
			return(wrt(p+1));
		}
	if ( getparam( &p ,&value1 )==0 ) return(-1);
	skipspc(&p);
	switch( *p ) {
	case	'j':
		jump(position()+value1);
		return(-1);
	case	's':
		string(value1,p+1);
		return(-1);
	case	'S':
		if (insert(value1,(long)strlen(p+1))) string(value1,p+1);
		return(-1);
	case	'R':
		readfilei(value1,p+1);
		return(-1);
	case	'r':
		readfile(value1,p+1);
		return(-1);
	case	'f':
		++p;
		if (!getparam(&p,&value2))	return(-1);
		while( *p==' ' ) ++p;
		if (*p==',') ++p;
		if (!getparam(&p,&value3))	return(-1);
		fill( value1,value2,value3 );
		return(-1);
	case	'd':
		++p;
		if (!getparam(&p,&value2))	return(-1);
		if (value2==UNASSIGNED) value2=1;
		delete( value1,value2 );
		return(-1);
	case	'i':
		++p;
		if (!getparam(&p,&value2))	return(-1);
		while( *p==' ' ) ++p;
		if (*p==',') ++p;
		if (!getparam(&p,&value3))	return(-1);
		if (value2==UNASSIGNED) value2=1;
		if (insert( value1,value2 )) {
			fill( value1,value2 ,value3 );
			stdmv(value2," Bytes inserted.");
			}
		return(-1);
	case	',':
		++p;
		if (!getparam( &p, &value2 ))	return(-1);
		if (value2<value1) {
			errm("Invalid Range");
			return(-1);
			}
		while( *p==' ' ) ++p;
		switch( tolower(*p) ) {
		case	'y':
			yank( value1,value2-value1+1 );
			return(-1);
		case	'c':
			++p;
			if (!getparam( &p,&value3 ))	return(-1);
			yank( value1,value2-value1+1 );
			paste( value3 );
			return(-1);
		case	'm':
			++p;
			if (!getparam( &p,&value3 ))	return(-1);
			mv( value1,value2-value1+1,value3 );
			return(-1);
		case	'w':
			writefile( value1,value2-value1+1,++p );
			return(-1);
		case	'd':
			delete( value1,value2-value1+1 );
			return(-1);
		case	'f':
			++p;
			if (!getparam( &p,&value3 ))	return(-1);
			fill( value1,value2-value1+1,value3 );
			return(-1);
		default	:
			errm(ERR_UNRECO);
			return(-1);
		}
	case	'\0':
		if (value1==UNASSIGNED) value1= position();
		jump( value1 );
		return(-1);
	default	:
		errm(ERR_UNRECO);
		return(-1);
	}
}

int	wrt(char *p)
{
	long	l;
	int	ovw;
	char	buff[ 100 ];
	char	*fnp;
	if (*p =='q' && *(p+1)==0 ) return(1);
	skipspc(&p);
	ovw = *p=='!';
	p+=ovw;
	skipspc(&p);
	if (*p==0)	{
		fnp = srcfilename;
		modified = 0;
		}
	else	{
		fnp = p;
		if ( exist(fnp) && !ovw ) {
			errm(ERR_FLEXIST);
			return(-1);
			}
		}
	l = filelength(handle);
	fclose(fp);
	move(filename,fnp);
	sprintf(buff,"\"%s\" %d byte(s).",pointfn(fnp),l);
	stdmm(buff);
	fp = fopen(filename,"r+b");
	handle = fileno(fp);
	return(-1);
}

int	quit(char *p)
{
	if (*p==0) {
		if (modified)	{
			errm("No write since last change (use ! to override)");
			return(-1);
			}
		return(1);
		}
	if (*p=='!' && *(p+1)==0) return(0);
	errm(ERR_UNRECO);
	return(-1);
}

long filecopy( FILE *sp,FILE *dp, long sfp,long dfp,long len)
{
	char	buff[ 0x4000 ];
	int	i;
	long	v;

	if (sfp!=UNASSIGNED) fseek(sp,sfp,0);
	if (dfp!=UNASSIGNED) fseek(dp,dfp,0);
	v =0;
	for(i=0;i<(len/0x4000);i++) {
		fread(buff,1,0x4000,sp);
		v+=fwrite(buff,1,0x4000,dp);
		}
	fread(buff,1,len & 0x3fffl,sp);
	v+=fwrite(buff,1,len & 0x3fffl,dp);
	return(v);
}

mv( long src, long len, long dst)
{
	FILE		*fpd,*fpr;
	long		l,m,addl;
	int		i,c;

	if (src == UNASSIGNED) src = position();
	if (len == UNASSIGNED) len = 1;
	if (dst == UNASSIGNED) dst = position();

	if ( dst>= src && dst< src+len ) {
		errm("Move to moved address.");
		return(0);
		}
	if ( src >=flen ) {
		errm("No data");
		return(0);
		}

	addl = dst>=flen?dst-flen:0;

	if (!fillblank( dst,0 ))	return(0);
	if ((fpd = fopen( tempfn,"wb"))==NULL) {
		errm("Can't read. Working file open error.");
		return(0);
		}
	if (addl> diskfree(drive)) goto error;
	if (src<dst) {
		if (dst-src+addl > diskfree(0)) goto error;
		filecopy( fp,fpd,src+len,UNASSIGNED, dst-(src+len));
		filecopy( fp,fpd,src,UNASSIGNED,len);
		fseek(fp,src,0);
		}
	else	{
		if (len+src-dst+addl> diskfree(0)) goto error;
		filecopy( fp,fpd,src,UNASSIGNED,len);
		filecopy( fp,fpd,dst,UNASSIGNED,src-dst);
		fseek(fp,dst,0);
		}
		
	fclose(fpd);
	fpd = fopen(tempfn,"rb");
	filecopy( fpd,fp,UNASSIGNED,UNASSIGNED,filelength(fileno(fpd)));
	fclose(fpd);
	unlink(tempfn);
	for(i=0;i<26;i++) {
		m = mark[i];
		if (m!=UNMARKED) {
			if (src<dst) {
				if ( m >= src && m< src+len ) m+=dst-(src+len);
				else if ( m >= src+len && m<dst ) m-= len;
				}
			else	{
				if ( m>=dst && m< src ) m+=len;
				else if ( m>=src && m< src+len ) m-= src-dst;
				}
			}
		mark[i] = m;
		}
	if (src<dst) jump(dst-len);
	else	jump(dst);
	df = 1;
	stdmv( len," Bytes moved.");
	modified = 1;
	return(1);

error:
	errm(ERR_NOWKSPACE);
	fclose(fpd);
	return(0);
}


fill(  long v1, long len, long data)
{
	long		idx;

	if (v1 	== UNASSIGNED)	v1= position();
	if (len == UNASSIGNED)	len = 1;
	if (data == UNASSIGNED) data =0;
	if (v1+len - flen > diskfree(drive)) {
		errm(ERR_NOWKSPACE);
		return(0);
		}
	if (!fillblank( v1,(int) data )) return(0);
	fseek( fp,v1,0 );
	for(idx = len; idx ; idx-- ) 
		if (fputc((int)data,fp)==EOF) break;
	if (idx) {
		errm("Fill incomplete. Make sure and press return.");
		get_a_char();
		}
	stdmv( len," Bytes Filled." );
	modified = 1;
	df = 1;
	jump(v1+len);
	return(1);
}

string( long pos,char *p)
{
	if (pos ==UNASSIGNED) pos = position();
	if ( pos+strlen(p) - flen > diskfree(drive)) {
		errm(ERR_NOWKSPACE);
		return(0);
		}
	fillblank( pos ,0);
	fseek(fp,pos,0);
	while(*p) {
		fputc(*p++,fp);
		++pos;
		}
	jump(pos);
	df = 1;
	modified = 1;
	return(1);
}

insert( long pos, long ilength)
{
	FILE		*fpd,*fpr;
	long		len;
	int		i,c;
	char		buff[100];

	if (pos == UNASSIGNED) pos = position();
	if (ilength == UNASSIGNED) ilength = 1;
	if (flen<pos) return(1);

	if ( pos > flen ) {
		if (pos+ilength > diskfree(0)) goto error;
		if (pos+ilength - flen >diskfree(drive)) goto error;
		}
	else {
		if (flen+ilength > diskfree(0)) goto error;
		if (ilength > diskfree(drive)) goto error;
		}

	if (!fillblank( pos,0 )) return(0);
	if ((fpd = fopen( tempfn,"wb"))==NULL) {
		errm("Can't read. Working file open error.");
		return(0);
		}
	filecopy( fp,fpd,0l,0l,pos );
	filecopy( fp,fpd,UNASSIGNED,pos+ilength,MAXLENGTH);
	fcloseall();
	unlink(filename);
	move(tempfn,filename);
	fp = fopen(filename,"rb+");
	handle = fileno(fp);
	flen = filelength(handle);
	for(i=0;i<26;i++)
		if (mark[i]>pos && mark[i]!=UNMARKED ) mark[i]+=ilength;
	modified = 1;
	return(1);
error:
	errm(ERR_NOWKSPACE);
	return(0);
}

writefile( long v1, long len,char *p )
{
	FILE		*fp2;
	int		c;
	int		ovr;

	if (len > diskfree(drvnum(p))) {
		errm(ERR_NOWKSPACE);
		return(0);
		}
	skipspc(&p);
	ovr = *p=='!';
	p+=ovr;
	skipspc(&p);
	if (exist(p) && !ovr) {
		errm(ERR_FLEXIST);
		return(0);
		}
	if ((fp2 = fopen( p,"wb"))==NULL ) {
		errm("Can't open the destination file.");
		return(0);
		}
	filecopy(fp,fp2,v1,0l,len);
	fclose(fp2);
	stdmv(len," Bytes Written.");
	return(1);
}

readfile( long rpos, char *p)
{
	FILE		*fpr;
	long		len,rlen;
	int		c;

	if (rpos == UNASSIGNED) rpos = position();
	while(*p==' ') p++;
	if ((fpr = fopen( p,"rb"))==NULL) {
		errm("No such file.");
		return(0);
		}

	rlen = filelength(fileno(fpr));
	if ( rpos <=flen && rpos+rlen>flen ) {
		if (rpos+rlen - flen > diskfree(drive)) goto error;
		}
	else if ( rpos>flen ) {
		if (rpos+rlen - flen > diskfree(drive)) goto error;
		}
		
	fillblank( rpos,0 );
	len = filecopy(fpr,fp,0l,rpos,MAXLENGTH);
	fclose(fpr);
	handle = fileno(fp);
	jump(rpos+len);
	df = 1;
	stdmv( len," Bytes overwritten.");
	modified = 1;
	return(1);

error:
	errm(ERR_NOWKSPACE);
	fclose(fpr);
	return(0);
}

readfilei( long rpos, char *p)
{
	FILE		*fpd,*fpr;
	long		idx,len,rlen;
	int		c,i;
	char		buff[100];

	if (rpos == UNASSIGNED) rpos = position();
	while(*p==' ') p++;
	if ((fpr = fopen( p,"rb"))==NULL) {
		errm("No such file.");
		return(0);
		}
	if ((fpd = fopen( tempfn,"wb"))==NULL) {
		errm("Can't read. Working file open error.");
		return(0);
		}
	rlen = filelength(fileno(fpr));
	if (rpos > flen ) {
		if ( rpos - flen + rlen > diskfree(drive)) goto error;
		if ( rpos + rlen > diskfree(0)) goto error;
		}
	else	{
		if ( rlen > diskfree(drive)) goto error;
		if ( flen + rlen > diskfree(0)) goto error;
		}

	fillblank( rpos,0 );
	filecopy( fp,fpd,0l,0l,rpos);
	len = filecopy( fpr,fpd,UNASSIGNED,UNASSIGNED,MAXLENGTH );
	filecopy( fp,fpd,UNASSIGNED,UNASSIGNED,MAXLENGTH );
	fcloseall();
	unlink(filename);
	move(tempfn,filename);
	fp = fopen(filename,"rb+");
	handle = fileno(fp);
	jump(rpos+len);
	for(i=0;i<26;i++)
		if (mark[i]>rpos && mark[i]!=UNMARKED ) mark[i]+=len;
	df = 1;
	stdmv( len," Bytes inserted.");
	modified = 1;
	return(1);
error:
	errm(ERR_NOWKSPACE);
	fclose(fpd);
	fclose(fpr);
	return(0);
}

delete( long pos, long len )
{
	FILE		*fpd;
	long		idx,datlen;
	int		c,i;

	if (pos == UNASSIGNED) pos = position();
	yank( pos,len );

	if ( pos+len < flen ) {
		if (flen-len > diskfree(0)) {
			errm(ERR_NOWKSPACE);
			return(0);
			}
		if ((fpd = fopen( tempfn,"wb"))==NULL) {
			errm("Can't delete. Working file open error.");
			return(0);
			}
		filecopy(fp,fpd,0l,0l,pos);
		filecopy(fp,fpd,pos+len,UNASSIGNED,MAXLENGTH);
		fcloseall();
		unlink(filename);
		move(tempfn,filename);
		fp = fopen(filename,"rb+");
		handle = fileno(fp);
		}
	else	chsize( handle,pos );

	jump(pos);
	for(i=0;i<26;i++)
		if (mark[i]>pos && mark[i]!=UNMARKED) mark[i]-=len;
	df = 1;
	stdmv( len," Bytes Deleted.");
	modified = 1;
}

paste( long pos )
{
	int		c;

	if (pos == UNASSIGNED ) pos = position();
	if (!yanked) {
		errm("Yank buffer empty.");
		return(0);
		}

	if ( pos <=flen && pos+yanklen>flen ) {
		if (pos+yanklen - flen > diskfree(drive)) goto error;
		}
	else if ( pos>flen ) {
		if (pos+yanklen - flen > diskfree(drive)) goto error;
		}

	fillblank( pos,0 );
	fseek(fp,pos,0);
	if (yanked == 1) {
		write( handle,yankbuffer,yanklen);
		stdmv( yanklen," Bytes overwritten.");
		}
	else readfile(pos,yankfn);
	modified = 1;
	jump(pos+yanklen);
	df = 1;
	return(1);

error:
	errm(ERR_NOWKSPACE);
	return(0); 
}

pastei( long pos)
{
	int		c;

	modified = 1;
	if (!yanked) {
		errm("Yank buffer empty.");
		return(0);
		}
	if (!fillblank( pos,0 )) return(0);
	if (yanked == 1) {
		if (!insert(pos,(long)yanklen)) return(0);
		if ( pos+yanklen - flen > diskfree(drive)) {
			errm(ERR_NOWKSPACE);
			return(0);
			}
		fseek(fp,pos,0);
		write( handle,yankbuffer,(int)yanklen );
		stdmv( yanklen," Bytes inserted.");
		jump(pos+yanklen);
		} else readfilei(pos,yankfn);
	jump(pos+yanklen);
	df = 1;
}


yank( long v1, long len )
{
	long		idx;
	int		c;
	if ( len >= sizeof(yankbuffer) ) {
		if (len > diskfree(0)) {
			errm(ERR_NOWKSPACE);
			return(0);
			}
		yankfp = fopen(yankfn,"wb");
		yanklen = len;
		filecopy( fp,yankfp,v1,0l,len );
		fclose(yankfp);
		yanked = 2;
		}
	else	{
		fseek( fp,v1,0 );
		yanklen = (long)fread( yankbuffer,1,(int) len,fp );
		yanked = 1;
		}
	if (len != yanklen) {
		errm("No that length, Make sure and press return.");
		get_a_char();
		}
	stdmv(yanklen," Bytes Yanked.");
	return(1);
}

fillblank( long v,int c )
{
	long	t;
	t = flen;
	if ( (v-t) > diskfree(drive)) {
		errm(ERR_NOWKSPACE);
		return(0);
		}
	fseek( fp,t,0); 
	while( t<v ) {
		if (keyinp(0)&bit0) {
			errm("Aborted.");
			return(0);
			}
		if (fputc(c,fp)==EOF) {
			errm("Disk i/o error");
			clearerr(fp);
			return(0);
			}
		t++;
		}
	flen = filelength(handle);
	return(1);
}


hexdmp(long offset,unsigned char *p, int l,int line )
{
	char		hex_buf[ 58 ],asc_buf[ 18 ];
	int		i,lsave;
	unsigned char	c,d,*psave;
	int		kf;
	if (line == 0) {
		line = l/16;
		}
	kf = 0;
	while ( line!=0 ) {
		memset( hex_buf,' ',58 );
		memset( asc_buf,' ',18 );
		hex_buf[ 57 ] = '\0';
		asc_buf[ 17 ] = '\0';
	sprintf( hex_buf,"%08lX",offset&~0xf);
	hex_buf[ 8 ] = ' ';
	lsave = l;
	psave = p;
	for (i=offset&0xf;i<16;i++) {
		sprintf( &(hex_buf[ 9 + i*3 ]),l?"%02X":"~~",*p++);
		hex_buf[ 11 + i*3 ] = ' ';
		if (l)	--l;
		}
	l = lsave;
	p = psave;
	for (i=offset&0xf;i<16;i++) {
		c = *p++;
		if (l)
		switch ( kf ) {
			case 0 :
				if ( c>=0x81 && c<=0x9f || c>=0xe0 && c<=0xfc) {
					if (l<2) {
						asc_buf[ i ] = '.';
						break; }
					if ((d =*p)>=0x40 &&
					    d<=0x7e || d>=0x80 &&
					    d<=0xfc) {
						kf = 1;
						asc_buf[ i ] = c;
						break; }
					else
						asc_buf[ i ] = '.';
						break; }
				asc_buf[ i ] = c<' '?'.':c;
				break;
			case 1 :
				asc_buf[ i ] = c;
				kf = 0;
				break;
			case 2 :
				asc_buf[ i ] = '.';
				kf = 0;
				break;
			}
		else	asc_buf[ i ] = '~';
		offset++;
		if (l)	--l;
		}
	if ( l && kf == 1 ) {
		asc_buf[ 16 ] = *p;
		asc_buf[ 17 ] = 0;
		kf = 2;
		}
	print("%s %s\n",hex_buf,asc_buf);
	line--;
	}
}

move(char *s,char *d)
{
	char	buff[100];

	sprintf(buff,"copy %s %s >nul",s,d);
	system(buff);
	unlink(s);
}

