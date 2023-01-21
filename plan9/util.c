#include "er.h"
#include "../dat.h"
#include "../fns.h"

extern Buffer  bufs[32], *buf;
extern size_t  nbuf;
extern jmp_buf env;

volatile int status;
char         ch[5];

/* throw error */
void
err(int n)
{
	status = n;
	longjmp(env, 1);
}

/* open file and determine number of bytes */
int
fileopen(const char *path, size_t *len)
{
	Dir *d;
	int fd;

	fd = -1;
	*len = 0;
	if((fd = open(path, OREAD)) > 0 && (d = dirfstat(fd)) != nil)
		*len = d->length;
	else
		fd = create(path, OREAD, 0666);
	return fd;
}

/* decode bytes in buffer until a complete UTF-8 character is found */
static int
decode(size_t *i)
{
	int n;

	for(n = 1, ch[0] = buf->c[bufaddr((*i)++)]; n < 5; n++){
		if(fullrune(ch, n))
			return ch[0];
		if(*i == len())
			return -1;
		ch[n] = buf->c[bufaddr((*i)++)];
	}
	return -1;
}

/* next character in the current buffer */
int
next(size_t *i)
{
	memset(ch, 0, 5);
	if(decode(i) == -1){
		memcpy(ch, invalid, sizeof(invalid));
		return stringwidth(font, invalid);
	}
	return stringwidth(font, ch);
}

/* notification handler that does nothing */
static void
ignore(void *a, char *s)
{
	noted(NCONT);
}

/* ignore all incoming notes */
void
sigpend(void)
{
	notify(ignore);
}

/* notification handler */
static void
sig(void *a, char *s)
{
	if(strcmp(s, "interrupt") == 0){
		status = Reset;
	}else
		status = Panic;
	notejmp(a, env, 1);
}

/* install notification handler */
void
siginit(void)
{
	notify(sig);
}

/* emergency backup in case of panic */
void
save(void)
{
	int fd, i;
	fd = create("er.out", OWRITE, 0666);
	if(fd > 0){
		for(i = 0; i < nbuf; i++){
			buf = &bufs[i];
			writef(fd);
		}
		close(fd);
	}
}

/* search for regular expression in current buffer */
void
search(size_t *a, size_t *b, int replace, int all)
{
	/* TODO */
}

ssize_t
writeall(int f, const char *s, size_t n)
{
	long w;

	w = write(f, s, n);
	if(w != n)
		return -1;
	return w;
}
