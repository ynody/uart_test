#include "uart_comm.h"

#include <stdio.h>
#include <string.h>

/** @brief Mutable test-double state and captured callback observations. */
typedef struct {
    uint32 write_calls;
    uint32 read_calls;
    uint32 external_calls;
    uint32 report_set_calls;
    uint32 report_read_calls;
    uint32 report_transfer_calls;
    uint32 report_error_calls;
    uint32 last_write_address;
    uint32 last_write_value;
    uint32 last_read_address;
    uint32 last_report_address;
    uint32 last_report_value;
    uint32 external_length;
    char external_text[UART_COMM_TRANSFER_BUFFER_SIZE];
    char output[1024];
    uint32 output_length;
    uart_comm_result_t last_error;
    uart_comm_result_t write_result;
    uart_comm_result_t read_result;
    uart_comm_result_t external_result;
    uint32 read_value;
} test_context_t;

/** @brief Append bounded output-wrapper text to the test log. */
static void test_output_append(test_context_t *context,
                               const char *text,
                               uint32 length)
{
    uint32 copy_length = length;

    if (copy_length > 1023U - context->output_length) {
        copy_length = 1023U - context->output_length;
    }
    if (copy_length != 0U) {
        memcpy(&context->output[context->output_length], text, copy_length);
        context->output_length += copy_length;
    }
    context->output[context->output_length] = '\0';
}

/** @brief Record a register-write callback invocation. */
static uart_comm_result_t test_register_write(void *opaque,
                                               uint32 address,
                                               uint32 value)
{
    test_context_t *context = (test_context_t *)opaque;
    ++context->write_calls;
    context->last_write_address = address;
    context->last_write_value = value;
    return context->write_result;
}

/** @brief Record a register-read callback invocation and return the test value. */
static uart_comm_result_t test_register_read(void *opaque,
                                              uint32 address,
                                              uint32 *value)
{
    test_context_t *context = (test_context_t *)opaque;
    ++context->read_calls;
    context->last_read_address = address;
    *value = context->read_value;
    return context->read_result;
}

/** @brief Record and copy an external T-command callback invocation. */
static uart_comm_result_t test_external_command(void *opaque,
                                                const char *text,
                                                uint32 length)
{
    test_context_t *context = (test_context_t *)opaque;
    ++context->external_calls;
    context->external_length = length;
    if (length >= UART_COMM_TRANSFER_BUFFER_SIZE) {
        length = UART_COMM_TRANSFER_BUFFER_SIZE - 1U;
    }
    memcpy(context->external_text, text, length);
    context->external_text[length] = '\0';
    return context->external_result;
}

/** @brief Format and record a successful S-command response. */
static void test_report_set(void *opaque, uint32 address, uint32 value)
{
    test_context_t *context = (test_context_t *)opaque;
    char message[64];
    int length;

    ++context->report_set_calls;
    context->last_report_address = address;
    context->last_report_value = value;
    length = snprintf(message, sizeof(message),
                      "=> S 0x%08x %xh\n", address, value);
    if (length > 0) {
        test_output_append(context, message, (uint32)length);
    }
}

/** @brief Format and record a successful L-command response. */
static void test_report_read(void *opaque, uint32 address, uint32 value)
{
    test_context_t *context = (test_context_t *)opaque;
    char message[64];
    int length;

    ++context->report_read_calls;
    context->last_report_address = address;
    context->last_report_value = value;
    length = snprintf(message, sizeof(message),
                      "=> L 0x%08x %xh\n", address, value);
    if (length > 0) {
        test_output_append(context, message, (uint32)length);
    }
}

/** @brief Format and record a successful T-command response. */
static void test_report_transfer(void *opaque,
                                 const char *text,
                                 uint32 length)
{
    test_context_t *context = (test_context_t *)opaque;
    static const char prefix[] = "=> T ";

    ++context->report_transfer_calls;
    test_output_append(context, prefix, (uint32)(sizeof(prefix) - 1U));
    test_output_append(context, text, length);
    test_output_append(context, "\n", 1U);
}

/** @brief Record an error response and append a diagnostic test message. */
static void test_report_error(void *opaque, uart_comm_result_t result)
{
    test_context_t *context = (test_context_t *)opaque;
    char message[32];
    int length;

    ++context->report_error_calls;
    context->last_error = result;
    length = snprintf(message, sizeof(message), "ERROR %u\n", (unsigned int)result);
    if (length > 0) {
        test_output_append(context, message, (uint32)length);
    }
}

