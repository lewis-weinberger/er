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

enum
{
	Gaplen  = 256,
	Vbufmax = 4096
};

enum
{
	Ok,
	Panic,
	Reset
};

enum
{
	Kesc = 0x1b,
	Kbs  = 0x7f,
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

enum
{
	Command,
	Input,
	Select
};

enum
{
	Char,
	Chnge
};

enum
{
	Udelete,
	Uinsert,
	Uend
};

typedef struct Change Change;
typedef struct Array Array;
typedef struct Buffer Buffer;

struct Change
{
	short  type;
	size_t i;
	char   c;
};

struct Array
{
	void   *data;
	size_t len;
	size_t cap;
};

struct Buffer
{
	char   *c;
	char   path[PATH_MAX];
	Array  changes;
	int    dirty;
	size_t addr1, addr2;
	size_t cap, gap, start;
	size_t vstart, vline;
};

Buffer                bufs[32], *buf;
Array                 ybuf, bbuf, dbuf;
char                  ch[5], vbuf[Vbufmax];
size_t                vbuflen, current, nbuf;
struct winsize        dim;
jmp_buf               env;
const char            invalid[] = "�";
short                 mode, refresh, quit, usetabs, tabspace;
sigset_t              oset;
volatile sig_atomic_t status;
struct termios        term;

size_t
bufaddr(size_t in)
{
	return (in >= buf->start) ? in + buf->gap : in;
}

void
err(int n)
{
	status = n;
	siglongjmp(env, 1);
}

int
digits(long v)
{
	return (v == 0) ? 1 : floor(log10((double)v)) + 1;
}

int
dims(void)
{
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &dim) == -1 || dim.ws_col == 0)
		return -1;
	return 0;
}

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

int
parsechar(char c)
{
	if(decode(c, NULL, readbyte, NULL, NULL) == -1)
		err(Reset);
	return c;
}

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
		{ '[', 'A',  -1,     Kup },
		{ '[', 'B',  -1,   Kdown },
		{ '[', 'C',  -1,  Kright },
		{ '[', 'D',  -1,   Kleft },
		{ '[', 'H',  -1,   Khome },
		{ '[', 'F',  -1,    Kend },
		{ '[', 'P',  -1,    Kdel },
		{ '[', '4', 'h',    Kins },
		{ '[', '1', '~',   Khome },
		{ '[', '7', '~',   Khome },
		{ '[', '4', '~',    Kend },
		{ '[', '8', '~',    Kend },
		{ '[', '3', '~',    Kdel },
		{ '[', '5', '~',   Kpgup },
		{ '[', '6', '~', Kpgdown },
		{ '[', '2', '~',    Kins },
		{ 'O', 'A',  -1,     Kup },
		{ 'O', 'B',  -1,   Kdown },
		{ 'O', 'C',  -1,  Kright },
		{ 'O', 'D',  -1,   Kleft },
		{ 'O', 'H',  -1,   Khome },
		{ 'O', 'F',  -1,    Kend }
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
	if (k <= 0x1f)
		return k;
	if (k == Kbs)
		return Kbs;
	return parsechar(k);
}

size_t
len(void)
{
	return buf->cap - buf->gap;
}

int
nextbuf(size_t *i)
{
	return (*i == len()) ? -1 : buf->c[bufaddr((*i)++)];
}

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
	return wcwidth(wc);
}

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

void
resize(Array *a, short type)
{
	void *new;
	size_t size;

	switch(type){
	case Char:
		size = sizeof(char);
		break;
	case Chnge:
		size = sizeof(Change);
		break;
	default:
		err(Panic);
		break;
	}
	new = realloc(a->data, size * 2 * a->cap);
	if(new == NULL)
		err(Panic);
	a->data = new;
	a->cap *= 2;
}

void
append(Array *a, short type, ...)
{
	va_list args;

	if(a->len == a->cap)
		resize(a, type);
	va_start(args, type);
	switch(type){
		case Char:
			((char *)a->data)[a->len++] = va_arg(args, int);
			break;
		case Chnge:
			((Change *)a->data)[a->len++] = va_arg(args, Change);
			break;
	}
	va_end(args);
}

