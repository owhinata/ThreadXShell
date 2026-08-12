# ThreadX Shell プロジェクト

マルチボード対応の **Eclipse ThreadX + シェルコンソール** ファームウェア。
`../stm32f746g-disco` と `../wio-lite-ai` で個別に育てた shell 実装を単一リポジトリに統合し、
複数ボードを 1 つの shell コアでサポートする。ST 公式 HAL、ビルドは CMake + Ninja。
HAL/CMSIS/ThreadX 等は upstream ミラー submodule、ARM GNU ツールチェーンは初回 configure で
自動取得（両元リポジトリの方式を踏襲）。

## 対応ボード

| ボード | MCU | クロック | コンソール | フラッシュ書込 |
|---|---|---|---|---|
| STM32F746G-DISCO | STM32F746NGH6 / Cortex-M7 | 216 MHz（自前設定） | VCP: USART1 PA9/PB7 115200 | ST-Link（`--target flash`） |
| Wio Lite AI | STM32H725AEI6 / Cortex-M7 | 550 MHz（**DFU boot から継承**） | USB CDC（USB1_OTG_HS を FS 動作 / TinyUSB） | **DFU のみ**（`dfu-util`） |

ボードはこの先追加していく。**ボード追加時に必ず更新するもの**: この表 /「ボード固有ルール」節 /
`AGENTS.md` / `.claude/settings.json` の upstream ブロックリスト /
`.claude/skills/codex-{review,debug}` のボード別観点 /
**`boards/<board>/submodules.cmake`（そのボードが必要とする submodule の sentinel 一覧。
トップの fetch はここから導出される。共有リストは無い）**。

## 現行の作業目標

**両リポジトリの shell をボード非依存コアとして統合し、STM32F746G-DISCO と Wio Lite AI の
2 ポートで動かす。** 確定済みの方針（2026-08-11 ユーザー決定）:

- **shell 統合のベースは wio-lite-ai**（最新実装。core の差分は `cli_core.c` 63 行 /
  `cli_parse.c` 105 行のみで、乖離の本体は cmds と backend）。F746 側の uart backend と
  F746 専用 cmds（fs/gui/qspi/sdram 等）は移植で追加する。
- **移植順**: M1 = 骨組み + Wio Lite AI ポート（USB CDC で shell 起動、完了 #2）→
  M2 = F746 ポート（完了 #3）→ **M3 = boot ツリー取り込み（ビルドのみ、焼かない。完了 #11）**
  → M4 = 全ボードビルドのスクリプト/CI 化。
- **Wio Lite AI の DFU ブートローダは本リポジトリに統合済み**（`boards/wio-lite-ai/boot/`、
  移植元 `owhinata/wio-lite-ai` @ `09468bb`）。app とソースを共有しない独立ツリーで、
  **統合後も不変扱い**（下記「ボード固有ルール」参照）。

ビルドは `-DBOARD=<board>` で 1 ビルドディレクトリ = 1 ボード（既定なし）。ボードごとの
submodule 一覧は `boards/<board>/submodules.cmake`。M1/M2/M3 完了により、両ボードのビルド/
フラッシュ、および boot ツリーの正は**本リポジトリ**（元リポジトリではない）。

## アーキテクチャ / レイヤリング

一方向依存を守る: **HAL/CMSIS/ThreadX（`lib/`）← port（ボード別グルー）← shell ← app**。

リポジトリ構成（確定。ボード固有物は `boards/<board>/` に集約する）:

```
shell/            # ボード非依存: core/ cmds/ backend/ include/ test/
lib/              # upstream submodules (f7 系と h7 系が同居)
cmake/
boards/
  f746g-disco/    # port/ ldscript/ src/
  wio-lite-ai/    # port/ ldscript/ src/ boot/
```

- **shell コアはボード非依存**。`#ifdef <BOARD>` やペリフェラル直叩きを shell の core/cmds に
  入れない。ボード差は transport 抽象（`struct cli_transport_api` の backend）と port 側グルーで
  吸収する。
- **共有コアに触れる変更は、全対応ボードのビルドが通ることを確認してからコミットする**
  （全ボードビルドのスクリプト/CI は統合初期に整備する）。
