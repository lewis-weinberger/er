#include <errno.h>
#include <fcntl.h>
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

#define CSI(ch)   ("\x1b[" ch)
#define LEN(X)    (sizeof(X) / sizeof(X[0]))
#define MODE(X)   mode = X; bar(#X)

enum defs { TABSPACE=4, GAPLEN=256, VBUFMAX=4096 };
enum errs { OK, PANIC, RESET };
enum keys { NONE=-14, BACKSPACE, DELETE, LEFT, RIGHT, UP, DOWN,
            HOME, END, PAGEUP, PAGEDOWN, INSERT, ESCAPE };
enum modes { COMMAND, INPUT, SELECT };
typedef enum { UDELETE, UINSERT, UEND } chtype;

typedef struct {
    chtype type;
    size_t i;
    char   c;
} change;

size_t                addr1, addr2;
char                  *buf, ch[5], *name, vbuf[VBUFMAX], *ybuf;
size_t                cap, gap, start, dirty;
struct winsize        dim;
jmp_buf               env;
char                  invalid[] = { '\xef', '\xbf', '\xbd', '\0', '\0' };
int                   mode = COMMAND;
sigset_t              oset;
int                   refresh, quit;
change                *ubuf;
size_t                ubuflen, ubufcap, vbuflen, vstart, vline, ybuflen, ybufcap;
volatile sig_atomic_t status;
struct termios        term;

size_t bufaddr(size_t in) {
    return (in >= start) ? in + gap : in;
}

void err(int n) {
    status = n;
    siglongjmp(env, 1);
}

int digits(long v) {
    return (v == 0) ? 1 : floor(log10((double)v)) + 1;
}

int dims(void) {
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &dim) == -1 || dim.ws_col == 0)
        return -1;
    return 0;
}

int readbyte() {
    int n;
    char c;
    if ((n = read(STDIN_FILENO, &c, 1)) == -1) {
        if (errno == EINTR)
            n = 0;
        else
            err(PANIC);
    }
    if (n == 0)
        return -1;
    return c;
}

