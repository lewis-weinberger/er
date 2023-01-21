#include "er.h"
#include "../dat.h"
#include "../fns.h"

ulong tick;

extern Buffer *buf;
extern Array  bbuf, dbuf;
extern short  refresh;

/* restore connection to window after resizing */
int
dims(void)
{
	if(getwindow(display, Refnone) < 0)
		return -1;
	refresh = 1;
	return 0;
}

/* return current number of rows in display */
int
rows(void)
{
	return (Dy(screen->r) / font->height);
}

/* get user input from keyboard */
int
key(void)
{
	Event e;

	if(event(&e) == Ekeyboard){
		memset(ch, 0, sizeof(ch));
		if(e.kbdc < KF)
			runetochar(ch, &e.kbdc);
		return e.kbdc;
	}
	return -1;
}

/* handler for window resizing event */
void
eresized(int new){
	err(Reset);
}

/* initialise graphics and event handler */
void
terminit(void)
{
	if(initdraw(nil, nil, "er") == -1)
		sysfatal("%r");
	einit(Ekeyboard);
	eresized(0);
	if((tick = etimer(0, Timeout * 100)) == 0)
		sysfatal("%r");
}

/* disconnect display */
void
termreset(void)
{
	closedisplay(display);
}

/* draw message to status bar */
void
bar(const char *fmt, ...)
{
	int n;
	va_list args;
	size_t cols;
	Point pos, nxt;
	Rectangle box;

	pos.x = screen->r.min.x;
	pos.y = screen->r.max.y - font->height;
	box.min = pos;
	box.max = screen->r.max;
	draw(screen, box, display->white, nil, ZP);
	cols = Dx(screen->r) / stringwidth(font, "i");
	while(bbuf.cap < cols + 1)
		resize(&bbuf, Char);
	va_start(args, fmt);
	n = vsnprint(bbuf.data, cols + 1, fmt, args);
	va_end(args);
	nxt = string(screen, pos, display->black, ZP, font, bbuf.data);
	/* TODO overflow */
	flushimage(display, 1);
}

/* start dialogue prompt in status bar */
int
dialogue(const char *prompt)
{
	/* TODO */
	return 0;
}

/* display current buffer to screen */
void
view(void)
{
	int l, i, width;
	size_t k;
	Point pos, status;
	char tmp[32];
	Rectangle box;

	box.min = screen->r.min;
	status.x = screen->r.max.x;
	status.y = screen->r.max.y - font->height;
	box.max = status;
	draw(screen, box, display->white, nil, ZP);
	pos.x = screen->r.min.x;
	pos.y = screen->r.min.y;
	l = digits(buf->vline + rows());
	i = 0;
	k = buf->vstart;
	while(pos.y < screen->r.max.y - font->height){
		if(k < len()){
			snprint(tmp, sizeof(tmp), " %*ld ", l, buf->vline + i);
			pos = string(screen, pos, display->black, ZP, font, tmp);
			do{
				width = next(&k);
				if(ch[0] == '\n')
					break;
				if(pos.x + width < screen->r.max.x)
					pos = string(screen, pos, display->black, ZP, font, ch);
			}while(k < len());
		}else
			string(screen, pos, display->black, ZP, font, "~");
		pos.x = screen->r.min.x;
		pos.y += font->height;
		i++;
	}
	flushimage(display, 1);
}
