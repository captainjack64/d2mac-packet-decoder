CC=gcc
CFLAGS=-g -Wall
LDFLAGS=-g

OBJS=d2mac-decoder.o

all: d2mac-decoder

readframe: $(OBJS)
	$(CC) -o d2mac-decoder $(OBJS) $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o d2mac-decoder

