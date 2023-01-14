#include "er.h"
#include "../dat.h"
#include "../fns.h"

extern Buffer  bufs[32], *buf;
extern Array   dbuf;
extern size_t  nbuf;
extern jmp_buf env;

sigset_t              oset;
volatile sig_atomic_t status;

/* throw error */
void
err(int n)
{
	status = n;
	siglongjmp(env, 1);
}

/* open file and determine number of bytes */
int
fileopen(const char *path, size_t *len)
{
	struct stat st;
	int fd;

	fd = -1;
	*len = 0;
	if((fd = open(path, O_RDWR | O_CREAT, 0666)) > 0 && fstat(fd, &st) != -1)
		*len = st.st_size;
	return fd;
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
