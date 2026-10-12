CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c99

pacman: pacman.c
	$(CC) $(CFLAGS) -o $@ pacman.c

clean:
	rm -f pacman

.PHONY: clean
