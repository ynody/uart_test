#include "uart_comm.h"

/** @brief Callback configuration copied for the single core instance. */
static uart_comm_config_t g_config;
/** @brief Initialization flag read by both normal and receive contexts. */
static volatile unsigned char g_initialized;

/** @brief Statically allocated command-line storage for every FIFO buffer. */
static char g_rx_buffers[UART_COMM_RX_BUFFER_COUNT][UART_COMM_LINE_BUFFER_SIZE];
/** @brief Per-buffer ownership states shared with the receive context. */
static volatile uart_comm_buffer_state_t g_buffer_states[UART_COMM_RX_BUFFER_COUNT];
/** @brief Per-buffer valid command lengths, excluding NUL. */
static volatile uint32 g_buffer_lengths[UART_COMM_RX_BUFFER_COUNT];
/** @brief Per-buffer line-overflow flags captured at line completion. */
static volatile unsigned char g_buffer_overflow[UART_COMM_RX_BUFFER_COUNT];
/** @brief Per-buffer invalid-character flags captured at line completion. */
static volatile unsigned char g_buffer_invalid_character[UART_COMM_RX_BUFFER_COUNT];
/** @brief FIFO entries containing completed receive-buffer indices. */
static volatile uint32 g_fifo[UART_COMM_RX_BUFFER_COUNT];
/** @brief FIFO read position shared by normal and receive contexts. */
static volatile uint32 g_fifo_head;
/** @brief FIFO write position shared by normal and receive contexts. */
static volatile uint32 g_fifo_tail;
/** @brief Number of completed lines currently queued in the FIFO. */
static volatile uint32 g_fifo_count;

/** @brief Index of the buffer currently owned by the receive context. */
static volatile uint32 g_receiving_buffer;
/** @brief Number of stored bytes in the currently received line. */
static volatile uint32 g_receiving_length;
/** @brief Overflow flag for the currently received line. */
static volatile unsigned char g_receiving_overflow;
/** @brief Invalid-byte flag for the currently received line. */
static volatile unsigned char g_receiving_invalid_character;
/** @brief Indicates that one CR is held pending CRLF recognition. */
static volatile unsigned char g_receiving_pending_cr;
/** @brief Indicates that no buffer is selected until the next input byte. */
static volatile unsigned char g_waiting_for_receive_buffer;
/** @brief Indicates that the current line is being discarded until LF. */
static volatile unsigned char g_discarding_line;
/** @brief Number of discarded lines awaiting normal-context error reporting. */
static volatile uint32 g_queue_overflow_pending;

/**
 * @brief Advance a circular FIFO index without using division.
 * @param index Current valid FIFO index.
 * @return The next FIFO index, wrapping to zero at the configured count.
 */
static uint32 uart_comm_next_index(uint32 index)
{
    ++index;
    if (index >= UART_COMM_RX_BUFFER_COUNT) {
        index = 0U;
    }
    return index;
}

/**
 * @brief Test whether a byte is an S/L command separator.
 * @param character Byte to classify.
 * @return Nonzero for ASCII space or horizontal tab.
 */
static unsigned char uart_comm_is_separator(unsigned char character)
{
    return (unsigned char)(character == (unsigned char)' ' ||
                           character == (unsigned char)'\t');
}

/**
 * @brief Send an error result through the configured output wrapper.
 * @param result Error result to report.
 *
 * This helper runs only in normal command-processing context.
 */
static void uart_comm_report_error(uart_comm_result_t result)
{
    if (g_config.report_error != 0) {
        g_config.report_error(g_config.context, result);
    }
}

/**
 * @brief Report an error and return the same result to the caller.
 * @param result Result to report and return.
 * @return The supplied result.
 */
static uart_comm_result_t uart_comm_fail(uart_comm_result_t result)
{
    uart_comm_report_error(result);
    return result;
}

/**
 * @brief Check configuration required by normal-context processing.
 * @return Nonzero when initialization, error reporting, and critical-section
 *         callbacks are all available.
 */
