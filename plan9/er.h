#include <u.h>
#include <libc.h>
#include <draw.h>
#include <event.h>
#include <keyboard.h>
#include <regexp.h>
#include <stdio.h>

#define CTRL(X)   (X & 0x1F)
#define PATH_MAX  255
#define SAVE      (OWRITE | OTRUNC)
#define jmp(X, Y) setjmp(X)
#define locale() 

#ifndef _SIZE_T
typedef uvlong size_t;
typedef vlong  ssize_t;
#endif

extern volatile int status;
extern char         ch[5];
