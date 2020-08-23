CFLAGS = -Wextra -Wall -g

.PHONY: build clean
build: libscheduler

libscheduler:
	gcc -fPIC $(CFLAGS) -o so_scheduler.o -c so_scheduler.c
	gcc -fPIC $(CFLAGS) -shared -o libscheduler.so so_scheduler.o

clean:
	rm -f so_scheduler.o libscheduler.so

