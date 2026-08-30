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

#if 0
#include <stdio.h>
#include "platform.h"
#include "xil_printf.h"


int main()
{
    init_platform();

    xil_printf("Hello World\n\r");
    xil_printf("Successfully ran Hello World application!\r\n");
    cleanup_platform();
    return 0;
}
#endif


#if 0 
// xil_printf 出力先は、platformのvitis-comp.jsonを確認すること。
// BSP->Standaloneの表standalone_stdin/stdoutに、インスタンスが記述されている。

/******************************************************************************
 * uartlite_dual_loopback_test.c
 *
 * Target:
 *   AMD/Xilinx MicroBlaze V
 *   AXI UARTLite x 2
 *
 * Hardware configuration:
 *
 *   UARTLite_0
 *     TX ----> PC RX
 *     RX <---- PC TX
 *
 *   UARTLite_1
 *     TX ----+
 *            |
 *     RX <---+
 *          loopback
 *
 * Test sequence:
 *
 *   1. UARTLite_0 TX test using xil_printf()
 *   2. UARTLite_1 sends 0x00 ... 0xFF and checks loopback reception
 *   3. Interactive test:
 *
 *        PC
 *         |
 *         v
 *     UARTLite_0 RX
 *         |
 *     MicroBlaze V
 *         |
 *     UARTLite_1 TX
 *         |
 *      loopback
 *         |
 *     UARTLite_1 RX
 *         |
 *     MicroBlaze V
 *         |
 *     UARTLite_0 TX
 *         |
 *         v
 *        PC
 *
 ******************************************************************************/

#include "xparameters.h"
#include "xil_types.h"
#include "xil_printf.h"
#include "xuartlite_l.h"


/* -------------------------------------------------------------------------
 * UART base-address definitions
 * -------------------------------------------------------------------------
 *
 * IMPORTANT:
 *
 * Check the generated xparameters.h and confirm that:
 *
 *     UART instance 0 = PC-side UARTLite
 *     UART instance 1 = loopback UARTLite
 *
 * In current SDT-based Vitis flow, the generated macros are normally:
 *
 *     XPAR_XUARTLITE_0_BASEADDR
 *     XPAR_XUARTLITE_1_BASEADDR
 *
 * In the older/classic flow they are normally:
 *
 *     XPAR_UARTLITE_0_BASEADDR
 *     XPAR_UARTLITE_1_BASEADDR
 *
 * If the numbering in your generated xparameters.h differs, change only
 * PC_UART_BASEADDR and LOOP_UART_BASEADDR below.
 *
 * NOTE:
 * xil_printf() does NOT automatically use PC_UART_BASEADDR defined here.
 * xil_printf() uses the BSP stdout setting.
 *
 * Therefore, configure BSP stdout as UARTLite_0 (PC-side UART).
 * ------------------------------------------------------------------------- */

#ifdef SDT // 通常SDTのはず

#define PC_UART_BASEADDR       XPAR_XUARTLITE_0_BASEADDR
#define LOOP_UART_BASEADDR     XPAR_XUARTLITE_1_BASEADDR

#else

#define PC_UART_BASEADDR       XPAR_UARTLITE_0_BASEADDR
#define LOOP_UART_BASEADDR     XPAR_UARTLITE_1_BASEADDR

#endif


/* -------------------------------------------------------------------------
 * Timeout
 * -------------------------------------------------------------------------
 *
 * This is a simple polling-loop timeout, NOT an accurate time measurement.
 *
 * It prevents the program from hanging forever when, for example,
 * UARTLite_1 TX and RX are not actually connected.
 *
 * Increase this value if necessary.
 * ------------------------------------------------------------------------- */

#define UART_TIMEOUT_COUNT     10000000U


/* -------------------------------------------------------------------------
 * Number of bytes used by startup loopback test
 *
 * 256 tests all possible 8-bit values.
 * ------------------------------------------------------------------------- */

#define LOOPBACK_TEST_BYTES    256U


/* -------------------------------------------------------------------------
 * Function prototypes
 * ------------------------------------------------------------------------- */

static void uart_reset(UINTPTR baseaddr);

static int uart_send_byte_timeout(UINTPTR baseaddr, u8 data);

static int uart_recv_byte_timeout(UINTPTR baseaddr, u8 *data);

static int uart1_loopback_selftest(void);


/******************************************************************************
 * Reset TX and RX FIFOs
 ******************************************************************************/
static void uart_reset(UINTPTR baseaddr)
{
    XUartLite_WriteReg(
        baseaddr,
        XUL_CONTROL_REG_OFFSET,
        XUL_CR_FIFO_RX_RESET | XUL_CR_FIFO_TX_RESET
    );
}


