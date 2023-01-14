#include "er.h"
#include "../dat.h"
#include "../fns.h"

extern Buffer         bufs[32], *buf;
extern size_t         nbuf;
extern jmp_buf        env;

sigset_t              oset;
volatile sig_atomic_t status;

/* throw error */
void
err(int n)
{
	status = n;
	siglongjmp(env, 1);
}

/* next byte in the current buffer */
static int
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

/* signal handler */
static void
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
