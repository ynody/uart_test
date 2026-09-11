# SEM IP 手動試験：UARTメッセージ一覧

対象は KCU105（Kintex UltraScale）上の SEM IP である。SEM IP は **Mitigation and Testing**、訂正有効として生成し、UART通信路は正常動作を前提とする。

SEM IP に送るコマンドはすべて大文字で入力し、各行末に **CR（`\r`）** を付加する。端末上で `I>` または `O>` に続いてコマンドが見える場合、コマンド文字列は端末のエコーによる場合がある。合否は SEM IP が返す `SC`、`ECC`、`COR`、`FC`、および最終プロンプトで判定する。

## 共通記法

```text
# XCKU040、SLR=0、UltraScale用の注入アドレス
# A(F, W, B) = C000000000 + F × 1000 + W × 20 + B
#
# F: LFA（0 ～ MF-2）
# W: Frame内Word番号（0 ～ 122）
# B: Word内Bit番号（0 ～ 31）
#
# 基本注入対象：Frame 0、Word 61、Bit 0
A0 = C0000007A0

# Frame 0のQueryアドレス
Q0 = C000000000

# Frame 1、Word 61、Bit 0
A1 = C0000017A0

# QueryではWord/Bitフィールドを無視し、Frame全体を返す。
```

| 受信メッセージ | 意味 |
|---|---|
| `SC 00` | Idle |
| `SC 01` | Initialization |
| `SC 02` | Observation |
| `SC 04` | Correction |
| `SC 08` | Classification |
| `SC 10` | Injection |
| `FC 00` / `FC 40` | Correctable。後者はEssential |
| `FC 20` / `FC 60` | Uncorrectable。後者はEssential |

## 1. SEM初期化確認

### SEM IPへの入力

```text
入力なし（FPGA ConfigurationまたはSEMのリセットにより開始）
```

### 期待受信メッセージ

```text
SEM_ULTRA_Vx_x
SC 01
FS xx
AF xx
ICAP OK
RDBK OK
INIT OK
SC 02
O>
```

`FS`、`AF`、バージョン文字列は生成したIP設定に依存する。`ICAP OK`、`RDBK OK`、`INIT OK`、`SC 02`、`O>` を確認する。

## 2. Status Report

### SEM IPへの入力

```text
S
```

### 期待受信メッセージ

```text
SN 00
SC 02
FC xx
RI xx
MF 0000xxxx
TS xxxxxxxx
TB xxxxxxxx
CB xxxxxxxx
CL xxx
O>
```

初期状態では `SC 02` を確認する。`MF` は最大LFAであり、以降の注入対象Frameは `0 ～ MF-2` の範囲から選ぶ。

## 3. Observation → Idle

### SEM IPへの入力

```text
I
```

### 期待受信メッセージ

```text
SC 00
I>
```

## 4. Idle → Observation

### SEM IPへの入力

```text
O
```

### 期待受信メッセージ

```text
SC 02
O>
```

エラーを残していない状態では、この直後に追加のエラーレポートは出ない。

## 5. Frame Query

### SEM IPへの入力

```text
I
Q C000000000
```

### 期待受信メッセージ

```text
SC 00
I>

00000000  # Word 0
........
xxxxxxxx  # Word 61
........
xxxxxxxx  # Word 122
I>
```

UltraScaleでは、`Q` に対してWord 0からWord 122まで、123行の32-bit値が返る。実装済みデザインでは全行がゼロとは限らない。Word 61の値を記録する。

## 6. ECC領域への1-bit注入

Frame 0、Word 61、Bit 0を例にする。実行前に `I>` であることを確認する。

### SEM IPへの入力

```text
N C0000007A0
```

### 期待受信メッセージ

```text
SC 10
SC 00
I>
```

`SC 10` はInjectionへの遷移、`SC 00` はIdleへの復帰である。

## 7. 注入前後のQuery比較

No. 5の注入前ログと比較する。

### SEM IPへの入力

```text
Q C000000000
```

