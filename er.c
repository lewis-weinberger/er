#ifdef __linux__
#	define _XOPEN_SOURCE 600
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <regex.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#define CSI(ch) ("\x1b[" ch)
#define LEN(X)  (sizeof(X) / sizeof(X[0]))

#ifndef CTRL
#	define CTRL(X) ((X) & 0x1F)
#endif

/* misc constants */
enum
{
	Gaplen  = 256, /* number of bytes in a full gap */
	Vbufmax = 4096 /* number of bytes in screen buffer */
};

/* error handling status */
enum
{
	Ok,
	Panic, /* Unrecoverable errors */
	Reset  /* Recoverable errors */
};

/* keyboard keys */
enum
{
	Kesc = 0x1b,
	Kbs  = 0x7f, /* assume backspace sends DEL */
	Kdel = 0xE000,
	Kleft,
	Kright,
	Kup,
	Kdown,
	Khome,
	Kend,
	Kpgup,
	Kpgdown,
	Kins
};

/* editing mode */
enum
{
	Command,
	Input,
	Select
};

/* undo stack type */
enum
{
	Udelete,
	Uinsert,
	Uend     /* sentinel for sequence of changes */
};

typedef struct Change Change;
typedef struct Array Array;
typedef struct Buffer Buffer;

/* textual change */
struct Change
{
	short  type; /* undo stack type */
	size_t i;    /* byte offset of change */
	char   c;    /* value of change */
};

/* dynamic array */
struct Array
{
	void   *data; /* contents */
	size_t size;  /* element size */
	size_t len;   /* length */
	size_t cap;   /* capacity */
};

/* editing buffer */
struct Buffer
{
	char   *c;                  /* contents */
	char   path[PATH_MAX];      /* filename */
	Array  changes;             /* undo stack */
	short  dirty;               /* modified flag */
	size_t *lead, addr1, addr2; /* selection offsets */
	size_t cap, gap, start;     /* gap book-keeping */
	size_t vstart, vline;       /* display book-keeping */
};

Buffer                bufs[32], *buf;
Array                 ybuf, bbuf, dbuf;
char                  ch[5], vbuf[Vbufmax];
size_t                vbuflen, current, nbuf;
struct winsize        dim;
jmp_buf               env;
const char            invalid[] = "�";
short                 mode, refresh, quit, usetabs, tabspace, autoindent;
sigset_t              oset;
volatile sig_atomic_t status;
struct termios        term;

/* convert logical byte offset to internal byte offset */
size_t
bufaddr(size_t in)
{
	return (in >= buf->start) ? in + buf->gap : in;
}

/* throw error */
void
err(int n)
{
	status = n;
	siglongjmp(env, 1);
}

/* number of decimal digits */
int
digits(long v)
{
	return (v == 0) ? 1 : floor(log10((double)v)) + 1;
}

/* get current terminal dimensions */
int
dims(void)
{
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &dim) == -1 || dim.ws_col == 0)
		return -1;
	return 0;
}

/* read a byte from STDIN (see terminit() for timeout) */
int
readbyte(void)
{
	int n;
	unsigned char c;

	if((n = read(STDIN_FILENO, &c, 1)) == -1){
		if(errno == EINTR)
			n = 0;
		else
			err(Panic);
	}
	if(n == 0)
		return -1;
	return c;
}

/* read a byte stream until a complete multibyte character is found */
int
decode(char first, int (*m1)(size_t*), int (*m2)(void), size_t *i, wchar_t *wc)
{
	int n, r;
	mbstate_t ps;

	for(n = 1, ch[0] = first; n < 5; n++){
		memset(&ps, 0, sizeof(ps));
		switch (mbrtowc(wc, ch, n, &ps)){
		case (size_t)-2:
			if((r = (m1 != NULL) ? m1(i) : m2()) == -1)
				return -1;
			ch[n] = r;
			break;
		case (size_t)-1:
			return -1;
		default:
			return first;
		}
	}
	return -1;
}

/* read a character from STDIN */
int
parsechar(char c)
{
	if(decode(c, NULL, readbyte, NULL, NULL) == -1)
		err(Reset);
	return c;
}

