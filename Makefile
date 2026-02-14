CC = gcc
CFLAGS = -std=c99 -O2 -pthread -g
TEST_CFLAGS = -std=c99 -O2 -pthread -g

SRC = thread_pool.c
OBJ = $(SRC:.c=.o)

TARGET = thread_pool.o

all: $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
	rm -f test_mpmc

test_mpmc: tests/test_mpmc.c thread_pool.c
	$(CC) $(TEST_CFLAGS) -o $@ tests/test_mpmc.c
	./test_mpmc

.PHONY: all clean test_mpmc