void
record(short t, size_t i, char c) {
	append(&buf->changes, Chnge, (Change){ t, i, c });
}

void
grow(void)
{
	char *new;

	new = realloc(buf->c, (buf->cap + Gaplen) * sizeof(char));
	if(new == NULL)
		err(Panic);
	buf->c = new;
	buf->start = buf->cap;
	buf->gap = Gaplen;
	buf->cap += Gaplen;
}

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
	sol(&b);
	if(buf->c[bufaddr(b)] == '\n' && b < len())
		next(&b);
	while(buf->c[bufaddr(b++)] == (usetabs ? '\t' : ' ') && b < len())
		insert((*a)++, usetabs ? '\t' : ' ', 1);
}

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
	b->c = calloc(n + Gaplen, sizeof(char));
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

void
sigpend(void)
{
	sigset_t mask;

	if(sigfillset(&mask) != -1 && sigprocmask(SIG_SETMASK, &mask, &oset) != -1)
		return;
	perror("sigpend");
	exit(1);
}

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

int
arrinit(Array *a, size_t size)
{
	a->data = calloc(Gaplen, size);
	if(a->data == NULL)
		return -1;
	a->len = 0;
	a->cap = Gaplen;
	return 0;
}

void
arrfree(Array *a)
{
	free(a->data);
}

int
bufinit(int i, const char *path)
{
	bufs[i].addr1 = bufs[i].addr2 = bufs[i].vstart = bufs[i].vline = 0;
	bufs[i].dirty = 0;
	strncpy(bufs[i].path, path, PATH_MAX);
	if(arrinit(&bufs[i].changes, sizeof(Change)) != -1){
		if(fileinit(&bufs[i]) != -1)
			return 0;
		arrfree(&bufs[i].changes);
	}
	return -1;
}

void
buffree(Buffer *b)
{
	arrfree(&b->changes);
	free(b->c);
}

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
	if(arrinit(&ybuf, sizeof(char)) == -1)
		goto Error;
	if(arrinit(&bbuf, sizeof(char)) == -1)
		goto Error;
	if(arrinit(&dbuf, sizeof(char)) == -1)
		goto Error;
	terminit();
	siginit();
	return;
Error:
	perror("init");
	exit(1);
}

void
termreset(void)
{
	(void)!write(STDOUT_FILENO, CSI("9999;1H\r\n"), 11);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);
}

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
	}
}

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

void
vflush(void)
{
	if(writeall(STDOUT_FILENO, vbuf, vbuflen) == -1)
		err(Panic);
	vbuflen = 0;
}

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

void
cursor(unsigned int x, unsigned int y)
{
	char tmp[32];
	if(snprintf(tmp, sizeof(tmp), CSI("%u;%uH"), y + 1 , x + 1) < 0)
		err(Panic);
	vpush(1, tmp);
}

void
bar(const char *fmt, ...){
	int n;
	va_list args;

	while(bbuf.cap < (size_t)dim.ws_col + 1)
		resize(&bbuf, Char);
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
			((char *)dbuf.data)[--dbuf.len] = 0;
		else{
			s = ch;
			while (*s)
				append(&dbuf, Char, *s++);
		}
		bar("%s%s", prompt, dbuf.data);
	}
	return 0;
}

void
search(size_t *a, size_t *b)
{
	regex_t reg;
	regmatch_t m[1];
	char err[128];
	int r;

	if(dialogue("Search: ") == -1)
		return;
	move(len());
	r = regcomp(&reg, dbuf.data, REG_EXTENDED | REG_NEWLINE);
	if(r == 0){
		r = regexec(&reg, buf->c + *b, 1, m, 0);
		if(r == 0 && *b + m[0].rm_so < len()){
			*a = *b + m[0].rm_so;
			*b += m[0].rm_eo - 1;
		}
	}
	bar("");
	if(r != 0){
		regerror(r, &reg, err, sizeof(err));
		bar(err);
	}
	regfree(&reg);
	return;
}