- **shell の常設状態は静的割当**。共有 shell コア（インスタンス / スタック / ジョブプール）と
  transport の常設状態は静的割当で、init / dispatch / 出力経路は heap を要求しない。board 固有
  コマンドのペイロードは、board が bounded heap・排他（`malloc_lock`）・失敗処理を明示的に
  提供する場合に限り heap を使用できる（wio の coremark が実例）。スタックサイズ・スレッド
  優先度は `cli_config.h` の既定を踏襲し、`_Static_assert` を必ず通す。
- upstream submodule（`lib/` 配下）は read-only。編集せず、必要な調整は port 側で吸収する。
- **boot ツリー（`boards/wio-lite-ai/boot/`）は独立**。app / shell とソースを
  共有しない。shell 側の都合で boot に依存を張らない。

## 開発ワークフロー

### コード修正サイクル

以下を小さく繰り返す:

1. **コード修正** — 機能実装 or バグ修正
2. **ビルド** — 全対応ボード分を通す（共有コアに触れた場合は必須）
3. **フラッシュ** — ボード固有ルール節の手順で（F746=ST-Link / Wio=DFU。**Wio は自動ループで
   焼き直さない**）
4. **動作確認** — ユーザーが実機で確認（シェルコンソール / LED / 必要なら SWD）
5. **ドキュメント更新** — `README.md` / 該当モジュールの README を更新。非自明な知見は
   永続メモリ（`~/.claude/projects/.../memory/`）へ
6. **コミット** — 動作確認後にコミット

**動作確認前にコミットしない。ドキュメント／メモリ更新を忘れない。**

### Plan + Codex review ワークフロー

**Phase 系 / architecture を変える plan は、plan 確定前と実装後の両方で codex-review を実施する。**

対象となる plan:

- shell コアの構造・transport 抽象・コマンド API の変更（全ボードに波及する）
- ボードポートの追加、クロック/メモリ/割込み優先度の構造変更
- ThreadX 統合方針（`_tx_initialize_low_level`、SysTick、PendSV、tick 供給、スタック）
- 新規ペリフェラル採用、DMA・キャッシュ構成、リンカスクリプト/起動フローの変更
- 複数レイヤ（HW + HAL + RTOS + shell）に跨る変更
- Wio Lite AI のクロック継承・メモリ配置・USB CDC に関わる変更（ブリックリスク。特に厳格に）
- **boot ツリー（`boards/wio-lite-ai/boot/`）に触れる一切の変更**（原則やらない。やるなら最優先で厳格 review）

ゲートのタイミング:

1. **Plan 確定前**（実装着手前）: plan を **`codex-review` skill** で 3 面（設計 / MCU 実機能
   （対象ボードの RM 照合）/ HW リソース競合）review。BLOCKING / CONCERN を全解消してから
   `ExitPlanMode`。
2. **実装後**（commit 前）: branch の diff を review。
   BLOCKING 解消 → user に実機 verify 依頼 → commit。

   [!] **1 つの diff に掛けるレビューは 1 本だけ。** `/codex:review` と
   `/codex:adversarial-review` を同じ diff に両方掛けない — 後者は前者を含むので、
   重複するのは待ち時間だけ（実測: 4,871 行の diff で 11 分 + 15 分）。**どちらか一方を選ぶ**:
   - **既定は `/codex:review`**（focus を取らない内蔵レビュアー）
   - **`/codex:adversarial-review <focus>` を選ぶのは、変更がチェック機構・ゲート・安全機構
     そのものを足す/変えるとき**。汎用レビュアーは「コードが正しいか」を見るが、
     「この検査は騙せるか（fail open するか）」は見ない。M3 では実際に前者が findings ゼロ、
     後者が実在する fail-open を 2 件出した
   - **focus は 1〜2 問に絞る。** 観点を並べるほど時間が伸びる（M3 の 5 観点のうち
     当たったのは 2 つ）。「この検査を通過したまま X できるか」の形で、
     疑っている fail-open 面を名指しする

**Codex 呼び出しは codex plugin（`codex@openai-codex`）のランタイムに一本化**（wio-lite-ai の
方式を踏襲。MCP server は使わない）。入口の使い分け:

| 対象 | 使うもの |
|---|---|
| **plan（会話中の設計・実装計画）** | **`codex-review` skill**（marker を更新する唯一の経路） |
| 実装後の差分（既定） | `/codex:review` |
| 検査機構・ゲート・安全機構を足す/変える差分、設計判断への異議 | `/codex:adversarial-review <focus>` |
| 不具合の原因追跡 | `codex-debug` skill、または `/codex:rescue` |

