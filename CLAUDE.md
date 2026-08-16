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
| Wio Lite AI | STM32H725AEI6 / Cortex-M7 | 550 MHz（**DFU boot から継承**） | USB CDC（USB1_OTG_HS を FS 動作 / TinyUSB） | **DFU のみ**（`--target flash` = `dfu-shell`） |
| Grove Vision AI V2 | Himax HX6538 WiseEye2 / dual Cortex-M55 + Ethos-U55（app は CM55M / Secure） | 400 MHz（**bootloader から継承**。SCU 読み戻しで実測確認済み） | UART0（CH343P ブリッジ経由）921600 | **UART xmodem のみ**（`--target flash`。リセットボタン押下が要る手動フロー） |

ボードはこの先追加していく。**ボード追加時に必ず更新するもの**: この表 /「ボード固有ルール」節 /
`AGENTS.md` / `.claude/settings.json` の upstream ブロックリスト /
`.claude/skills/codex-{review,debug}` のボード別観点 /
**`boards/<board>/README.md`（下記のとおり必須。そのボードの説明はここが正）** /
**`boards/<board>/submodules.cmake`（そのボードが必要とする submodule の sentinel 一覧。
トップの fetch はここから導出される。共有リストは無い）** /
`boards/<board>/test/host_tests.sh`（そのボードが持つ board-pinned ホストテスト。
`shell/test/run_host_tests.sh` が同じフラグで呼ぶ。**`test/` ディレクトリを作ったなら
`host_tests.sh` は必須** — 無いとランナーが fail する（dispatcher の消失で
テストが黙って走らなくなるのを防ぐため）。pin するものが無いボードは `test/` ごと作らない）。

## 現行の作業目標

**両リポジトリの shell をボード非依存コアとして統合し、STM32F746G-DISCO と Wio Lite AI の
2 ポートで動かす。** 確定済みの方針（2026-08-11 ユーザー決定）:

- **shell 統合のベースは wio-lite-ai**（最新実装。core の差分は `cli_core.c` 63 行 /
  `cli_parse.c` 105 行のみで、乖離の本体は cmds と backend）。F746 側の uart backend と
  F746 専用 cmds（fs/gui/qspi/sdram 等）は移植で追加する。
- **移植順**: M1 = 骨組み + Wio Lite AI ポート（USB CDC で shell 起動、完了 #2）→
  M2 = F746 ポート（完了 #3）→ **M3 = boot ツリー取り込み（ビルドのみ、焼かない。完了 #11）**
  → M4 = 全ボードビルドのスクリプト/CI 化（**保留**。着手条件は Epic #1 のコメント:
  枚数やマイルストーンではなく、手動運用が実際に破れたとき）。
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
  （スクリプト/CI 化は保留中。手打ちで確認する）。
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

**説明の正は `boards/f746g-disco/README.md`**（クロック・ピン・メモリマップ・手順・
ハマりどころ）。ここに置くのは破ってはいけないことだけ:

- **LTO 禁止**。ldscript の ASSERT 群が配置 invariant の本体で、LTO はその ASSERT が
  依拠するシンボル / 入力セクション名を改名してしまう（`board.cmake` が per-config 変種込みで
  FATAL_ERROR にする）。POST_BUILD の `cmake/check_f746_layout.py` と併せて外さない。
- **SDRAM は FMC 内部バンクで用途固定**（bank0 = LTDC / bank1 = カメラ DMA / bank2 = ETH /
  bank3 = NN アリーナ）。バンクをまたぐ配置変更は FE とキャッシュコヒーレンシに直結する。
  境界は ldscript の ASSERT が持っているので、緩めない。
- **`CLI_CPU_CYCLES_PER_US=108`**（timebase は TIM2 @ 108 MHz。コアクロックの 216 ではない）。
- **`CLI_INSTANCE_TIME_SLICE=0`（TX_NO_TIME_SLICE）を維持**する。coremark / membench / nn_run が
  多重実行に非再入（#4）で、CPU-bound コマンド中に他コンソールが応答しないのが**期待挙動**。
- リファレンス: RM0385 / UM1907 / ST 公式デモ（`_ref/f746g-disco/_ref/`、read-only）。

### Wio Lite AI（[!] ブリック安全則あり）