void
display(void)
{
	int i, j, l, i2, j2, n;
	size_t k;
	char tmp[32];

	l = digits(buf->vline + dim.ws_row);
	j2 = l + 2;
	for(i = j = i2 = 0, k = buf->vstart; i < dim.ws_row - 1; i++, j = 0){
		cursor(j, i);
		if(k < len()){
			snprintf(tmp, sizeof(tmp), CSI("34m %*ld "), l, buf->vline + i);
			vpush(2, tmp, CSI("0m"));
			j += l + 2;
			if(buf->addr1 != buf->addr2 && k >= buf->addr1 && k <= buf->addr2)
				vpush(1, CSI("7m"));
			do{
				if(buf->addr1 != buf->addr2 && k == buf->addr1)
					vpush(1, CSI("7m"));
				if(k == buf->addr2){
					vpush(1, CSI("0m"));
					j2 = j;
					i2 = i;
				}
				j += next(&k);
				if(j < dim.ws_col){
					if(ch[0] == '\t') {
						for(n = 0; n < 8; n++)
							vpush(1, " ");
					}else
						vpush(1, (ch[0] == '\n') ? " " : ch);
				}
				if(ch[0] == '\n')
					break;
				else if(j == dim.ws_col){
					cursor(0, i);
					vpush(2, CSI("35m>"), CSI("0m"));
					cursor(j, i);
				}
			}while(k < len());
			if(k == buf->addr2){
				if(ch[0] == '\n'){
					j2 = l + 2;
					i2 = i + 1;
				}else{
					j2 = j;
					i2 = i;
				}
			}
		}else{
			vpush(2, CSI("90m~"), CSI("0m"));
		}
		vpush(1, CSI("K"));
	}
	vpush(1, CSI("?25h"));
	cursor(j2, i2);
	vflush();
}

void
yank(void)
{
	size_t j, k, n;

	j = buf->addr1;
	k = buf->addr2;
	next(&k);
	n = k - buf->addr1;
	while(ybuf.cap < n)
		resize(&ybuf, Char);
	ybuf.len = 0;
	while(ybuf.len < n)
		((char *)ybuf.data)[ybuf.len++] = buf->c[bufaddr(j++)];
	bar("%ld bytes yanked", n);
}

int
motion(int k)
{
	size_t i, tmp;
	static int ignore[] = { 'h', 'j', 'k', 'l', '0', '$' };

	if(mode == Input){
		for(i = 0; i < LEN(ignore); i++){
			if(k == ignore[i])
				return -1;
		}
	}
	refresh = 1;
	switch(k){
	case Kleft:
	case 'h':
		if(mode != Select && buf->addr1 > 0){
			prev(&buf->addr1);
			buf->addr2 = buf->addr1;
		}else if(mode == Select && buf->addr2 > buf->addr1)
			prev(&buf->addr2);
		checkline(0);
		break;
	case Kright:
	case 'l':
		if(buf->addr2 < len() - 1){
			next(&buf->addr2);
			if(mode != Select)
				buf->addr1 = buf->addr2;
			checkline(1);
		}
		break;
	case Kup:
	case 'k':
		if(buf->addr2 > 0)
			prevline(&buf->addr2);
		if(mode != Select)
			buf->addr1 = buf->addr2;
		checkline(0);
		break;
	case Kdown:
	case 'j':
		if(buf->addr2 < len() - 1)
			nextline(&buf->addr2);
		if(mode != Select)
			buf->addr1 = buf->addr2;
		checkline(1);
		break;
	case Khome:
	case CTRL('a'):
	case '0':
		sol(&buf->addr1);
		if(buf->addr1 > 0 && buf->addr1 < len() - 1)
			next(&buf->addr1);
		if(mode != Select)
			buf->addr2 = buf->addr1;
		break;
	case Kend:
	case CTRL('e'):
	case '$':
		eol(&buf->addr2);
		if(mode != Select)
			buf->addr1 = buf->addr2;
		break;
	case Kpgup:
	case CTRL('b'):
		tmp = dim.ws_row - 1;
		while(tmp-- > 0)
			prevline(&buf->addr1);
		buf->addr2 = buf->addr1;
		checkline(0);
		break;
	case Kpgdown:
	case CTRL('f'):
		tmp = dim.ws_row - 1;
		while(tmp-- > 0)
			nextline(&buf->addr2);
		buf->addr1 = buf->addr2;
		checkline(1);
		break;
	default:
		refresh = 0;
		return -1;
	};
	if(buf->addr1 > buf->addr2){
		buf->addr1 = buf->addr2;
		mode = Command;
		bar("COMMAND");
	}
	return 1;
}

