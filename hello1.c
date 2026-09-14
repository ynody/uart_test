#include "platform.h"
#include "xstatus.h"
#include "xil_types.h"
#include "xil_printf.h"

#include "xparameters.h"
#include "xuartlite.h"
#include "xuartlite_l.h"
#include "xinterrupt_wrap.h"

#include "uart_comm.h"
#include "uart_comm_xilinx_adapter.h"

#define PC_UART_BAESADDR XPAR_XUARTLITE_0_BASEADDR
#define SEM_UART_BASEADDR XPAR_XUARTLITE_1_BASEADDR

static uart_comm_xilinx_adapter_t adapter;
static uart_comm_xilinx_adapter_options_t options;
static uart_comm_config_t config;

static XUartLite PCUart;
static XUartLite SEMUart;

static u8 PCRxByte;
static u8 SEMRxByte;

static volatile int PCRxReady = 0;

/*
 * SEM UART receive ring buffer.
 *
 * The producer is SEMUartRecvHandler() running in interrupt context.
 * The consumer is main().  Monotonic head/tail counters allow all 256 bytes
 * of the array to be used; the array index itself is masked to 8 bits.
 */
#define SEM_RX_RING_SIZE 256U
#define SEM_RX_RING_MASK (SEM_RX_RING_SIZE - 1U)

static u8 sem_rx_ring[SEM_RX_RING_SIZE];
static volatile u32 sem_rx_head = 0U;
static volatile u32 sem_rx_tail = 0U;
static volatile u32 sem_rx_overflow_count = 0U;

/*
 * Buffer used to reconstruct one line of SEM monitor output.
 * SEM output is emitted to the PC only after CR/LF, preserving line order.
 */
#define SEMIP_MSEG_BUFLEN 256U
static u8 semip_msgbuf[SEMIP_MSEG_BUFLEN];
static u32 semip_ptr = 0U;
static int semip_line_truncated = 0;

static u8 app_t_buf[16];

#if 0
static uart_comm_result_t application_t_command(
    void *opaque, const char *text, uint32 length
){
    (void)opaque;
    (void)length;

    //return application_handle_text(text, length);

    for( uint32 i = 0; i < length; i++){
        app_t_buf[i] = text[i];
    }

    unsigned int sent_bytes = 0;

    if ( length > 16 ){
        xil_printf("Too long input for transferreing UART\r\n");
        return 1;
    }
    if ( length > 1 ){
        xil_printf("Transferring another UART (%u): %s", length, app_t_buf);
        app_t_buf[length-1] = '\r';

        for(uint32 i = 0; i < length; i++ ){
            XUartLite_SendByte(SEM_UART_BASEADDR, app_t_buf[i]);
        }
        //sent_bytes = XUartLite_Send(&SEMUart, app_t_buf, length+1);
        xil_printf("sent_bytes=%u\n", sent_bytes);
    }

    return UART_COMM_RESULT_OK;
}
#endif

static uart_comm_result_t application_t_command(
    void *opaque,
    const char *text,
    uint32 length
)
{
    (void)opaque;

    if (length > sizeof(app_t_buf)) {
        xil_printf("Too long input for transferring UART\r\n");
        return 1;
    }

    if (length <= 1U) {
        return UART_COMM_RESULT_OK;
    }

    for (uint32 i = 0; i < length; i++) {
        app_t_buf[i] = (u8)text[i];
    }

    /*
     * 最後の改行文字をCRに置換
     */
    app_t_buf[length - 1U] = '\r';
    xil_printf("Transferring another UART (%u bytes)\r\n",
               (unsigned int)length);

    /*
     * Blocking送信。
     * CPU interrupt自体は有効なので、
     * SEM UARTのRX interruptはこの途中でも入り、
     * 受信文字はsem_rx_ring[]へ退避される。
     */
    for (uint32 i = 0; i < length; i++) {
        XUartLite_SendByte(SEM_UART_BASEADDR, app_t_buf[i]);
    }

    return UART_COMM_RESULT_OK;
}

static void board_uart_enter_critical(void *opaque){
    (void)opaque;
    //board_uart_disable_receive_interrupt();
}

static void board_uart_exit_critical(void *opaque){
    (void)opaque;
    //board_uart_enable_receive_interrupt();
}

/*
 * Push one received SEM byte into the ring buffer.
 * Called only from SEM RX interrupt context.
 */
