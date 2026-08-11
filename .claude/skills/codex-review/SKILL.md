---
name: codex-review
description: Codex による plan（実装計画）レビュー。ExitPlanMode ゲートの marker を更新する唯一の経路。ThreadX Shell（マルチボード: STM32F746G-DISCO / Wio Lite AI）の shell コア構造 / transport 抽象 / ThreadX 統合 / ボードポート / メモリ配置に関わる計画に使う。実装後の diff レビューは codex plugin の /codex:review・/codex:adversarial-review を使う（この skill ではない）。
argument-hint: <plan | 設計説明>
---

# Codex plan レビュー (ThreadX Shell / マルチボード)

## この skill の役割 / 役割でないもの

Codex 呼び出しは **codex plugin（`codex@openai-codex`）のランタイムに一本化**されている。
用途ごとの入口は以下:

| レビュー対象 | 使うもの |
|---|---|
| **実装計画（plan mode / 会話中の設計）** | **この skill**（ExitPlanMode の marker を更新する） |
| 実装後の差分（working tree / branch） | `/codex:review`（focus text は渡せない） |
| 差分を観点付きで叩く / 設計判断そのものを疑う | `/codex:adversarial-review [--base <ref>] <focus>` |
| 不具合の原因追跡 | `codex-debug` skill、または `/codex:rescue` |

**plan だけがこの skill に残っている理由**: plugin の review コマンドは git 差分専用で、
まだコードになっていない会話中の plan をレビューできない。「Plan 確定前ゲート」（CLAUDE.md）は
コード以前に効く必要があるので、ここだけ自前で持つ。

## 実行手順

### 1. plan を自己完結した 1 枚のプロンプトに落とす

Codex は**この会話のコンテキストを見られない**。「上記の plan」のような参照は書かない。
scratchpad にプロンプトファイルを書く（`$CLAUDE_SCRATCHPAD` が無ければ `/tmp` 配下でよい）:

- plan の全文（変更するファイル、追加する初期化、触るペリフェラル、割込み、メモリ配置）
- **plan が対象とするボードを明示する**（F746 / Wio / 共有コア）。共有コアに触れる plan は
  「全対応ボードへの影響」を必ずレビュー対象に含める
- プロジェクト不変条件（`AGENTS.md` にも置いてあるが、プロンプトにも明示する）
- 下の「3 面レビュー観点」を**明示的に 3 つとも指示**
- 「LGTM を出す場合は該当面すべてについて根拠（RM の節番号 / ファイル:行）を示すこと」
- 出力形式の指定: 面ごとに `LGTM` / `CONCERN` / `BLOCKING` と、根拠と修正案

Codex は read-only サンドボックスでローカルシェルを使える（`git diff`・`grep`・`sed` が通る）。
つまり**リポジトリの実物を読ませてよい** — plan 中のファイルパスはそのまま書けば Codex が開く。
統合元の参照実装は `../stm32f746g-disco/` と `../wio-lite-ai/` にある（絶対パスで書く）。

### 2. Codex に投げる

plugin のバージョン番号をハードコードしないこと（更新で壊れる）:

```bash
node "$(ls -d "$HOME"/.claude/plugins/cache/openai-codex/codex/*/scripts/codex-companion.mjs \
        | sort -V | tail -1)" task --effort xhigh --prompt-file /abs/path/to/plan-review.md
```

（コマンドは **`node` で始める**こと。先頭に変数代入を置くと `Bash(node:*)` の
permission ルールに当たらず毎回プロンプトが出る。）

- `--write` は**付けない**。付けなければサンドボックスは `read-only`（レビューに書込は不要）。
- `--effort` は `xhigh`（Wio 側はブリックリスクがあるゲートなので最大で回す）。
- **`Bash(run_in_background: true)` で起動して待つ**。plan review は毎回 120s を超えるので
  フォアグラウンドだとツール側でタイムアウトする。出力ファイルを読んで結果を取る。
  （スコープを削ってフォアグラウンドに収めると根拠の薄いレビューになる方が損。）

### 3. 結果を整理してユーザーに報告