`/codex:review` は git 差分専用で plan をレビューできず marker も更新しない。だから plan
ゲートだけが `codex-review` skill に残っている。レビューは毎回 120s を超えるので
`run_in_background: true` で起動して待つ（進捗は `/codex:status`）。

**プロジェクト不変条件は `AGENTS.md` に置く**。`/codex:review` の内蔵レビュアーは focus text を
受け取れないので、不変条件を Codex に伝える経路は `AGENTS.md` しかない。**不変条件を変えたら
CLAUDE.md と `AGENTS.md` の両方を直す。**

**ゲートの強制**: `ExitPlanMode` の PreToolUse hook（`.claude/settings.json`）が
`~/.claude/.threadx-shell-plan-codex-reviewed` marker を確認する。marker が無い/古い（2h 超）と
block される。codex-review が LGTM に至ると marker を更新して通過。trivial plan で skip する
場合も **user 承認を得てから** `touch ~/.claude/.threadx-shell-plan-codex-reviewed`。

**Codex の指摘は鵜呑みにしない。** ローカルの実物・対象ボードの RM で裏を取ってから報告する
（実測: wio-lite-ai で CMake `file(DOWNLOAD)` について事実と異なる P1 が出た例がある）。

## Git ワークフロー

**PR は作らない。** Issue 駆動で feature/fix ブランチを切り、ローカル `main` に `--ff-only`
merge → push → Issue へ対応コメント → Issue クローズ、の流れ。リポジトリは
`owhinata/ThreadXShell`。

- **ブランチ**: `feat/`, `fix/`, `docs/`, `build/`, `refactor/`, `chore/`, `style/` prefix。
  `<prefix>/<N>-short-description`（`<N>` は Issue 番号）。ベースは常に `main`
- **コミット**: conventional commits 形式 `type: short description`。Issue 対応時は
  `type: #N short description` で **subject に Issue 番号**を含める（GitHub のリンク生成＋
  オートクローズ判定のため）
- [!] **コミットメッセージ（subject / body）と README は英語で書く。** 会話・Issue・
  コード内コメントは日本語でよい（wio-lite-ai で言語を規定せず 41 件のコミットが日本語に
  漂流した教訓。このリポジトリは初日から規定する）
- コミットメッセージ末尾に `Co-Authored-By: Claude ...` を付与
- 動作確認していない変更を commit / push しない

### 手順

```bash
# 1. Issue 作成（着手前に必ず）
gh issue create --repo owhinata/ThreadXShell --title "short description" --body "$(cat <<'EOF'
## Summary
- 症状・問題・やりたいことの説明
## Environment
- Board: <STM32F746G-DISCO | Wio Lite AI | common>
- Reproduced at: <commit-hash>
## Notes
- 調査メモ・仮説・設計案
EOF
)"

# 2. ブランチを切って実装 → ビルド → フラッシュ → 実機で動作確認 → コミット
git checkout -b feat/<N>-short-description
git commit -m "type: #<N> short description"

# 3. ローカル main に ff-merge（merge コミットは作らない）
git checkout main
git merge --ff-only feat/<N>-short-description

# 4. push（subject の `#<N>` / `closes #<N>` でオートクローズ）
git push origin main

# 5. Issue へ対応コメント（コミットレンジ <base>..<head> を必ず含める）
gh issue comment <N> --repo owhinata/ThreadXShell --body "..."

# 6. Issue クローズ（未クローズなら）＋ マージ済みブランチ削除
gh issue close <N> --repo owhinata/ThreadXShell
git branch -d feat/<N>-short-description
```

### 注意

- **PR は作らない**。`gh pr create` / `gh pr merge` は使わない
- `main` への直 push なので `--ff-only` を厳守、**force push は禁止**
- Issue を立てずにコミットしない（`#<N>` 参照が無いコミットは追跡できない）。Epic / 親 Issue は
  クローズキーワードを使わず `#<epic>` 参照のリンクのみとする（誤クローズ防止）
- 元リポジトリの Issue を参照するときは `owhinata/wio-lite-ai#N` / `owhinata/stm32f746g-disco#N`
  の完全形で書く（裸の `#N` はこのリポジトリの Issue と解釈される）

### Upstream submodule は read-only

以下は upstream のミラー submodule。`gh` での書き込み操作（PR/issue/comment 等）を行っては
ならない（PreToolUse hook がブロックする）。コードも編集せず、port 側のグルーで吸収する:

