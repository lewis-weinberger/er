CC     = gcc
CFLAGS = -std=c99 -O2 -pedantic -Wall -Wextra

ifeq ($(shell uname),Darwin)
	CFLAGS += -D_DARWIN_C_SOURCE
endif

er: er.c
	$(CC) $(CFLAGS) -D_XOPEN_SOURCE=600 er.c -o er -lm

clean:
	rm -f er er.out

.PHONY: clean