/******************************************************************************
 * Send one byte with timeout
 *
 * Return:
 *   0  success
 *  -1  timeout
 ******************************************************************************/
static int uart_send_byte_timeout(UINTPTR baseaddr, u8 data)
{
    u32 timeout = UART_TIMEOUT_COUNT;

    while (XUartLite_IsTransmitFull(baseaddr)) {

        if (timeout == 0U) {
            return -1;
        }

        timeout--;
    }

    XUartLite_WriteReg(
        baseaddr,
        XUL_TX_FIFO_OFFSET,
        (u32)data
    );

    return 0;
}


/******************************************************************************
 * Receive one byte with timeout
 *
 * Return:
 *   0  success
 *  -1  timeout
 ******************************************************************************/
static int uart_recv_byte_timeout(UINTPTR baseaddr, u8 *data)
{
    u32 timeout = UART_TIMEOUT_COUNT;

    while (XUartLite_IsReceiveEmpty(baseaddr)) {

        if (timeout == 0U) {
            return -1;
        }

        timeout--;
    }

    *data = (u8)XUartLite_ReadReg(
        baseaddr,
        XUL_RX_FIFO_OFFSET
    );

    return 0;
}


/******************************************************************************
 * UARTLite_1 loopback self-test
 *
 * Sends:
 *
 *     0x00
 *     0x01
 *     ...
 *     0xFF
 *
 * one byte at a time.
 *
 * Because each transmitted byte is received before sending the next byte,
 * the 16-byte UARTLite FIFO cannot overflow during this test.
 *
 * Return:
 *   0  success
 *  -1  TX timeout
 *  -2  RX timeout
 *  -3  received data mismatch
 ******************************************************************************/
static int uart1_loopback_selftest(void)
{
    u32 i;

    for (i = 0U; i < LOOPBACK_TEST_BYTES; i++) {

        u8 tx_data;
        u8 rx_data;

        tx_data = (u8)i;

        /* Send one byte from UARTLite_1 */
        if (uart_send_byte_timeout(
                LOOP_UART_BASEADDR,
                tx_data) != 0) {

            xil_printf(
                "ERROR: UART1 TX timeout at data=0x%02x\r\n",
                (unsigned int)tx_data
            );

            return -1;
        }


        /* Receive the same byte through UARTLite_1 RX loopback */
        if (uart_recv_byte_timeout(
                LOOP_UART_BASEADDR,
                &rx_data) != 0) {

            xil_printf(
                "ERROR: UART1 RX timeout at data=0x%02x\r\n",
                (unsigned int)tx_data
            );

            return -2;
        }


        /* Compare transmitted and received values */
        if (rx_data != tx_data) {

            xil_printf(
                "ERROR: UART1 mismatch: TX=0x%02x RX=0x%02x\r\n",
                (unsigned int)tx_data,
                (unsigned int)rx_data
            );

            return -3;
        }
    }

    return 0;
}


/******************************************************************************
 * main
 ******************************************************************************/