/** @brief No-op critical-section entry callback used by PC tests. */
static void test_enter_critical(void *opaque)
{
    (void)opaque;
}

/** @brief No-op critical-section exit callback used by PC tests. */
static void test_exit_critical(void *opaque)
{
    (void)opaque;
}

/** @brief Build a complete callback configuration for one test context. */
static uart_comm_config_t test_config(test_context_t *context)
{
    uart_comm_config_t config;

    config.register_read = test_register_read;
    config.register_write = test_register_write;
    config.external_command = test_external_command;
    config.report_set = test_report_set;
    config.report_read = test_report_read;
    config.report_transfer = test_report_transfer;
    config.report_error = test_report_error;
    config.enter_critical = test_enter_critical;
    config.exit_critical = test_exit_critical;
    config.context = context;
    return config;
}

/** @brief Reset a test context and initialize the core with test callbacks. */
static void test_init(test_context_t *context)
{
    uart_comm_config_t config;

    memset(context, 0, sizeof(*context));
    context->write_result = UART_COMM_RESULT_OK;
    context->read_result = UART_COMM_RESULT_OK;
    context->external_result = UART_COMM_RESULT_OK;
    context->read_value = 0x2AU;
    config = test_config(context);
    uart_comm_init(&config);
}

/** @brief Feed one NUL-terminated command line using LF or CRLF. */
static void test_feed_line(const char *text, unsigned char crlf)
{
    uint32 index;
    uint32 length = (uint32)strlen(text);

    for (index = 0U; index < length; ++index) {
        uart_comm_receive_char(text[index]);
    }
    if (crlf != 0U) {
        uart_comm_receive_char('\r');
    }
    uart_comm_receive_char('\n');
}

/** @brief Feed an explicitly sized byte sequence, including embedded NULs. */
static void test_feed_bytes(const unsigned char *text, uint32 length)
{
    uint32 index;

    for (index = 0U; index < length; ++index) {
        uart_comm_receive_char((char)text[index]);
    }
}

