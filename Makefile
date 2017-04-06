all: cactus

cactus: cactus.c
	$(CC) cactus.c -o cactus -Wall -Wextra -pedantic -std=c99

clean:
	rm cactus
