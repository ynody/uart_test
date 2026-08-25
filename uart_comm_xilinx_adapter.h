#ifndef UART_COMM_XILINX_ADAPTER_H
#define UART_COMM_XILINX_ADAPTER_H

#include "uart_comm.h"

/**
 * @brief Platform-specific operation that enters or exits a short critical section.
 *
 * The callback is supplied by the board integration layer.  It normally disables
 * or restores only the UART receive interrupt.
 *
 * @param context Board-owned critical-section context.
 */
typedef void (*uart_comm_xilinx_critical_fn)(void *context);

/**
 * @brief Application callback used as the final destination of a T command.
 *
 * @param context Application-owned callback context.
 * @param text NUL-terminated command text with a trailing LF.
 * @param length Number of bytes in @p text excluding its NUL terminator.
 * @return UART_COMM_RESULT_OK on success, or an application-specific failure.
 */
typedef uart_comm_result_t (*uart_comm_xilinx_external_command_fn)(
    void *context,
    const char *text,
    uint32 length);

/**
 * @brief Board and application callbacks needed by the Xilinx adapter.
 */
typedef struct {
    /** @brief Callback that receives T command text. */
    uart_comm_xilinx_external_command_fn external_command;
    /** @brief Application-owned context passed to @c external_command. */
    void *external_context;
    /** @brief Board callback that enters the UART receive critical section. */
    uart_comm_xilinx_critical_fn enter_critical;
    /** @brief Board callback that leaves the UART receive critical section. */
    uart_comm_xilinx_critical_fn exit_critical;
    /** @brief Board-owned context passed to the critical-section callbacks. */
    void *critical_context;
} uart_comm_xilinx_adapter_options_t;

/**
 * @brief Persistent state owned by one Xilinx adapter instance.
 *
 * The adapter is intended for the core module's single-instance configuration.
 * It must remain valid from @c uart_comm_init() until UART command processing
 * has stopped.
 */
typedef struct {
    /** @brief Application callback and context for T command forwarding. */
    uart_comm_xilinx_external_command_fn external_command;
    void *external_context;
    /** @brief Board-specific critical-section callbacks and context. */
    uart_comm_xilinx_critical_fn enter_critical;
    uart_comm_xilinx_critical_fn exit_critical;
    void *critical_context;
} uart_comm_xilinx_adapter_t;

/**
 * @brief Initialize persistent adapter state from integration options.
 *
 * This function does not initialize a UART peripheral, register an ISR, or
 * enable interrupts.
 *
 * @param adapter Persistent adapter storage supplied by the caller.
 * @param options Board and application callback options.
 */
void uart_comm_xilinx_adapter_init(
    uart_comm_xilinx_adapter_t *adapter,
    const uart_comm_xilinx_adapter_options_t *options);

/**
 * @brief Create a core configuration backed by the Xilinx adapter.
 *
 * The generated configuration contains Xilinx printf, register-access, and
 * forwarding wrappers.  The caller passes it to @c uart_comm_init().
 *
 * @param adapter Initialized persistent adapter state.
 * @param config Destination core configuration.
 */
void uart_comm_xilinx_adapter_make_config(
    uart_comm_xilinx_adapter_t *adapter,
    uart_comm_config_t *config);

#endif /* UART_COMM_XILINX_ADAPTER_H */
