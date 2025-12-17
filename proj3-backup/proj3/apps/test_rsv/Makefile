CC = gcc
CFLAGS = -Wall -O2

all: test_rsv test_exit

test_rsv: test_rsv.c
	$(CC) $(CFLAGS) -o test_rsv test_rsv.c

test_exit: test_exit.c
	$(CC) $(CFLAGS) -o test_exit test_exit.c

clean:
	rm -f test_rsv test_exit

.PHONY: all clean
