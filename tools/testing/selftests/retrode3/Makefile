# Hardware-free adapter transport policy tests. No Retrode3 required.
CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -pedantic

.PHONY: all test clean
all: adapter_mode_test
adapter_mode_test: adapter_mode_test.c ../../../../drivers/misc/retrode3/retrode3_adapter_mode.h
	$(CC) $(CFLAGS) adapter_mode_test.c -o $@

test: adapter_mode_test
	./adapter_mode_test

clean:
	rm -f adapter_mode_test
