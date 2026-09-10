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
static volatile int SEMRxReady = 0;

#define SEMIP_MSEG_BUFLEN 128
static u8 semip_msgbuf[SEMIP_MSEG_BUFLEN];
static u32 semip_ptr = 0;

static u8 app_t_buf[16];

# if 0
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
     * SEM UARTのRX interruptはこの途中でも入れる。
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

static int receive_sem_byte(unsigned char c){
    //xil_printf("char: %c\r\n", c);
    if ( (c != '\r') && ( c != '\n') && ( c != 0 ) ){
        semip_msgbuf[semip_ptr++] = c;
    }else{
        if ( semip_ptr !=  0 ){
            semip_msgbuf[semip_ptr] = 0;
            xil_printf("Received SEM IP MSG: %s\r\n", semip_msgbuf);
            semip_ptr = 0;
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
 * 重い処理はしない方がよい。
 */
static void UartRecvHandler(void *CallBackRef, unsigned int EventData)
{
    (void)CallBackRef;


    if (EventData > 0) {
        PCRxReady = 1;
    }
}

static void SEMUartRecvHandler(void *CallBackRef, unsigned int EventData)
{
    (void)CallBackRef;


    if (EventData > 0) {
        SEMRxReady = 1;
    }
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
    if ( Status != XST_SUCCESS ){
        xil_printf("ERROR: PC Uart Init failed\r\n");
    }
    Status = SetupUart(&SEMUart, SEMUartBase);
    if ( Status != XST_SUCCESS ){
        xil_printf("ERROR: SEM Uart Init failed\r\n");
    }



    /*
     * UARTドライバから呼ばれる
     * 受信コールバックを登録
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
     * UART Lite側の割り込みをEnable
     */
    XUartLite_EnableInterrupt(&PCUart);
    XUartLite_EnableInterrupt(&SEMUart);
    /*
     * 「次の1文字を受信する」という要求を登録
     *
     * ここが重要。
     * 受信割り込みをEnableするだけではなく、
     * XUartLite_Recv()を呼んでおく。
     */
    XUartLite_Recv(&PCUart, &PCRxByte, 1);
    XUartLite_Recv(&SEMUart, &SEMRxByte, 1);

    xil_printf("UART interrupt test\r\n");
    xil_printf("Type a character.\r\n");

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

            } while (XUartLite_Recv(&PCUart, &PCRxByte, 1) == 1U);
        }
    #if 0
        if (PCRxReady) {

            /*
             * ISRとmainとの競合を単純化するため、
             * 先にフラグを落とす。
             */
            PCRxReady = 0;

            /*
            xil_printf("received: '%c' (0x%02x)\r\n",
                       RxByte, RxByte);
                       */

            uart_comm_receive_char(PCRxByte);

            while( uart_comm_has_pending_line() !=0U ){
                (void)uart_comm_process_received_line();
            }

            /*
             * 次の1文字の受信を再度予約する。
             *
             * これを忘れると次の文字を受信しない。
             */
            XUartLite_Recv(&PCUart, &PCRxByte, 1);

            
        }
#endif
        if (SEMRxReady) {

            SEMRxReady = 0;

            /*
            * 割り込みによって受信された1文字を処理
            */
            receive_sem_byte(SEMRxByte);

            /*
            * FIFOに既に文字が溜まっていれば、
            * XUartLite_Recv()自身がその場で1文字取得する。
            *
            * 戻り値==1 の間は、その文字もここで処理する。
            *
            * 戻り値==0 になったところで、
            * 「次の1文字待ち」の状態になる。
            */
            while (XUartLite_Recv(&SEMUart, &SEMRxByte, 1) == 1U) {
                receive_sem_byte(SEMRxByte);
            }
        }
#if 0
        if (SEMRxReady) {

            /*
             * ISRとmainとの競合を単純化するため、
             * 先にフラグを落とす。
             */
            SEMRxReady = 0;

            /*
            xil_printf("received: '%c' (0x%02x)\r\n",
                       RxByte, RxByte);
                       */

            receive_sem_byte(SEMRxByte);

            /*
             * 次の1文字の受信を再度予約する。
             *
             * これを忘れると次の文字を受信しない。
             */
            XUartLite_Recv(&SEMUart, &SEMRxByte, 1);

            
        }
#endif
    }

    return 0;
}