面ごとに verdict と根拠を並べる。Codex の指摘は**そのまま鵜呑みにしない** — 対象ボードの RM
（F746 = RM0385 / H725 = RM0468）やリポジトリの実物で裏を取れる指摘かどうかを確認してから
報告する。

### 4. marker の更新（ここがゲート）

3 面すべて問題なしなら「実装着手 OK」とし、marker を更新する:

```bash
touch ~/.claude/.threadx-shell-plan-codex-reviewed
```

- 問題ありなら**marker は更新しない**。BLOCKING / CONCERN を解消して再 review し、
  LGTM に至ってから touch する。
- この marker は `ExitPlanMode` の PreToolUse gate（`.claude/settings.json`）が確認する。
  marker が無い / 古い（2h 超）と ExitPlanMode は block される。
- trivial plan で skip する場合も、**user 承認を得てから** touch する。

## 3 面レビュー観点

それぞれ**独立したチェック**として実施する。1 面が LGTM でも、他面が未確認なら全体 LGTM に
しない。

### 観点 1: 設計レビュー（マルチボード整合を含む）

- アーキテクチャの妥当性、レイヤ分離、API 設計
  （一方向依存 **HAL/CMSIS/ThreadX（lib/）← port ← shell ← app**）
- **shell コアのボード非依存性**: core/cmds に `#ifdef <BOARD>` やペリフェラル直叩きを
  持ち込んでいないか。ボード差は transport 抽象（`struct cli_transport_api`）と port 側グルーで
  吸収しているか
- **共有コアに触れる plan は全対応ボードで成立するか**（片方のボードだけ見て LGTM しない）
- ST HAL の使い方・初期化順序が HAL の前提と整合しているか
- ThreadX 統合の正しさ（`tx_application_define`、スタックサイズ、`_tx_initialize_low_level`、
  tick 供給、PendSV/SysTick、割込みは TX オブジェクト生成後に有効化しているか）
- shell の静的割当を維持しているか、`cli_config.h` の `_Static_assert` を通すか
- エラーハンドリング、排他制御、エッジケース

### 観点 2: MCU 実機能レビュー（対象ボードの RM / ボード資料と照合）

**「API がコンパイルできる」≠「実機で期待通り動く」**。レジスタ/能力の根拠を対象ボードの RM で
確認する（F746 = RM0385 + UM1907 / H725 = RM0468 + PM0253 + schematic）。

**STM32F746G-DISCO:**

- クロック構成: PLL 係数・VOS スケール・over-drive・**Flash wait states** が目標 SYSCLK と
  整合するか（216 MHz は VOS1 + OD + 7WS）
- FPU / I-Cache / D-Cache の有効化順序、キャッシュコヒーレンシ（DMA 使用時）
- タイマの種別と能力（OPM/TRGO/PWM/DMA req/32-bit/CC チャネル数）が用途に合うか
- ピンの AF 番号が RM0385 の alternate function mapping と一致するか。ボード固有の
  ソルダーブリッジ依存（**PA9 は VCP_TX と OTG_FS_VBUS の共用** — UM1907）

**Wio Lite AI:**

- **クロック継承（app は RCC を再設定しない）**: app 側の `SystemInit`/初期化が
  RCC/PLL/FLASH ACR/PWR を書き換えていないか。カスタム `SystemInit` は
  **FPU + VTOR + ITCM ロードのみ**か。VTOR は `g_pfnVectors` から取っているか。
  `SystemCoreClock` = 550 MHz（SysTick reload 計算の根拠）
- **USB CDC**: 単一 USB = USB1_OTG_HS を FS（内蔵 PHY）動作。CMSIS に `USB2_OTG_FS` /
  `OTG_FS_IRQn` は無く、TinyUSB(dwc2) は rhport0 を OTG_HS base + `OTG_HS_IRQHandler` に
  エイリアス（`tud_int_handler(0)`）。GPIO = PA11/PA12 `GPIO_AF10_OTG1_FS`、
  USB クロック PLL3Q 48 MHz、app の VID/PID = `0483:5740`