/* get user input from keyboard */
int
key(void)
{
	int k, l, m, n;
	size_t i;
	static const struct
	{
		int a;
		int b;
		int c;
		int out;
	} vt[] = {
		/* VT100 */
		{ 'O', 'A',  -1,     Kup },
		{ 'O', 'B',  -1,   Kdown },
		{ 'O', 'C',  -1,  Kright },
		{ 'O', 'D',  -1,   Kleft },
		/* VT220 */
		{ '[', 'A',  -1,     Kup },
		{ '[', 'B',  -1,   Kdown },
		{ '[', 'C',  -1,  Kright },
		{ '[', 'D',  -1,   Kleft },
		{ '[', '1', '~',   Khome },
		{ '[', '2', '~',    Kins },
		{ '[', '3', '~',    Kdel },
		{ '[', '4', '~',    Kend },
		{ '[', '5', '~',   Kpgup },
		{ '[', '6', '~', Kpgdown },
		/* xterm */
		{ 'O', 'H',  -1,   Khome },
		{ 'O', 'F',  -1,    Kend },
		/* rxvt */
		{ '[', '7', '~',   Khome },
		{ '[', '8', '~',    Kend },
		/* SCO ANSI */
		{ '[', 'H',  -1,   Khome },
		{ '[', 'F',  -1,    Kend }
	};

	memset(ch, 0, 5);
	if((k = readbyte()) == -1)
		return -1;
	if(k == Kesc){
		if((l = readbyte()) != -1 && (m = readbyte()) != -1){
			for(i = 0; i < LEN(vt); i++){
				if(l == vt[i].a && m == vt[i].b && vt[i].c == -1)
					return vt[i].out;
			}
			if((n = readbyte()) != -1){
				for(i = 0; i < LEN(vt); i++){
					if(l == vt[i].a && m == vt[i].b && n == vt[i].c)
						return vt[i].out;
				}
			}
		}
		return Kesc;
	}
	if(k == '\b' || k == Kbs)
		return Kbs;
	if (k <= 0x1f)
		return k;
	return parsechar(k);
}

/* length of current buffer */
size_t
len(void)
{
	return buf->cap - buf->gap;
}

/* next byte in the current buffer */
int
nextbuf(size_t *i)
{
	return (*i == len()) ? -1 : buf->c[bufaddr((*i)++)];
}

/* next character in the current buffer */
int
next(size_t *i)
{
	wchar_t wc;

	memset(ch, 0, 5);
	if(decode(buf->c[bufaddr((*i)++)], nextbuf, NULL, i, &wc) == -1){
		memcpy(ch, invalid, sizeof(invalid));
		return 1;
	}
	if(wc == '\t')
		return 8;
	if(wc == '\n')
		return 1;
	return wcwidth(wc);
}

/* end of line in current buffer */
int
eol(size_t *i)
{
	int r;

	r = 0;
	if(*i < len()){
		do{
			next(i);
			r++;
		}while(*i < len() && buf->c[bufaddr(*i)] != '\n');
	}
	return r;
}

/* previous character in current buffer */
int
prev(size_t *i)
{
	size_t m, n;
	int r;

	n = r = 0;
	if(*i > 0){
		do{
			n++;
			m = *i - n;
			r = next(&m);
		}while(m > 0 && n < 4 && strncmp(ch, invalid, 5) == 0);
		*i = (*i < n) ? 0 : *i - n;
	}
	return r;
}

/* start of line in current buffer */
int
sol(size_t *i)
{
	int r;

	r = 0;
	if (*i > 0) {
		do{
			prev(i);
			r++;
		}while(*i > 0 && buf->c[bufaddr(*i)] != '\n');
	}
	return r;
}

/* next line in current buffer */
void
nextline(size_t *i)
{
	size_t m, n;

	m = *i;
	n = sol(&m);
	if(m > 0)
		n--;
	if(buf->c[bufaddr(*i)] != '\n')
		eol(i);
	if(*i < len() - 1){
		next(i);
		while(n && buf->c[bufaddr(*i)] != '\n' && *i < len() - 1){
			next(i);
			n--;
		}
	}
}

/* previous line in current buffer */
void
prevline(size_t *i)
{
	size_t n;

	if(*i > 0){
		n = sol(i) - 1;
		if (*i == 0)
			return;
		sol(i);
		if(buf->c[bufaddr(*i)] == '\n')
			next(i);
		while(n && buf->c[bufaddr(*i)] != '\n' && *i < len() - 1){
			next(i);
			n--;
		}
	}
}

/* check if displayed subset of current buffer needs to be updated */
int
checkline(int dir)
{
	int n, r;
	size_t i;

	n = 1;
	r = 0;
	if(dir){
		for(i = buf->vstart; i < buf->addr2; i++){
			if (buf->c[bufaddr(i)] == '\n')
				n++;
		}
		r = n;
		n -= dim.ws_row - 1;
		while(n-- > 0){
			nextline(&buf->vstart);
			buf->vline++;
		}
	}else{
		while(buf->addr1 < buf->vstart){
			prevline(&buf->vstart);
			buf->vline--;
		}
	}
	return r;
}

/* reposition current buffer's gap ready for insertion/deletion */
void
move(size_t i)
{
	size_t j, dst, src, num;

	j = bufaddr(i);
	if(j != buf->start + buf->gap){
		dst = (j < buf->start) ? j + buf->gap : buf->start;
		src = (j < buf->start) ? j : buf->start + buf->gap;
		num = (j < buf->start) ? buf->start - j : j - (buf->gap + buf->start);
		memmove(buf->c + dst, buf->c + src, num);
		buf->start = (j < buf->start) ? j : j - buf->gap;
	}
}

/* expand allocated memory for dynamic array */
void
resize(Array *a)
{
	void *new;

	new = realloc(a->data, a->size * 2 * a->cap);
	if(new == NULL)
		err(Panic);
	a->data = new;
	a->cap *= 2;
}