static void sem_rx_ring_push(u8 c)
{
    u32 head = sem_rx_head;

    if ((head - sem_rx_tail) >= SEM_RX_RING_SIZE) {
        /*
         * Do not overwrite old data: preserving ordering is more useful than
         * silently replacing an earlier byte.  main() reports this condition.
         */
        sem_rx_overflow_count++;
        return;
    }

    sem_rx_ring[head & SEM_RX_RING_MASK] = c;
    sem_rx_head = head + 1U;
}

/*
 * Pop one SEM byte from the ring buffer.
 * Called only from main context.
 */
static int sem_rx_ring_pop(u8 *c)
{
    u32 tail = sem_rx_tail;

    if (tail == sem_rx_head) {
        return 0;
    }

    *c = sem_rx_ring[tail & SEM_RX_RING_MASK];
    sem_rx_tail = tail + 1U;
    return 1;
}

static int receive_sem_byte(unsigned char c)
{
    if ((c != '\r') && (c != '\n') && (c != 0U)) {
        if (semip_ptr < (SEMIP_MSEG_BUFLEN - 1U)) {
            semip_msgbuf[semip_ptr++] = c;
        } else {
            /* Keep the first 255 bytes and discard until the line terminator. */
            semip_line_truncated = 1;
        }
    } else {
        if ((semip_ptr != 0U) || (semip_line_truncated != 0)) {
            semip_msgbuf[semip_ptr] = 0U;

            if (semip_line_truncated != 0) {
                xil_printf("Received SEM IP MSG: %s [TRUNCATED]\r\n",
                           semip_msgbuf);
            } else {
                xil_printf("Received SEM IP MSG: %s\r\n", semip_msgbuf);
            }

            semip_ptr = 0U;
            semip_line_truncated = 0;
        }
    }

    return 0;
}

static int SetupUart(XUartLite *Uart,
                     UINTPTR BaseAddress)
{
    int Status;
    XUartLite_Config *CfgPtr;

    /*
     * Look up configuration using base address.
     *
     * This is the SDT-style API.
     */
    CfgPtr = XUartLite_LookupConfig(BaseAddress);

    if (CfgPtr == NULL) {
        return XST_FAILURE;
    }

    /*
     * Initialize UARTLite driver.
     *
     * In SDT flow, XUartLite_Initialize() takes BaseAddress.
     */
    Status = XUartLite_Initialize(Uart, BaseAddress);

    if (Status != XST_SUCCESS) {
        return Status;
    }

    /*
     * Reset UART FIFOs.
     */
    XUartLite_ResetFifos(Uart);

    /*
     * Connect the UARTLite driver's interrupt handler.
     *
     * XUartLite_InterruptHandler performs the actual UART hardware
     * interrupt processing.
     *
     * It then invokes the user callbacks configured by:
     *
     *   XUartLite_SetRecvHandler()
     *   XUartLite_SetSendHandler()
     */
    Status = XSetupInterruptSystem(
        Uart,
        (XInterruptHandler)XUartLite_InterruptHandler,
        CfgPtr->IntrId,
        CfgPtr->IntrParent,
        XINTERRUPT_DEFAULT_PRIORITY
    );

    if (Status != XST_SUCCESS) {
        return Status;
    }

    return XST_SUCCESS;
}

/*
 * UART Lite受信完了時に呼ばれるコールバック
 *
 * これは実質的にISRコンテキストから呼ばれるので、
 * 重い処理はしない。
 */
static void UartRecvHandler(void *CallBackRef, unsigned int EventData)
{
    (void)CallBackRef;

    if (EventData > 0U) {
        PCRxReady = 1;
    }
}

/*
 * Drain bytes that are already waiting in the SEM UART RX FIFO, put them into
 * the software ring buffer, then leave one XUartLite_Recv() request armed for
 * the next byte.
 *
 * XUartLite_Recv() is non-blocking.  If it returns 1, one byte was obtained
 * immediately from the hardware FIFO.  When it returns 0, no byte is currently
 * available and the driver's one-byte receive request remains armed.
 *
 * This function is short and contains no printf, so it is safe to use from the
 * receive callback.
 */
static void SEMUartDrainAndRearm(void)
{
    while (XUartLite_Recv(&SEMUart, &SEMRxByte, 1U) == 1U) {
        sem_rx_ring_push(SEMRxByte);
    }
}