int main(void)
{
    int status;

    u8 pc_rx_data;
    u8 loop_rx_data;


    /*
     * Reset both UARTLite FIFOs.
     *
     * UART0:
     *     connected to PC
     *
     * UART1:
     *     TX -> RX loopback
     */
    uart_reset(PC_UART_BASEADDR);
    uart_reset(LOOP_UART_BASEADDR);


    /*
     * UARTLite_0 TX test.
     *
     * IMPORTANT:
     * BSP stdout must be configured to UARTLite_0.
     *
     * If this message appears on the PC terminal,
     * UARTLite_0 TX and xil_printf stdout are working.
     */
    xil_printf("\r\n");
    xil_printf("========================================\r\n");
    xil_printf(" MicroBlaze V Dual UARTLite Test\r\n");
    xil_printf("========================================\r\n");

    xil_printf("PC UART base  : 0x%08x\r\n",
               (unsigned int)PC_UART_BASEADDR);

    xil_printf("LOOP UART base: 0x%08x\r\n",
               (unsigned int)LOOP_UART_BASEADDR);

    xil_printf("\r\n");


    /*
     * UARTLite_1 startup loopback test
     */
    xil_printf("Testing UARTLite_1 loopback...\r\n");

    status = uart1_loopback_selftest();

    if (status != 0) {

        xil_printf("\r\n");
        xil_printf("*** UARTLite_1 LOOPBACK TEST FAILED ***\r\n");
        xil_printf("\r\n");

        /*
         * Stop here because interactive relay test cannot work
         * if UARTLite_1 loopback itself is broken.
         */
        while (1) {
            /* halt */
        }
    }


    xil_printf(
        "UARTLite_1 loopback test PASSED "
        "(0x00 - 0xFF)\r\n"
    );

    xil_printf("\r\n");

    xil_printf(
        "Interactive test started.\r\n"
    );

    xil_printf(
        "Type characters from the PC terminal.\r\n"
    );

    xil_printf(
        "Each character will travel:\r\n"
    );

    xil_printf(
        "PC -> UART0 -> CPU -> UART1 -> loopback "
        "-> UART1 -> CPU -> UART0 -> PC\r\n"
    );

    xil_printf("\r\n");
    xil_printf("> ");


    /*
     * Interactive UART relay test
     */
    while (1) {

        /*
         * Wait for one character from PC through UARTLite_0.
         *
         * This explicitly accesses UARTLite_0 rather than using
         * inbyte(), so we are directly testing UARTLite_0 RX.
         */
        if (XUartLite_IsReceiveEmpty(PC_UART_BASEADDR)) {
            continue;
        }


        /*
         * Read one character from UARTLite_0 RX FIFO.
         */
        pc_rx_data = (u8)XUartLite_ReadReg(
            PC_UART_BASEADDR,
            XUL_RX_FIFO_OFFSET
        );


        /*
         * Forward the received byte to UARTLite_1 TX.
         */
        if (uart_send_byte_timeout(
                LOOP_UART_BASEADDR,
                pc_rx_data) != 0) {

            xil_printf(
                "\r\nERROR: UART1 TX timeout\r\n> "
            );

            continue;
        }


        /*
         * Receive the byte returning through UARTLite_1 loopback.
         */
        if (uart_recv_byte_timeout(
                LOOP_UART_BASEADDR,
                &loop_rx_data) != 0) {

            xil_printf(
                "\r\nERROR: UART1 RX timeout\r\n> "
            );

            continue;
        }


        /*
         * Check that UARTLite_1 returned exactly the same byte.
         */
        if (loop_rx_data != pc_rx_data) {

            xil_printf(
                "\r\nERROR: UART1 data mismatch "
                "TX=0x%02x RX=0x%02x\r\n> ",
                (unsigned int)pc_rx_data,
                (unsigned int)loop_rx_data
            );

            continue;
        }


        /*
         * Send the byte returned from UARTLite_1 back to the PC.
         *
         * Note that xil_printf() is deliberately NOT used here.
         *
         * By directly writing UARTLite_0 TX, this verifies the
         * complete UARTLite_0 RX/TX path independently of the
         * stdout abstraction.
         */
        if (uart_send_byte_timeout(
                PC_UART_BASEADDR,
                loop_rx_data) != 0) {

            /*
             * We cannot reliably report the error through UART0
             * if UART0 TX itself has failed.
             */
            continue;
        }
    }


    /* Never reached */
    return 0;
}

#else

/******************************************************************************
 * dual_uartlite_interrupt_test.c
 *
 * Target:
 *   AMD/Xilinx MicroBlaze V
 *   Vitis Unified IDE / SDT flow
 *   AXI UARTLite x 2
 *
 * Hardware:
 *
 *   UARTLite_0
 *      TX ----> PC RX
 *      RX <---- PC TX
 *
 *   UARTLite_1
 *      TX ----+
 *             |
 *      RX <---+
 *           loopback
 *
 * Interrupt connection:
 *
 *   UARTLite_0 interrupt ----\
 *                            +--> AXI Interrupt Controller --> MicroBlaze V
 *   UARTLite_1 interrupt ----/
 *
 *
 * Operation:
 *
 *   PC sends one character
 *       ↓
 *   UARTLite_0 RX interrupt
 *       ↓
 *   main() detects received byte
 *       ↓
 *   UARTLite_1 sends byte
 *       ↓
 *   hardware loopback
 *       ↓
 *   UARTLite_1 RX interrupt
 *       ↓
 *   main() sends returned byte through UARTLite_0
 *       ↓
 *   PC receives same character
 *
 ******************************************************************************/

#include "xparameters.h"
#include "xstatus.h"
#include "xil_types.h"
#include "xil_printf.h"

#include "xuartlite.h"
#include "xinterrupt_wrap.h"


/* -------------------------------------------------------------------------
 * UART base addresses
 * -------------------------------------------------------------------------
 *
 * Check xparameters.h.
 *
 * Typical SDT definitions:
 *
 *   XPAR_XUARTLITE_0_BASEADDR
 *   XPAR_XUARTLITE_1_BASEADDR
 *
 * UARTLite_0 must be the PC-side UART.
 * UARTLite_1 must be the loopback UART.
 *
 * If your generated names differ, change only these macros.
 * ------------------------------------------------------------------------- */

