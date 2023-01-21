#include "er.h"
#include "../dat.h"
#include "../fns.h"

ulong tick;

/* restore connection to window after resizing */
int
dims(void)
{
	if(getwindow(display, Refnone) < 0)
		return -1;
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
			runetochar(ch, e.kbdc);
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
	/* TODO */
}

/* start dialogue prompt in status bar */
int
dialogue(const char *prompt)
{
	/* TODO */
	return 0;
}

/* display current buffer to terminal */
void
view(void)
{
	/* TODO */
}
