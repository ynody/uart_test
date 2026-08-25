# UART コマンド通信モジュール統合ガイド

## 1. 目的と対象読者

本書は、UART コマンド通信コアを Xilinx MicroBlaze V システムへ統合する利用者向けの
説明書である。UART ハードウェア、Xilinx ドライバ、割り込み制御、`xil_printf`、
および実レジスタアクセスは本コアの対象外であり、統合側のアダプタが担当する。

コアの公開インタフェースは [uart_comm.h](uart_comm.h) を参照すること。コマンドの
文法および結果コードの定義は [UART_COMM_SPEC.md](UART_COMM_SPEC.md) を参照すること。

## 2. モジュール分割

```text
UART IP / Xilinx driver / interrupt controller
                    |
                    v
main.c / board integration
  - receive ISR
                    |
                    v
Xilinx-dependent adapter
  - critical-section callbacks
  - xil_printf output wrappers
  - register read/write callbacks
                    |
                    v
UART command core
  - uart_comm_receive_char()
  - uart_comm_has_pending_line()
  - uart_comm_process_received_line()
                    |
                    v
Application T-command handler
```

`uart_comm.c` および `uart_comm.h` は、Xilinx API、Xilinx ヘッダ、`stdio`、
`xil_printf` に依存してはならない。Xilinx ヘッダを必要とする記述は、
`uart_comm_xilinx_adapter.c`、`main.c`、およびボード統合層に限定すること。

推奨するファイル構成は以下のとおりである。

```text
uart_comm.c / uart_comm.h
    ハードウェア非依存のコア

uart_comm_xilinx_adapter.c / uart_comm_xilinx_adapter.h
    xil_printf、Xil_In32/Xil_Out32、排他コールバック、
    T ハンドラの接続を収容するアダプタ

application_command.c
    T コマンドの実際の転送先ハンドラ

main.c
    UART ドライバ初期化、UART受信ISR、割り込みの有効化、メインループ
```

## 3. 初期化順序

UART 受信割り込みを有効化する前に、コアを初期化すること。初期化中に割り込みが
発生すると、未初期化のコールバック設定へアクセスする危険がある。

推奨する順序は以下のとおりである。

1. Xilinx UART ドライバおよび割り込みコントローラを初期化する。
2. アダプタ用コンテキストと `uart_comm_config_t` を構築する。
3. `uart_comm_init(&config)` を呼び出す。
4. UART 受信 ISR を割り込みコントローラへ接続する。
5. UART 受信割り込みと CPU 割り込みを有効化する。

アダプタ設定例の擬似コードを以下に示す。UART IP と割り込みコントローラの種類に
応じて、ドライバ呼び出し名を置き換えること。

```c
static uart_comm_xilinx_adapter_t adapter;
static uart_comm_xilinx_adapter_options_t options;
static uart_comm_config_t config;

options.external_command = application_t_command;
options.external_context = &application_context;
options.enter_critical = board_uart_enter_critical;
options.exit_critical = board_uart_exit_critical;
options.critical_context = &board_context;

uart_comm_xilinx_adapter_init(&adapter, &options);
uart_comm_xilinx_adapter_make_config(&adapter, &config);

uart_comm_init(&config);
```

## 4. `main.c` の UART 受信割り込みハンドラ

`uart_comm_receive_char()` は UART 受信割り込みから呼び出すための軽量な関数である。
1 回の呼び出しで 1 文字を受け取り、受信バッファへの格納、CRLF/LF の検出、受信FIFOの
状態遷移だけを行う。

ISR から次の処理を行ってはならない。

- `uart_comm_process_received_line()` の呼び出し
- `xil_printf` による出力
- レジスタ読み出し・書き込み
- `T` ハンドラの呼び出し

本アダプタは ISR 関数を提供しない。UART FIFO に複数バイトが入るIPでは、`main.c` または
ボード統合層が、ISR内でFIFOを空にするまで以下を繰り返す。

```c
static void main_uart_receive_isr(void *reference)
{
    board_context_t *context = (board_context_t *)reference;

    while (board_uart_rx_available(context) != 0) {
        char character = (char)board_uart_read_byte(context);
        (void)uart_comm_receive_char(character);
    }

    board_uart_acknowledge_receive_interrupt(context);
}
```

`board_uart_rx_available()`、`board_uart_read_byte()`、割り込み要因の確認・クリアの
順序は、使用する UART IP の Xilinx ドライバ仕様に従うこと。たとえば AXI UARTLite、
AXI UART 16550 などではAPIおよび割り込み処理手順が異なる。

## 5. メインループでのコマンド処理

行の解析、メッセージ出力、レジスタアクセス、外部コマンド転送は、通常コンテキストで
のみ実行する。メインループでは、保留中の行または受信キューオーバーフロー報告が
なくなるまで処理する。

```c
for (;;) {
    while (uart_comm_has_pending_line() != 0U) {
        (void)uart_comm_process_received_line();
    }

    application_background_task();
}
```

`uart_comm_process_received_line()` は、1回の呼び出しで1行、または受信キュー
オーバーフローを1件だけ処理する。戻り値が `UART_COMM_RESULT_NO_LINE_READY` の場合は
処理対象がないことを示す。

## 6. 排他制御コールバック