static unsigned char uart_comm_has_base_configuration(void)
{
    return (unsigned char)(g_initialized != 0U &&
                           g_config.report_error != 0 &&
                           g_config.enter_critical != 0 &&
                           g_config.exit_critical != 0);
}

/**
 * @brief Find a FREE receive buffer.
 * @param index Destination for the selected buffer index.
 * @return Nonzero when a FREE buffer was found.
 *
 * Called by the receive context and therefore limited to fixed-size scanning.
 */
static unsigned char uart_comm_find_free_buffer(uint32 *index)
{
    uint32 candidate;

    for (candidate = 0U; candidate < UART_COMM_RX_BUFFER_COUNT; ++candidate) {
        if (g_buffer_states[candidate] == UART_COMM_BUFFER_FREE) {
            *index = candidate;
            return 1U;
        }
    }
    return 0U;
}

/**
 * @brief Initialize one buffer as the active receive target.
 * @param index Buffer index to transition to RECEIVING.
 */
static void uart_comm_start_receiving(uint32 index)
{
    g_receiving_buffer = index;
    g_receiving_length = 0U;
    g_receiving_overflow = 0U;
    g_receiving_invalid_character = 0U;
    g_receiving_pending_cr = 0U;
    g_buffer_lengths[index] = 0U;
    g_buffer_states[index] = UART_COMM_BUFFER_RECEIVING;
}

/**
 * @brief Append one validated byte to the active receive line.
 * @param character Byte after CR handling; invalid bytes mark the line bad.
 *
 * This helper is used by the lightweight receive path and never parses a
 * command or invokes callbacks.
 */
static void uart_comm_append_received_byte(unsigned char character)
{
    /* Once an invalid byte is seen, discard the rest of this line. */
    if (g_receiving_invalid_character != 0U) {
        return;
    }

    if (character == 0U || character >= 0x80U) {
        g_receiving_invalid_character = 1U;
        return;
    }

    if (g_receiving_length < UART_COMM_MAX_LINE_LENGTH) {
        g_rx_buffers[g_receiving_buffer][g_receiving_length] = (char)character;
        ++g_receiving_length;
    } else {
        g_receiving_overflow = 1U;
    }
}

/**
 * @brief Complete the active line and enqueue its buffer.
 * @return UART_COMM_RECEIVE_LINE_READY for the completed line.
 *
 * This function is receive-context code only; it selects a new buffer or
 * enters the deferred no-buffer state without invoking output callbacks.
 */
static uart_comm_receive_state_t uart_comm_finish_received_line(void)
{
    uint32 next_buffer;
    uint32 current = g_receiving_buffer;

    g_rx_buffers[current][g_receiving_length] = '\0';
    g_buffer_lengths[current] = g_receiving_length;
    g_buffer_overflow[current] = g_receiving_overflow;
    g_buffer_invalid_character[current] = g_receiving_invalid_character;
    g_buffer_states[current] = UART_COMM_BUFFER_READY;
    g_fifo[g_fifo_tail] = current;
    g_fifo_tail = uart_comm_next_index(g_fifo_tail);
    ++g_fifo_count;

    if (uart_comm_find_free_buffer(&next_buffer) != 0U) {
        g_waiting_for_receive_buffer = 0U;
        g_discarding_line = 0U;
        uart_comm_start_receiving(next_buffer);
    } else {
        /* Wait for the first byte of the next line before declaring overflow. */
        g_waiting_for_receive_buffer = 1U;
        g_discarding_line = 0U;
        g_receiving_pending_cr = 0U;
    }
    return UART_COMM_RECEIVE_LINE_READY;
}

/**
 * @brief Finish one discarded line and queue its deferred overflow report.
 *
 * The function is called from the receive context at LF and never reports
 * directly; normal context consumes the pending counter later.
 */
