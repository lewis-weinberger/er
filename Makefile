# Makefile for UNIX-like systems (SUSv3/POSIX.1-2001)

.POSIX:
.SUFFIXES:

CC      = cc
CFLAGS  = -std=c99 -O2 -pedantic -Wall -Wextra
LDFLAGS =
LDLIBS  = -lm
PREFIX  = /usr/local

er: er.o
	$(CC) $(LDFLAGS) -o $@ er.o $(LDLIBS)

clean:
	rm -f er er.o er.out

install: er er.1
	mkdir -p $(PREFIX)/bin
	cp -f er $(PREFIX)/bin/
	chmod 755 $(PREFIX)/bin/er
	mkdir -p $(PREFIX)/share/man/man1
	cp -f er.1 $(PREFIX)/share/man/man1/
	chmod 644 $(PREFIX)/share/man/man1/er.1

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) -o $@ -c $<