/* append new item to the end of dynamic array */
#define APPEND(A, T, E) do{                 \
	if((A)->len == (A)->cap)            \
		resize(A);                  \
	((T *)(A)->data)[(A)->len++] = (E); \
}while(0)

/* append new textual change to the current buffer's undo stack */
void
record(short t, size_t i, char c) {
	Change x = { t, i, c };
	APPEND(&buf->changes, Change, x);
}

/* reallocate memory for current buffer's gap */
void
grow(void)
{
	char *new;

	new = realloc(buf->c, (buf->cap + Gaplen));
	if(new == NULL)
		err(Panic);
	buf->c = new;
	buf->start = buf->cap;
	buf->gap = Gaplen;
	buf->cap += Gaplen;
}

/* insert byte into current buffer, optionally recording on undo stack */
void
insert(size_t i, char c, int r)
{
	if(buf->gap == 0)
		grow(); /* please mind the gap */
	move(i);
	buf->c[buf->start++] = c;
	buf->gap--;
	buf->dirty = 1;
	if(r)
		record(Uinsert, i, 0);
}

/* delete byte from current buffer, optionally recording on undo stack */
void
delete(size_t i, int r)
{
	char c;

	move(i);
	c = buf->c[buf->start + buf->gap++];
	buf->dirty = 1;
	if(r)
		record(Udelete, i, c);
}

/* (de/in)dent selected lines in current buffer */
void
indent(size_t *a, size_t *b, int fwd)
{
	int k;
	size_t i;

	i = *a;
	if(i > 0){
		sol(&i);
		if(buf->c[bufaddr(i)] == '\n' && i < len() - 1)
			next(&i);
	}
	while(i <= *b && i < len() - 1){
		k = tabspace;
		while(k-- > 0){
			if(fwd){
				if(i <= *a)
					(*a)++;
				insert(i, usetabs ? '\t' : ' ', 1);
				(*b)++;
			}else{
				if(len() > 0 && i < len() &&
				   buf->c[bufaddr(i)] == (usetabs ? '\t' : ' ')){
					delete(i, 1);
					if(i <= *a)
						(*a)--;
					(*b)--;
				}
			}
		}
		if(buf->c[bufaddr(i)] != '\n')
			eol(&i);
		if(buf->c[bufaddr(i)] == '\n' && i < len() - 1)
			next(&i);
	}
	record(Uend, 0, 0);
}

/* insert newline and automatically match previous indentation */
void
newline(size_t *a, int O)
{
	size_t b;

	b = *a;
	if(O && *a == 0){
		insert(0, '\n', 1);
		return;
	}
	insert((*a)++, '\n', 1);
	if(autoindent){
		sol(&b);
		if(buf->c[bufaddr(b)] == '\n' && b < len())
			next(&b);
		while(buf->c[bufaddr(b++)] == (usetabs ? '\t' : ' ') && b < len())
			insert((*a)++, usetabs ? '\t' : ' ', 1);
	}
}

/* attempt to open file and read contents into buffer */
int
fileinit(Buffer *b)
{
	struct stat st;
	size_t n;
	ssize_t k;
	char *p;
	int fd;

	fd = -1;
	n = 0;
	if((fd = open(b->path, O_RDWR | O_CREAT, 0666)) > 0 && fstat(fd, &st) != -1)
		n = st.st_size;
	b->c = calloc(n + Gaplen, 1);
	if(b->c != NULL){
		b->cap = n + Gaplen;
		b->start = n;
		b->gap = Gaplen;
		if(fd > 0){
			p = b->c;
			while(n){
				k = read(fd, p, n);
				if(k == -1){
					free(b->c);
					close(fd);
					return -1;
				}
				p += k;
				n -= k;
			};
			close(fd);
		}
		return 0;
	}
	if(fd > 0)
		close(fd);
	return -1;
}

/* signal handler */
void
sig(int n)
{
	switch(n){
	case SIGINT:
	case SIGWINCH:
		err(Reset);
		break;
	case SIGTERM:
	case SIGQUIT:
		err(Panic);
	}
}

/* block all incoming signals */
void
sigpend(void)
{
	sigset_t mask;

	if(sigfillset(&mask) != -1 && sigprocmask(SIG_SETMASK, &mask, &oset) != -1)
		return;
	perror("sigpend");
	exit(1);
}

/* install signal handler and unblock signals */
void
siginit(void)
{
	size_t i;
	struct sigaction sa;
	static const int siglist[] = { SIGINT, SIGWINCH, SIGTERM, SIGQUIT };

	sa.sa_handler = sig;
	sa.sa_flags = 0;
	if(sigfillset(&sa.sa_mask) != -1){
		for(i = 0; i < LEN(siglist); i++){
			if(sigaction(siglist[i], &sa, NULL) == -1)
				goto Error;
		}
		if(sigprocmask(SIG_SETMASK, &oset, NULL) != -1)
			return;
	}
Error:
	perror("siginit");
	exit(1);
}