int decode(char first, int (*more)(size_t*), size_t *i, wchar_t *wc) {
    int n, r;
    mbstate_t ps;
    memset(ch, 0, 5);
    for (n = 1, ch[0] = first; n < 5; n++) {
        memset(&ps, 0, sizeof(ps));
        switch (mbrtowc(wc, ch, n, &ps)) {
        case (size_t)-2:
            if ((r = more(i)) == -1)
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

int parsechar(char c) {
    if (decode(c, readbyte, NULL, NULL) == -1)
        err(RESET);
    return c;
}

int key(void) {
    int k, l, m, n;
    static const struct {
        int a;
        int b;
        int c;
        int out;
    } vt[] = {
        { '[', 'A',  -1,       UP },
        { '[', 'B',  -1,     DOWN },
        { '[', 'C',  -1,    RIGHT },
        { '[', 'D',  -1,     LEFT },
        { '[', 'H',  -1,     HOME },
        { '[', 'F',  -1,      END },
        { '[', 'P',  -1,   DELETE },
        { '[', '4', 'h',   INSERT },
        { '[', '1', '~',     HOME },
        { '[', '7', '~',     HOME },
        { '[', '4', '~',      END },
        { '[', '8', '~',      END },
        { '[', '3', '~',   DELETE },
        { '[', '5', '~',   PAGEUP },
        { '[', '6', '~', PAGEDOWN },
        { '[', '2', '~',   INSERT },
        { 'O', 'A',  -1,       UP },
        { 'O', 'B',  -1,     DOWN },
        { 'O', 'C',  -1,    RIGHT },
        { 'O', 'D',  -1,     LEFT },
        { 'O', 'H',  -1,     HOME },
        { 'O', 'F',  -1,      END }
    };
    if ((k = readbyte()) == -1) {
        memset(ch, 0, 5);
        return NONE;
    }
    if (k == 0x1b) {
        if ((l = readbyte()) != -1 && (m = readbyte()) != -1) {
            for (size_t i = 0; i < LEN(vt); i++) {
                if (l == vt[i].a && m == vt[i].b && vt[i].c == -1)
                    return vt[i].out;
            }
            if ((n = readbyte()) != -1) {
                for (size_t i = 0; i < LEN(vt); i++) {
                    if (l == vt[i].a && m == vt[i].b && n == vt[i].c)
                        return vt[i].out;
                }
            }
        }
        return ESCAPE;
    }
    if (k <= 0x1f)
        return k;
    if (k == 0x7f)
        return BACKSPACE;
    return parsechar(k);
}

int nextbuf(size_t *i) {
    return (*i == cap - gap) ? -1 : buf[bufaddr((*i)++)];
}

int next(size_t *i) {
    wchar_t wc;
    if (decode(buf[bufaddr((*i)++)], nextbuf, i, &wc) == -1) {
        memcpy(ch, invalid, 5);
        return 1;
    }
    return wcwidth(wc);
}

int eol(size_t *i) {
    int r = 0;
    if (*i < cap - gap) {
        do {
            next(i);
            r++;
        } while (*i < cap - gap && buf[bufaddr(*i)] != '\n');
    }
    return r;
}

int prev(size_t *i) {
    size_t m, n = 0;
    int r = 0;
    if (*i > 0) {
        do {
            n++;
            m = *i - n;
            r = next(&m);
        } while(m > 0 && n < 4 && strncmp(ch, invalid, 5) == 0);
        *i = (*i < n) ? 0 : *i - n;
    }
    return r;
}

int sol(size_t *i) {
    int r = 0;
    if (*i > 0) {
        do {
            prev(i);
            r++;
        } while (*i > 0 && buf[bufaddr(*i)] != '\n');
    }
    return r;
}

void nextline(size_t *i) {
    size_t m = *i, n;
    if (buf[bufaddr(*i)] == '\n')
        n = 0;
    else {
        n = sol(&m);
        if (*i == 0)
            n = 0;
        else if (m > 0)
            n -=1;
        eol(i);
    }
    if (*i < cap - gap - 1) {
        next(i);
        while (n && buf[bufaddr(*i)] != '\n' && *i < cap - gap - 1) {
            next(i);
            n--;
        }
    }
}

void prevline(size_t *i) {
    size_t n;
    if (*i > 0) {
        n = sol(i) - 1;
        if (*i == 0)
            return;
        sol(i);
        if (buf[bufaddr(*i)] == '\n')
            next(i);
        while (n && buf[bufaddr(*i)] != '\n' && *i < cap - gap - 1) {
            next(i);
            n--;
        }
    }
}

int checkline(int dir) {
    int n = 1, r = 0;
    size_t i;
    if (dir) {
        for (i = vstart; i < addr2; i++) {
            if (buf[bufaddr(i)] == '\n')
                n++;
        }
        r = n;
        n -= dim.ws_row - 1;
        while (n-- > 0) {
            nextline(&vstart);
            vline++;
        }
    } else {
        while (addr1 < vstart) {
            prevline(&vstart);
            vline--;
        }
    }
    return r;
}

void move(size_t i) {
    size_t j = bufaddr(i), dst, src, num;
    if (j != start + gap) {
        dst = (j < start) ? j + gap : start;
        src = (j < start) ? j : start + gap;
        num = (j < start) ? start - j : j - (gap + start);
        memmove(buf + dst, buf + src, num);
        start = (j < start) ? j : j - gap;
    }
}

void record(chtype t, size_t i, char c) {
    change *new;
    if (ubuflen == ubufcap - 1) {
        if (!(new = realloc(ubuf, 2 * ubufcap * sizeof(change))))
            err(PANIC);
        ubuf = new;
        ubufcap *= 2;
    }
    ubuf[ubuflen++] = (change){ t, i, c };
}

void grow(void) {
    char *new;
    if (!(new = realloc(buf, (cap + GAPLEN) * sizeof(char))))
        err(PANIC);
    buf = new;
    start = cap;
    gap = GAPLEN;
    cap += GAPLEN;
}

void insert(size_t i, char c, int r) {
    if (gap == 0)
        grow(); /* please mind the gap */
    move(i);
    buf[start++] = c;
    gap--;
    dirty = 1;
    if (r)
        record(UINSERT, i, 0);
}

void delete(size_t i, int r) {
    move(i);
    char c = buf[start + gap++];
    dirty = 1;
    if (r)
        record(UDELETE, i, c);
}

void indent(size_t *a, size_t *b, int fwd) {
    size_t i = *a;
    if (i > 0) {
        sol(&i);
        if (buf[bufaddr(i)] == '\n' && i < cap - gap - 1)
            next(&i);
    }
    while (i <= *b && i < cap - gap - 1) {
        int k = TABSPACE;
        while (k-- > 0) {
            if (fwd) {
                if (i <= *a)
                    (*a)++;
                insert(i, ' ', 1);
                (*b)++;
            } else {
                if (cap - gap > 0 && i < cap - gap && buf[bufaddr(i)] == ' ') {
                    delete(i, 1);
                    if (i <= *a)
                        (*a)--;
                    (*b)--;
                }
            }
        }
        if (buf[bufaddr(i)] != '\n')
            eol(&i);
        if (buf[bufaddr(i)] == '\n' && i < cap - gap - 1)
            next(&i);
    }
    record(UEND, 0, 0);
}

void newline(size_t *a, int O) {
    size_t b = *a;
    if (O && *a == 0) {
        insert(0, '\n', 1);
        return;
    }
    insert((*a)++, '\n', 1);
    sol(&b);
    if (buf[bufaddr(b)] == '\n' && b < cap - gap)
        next(&b);
    while (buf[bufaddr(b++)] == ' ' && b < cap - gap)
        insert((*a)++, ' ', 1);
}

void fileinit(void) {
    struct stat st;
    size_t n;
    ssize_t k;
    int fd;
    if ((fd = open(name, O_RDWR | O_CREAT, 0666)) > 0) {
        if (fstat(fd, &st) == -1)
            goto Error;
        n = st.st_size;
        if ((buf = calloc(n + GAPLEN, sizeof(char)))) {
            cap = n + GAPLEN;
            start = n;
            gap = GAPLEN;
            while (n) {
                if ((k = read(fd, buf, n)) == -1)
                    goto Error;
                n -= k;
            };
        }
        close(fd);
        return;
    }
Error:
    perror("fileinit");
    exit(1);
}

void sig(int n) {
    switch (n) {
    case SIGINT:
    case SIGWINCH:
        err(RESET);
        break;
    case SIGTERM:
    case SIGQUIT:
        err(PANIC);
    }
}

void sigpend(void) {
    sigset_t mask;
    if (sigfillset(&mask) != -1 && sigprocmask(SIG_SETMASK, &mask, &oset) != -1)
        return;
    perror("sigpend");
    exit(1);
}

void siginit(void) {
    static const int siglist[] = { SIGINT, SIGWINCH, SIGTERM, SIGQUIT };
    struct sigaction sa;
    sa.sa_handler = sig;
    sa.sa_flags = 0;
    if (sigfillset(&sa.sa_mask) != -1) {
        for (size_t i = 0; i < LEN(siglist); i++) {
            if (sigaction(siglist[i], &sa, NULL) == -1)
                goto Error;
        }
        if (sigprocmask(SIG_SETMASK, &oset, NULL) != -1)
            return;
    }
Error:
    perror("siginit");
    exit(1);
}

void terminit(void) {
    struct termios new;
    if (tcgetattr(STDIN_FILENO, &new) != -1) {
        term = new;
        new.c_iflag &= ~(BRKINT | ISTRIP | INLCR | INPCK | IXON);
        new.c_oflag &= ~OPOST;
        new.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        new.c_cflag |= CS8;
        new.c_cc[VMIN] = 0;
        new.c_cc[VTIME] = 1; /* ds */
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new) != -1)
            return;
    }
    perror("terminit");
    exit(1);
}

void init(void) {
    sigpend();
    ubufcap = ybufcap = GAPLEN;
    ubuflen = ybuflen = 0;
    if ((ubuf = malloc(ubufcap * sizeof(change)))) {
        if ((ybuf = malloc(ybufcap * sizeof(char)))) {
            setlocale(LC_ALL, "");
            fileinit();
            terminit();
            siginit();
            return;
        }
    }
    perror("init");
    exit(1);
}

void termreset(void) {
    (void)!write(STDOUT_FILENO, CSI("9999;1H\r\n"), 11);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);
}