### 期待受信メッセージ

```text
xxxxxxxx  # Word 0 ～ Word 60：注入前と一致
00000001  # Word 61：注入前の値に対してBit 0のみ反転
xxxxxxxx  # Word 62 ～ Word 122：注入前と一致
I>
```

Word 61の元の値がゼロの場合は `00000000 → 00000001` となる。元の値がゼロでない場合は、元の値とのXORが `00000001` であることを確認する。

## 8. 1-bitエラー検出

No. 6で注入済みの状態から実施する。

### SEM IPへの入力

```text
O
```

### 期待受信メッセージ

```text
SC 02
O>
RI xx
SC 04
ECC
TS xxxxxxxx
PA xxxxxxxx
LA 00000000
COR
WD 3D BT 00
END
FC 00 または FC 40
SC 08
FC 00 または FC 40
SC 02
O>
```

`WD 3D BT 00` はWord 61（16進数で`3D`）、Bit 0を示す。`LA` は注入したLFA 0と一致することを確認する。`PA` は同じFrameの物理アドレスである。

## 9. 1-bit訂正

No. 8のレポート内の訂正部を確認する。

### SEM IPへの入力

```text
入力なし
```

### 期待受信メッセージ

```text
COR
WD 3D BT 00
END
FC 00 または FC 40
SC 02
O>
```

`COR` と `END` の間に `WD 3D BT 00` が存在し、最終的に `SC 02` と `O>` へ復帰することを確認する。

## 10. 訂正後Query

### SEM IPへの入力

```text
I
Q C000000000
```

### 期待受信メッセージ

```text
SC 00
I>

xxxxxxxx  # Word 0 ～ Word 60：注入前と一致
00000000  # Word 61：注入前の値へ復元
xxxxxxxx  # Word 62 ～ Word 122：注入前と一致
I>
```

Word 61は、No. 5で取得した注入前の値と完全一致することが合格条件である。

## 11. Correction Report

No. 8～10で得た以下のブロックを、一つの訂正レポートとして保存・照合する。

### SEM IPへの入力

```text
O
```

### 期待受信メッセージ

```text
RI xx
SC 04
ECC
TS xxxxxxxx
PA xxxxxxxx
LA 00000000
COR
WD 3D BT 00
END
FC 00 または FC 40
SC 08
FC 00 または FC 40
SC 02
O>
```

`LA`＝注入Frame、`WD`＝注入Word、`BT`＝注入Bitの対応を確認する。`TS` はイベント時刻として記録する。

## 12. Injection時の状態遷移

### SEM IPへの入力

```text
I
N C0000007A0
```

### 期待受信メッセージ

```text
SC 00
I>
SC 10
SC 00
I>
```

状態列が `Idle → Injection → Idle` であることを確認する。

## 13. 検出・訂正時の状態遷移

### SEM IPへの入力

```text
O
```

### 期待受信メッセージ

```text
SC 02
O>
RI xx
SC 04
ECC
...
COR
WD 3D BT 00
END
FC xx
SC 08
FC xx
SC 02
O>
```

状態列は `Observation → Correction → Classification → Observation` である。分類機能を有効にした構成では、`SC 08` と `SC 02` の間に以下の分類レポートが追加される。

```text
CLA
WD xx BT xx LV xx
END
```

## 14. 複数Frameでの1-bit注入

Frame 0以外の、Queryで読出し可能なFrameを選ぶ。例ではFrame 1を使う。

### SEM IPへの入力

```text
# 注入前確認
I
Q C000001000

# Frame 1、Word 61、Bit 0へ注入
N C0000017A0

# 注入確認
Q C000001000

# 検出・訂正
O
```

### 期待受信メッセージ

```text
# 注入
SC 10
SC 00
I>

# 注入後QueryのWord 61
00000001  # 注入前との差分がBit 0のみ

# 検出・訂正
SC 02
O>
RI xx
SC 04
ECC
TS xxxxxxxx
PA xxxxxxxx
LA 00000001
COR
WD 3D BT 00
END
FC 00 または FC 40
SC 08
FC xx
SC 02
O>
```