/* set terminal into raw mode */
void
terminit(void)
{
	struct termios new;

	if(tcgetattr(STDIN_FILENO, &new) != -1){
		term = new;
		new.c_iflag &= ~(BRKINT | ISTRIP | INLCR | INPCK | IXON);
		new.c_oflag &= ~OPOST;
		new.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
		new.c_cflag |= CS8;
		new.c_cc[VMIN] = 0;
		new.c_cc[VTIME] = 1; /* ds */
		if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &new) != -1)
			return;
	}
	perror("terminit");
	exit(1);
}

/* allocate memory for dynamic array */
int
arrinit(Array *a, size_t size)
{
	a->data = calloc(Gaplen, size);
	if(a->data == NULL)
		return -1;
	a->size = size;
	a->len = 0;
	a->cap = Gaplen;
	return 0;
}

/* free memory for dynamic array */
void
arrfree(Array *a)
{
	free(a->data);
}

/* initialise buffer for given file */
int
bufinit(int i, const char *path)
{
	bufs[i].addr1 = bufs[i].addr2 = bufs[i].vstart = bufs[i].vline = 0;
	bufs[i].lead = &bufs[i].addr2;
	bufs[i].dirty = 0;
	strncpy(bufs[i].path, path, PATH_MAX);
	if(arrinit(&bufs[i].changes, sizeof(Change)) != -1){
		if(fileinit(&bufs[i]) != -1)
			return 0;
		arrfree(&bufs[i].changes);
	}
	return -1;
}

/* free memory for buffer */
void
buffree(Buffer *b)
{
	arrfree(&b->changes);
	free(b->c);
}

/* initialise all components of the editor */
void
init(int n, char **paths)
{
	int i;

	sigpend();
	setlocale(LC_ALL, "");
	for(i = 0; i < n; i++){
		if((bufinit(i, paths[i + 1])) == -1)
			goto Error;
	}
	if(arrinit(&ybuf, 1) == -1)
		goto Error;
	if(arrinit(&bbuf, 1) == -1)
		goto Error;
	if(arrinit(&dbuf, 1) == -1)
		goto Error;
	terminit();
	siginit();
	return;
Error:
	perror("init");
	exit(1);
}

/* return the terminal to original state */
void
termreset(void)
{
	(void)!write(STDOUT_FILENO, CSI("9999;1H\r\n"), 11);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);
}

/* deinitialise all components of the editor */
void
end(void)
{
	size_t i;

	termreset();
	for(i = 0; i < nbuf; i++)
		buffree(&buf[i]);
	arrfree(&ybuf);
	arrfree(&bbuf);
	arrfree(&dbuf);
}

/* revert last sequence of changes, popping from top of undo stack */
void
undo(size_t *a, size_t *b)
{
	Change m;

	if(buf->changes.len > 0){
		buf->changes.len--;
		while(buf->changes.len > 0){
			m = ((Change *)buf->changes.data)[--buf->changes.len];
			switch(m.type){
			case Uinsert:
				delete(m.i, 0);
				*a = *b = m.i;
				break;
			case Udelete:
				insert(m.i, m.c, 0);
				*a = *b = m.i;
				break;
			case Uend:
				buf->changes.len++;
				return;
			}
		}
	} else /* no more changes */
		buf->dirty = 0;
}

/* write contents of byte string to file */
ssize_t
writeall(int f, const char *s, size_t n)
{
	int w;
	ssize_t r;

	r = n;
	while(n){
		w = write(f, s, n);
		if(w == -1){
			if(errno == EINTR)
				w = 0;
			else
				return -1;
		}
		s += w;
		n -= w;
	}
	return r;
}

/* write entire contents of current buffer to file */
ssize_t
writef(int f)
{
	ssize_t n, r;

	n = writeall(f, buf->c, buf->start);
	if(n == -1)
		return -1;
	r = writeall(f, buf->c + buf->start + buf->gap, len() - buf->start);
	if(r == -1)
		return -1;
	return r + n;
}

/* emergency backup in case of panic */
void
save(void)
{
	int fd;
	size_t i;

	fd = creat("er.out", 0666);
	if(fd > 0){
		for(i = 0; i < nbuf; i++){
			buf = &bufs[i];
			writef(fd);
		}
		close(fd);
	}
}

/* flush contents of screen buffer to STDOUT */
void
vflush(void)
{
	if(writeall(STDOUT_FILENO, vbuf, vbuflen) == -1)
		err(Panic);
	vbuflen = 0;
}

/* append to screen buffer */
void
vpush(int n, ...)
{
	int i;
	char *s;
	va_list args;

	va_start(args, n);
	for(i = 0; i < n; i++){
		s = va_arg(args, char*);
		while(*s){
			if(vbuflen == Vbufmax)
				vflush();
			vbuf[vbuflen++] = *s++;
		}
	}
	va_end(args);
}

/* position cursor */
void
cursor(unsigned int x, unsigned int y)
{
	char tmp[32];
	if(snprintf(tmp, sizeof(tmp), CSI("%u;%uH"), y + 1 , x + 1) < 0)
		err(Panic);
	vpush(1, tmp);
}