**説明の正は `boards/wio-lite-ai/README.md`**（ブート経路・継承クロック・メモリマップ・
DFU 手順・ゲートの中身）。復旧手順は `boards/wio-lite-ai/boot/README.md`。
ここに残すのは**エージェントの行動を止める規則**だけで、説明は README を見る。

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
  焼くターゲットになる）。ゲートは **`cmake/check_boot_safety.py`**（precheck = ソース
  manifest + compile command 監査 / POST_BUILD = 配置・収容・禁止経路・LTO IR・golden hash。
  **中身と設計理由は board README**）。**外す・弱める変更は不可**。
  golden hash は donor `09468bb` の**再現性ベースライン**であって実機イメージの証明ではない。
- **DFU フォールバックの安全網を app 側から壊さない**: erased/invalid app では必ず DFU モード
  に入る（中断した DFU 転送も、先頭 32B を最後に書く vector-last commit により必ずここに落ちる）。
  boot の DFU 判定条件に影響する変更を app 側から入れない。
- app は**内蔵 Flash `0x08020000`（セクタ1-3、384KB）から実行**。書込は **DFU のみ**:
  **PF1（USER）保持リセット**で DFU モード（`0483:df11`）→
  `cmake --build build/wio-lite-ai --target flash`（= `dfu-shell` のエイリアス。素の
  `dfu-util -d 0483:df11 -a 0 -D <app>.bin` と同じ）→ 自動 reboot。
  **`flash` は app パーティション専用**でセクタ0 には届かない（両ボードで同じ
  コマンド形にするためのエイリアスであって、ST-Link 経路を足したものではない）。
  [!] **内蔵 Flash の書換え耐久は ~10k サイクル**。自動ループで焼き直さない。
- [!] **app はクロックツリーを再設定しない**。boot から継承した system/PLL クロックツリー
  （クロックソース、D1/D2/D3 プリスケーラ、PLL1/PLL2、および下記例外以外の PLL3 設定）、
  FLASH ACR、電源供給選択（SMPS/LDO）・VOS を再設定しない。ペリフェラルの bus clock gate と
  kernel clock mux の設定は許可する。**例外は次の 2 つのみ**: (a) `ltdc_clock_init()` が
  USB クロック供給前に行う PLL3R の再設定、(b) `HAL_PWREx_EnableUSBVoltageDetector()` による
  `PWR_CR3.USB33DEN` set。**レジスタ単位の内訳と継承値は board README**。
  `SystemInit` は **FPU + VTOR + TCM 初期化のみ**のカスタム版で、stock CMSIS の
  `SystemInit` / `SystemClock_Config` は呼ばない。
- [!] **RAM 配置ポリシー**（wio-lite-ai#46）: **AXI-SRAM（320KB @ 0x24000000）= バスマスタから
  見える必要があるものだけ / DTCM（128KB @ 0x20000000）= CPU 専用のホットなもの /
  ITCM（64KB）= ISR コード**。**DMA1/DMA2・SDMMC1 IDMA は TCM に届かない**
  （RM0468 §2.1.2/§2.1.5/§2.1.6）。**DTCM の DMA バッファは fault せず無言で転送されない**。
- [!] **リンカスクリプトの `ASSERT` は LTO 下で空振りする**。配置の最終ガードはポストリンクの
  residency チェック（`check_itcm_residency.py` / `check_dtcm_residency.py` を移植して維持する）。
- **オプションバイト / RDP / DBGMCU / SWD 端子（PA13/PA14）は絶対に触らない。**
- リファレンス: RM0468 / PM0253 / 基板 schematic（`_ref/wio-lite-ai/`、read-only）。

### Grove Vision AI V2

- **公開 TRM が無い**。レジスタの正は SDK 同梱 `WE2_S.svd`、資料は
  `_ref/grove-vision-ai-v2/`（HX6538 datasheet / 回路図 / M55 TRM）。
  レジスタ/能力の主張は SVD と SDK 実装で裏を取る。
- **Himax SDK は submodule ではない**。configure 時に pin した SHA（933810cc、donor と同一）を
  `boards/grove-vision-ai-v2/sdk/` へ直接 fetch（`cmake/himax_sdk.cmake`。
  `-DGROVE_SDK_DIR=` で既存チェックアウトを指定可）。**SDK ツリーは lib/ と同じ read-only 扱い**。
  ペリフェラルドライバはプリビルト（`prebuilt_libs/gnu/libdriver.a`、ソース非公開）で、
  実行時に `EPII_NVIC_SetVector` で ITCM 上の書込可能ベクタテーブルへ ISR を登録する。
