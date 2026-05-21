CC ?= gcc
CFLAGS ?= -Wall -Werror -pthread -O2

.PHONY: all clean test

all: psort gen_records check_sorted

psort: psort.c
	$(CC) $(CFLAGS) -o $@ $<

gen_records: gen_records.c
	$(CC) $(CFLAGS) -o $@ $<

check_sorted: check_sorted.c
	$(CC) $(CFLAGS) -o $@ $<

test: all
	./gen_records 100000 input.bin 42
	./psort input.bin output.bin
	./check_sorted input.bin output.bin

clean:
	rm -f psort gen_records check_sorted input.bin output.bin small.bin small.sorted.bin empty.bin empty.sorted.bin