- **メモリ配置と実行元**: app は内蔵 Flash `0x08020000`（セクタ1-3, 384KB）から実行。
  セクタ0 `0x08000000` は DFU ブートローダ専用（不可侵。boot は本リポジトリ `boards/wio-lite-ai/boot/` にあるが**不変**）
- ピンの AF 番号が schematic / RM0468 と一致するか（LED0=PC13, LED1=PF0, USER=PF1）

### 観点 3: HW リソース競合レビュー

- **割込み優先度 (NVIC/SCB)**: F746/H725 とも優先度 4 bit。ThreadX 使用時は PendSV=最低、
  **SysTick > PendSV**。ThreadX クリティカルセクションは **PRIMASK ベース**
  （`TX_PORT_USE_BASEPRI` 未定義）。HAL ISR と競合しないか
- **タイマ / DMA**: 使用ストリーム/チャネルの競合、DMA と D-Cache のコヒーレンシ
  （DMA 共有バッファは両端 32B align）
- **GPIO / AF**: ピンの多重割当（F746: VCP=PA9/PB7, LD1=PI1 / Wio: USB=PA11/PA12,
  LED=PC13/PF0, USER=PF1 など既存用途と衝突しないか）
- **メモリ領域**: リンカスクリプトと startup の symbol 整合（`_estack`, `_sidata` 等）
  - F746: Flash 1MB @ 0x08000000 / ITCM 16KB / DTCM 64KB @ 0x20000000 / SRAM 256KB @ 0x20010000
  - Wio [!] **RAM 配置ポリシー**（wio-lite-ai#46）: AXI-SRAM（320KB @ 0x24000000）=
    バスマスタから見える必要があるものだけ（**DMA が届く唯一の RAM**）/ DTCM（128KB @
    0x20000000）= CPU 専用のホットなもの / ITCM = ISR コード。
    **DMA1/DMA2・SDMMC1 IDMA は TCM に届かない**（RM0468 §2.1.2/§2.1.5/§2.1.6）。
    **DTCM に DMA バッファを置くと fault せず無言で「何も転送されない」**。
    新しいバッファを足す plan は必ずこの表で行き先を決めさせる
- [!] **リンカスクリプトの `ASSERT` は LTO 下で空振りする**。配置を変える plan はポストリンクの
  residency チェック（`check_itcm_residency.py` / `check_dtcm_residency.py` 相当）を通るか
- **Wio boot 境界**: 変更が boot ツリー（`boards/wio-lite-ai/boot/`）・
  `STM32H725AEIx_ROM.ld`・内蔵 Flash セクタ0 に波及していないか。**boot の `iflash.c` の範囲チェックはセクタ0 を守る唯一の砦**で、
  緩める変更は不可。boot に触れる plan はそれだけで最厳格レビュー対象
- **ビルド入力**: `_ref/`（および `../*/_ref/`）は git 管理外なので、CMake / スクリプトが
  読む plan は「クローンしただけでは configure できないリポジトリ」を作る（wio-lite-ai#58）
- **Wio 書換え耐久**: 内蔵 Flash は ~10k サイクル。自動ループで焼き直す plan は不可

## 成立性の証拠

HW 依存の設計には、LGTM 前に成立性の証拠を要求する:

- 対象ボードの RM のレジスタ記述に基づく根拠（**節番号まで**）
- 最小実機テスト or 観測(コンソールバナー、LED 挙動、必要なら SWD/OpenOCD)
- **「コンパイルが通った」は証拠にならない**。特に Wio の「クロックを触っていないこと」
  「配置が剥がれていないこと」は実機 or objdump/`.map` 監査で裏取りする

## リファレンス（git 管理外・読む専用）

- `_ref/f746g-disco/_ref/` — RM0385 / UM1907 / STM32Cube_FW_F7（ST 公式デモ）
- `_ref/wio-lite-ai/` — RM0468 / PM0253 / Wio Lite AI schematic / STM32Cube_FW_H7
- `boards/wio-lite-ai/boot/README.md` — ブートローダが app へ渡す実測クロック値と boot 書込手順
  （boot 統合完了までは `../wio-lite-ai/boot/README.md`）
- 統合元の実装: `../stm32f746g-disco/shell/` `../wio-lite-ai/shell/`（および両 `port/`）