static void uart_comm_finish_discarded_line(void)
{
    uint32 next_buffer;

    /* Report exactly one overflow for the line just discarded. */
    if (g_queue_overflow_pending != 0xFFFFFFFFU) {
        ++g_queue_overflow_pending;
    }

    if (uart_comm_find_free_buffer(&next_buffer) != 0U) {
        g_waiting_for_receive_buffer = 0U;
        g_discarding_line = 0U;
        uart_comm_start_receiving(next_buffer);
    } else {
        /* The discarded line ended; wait for a future byte before retrying. */
        g_discarding_line = 0U;
        g_waiting_for_receive_buffer = 1U;
    }
}

/**
 * @brief Reset all receive state and install callback configuration.
 * @param config Configuration copied into the single module instance.
 *
 * Call from normal initialization context before enabling UART reception.
 */
void uart_comm_init(const uart_comm_config_t *config)
{
    uint32 index;

    g_initialized = 0U;
    g_config.register_read = 0;
    g_config.register_write = 0;
    g_config.external_command = 0;
    g_config.report_set = 0;
    g_config.report_read = 0;
    g_config.report_transfer = 0;
    g_config.report_error = 0;
    g_config.enter_critical = 0;
    g_config.exit_critical = 0;
    g_config.context = 0;

    if (config != 0) {
        g_config = *config;
    }

    g_fifo_head = 0U;
    g_fifo_tail = 0U;
    g_fifo_count = 0U;
    g_receiving_buffer = 0U;
    g_receiving_length = 0U;
    g_receiving_overflow = 0U;
    g_receiving_invalid_character = 0U;
    g_receiving_pending_cr = 0U;
    g_waiting_for_receive_buffer = 0U;
    g_discarding_line = 0U;
    g_queue_overflow_pending = 0U;

    for (index = 0U; index < UART_COMM_RX_BUFFER_COUNT; ++index) {
        g_buffer_states[index] = UART_COMM_BUFFER_FREE;
        g_buffer_lengths[index] = 0U;
        g_buffer_overflow[index] = 0U;
        g_buffer_invalid_character[index] = 0U;
    }
    uart_comm_start_receiving(0U);
    g_initialized = 1U;
}

/**
 * @brief Receive one byte from the UART and update line-buffer state.
 * @param character Received byte.
 * @return Line-ready state when LF completed and queued a line; otherwise
 *         receive-in-progress.
 *
 * This entry point is ISR-safe and intentionally performs no parsing or
 * callback invocation.
 */
uart_comm_receive_state_t uart_comm_receive_char(char character)
{
    unsigned char byte = (unsigned char)character;

    if (g_initialized == 0U) {
        return UART_COMM_RECEIVE_IN_PROGRESS;
    }

    /* A completed line may temporarily have no receiving buffer. */
    if (g_waiting_for_receive_buffer != 0U) {
        uint32 next_buffer;

        if (uart_comm_find_free_buffer(&next_buffer) != 0U) {
            g_waiting_for_receive_buffer = 0U;
            uart_comm_start_receiving(next_buffer);
        } else {
            g_waiting_for_receive_buffer = 0U;
            g_discarding_line = 1U;
            if (byte == (unsigned char)'\n') {
                uart_comm_finish_discarded_line();
            }
            return UART_COMM_RECEIVE_IN_PROGRESS;
        }
    }

    if (g_discarding_line != 0U) {
        if (byte == (unsigned char)'\n') {
            uart_comm_finish_discarded_line();
        }
        return UART_COMM_RECEIVE_IN_PROGRESS;
    }

    /* Hold CR until the following byte tells us whether this is CRLF. */
    if (byte == (unsigned char)'\r') {
        if (g_receiving_pending_cr != 0U) {
            /* The previous CR is content; only the final CR precedes LF. */
            uart_comm_append_received_byte((unsigned char)'\r');
        }
        g_receiving_pending_cr = 1U;
        return UART_COMM_RECEIVE_IN_PROGRESS;
    }

    if (byte == (unsigned char)'\n') {
        g_receiving_pending_cr = 0U;
        return uart_comm_finish_received_line();
    }

    if (g_receiving_pending_cr != 0U) {
        uart_comm_append_received_byte((unsigned char)'\r');
        g_receiving_pending_cr = 0U;
    }
    uart_comm_append_received_byte(byte);
    return UART_COMM_RECEIVE_IN_PROGRESS;
}