/** @brief Fail the current test immediately when a condition is false. */
#define TEST_CHECK(condition) \
    do { \
        if (!(condition)) { \
            printf("  check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 0; \
        } \
    } while (0)

/** @brief Verify initialization and receive/process separation. */
static int test_initialization_and_separation(void)
{
    test_context_t context;

    test_init(&context);
    test_feed_line("S 0000 1d", 0U);
    TEST_CHECK(context.write_calls == 0U);
    TEST_CHECK(context.report_set_calls == 0U);
    TEST_CHECK(uart_comm_has_pending_line() != 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.write_calls == 1U);
    TEST_CHECK(context.last_write_value == 1U);
    return 1;
}

/** @brief Verify LF and CRLF line completion behavior. */
static int test_line_endings(void)
{
    test_context_t context;

    test_init(&context);
    test_feed_line("S 0000 1d", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.write_calls == 1U);

    test_init(&context);
    test_feed_line("S 0000 2d", 1U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.write_calls == 1U);
    TEST_CHECK(context.last_write_value == 2U);
    return 1;
}

/** @brief Verify empty-line handling and S response formatting. */
static int test_empty_and_set_output(void)
{
    test_context_t context;

    test_init(&context);
    test_feed_line("", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_EMPTY_LINE);
    TEST_CHECK(context.report_error_calls == 0U);

    test_init(&context);
    test_feed_line("S 0000 4d", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(strcmp(context.output, "=> S 0x00000000 4h\n") == 0);

    test_init(&context);
    test_feed_line("S 0000 4294967295d", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.last_write_value == 0xFFFFFFFFU);
    return 1;
}

/** @brief Verify uint32 hexadecimal boundaries and L processing. */
static int test_hex_boundary_and_load(void)
{
    test_context_t context;

    test_init(&context);
    test_feed_line("S FFFFFFFF FFFFFFFFh", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.last_write_address == 0xFFFFFFFFU);
    TEST_CHECK(context.last_write_value == 0xFFFFFFFFU);
    TEST_CHECK(strcmp(context.output, "=> S 0xffffffff ffffffffh\n") == 0);

    test_init(&context);
    test_feed_line("L 0x0004 ignored", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.read_calls == 1U);
    TEST_CHECK(context.last_read_address == 4U);
    TEST_CHECK(strcmp(context.output, "=> L 0x00000004 2ah\n") == 0);
    return 1;
}

/** @brief Verify T forwarding, echoing, and control-byte filtering. */
static int test_transfer(void)
{
    test_context_t context;
    static const unsigned char control_text[] = {
        'T', ' ', 'A', 0x01U, 'B', '\t', 'C'
    };

    test_init(&context);
    test_feed_line("T any string", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.external_calls == 1U);
    TEST_CHECK(context.external_length == 11U);
    TEST_CHECK(strcmp(context.external_text, "any string\n") == 0);
    TEST_CHECK(strcmp(context.output, "=> T any string\n") == 0);

    test_init(&context);
    test_feed_line("T ", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_MISSING_ARGUMENT);
    TEST_CHECK(context.external_calls == 0U);

    test_init(&context);
    test_feed_bytes(control_text, (uint32)sizeof(control_text));
    uart_comm_receive_char('\n');
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(strcmp(context.external_text, "AB\tC\n") == 0);
    TEST_CHECK(strcmp(context.output, "=> T AB\tC\n") == 0);
    return 1;
}

/** @brief Verify repeated-CR semantics and invalid-byte rejection. */
static int test_repeated_cr_and_invalid_bytes(void)
{
    test_context_t context;
    static const unsigned char repeated_cr_set[] = {
        'S', '\r', '\r', '\n'
    };
    static const unsigned char repeated_cr_transfer[] = {
        'T', ' ', 'A', '\r', '\r', '\n'
    };
    static const unsigned char non_ascii_transfer[] = {
        'T', ' ', 'A', 0x80U, 'B', '\n'
    };

    test_init(&context);
    test_feed_bytes(repeated_cr_set, (uint32)sizeof(repeated_cr_set));
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_INVALID_SYNTAX);
    TEST_CHECK(context.write_calls == 0U);

    test_init(&context);
    test_feed_bytes(repeated_cr_transfer, (uint32)sizeof(repeated_cr_transfer));
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.external_calls == 1U);
    TEST_CHECK(strcmp(context.external_text, "A\n") == 0);

    test_init(&context);
    test_feed_bytes(non_ascii_transfer, (uint32)sizeof(non_ascii_transfer));
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_INVALID_CHARACTER);
    TEST_CHECK(context.external_calls == 0U);

    test_init(&context);
    TEST_CHECK(uart_comm_process_line((const char *)non_ascii_transfer, 5U) ==
               UART_COMM_RESULT_INVALID_CHARACTER);
    TEST_CHECK(context.external_calls == 0U);
    return 1;
}

/** @brief Verify NUL invalidates a line and the receiver recovers. */
static int test_nul_stops_line_storage(void)
{
    test_context_t context;
    static const unsigned char nul_then_command[] = {
        'T', ' ', 'A', 0x00U, 'B', ' ', 'C', '\n'
    };

    test_init(&context);
    test_feed_bytes(nul_then_command, (uint32)sizeof(nul_then_command));
    TEST_CHECK(uart_comm_process_received_line() ==
               UART_COMM_RESULT_INVALID_CHARACTER);
    TEST_CHECK(context.external_calls == 0U);

    /* The receiver must be usable immediately after the invalid line. */
    test_feed_line("S 0000 1d", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.write_calls == 1U);
    return 1;
}

/** @brief Verify standalone S, L, and T missing-argument results. */
static int test_single_command_missing_arguments(void)
{
    test_context_t context;
    static const char *commands[] = {"S", "L", "T"};
    uint32 index;

    for (index = 0U; index < 3U; ++index) {
        test_init(&context);
        test_feed_line(commands[index], 0U);
        TEST_CHECK(uart_comm_process_received_line() ==
                   UART_COMM_RESULT_MISSING_ARGUMENT);
        TEST_CHECK(context.write_calls == 0U);
        TEST_CHECK(context.read_calls == 0U);
        TEST_CHECK(context.external_calls == 0U);
    }
    return 1;
}

/** @brief Verify invalid command, syntax, and numeric-input results. */
static int test_invalid_commands_and_values(void)
{
    test_context_t context;
    static const char *invalid_lines[] = {
        "S 0000 1",
        "S 0000 -1d",
        "S 0000 FFFFFFFFFh",
        "S 0000 4294967296d",
        "L 0004 x y"
    };
    uint32 index;

    test_init(&context);
    test_feed_line("X test", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_INVALID_COMMAND);
    TEST_CHECK(context.write_calls == 0U && context.read_calls == 0U);

    for (index = 0U; index < (uint32)(sizeof(invalid_lines) / sizeof(invalid_lines[0])); ++index) {
        test_init(&context);
        test_feed_line(invalid_lines[index], 0U);
        TEST_CHECK(uart_comm_process_received_line() ==
                   (index == 4U ? UART_COMM_RESULT_TOO_MANY_ARGUMENTS :
                                  UART_COMM_RESULT_INVALID_DATA));
        TEST_CHECK(context.write_calls == 0U && context.read_calls == 0U);
    }
    return 1;
}

/** @brief Verify line overflow, NUL handling, and post-error recovery. */
static int test_nul_and_overflow_recovery(void)
{
    test_context_t context;
    static const unsigned char nul_line[] = {'S', ' ', '0', 0x00U, ' ', '1', 'd'};
    char long_line[258];
    uint32 index;

    test_init(&context);
    test_feed_bytes(nul_line, (uint32)sizeof(nul_line));
    uart_comm_receive_char('\n');
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_INVALID_CHARACTER);
    TEST_CHECK(context.write_calls == 0U);

    test_init(&context);
    long_line[0] = 'X';
    for (index = 1U; index < 257U; ++index) {
        long_line[index] = 'a';
    }
    test_feed_bytes((const unsigned char *)long_line, 257U);
    uart_comm_receive_char('\n');
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_LINE_OVERFLOW);

    test_feed_line("S 0000 3d", 0U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.last_write_value == 3U);
    return 1;
}

