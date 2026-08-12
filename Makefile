CC := gcc
override CFLAGS += -Wno-int-conversion -Wno-unused-variable -Wno-error

.PHONY: all clean
all:
	$(CC) $(CFLAGS) -o code main.c buddy.c
clean:
	rm -f code test
