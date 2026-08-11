# ThreadX Shell — Codex 向けプロジェクト指示

マルチボード対応の **Eclipse ThreadX + シェルコンソール** ファームウェア。
`stm32f746g-disco` と `wio-lite-ai` の shell 実装を統合し、1 つの shell コアで複数ボードを
サポートする。ST 公式 HAL、CMake + Ninja、ARM GNU ツールチェーン。

このファイルは Codex（`/codex:review` の内蔵レビュアーを含む）が**毎回読む前提の要約**。
人間向けの詳細は `CLAUDE.md` にある。**レビュー時はここの不変条件を最優先の判定基準にする。**

## 対応ボード

| ボード | MCU | クロック | コンソール | 書込 |
|---|---|---|---|---|
| STM32F746G-DISCO | STM32F746NGH6 (M7, 216 MHz 自前設定) | VCP: USART1 PA9/PB7 115200 | ST-Link |
| Wio Lite AI | STM32H725AEI6 (M7, 550 MHz **DFU boot から継承**) | USB CDC (OTG_HS FS / TinyUSB, `0483:5740`) | **DFU のみ** |

## [!] 不変条件（違反はそれだけで BLOCKING）

1. **レイヤリング**: 一方向依存 **HAL/CMSIS/ThreadX（`lib/`）← port（ボード別）← shell ← app**。
   shell コアはボード非依存 — `#ifdef <BOARD>` やペリフェラル直叩きを shell の core/cmds に
   入れない。ボード差は transport 抽象（`struct cli_transport_api`）と port 側グルーで吸収する。
   **`boot/`（Wio の DFU ブートローダ）は独立ツリー**で、app / shell とソースを共有しない。

2. **共有コアに触れる変更は全対応ボードで成立すること。** 片方のボードだけを見て LGTM しない。

3. **upstream submodule（`lib/` 配下）は read-only。** HAL / CMSIS / ThreadX 系 / TinyUSB /
   CoreMark ほか。編集は不可、調整は port 側で。

4. **shell は静的割当（ヒープ非使用）。** スタックサイズ・優先度は `cli_config.h`、
   `_Static_assert` を通すこと。

5. **Wio Lite AI: app はクロックツリーを再設定しない。**
   DFU ブートローダ（本リポジトリの `boot/`）が構成した 550 MHz / PLL3Q 48 MHz USB /
   FLASH latency 3 を継承する。app が RCC/PLL/FLASH ACR/PWR を書き換えると全部壊れる
   （HSI 64 MHz に落ちるのに latency は 550 MHz 用のまま）。`SystemInit` は
   **FPU + VTOR + ITCM ロードのみ**。VTOR はリンカの `g_pfnVectors` から取る（ハードコード不可）。

6. **Wio Lite AI: `boot/` と `ldscript/STM32H725AEIx_ROM.ld` は不変。**
   内蔵 Flash セクタ0 `0x08000000`（128KB）に DFU ブートローダが常駐する。ここを焼き直す
   操作はブリック本番で、**現存する実機は 1 枚しかない**（board #1 は恒久文鎮化済み）。
   **`boot/iflash.c` の書込先セクタ範囲チェックはセクタ0 を守る唯一の砦**で、緩める変更は不可。
   app は `0x08020000`（セクタ1-3, 384KB）から実行し、書込は DFU 経由のみ。
   **書換え耐久 ~10k サイクル** — 自動ループで焼く提案は不可。DFU フォールバック
   （erased/invalid app は必ず DFU モードに入る）を app 側から壊す変更も不可。
   オプションバイト / RDP / DBGMCU / SWD 端子（PA13/PA14）に触れる提案も不可。

7. **Wio Lite AI: RAM 配置ポリシー**: AXI-SRAM（320KB @ 0x24000000）= バスマスタから見える
   必要があるものだけ（**DMA が届く唯一の RAM**）/ DTCM（128KB @ 0x20000000）= CPU 専用 /
   ITCM = ISR コード。**DMA1/DMA2・SDMMC1 IDMA は TCM に届かない**（RM0468
   §2.1.2/§2.1.5/§2.1.6）。**DTCM の DMA バッファは fault せず無言で転送されない。**

8. **リンカスクリプトの `ASSERT` は LTO 下で空振りする。** 配置保証はポストリンクの
   residency チェックスクリプトで行う。配置を変える変更はこのゲートを維持すること。

9. **ビルドは `_ref/` を読まない。** `_ref/`（および `../*/_ref/`）は git 管理外の資料置き場。
   CMake / スクリプトが参照するとクローンしただけでは configure できなくなる。
   C コード中の言及は出典コメントのみ可。

## ThreadX 統合（全ボード共通）

- **SysTick > PendSV**（優先度）。同一だと idle 時 PendSV スピンを tick が割り込めず tick 停止 →
  デッドロック（F746 で実証済み）。PendSV は最低優先度。
- ThreadX が自前で `PendSV_Handler` を供給（`stm32xxxx_it.c` のものと競合させない）。
- クリティカルセクションは **PRIMASK ベース**（`TX_PORT_USE_BASEPRI` 未定義）。
- `__disable_irq` 下の `tx_application_define` で `HAL_GetTick` 依存の init を呼ばない。
- 割込みは TX オブジェクト生成後に有効化。

## ボード要点

- **STM32F746G-DISCO**: 216 MHz = HSE 25 MHz → PLL M25 N432 P2、VOS1 + over-drive、Flash 7WS。
  **PA9 は VCP_TX と OTG_FS_VBUS の共用**（UM1907 ソルダーブリッジ）。LED LD1 = PI1。
  メモリ: Flash 1MB @ 0x08000000 / ITCM 16KB / DTCM 64KB @ 0x20000000 / SRAM 256KB @ 0x20010000。
  I/D-cache 有効時、ITCM 配置の効果は ~0.6%。
- **Wio Lite AI**: USB は単一で **USB1_OTG_HS を FS（内蔵 PHY）動作**。CMSIS に
  `USB2_OTG_FS` / `OTG_FS_IRQn` は無く、TinyUSB(dwc2) は rhport0 を OTG_HS base +
  `OTG_HS_IRQHandler` にエイリアス（`tud_int_handler(0)`）。GPIO = PA11/PA12
  `GPIO_AF10_OTG1_FS`、USB クロック PLL3Q 48 MHz。LED0 = PC13 / LED1 = PF0 / USER = PF1
  （active-low、保持リセットで DFU）。`.axi_dma` 等 DMA 共有バッファは両端 32B align
  （D-Cache コヒーレンシ）。

## レビュー時の作法

- **「コンパイルが通る」は根拠にならない。** レジスタ/能力の主張は対象ボードの RM
  （F746 = RM0385 / H725 = RM0468）の節番号で、配線の主張は UM1907 / schematic で裏を取る。
  裏が取れない推測は推測として明示する。
- 存在しないファイル・行・レジスタ・実機挙動を作らない。Wio の実機は 1 枚しかないので
  「焼いて試せばわかる」は安いコストではない。
- 指摘には影響（何がどう壊れるか）と具体的な修正案を付ける。

## ビルド / フラッシュ

CMake 構成は統合の最初の Issue で確定する（それまでの正は各元リポジトリ）。確定後の想定:

```bash
cmake -B <builddir> -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake <board 選択>
cmake --build <builddir>
# F746: cmake --build <builddir> --target flash   (ST-Link)
# Wio : dfu-util -d 0483:df11 -a 0 -D <app>.bin   (PF1 保持リセットで DFU モードに入ってから)
```
