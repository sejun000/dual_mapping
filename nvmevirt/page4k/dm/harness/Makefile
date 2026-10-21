# userspace replay harness: make POLICY=page4k  ->  replay_page4k
POLICY ?= page4k
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11
replay_$(POLICY): replay.c ../dm_wbuf.c ../ftl_$(POLICY).c ../*.h
	$(CC) $(CFLAGS) -o $@ replay.c ../dm_wbuf.c ../ftl_$(POLICY).c -lm
clean:
	rm -f replay_*
