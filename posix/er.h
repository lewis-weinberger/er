#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

#define SAVE      (O_WRONLY | O_TRUNC)
#define jmp       sigsetjmp
#define locale()  setlocale(LC_ALL, "")

/* keyboard keys */
enum
{
	Kesc = 0x1b,
	Kbs  = 0x7f,
	Kdel = 0xE000,
	Kleft,
	Kright,
	Kup,
	Kdown,
	Khome,
	Kend,
	Kpgup,
	Kpgdown,
	Kins
};

extern volatile sig_atomic_t status;
extern char                  ch[5];

int decode(char, int (*)(size_t*), int (*)(void), size_t*, wchar_t*);