/** @brief Format and feed a generated S command for FIFO tests. */
static void test_feed_set_line(uint32 address, uint32 value)
{
    char line[40];
    int length;

    length = snprintf(line, sizeof(line), "S %04x %ud", address, value);
    if (length > 0) {
        test_feed_line(line, 0U);
    }
}

/** @brief Verify configurable FIFO order and queue-overflow recovery. */
static int test_fifo_and_queue_overflow(void)
{
    test_context_t context;
    uint32 index;
    uint32 expected_address;

    /* Fill every configured buffer, free one, and accept the next line. */
    test_init(&context);
    for (index = 0U; index < UART_COMM_RX_BUFFER_COUNT; ++index) {
        test_feed_set_line(index * 4U, index + 1U);
    }
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.last_write_address == 0U);
    test_feed_set_line(UART_COMM_RX_BUFFER_COUNT * 4U,
                       UART_COMM_RX_BUFFER_COUNT + 1U);
    for (index = 1U; index < UART_COMM_RX_BUFFER_COUNT; ++index) {
        expected_address = index * 4U;
        TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
        TEST_CHECK(context.last_write_address == expected_address);
    }
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.last_write_address == UART_COMM_RX_BUFFER_COUNT * 4U);
    TEST_CHECK(context.write_calls == UART_COMM_RX_BUFFER_COUNT + 1U);
    TEST_CHECK(context.report_error_calls == 0U);

    /* A discarded full-queue line must not cause the following line to be lost. */
    test_init(&context);
    for (index = 0U; index < UART_COMM_RX_BUFFER_COUNT; ++index) {
        test_feed_set_line(index * 4U, index + 1U);
    }
    test_feed_set_line(UART_COMM_RX_BUFFER_COUNT * 4U,
                       UART_COMM_RX_BUFFER_COUNT + 1U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.last_write_address == 0U);
    test_feed_set_line((UART_COMM_RX_BUFFER_COUNT + 1U) * 4U,
                       UART_COMM_RX_BUFFER_COUNT + 2U);
    for (index = 1U; index < UART_COMM_RX_BUFFER_COUNT; ++index) {
        expected_address = index * 4U;
        TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
        TEST_CHECK(context.last_write_address == expected_address);
    }
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    TEST_CHECK(context.last_write_address ==
               (UART_COMM_RX_BUFFER_COUNT + 1U) * 4U);
    TEST_CHECK(uart_comm_process_received_line() ==
               UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW);
    TEST_CHECK(context.write_calls == UART_COMM_RX_BUFFER_COUNT + 1U);
    TEST_CHECK(context.report_error_calls == 1U);
    return 1;
}

