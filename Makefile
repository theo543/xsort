CC ?= gcc
CFLAGS ?= -O0 -g -fsanitize=address,undefined -Wall -Wextra -pedantic

xsort: xsort.c xsort_subproc.c utils.c utils.h
	$(CC) $(CFLAGS) -o $@ $^ -lX11

.PHONY = clean run

clean:
	rm -f xsort

run: xsort
	./xsort
