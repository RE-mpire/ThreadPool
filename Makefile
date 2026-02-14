CC = gcc
CFLAGS = -std=c99 -O2 -pthread -g

SRC = thread_pool.c gol.c
OBJ = $(SRC:.c=.o)

TARGET = gol

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
