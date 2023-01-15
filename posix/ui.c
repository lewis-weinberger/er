#include "er.h"
#include "../dat.h"
#include "../fns.h"

#define CSI(ch) ("\x1b[" ch)

enum
{
	Vbufmax = 4096
};

char           vbuf[Vbufmax];
size_t         vbuflen;
struct winsize dim;
struct termios term;
extern Buffer  *buf;
extern Array   bbuf, dbuf;

/* get current terminal dimensions */
int
dims(void)
{
	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &dim) == -1 || dim.ws_col == 0)
		return -1;
	return 0;
}

/* return current number of rows in terminal */
int
rows(void)
{
	return dim.ws_row - 1;
}

/* read a byte from STDIN (see terminit() for timeout) */
static int
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

/* read a character from STDIN */
static int
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

/* return the terminal to original state */
void
termreset(void)
{
	(void)!write(STDOUT_FILENO, CSI("9999;1H\r\n"), 11);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);
}

/* flush contents of screen buffer to STDOUT */
static void
vflush(void)
{
	if(writeall(STDOUT_FILENO, vbuf, vbuflen) == -1)
		err(Panic);
	vbuflen = 0;
}

/* append to screen buffer */
static void
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
static void
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

/* display current buffer to terminal */
void
view(void)
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
