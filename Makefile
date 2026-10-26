CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -pthread

all:
	$(CC) $(CFLAGS) torneo.c -o torneo

clean:
	rm -f torneo