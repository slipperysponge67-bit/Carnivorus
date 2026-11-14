CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11 -pthread
LDLIBS = -lm

carnivorus: carnivorus.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: carnivorus
	sh tests/perft.sh
	$(CC) $(CFLAGS) -o /tmp/carnivorus-regression tests/regression.c $(LDLIBS)
	/tmp/carnivorus-regression

clean:
	rm -f carnivorus

.PHONY: test clean
