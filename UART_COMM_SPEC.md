# UART コマンド通信モジュール仕様書

## 1. 文書情報

| 項目 | 内容 |
| --- | --- |
| 文書名 | UART コマンド通信モジュール仕様書 |
| 版数 | 0.6（ドラフト） |
| 対象 | Xilinx MicroBlaze V システム向け UART コマンド処理コア |
| 言語 | C |
| 対象範囲 | ハードウェア非依存の上位通信モジュール |

本書は、UART から受信した ASCII 文字を受け取り、1 行の入力を構成し、
その行をコマンドとして解釈するコアモジュールを規定する。

コアモジュールは、`stdio`、Xilinx ヘッダ、Xilinx ドライバ、その他の
プラットフォーム固有ライブラリに依存してはならない。UART ハードウェア
アクセスおよび実レジスタへのアクセスは、コールバック関数を介して別の
統合モジュールから提供するものとする。

## 2. 目的

本モジュールは、FPGA 内の MicroBlaze V プロセッサ上で動作するソフトウェアに
コマンドインタフェースを提供する。以下を実現すること。

1. 統合層から ASCII 文字を 1 文字ずつ受信する。
2. 行終端文字を受信するまで文字を保持する。
3. 完成した 1 行をコマンド処理用エントリ関数へ渡す。
4. 行がレジスタコマンドの場合、レジスタの読み出しまたは書き込みを実行する。
5. 行が外部処理コマンドの場合、指定の先頭文字以降の文字列を外部関数へ転送する。

## 3. システム境界およびモジュール分割

```text
UART ハードウェア / Xilinx ドライバ
        |
        | 受信バイト 1 個
        v
プラットフォームアダプタ（Xilinx 依存。本仕様の対象外）
        |
        | uart_comm_receive_char()
        v
UART コマンドコア（本仕様の対象。ハードウェア非依存）
        |------------------------------|
        |                              |
        v                              v
レジスタアクセスコールバック       外部コマンドコールバック
        |                              |
        v                              v
プラットフォーム固有のレジスタ      アプリケーション固有の
アクセス実装                        コマンドハンドラ
```

プラットフォームアダプタは、UART バイトの受信およびコールバック実装の提供を
担当する。アダプタは Xilinx API を使用してよいが、その依存をコアモジュールへ
持ち込んではならない。

## 4. 用語

| 用語 | 定義 |
| --- | --- |
| 文字 | 受信した 8 ビット値 1 個。本仕様では ASCII 入力のみを対象とする。 |
| 行 | 行終端文字より前の入力文字列。行終端文字自体は行に含めない。 |
| コマンド | コマンドプロセッサが解釈する、完結した 1 行の入力。 |
| レジスタコマンド | レジスタの読み出しまたは書き込みを要求するコマンド。 |
| 外部コマンド | 指定された先頭文字により選別され、外部ハンドラへ転送されるコマンド。 |
| コアモジュール | 本仕様で定義する、プラットフォーム非依存のモジュール。 |

## 5. 機能要件

### 5.1 初期化

- コアモジュールは初期化関数を提供すること。
- 初期化関数は受信行の状態をクリアし、呼び出し元が指定したコールバック設定を
  保存すること。
- 初期化関数は、すべての受信バッファを空き状態に設定し、そのうち 1 個を受信中状態に
  設定すること。
- 呼び出し元は、コールバックへ変更せずに引き渡されるコンテキストポインタを
  指定できること。
- コアモジュールは動的メモリ割り当てを行わないこと。

### 5.2 文字受信および行バッファリング

- コアモジュールは、受信 ASCII 文字を 1 文字ずつ受け取るエントリ関数を
  提供すること。
- 通常の入力文字は、現在の受信中バッファへ追加すること。
- 行終端文字を受信した場合、コアモジュールは内部的に行を終端して「行準備完了」の
  状態を返すこと。完結したバッファを準備完了キューへ追加し、空きバッファがあれば
  直ちに次の受信先として使用すること。文字受信関数からコマンド処理用エントリ関数を
  呼び出してはならない。
- メインループは、別のコマンド処理用エントリ関数を呼び出すこと。コマンド処理用
  エントリ関数は、最も古く準備完了となったバッファを 1 個だけ取り出して処理すること。
