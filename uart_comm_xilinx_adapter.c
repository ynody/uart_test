#include "uart_comm_xilinx_adapter.h"

#include "xil_io.h"
#include "xil_printf.h"

/**
 * @brief Return the fixed Xilinx-console message for one core error result.
 *
 * @param result Core result that requires error reporting.
 *
 * @note Called only from normal context through @c uart_comm_process_received_line().
 */
static void uart_comm_xilinx_report_error_message(uart_comm_result_t result)
{
    switch (result) {
    case UART_COMM_RESULT_LINE_OVERFLOW:
        xil_printf("ERROR: command line is too long\n");
        break;
    case UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW:
        xil_printf("ERROR: receive queue overflow\n");
        break;
    case UART_COMM_RESULT_INVALID_CHARACTER:
        xil_printf("ERROR: invalid input character\n");
        break;
    case UART_COMM_RESULT_INVALID_COMMAND:
        xil_printf("ERROR: unknown command\n");
        break;
    case UART_COMM_RESULT_INVALID_SYNTAX:
        xil_printf("ERROR: invalid command syntax\n");
        break;
    case UART_COMM_RESULT_MISSING_ARGUMENT:
        xil_printf("ERROR: missing command argument\n");
        break;
    case UART_COMM_RESULT_TOO_MANY_ARGUMENTS:
        xil_printf("ERROR: too many command arguments\n");
        break;
    case UART_COMM_RESULT_INVALID_ADDRESS:
        xil_printf("ERROR: invalid register address\n");
        break;
    case UART_COMM_RESULT_INVALID_DATA:
        xil_printf("ERROR: invalid register value\n");
        break;
    case UART_COMM_RESULT_CONFIGURATION_ERROR:
        xil_printf("ERROR: UART command configuration\n");
        break;
    case UART_COMM_RESULT_REGISTER_ACCESS_ERROR:
        xil_printf("ERROR: register access failed\n");
        break;
    case UART_COMM_RESULT_EXTERNAL_HANDLER_ERROR:
        xil_printf("ERROR: external command failed\n");
        break;
    default:
        xil_printf("ERROR: UART command processing failed\n");
        break;
    }
}

/**
 * @brief Read one 32-bit memory-mapped register through the Xilinx I/O API.
 *
 * @param context Unused adapter context.
 * @param address Register address supplied by the parsed command.
 * @param value Destination for the register value.
 * @return @c UART_COMM_RESULT_OK on success.
 *
 * @note The core has already validated the textual address.  This wrapper
 * performs no address-range or alignment validation by design.
 */
static uart_comm_result_t uart_comm_xilinx_register_read(
    void *context,
    uint32 address,
    uint32 *value)
{
    (void)context;

    if (value == 0) {
        return UART_COMM_RESULT_REGISTER_ACCESS_ERROR;
    }

    *value = (uint32)Xil_In32((UINTPTR)address);
    return UART_COMM_RESULT_OK;
}

/**
 * @brief Write one 32-bit value to a memory-mapped register through Xilinx I/O.
 *
 * @param context Unused adapter context.
 * @param address Register address supplied by the parsed command.
 * @param value Register value supplied by the parsed command.
 * @return @c UART_COMM_RESULT_OK on success.
 *
 * @note The core and this wrapper deliberately perform no address-range or
 * alignment validation.
 */
static uart_comm_result_t uart_comm_xilinx_register_write(
    void *context,
    uint32 address,
    uint32 value)
{
    (void)context;

    Xil_Out32((UINTPTR)address, (u32)value);
    return UART_COMM_RESULT_OK;
}

/**
 * @brief Print the fixed-format response for a successful S command.
 *
 * @param context Unused adapter context.
 * @param address Written register address.
 * @param value Written register value.
 *
 * @note This function is called from normal context, never from the UART ISR.
 */
static void uart_comm_xilinx_report_set(void *context,
                                        uint32 address,
                                        uint32 value)
{
    (void)context;
    xil_printf("=> S 0x%08x %xh\n", address, value);
}

/**
 * @brief Print the fixed-format response for a successful L command.
 *
 * @param context Unused adapter context.
 * @param address Read register address.
 * @param value Value read from the register.
 *
 * @note This function is called from normal context, never from the UART ISR.
 */
static void uart_comm_xilinx_report_read(void *context,
                                         uint32 address,
                                         uint32 value)
{
    (void)context;
    xil_printf("=> L 0x%08x %xh\n", address, value);
}

/**
 * @brief Print the echo response for a successful T command.
 *
 * @param context Unused adapter context.
 * @param text NUL-terminated forwarded text without its trailing LF.
 * @param length Number of payload bytes in @p text.
 *
 * @note The core guarantees that @p text remains valid throughout this call.
 */