static void SEMUartRecvHandler(void *CallBackRef, unsigned int EventData)
{
    (void)CallBackRef;

    /*
     * The byte that completed the currently armed one-byte receive request is
     * already in SEMRxByte when this callback is entered.
     */
    if (EventData > 0U) {
        sem_rx_ring_push(SEMRxByte);
    }

    /*
     * Important: drain any bytes that accumulated in the 16-byte UARTLite FIFO
     * while main was busy (for example inside xil_printf), then immediately
     * re-arm reception before returning from the interrupt context.
     */
    SEMUartDrainAndRearm();
}

int main()
{
    init_platform();

    options.external_command = application_t_command;
    options.external_context = NULL;
    options.enter_critical = board_uart_enter_critical;
    options.exit_critical = board_uart_exit_critical;
    options.critical_context = NULL;

    uart_comm_xilinx_adapter_init(&adapter, &options);
    uart_comm_xilinx_adapter_make_config(&adapter, &config);
    uart_comm_init(&config);

#if 0
    xil_printf("Hello World\n\r");
    xil_printf("Successfully ran Hello World application!\r\n");
    cleanup_platform();
    return 0;
#endif

    int Status;

    /*
     * ここは実際のUART Liteのベースアドレスに合わせる。
     *
     * xparameters.hを見て例えば
     *
     * XPAR_XUARTLITE_0_BASEADDR
     * XPAR_AXI_UARTLITE_0_BASEADDR
     *
     * などを使用。
     */
    UINTPTR UartBaseAddress = PC_UART_BAESADDR;
    UINTPTR SEMUartBase = SEM_UART_BASEADDR;

    Status = SetupUart(&PCUart, UartBaseAddress);
    if (Status != XST_SUCCESS) {
        xil_printf("ERROR: PC Uart Init failed\r\n");
    }

    Status = SetupUart(&SEMUart, SEMUartBase);
    if (Status != XST_SUCCESS) {
        xil_printf("ERROR: SEM Uart Init failed\r\n");
    }

    /*
     * UARTドライバから呼ばれる受信コールバックを登録
     */
    XUartLite_SetRecvHandler(
        &PCUart,
        UartRecvHandler,
        &PCUart
    );

    XUartLite_SetRecvHandler(
        &SEMUart,
        SEMUartRecvHandler,
        &SEMUart
    );

    /*
     * PC UART is kept in the same one-byte receive scheme as the original.
     */
    XUartLite_EnableInterrupt(&PCUart);
    XUartLite_Recv(&PCUart, &PCRxByte, 1U);

    /*
     * SEM UART:
     * First drain anything already in the FIFO and leave the next receive
     * request armed, then enable the hardware interrupt.
     */
    SEMUartDrainAndRearm();
    XUartLite_EnableInterrupt(&SEMUart);

    xil_printf("UART interrupt test\r\n");
    xil_printf("Type a character.\r\n");

    {
        u32 reported_sem_overflow_count = 0U;

        while (1) {
            if (PCRxReady) {
                PCRxReady = 0;

                /*
                 * IRQで取得した文字と、
                 * 既にFIFOに溜まっている文字を全部処理
                 */
                do {
                    uart_comm_receive_char(PCRxByte);

                    while (uart_comm_has_pending_line() != 0U) {
                        (void)uart_comm_process_received_line();
                    }
                } while (XUartLite_Recv(&PCUart, &PCRxByte, 1U) == 1U);
            }

            /*
             * SEM UART bytes are now consumed only from the software ring.
             * receive_sem_byte() reconstructs lines and prints them in exactly
             * the same byte/line order in which they were received.
             *
             * While xil_printf() is blocking here, SEM RX interrupts remain
             * enabled and SEMUartRecvHandler() continues filling sem_rx_ring[].
             */
            {
                u8 sem_byte;

                while (sem_rx_ring_pop(&sem_byte) != 0) {
                    receive_sem_byte(sem_byte);
                }
            }

            /*
             * Report overflow only after the currently buffered SEM data has
             * been drained, so this diagnostic does not get inserted between
             * already-buffered SEM lines.
             */
            if ((sem_rx_tail == sem_rx_head) &&
                (reported_sem_overflow_count != sem_rx_overflow_count)) {
                reported_sem_overflow_count = sem_rx_overflow_count;
                xil_printf("WARNING: SEM RX ring overflow, dropped bytes=%u\r\n",
                           (unsigned int)reported_sem_overflow_count);
            }
        }
    }

    return 0;
}