- **app は XIP ではない**。`.img`（bootloader + 2nd bootloader + 記述子 + 署名済み app）を
  UART xmodem で焼き、2nd bootloader が ELF を ITCM 256KB @0x10000000（ベクタ+コード+rodata）/
  DTCM 256KB @0x30000000（データ+スタック）へ展開する。SRAM0 窓（0x3401F000〜）は
  明示配置専用で M-G1 では空。**ITCM 溢れはリンクエラー**（SRAM0 へは逃げない）。
- **クロック継承**: app は PLL を設定しない。SCU 読み戻し → `SystemCoreClockUpdate` が唯一の
  真実（コンパイル時 config は 24 MHz プレースホルダ。**実測 400 MHz**）。SysTick reload は
  実行時値から計算し、起動前に妥当性 assert（`port/threadx/tx_glue.c`）。
- **全部 Secure（TrustZone SEC_ONLY）**: SAU 無効・全空間 Secure。ThreadX は
  **`TX_SINGLE_MODE_SECURE` 必須**（tx_user.h。無いと初回 PendSV の EXC_RETURN が
  Non-secure 向けで即死）。優先度は 3-bit: **PendSV=7 / SysTick=6**（M7 ボードの 15/14 を
  流用しない）。SDK の `ENABLE_OS` define が SDK 側 SysTick/SVC 強シンボルを外す seam。
- **IRQ 衛生**: `platform_driver_init()`（TZ 設定 + プリビルト init が IRQ を有効化し得る）は
  PRIMASK 下で実行し、カーネル入場前に IRQ 0..200 を disable + pending clear。コンソール
  UART IRQ は backend の enable()（shell スレッド上）でのみ開く。fallback の
  `uart_read_udma` を使う場合は **DMA3 combined IRQ（69）も** enable（67/68 は不可）。
- [!] **毎回の flash は bootloader 領域も書き直す**（Himax 標準フロー、donor 実績多数）。
  外付け W25Q128JW の耐久 ~100k。**自動ループで焼かない**。復旧 = チップ内 boot ROM +
  BOOT_OPT ストラップ + factory image（手順は `boards/grove-vision-ai-v2/README.md`）。
  コンソールと flash は同一シリアルデバイス（ターミナルを閉じてから焼く。
  CH343P は環境により `/dev/ttyUSB*` に見える）。
- **EPK は有効**（M-G2 / #25）。時間源は **Himax TIMER2**（RELOAD 全 1・割込み無効の
  自由走行。**触るのは `port/threadx/tx_glue.c` の bring-up 1 箇所だけ** — ベンダの
  `hx_drv_timer_*` は `hx_drv_timer_init` を除き API 丸ごと禁止シンボル）。
  プリビルトのカメラ系アーカイブがこの API を参照するが、対処は**ゲート緩和ではなく
  リンカ `--wrap` の board 所有 seam**（`port/sdk_seam/timer_seam.c` / #30。
  `__real_*` を呼ばないので禁止シンボルは最終 ELF に残らない）。
  **EPK の会計対象 IRQ は集合**で、「有効だが未ラップ」を作らない。ベンダ由来で
  番号が事前に分からない周辺は **ISER スナップショットで実測**して全部ラップする
  （`port/sdk_seam/epk_irq_wrap.c`）。
  最外周の UART0 ISR はプリビルト内なので、**ITCM 上のベクタを実行時にラップ**して
  enter/exit を挟む（`backend/cli_backend_uart.c`）。**失敗してもコンソールは生かし**、
  共有 `thread` が `--` と理由を出す（`cli_thread_cpu_source_ok` 弱シンボル）。
  信用判定は boot 時ラッチではなく **`tx_glue_profile_ok()` が毎回再検証**する
  （TIMER2 の CTRL/RELOAD と計数 / ラップしたベクタ / 会計対象外の IRQ が
  有効でないこと / EPK ネストカウンタ 0）。
  時間源の分担: **EPK = TIMER2 / udelay と membench = DWT CYCCNT /
  CoreMark = `tx_time_get()`**。
