/******************************************************************************
* Copyright (C) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/
/*
 * helloworld.c: simple test application
 *
 * This application configures UART 16550 to baud rate 9600.
 * PS7 UART (Zynq) is not initialized by this application, since
 * bootrom/bsp configures it to baud rate 115200
 *
 * ------------------------------------------------
 * | UART TYPE   BAUD RATE                        |
 * ------------------------------------------------
 *   uartns550   9600
 *   uartlite    Configurable only in HW design
 *   ps7_uart    115200 (configured by bootrom/bsp)
 */

#include <stdio.h>
#include "platform.h"
#include "xil_printf.h"

#include "xparameters.h"
#include "xuartlite.h"
#include "xinterrupt_wrap.h"

#include "uart_comm.h"
#include "uart_comm_xilinx_adapter.h"

static uart_comm_xilinx_adapter_t adapter;
static uart_comm_xilinx_adapter_options_t options;
static uart_comm_config_t config;

static uart_comm_result_t application_t_command(
    void *opaque, const char *text, uint32 length
){
    (void)opaque;
    (void)length;
    //return application_handle_text(text, length);

    xil_printf("Transferring another UART: %s", text);

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

static XUartLite UartLite;

/* 受信先 */
static u8 RxByte;

/* main側へ通知するフラグ */
static volatile int RxReady = 0;


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
        RxReady = 1;
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
    XUartLite_Config *Config;

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
    UINTPTR UartBaseAddress = XPAR_XUARTLITE_0_BASEADDR;

    /*
     * UART Lite初期化
     */
    Config = XUartLite_LookupConfig(UartBaseAddress);

    if (Config == NULL) {
        return XST_FAILURE;
    }

    Status = XUartLite_Initialize(&UartLite, UartBaseAddress);

    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    /*
     * UART Liteの割り込みハンドラを
     * システムの割り込み機構へ登録
     */
    Status = XSetupInterruptSystem(
        &UartLite,
        &XUartLite_InterruptHandler,
        Config->IntrId,
        Config->IntrParent,
        XINTERRUPT_DEFAULT_PRIORITY
    );

    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    /*
     * UARTドライバから呼ばれる
     * 受信コールバックを登録
     */
    XUartLite_SetRecvHandler(
        &UartLite,
        UartRecvHandler,
        &UartLite
    );

    /*
     * UART Lite側の割り込みをEnable
     */
    XUartLite_EnableInterrupt(&UartLite);

    /*
     * 「次の1文字を受信する」という要求を登録
     *
     * ここが重要。
     * 受信割り込みをEnableするだけではなく、
     * XUartLite_Recv()を呼んでおく。
     */
    XUartLite_Recv(&UartLite, &RxByte, 1);

    xil_printf("UART interrupt test\r\n");
    xil_printf("Type a character.\r\n");

    while (1) {

        if (RxReady) {

            /*
             * ISRとmainとの競合を単純化するため、
             * 先にフラグを落とす。
             */
            RxReady = 0;

            /*
            xil_printf("received: '%c' (0x%02x)\r\n",
                       RxByte, RxByte);
                       */

            uart_comm_receive_char(RxByte);

            while( uart_comm_has_pending_line() !=0U ){
                (void)uart_comm_process_received_line();
            }

            /*
             * 次の1文字の受信を再度予約する。
             *
             * これを忘れると次の文字を受信しない。
             */
            XUartLite_Recv(&UartLite, &RxByte, 1);

            
        }
    }

    return 0;
}

