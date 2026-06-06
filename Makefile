CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11
LDFLAGS =

all: file-server file-client

file-server: file-server.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lpthread

file-client: file-client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f file-server file-client