- `STMicroelectronics/` — `stm32f7xx_hal_driver`, `stm32h7xx_hal_driver`, `cmsis_device_f7`,
  `cmsis_device_h7`, `cmsis-core`, `stm32-ov5640`
- `eclipse-threadx/` — `threadx`, `filex`, `levelx`, `netxduo`, `guix`
- `hathach/tinyusb`, `eembc/coremark`, `armink/FlashDB`, `mlcommons/tiny`

## ThreadX 統合の共通教訓（全ボード共通）

- **SysTick > PendSV**（優先度）。同一だと idle 時 PendSV スピンを tick が割り込めず tick 停止 →
  スリープ中スレッドが起床しないデッドロック（F746 で実証済み。F746 は 14 vs 15）。
  PendSV は最低優先度。
- ThreadX が自前で `PendSV_Handler` を供給する（`stm32xxxx_it.c` のものと競合させない）。
- ThreadX クリティカルセクションは **PRIMASK ベース**（`TX_PORT_USE_BASEPRI` 未定義）。
  ISR から `tx_event_flags_set` 等を呼んでもクリティカルセクションを preempt できず安全。
- `__disable_irq` 下の `tx_application_define` で `HAL_GetTick` 依存の init を呼ばない
  （SysTick 凍結中のため）。
- 割込みは TX オブジェクト生成後に有効化する。

## ボード固有ルール

### STM32F746G-DISCO

- **216 MHz**（HSE 25 MHz → PLL M25 N432 P2、VOS1 + over-drive、Flash 7WS）。FPU / I$ / D$ on。
  FPU は**単精度のみ**（`-mfpu=fpv5-sp-d16`）— CoreMark の `%f` スコア行は `__aeabi_d*` +
  `_printf_float` 経由（`-u _printf_float` でリンク）。
- VCP: USART1、TX=PA9 / RX=PB7、115200 8N1 → `/dev/ttyACM0`。**PA9 は OTG_FS_VBUS と共用**
  （既定ソルダーブリッジで VCP 有効、UM1907）。LED LD1（緑）= PI1。
- メモリ: Flash 1MB @ 0x08000000 / ITCM 16KB @ 0x00000000 / DTCM 64KB @ 0x20000000 /
  SRAM 256KB @ 0x20010000。
- フラッシュ書込: ST-Link（`cmake --build build/f746g-disco --target flash`）。
- **timebase = TIM2 @ 108 MHz**（2×PCLK1、APB1 /4 で TIMPRE=0。RM0385）。EPK の time source と
  udelay で共用するため **`CLI_CPU_CYCLES_PER_US=108`**（コアクロックの 216 ではない）。
- **`CLI_INSTANCE_TIME_SLICE=0`（TX_NO_TIME_SLICE）を維持**する。VCP + telnet の 2 インスタンスが
  同一優先度に並ぶが、coremark / membench / nn_run が静的状態と DWT CYCCNT を共有していて
  多重実行に非再入（#4）。CPU-bound コマンド実行中に他コンソールが応答しないのが**期待挙動**。
- **LTO 禁止**。ldscript の ASSERT 群が配置 invariant の本体で、LTO はその ASSERT が
  依拠するシンボル / 入力セクション名を改名してしまう（`board.cmake` が per-config 変種込みで
  FATAL_ERROR にする）。加えて POST_BUILD の `cmake/check_f746_layout.py` が
  シンボル常駐・ベクタテーブル・float ランタイムを実イメージで検査する。
- **SDRAM は FMC 内部バンクで用途固定**: bank0 = LTDC スキャンアウト面 + 固定居住者 /
  bank1 = カメラ DMA アリーナ 2MB / bank2 = ETH ディスクリプタ + プール /
  bank3 = NN アリーナ（上半分 1MB @ 0xC0700000 は reloc モデルの実行窓）。
  バンクをまたぐ配置変更は FE とキャッシュコヒーレンシに直結する。
- 教訓: I/D-cache 有効時は ITCM 配置の効果 ~0.6%（キャッシュが flash WS を隠蔽済み）。
- リファレンス: RM0385 / UM1907 / ST 公式デモ（`_ref/f746g-disco/_ref/`、read-only）。

### Wio Lite AI（[!] ブリック安全則あり）