- **WFI 有効**（`BSP_ENABLE_WFI`、既定 ON）。コンパイル時スイッチなので前提は検査ではなく
  **強制** — カーネル入場前に `SCB->SCR` の SLEEPDEEP / SLEEPONEXIT を clear → 読み戻し →
  駄目なら fail-stop。`TX_LOW_POWER` は使わない。`hx_lib_pm_*` は禁止シンボル接頭辞。
  「TIMER2 がスリープ中も進む」ことの実測は **`epk sleep <ms>`**（`(idle)` の観察では不十分）。
- **ベンチマーク**（`coremark` / `membench`）は入口で ThreadX tick と SCU の CM55M 周波数を
  検査し、駄目なら実行拒否、実行後に再読み出しして動いていたら警告（`cmds/bench_gate.c`）。
  SCU の値自体の正しさは検証できない（独立した時間源が無い。DWT を tick で較正するのは
  循環）ので、結果は「明示したクロックの下での実測値」。スコアは MEM_STATIC / TCM 配置 /
  スカラビルドとセットでのみ比較可能。membench のバッファは NOLOAD なので測定前に
  明示初期化が要る。MPU / キャッシュ属性は decode せず生ダンプ。
- **MVE**: 自作コードで MVE intrinsics/自動ベクトル化を使わない（ベンチの TU は
  `-fno-tree-vectorize`。ポストリンクの `check_mve_predication.py` が検査）。
  **[!] 禁じている根拠は誤り**（#42）— ハードウェアが VPR を保存する（Armv8-M ARM の
  PushStack/PopStack が `HaveMve()` の下で退避・復元し、規則 RZWQX により MVE 命令は
  `CONTROL.FPCA` を立てる）。ゲートは fail-closed なので **#42 が判断するまでは維持**する。
- **SPI LCD**（Waveshare 2inch / ST7789VW、M-G3a / #30）: SSPIM を **PB7(DO)/PB8(CLK)**
  へ mux、**CS=PB11 / DC=PB6 / RST=PA0 / BL=PA2 は GPIO**。
  **[!] 配線は pad 番号で数える。刻印は XIAO のピン位置ラベルで HX6538 の信号名ではない** —
  `CLK`/`MISO`/`MOSI` は microSD バス（PB4/PB3/PB2）、さらに **`TXD` は pad 7 (PB6) /
  `RXD` は pad 8 (PB7) で機能名と逆**（実際に DIN と DC を入れ替えて配線し 1 セッション溶かした）。
  pad 番号の確認は **`lcd off`**（= バックライト PA2。PA2 だけが無計測で導通を確認できる。
  点灯しているだけでは 2.2k プルアップのせいで何の証明にもならない）。
  CS を GPIO にするのは RAMWR 中に CS を保持するため。フレームは `spi_write_ptl()` 1 発
  （内部で DMA の circular LLI に落ちる。`spi_write_dma` の 4095B 上限は回避）。
  **[!] DMA 完了コールバックは「FIFO に渡した」であって「ワイヤに出した」ではない** —
  DC/CS を動かす前に `SR.BUSY==0 && SR.TFE==1` を待つ。
  フレームバッファとコマンドのバウンスバッファは **SRAM の `.lcd_fb`（NOLOAD）**
  — TCM は DMA から見えない（スタックも .rodata も不可）。**fps は完了条件にしない**。
  GPIO は **PL061 系**（`+0x000`〜`+0x3FC` はアドレスがビットマスクのデータレジスタ、
  方向は `+0x400`）。詳細は board README。
- **カメラ**（IMX219 / MIPI CSI、M-G3b / #35）: データパスは固定
  （3280x2464 RAW10 2 lane → INP crop → 10:2 binning → 4:2 subsample → 320x240 →
  HW5x5 demosaic BGGR → WDMA3。`tflm_yolov8_od` の出荷構成）。
  **WDMA3 はプレーナ B/G/R で、パックはソフト**（HXCSC は入力アンパッカー）。
  **[!] DMA が触るバッファは TCM 不可**（`.cam_raw` / `.cam_slots` は SRAM NOLOAD、
  placement gate が pin）。**WDMA3 は frame-ready 後・読取前に全長 invalidate**
  （ベンダのグルーはやっていない）。**停止は単一ルーチン、再開はバリア
  （静止 → クリア → 再 arm）** — callback は status しか持たないので世代番号では
  遅延イベントを弾けない。**エラーはフレームより優先・未知の負値は terminal**。
  `lcd_blit` は BE / pipeline は LE で、swap は `lcd_blit_le()` が持つ。
  **EPK 容量 32**（`GROVE_EPK_WRAP_MAX` == `TX_GLUE_EPK_MAX_IRQ`）。
  **Timer0 の割込み到達は probe で検証済み**（M-G3a 申し送りを解消。PRIMASK 外で
  実行し、失敗したら bring-up ごと拒否）。詳細は board README。