/* draw message to status bar */
void
bar(const char *fmt, ...){
	int n;
	va_list args;

	while(bbuf.cap < (size_t)dim.ws_col + 1)
		resize(&bbuf);
	va_start(args, fmt);
	n = vsnprintf(bbuf.data, dim.ws_col + 1, fmt, args);
	va_end(args);
	cursor(0, dim.ws_row - 1);
	vpush(2, CSI("36m"), bbuf.data);
	if(n > dim.ws_col){
		cursor(dim.ws_col - 2, dim.ws_row - 1);
		vpush(2, CSI("35m>"), CSI("0m"));
	}
	vpush(2, CSI("K"), CSI("0m"));
	vflush();
}

/* start dialogue prompt in status bar */
int
dialogue(const char *prompt)
{
	char *s;
	int k;

	bar("%s%s", prompt, dbuf.data);
	while((k = key()) != '\n'){
		if(k == -1)
			continue;
		else if(k == Kesc){
			bar("");
			return -1;
		}else if(k == Kbs && dbuf.len > 0)
			while(dbuf.len)
				((char *)dbuf.data)[--dbuf.len] = 0;
			/* TODO: single character backspace (handling multibyte properly) */
		else{
			s = ch;
			while (*s)
				APPEND(&dbuf, char, *s++);
		}
		bar("%s%s", prompt, dbuf.data);
	}
	return 0;
}

/* search for regular expression in current buffer */
void
search(size_t *a, size_t *b, int replace, int all)
{
	regex_t reg;
	regmatch_t m[1];
	char err[128], *s;
	int r;
	size_t n, k;

	if(dialogue(replace ?
	            (all? "Replace all: " : "Replace: ") : "Search: ") == -1)
		return;
	r = regcomp(&reg, dbuf.data, REG_EXTENDED | REG_NEWLINE);
	k = 0;
	if(r == 0){
		if(replace && dialogue("with: ") == -1){
			regfree(&reg);
			return;
		}
		do{
			move(len()); /* please mind the gap */
			r = regexec(&reg, buf->c + *b, 1, m, 0);
			if(r == 0 && *b + m[0].rm_so < len()){
				*a = *b + m[0].rm_so;
				*b += m[0].rm_eo - 1;
				if(replace){
					n = 1 + *b - *a;
					while(n-- > 0)
						delete(*a, 1);
					*b = *a;
					s = dbuf.data;
					while(*s)
						insert((*b)++, *s++, 1);
					*a = *b;
					k++;
					record(Uend, 0, 0);
				}
			}else
				break;
		}while(all && r == 0);
	}
	bar("");
	if(r != 0){
		if(k > 0)
			bar("Replaced %ld matches", k);
		else{
			regerror(r, &reg, err, sizeof(err));
			bar(err);
		}
	}
	regfree(&reg);
	return;
}

/* display current buffer to terminal */
void
display(void)
{
	int i, j, l, i2, j2, n, h, jp, width;
	size_t k, kp;
	char tmp[32];

	vflush();
	l = digits(buf->vline + dim.ws_row);
	j2 = l + 2;
	h = 0;
Restart:
	for(i = jp = j = i2 = 0, kp = k = buf->vstart; i < dim.ws_row - 1; i++, jp = j = 0){
		cursor(j, i);
		if(k < len()){
			snprintf(tmp, sizeof(tmp), CSI("34m %*ld "), l, buf->vline + i);
			vpush(2, tmp, CSI("0m"));
			if(h > 0){
				cursor(0, i);
				vpush(2, CSI("35m<"), CSI("0m"));
				cursor(l + 2, i);
			}
			j = l + 2;
			if(buf->addr1 != buf->addr2 && k >= buf->addr1 && k <= buf->addr2)
				vpush(1, CSI("7m"));
			do{
				if(buf->addr1 != buf->addr2 && k == buf->addr1)
					vpush(1, CSI("7m"));
				if(kp == buf->addr2)
					vpush(1, CSI("0m"));
				if(k == *buf->lead){
					j2 = j;
					i2 = i;
				}
				kp = k;
				width = next(&k);
				jp += width;
				if(jp - width >= h){
					j = l + 2 + jp - h;
					if(j <= dim.ws_col){
						if(ch[0] == '\t') {
							for(n = 0; n < width; n++)
								vpush(1, " ");
						}else
							vpush(1, (ch[0] == '\n') ? " " : ch);
					}
				}else{
					for(n = 0; n < jp - h; n++)
						vpush(1, " ");
				}
				if(kp == *buf->lead && j > (dim.ws_col - 1)){
					h = j - (dim.ws_col - 1);
					vbuflen = 0;
					goto Restart;
				}
				if(ch[0] == '\n'){
					if(j > dim.ws_col - 1){
						cursor(0, i);
						vpush(2, CSI("35m>"), CSI("0m"));
						cursor(j, i);
					}
					break;
				}
			}while(k < len());
			if(k == *buf->lead){
				if(ch[0] == '\n'){
					j2 = l + 2;
					i2 = i + 1;
				}else{
					j2 = j;
					i2 = i;
				}
			}
		}else if(ch[0] == '\n'){
			snprintf(tmp, sizeof(tmp), CSI("36m %*ld "), l, buf->vline + i);
			vpush(2, tmp, CSI("0m"));
			memset(ch, 0, sizeof(ch));
		}else
			vpush(2, CSI("90m~"), CSI("0m"));
		vpush(1, CSI("K"));
	}
	vpush(1, CSI("?25h"));
	cursor(j2, i2);
	vflush();
}