- **現存する実機は board #2 のみ**（board #1 は恒久文鎮化）。焼き直し・実験のコストを常に意識する。
- [!] **boot ツリー（`boards/wio-lite-ai/boot/`）と ROM リンカスクリプト
  （`STM32H725AEIx_ROM.ld`）は不変**。
  内蔵 Flash セクタ0 `0x08000000`（128KB）に boot が常駐し、ここを焼き直す操作は
  **ブリック本番**。boot は本リポジトリに統合済み（`boards/wio-lite-ai/boot/`、移植元
  `owhinata/wio-lite-ai` @ `09468bb`）だが、app 開発で boot 側を変更する必要は原則ない。
  もし触る必要が生じたら **必ず** codex-review（3 面）+ 監査 + バックアップを経てから、
  良品 ST-Link（mode=UR）接続下でユーザーに実機書込を依頼する
  （手順は `boards/wio-lite-ai/boot/README.md`）。
  **boot の `iflash.c` の書込先セクタ範囲チェックはセクタ0 を守る唯一の砦**で、緩める変更は
  絶対にしない。ST-Link での内蔵 Flash 書込は boot をセクタ0 に焼く用だけ（通常やらない）。
- **boot は「参照ビルド」としてのみビルドする**（`boot` ターゲット → `boot-reference/`）。
  **セクタ0 に書けるターゲットも `dfu-boot` も作らない**（後者は boot を app パーティションに
  焼くターゲットになる）。ビルドする理由はビルド継続性 — HAL / TinyUSB / toolchain /
  board.cmake の変更が boot を黙って壊さないことの確認。
  ゲートは **`cmake/check_boot_safety.py`**（`boot_precheck` = ソース manifest + compile
  command 監査、POST_BUILD = ベクタ配置 / セクタ0 収容 / option-byte・DBGMCU 経路の不在 /
  flash 書込の呼び出しグラフ / DFU クラス / ELF↔bin 結合 / **各オブジェクトの LTO IR 不在** /
  golden hash）。**LTO の保証はコマンドライン走査ではなくオブジェクト（`.gnu.lto_*`
  セクション）で取る** — specs ファイル・compiler launcher・ninja 外で作り直した
  オブジェクトはコマンドラインに痕跡を残さない。flash 書込 API のアドレスは
  データワードだけでなく **`movw`/`movt` 対**でも検出する（間接呼び出しは辺を残さない）。
  **`boot_image` は毎ビルド再リンクする**（`boot_precheck` の stamp を `LINK_DEPENDS` に
  入れてある）ので、up-to-date による迂回経路は存在しない。
  golden hash は donor `09468bb` の**再現性ベースライン**であって実機イメージの証明ではない。
  ゲートの negative test は `cmake/fixtures/run_fixture_tests.py`。
- **DFU フォールバックの安全網を app 側から壊さない**: erased/invalid app では必ず DFU モード
  に入る（中断した DFU 転送も、先頭 32B を最後に書く vector-last commit により必ずここに落ちる）。
  boot の DFU 判定条件に影響する変更を app 側から入れない。
- app は**内蔵 Flash `0x08020000`（セクタ1-3、384KB）から実行**。書込は **DFU のみ**:
  **PF1（USER）保持リセット**で DFU モード（`0483:df11`）→
  `dfu-util -d 0483:df11 -a 0 -D <app>.bin` → 自動 reboot。
  [!] **内蔵 Flash の書換え耐久は ~10k サイクル**。自動ループで焼き直さない。
- [!] **app はクロックツリーを再設定しない**。app は boot から継承した system/PLL クロック
  ツリー（クロックソース、D1/D2/D3 プリスケーラ、PLL1/PLL2、および下記例外以外の PLL3 設定）、
  FLASH ACR、電源供給選択（SMPS/LDO）・VOS を再設定しない。ペリフェラルの bus clock gate と
  kernel clock mux の設定は許可する。**例外は次の 2 つのみ**:
  (a) `ltdc_clock_init()` が USB クロック供給前に行う 3 フィールド・成功パス計 4 書込み
  （`RCC_CR.PLL3ON` clear / `RCC_PLL3DIVR.DIVR3` 更新 / `RCC_PLLCFGR.DIVR3EN` set /
  `RCC_CR.PLL3ON` set。RM0468 §8.7.1 / §8.7.11 / §8.7.16）、
  (b) `HAL_PWREx_EnableUSBVoltageDetector()` による `PWR_CR3.USB33DEN` set（RM0468 §6.8.4）。
  継承値は 550 MHz / PLL3Q 48 MHz USB / FLASH latency 3。`SystemInit` は
  **FPU + VTOR + TCM 初期化のみ**のカスタム版（ECC 有効化・ゼロ充填・`.itcm` ロード・MSP fill。
  RCC / PWR / FLASH ACR は触らない。stock CMSIS `SystemInit` / `SystemClock_Config` は
  呼ばない）。VTOR はリンカの `g_pfnVectors` から取る。SysTick reload は継承した
  `SystemCoreClock`（550 MHz）から計算し、その過程で RCC は触らない。
