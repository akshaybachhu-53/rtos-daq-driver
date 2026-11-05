# Host unit tests for the ring buffer.
#   make -C Tests        build and run
#   make -C Tests clean
CC      ?= gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror -pthread -I../Core/Inc
SRCS    := test_ring_buffer.c ../Core/Src/ring_buffer.c
BIN     := build/test_ring_buffer

.PHONY: all clean
all: $(BIN)
	./$(BIN)

$(BIN): $(SRCS) ../Core/Inc/ring_buffer.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) -o $@

clean:
	rm -rf build
