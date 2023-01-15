#include "er.h"
#include "../dat.h"
#include "../fns.h"

/* get current terminal dimensions */
int
dims(void)
{
	return 0;
}

/* return current number of rows in terminal */
int
rows(void)
{
	return 0;
}

/* get user input from keyboard */
int
key(void)
{
	return 0;
}

/* set terminal into raw mode */
void
terminit(void)
{
}

/* return the terminal to original state */
void
termreset(void)
{
}

/* draw message to status bar */
void
bar(const char *fmt, ...)
{
}

/* start dialogue prompt in status bar */
int
dialogue(const char *prompt)
{
	return 0;
}

/* display current buffer to terminal */
void
view(void)
{
}