- ポストビルドゲート 4 本（`boards/grove-vision-ai-v2/cmake/`）: イメージ整合
  （生成 `.img` と ELF の突き合わせ + `.rodata` 内のコマンドレジストリ検証）/
  配置・予算（ITCM/DTCM headroom、ベクタ常駐、静的スタック、禁止シンボル残存、
  測定・表示バッファの常駐）/ MVE 述語命令スキャン /
  **timer seam**（`check_timer_seam.py`。カメラアーカイブ込みの `seam_probe` リンクで
  「ベンダ timer コードが 1 バイトも残らない」ことを検査。negative test は
  `cmake/fixtures/run_fixture_tests.py`）。**外す・弱める変更は不可**
  （f746/wio のゲートと同格）。
- **推論（`nn` / #44）**: TFLM は**ソースからビルド**（プリビルトは CMSIS-NN 版のみ＝MVE を
  持ち込む）。op resolver は `AddEthosU()` 1 個で **CPU カーネル 0 本**。実リンク量 15,360 B。
  **[!] フラッシュのメモリマップ読み出し窓はアプリが開ける** — リセット時は死んでおり、
  しかもフォルトも 0xFF も返さず**窓全体が同一レジスタにエイリアス**する
  （`hx_lib_spi_eeprom_open` + `enable_XIP`）。その open は **DMAC1 の IRQ 133 を有効化する**
  （EPK スナップショットが実測で捕捉。番号を列挙せず測る方式の存在理由）。
  **[!] `lib_spi_eeprom.a` の erase/write 系は禁止シンボル** — このフラッシュには
  ブートローダが載る（wio のセクタ0 と同格）。`setWriteEnable` のみ QUAD 有効化に必要なので許可。
  **[!] アリーナの保守は「範囲ごと」ではなく「全体を 2 点で切り替える」**（#46）。
  TFLM の確保は 16 B 整列 / キャッシュラインは 32 B なので、範囲ごとの外側丸めは隣の半ラインを
  巻き込む。しかも NPU は中間 FM をアリーナ全体に書き、CPU は ethos-u カーネルのスクラッチ
  （アリーナ内）と永続アロケータを書くので、公開 I/O を測っても境界は覆えない。
  **潰すのは `ethosu_invalidate_dcache()` だけ**（完了セマフォより前に呼ばれる）で、
  **`ethosu_flush_dcache()` はタイミングが正しいので本物に戻す**。引き渡しは
  `ethosu_inference_begin/end`（weak・`drv` を受け取る）に置き、成功条件は
  **`job.state == DONE` かつ `job.result == OK` の両方**（fault でも DONE になり、result は
  OK で初期化される）。異常時は `ethosu_soft_reset()` の**成功を確認してから** invalidate、
  失敗なら fail-stop。**呼び出し側でキャッシュ保守をしない**（TFLM は `Invoke()` 復帰前に
  アリーナを書く）。実測コストは +1 ms（91→92 ms）。
  **[!] `npu_open()` はペイロードを検査する** — `COMMAND_STREAM` が 1 個かつ最後でなければ拒否。
  ドライバは launch 後もアクション解析を続け、失敗すると `ethosu_wait()` を通らずに戻れるため。
  **検査対象は `custom_options` ではなく入力テンソル 0**（前者は CO_TYPE マーカー 3 B）。
  **`is_variable()` は拒否**（`AllocateVariables()` がシリアライズ済みバッファでも
  アリーナ確保で上書きする）。
  NPU bring-up は SEC_ONLY 経路に無いので自前（読み戻し + fail-closed）。詳細は board README。