/**
 * @brief Test for queued lines or deferred queue-overflow reports.
 * @return Nonzero when normal-context processing has work pending.
 */
unsigned char uart_comm_has_pending_line(void)
{
    return (unsigned char)(g_fifo_count != 0U ||
                           g_queue_overflow_pending != 0U);
}

/** @brief Non-owning token view into a command line. */
typedef struct {
    const char *text;
    uint32 length;
} uart_comm_token_t;

/**
 * @brief Extract the next SP/HT-delimited token from a line.
 * @param line Complete command line.
 * @param length Number of bytes in @p line.
 * @param position In/out scan position.
 * @param token Destination token view.
 * @return Nonzero when a token was found.
 */
static unsigned char uart_comm_next_token(const char *line,
                                          uint32 length,
                                          uint32 *position,
                                          uart_comm_token_t *token)
{
    uint32 start;

    while (*position < length &&
           uart_comm_is_separator((unsigned char)line[*position]) != 0U) {
        ++(*position);
    }
    if (*position >= length) {
        return 0U;
    }

    start = *position;
    while (*position < length &&
           uart_comm_is_separator((unsigned char)line[*position]) == 0U) {
        ++(*position);
    }
    token->text = &line[start];
    token->length = *position - start;
    return 1U;
}

/**
 * @brief Convert one ASCII hexadecimal digit to its numeric value.
 * @param character Byte to convert.
 * @return Digit value 0-15, or -1 for a non-hexadecimal byte.
 */
static int uart_comm_hex_digit(unsigned char character)
{
    if (character >= (unsigned char)'0' && character <= (unsigned char)'9') {
        return (int)(character - (unsigned char)'0');
    }
    if (character >= (unsigned char)'A' && character <= (unsigned char)'F') {
        return (int)(character - (unsigned char)'A' + 10U);
    }
    if (character >= (unsigned char)'a' && character <= (unsigned char)'f') {
        return (int)(character - (unsigned char)'a' + 10U);
    }
    return -1;
}

/**
 * @brief Parse an optional-0x hexadecimal address token.
 * @param token Address token view.
 * @param address Destination uint32 address.
 * @return OK or INVALID_ADDRESS.
 */
static uart_comm_result_t uart_comm_parse_address(const uart_comm_token_t *token,
                                                  uint32 *address)
{
    uint32 position = 0U;
    uint32 value = 0U;
    int digit;

    if (token->length >= 2U && token->text[0] == '0' &&
        (token->text[1] == 'x' || token->text[1] == 'X')) {
        position = 2U;
    }
    if (position == token->length || token->length - position > 8U) {
        return UART_COMM_RESULT_INVALID_ADDRESS;
    }
    while (position < token->length) {
        digit = uart_comm_hex_digit((unsigned char)token->text[position]);
        if (digit < 0) {
            return UART_COMM_RESULT_INVALID_ADDRESS;
        }
        /* Eight hex digits have already been bounded above. */
        value = (value << 4U) | (uint32)digit;
        ++position;
    }
    *address = value;
    return UART_COMM_RESULT_OK;
}

/**
 * @brief Parse a suffixed decimal or hexadecimal uint32 value token.
 * @param token Value token view.
 * @param value Destination numeric value.
 * @return OK or INVALID_DATA.
 */