void end(void) {
    termreset();
    free(buf);
    free(ybuf);
    free(ubuf);
}

void undo(size_t *a, size_t *b) {
    if (ubuflen > 0) {
        ubuflen--;
        while (ubuflen > 0) {
            change m = ubuf[--ubuflen];
            switch (m.type) {
            case UINSERT:
                delete(m.i, 0);
                *a = *b = m.i;
                break;
            case UDELETE:
                insert(m.i, m.c, 0);
                *a = *b = m.i;
                break;
            case UEND:
                ubuflen++;
                return;
            }
        }
    }
}

ssize_t writeall(int f, const char *s, size_t n) {
     int w;
     ssize_t r = n;
     while (n) {
         if ((w = write(f, s, n)) == -1) {
             if (errno == EINTR)
                 w = 0;
             else
                 return -1;
         }
         s += w;
         n -= w;
     }
     return r;
}

ssize_t writef(int f) {
     ssize_t n = 0, r = 0;
     if ((n = writeall(f, buf, start)) == -1)
         return -1;
     if ((r = writeall(f, buf + start + gap, cap - (start + gap))) == -1)
         return -1;
     return r + n;
}

void save(void) {
    int f;
    if ((f = creat("er.out", 0666)) > 0)
        writef(f);
}

void vflush(void) {
     if (writeall(STDOUT_FILENO, vbuf, vbuflen) == -1)
         err(PANIC);
     vbuflen = 0;
}