- コマンド処理用エントリ関数は、行終端文字を含まない行を受け取ること。
- 受信バッファは固定長かつ固定個数とし、容量および個数はコンパイル時に設定可能と
  すること。
- 受信バッファ数は `UART_COMM_RX_BUFFER_COUNT` で指定し、初期値を 2、最小値を 2
  とすること。3 以上を指定した場合も同じ方式で動作すること。
- 受信可能なコマンド行の最大長は、行終端文字を除いて 256 文字とすること。
- コマンド行を C 文字列として扱う内部バッファは、最大入力 256 文字と終端 NUL 文字の
  ため、少なくとも 257 バイト確保すること。
- 設定容量を超える入力行を検出し、バッファ範囲外への書き込みを防止すること。
- 受信中、準備完了、処理中、空きのいずれでもないバッファ状態へ遷移してはならない。
- すべての受信バッファが準備完了または処理中であり、次の受信先が確保できない場合、
  受信側は次の行終端までの入力を破棄すること。この受信キューオーバーフローは、
  メインループ側でエラーメッセージとして報告すること。

### 5.3 コマンドの振り分け

- コマンド処理用エントリ関数は、完成した行をレジスタコマンド、外部コマンド、
  または不正なコマンドに分類すること。
- 行の先頭文字が `S`、`L`、`T` のいずれでもない場合、不正コマンドとして
  エラーメッセージを出力すること。
- レジスタコマンドは解析し、レジスタアクセスコールバックへ振り分けること。
- 外部コマンドは外部コマンドコールバックへ転送すること。
- 外部コマンドコールバックには、先頭の選別文字およびその直後の区切り空白を除いた
  文字列へ改行文字を 1 個付加して渡すこと。
- 外部コマンドコールバックが成功した場合、コアモジュールは転送した本文を用いて
  エコーバックの成功メッセージを出力すること。
- 外部コマンドコールバックへ渡す文字列の有効期間は、そのコールバック呼び出し中に
  限定すること。転送先が呼び出し終了後も文字列を保持する必要がある場合、転送先の
  責務で文字列をコピーすること。コアモジュールは、コールバックから復帰した後に
  当該バッファを自由に変更または再利用してよい。
- コアモジュールは、構文エラー、未対応コマンド、入力オーバーフロー、
  コールバック失敗を定義済みの結果通知機構で報告すること。

### 5.4 数値変換

- アドレスおよび書き込みデータの文字列から整数値への変換は、コアモジュール内で
  自前実装すること。
- `strtol`、`strtoul`、`sscanf` など、標準ライブラリの文字列変換関数を使用しては
  ならない。
- 数値文字列に対応する整数型の表現範囲を超える値はエラーとすること。
- アドレスおよびレジスタ値は、コアモジュールで定義する 32 ビット符号なし整数型
  `uint32` の表現範囲（`0x00000000`～`0xFFFFFFFF`）のみを扱うこと。

### 5.5 レジスタアクセス

- レジスタ読み出しコマンドは、コマンドで指定されたアドレスの値を要求すること。
- レジスタ書き込みコマンドは、指定アドレスへ指定データを書き込むことを要求すること。
- コマンドパーサは、コマンドで与えられたアドレスを直接デリファレンスしてはならない。
  レジスタアクセスは統合層から指定されたコールバックを介してのみ行うこと。
- 構文として有効な `uint32` アドレスに対して、アドレス範囲、アラインメント、
  読み書き権限の事前検査は行わないこと。解析後の値をそのまま該当するレジスタ
  アクセスコールバックへ渡すこと。

### 5.6 応答処理

- コアモジュールは、成功およびエラーを含むすべての処理結果について、呼び出し元
  および／またはアプリケーション提供の出力コールバックへ通知すること。
- UART 上の応答文字列の整形および送信は、パーサおよびレジスタアクセスロジックから
  分離すること。
- 応答コールバックを使用する場合、その実装はプラットフォーム依存であってよい。
  ただし、コアモジュールはコールバックを呼び出すのみとし、UART ドライバ API を
  直接使用してはならない。