void
command(int k)
{
	size_t i;
	ssize_t r;
	int fd;
	char *s;

	if(motion(k) > 0)
		return;
	refresh = 1;
	switch(k){
	case Kesc:
		buf->addr1 = buf->addr2;
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
			i = 1 + buf->addr2 - buf->addr1;
			while(i-- > 0)
				delete(buf->addr1, 1);
			record(Uend, 0, 0);
			buf->addr2 = buf->addr1;
			mode = Command;
		}
		break;
	case 'r':
		while((k = key()) < ' ' || k >= 0xE000);
		delete(buf->addr2, 1);
		s = ch;
		while(*s)
			insert(buf->addr2++, *s++, 1);
		record(Uend, 0, 0);
		buf->addr1 = buf->addr2;
		break;
	case 'y':
		yank();
		buf->addr1 = buf->addr2;
		mode = Command;
		break;
	case 'p':
		if(ybuf.len > 0)
			next(&buf->addr2); /* fallthrough */
	case 'P':
		for(i = 0; i < ybuf.len; i++)
			insert(buf->addr2++, ((char *)ybuf.data)[i], 1);
		record(Uend, 0, 0);
		buf->addr1 = buf->addr2;
		bar("%ld bytes pasted", ybuf.len);
		break;
	case 'u':
		undo(&buf->addr1, &buf->addr2);
		break;
	case '<':
		indent(&buf->addr1, &buf->addr2, 0);
		break;
	case '>':
		indent(&buf->addr1, &buf->addr2, 1);
		break;
	case 'g':
		buf->addr1 = buf->addr2 = buf->vstart = buf->vline = 0;
		break;
	case 'G':
		while(buf->addr2 < len() - 1)
			next(&buf->addr2);
		buf->addr1 = buf->addr2;
		checkline(1);
		break;
	case CTRL('G'):
		i = buf->addr1;
		sol(&i);
		if(buf->c[bufaddr(i)] == '\n' && i < len())
			next(&i);
		r = 0;
		while(i < buf->addr1){
			next(&i);
			r++;
		}
		fd = checkline(1) - 1;
		bar("Line %ld, Column %ld, %ld of %ld bytes (%.1f%%)",
		    buf->vline + fd, r, buf->addr1, len(),
		    (len() > 0) ? 100.0 * (buf->addr1 + 1) / len() : 0);
		break;
	case 't':
		usetabs = 1 - usetabs;
		tabspace = usetabs ? 1 : 4;
		bar(usetabs ? "Using tabs (\\t)" : "Using %d spaces", tabspace);
		break;
	case 's':
		search(&buf->addr1, &buf->addr2);
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

void
input(int k)
{
	char *s;

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
			delete(buf->addr2, 1);
			record(Uend, 0, 0);
			buf->addr1 = buf->addr2;
		}
		break;
	case Kdel:
		if(buf->addr2 < len()){
			delete(buf->addr2, 1);
			record(Uend, 0, 0);
		}
		break;
	case Kesc:
		if(buf->addr2 > len() - 1)
			buf->addr2--;
		buf->addr1 = buf->addr2;
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
		fprintf(stderr, "er (0.3.0)\nUsage:\n\ter file...\n");
		exit(1);
	}
	nbuf = argc - 1;
	if(sigsetjmp(env, 1) == 0)
		 init(nbuf, argv);
	buf = &bufs[current];
	refresh = 1;
        mode = Command;
	usetabs = tabspace = 1;
	if(status != Panic)
		run();
	else
		save();
	end();
	if(status == Panic)
		fprintf(stderr, "er panicked and tried to save buffers to er.out!");
	return 0;
}