/** @brief Verify deferred and counted queue-overflow reporting. */
static int test_queue_overflow_pending_timing(void)
{
    test_context_t context;
    uint32 index;

    test_init(&context);
    for (index = 0U; index < UART_COMM_RX_BUFFER_COUNT; ++index) {
        test_feed_set_line(index * 4U, index + 1U);
    }
    /* Enter discard mode, but do not report until the discarded line ends. */
    uart_comm_receive_char('X');
    TEST_CHECK(context.report_error_calls == 0U);
    uart_comm_receive_char('\n');
    for (index = 0U; index < UART_COMM_RX_BUFFER_COUNT; ++index) {
        TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    }
    TEST_CHECK(uart_comm_process_received_line() ==
               UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW);
    TEST_CHECK(context.report_error_calls == 1U);

    /* Consecutive discarded lines produce one pending report each. */
    test_init(&context);
    for (index = 0U; index < UART_COMM_RX_BUFFER_COUNT; ++index) {
        test_feed_set_line(index * 4U, index + 1U);
    }
    uart_comm_receive_char('X');
    uart_comm_receive_char('\n');
    uart_comm_receive_char('Y');
    uart_comm_receive_char('\n');
    for (index = 0U; index < UART_COMM_RX_BUFFER_COUNT; ++index) {
        TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_OK);
    }
    TEST_CHECK(uart_comm_process_received_line() ==
               UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW);
    TEST_CHECK(uart_comm_process_received_line() ==
               UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW);
    TEST_CHECK(context.report_error_calls == 2U);
    TEST_CHECK(uart_comm_process_received_line() == UART_COMM_RESULT_NO_LINE_READY);
    return 1;
}

/** @brief Verify callback failures and missing-callback configuration errors. */
static int test_callback_failures_and_configuration(void)
{
    test_context_t context;
    uart_comm_config_t config;

    test_init(&context);
    context.write_result = UART_COMM_RESULT_INVALID_DATA;
    test_feed_line("S 0000 1d", 0U);
    TEST_CHECK(uart_comm_process_received_line() ==
               UART_COMM_RESULT_REGISTER_ACCESS_ERROR);
    TEST_CHECK(context.report_set_calls == 0U);

    test_init(&context);
    context.external_result = UART_COMM_RESULT_INVALID_SYNTAX;
    test_feed_line("T command", 0U);
    TEST_CHECK(uart_comm_process_received_line() ==
               UART_COMM_RESULT_EXTERNAL_HANDLER_ERROR);
    TEST_CHECK(context.report_transfer_calls == 0U);

    test_init(&context);
    config = test_config(&context);
    config.register_write = 0;
    uart_comm_init(&config);
    test_feed_line("S 0000 1d", 0U);
    TEST_CHECK(uart_comm_process_received_line() ==
               UART_COMM_RESULT_CONFIGURATION_ERROR);
    return 1;
}

/** @brief Run one named test and print its pass/fail status. */
static int run_test(const char *name, int (*test_function)(void))
{
    int passed = test_function();
    printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    return passed;
}

/** @brief Execute the complete PC-side UART communication test suite. */
int main(void)
{
    int passed = 0;
    int total = 0;

    passed += run_test("initialization and separation", test_initialization_and_separation);
    ++total;
    passed += run_test("LF and CRLF", test_line_endings);
    ++total;
    passed += run_test("empty line and set output", test_empty_and_set_output);
    ++total;
    passed += run_test("hex boundary and load", test_hex_boundary_and_load);
    ++total;
    passed += run_test("transfer and control filtering", test_transfer);
    ++total;
    passed += run_test("repeated CR and invalid bytes", test_repeated_cr_and_invalid_bytes);
    ++total;
    passed += run_test("NUL stops line storage", test_nul_stops_line_storage);
    ++total;
    passed += run_test("single command missing arguments", test_single_command_missing_arguments);
    ++total;
    passed += run_test("invalid commands and values", test_invalid_commands_and_values);
    ++total;
    passed += run_test("NUL and overflow recovery", test_nul_and_overflow_recovery);
    ++total;
    passed += run_test("FIFO and queue overflow", test_fifo_and_queue_overflow);
    ++total;
    passed += run_test("queue overflow pending timing", test_queue_overflow_pending_timing);
    ++total;
    passed += run_test("callback failures and configuration", test_callback_failures_and_configuration);
    ++total;

    printf("%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