/* copy selection in current buffer to yank buffer */
void
yank(void)
{
	size_t j, k, n;

	j = buf->addr1;
	k = buf->addr2;
	next(&k);
	n = k - buf->addr1;
	while(ybuf.cap < n)
		resize(&ybuf);
	ybuf.len = 0;
	while(ybuf.len < n)
		((char *)ybuf.data)[ybuf.len++] = buf->c[bufaddr(j++)];
	bar("%ld bytes yanked", n);
}

/* swap values of address variables */
void
swap(size_t *a, size_t *b){
	size_t tmp;

	tmp = *a;
	*a = *b;
	*b = tmp;
}

/* interpret key for motion within current buffer */
int
motion(int k)
{
	size_t tmp;

	refresh = 1;
	switch(k){
	case 'h':
		if(mode == Input)
			return -1; /* else fallthrough */
	case Kleft:
		if(buf->addr1 == buf->addr2)
			buf->lead = &buf->addr1;
		if(*buf->lead > 0){
			prev(buf->lead);
			if(mode != Select)
				buf->addr2 = buf->addr1;
			checkline(buf->lead == &buf->addr1 ? 0 : 1);
		}
		break;
	case 'l':
		if(mode == Input)
			return -1; /* else fallthrough */
	case Kright:
		if(buf->addr1 == buf->addr2)
			buf->lead = &buf->addr2;
		if(*buf->lead < len() - 1){
			next(buf->lead);
			if(mode != Select)
				buf->addr1 = buf->addr2;
			checkline(buf->lead == &buf->addr1 ? 0 : 1);
		}
		break;
	case 'k':
		if(mode == Input)
			return -1; /* else fallthrough */
	case Kup:
		if(buf->addr1 == buf->addr2)
			buf->lead = &buf->addr1;
		if(*buf->lead > 0){
			prevline(buf->lead);
			if(buf->addr1 > buf->addr2){
				swap(&buf->addr1, &buf->addr2);
				buf->lead = &buf->addr1;
			}
			if(mode != Select)
				buf->addr2 = buf->addr1;
			checkline(buf->lead == &buf->addr1 ? 0 : 1);
		}
		break;
	case 'j':
		if(mode == Input)
			return -1; /* else fallthrough */
	case Kdown:
		if(buf->addr1 == buf->addr2)
			buf->lead = &buf->addr2;
		if(*buf->lead < len() - 1){
			nextline(buf->lead);
			if(buf->addr1 > buf->addr2){
				swap(&buf->addr1, &buf->addr2);
				buf->lead = &buf->addr2;
			}
			if(mode != Select)
				buf->addr1 = buf->addr2;
			checkline(buf->lead == &buf->addr1 ? 0 : 1);
		}
		break;
	case '0':
		if(mode == Input)
			return -1; /* else fallthrough */
	case Khome:
	case CTRL('a'):
		sol(&buf->addr1);
		if(buf->addr1 > 0 && buf->addr1 < len() - 1)
			next(&buf->addr1);
		if(mode != Select)
			buf->addr2 = buf->addr1;
		buf->lead = &buf->addr1;
		break;
	case '$':
		if(mode == Input)
			return -1; /* else fallthrough */
	case Kend:
	case CTRL('e'):
		eol(&buf->addr2);
		if(mode != Select)
			buf->addr1 = buf->addr2;
		buf->lead = &buf->addr2;
		break;
	case Kpgup:
	case CTRL('b'):
		tmp = dim.ws_row - 1;
		while(tmp-- > 0)
			prevline(&buf->addr1);
		if(mode != Select)
			buf->addr2 = buf->addr1;
		buf->lead = &buf->addr1;
		checkline(0);
		break;
	case Kpgdown:
	case CTRL('f'):
		tmp = dim.ws_row - 1;
		while(tmp-- > 0)
			nextline(&buf->addr2);
		if(mode != Select)
			buf->addr1 = buf->addr2;
		buf->lead = &buf->addr2;
		checkline(1);
		break;
	case CTRL('D'):
		tmp = dim.ws_row / 2;
		while(tmp-- > 0 && buf->vstart < len() - 1){
			nextline(&buf->vstart);
			buf->vline++;
		}
		checkline(0);
		break;
	case CTRL('U'):
		tmp = dim.ws_row / 2;
		while(tmp-- > 0 && buf->vstart > 0){
			prevline(&buf->vstart);
			buf->vline--;
		}
		checkline(1);
		break;
	default:
		refresh = 0;
		return -1;
	};
	return 1;
}

/* number of bytes encoding character at offset */
size_t
encwidth(size_t a)
{
	size_t n;

	n = a;
	next(&n);
	return n - a;
}