void vpush(const char *s) {
    while (*s) {
        if (vbuflen == VBUFMAX)
            vflush();
        vbuf[vbuflen++] = *s++;
    }
}

void cursor(unsigned int x, unsigned int y) {
    char tmp[32];
    if (snprintf(tmp, sizeof(tmp), CSI("%u;%uH"), y + 1 , x + 1) < 0)
        err(PANIC);
    vpush(tmp);
}

void bar(const char *fmt, ...) {
    char *tmp;
    va_list args;
    if (!(tmp = malloc((dim.ws_col + 1) * sizeof(char))))
        err(PANIC);
    va_start(args, fmt);
    vsnprintf(tmp, dim.ws_col + 1, fmt, args);
    va_end(args);
    cursor(0, dim.ws_row - 1);
    vpush(CSI("36m"));
    vpush(tmp);
    vpush(CSI("K"));
    vpush(CSI("0m"));
    vflush();
    free(tmp);
}

void search(size_t *a, size_t *b) {
    regex_t reg;
    regmatch_t m[1];
    static char pat[256] = { 0 };
    char tmp[128], *s;
    int r, k;
    static size_t i = 0;
    while ((k = key()) != '\n') {
        if (k == ESCAPE) {
            bar("");
            return;
        } else if (k == BACKSPACE) {
            i = 0;
            memset(pat, 0, sizeof(pat));
            bar("");
            continue;
        }
        s = ch;
        while (i < sizeof(pat) && *s)
            pat[i++] = *s++;
        bar(pat);
    }
    move(cap - gap);
    if (!(r = regcomp(&reg, pat, REG_EXTENDED | REG_NEWLINE))) {
        if (!(r = regexec(&reg, buf + *b, 1, m, 0)) && *b + m[0].rm_so < cap - gap) {
            *a = *b + m[0].rm_so;
            *b += m[0].rm_eo - 1;
        }
    }
    bar("");
    if (r != 0) {
        regerror(r, &reg, tmp, sizeof(tmp));
        bar(tmp);
    }
    regfree(&reg);
    return;
}

void display(void) {
    int i, j, l, i2, j2;
    size_t k;
    char tmp[32];
    l = digits(vline + dim.ws_row);
    j2 = l + 2;
    for (i = j = i2 = 0, k = vstart; i < dim.ws_row - 1; i++, j = 0) {
        cursor(j, i);
        if (k < cap - gap) {
            snprintf(tmp, sizeof(tmp), CSI("34m %*ld "), l, vline + i);
            vpush(tmp);
            vpush(CSI("0m"));
            j += l + 2;
            if (addr1 != addr2 && k >= addr1 && k <= addr2)
                vpush(CSI("7m"));
            do {
                if (addr1 != addr2 && k == addr1)
                    vpush(CSI("7m"));
                if (k == addr2) {
                    vpush(CSI("0m"));
                    j2 = j;
                    i2 = i;
                }
                j += next(&k);
                if (j < dim.ws_col)
                    vpush(ch[0] == '\n' ? " " : ch);
                if (ch[0] == '\n')
                    break;
                else if (j == dim.ws_col) {
                    cursor(0, i);
                    vpush(CSI("35m>"));
                    vpush(CSI("0m"));
                    cursor(j, i);
                }
            } while (k < cap - gap);
            if (k == addr2) {
                if (ch[0] == '\n') {
                    j2 = l + 2;
                    i2 = i + 1;
                } else {
                    j2 = j;
                    i2 = i;
                }
            }
        } else {
            vpush(CSI("90m~"));
            vpush(CSI("0m"));
        }
        vpush(CSI("K"));
    }
    vpush(CSI("?25h"));
    cursor(j2, i2);
    vflush();
}

