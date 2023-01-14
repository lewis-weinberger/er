# Makefile for UNIX-like systems (SUSv3/POSIX.1-2001)

# Change as needed
CC     = gcc
CFLAGS = -std=c99 -O2 -pedantic -Wall -Wextra
PREFIX = /usr/local

# MacOS
ifeq ($(shell uname),Darwin)
	CFLAGS += -D_DARWIN_C_SOURCE
endif

INC = posix/er.h dat.h fns.h

er: er.o ui.o util.o decode.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

%.o: %.c $(INC)
	$(CC) $(CFLAGS) -D_XOPEN_SOURCE=600 -Iposix -c -o $@ $<

%.o: posix/%.c $(INC)
	$(CC) $(CFLAGS) -D_XOPEN_SOURCE=600 -Iposix -c -o $@ $<

clean:
	rm -f *.o er er.out

install: er er.1
	mkdir -p $(PREFIX)/bin
	cp -f er $(PREFIX)/bin/
	chmod 755 $(PREFIX)/bin/er
	mkdir -p $(PREFIX)/share/man/man1
	cp -f er.1 $(PREFIX)/share/man/man1/
	chmod 644 $(PREFIX)/share/man/man1/er.1

.PHONY: clean install