Frame 1がmaskedまたは未実装で注入が反映されない場合は、`Q` の前後差分が生じない。その場合は別の有効Frameへ変更する。

## 15. 同一Frameへの2-bit注入

この試験は訂正不能になる可能性があるため、実施後にNo. 17の再Configurationを行う。Frame 0、Word 61、Bit 0およびBit 1へ注入する例である。

### SEM IPへの入力

```text
I
Q C000000000

N C0000007A0
N C0000007A1

Q C000000000
```

2つ目の `N` は、最初の `N` に対する `SC 00` と `I>` を確認してから送る。

### 期待受信メッセージ

```text
# 1回目の注入
SC 10
SC 00
I>

# 2回目の注入
SC 10
SC 00
I>

# 注入後QueryのWord 61
00000003
I>
```

注入前のWord 61がゼロの場合、Bit 0とBit 1が反転して `00000003` となる。注入前の値との差分が `00000003` であることを確認する。

## 16. Uncorrectable発生

No. 15の2-bit注入後、Observationへ戻す。

### SEM IPへの入力

```text
O
```

### 期待受信メッセージ

```text
SC 02
O>
RI xx
SC 04
ECC
TS xxxxxxxx
PA xxxxxxxx
LA 00000000
COR
END
FC 20 または FC 60
SC 08
FC 20 または FC 60
SC 00
I>
```

訂正可能時と異なり、`COR` と `END` の間に `WD ... BT ...` の訂正リストがない。最終状態が `SC 00`、`I>` となり、Uncorrectableフラグが立つことを確認する。

## 17. Uncorrectable後の復旧

### SEM IPへの入力

```text
入力なし
```

FPGAをJTAGまたは使用中のConfiguration手段で再Configurationする。

### 期待受信メッセージ

```text
SEM_ULTRA_Vx_x
SC 01
FS xx
AF xx
ICAP OK
RDBK OK
INIT OK
SC 02
O>
```

続けて状態を確認する。

### SEM IPへの入力

```text
S
```

### 期待受信メッセージ

```text
SN 00
SC 02
FC 00
...
O>
```

Uncorrectable状態を残したまま、次の注入試験を続けない。

## 18. SEM Software Reset

開始状態をIdleにする。

### SEM IPへの入力

```text
I
R 00
```

`R` の後ろの2桁はdon't careだが、試験手順では `00` に固定する。

### 期待受信メッセージ

```text
SC 00
I>

SEM_ULTRA_Vx_x
SC 01
FS xx
AF xx
ICAP OK
RDBK OK
INIT OK
SC 02
O>
```

ソフトウェアリセット後、初期化レポートが再度出力され、Observationへ戻ることを確認する。

## 19. FPGA再Configuration

### SEM IPへの入力

```text
入力なし
```

FPGAを再Configurationする。

### 期待受信メッセージ

```text
SEM_ULTRA_Vx_x
SC 01
FS xx
AF xx
ICAP OK
RDBK OK
INIT OK
SC 02
O>
```

続けて状態を確認する。

### SEM IPへの入力

```text
S
```

### 期待受信メッセージ

```text
SN 00
SC 02
FC 00
RI xx
MF 0000xxxx
TS xxxxxxxx
TB xxxxxxxx
CB xxxxxxxx
CL xxx
O>
```

## 参照

- [PG187: UART Interface Commands](https://docs.amd.com/r/en-US/pg187-ultrascale-sem/UART-Interface-Commands)
- [PG187: Demonstration on Communicating with the SEM Controller through the UART Interface](https://docs.amd.com/r/en-US/pg187-ultrascale-sem/Demonstration-on-Communicating-with-the-SEM-Controller-through-the-UART-Interface)
- [PG187: Error Correction Report – Mitigation Modes Only](https://docs.amd.com/r/en-US/pg187-ultrascale-sem/Error-Correction-Report-Mitigation-Modes-Only)