コアは、メインループが受信FIFOのエントリを取得・解放する短い区間だけ
`enter_critical()` と `exit_critical()` を呼び出す。アダプタはこの区間で UART
受信割り込みが同じFIFOを更新しないようにすること。

- 原則として、UART受信割り込みだけを一時的に無効化すること。
- コマンド解析、`xil_printf`、レジスタアクセス、外部ハンドラ実行中は割り込みを
  無効化してはならない。
- PC 単体テストでは、両コールバックを何もしない関数にできる。

現行の公開APIは `void enter_critical(void *context)` と
`void exit_critical(void *context)` であり、割り込み状態を返却・復元するトークンを
持たない。そのため、統合側はこれらのコールバックをネストさせず、呼び出し元でUART
受信割り込みが有効であることを前提とする実装にすること。

呼び出し元の割り込み有効状態を常に保存・復元する必要がある場合は、実アダプタの
実装前に、公開APIを「状態トークンを返す `enter_critical`」と「そのトークンを
受け取る `exit_critical`」へ拡張することを推奨する。

## 7. `xil_printf` 出力ラッパー

コアは文字列の書式化を行わない。成功・エラーの出力は、以下のラッパー関数に実装する。

- `adapter_report_set()`
- `adapter_report_read()`
- `adapter_report_transfer()`
- `adapter_report_error()`

書き込み・読み出しの出力形式は仕様で固定されている。

```c
static void adapter_report_set(void *opaque, uint32 address, uint32 value)
{
    (void)opaque;
    xil_printf("=> S 0x%08x %xh\n", address, value);
}

static void adapter_report_read(void *opaque, uint32 address, uint32 value)
{
    (void)opaque;
    xil_printf("=> L 0x%08x %xh\n", address, value);
}
```

`xil_printf` の実装が対象BSPで `%08x` のフィールド幅とゼロ埋めをサポートすることを
確認すること。サポートされない場合、アダプタ内で8桁16進数を出力する補助関数を作成し、
コアを変更しないこと。

`T` の成功メッセージは、転送先に渡す末尾 `LF` を除いた本文をエコーバックする。

```c
static void adapter_report_transfer(void *opaque,
                                    const char *text,
                                    uint32 length)
{
    (void)opaque;
    xil_printf("=> T %s\n", text);
    (void)length;
}
```

エラー文面はアダプタ側で `uart_comm_result_t` を原因説明文字列へ対応付ける。
すべてのメッセージの末尾には `\n` を付加すること。

## 8. レジスタアクセスラッパー

コアはアドレスを直接デリファレンスしない。アダプタは次のコールバック内で
実レジスタアクセスを実装する。

```c
static uart_comm_result_t adapter_register_read(void *opaque,
                                                 uint32 address,
                                                 uint32 *value)
{
    (void)opaque;
    *value = (uint32)Xil_In32(address);
    return UART_COMM_RESULT_OK;
}

static uart_comm_result_t adapter_register_write(void *opaque,
                                                  uint32 address,
                                                  uint32 value)
{
    (void)opaque;
    Xil_Out32(address, value);
    return UART_COMM_RESULT_OK;
}
```

`Xil_In32` / `Xil_Out32` を使わず直接アクセスする場合も、`volatile` を使用し、
その実装をアダプタ側だけに置くこと。現行仕様では、コアおよびアダプタともに
アドレス範囲、アラインメント、読み書き権限の事前検査を行わない。

## 9. `T` コマンドの外部ハンドラ

`T` コマンドでは、本文の末尾に `LF` を1文字付加し、NUL終端した文字列が
`external_command` コールバックへ渡される。長さには末尾 `LF` を含み、NUL文字は
含まれない。

```c
static uart_comm_result_t application_t_command(void *opaque,
                                                 const char *text,
                                                 uint32 length)
{
    application_context_t *context = (application_context_t *)opaque;

    return application_handle_text(context, text, length);
}
```

渡された `text` はコールバック呼び出し中だけ有効である。呼び出し後も文字列を
保持する必要がある場合、外部ハンドラ側で必要な長さだけコピーすること。コアは
コールバックから復帰した直後に、そのバッファを再利用または変更できる。

## 10. 実装開始前の確認事項

実アダプタを作成する前に、次を確認すること。

1. 使用する UART IP と Xilinx ドライバ名
2. 使用する割り込みコントローラと UART 割り込みID
3. BSPの標準出力が `xil_printf` の送信先UARTへ割り当てられていること
4. `xil_printf` の16進フォーマット機能
5. UART受信割り込みだけを無効化・復帰するアダプタ実装が可能であること
6. `T` ハンドラの実関数とコンテキスト
7. レジスタアクセスに `Xil_In32` / `Xil_Out32` を使用するか、直接 `volatile`
   アクセスを使用するか

## 11. 統合時の動作確認

少なくとも以下を実機で確認すること。

1. `S 0000 4d` に対して、対象レジスタが書き換わり
   `=> S 0x00000000 4h\n` が送信される。
2. `L 0004` に対して、読み出し結果が `=> L 0x00000004 <value>h\n` として送信される。
3. `T example` に対して、外部ハンドラが `example\n` を受け取り、
   `=> T example\n` が送信される。
4. LF と CRLF の双方でコマンドが一度だけ処理される。
5. 連続した複数行で、受信キューの順序どおりに処理される。
6. 不正コマンドで原因を識別できるエラーメッセージが送信される。
