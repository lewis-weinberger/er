#include "er.h"
#include "../dat.h"
#include "../fns.h"

volatile int status;
char         ch[5];

/* throw error */
void
err(int n)
{
}

/* open file and determine number of bytes */
int
fileopen(const char *path, size_t *len)
{
	return 0;
}

/* next character in the current buffer */
int
next(size_t *i)
{
	return 0;
}

/* block all incoming signals */
void
sigpend(void)
{
}

/* install signal handler and unblock signals */
void
siginit(void)
{
}

/* emergency backup in case of panic */
void
save(void)
{
}

/* search for regular expression in current buffer */
void
search(size_t *a, size_t *b, int replace, int all)
{
}

ssize_t
writeall(int f, const char *s, size_t n)
{
	return 0;
}