- **ライブ推論オーバーレイ（`nn preview` / #48）**: 推論は**カメラ producer スレッド上・
  sink の `consume()` 内**で走る（WDMA3 の再 arm は全 sink の consume 後なので raw フレームは
  安定 = 枠は必ず表示中のフレームのもの）。順序は**推論（パネルガード無し）→ ガードを 1 回
  取って stage/draw/present**（`lcd_blit_le_overlay()` の callback は staging と DMA の間。
  ガードは再帰的なので再入は deadlock ではなく**進行中トランザクションの破壊**になる）。
  [!] **`camera_stream_stop()` は成功時のみ join を保証する**（`CAM_OK` = producer 停止済み。
  timeout は何も証明しない）。**全ての呼び出し元は `CAM_OK` の時だけ detach する**
  （`camera preview` も同様。従来は無条件 detach で、これは既存のバグだった）。
  未確認の join は **`CAM_ST_LOST`**（**再起動まで全ハードウェア操作を拒否**。
  `CAM_ST_FAULTED` は「次の bring-up で作り直す」= ここでは最悪の動作なので使えない）で、
  この経路では **detach も teardown も `nn` gate 解放もしない**（overlay/sink の状態が
  全て静的だから成立する）。overlay の stop-pending は **join 要求より前に**立て、
  前処理前 / invoke 直前 / 推論後の 3 点で見る（実行中の invoke は取り消せない）。
  **推論タイムアウトの定義は `port/npu/npu_hw.h` の 1 箇所**（1000 ticks。board.cmake が
  parse する。従来は 2 箇所に書かれ片方が dead だった）。producer スタックは **8 KiB**
  （`Invoke()` を載せたため。実測 high-water で確認する）。
  **[!] EPK は 31/32 に達する**（camera 26 + UART0 1 + LCD 2 + QSPI/U55 2）。
  `nn run` はパネルを上げないので、5 者が同時に載るのは `nn preview` が初。
- **顔検出（`nn detect` / #45, #48）**: BlazeFace-front 128 を Ethos-U55 で。
  **op resolver は `<1>` のまま維持する。[!] 理由は「CPU op がキャッシュ的に危険」ではない**
  （#46 で消えた） — **CMSIS-NN（= Helium）を持ち込まない / Vela が全面 offload していない
  モデルを `AllocateTensors` でうるさく落とす**という設計判断。境界の型変換 5 個
  （先頭 QUANTIZE + 末尾 DEQUANTIZE 4）は**ファイル側で剥がす**
  （`scripts/tflite_strip_boundary.cc`。テンソルは削除せず孤児のまま = 番号の振り直しが不要）。
  剥がすと Vela は **CPU operators = 0**。入力量子化 scale 1/255・zp -128 は
  前処理の `pixel - 128` と一致し、`nn detect` / `nn preview` は入口でそれを**検査して拒否する**。
  **[!] 入力は 240x240（フレーム中央の最大正方形）を「スケール」する**（#48。従来は 128x128 の
  ただの中央 crop で、実用距離では検出できないほど画角が狭かった）。前処理は
  `port/npu/nn_preproc.c`（half-pixel 中心の bilinear・固定小数・**ホストテスト必須**）。
  **[!] サンプリングは半ピクセル項を持ち、箱の「辺」は持たない** — 同一の変換の
  2 つの表現で、辺に sampling の規約を当てると全部の箱が半ソースピクセルずれる
  （顔相手には見えない）。**負座標は数学的 floor**（C の 0 方向切り捨ては upscale で 1 ずれる）。
  `nn detect` の箱は**フレーム座標**で表示する（overlay と同じ関数を通すので
  コンソールとパネルが食い違わない）。
  **モデルが見るのは生の planar フレームで、パネルに出ている絵ではない**
  （WB / gamma / saturation はパネル向けの調整。gamma 付きを食わせる案は別実験）。
  **vela は `requirements.txt` で pin**（5.1.0）し `build/<board>/venv` に同居。
  ゲートは `scripts/verify_vela_model.cc`（**ファームの `npu_payload.c` と `npu_arena.c` を
  そのままリンク**するので、ホストの答えと `npu_open()` の答えがずれない）。
  実測: モデル 164,512 B / **実機 13 ms・アリーナ 394,800 B** / config word `0x00001006` は
  実績 MobileNet と**ビット一致** / arch 1.0.6。
  **ホストゲートのアリーナ値は実機より ~0.1% 大きく出る**（64 bit ホストはポインタが 8 B）が
  過大＝安全側なので補正しない。
  **[!] score が 775/1000 で固定に見えたら 8x8 群の天井**（zp 126 / scale 1.2247 で
  `q=127` の 1.2247 が上限、`sigmoid(1.2247)=0.775`。デコーダではなくモデルの量子化）。
  **#48 以降は通常こちらではない** — 画角が広がって顔が入力上で小さくなり、細かい 16x16 群
  （scale 0.036937 / zp 49、上限 2881）が拾うのでスコアは動く（実測 peak 1292 / score 781）。
  **[!] フラッシュ配置は「予約」で宣言し、検査する**: `GROVE_MODEL_{CLS,DET}_{FILE,ADDR,RESERVED}`
  （1 個の無名アドレスを使い回さない。アドレスは cmd_nn.c へコンパイル定義で渡し
  `nn open cls|det` と同一）。`cmake/check_flash_partitions.py` は**予約どうしの非重複を
  成果物ゼロでも検査**し、**存在必須なのは今から書く成果物だけ**。
  **[!] 全ファイルを要求してはいけない** — 検出モデルはライセンス上コミットできないので、
  クリーンなツリーでは**ただのファーム焼き（`--target flash`）が止まる**（実際に一度そうした）。
  比較は**ファイル範囲ではなく破壊フットプリント**（xmodem の 128 B パディング + 消去ブロック
  丸め。ブートローダの消去粒度は不明なので **64 KB = この NOR の最大消去単位**という保守側の
  境界）。cls 0xB7B000 と det 0xD20000 の予約は**ブロック単位でちょうど隣接**する。
  **モデルの flash ターゲットは staging コピーに対して 検査 → `verify_vela_model` → 送信**を
  同一ファイルで行う（検証を README の手順に残さない。ホスト C++ が無ければ skip せず拒否）。
  デコーダは **npu シングルトン非依存**（`npu_tensor` 配列を受け取る）で、
  **全 896 アンカーを必ず走査**し候補は上限付き top-N（donor は満杯で打ち切るため
  ピークが前半の最大になり、後方 384 群の最強顔を落とす）。出力 4 本は **shape で探す**。
  **4 本の scale/zp は全部違う**（8x8 のスコアは zp 126 / scale 1.22 で実質 3 値）ので
  共有の脱量子化定数を作らない。詳細は board README。
