CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDLIBS = -lm

carnivorus: carnivorus.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: carnivorus
	./tests/perft.sh

clean:
	rm -f carnivorus

.PHONY: test clean