/* interpret key for command mode */
void
command(int k)
{
	size_t i;
	ssize_t r;
	int fd;
	char *s;
	char tmp[5];

	if(motion(k) > 0)
		return;
	refresh = 1;
	switch(k){
	case Kesc:
		fd = (buf->lead == &buf->addr1) ? 0 : 1;
		buf->addr1 = buf->addr2 = *buf->lead;
		checkline(fd);
		mode = Command;
		bar("COMMAND");
		break;
	case 'b':
		bar("Current buffer [%d/%d]: %s", current + 1, nbuf, buf->path);
		break;
	case 'n':
		if(++current == nbuf)
			current = 0;
		buf = &bufs[current];
		bar("Current buffer [%d/%d]: %s", current + 1, nbuf, buf->path);
		break;
	case 'N':
		current = (current == 0) ? nbuf - 1 : current - 1;
		buf = &bufs[current];
		bar("Current buffer [%d/%d]: %s", current + 1, nbuf, buf->path);
		break;
	case 'f':
		if(nbuf == LEN(bufs))
			bar("32 buffers already open!");
		if(dialogue("File: ") == -1)
			break;
		if(bufinit(nbuf, dbuf.data) == -1){
			bar("Unable to open %s", dbuf.data);
			break;
		}
		current = ++nbuf - 1;
		buf = &bufs[current];
		bar("Current buffer [%d/%d]: %s", current + 1, nbuf, buf->path);
		break;
	case 'i':
	case Kins:
		buf->addr1 = buf->addr2;
		mode = Input;
		bar("INPUT");
		break;
	case 'a':
		if(len() > 0)
			next(&buf->addr2);
		buf->addr1 = buf->addr2;
		mode = Input;
		bar("INPUT");
		break;
	case 'o':
		if(buf->c[bufaddr(buf->addr2)] != '\n')
			eol(&buf->addr2);
		newline(&buf->addr2, 0);
		record(Uend, 0, 0);
		buf->addr1 = buf->addr2;
		checkline(1);
		mode = Input;
		bar("INPUT");
		break;
	case 'O':
		if(buf->c[bufaddr(buf->addr1)] != '\n')
			sol(&buf->addr1);
		newline(&buf->addr1, 1);
		record(Uend, 0, 0);
		buf->addr2 = buf->addr1;
		checkline(0);
		mode = Input;
		bar("INPUT");
		break;
	case 'v':
		mode = Select;
		bar("SELECT");
		break;
	case 'V':
		sol(&buf->addr1);
		if(buf->addr1 > 0 && buf->addr1 < len() - 1)
			next(&buf->addr1);
		if(buf->c[bufaddr(buf->addr2)] != '\n')
			eol(&buf->addr2);
		buf->lead = &buf->addr2;
		mode = Select;
		bar("SELECT");
		break;
	case 'd':
		yank(); /* fallthrough */
	case 'x':
		if(len() > 0) {
			if(buf->addr2 > len() - 1){
				prev(&buf->addr2);
				buf->addr1 = buf->addr2;
			}
			i = encwidth(buf->addr2);
			i += (buf->addr2 - buf->addr1);
			while(i-- > 0)
				delete(buf->addr1, 1);
			record(Uend, 0, 0);
			buf->addr2 = buf->addr1;
			mode = Command;
		}
		break;
	case 'r':
		while((k = key()) < ' ' || k >= 0xE000);
		memcpy(tmp, ch, 5);
		i = encwidth(buf->addr2);
		while(i-- > 0)
			delete(buf->addr2, 1);
		s = tmp;
		while(*s)
			insert(buf->addr2++, *s++, 1);
		record(Uend, 0, 0);
		buf->addr1 = buf->addr2;
		break;
	case 'y':
		yank();
		fd = (buf->lead == &buf->addr1) ? 0 : 1;
		buf->addr1 = buf->addr2 = *buf->lead;
		checkline(fd);
		mode = Command;
		break;
	case '[':
		if(ybuf.len > 0)
			eol(&buf->addr2); /* fallthrough */
	case 'p':
		if(ybuf.len > 0)
			next(&buf->addr2); /* fallthrough */
	case 'P':
		for(i = 0; i < ybuf.len; i++)
			insert(buf->addr2++, ((char *)ybuf.data)[i], 1);
		record(Uend, 0, 0);
		buf->addr1 = buf->addr2;
		checkline(1);
		bar("%ld bytes pasted", ybuf.len);
		break;
	case 'u':
		undo(&buf->addr1, &buf->addr2);
		checkline(buf->vstart > buf->addr1 ? 0 : 1);
		break;
	case '<':
		indent(&buf->addr1, &buf->addr2, 0);
		break;
	case '>':
		indent(&buf->addr1, &buf->addr2, 1);
		break;
	case ',':
		buf->addr1 = buf->vstart = buf->vline = 0;
		if(mode != Select)
			buf->addr2 = buf->addr1;
		buf->lead = &buf->addr1;
		break;
	case 'G':
		while(buf->addr2 < len() - 1)
			next(&buf->addr2);
		if(mode != Select)
			buf->addr1 = buf->addr2;
		buf->lead = &buf->addr2;
		checkline(1);
		break;
	case CTRL('G'):
		i = buf->addr1;
		sol(&i);
		if(buf->c[bufaddr(i)] == '\n' && i < len())
			next(&i);
		r = 0;
		while(i < buf->addr1)
			r += next(&i);
		fd = checkline(1) - 1;
		bar("Line %ld, Column %ld, %ld of %ld bytes (%.1f%%)",
		    buf->vline + fd, r, buf->addr1, len(),
		    (len() > 0) ? 100.0 * (buf->addr1 + 1) / len() : 0);
		break;
	case 't':
		if(usetabs || tabspace == 8)
			usetabs = 1 - usetabs;
		tabspace = usetabs ? 1 : (tabspace + 1) % 9;
		bar(usetabs ? "Indent %d tab (\\t)" : "Indent %d spaces", tabspace);
		break;
	case 'A':
		autoindent = 1 - autoindent;
		bar(autoindent ? "Autoindent on" : "Autoindent off");
		break;
	case 's':
		search(&buf->addr1, &buf->addr2, 0, 0);
		checkline(1);
		break;
	case 'm':
		search(&buf->addr1, &buf->addr2, 1, 0);
		checkline(1);
		break;
	case 'M':
		search(&buf->addr1, &buf->addr2, 1, 1);
		checkline(1);
		break;
	case 'W':
		if(len() == 0 || buf->c[bufaddr(len() - 1)] != '\n')
			insert(len(), '\n', 0);
		fd = open(buf->path, O_WRONLY | O_TRUNC, 0666);
		if(fd > 0){
			r = writef(fd);
			if(r > 0){
				buf->dirty = 0;
				bar("%ld bytes written to %s", r, buf->path);
				close(fd);
				return;
			}
		}
		err(Panic);
		break;
	case 'q':
		if(buf->dirty){
			bar("Current buffer contains unsaved modifications");
			return;
		}
		if(nbuf > 1){
			buffree(buf);
			memmove(bufs + current, bufs + current + 1,
			        sizeof(Buffer) * (nbuf - current - 1));
			current = 0;
			nbuf--;
			buf = &bufs[current];
			bar("Current buffer [%d/%d]: %s", current + 1, nbuf, buf->path);
			break;
		} /* fallthrough */
	case 'Q':
		for(i = 0; i < nbuf; i++){
			if(bufs[i].dirty){
				bar("Buffer %d contains unsaved modifications", i + 1);
				return;
			}
		} /* fallthrough */
	case 'Z':
		quit = 1;
		break;
	default:
		refresh = 0;
		break;
	}
}