void yank(void) {
    size_t i, j = addr1, k = addr2, n;
    char *new;
    next(&k);
    n = k - addr1;
    if (n > ybufcap) {
        if (!(new = realloc(ybuf, n * sizeof(char))))
            err(PANIC);
        ybuf = new;
        ybufcap = n;
    }
    for (i = 0; i < n; i++)
        ybuf[i] = buf[bufaddr(j++)];
    ybuflen = n;
    bar("%ld bytes yanked", ybuflen);
}

int motion(int k) {
    static int ignore[] = { 'h', 'j', 'k', 'l', '0', '$' };
    if (mode == INPUT) {
        for (size_t i = 0; i < LEN(ignore); i++) {
            if (k == ignore[i])
                return -1;
        }
    }
    size_t tmp;
    refresh = 1;
    switch (k) {
    case LEFT:
    case 'h':
        if (mode != SELECT && addr1 > 0) {
            prev(&addr1);
            addr2 = addr1;
        } else if (mode == SELECT && addr2 > addr1)
            prev(&addr2);
        checkline(0);
        break;
    case RIGHT:
    case 'l':
        if (addr2 < cap - gap - 1) {
            next(&addr2);
            if (mode != SELECT)
                addr1 = addr2;
            checkline(1);
        }
        break;
    case UP:
    case 'k':
        if (addr2 > 0)
            prevline(&addr2);
        if (mode != SELECT)
            addr1 = addr2;
        checkline(0);
        break;
    case DOWN:
    case 'j':
        if (addr2 < cap - gap - 1)
            nextline(&addr2);
        if (mode != SELECT)
            addr1 = addr2;
        checkline(1);
        break;
    case HOME:
    case CTRL('a'):
    case '0':
        sol(&addr1);
        if (addr1 > 0 && addr1 < cap - gap - 1)
            next(&addr1);
        if (mode != SELECT)
            addr2 = addr1;
        break;
    case END:
    case CTRL('e'):
    case '$':
        eol(&addr2);
        if (mode != SELECT)
            addr1 = addr2;
        break;
    case PAGEUP:
    case CTRL('b'):
        tmp = dim.ws_row - 1;
        while (tmp-- > 0)
            prevline(&addr1);
        addr2 = addr1;
        checkline(0);
        break;
    case PAGEDOWN:
    case CTRL('f'):
        tmp = dim.ws_row - 1;
        while (tmp-- > 0)
            nextline(&addr2);
        addr1 = addr2;
        checkline(1);
        break;
    default:
        refresh = 0;
        return -1;
    };
    if (addr1 > addr2) {
        addr1 = addr2;
        MODE(COMMAND);
    }
    return 1;
}