- [!] **RAM 配置ポリシー**（wio-lite-ai#46）: **AXI-SRAM（320KB @ 0x24000000）= バスマスタから
  見える必要があるものだけ / DTCM（128KB @ 0x20000000）= CPU 専用のホットなもの /
  ITCM（64KB）= ISR コード**。**DMA1/DMA2・SDMMC1 IDMA は TCM に届かない**
  （RM0468 §2.1.2/§2.1.5/§2.1.6）。**DTCM の DMA バッファは fault せず無言で転送されない**。
- [!] **リンカスクリプトの `ASSERT` は LTO 下で空振りする**。配置の最終ガードはポストリンクの
  residency チェック（`check_itcm_residency.py` / `check_dtcm_residency.py` を移植して維持する）。
- コンソール = USB CDC: USB1_OTG_HS を FS（内蔵 PHY）動作。TinyUSB(dwc2) は rhport0 を
  OTG_HS base + `OTG_HS_IRQHandler` にエイリアス（`tud_int_handler(0)`）。GPIO = PA11/PA12
  `GPIO_AF10_OTG1_FS`、USB クロック PLL3Q 48 MHz。app = **`0483:5740`**（ST 汎用 VCP）→
  `/dev/ttyACM0`。
- LED0（赤）= PC13 / LED1（黄）= PF0 / USER ボタン = PF1（active-low）。
- **オプションバイト / RDP / DBGMCU / SWD 端子（PA13/PA14）は絶対に触らない。**
- リファレンス: RM0468 / PM0253 / 基板 schematic（`_ref/wio-lite-ai/`、read-only）。

## SWD デバッグ（共通）

- GDB はシステムの **`gdb-multiarch`**（toolchain 同梱 gdb は `libncursesw.so.5` 欠如で不可）。
- GDB サーバ: OpenOCD（`-f interface/stlink.cfg -f target/stm32f7x.cfg` or `stm32h7x.cfg`、
  :3333。SCS 読みが安定）か `st-util`（:4242）。
- コンソール（picocom 等）と `st-flash`/読み出しは `/dev/ttyACM0` を奪い合うと文字化けする。
  SWD とコンソールは別系統。
- Wio Lite AI の boot 書込/復旧手順は `boards/wio-lite-ai/boot/README.md`。焼き/復旧用 ST-Link の
  個体情報は `../wio-lite-ai/CLAUDE.md` を参照（良品 Discovery ST-Link のみ mode=UR 可）。

## リファレンス（`_ref/`）

- `_ref/` は git 管理外（`.gitignore` 済）の「ローカルで読むための資料」専用。
  **公開環境に含めない — ビルド（CMake）・`scripts/`・git 管理下のファイルから `_ref/` を
  一切参照しない**。参照した瞬間、クローンしただけでは configure できないリポジトリになる
  （wio-lite-ai#58 の教訓）。C コード中の `_ref/...` 言及は出典コメントのみ可。
- 構成（2026-08-11 に両元リポジトリからコピー済み）:
  - `_ref/f746g-disco/` — stm32f746g-disco リポジトリ全体のコピー。資料はその中の
    `_ref/f746g-disco/_ref/`（RM0385、UM1907、STM32Cube_FW_F7 の ST 公式デモ、
    出荷時デモの `backup_full.bin`）
  - `_ref/wio-lite-ai/` — wio-lite-ai の資料一式（RM0468、PM0253、H72x/73x errata、
    基板 schematic、W25Q128 / PSRAM / RTL872xD datasheet、STM32Cube_FW_H7、
    tinyuf2 / tinyusb 参照、実機フラッシュバックアップ類）

## ドキュメント

- **`README.md` と各モジュールの README は英語で書く**。変更と同時に更新する。
- リポジトリ内ファイルの記号は **基本 ASCII**。絵文字は使わない（強調マーカーは `[!]`）。
- mkdocs / GitHub Pages（stm32f746g-disco 方式）への移行は将来 Issue で判断。