- 数値からメッセージ文字列への変換および UART への出力は、成功・エラーを問わず
  出力ラッパー関数が担当すること。最終版の Xilinx 環境用ラッパー実装では
  `xil_printf` を使用すること。
- コアモジュールは `xil_printf` を直接呼び出してはならない。PC 上のテスト用
  ラッパー実装では、同一のラッパーインタフェースの内部で `printf` を使用すること。
- レジスタ書き込み成功時のメッセージは、`=> S 0x<address> <value>h\n` とすること。
- レジスタ読み出し成功時のメッセージは、`=> L 0x<address> <value>h\n` とすること。
- 上記メッセージの `address` は小文字の 16 進数8桁、`value` は先行ゼロを省略した
  小文字の16進数とする。入力時の10進／16進表記および桁数には依存しないこと。
- エラーメッセージは、エラー原因が判別可能な文面とし、末尾に `\n` を付加すること。
- 外部コマンド転送成功時のメッセージは、`=> T <transfer-text>\n` とすること。
  `transfer-text` は外部コマンドコールバックへ渡した本文から末尾の `LF` を除いた
  文字列とすること。

## 6. 公開 C インタフェース案（ドラフト）

以下のインタフェースは依存関係の境界を示すものである。関数名および正確な型は
レビュー対象とするが、コアモジュールではコールバックによる分離方式を維持すること。

```c
/* This project requires unsigned int to be 32 bits. */
typedef unsigned int uint32;

#define UART_COMM_MAX_LINE_LENGTH  (256U)
#define UART_COMM_LINE_BUFFER_SIZE (UART_COMM_MAX_LINE_LENGTH + 1U)
#define UART_COMM_RX_BUFFER_COUNT  (2U)

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

typedef enum {
    UART_COMM_RECEIVE_IN_PROGRESS = 0,
    UART_COMM_RECEIVE_LINE_READY
} uart_comm_receive_state_t;

typedef enum {
    UART_COMM_BUFFER_FREE = 0,
    UART_COMM_BUFFER_RECEIVING,
    UART_COMM_BUFFER_READY,
    UART_COMM_BUFFER_PROCESSING
} uart_comm_buffer_state_t;

typedef uart_comm_result_t (*uart_comm_register_read_fn)(
    void *context,
    uint32 address,
    uint32 *value);

typedef uart_comm_result_t (*uart_comm_register_write_fn)(
    void *context,
    uint32 address,
    uint32 value);

typedef uart_comm_result_t (*uart_comm_external_command_fn)(
    void *context,
    const char *text,
    uint32 length);

typedef void (*uart_comm_report_set_fn)(
    void *context,
    uint32 address,
    uint32 value);

typedef void (*uart_comm_report_read_fn)(
    void *context,
    uint32 address,
    uint32 value);

typedef void (*uart_comm_report_transfer_fn)(
    void *context,
    const char *text,
    uint32 length);

typedef void (*uart_comm_report_error_fn)(
    void *context,
    uart_comm_result_t result);

typedef void (*uart_comm_critical_section_fn)(
    void *context);

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

void uart_comm_init(const uart_comm_config_t *config);
uart_comm_receive_state_t uart_comm_receive_char(char character);
unsigned char uart_comm_has_pending_line(void);
uart_comm_result_t uart_comm_process_received_line(void);
uart_comm_result_t uart_comm_process_line(const char *line,
                                          uint32 length);
```

ここで使用する `uint32` は、標準ライブラリの `stdint.h` には依存しない、
コアモジュールのヘッダ内で定義する 32 ビット符号なし整数型とする。対象環境および
PC テスト環境では、`unsigned int` が 32 ビットであることを前提とする。

`UART_COMM_RX_BUFFER_COUNT` は 2 以上のコンパイル時定数とする。初期値 2 の場合、
入力行の保持用メモリは `UART_COMM_LINE_BUFFER_SIZE * UART_COMM_RX_BUFFER_COUNT`、
すなわち `257 * 2 = 514` バイトである。

`enter_critical` および `exit_critical` は、メインループが準備完了バッファを
確保または解放する際の短い排他区間にだけ使用する。Xilinx 環境では、プラットフォーム
アダプタが UART 受信割り込みを一時的に無効化／復帰する実装を提供してよい。コア
モジュールは、割り込み制御 API を直接呼び出してはならない。PC テスト用実装では、
両コールバックを何もしない関数としてよい。

