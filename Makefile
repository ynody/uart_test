CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic

.PHONY: all test test-3buf test-all test-asan test-3buf-asan clean

all: test_uart_comm

test_uart_comm: uart_comm.c uart_comm.h test_uart_comm.c
	$(CC) $(CFLAGS) uart_comm.c test_uart_comm.c -o $@

test: test_uart_comm
	./test_uart_comm

test-3buf: uart_comm.c uart_comm.h test_uart_comm.c
	$(CC) $(CFLAGS) -DUART_COMM_RX_BUFFER_COUNT=3 uart_comm.c test_uart_comm.c -o test_uart_comm_3buf
	./test_uart_comm_3buf

test-all: test test-3buf

test-asan: uart_comm.c uart_comm.h test_uart_comm.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer uart_comm.c test_uart_comm.c -o test_uart_comm_asan
	./test_uart_comm_asan

test-3buf-asan: uart_comm.c uart_comm.h test_uart_comm.c
	$(CC) $(CFLAGS) -DUART_COMM_RX_BUFFER_COUNT=3 -fsanitize=address,undefined -fno-omit-frame-pointer uart_comm.c test_uart_comm.c -o test_uart_comm_3buf_asan
	./test_uart_comm_3buf_asan

clean:
	rm -f test_uart_comm test_uart_comm_3buf test_uart_comm_asan test_uart_comm_3buf_asan