static uart_comm_result_t uart_comm_parse_value(const uart_comm_token_t *token,
                                                uint32 *value)
{
    uint32 position;
    uint32 result = 0U;
    uint32 digit_count;
    int digit;
    unsigned char suffix;

    if (token->length < 2U) {
        return UART_COMM_RESULT_INVALID_DATA;
    }
    suffix = (unsigned char)token->text[token->length - 1U];
    digit_count = token->length - 1U;
    if (suffix != (unsigned char)'d' && suffix != (unsigned char)'h') {
        return UART_COMM_RESULT_INVALID_DATA;
    }
    if (suffix == (unsigned char)'h' && digit_count > 8U) {
        return UART_COMM_RESULT_INVALID_DATA;
    }

    for (position = 0U; position < digit_count; ++position) {
        if (suffix == (unsigned char)'d') {
            unsigned char character = (unsigned char)token->text[position];
            if (character < (unsigned char)'0' ||
                character > (unsigned char)'9') {
                return UART_COMM_RESULT_INVALID_DATA;
            }
            digit = (int)(character - (unsigned char)'0');
            if (result > 429496729U ||
                (result == 429496729U && digit > 5)) {
                return UART_COMM_RESULT_INVALID_DATA;
            }
        } else {
            digit = uart_comm_hex_digit((unsigned char)token->text[position]);
            if (digit < 0) {
                return UART_COMM_RESULT_INVALID_DATA;
            }
            /* Eight hex digits have already been bounded above. */
        }
        if (suffix == (unsigned char)'d') {
            result = result * 10U + (uint32)digit;
        } else {
            result = (result << 4U) | (uint32)digit;
        }
    }
    *value = result;
    return UART_COMM_RESULT_OK;
}

/**
 * @brief Test whether a byte is retained in a T-command payload.
 * @param character Byte to classify.
 * @return Nonzero for printable ASCII, HT, VT, or FF.
 */
static unsigned char uart_comm_is_transfer_character(unsigned char character)
{
    return (unsigned char)((character >= 0x20U && character <= 0x7EU) ||
                           character == 0x09U || character == 0x0BU ||
                           character == 0x0CU);
}

/**
 * @brief Parse and execute an S register-write command.
 * @param address_token Parsed address token.
 * @param value_token Parsed value token.
 * @return Command or register-access result.
 */
static uart_comm_result_t uart_comm_process_set(const uart_comm_token_t *address_token,
                                                const uart_comm_token_t *value_token)
{
    uint32 address;
    uint32 value;
    uart_comm_result_t result;

    result = uart_comm_parse_address(address_token, &address);
    if (result != UART_COMM_RESULT_OK) {
        return uart_comm_fail(result);
    }
    result = uart_comm_parse_value(value_token, &value);
    if (result != UART_COMM_RESULT_OK) {
        return uart_comm_fail(result);
    }
    if (g_config.register_write == 0 || g_config.report_set == 0) {
        return uart_comm_fail(UART_COMM_RESULT_CONFIGURATION_ERROR);
    }
    result = g_config.register_write(g_config.context, address, value);
    if (result != UART_COMM_RESULT_OK) {
        return uart_comm_fail(UART_COMM_RESULT_REGISTER_ACCESS_ERROR);
    }
    g_config.report_set(g_config.context, address, value);
    return UART_COMM_RESULT_OK;
}

/**
 * @brief Parse and execute an L register-read command.
 * @param address_token Parsed address token; any ignored L argument is omitted.
 * @return Command or register-access result.
 */
static uart_comm_result_t uart_comm_process_load(const uart_comm_token_t *address_token)
{
    uint32 address;
    uint32 value = 0U;
    uart_comm_result_t result;

    result = uart_comm_parse_address(address_token, &address);
    if (result != UART_COMM_RESULT_OK) {
        return uart_comm_fail(result);
    }
    if (g_config.register_read == 0 || g_config.report_read == 0) {
        return uart_comm_fail(UART_COMM_RESULT_CONFIGURATION_ERROR);
    }
    result = g_config.register_read(g_config.context, address, &value);
    if (result != UART_COMM_RESULT_OK) {
        return uart_comm_fail(UART_COMM_RESULT_REGISTER_ACCESS_ERROR);
    }
    g_config.report_read(g_config.context, address, value);
    return UART_COMM_RESULT_OK;
}

