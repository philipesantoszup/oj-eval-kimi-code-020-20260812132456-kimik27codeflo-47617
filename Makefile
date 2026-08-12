.PHONY: all clean
all:
	gcc -Wall -Wextra -o code main.c buddy.c
clean:
	rm -f code test