上記の `report_*` 関数は出力ラッパーである。Xilinx 用実装は固定書式の
`xil_printf` 呼び出しを、PC テスト用実装は対応する `printf` 呼び出しを行う。
成功メッセージとエラーメッセージは、すべてこのラッパー経由で出力する。
コアモジュールは、メッセージ文字列の書式化および出力を行わない。

この宣言は例示である。実装仕様では、複数の独立したコアモジュールインスタンスを
必要とする場合のインスタンス所有方法を確定すること。

## 7. 入力処理規則

| 項目 | 規則 |
| --- | --- |
| 文字エンコーディング | ASCII |
| 入力単位 | 受信関数 1 回の呼び出しにつき 1 文字 |
| 行終端 | `LF`（`0x0A`）および `CRLF`（`0x0D 0x0A`）を受け付ける。 `CR` 単体は行終端として受け付けない。 |
| 終端文字の格納 | パーサへ渡すコマンド行には格納しない。 |
| `CRLF` の処理 | `LF` を受信した時点で行を完結する。直前の文字が `CR` の場合、その `CR` を行バッファから除外する。 |
| 受信・解釈の分離 | `uart_comm_receive_char()` はバッファリングと行完結の検出のみを行う。メインループは `uart_comm_has_pending_line()` を確認し、真の場合に `uart_comm_process_received_line()` を呼び出して解釈・出力を行う。 |
| 空行 | `UART_COMM_RESULT_EMPTY_LINE` を報告し、レジスタおよび外部コールバックを呼び出さない。 |
| NUL 入力 | 行内で NUL（`0x00`）を受信した場合、その行を不正文字入力として行終端まで破棄し、`UART_COMM_RESULT_INVALID_CHARACTER` を報告する。 |
| 最大行長 | 行終端文字を除いて 256 文字。 |
| 内部行バッファ | 1 面につき入力 256 文字と終端 NUL 文字のため、257 バイト。既定の2面構成では514バイト。 |
| 受信バッファ数 | `UART_COMM_RX_BUFFER_COUNT`。初期値2、最小値2。 |
| アドレス・データ型 | 32 ビット符号なし整数型 `uint32`。 |
| バッファオーバーフロー | 行全体を実行せず、行終端時に破棄して `UART_COMM_RESULT_LINE_OVERFLOW` を報告する。 |
| 区切り空白 | ASCII の SP（`0x20`）または HT（`0x09`）が 1 文字以上連続したもの。 |
| 先頭空白 | 許容しない。1 文字目はコマンド種別文字でなければならない。 |

### 7.1 受信バッファ管理

各受信バッファは、以下のいずれか 1 つの状態を持つ。

| 状態 | 所有者 | 意味 |
| --- | --- | --- |
| `FREE` | 受信側 | 空きバッファ。次の受信先として選択可能。 |
| `RECEIVING` | UART 割り込み側 | 現在受信中の行を格納しているバッファ。 |
| `READY` | メインループ側 | 行終端を受信済みで、解釈待ちのバッファ。 |
| `PROCESSING` | メインループ側 | コマンドの解釈、出力、または外部コールバックの実行中のバッファ。 |

UART 割り込み側は `RECEIVING` 状態のバッファだけに書き込み、メインループ側は
`READY` 状態のバッファだけを `PROCESSING` へ遷移させること。これにより、
割り込み側とメインループ側が同一バッファの内容へ同時にアクセスしない。

準備完了バッファは完結順に FIFO として処理すること。メインループは
`uart_comm_process_received_line()` を繰り返し呼び出すことで、保留中の行を
1 行ずつ処理できる。保留行がない場合、同関数は `UART_COMM_RESULT_NO_LINE_READY` を
返すこと。