/**
 * @brief Filter, forward, and echo a T-command payload.
 * @param line Complete command line.
 * @param start First payload byte after the command separator.
 * @param length Number of bytes in @p line.
 * @return Command or external-handler result.
 */
static uart_comm_result_t uart_comm_process_transfer(const char *line,
                                                     uint32 start,
                                                     uint32 length)
{
    static char transfer_buffer[UART_COMM_TRANSFER_BUFFER_SIZE];
    uint32 position;
    uint32 filtered_length = 0U;
    uart_comm_result_t result;

    for (position = start; position < length; ++position) {
        unsigned char character = (unsigned char)line[position];
        if (uart_comm_is_transfer_character(character) != 0U) {
            /* The line grammar guarantees this bound for normal callers. */
            if (filtered_length >= UART_COMM_TRANSFER_BUFFER_SIZE - 2U) {
                return uart_comm_fail(UART_COMM_RESULT_LINE_OVERFLOW);
            }
            transfer_buffer[filtered_length] = (char)character;
            ++filtered_length;
        }
    }
    if (filtered_length == 0U) {
        return uart_comm_fail(UART_COMM_RESULT_MISSING_ARGUMENT);
    }
    if (g_config.external_command == 0 || g_config.report_transfer == 0) {
        return uart_comm_fail(UART_COMM_RESULT_CONFIGURATION_ERROR);
    }

    transfer_buffer[filtered_length] = '\n';
    transfer_buffer[filtered_length + 1U] = '\0';
    result = g_config.external_command(g_config.context,
                                       transfer_buffer,
                                       filtered_length + 1U);
    if (result != UART_COMM_RESULT_OK) {
        return uart_comm_fail(UART_COMM_RESULT_EXTERNAL_HANDLER_ERROR);
    }
    /* Echo excludes the LF because the output wrapper appends it. */
    transfer_buffer[filtered_length] = '\0';
    g_config.report_transfer(g_config.context,
                             transfer_buffer,
                             filtered_length);
    return UART_COMM_RESULT_OK;
}

/**
 * @brief Parse and execute one complete command line.
 * @param line Input bytes without LF/CRLF terminators.
 * @param length Number of input bytes; no NUL terminator is required.
 * @return The command result, including parse and callback failures.
 *
 * This function runs in normal context and may invoke output and hardware
 * adapter callbacks.
 */
uart_comm_result_t uart_comm_process_line(const char *line, uint32 length)
{
    uint32 position;
    uart_comm_token_t first;
    uart_comm_token_t second;
    uart_comm_token_t third;

    if (uart_comm_has_base_configuration() == 0U) {
        uart_comm_report_error(UART_COMM_RESULT_CONFIGURATION_ERROR);
        return UART_COMM_RESULT_CONFIGURATION_ERROR;
    }
    if (line == 0) {
        return uart_comm_fail(UART_COMM_RESULT_INVALID_SYNTAX);
    }
    if (length > UART_COMM_MAX_LINE_LENGTH) {
        return uart_comm_fail(UART_COMM_RESULT_LINE_OVERFLOW);
    }
    for (position = 0U; position < length; ++position) {
        if ((unsigned char)line[position] == 0U ||
            (unsigned char)line[position] >= 0x80U) {
            return uart_comm_fail(UART_COMM_RESULT_INVALID_CHARACTER);
        }
    }
    if (length == 0U) {
        return UART_COMM_RESULT_EMPTY_LINE;
    }
    if (uart_comm_is_separator((unsigned char)line[0]) != 0U) {
        return uart_comm_fail(UART_COMM_RESULT_INVALID_SYNTAX);
    }
    if (line[0] != 'S' && line[0] != 'L' && line[0] != 'T') {
        return uart_comm_fail(UART_COMM_RESULT_INVALID_COMMAND);
    }
    if (length == 1U) {
        return uart_comm_fail(UART_COMM_RESULT_MISSING_ARGUMENT);
    }
    if (uart_comm_is_separator((unsigned char)line[1]) == 0U) {
        return uart_comm_fail(UART_COMM_RESULT_INVALID_SYNTAX);
    }

    position = 1U;
    if (line[0] == 'T') {
        while (position < length &&
               uart_comm_is_separator((unsigned char)line[position]) != 0U) {
            ++position;
        }
        if (position >= length) {
            return uart_comm_fail(UART_COMM_RESULT_MISSING_ARGUMENT);
        }
        return uart_comm_process_transfer(line, position, length);
    }

    if (uart_comm_next_token(line, length, &position, &first) == 0U) {
        return uart_comm_fail(UART_COMM_RESULT_MISSING_ARGUMENT);
    }
    if (line[0] == 'S') {
        if (uart_comm_next_token(line, length, &position, &second) == 0U) {
            return uart_comm_fail(UART_COMM_RESULT_MISSING_ARGUMENT);
        }
        if (uart_comm_next_token(line, length, &position, &third) != 0U) {
            return uart_comm_fail(UART_COMM_RESULT_TOO_MANY_ARGUMENTS);
        }
        return uart_comm_process_set(&first, &second);
    }

    /* L accepts one ignored argument, but no third token. */
    if (uart_comm_next_token(line, length, &position, &second) != 0U &&
        uart_comm_next_token(line, length, &position, &third) != 0U) {
        return uart_comm_fail(UART_COMM_RESULT_TOO_MANY_ARGUMENTS);
    }
    return uart_comm_process_load(&first);
}

