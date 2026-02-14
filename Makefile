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
	rm -f test_mpmc test_thread_pool

test_mpmc: tests/test_mpmc.c thread_pool.c
	$(CC) $(TEST_CFLAGS) -o $@ tests/test_mpmc.c
	./$@

test_thread_pool: tests/test_thread_pool.c thread_pool.c
	$(CC) $(TEST_CFLAGS) -o $@ tests/test_thread_pool.c
	./$@

tests: test_mpmc test_thread_pool

.PHONY: all clean test_mpmc test_thread_pool tests
