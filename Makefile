sump-dump: sump-dump.c
	$(CC) -std=c11 -Wall -Werror -g3 -o $@ $<