すべてのバッファが `READY` または `PROCESSING` となった場合、割り込み側は
次の行終端まで新しい行を破棄する。破棄中に空きバッファが再び現れた場合も、
その行終端を受信するまで受信を再開してはならない。破棄開始時に
`UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW` の報告を保留し、メインループが
`report_error` 出力ラッパーを介してエラーメッセージを 1 回出力すること。
`uart_comm_has_pending_line()` は、準備完了バッファまたは保留中の受信キュー
オーバーフロー報告がある場合に真を返すこと。保留中の受信キューオーバーフロー報告を
処理する場合、`uart_comm_process_received_line()` は
`UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW` を返すこと。

## 8. コマンド文法

### 8.1 共通規則

- コマンド種別は行の先頭 1 文字で識別する。小文字は受け付けない。
- `S`、`L` では、コマンド種別と各引数を区切り空白で区切ること。
- `S`、`L` における引数末尾の区切り空白は許容する。
- 3 個以上の引数、または規定外の文字列を含む `S`、`L` コマンドは構文エラーとする。

### 8.2 形式文法

```text
line              = set-command / load-command / transfer-command
set-command       = "S" separator address separator value [separator]
load-command      = "L" separator address [separator ignored-argument] [separator]
transfer-command  = "T" separator transfer-text

separator         = 1*(SP / HT)
address           = ["0x" / "0X"] 1*HEXDIG
value             = decimal-value / hexadecimal-value
decimal-value     = 1*DIGIT "d"
hexadecimal-value = 1*HEXDIG "h"
ignored-argument  = 1*VCHAR
transfer-text     = 1*(ASCII-character except CR and LF)
```

### 8.3 `S`：レジスタ値設定

`S` はレジスタへ値を書き込むコマンドであり、アドレスと値の 2 引数を必須とする。

```text
S <address> <value>
```

例：`S 0x0000 1d` はアドレス `0x0000` へ 10 進数の `1` を書き込む。
書き込みが成功した場合、コアモジュールは `report_set` 出力ラッパーを呼び出して
設定完了メッセージの出力を要求する。

### 8.4 `L`：レジスタ値読み出し

`L` はレジスタから値を読み出すコマンドであり、アドレス引数を必須とする。

```text
L <address> [ignored-argument]
```

例：`L 0x0004` はアドレス `0x0004` の値を読み出す。読み出しが成功した場合、
コアモジュールは `report_read` 出力ラッパーを呼び出して、読み出した値のメッセージ
出力を要求する。

2 番目の引数が指定された場合、その内容は解釈せず無視する。ただし、引数は 1 個の
トークンでなければならず、3 番目以降の引数は構文エラーとする。

### 8.5 `T`：外部モジュールへのコマンド転送

`T` は他モジュールへコマンド文字列を転送するコマンドである。

```text
T <transfer-text>
```

`T` の直後には 1 文字以上の区切り空白を必要とする。区切り空白の直後から行終端の
直前までを `transfer-text` として切り出す。切り出した文字列の末尾に改行文字
`LF`（`0x0A`）を 1 個付加し、NUL 終端した文字列を外部コマンドコールバックへ
渡す。転送先へ渡す長さは、付加した `LF` を含み、終端 NUL 文字を含まない。
`transfer-text` が空の場合はエラーとし、外部コマンドコールバックを呼び出しては
ならない。

`T` の本文では、印字可能 ASCII 文字（`0x20`～`0x7E`）および空白制御文字である
HT（`0x09`）、VT（`0x0B`）、FF（`0x0C`）を許可する。LF および CRLF は行終端として
処理する。NUL は不正文字入力として行全体をエラーとする。CRLF を構成しない CR、および
それ以外の制御文字（`0x01`～`0x08`、`0x0E`～`0x1F`、`0x7F`）は本文から除外し、
外部関数への転送およびエコーバックメッセージへ含めない。

例：`T any string and different command` を受信した場合、外部コマンドコールバックへ
`any string and different command\n` を渡す。

転送用文字列は、最大長の入力行であっても 256 バイトのバッファに格納できる。
これは、`T` と必須の区切り空白が入力行の先頭 2 文字を占めるため、転送本文の
最大長が 254 文字となり、付加する `LF` と終端 NUL 文字を合わせて最大 256 バイト
となるためである。

### 8.6 アドレスおよび値の表記