/* interpret key for input mode */
void
input(int k)
{
	char *s;
	size_t a;

	if(motion(k) > 0)
		return;
	refresh = 1;
	switch (k) {
	case -1:
		refresh = 0;
		break;
	case Kbs:
		if(buf->addr2 > 0){
			prev(&buf->addr2);
			a = encwidth(buf->addr2);
			while(a-- > 0){
				delete(buf->addr2, 1);
				record(Uend, 0, 0);
			}
			buf->addr1 = buf->addr2;
		}
		break;
	case Kdel:
		if(buf->addr2 < len()){
			a = encwidth(buf->addr2);
			while(a-- > 0){
				delete(buf->addr2, 1);
				record(Uend, 0, 0);
			}
		}
		break;
	case Kesc:
		if(buf->addr2 > len() - 1)
			buf->addr2--;
		buf->addr1 = buf->addr2;
		checkline(1);
		mode = Command;
		bar("COMMAND");
		break;
	case '\t':
		k = tabspace;
		while(k-- > 0)
			insert(buf->addr2++, usetabs ? '\t' : ' ', 1);
		record(Uend, 0, 0);
		buf->addr1 = buf->addr2;
		break;
	case '\n':
		newline(&buf->addr2, 0);
		record(Uend, 0, 0);
		buf->addr1 = buf->addr2;
		checkline(1);
		break;
	default:
		s = ch;
		while(*s)
			insert(buf->addr2++, *s++, 1);
		record(Uend, 0, 0);
		buf->addr1 = buf->addr2;
		checkline(1);
		break;
	}
}

/* editor event loop */
void
run(void)
{
	int k;

	if(dims() == -1)
		err(Panic);
	while(!quit){
		if(refresh){
			display();
			refresh = 0;
		}
		k = key();
		if(mode == Input)
			input(k);
		else
			command(k);
	}
}

int
main(int argc, char **argv)
{
	if(argc < 2){
		fprintf(stderr, "er (0.6.0)\nUsage:\n\ter file...\n");
		exit(1);
	}
	nbuf = argc - 1;
	if(sigsetjmp(env, 1) == 0)
		 init(nbuf, argv);
	buf = &bufs[current];
        mode = Command;
	refresh = usetabs = tabspace = autoindent =  1;
	if(status != Panic)
		run();
	else
		save();
	end();
	if(status == Panic)
		fprintf(stderr,
		        "er panicked and tried to save buffer(s) to er.out!\n");
	return 0;
}
