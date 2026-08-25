#ifndef UART_COMM_H
#define UART_COMM_H

/**
 * @brief Unsigned 32-bit integer type used by the platform-independent core.
 *
 * The target and the PC test environment are required to provide a
 * 32-bit unsigned int.  The core deliberately does not include stdint.h.
 */
typedef unsigned int uint32;

/** @brief Maximum number of input characters in one command line. */
#define UART_COMM_MAX_LINE_LENGTH  (256U)
/** @brief Size of one command-line buffer including its NUL terminator. */
#define UART_COMM_LINE_BUFFER_SIZE (UART_COMM_MAX_LINE_LENGTH + 1U)
#ifndef UART_COMM_RX_BUFFER_COUNT
/** @brief Default number of statically allocated receive FIFO buffers. */
#define UART_COMM_RX_BUFFER_COUNT  (2U)
#endif

#if UART_COMM_RX_BUFFER_COUNT < 2U
#error "UART_COMM_RX_BUFFER_COUNT must be at least 2"
#endif

/** @brief Size of the T-command transfer buffer including LF and NUL. */
#define UART_COMM_TRANSFER_BUFFER_SIZE (UART_COMM_MAX_LINE_LENGTH)

/** @brief Result codes returned by command processing and callbacks. */
typedef enum {
    UART_COMM_RESULT_OK = 0,
    UART_COMM_RESULT_NO_LINE_READY,
    UART_COMM_RESULT_EMPTY_LINE,
    UART_COMM_RESULT_LINE_OVERFLOW,
    UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW,
    UART_COMM_RESULT_INVALID_CHARACTER,
    UART_COMM_RESULT_INVALID_COMMAND,
    UART_COMM_RESULT_INVALID_SYNTAX,
    UART_COMM_RESULT_MISSING_ARGUMENT,
    UART_COMM_RESULT_TOO_MANY_ARGUMENTS,
    UART_COMM_RESULT_INVALID_ADDRESS,
    UART_COMM_RESULT_INVALID_DATA,
    UART_COMM_RESULT_CONFIGURATION_ERROR,
    UART_COMM_RESULT_REGISTER_ACCESS_ERROR,
    UART_COMM_RESULT_EXTERNAL_HANDLER_ERROR
} uart_comm_result_t;

/** @brief State returned by the lightweight character receive entry point. */
typedef enum {
    UART_COMM_RECEIVE_IN_PROGRESS = 0,
    UART_COMM_RECEIVE_LINE_READY
} uart_comm_receive_state_t;

/** @brief Ownership state of one statically allocated receive buffer. */
typedef enum {
    UART_COMM_BUFFER_FREE = 0,
    UART_COMM_BUFFER_RECEIVING,
    UART_COMM_BUFFER_READY,
    UART_COMM_BUFFER_PROCESSING
} uart_comm_buffer_state_t;

/**
 * @brief Read a 32-bit register through the platform adapter.
 * @param context Opaque caller-supplied context.
 * @param address Register address supplied by the command.
 * @param value Destination for the read value.
 * @return UART_COMM_RESULT_OK on success, or a callback-specific failure.
 */
typedef uart_comm_result_t (*uart_comm_register_read_fn)(
    void *context,
    uint32 address,
    uint32 *value);

/**
 * @brief Write a 32-bit register through the platform adapter.
 * @param context Opaque caller-supplied context.
 * @param address Register address supplied by the command.
 * @param value Value to write.
 * @return UART_COMM_RESULT_OK on success, or a callback-specific failure.
 */
typedef uart_comm_result_t (*uart_comm_register_write_fn)(
    void *context,
    uint32 address,
    uint32 value);

/**
 * @brief Forward a filtered T-command string to an external handler.
 * @param context Opaque caller-supplied context.
 * @param text NUL-terminated text with a trailing LF; valid during the call.
 * @param length Text length including LF and excluding the NUL terminator.
 * @return UART_COMM_RESULT_OK on success, or a callback-specific failure.
 */
typedef uart_comm_result_t (*uart_comm_external_command_fn)(
    void *context,
    const char *text,
    uint32 length);

/** @brief Output wrapper for a successful register write. */
typedef void (*uart_comm_report_set_fn)(
    void *context,
    uint32 address,
    uint32 value);

/** @brief Output wrapper for a successful register read. */
typedef void (*uart_comm_report_read_fn)(
    void *context,
    uint32 address,
    uint32 value);

/**
 * @brief Output wrapper for a successful T-command transfer.
 * @param context Opaque caller-supplied context.
 * @param text Filtered transfer text without the trailing LF.
 * @param length Text length excluding the trailing LF.
 */
typedef void (*uart_comm_report_transfer_fn)(
    void *context,
    const char *text,
    uint32 length);

/**
 * @brief Output wrapper for an error result.
 * @param context Opaque caller-supplied context.
 * @param result Result to render and transmit.
 */
typedef void (*uart_comm_report_error_fn)(
    void *context,
    uart_comm_result_t result);

/**
 * @brief Enter or leave the short receive-buffer critical section.
 *
 * The platform adapter may disable and restore the UART receive interrupt.
 * @param context Opaque caller-supplied context.
 */
typedef void (*uart_comm_critical_section_fn)(void *context);

/** @brief Callback and platform-adapter configuration for the single instance. */
typedef struct {
    uart_comm_register_read_fn register_read;
    uart_comm_register_write_fn register_write;
    uart_comm_external_command_fn external_command;
    uart_comm_report_set_fn report_set;
    uart_comm_report_read_fn report_read;
    uart_comm_report_transfer_fn report_transfer;
    uart_comm_report_error_fn report_error;
    uart_comm_critical_section_fn enter_critical;
    uart_comm_critical_section_fn exit_critical;
    void *context;
} uart_comm_config_t;

/**
 * @brief Initialize the single global module instance and receive FIFO.
 * @param config Callback configuration copied by the core; may be NULL to
 *               initialize an intentionally unconfigured instance.
 */
void uart_comm_init(const uart_comm_config_t *config);

/**
 * @brief Store one received character and detect line completion.
 *
 * This function is intended for a UART ISR. It performs no parsing, output,
 * register access, or external callback invocation.
 * @param character One received byte.
 * @return UART_COMM_RECEIVE_LINE_READY when an input line was queued;
 *         otherwise UART_COMM_RECEIVE_IN_PROGRESS.
 */
uart_comm_receive_state_t uart_comm_receive_char(char character);

/**
 * @brief Check whether a line or deferred queue-overflow report is pending.
 * @return Nonzero when normal-context processing should be performed.
 */
unsigned char uart_comm_has_pending_line(void);

/**
 * @brief Process one queued line or one deferred queue-overflow report.
 *
 * Call this function from the main loop or another normal execution context,
 * never directly from the UART receive ISR.
 * @return The processed command or deferred-report result.
 */
uart_comm_result_t uart_comm_process_received_line(void);

/**
 * @brief Parse and process one complete line without line-ending bytes.
 * @param line Input bytes to parse; the supplied length is authoritative.
 * @param length Number of bytes in @p line.
 * @return Command processing result.
 */
uart_comm_result_t uart_comm_process_line(const char *line, uint32 length);

#endif /* UART_COMM_H */