void command(int k) {
    size_t i;
    ssize_t r;
    int fd;
    char *s;
    if (motion(k) > 0)
        return;
    refresh = 1;
    switch (k) {
    case ESCAPE:
        addr1 = addr2;
        MODE(COMMAND);
        break;
    case 'i':
    case INSERT:
        addr1 = addr2;
        MODE(INPUT);
        break;
    case 'a':
        if (cap - gap > 0)
            next(&addr2);
        addr1 = addr2;
        MODE(INPUT);
        break;
    case 'o':
        if (buf[bufaddr(addr2)] != '\n')
            eol(&addr2);
        newline(&addr2, 0);
        record(UEND, 0, 0);
        addr1 = addr2;
        checkline(1);
        MODE(INPUT);
        break;
    case 'O':
        if (buf[bufaddr(addr1)] != '\n')
            sol(&addr1);
        newline(&addr1, 1);
        record(UEND, 0, 0);
        addr2 = addr1;
        checkline(0);
        MODE(INPUT);
        break;
    case 'v':
        MODE(SELECT);
        break;
    case 'V':
        sol(&addr1);
        if (addr1 > 0 && addr1 < cap - gap - 1)
            next(&addr1);
        eol(&addr2);
        MODE(SELECT);
        break;
    case 'd':
        yank(); /* fallthrough */
    case 'x':
        if (cap - gap > 0) {
            if (addr2 > cap - gap - 1) {
                prev(&addr2);
                addr1 = addr2;
            }
            i = 1 + addr2 - addr1;
            while(i-- > 0)
                delete(addr1, 1);
            record(UEND, 0, 0);
            addr2 = addr1;
            mode = COMMAND;
        }
        break;
    case 'r':
        while (key() < 0);
        delete(addr2, 1);
        s = ch;
        while(*s)
            insert(addr2++, *s++, 1);
        record(UEND, 0, 0);
        addr1 = addr2;
        break;
    case 'y':
        yank();
        addr1 = addr2;
        mode = COMMAND;
        break;
    case 'p':
        if (ybuflen > 0)
            next(&addr2); /* fallthrough */
    case 'P':
        for (i = 0; i < ybuflen; i++)
            insert(addr2++, ybuf[i], 1);
        record(UEND, 0, 0);
        addr1 = addr2;
        bar("%ld bytes pasted", ybuflen);
        break;
    case 'u':
        undo(&addr1, &addr2);
        break;
    case '<':
        indent(&addr1, &addr2, 0);
        break;
    case '>':
        indent(&addr1, &addr2, 1);
        break;
    case 'g':
        addr1 = addr2 = vstart = vline = 0;
        break;
    case 'G':
        while (addr2 < cap - gap - 1)
            next(&addr2);
        addr1 = addr2;
        checkline(1);
        break;
    case CTRL('G'):
        i = addr1;
        sol(&i);
        if (buf[bufaddr(i)] == '\n' && i < cap - gap)
            next(&i);
        r = 0;
        while (i < addr1) {
            next(&i);
            r++;
        }
        fd = checkline(1) - 1;
        bar("Line %ld, Column %ld, %ld of %ld bytes (%.1f%%)",
            vline + fd, r, addr1, cap - gap,
            (cap - gap > 0) ? 100.0 * (addr1 + 1) / (cap - gap) : 0);
        break;
    case 's':
        search(&addr1, &addr2);
        checkline(1);
        break;
    case 'W':
        if (cap - gap == 0 || buf[bufaddr(cap - gap - 1)] != '\n')
            insert(cap - gap, '\n', 0);
        if ((fd = open(name, O_WRONLY | O_TRUNC, 0666)) > 0) {
            if ((r = writef(fd)) > 0) {
                dirty = 0;
                bar("%ld bytes written to %s", r, name);
                close(fd);
                return;
            }
        }
        err(PANIC);
        break;
    case 'Q':
        if (dirty) {
            bar("Buffer contains unsaved modifications");
            return;
        } /* fallthrough */
    case 'Z':
        quit = 1;
        break;
    default:
        refresh = 0;
        break;
    }
}

void input(int k) {
    char *s;
    if (motion(k) > 0)
        return;
    refresh = 1;
    switch (k) {
    case NONE:
        refresh = 0;
        break;
    case BACKSPACE:
        if (addr2 > 0) {
            prev(&addr2);
            delete(addr2, 1);
            record(UEND, 0, 0);
            addr1 = addr2;
        }
        break;
    case DELETE:
        if (addr2 < cap - gap) {
            delete(addr2, 1);
            record(UEND, 0, 0);
        }
        break;
    case ESCAPE:
        if (addr2 > cap - gap - 1)
            addr2--;
        addr1 = addr2;
        MODE(COMMAND);
        break;
    case '\t':
        k = TABSPACE;
        while (k-- > 0)
            insert(addr2++, ' ', 1);
        record(UEND, 0, 0);
        addr1 = addr2;
        break;
    case '\n':
        newline(&addr2, 0);
        record(UEND, 0, 0);
        addr1 = addr2;
        break;
    default:
        s = ch;
        while(*s)
            insert(addr2++, *s++, 1);
        memset(ch, 0, 5);
        record(UEND, 0, 0);
        addr1 = addr2;
        checkline(1);
        break;
    }
}

void run(void) {
    int k;
    if (dims() == -1)
        err(PANIC);
    while (!quit) {
        if (refresh) {
            display();
            refresh = 0;
        }
        k = key();
        (mode == INPUT) ? input(k) : command(k);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "er (%d.%d.%d)\nUsage:\n\ter file\n", 0, 1, 0);
        exit(1);
    }
    name = argv[1];
    if (!sigsetjmp(env, 1))
         init();
    refresh = 1;
    (status != PANIC) ? run() : save();
    end();
    if (status == PANIC)
        fprintf(stderr, "er panicked and tried to save buffer to er.out");
    return 0;
}