- アドレスは 16 進数の整数とする。先頭の `0x` または `0X` は省略可能とする。
- アドレスには 16 進数字（`0`–`9`、`A`–`F`、`a`–`f`）のみを使用できる。
  `0x`／`0X` を除く数値部は最大 8 文字とし、9 文字以上はエラーとする。
- `S` の値は、末尾が `d` のとき 10 進数、末尾が `h` のとき 16 進数とする。
- 値には末尾の `d` または `h` が必須である。接尾辞がない値はエラーとする。
- 16 進数値の数値部は最大 8 文字とする。数値部が 9 文字以上の 16 進数値はエラーと
  する。文字数の判定に接尾辞 `h` は含めない。
- 10 進数値の数値部には `0`–`9` のみを使用できる。負号（`-`）を含む10進数値は
  エラーとする。10進数値は、文字数ではなく `uint32` の最大値 `4294967295` 以下か
  どうかで受け付け可否を判定する。
- 16 進数値の数値部には 16 進数字（`0`–`9`、`A`–`F`、`a`–`f`）のみを
  使用できる。英字の大文字・小文字は同じ値として扱う。値の数値部に
  `0x`／`0X` プレフィックスは使用できない。
- 接尾辞 `D`、`H` のような大文字は受け付けない。

### 8.7 コマンド例

| 入力行 | 解釈および動作 |
| --- | --- |
| `S 0x0000 1d` | アドレス `0x0000` へ 10 進数の `1` を書き込み、設定完了メッセージを出力する。 |
| `S 0004 FFh` | アドレス `0x0004` へ 16 進数の `0xFF` を書き込み、設定完了メッセージを出力する。 |
| `L 0x0004` | アドレス `0x0004` を読み出し、取得値のメッセージを出力する。 |
| `L 0004 ignored` | アドレス `0x0004` を読み出す。第 2 引数 `ignored` は無視する。 |
| `T any string and different command` | `any string and different command\n` を外部コマンドコールバックへ転送する。 |
| `S 0000 1` | 値に `d` または `h` の接尾辞がないため、エラーを出力する。 |
| `S 0000 -1d` | 負の 10 進数値のため、エラーを出力する。 |
| `S 0000 FFFFFFFFh` | アドレス `0x0000` へ最大の `uint32` 値 `0xFFFFFFFF` を書き込む。 |
| `S 0000 FFFFFFFFFh` | 値の数値部が 9 文字であるため、エラーを出力する。 |
| `S 0000 4294967295d` | アドレス `0x0000` へ最大の `uint32` 値 `0xFFFFFFFF` を書き込む。 |
| `S 0000 4294967296d` | `uint32` の表現範囲を超えるため、エラーを出力する。 |
| `L 0004 extra another` | 第 3 引数があるため、構文エラーを出力する。 |
| `T any string` | 外部コマンドコールバックへ `any string\n` を渡り、成功時に `=> T any string\n` を出力する。 |
| `T ` | 転送文字列が空のため、エラーを出力し、外部コマンドコールバックを呼び出さない。 |

## 9. エラー処理要件

| 条件 | 必須動作 |
| --- | --- |
| 行がバッファ容量を超過 | 行の一部も実行せず、行終端で破棄してオーバーフローを報告する。 |
| 受信バッファがすべて使用中 | 次の行終端まで入力を破棄し、メインループから `UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW` のエラーメッセージを 1 回出力する。 |
| 未定義の先頭文字（`S`、`L`、`T` 以外） | レジスタアクセスおよび外部コマンド転送を行わず、出力ラッパー経由で不正コマンドのエラーメッセージを出力する。 |
| 未知のコマンド | レジスタアクセスを行わず、出力ラッパー経由で不正コマンドのエラーメッセージを出力する。 |
| 数値フィールドの形式不正 | レジスタアクセスを行わず、不正な構文または数値フィールドを報告する。 |
| アドレスの数値部が 9 文字以上 | レジスタアクセスを行わず、不正なアドレスとしてエラーメッセージを出力する。 |
| 16進レジスタ値の数値部が 9 文字以上 | レジスタアクセスを行わず、不正な数値フィールドとしてエラーメッセージを出力する。 |
| 数値の表現範囲超過 | レジスタアクセスを行わず、不正な数値フィールドを報告する。 |
| レジスタコールバック未設定 | レジスタアクセスを行わず、適切な設定エラーまたはアクセスエラーを報告する。 |
| レジスタコールバック失敗 | レジスタアクセスエラーとして伝播する。 |
| 外部コールバック未設定 | 文字列を転送せず、不正コマンドまたは設定エラーを報告する。 |
| 外部コールバック失敗 | 外部ハンドラエラーとして伝播する。 |
| `T` の転送文字列が空 | 外部コマンドコールバックを呼び出さず、エラーメッセージを出力する。 |