- **[!] SRAM 窓は 2 領域**（#29）: `CM55M_S_SRAM_LDR` 0x3401F000 = 2nd bootloader の実行窓で
  **NOLOAD 専用** / `CM55M_S_SRAM` 0x3404D000 = loadable 可。`.rodata` は後者。
  「CONTENTS を持つセクションが低位窓に降りていないか」は **ldscript には書けない規則**
  （ld は NOBITS を区別しない）なので `check_placement_budget.py` が ELF のフラグで検査する。
  負のテストは `cmake/fixtures/`。
- LTO は使わない（M-G1。導入するならゲート再設計とセット）。**実測（#40 Step 1.5）: LTO は ITCM を 3,616 B 増やす** —
  ITCM の 63% が IR を持たないプリビルトで、届く範囲が狭く元が取れない。

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

### [!] ボード固有の説明は `boards/<board>/README.md` に書く

**各ボードは `README.md` を持ち、そのボードの説明の正とする。** ここに書くもの:
ビルド/書込/コンソールの手順、ピン配置・メモリマップ、ブート経路、ボード固有の
ハマりどころ（例: Grove の「シリアルを開くとリセットする」= RTS が 220nF で
RESETN に結合されている件）、復旧手順、そのボードのゲートと未確認事項。

**分担**: 「そのボードがどう動くか」は board README。「エージェントが破ってはいけない
不変条件」は CLAUDE.md /「ボード固有ルール」節と `AGENTS.md`（ここは要点と禁止事項に
留め、詳細は board README を指す）。ルートの `README.md` は対応ボード表と共通手順のみで、
ボードの詳細は board README へリンクする。**同じ事実を 3 箇所に写経しない** — 増えるほど
食い違って、どれが正か分からなくなる。

3 ボードとも `boards/<board>/README.md` を持つ（#23 で規約化、#24 で f746g-disco と
wio-lite-ai を移設）。CLAUDE.md の各ボード節は**破ってはいけないことだけ**を残し、
説明は board README を指す。