#define PC_UART_BASEADDR       XPAR_XUARTLITE_0_BASEADDR
#define LOOP_UART_BASEADDR     XPAR_XUARTLITE_1_BASEADDR


/* -------------------------------------------------------------------------
 * UART driver instances
 * ------------------------------------------------------------------------- */

static XUartLite PcUart;
static XUartLite LoopUart;


/* -------------------------------------------------------------------------
 * One-byte receive buffers
 *
 * XUartLite_Recv() receives asynchronously in interrupt mode.
 * Therefore these buffers must remain valid after the function returns.
 * ------------------------------------------------------------------------- */

static u8 PcRxByte;
static u8 LoopRxByte;


/* -------------------------------------------------------------------------
 * Flags shared between interrupt callbacks and main()
 *
 * volatile is required because these values are modified asynchronously
 * from interrupt context.
 * ------------------------------------------------------------------------- */

static volatile int PcRxReady   = 0;
static volatile int LoopRxReady = 0;


/* -------------------------------------------------------------------------
 * Optional counters for debugging
 * ------------------------------------------------------------------------- */

static volatile u32 PcRxInterruptCount   = 0;
static volatile u32 LoopRxInterruptCount = 0;


/* -------------------------------------------------------------------------
 * Function prototypes
 * ------------------------------------------------------------------------- */

static int SetupUart(XUartLite *Uart,
                     UINTPTR BaseAddress);

static void PcRecvHandler(void *CallBackRef,
                          unsigned int EventData);

static void LoopRecvHandler(void *CallBackRef,
                            unsigned int EventData);

static void DummySendHandler(void *CallBackRef,
                             unsigned int EventData);


/******************************************************************************
 * Dummy TX-complete callback
 *
 * UARTLite interrupt mode uses both RX and TX interrupt processing internally.
 * We do not need to do anything when transmission completes here.
 ******************************************************************************/
static void DummySendHandler(void *CallBackRef,
                             unsigned int EventData)
{
    (void)CallBackRef;
    (void)EventData;
}


/******************************************************************************
 * PC UART receive callback
 *
 * Called from interrupt context.
 *
 * IMPORTANT:
 *   Do as little work as possible here.
 *   Do NOT call xil_printf() here.
 *
 * EventData is the number of received bytes completed by the driver.
 ******************************************************************************/
static void PcRecvHandler(void *CallBackRef,
                          unsigned int EventData)
{
    XUartLite *UartPtr = (XUartLite *)CallBackRef;

    if (EventData > 0U) {

        PcRxReady = 1;

        PcRxInterruptCount++;

        /*
         * Do NOT start the next receive here.
         *
         * main() will consume PcRxByte first, then arm the next receive.
         */
    }

    (void)UartPtr;
}


/******************************************************************************
 * Loopback UART receive callback
 ******************************************************************************/
static void LoopRecvHandler(void *CallBackRef,
                            unsigned int EventData)
{
    XUartLite *UartPtr = (XUartLite *)CallBackRef;

    if (EventData > 0U) {

        LoopRxReady = 1;

        LoopRxInterruptCount++;
    }

    (void)UartPtr;
}


/******************************************************************************
 * Initialize one UARTLite and connect it to the interrupt subsystem.
 *
 * SDT flow:
 *
 *   XUartLite_LookupConfig(BaseAddress)
 *        ↓
 *   Config->IntrId
 *   Config->IntrParent
 *        ↓
 *   XSetupInterruptSystem()
 *
 ******************************************************************************/
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


/******************************************************************************
 * main
 ******************************************************************************/