static void uart_comm_xilinx_report_transfer(void *context,
                                             const char *text,
                                             uint32 length)
{
    (void)context;
    (void)length;

    if (text == 0) {
        xil_printf("=> T \n");
        return;
    }

    xil_printf("=> T %s\n", text);
}

/**
 * @brief Print a reason-specific error response for the core.
 *
 * @param context Unused adapter context.
 * @param result Core result code to describe.
 */
static void uart_comm_xilinx_report_error(void *context,
                                          uart_comm_result_t result)
{
    (void)context;
    uart_comm_xilinx_report_error_message(result);
}

/**
 * @brief Forward a T command to the application-owned handler.
 *
 * @param context Adapter context containing the application callback.
 * @param text NUL-terminated command text with a trailing LF.
 * @param length Number of bytes in @p text excluding its NUL terminator.
 * @return Result returned by the application handler.
 *
 * @note The application must copy @p text before returning if it needs to
 * retain the command after this callback completes.
 */
static uart_comm_result_t uart_comm_xilinx_external_command(
    void *context,
    const char *text,
    uint32 length)
{
    uart_comm_xilinx_adapter_t *adapter =
        (uart_comm_xilinx_adapter_t *)context;

    if (adapter == 0 || adapter->external_command == 0) {
        return UART_COMM_RESULT_CONFIGURATION_ERROR;
    }

    return adapter->external_command(adapter->external_context, text, length);
}

/**
 * @brief Enter the short normal-context FIFO critical section.
 *
 * @param context Adapter context containing the board callback.
 *
 * @note The board callback should disable only the UART receive interrupt.
 * The core never calls this wrapper from the UART ISR.
 */
static void uart_comm_xilinx_enter_critical(void *context)
{
    uart_comm_xilinx_adapter_t *adapter =
        (uart_comm_xilinx_adapter_t *)context;

    if (adapter != 0 && adapter->enter_critical != 0) {
        adapter->enter_critical(adapter->critical_context);
    }
}

/**
 * @brief Exit the short normal-context FIFO critical section.
 *
 * @param context Adapter context containing the board callback.
 *
 * @note The board callback should restore the UART receive interrupt state
 * established by @c uart_comm_xilinx_enter_critical().
 */
static void uart_comm_xilinx_exit_critical(void *context)
{
    uart_comm_xilinx_adapter_t *adapter =
        (uart_comm_xilinx_adapter_t *)context;

    if (adapter != 0 && adapter->exit_critical != 0) {
        adapter->exit_critical(adapter->critical_context);
    }
}

/**
 * @brief Initialize persistent Xilinx adapter state from integration options.
 *
 * @param adapter Persistent adapter storage supplied by the caller.
 * @param options Board and application callbacks used by the adapter.
 */
void uart_comm_xilinx_adapter_init(
    uart_comm_xilinx_adapter_t *adapter,
    const uart_comm_xilinx_adapter_options_t *options)
{
    if (adapter == 0) {
        return;
    }

    adapter->external_command = 0;
    adapter->external_context = 0;
    adapter->enter_critical = 0;
    adapter->exit_critical = 0;
    adapter->critical_context = 0;

    if (options != 0) {
        adapter->external_command = options->external_command;
        adapter->external_context = options->external_context;
        adapter->enter_critical = options->enter_critical;
        adapter->exit_critical = options->exit_critical;
        adapter->critical_context = options->critical_context;
    }
}

/**
 * @brief Populate a core configuration with Xilinx adapter callbacks.
 *
 * @param adapter Initialized persistent adapter state.
 * @param config Destination core configuration.
 *
 * @note When a critical-section callback is absent, the corresponding core
 * callback is left null so that the core reports a configuration error.
 */
void uart_comm_xilinx_adapter_make_config(
    uart_comm_xilinx_adapter_t *adapter,
    uart_comm_config_t *config)
{
    if (config == 0) {
        return;
    }

    config->register_read = uart_comm_xilinx_register_read;
    config->register_write = uart_comm_xilinx_register_write;
    config->external_command = uart_comm_xilinx_external_command;
    config->report_set = uart_comm_xilinx_report_set;
    config->report_read = uart_comm_xilinx_report_read;
    config->report_transfer = uart_comm_xilinx_report_transfer;
    config->report_error = uart_comm_xilinx_report_error;
    config->enter_critical = 0;
    config->exit_critical = 0;
    config->context = adapter;

    if (adapter != 0) {
        if (adapter->enter_critical != 0) {
            config->enter_critical = uart_comm_xilinx_enter_critical;
        }
        if (adapter->exit_critical != 0) {
            config->exit_critical = uart_comm_xilinx_exit_critical;
        }
    }
}
