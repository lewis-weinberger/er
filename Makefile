# Makefile for UNIX-like systems (SUSv3/POSIX.1-2001)

# Change as needed
CC     = gcc
CFLAGS = -std=c99 -O2 -pedantic -Wall -Wextra

# MacOS
ifeq ($(shell uname),Darwin)
	CFLAGS += -D_DARWIN_C_SOURCE
endif

er: er.c
	$(CC) $(CFLAGS) -D_XOPEN_SOURCE=600 er.c -o er -lm

clean:
	rm -f er er.out

.PHONY: clean