### 9.1 結果コードの割り当て

| 条件 | 結果コード | エラーメッセージ |
| --- | --- | --- |
| 処理対象の行がない | `UART_COMM_RESULT_NO_LINE_READY` | 出力しない。 |
| 空行 | `UART_COMM_RESULT_EMPTY_LINE` | 出力しない。 |
| 行長超過 | `UART_COMM_RESULT_LINE_OVERFLOW` | 出力する。 |
| 受信キューオーバーフロー | `UART_COMM_RESULT_RECEIVE_QUEUE_OVERFLOW` | 出力する。 |
| NUL を含む行 | `UART_COMM_RESULT_INVALID_CHARACTER` | 出力する。 |
| 先頭文字が `S`、`L`、`T` 以外 | `UART_COMM_RESULT_INVALID_COMMAND` | 出力する。 |
| 必須アドレス、書き込み値、または `T` 本文の欠落 | `UART_COMM_RESULT_MISSING_ARGUMENT` | 出力する。 |
| `S` または `L` の引数が多すぎる | `UART_COMM_RESULT_TOO_MANY_ARGUMENTS` | 出力する。 |
| アドレスの形式不正、または16進数部が9文字以上 | `UART_COMM_RESULT_INVALID_ADDRESS` | 出力する。 |
| 値の接尾辞欠落、値の形式不正、負値、16進数部9文字以上、または `uint32` 範囲超過 | `UART_COMM_RESULT_INVALID_DATA` | 出力する。 |
| 上記以外の `S`／`L` 文法違反 | `UART_COMM_RESULT_INVALID_SYNTAX` | 出力する。 |
| 必要なコールバックまたは排他コールバックが未設定 | `UART_COMM_RESULT_CONFIGURATION_ERROR` | 出力する。 |
| レジスタアクセスコールバックの失敗 | `UART_COMM_RESULT_REGISTER_ACCESS_ERROR` | 出力する。 |
| 外部コマンドコールバックの失敗 | `UART_COMM_RESULT_EXTERNAL_HANDLER_ERROR` | 出力する。 |

## 10. 非機能要件

- コアモジュールは C 言語で記述し、動的メモリ割り当てを使用しないこと。
- コアモジュールは `stdio` および Xilinx ヘッダファイルをインクルードしないこと。
- コアモジュールは Xilinx API を呼び出さず、UART ハードウェアにも直接アクセスしないこと。
- コアモジュールは決定的かつ上限のあるメモリ使用量とすること。
- 入力解析はバッファ境界を検査して行うこと。
- ソースコード中のコメントは英語で記述すること。
- `uart_comm_receive_char()` は UART 受信割り込みハンドラから呼び出してよい。
  同関数は受信バッファへの格納、行終端の検出、バッファ状態の遷移だけを行い、
  文字列解析、メッセージ出力、レジスタアクセス、外部コマンドコールバックを
  実行してはならない。
- `uart_comm_has_pending_line()` および `uart_comm_process_received_line()` は、
  メインループまたは同等の通常コンテキストから呼び出すこと。
- 割り込み側とメインループ側で共有するバッファ状態および FIFO 管理情報は
  `volatile` な内部グローバル変数とすること。メインループ側で状態を取得または
  解放する最小区間は、出力ラッパーを含まない排他区間とすること。
- 本モジュールは単一インスタンス仕様とする。複数 UART チャネルで使用する場合は、
  インスタンス状態を外部から与える方式へ設計を拡張すること。

## 11. 確認が必要な事項

現時点で、実装開始を妨げる未確定事項はない。

## 12. 次版での作業

次版では、割り込み側とメインループ側の同時動作を含むテスト項目を定義する。
