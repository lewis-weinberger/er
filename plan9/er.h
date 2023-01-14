#include <u.h>
#include <libc.h>
#include <stdio.h>
#include <regexp.h>
#include <keyboard.h>

#define CTRL(X)   (X & 0x1F)
#define PATH_MAX  255
#define SAVE      (OWRITE | OTRUNC)
#define jmp(X, Y) setjmp(X)
#define locale() 

#ifndef _SIZE_T
typedef uvlong size_t;
typedef vlong  ssize_t;
#endif

#ifndef fstat
/* needed for plan9port? */
#include <sys/stat.h>
#endif

extern volatile int status;
extern char         ch[5];
