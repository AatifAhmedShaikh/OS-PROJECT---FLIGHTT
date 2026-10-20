CC     ?= gcc
CFLAGS  = -std=gnu11 -O2 -Wall -Wextra
LIB     = sched.c airline.c aerocore.h

aerocore: main.c $(LIB)
	$(CC) $(CFLAGS) -o $@ main.c sched.c airline.c

test: test_aerocore
	./test_aerocore

test_aerocore: test.c $(LIB)
	$(CC) $(CFLAGS) -o $@ test.c sched.c airline.c

clean:
	rm -f aerocore test_aerocore *.journal

.PHONY: test clean
