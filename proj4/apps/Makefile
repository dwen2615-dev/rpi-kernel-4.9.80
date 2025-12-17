CC = gcc
CFLAGS = -Wall -pthread -O2

all: test_edf_basic

test_edf_basic: test_edf_basic.c
	$(CC) $(CFLAGS) -o test_edf_basic test_edf_basic.c

clean:
	rm -f test_edf_basic *.o

.PHONY: all clean