int main(void)
{
    int Status;

    u8 TxByte;


    /**************************************************************************
     * Initialize PC-side UART
     **************************************************************************/

    Status = SetupUart(
        &PcUart,
        PC_UART_BASEADDR
    );

    if (Status != XST_SUCCESS) {

        /*
         * xil_printf may still work if BSP stdout was already configured
         * to UARTLite_0, but UART interrupt setup has failed.
         */
        xil_printf(
            "ERROR: PC UART initialization failed\r\n"
        );

        return XST_FAILURE;
    }


    /**************************************************************************
     * Initialize loopback UART
     **************************************************************************/

    Status = SetupUart(
        &LoopUart,
        LOOP_UART_BASEADDR
    );

    if (Status != XST_SUCCESS) {

        xil_printf(
            "ERROR: LOOP UART initialization failed\r\n"
        );

        return XST_FAILURE;
    }


    /**************************************************************************
     * Configure callback handlers
     **************************************************************************/

    XUartLite_SetRecvHandler(
        &PcUart,
        PcRecvHandler,
        &PcUart
    );

    XUartLite_SetSendHandler(
        &PcUart,
        DummySendHandler,
        &PcUart
    );


    XUartLite_SetRecvHandler(
        &LoopUart,
        LoopRecvHandler,
        &LoopUart
    );

    XUartLite_SetSendHandler(
        &LoopUart,
        DummySendHandler,
        &LoopUart
    );


    /**************************************************************************
     * Enable UARTLite interrupts
     **************************************************************************/

    XUartLite_EnableInterrupt(&PcUart);

    XUartLite_EnableInterrupt(&LoopUart);


    /**************************************************************************
     * Start one-byte receive operation on both UARTs
     *
     * VERY IMPORTANT:
     *
     * XUartLite interrupt reception does NOT mean:
     *
     *     "an interrupt automatically gives me a byte"
     *
     * Instead, the driver must first be told:
     *
     *     "receive N bytes into this buffer"
     *
     * using XUartLite_Recv().
     *
     * The interrupt handler then fills that buffer and invokes the callback
     * when the requested number of bytes has been received.
     *
     * Here N = 1.
     **************************************************************************/

    XUartLite_Recv(
        &PcUart,
        &PcRxByte,
        1U
    );

    XUartLite_Recv(
        &LoopUart,
        &LoopRxByte,
        1U
    );


    /**************************************************************************
     * Startup message
     *
     * BSP stdout must be configured to UARTLite_0.
     **************************************************************************/

    xil_printf("\r\n");
    xil_printf("========================================\r\n");
    xil_printf(" Dual UARTLite Interrupt Test\r\n");
    xil_printf(" MicroBlaze V\r\n");
    xil_printf("========================================\r\n");

    xil_printf(
        "PC UART base   : 0x%08x\r\n",
        (unsigned int)PC_UART_BASEADDR
    );

    xil_printf(
        "LOOP UART base : 0x%08x\r\n",
        (unsigned int)LOOP_UART_BASEADDR
    );

    xil_printf("\r\n");

    xil_printf(
        "UART interrupt mode enabled.\r\n"
    );

    xil_printf(
        "Type characters from the PC terminal.\r\n"
    );

    xil_printf(
        "Path:\r\n"
    );

    xil_printf(
        "PC -> UART0 RX IRQ -> CPU -> UART1 TX -> "
        "loopback -> UART1 RX IRQ -> CPU -> UART0 TX -> PC\r\n"
    );

    xil_printf("\r\n");
    xil_printf("> ");


    /**************************************************************************
     * Main loop
     **************************************************************************/

    while (1) {

        /**********************************************************************
         * UARTLite_0 RX completed
         *
         * PcRxByte was filled by the UARTLite interrupt driver.
         **********************************************************************/
        if (PcRxReady != 0) {

            /*
             * Clear the flag first.
             */
            PcRxReady = 0;


            /*
             * Save byte locally.
             *
             * PcRxByte will later be reused by the next XUartLite_Recv().
             */
            TxByte = PcRxByte;


            /*
             * IMPORTANT:
             *
             * Arm UARTLite_0 for the NEXT received byte.
             *
             * Without this call, only the first PC character would be
             * received.
             */
            XUartLite_Recv(
                &PcUart,
                &PcRxByte,
                1U
            );


            /*
             * Send the received PC byte through UARTLite_1.
             *
             * XUartLite_Send() is non-blocking.
             *
             * Since we send only one byte at a time here,
             * a one-byte send request is sufficient.
             */
            XUartLite_Send(
                &LoopUart,
                &TxByte,
                1U
            );
        }


        /**********************************************************************
         * UARTLite_1 loopback RX completed
         **********************************************************************/
        if (LoopRxReady != 0) {

            LoopRxReady = 0;


            /*
             * Save returned byte.
             */
            TxByte = LoopRxByte;


            /*
             * Arm UARTLite_1 RX for next loopback byte BEFORE doing any
             * other long processing.
             */
            XUartLite_Recv(
                &LoopUart,
                &LoopRxByte,
                1U
            );


            /*
             * Return the byte to PC using UARTLite_0.
             *
             * This explicitly exercises UARTLite_0 TX.
             */
            XUartLite_Send(
                &PcUart,
                &TxByte,
                1U
            );
        }
    }


    /* Never reached */
    return XST_SUCCESS;
}
#endif