/**
 * @brief Dequeue and process at most one completed line or overflow report.
 * @return Result for the processed item, or NO_LINE_READY when idle.
 *
 * Must run in normal context. Critical-section callbacks protect only the
 * short FIFO ownership transitions; parsing and output occur outside them.
 */
uart_comm_result_t uart_comm_process_received_line(void)
{
    uint32 buffer_index;
    uint32 length;
    unsigned char line_overflow;
    unsigned char line_invalid_character;
    uart_comm_result_t result;

    if (g_initialized == 0U) {
        uart_comm_report_error(UART_COMM_RESULT_CONFIGURATION_ERROR);
        return UART_COMM_RESULT_CONFIGURATION_ERROR;
    }
    if (g_fifo_count == 0U && g_queue_overflow_pending == 0U) {
        return UART_COMM_RESULT_NO_LINE_READY;
    }
    if (g_config.report_error == 0 || g_config.enter_critical == 0 ||
        g_config.exit_critical == 0) {
        uart_comm_report_error(UART_COMM_RESULT_CONFIGURATION_ERROR);
        return UART_COMM_RESULT_CONFIGURATION_ERROR;
    }

    g_config.enter_critical(g_config.context);
    if (g_fifo_count != 0U) {
        buffer_index = g_fifo[g_fifo_head];
        g_fifo_head = uart_comm_next_index(g_fifo_head);
        --g_fifo_count;
        g_buffer_states[buffer_index] = UART_COMM_BUFFER_PROCESSING;
        length = g_buffer_lengths[buffer_index];
        line_overflow = g_buffer_overflow[buffer_index];
        line_invalid_character = g_buffer_invalid_character[buffer_index];
        g_config.exit_critical(g_config.context);

        if (line_invalid_character != 0U) {
            result = uart_comm_fail(UART_COMM_RESULT_INVALID_CHARACTER);
        } else if (line_overflow != 0U) {
            result = uart_comm_fail(UART_COMM_RESULT_LINE_OVERFLOW);
        } else {
            result = uart_comm_process_line(g_rx_buffers[buffer_index], length);
        }

        g_config.enter_critical(g_config.context);
        g_buffer_states[buffer_index] = UART_COMM_BUFFER_FREE;
        g_config.exit_critical(g_config.context);
        return result;
    }

    --g_queue_overflow_pending;
    g_config.exit_critical(g_config.context);
    uart_comm_report_error(UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW);
    return UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW;
}
