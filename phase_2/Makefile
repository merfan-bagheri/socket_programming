CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -O3 -pthread -std=c99
DEBUG_CFLAGS = -Wall -Wextra -pedantic -g -O0 -pthread -fsanitize=address -std=c99

all: server client

server: server.c common.h
	$(CC) $(CFLAGS) server.c -o server

client: client.c common.h
	$(CC) $(CFLAGS) client.c -o client

debug: server.c client.c common.h
	$(CC) $(DEBUG_CFLAGS) server.c -o server_debug
	$(CC) $(DEBUG_CFLAGS) client.c -o client_debug

clean:
	rm -f server client server_debug client_debug

.PHONY: all debug clean
