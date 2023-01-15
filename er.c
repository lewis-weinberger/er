#include "er.h"
#include "dat.h"
#include "fns.h"

Buffer  bufs[32], *buf;
Array   ybuf, bbuf, dbuf;
size_t  current, nbuf;
jmp_buf env;
short   mode, refresh, quit, usetabs, tabspace, autoindent;

/* convert logical byte offset to internal byte offset */
size_t
bufaddr(size_t in)
{
	return (in >= buf->start) ? in + buf->gap : in;
}

/* number of decimal digits */
int
digits(long v)
{
	return (v == 0) ? 1 : floor(log10((double)v)) + 1;
}

/* length of current buffer */
size_t
len(void)
{
	return buf->cap - buf->gap;
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
		n -= rows();
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
resize(Array *a, short type)
{
	void *new;
	size_t size;

	switch(type){
	case Char:
		size = 1;
		break;
	case Chnge:
		size = sizeof(Change);
		break;
	default:
		err(Panic);
		return;
	}
	new = realloc(a->data, size * 2 * a->cap);
	if(new == NULL)
		err(Panic);
	a->data = new;
	a->cap *= 2;
}

/* append new item to the end of dynamic array */
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

/* append new textual change to the current buffer's undo stack */
void
record(short t, size_t i, char c) {
	append(&buf->changes, Chnge, (Change){ t, i, c });
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
	size_t n;
	ssize_t k;
	char *p;
	int fd;

	fd = fileopen(b->path, &n);
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

/* allocate memory for dynamic array */
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
	locale();
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
	}
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
		resize(&ybuf, Char);
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
		break;
	case '$':
		if(mode == Input)
			return -1; /* else fallthrough */
	case Kend:
	case CTRL('e'):
		eol(&buf->addr2);
		if(mode != Select)
			buf->addr1 = buf->addr2;
		break;
	case Kpgup:
	case CTRL('b'):
		tmp = rows();
		while(tmp-- > 0)
			prevline(&buf->addr1);
		buf->addr2 = buf->addr1;
		checkline(0);
		break;
	case Kpgdown:
	case CTRL('f'):
		tmp = rows();
		while(tmp-- > 0)
			nextline(&buf->addr2);
		buf->addr1 = buf->addr2;
		checkline(1);
		break;
	default:
		refresh = 0;
		return -1;
	};
	return 1;
}

/* interpret key for command mode */
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
	case ',':
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
		while(i < buf->addr1)
			r += next(&i);
		fd = checkline(1) - 1;
		bar("Line %ld, Column %ld, %ld of %ld bytes (%.1f%%)",
		    buf->vline + fd, r, buf->addr1, len(),
		    (len() > 0) ? 100.0 * (buf->addr1 + 1) / len() : 0);
		break;
	case 't':
		usetabs = 1 - usetabs;
		tabspace = usetabs ? 1 : 4;
		bar(usetabs ? "Using tabs (\\t)" : "Using spaces");
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
		fd = open(buf->path, SAVE);
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

/* editor event loop */
void
run(void)
{
	int k;

	if(dims() == -1)
		err(Panic);
	while(!quit){
		if(refresh){
			view();
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
		fprintf(stderr, "er (0.5.0)\nUsage:\n\ter file...\n");
		exit(1);
	}
	nbuf = argc - 1;
	if(jmp(env, 1) == 0)
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
		fprintf(stderr, "er panicked and tried to save buffer(s) to er.out!\n");
	return 0;
}
