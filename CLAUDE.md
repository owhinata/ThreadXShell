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

**両リポジトリの shell をボード非依存コアとして統合する**という当初の目標
（2026-08-11 ユーザー決定）は達成済み。以降は**ボードを足しながら、そのコアを
ボード非依存に保つ**段階にある。

統合フェーズ（完了）— 確定済みの方針:

- **shell 統合のベースは wio-lite-ai**（最新実装。core の差分は `cli_core.c` 63 行 /
  `cli_parse.c` 105 行のみで、乖離の本体は cmds と backend）。F746 側の uart backend と
  F746 専用 cmds（fs/gui/qspi/sdram 等）は移植で追加した。
- **移植順**: M1 = 骨組み + Wio Lite AI ポート（USB CDC で shell 起動、完了 #2）→
  M2 = F746 ポート（完了 #3）→ **M3 = boot ツリー取り込み（ビルドのみ、焼かない。完了 #11）**。
- **Wio Lite AI の DFU ブートローダは本リポジトリに統合済み**（`boards/wio-lite-ai/boot/`、
  移植元 `owhinata/wio-lite-ai` @ `09468bb`）。app とソースを共有しない独立ツリーで、
  **統合後も不変扱い**（下記「ボード固有ルール」参照）。
- **M4 = 全ボードビルドのスクリプト/CI 化は保留**。着手条件は Epic #1 のコメント:
  枚数やマイルストーンではなく、**手動運用が実際に破れたとき**（全ボードビルドを飛ばして
  共有コアの回帰を実際に踏んだとき / 自分以外がこのリポジトリにコミットするようになった
  とき）。**ボードが増えたことだけを理由には着手しない。**

Grove Vision AI V2（3 枚目、初の非 STM32。完了）:

- M-G1 = bring-up（#22）→ M-G2 = CoreMark / membench / thread cpu% / WFI（#25）→
  M-G3a = SPI LCD（#30）→ M-G3b = カメラ（#35）→
  M-G3c = Ethos-U55 推論（#44 / #45 / #46 / #48）。

ビルドは `-DBOARD=<board>` で 1 ビルドディレクトリ = 1 ボード（既定なし）。ボードごとの
submodule 一覧は `boards/<board>/submodules.cmake`。3 ボードのビルド / フラッシュ、および
boot ツリーの正は**本リポジトリ**（元リポジトリではない）。

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
- **[!] `nn` は 3 ボード共有の 1 コマンド**（#50。`shell/cmds/cmd_nn.c` +
  契約 `svc/nn_svc.h` + ボードごとの `nn_svc_*.c` アダプタ）。破ってはいけないこと:
  - **共有 TU（`cmd_nn.c` / `nn_cmd_core.c`）は可変記憶域を 1 バイトも持たない** —
    `check_no_mutable_storage.py` が**ボードごとの監査コンパイル**で強制する
    （デコーダ #97 と同じ理由・同じスクリプト。ホストの答えは別物なので当てにしない）。
    状態はアダプタが持つ。
  - **capability マクロは「性質」であって「ボード名」ではない**
    （`boards/<board>/svc/nn_svc_config.h`）。`#ifdef <BOARD>` の言い換えを作らない。
    バックエンドで変わる能力は `CONFIG_NN_BACKEND` に従わせる。
  - **status と claim disposition は別フィールド**。`retryable`（再試行で解決しうる）と
    `terminal`（再起動しかない）を畳まない — 畳むと再起動不要のボードで再起動させ、
    その逆もやる。**判断できないボードは `terminal` に fail-closed**。
    disposition は「いま誰が持っているか」ではなく**呼び出し側の解放権限**である。
  - **モデル指定はタグ付き**（`--name` / `--slot` / `--path` / `builtin` /
    `--addr <a> <len>`）。**裸の文字列は拒否する** — 同じ語がボードごとに別物を指すので、
    受け付けた瞬間に shell がボードを知ることになる。`--addr` の長さは必須。
  - **port のアダプタは `struct cli_instance` を取らない**。印字・待ち・キャンセル判定が
    要るものは `boards/<board>/cmds/` に置く（そこは shell 層）。port が上を名指ししない。
  - **ライブ推論は 3 ボードとも `nn stream start/stop/stats` の 1 文法**（#99 で統一、
    `preview` は削除済み。**復活させない**）。`start` は非ブロッキングで、待ちは
    共有コマンドの `--frames <n>` が 1 実装で持つ。
  - **[!] stream には世代がある**（#99）。`start` が返す generation を待ち手が持ち、
    `stop` はそれを**遷移を claim するのと同じクリティカルセクション内で照合する**。
    `NN_STREAM_GEN_ANY` は操作者が打つ `nn stream stop` 専用で、**待ち手は絶対に渡さない**
    — 渡すと「自分が起こしていない stream」を畳んでカメラ / NPU / バスガードを他人から
    奪う。**世代の照合と stop の claim は 1 呼び出し**（分けると 2 者が同じ stream に入る）。
    機械は `svc/nn_stream_life.c` の 1 本で、3 ボードが状態だけ持つ。
  - **[!] start の admission も機械が持つ**（#99 の 2 巡目）。**下位の worker を触る前に
    STARTING を claim し、失敗したら abort する**。後から記録すると「worker は動いて
    いるのに phase は IDLE」の窓ができ、wio の re-arm はその窓で**進行中の stop を
    上書きする**。`commit()` は STARTING 以外を拒否する（LOST を蘇生させないため）。**finish/retry/poison も
    STOPPING 以外を拒否**する — 機械が規律に頼るなら共有した意味が無い。
  - **[!] worker のカウンタは世代と一致しない**（#99 3 巡目）。wio は re-arm でカウンタを
    意図的に継続するので、**`nn_stream_stats` は commit 時に基準を latch して差を出す**。
    素通しすると `--frames 10` が re-arm 直後に即成立して、自分が上げた stream を止める。
    **re-arm は `nncam_record_reset()` で decode record も retire する**（バンド再取得の前）—
    さもないと前世代の顔と、境界を跨いで publish される推論が新世代の成果になる。
  - **[!] 遷移が拒否されたら wrapper の副作用も走らせない**（#99 4 巡目）。
    `finish/retry/poison/abort` は成否を返し、claim の解放や elapsed の凍結はその成功時のみ。
    黙って拒否するだけのガードは、守るはずの不変条件違反でこそ fail open する。
  - **[!] poll は 2 相 + 遷移カウンタ**（#99）。数値は自分のロックを持つサブシステムから
    来る（Grove のカメラ統計は frame pipeline の mutex に落ちる）ので、
    **割込み禁止下では集められない**。世代と状態だけでは足りない — **retryable な stop は
    世代を保ったまま状態を戻す**ので、両方が変わらないまま teardown 1 回ぶんを跨げる。
  - **[!] retryable と terminal の分類は「その時点で何ができるか」で決まる**（#99）。
    Grove の `CAM_ERR_LOCKED` は #99 以前 reboot 扱いだったが、その根拠は
    「sink を持つコマンドとは別に stop できるコマンドが無い」ことで、#99 がそれを作った。
    detach の `CAM_ERR_BUSY` も #79 が retryable と決めている。**terminal に畳み直さない。**
    表は `boards/grove-vision-ai-v2/port/npu/nn_stream_state.c`（純関数・ホストテスト必須）。
  - **[!] 未文書の stop コードは terminal に fail-closed**（#99 の adversarial review）。
    catch-all の retryable は「証拠が無い戻り値」を回復可能と約束してしまう。
    既定は `nn_stream_disp_of()` が持ち、ボードは**表だけ**を出す。
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
  外付け NOR の耐久 ~100k。**自動ループで焼かない**。
  [!] **その数字は回路図が指定する W25Q128JWSIQ のデータシート由来で、実装品は
  Zbit ZB25LQ128C**（#89。JEDEC `5e 50 18`、刻印で確認。1.8V/128Mbit のピン互換二次ソース）。
  ルールは変わらないが、**耐久も 32 KB 消去の可否も文書で裏が取れていない**
  （`_ref/` に ZB25LQ128 のデータシートは無い）。#88 の境界チェックが decode する対象。復旧 = チップ内 boot ROM +
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
- **MVE は解禁済み**（#42）。禁止の根拠だった「移植が VPR を保存しない」は誤りで、
  **ハードウェアが保存する** — Armv8-M ARM の FP フレームが `HaveMve()` の下で
  offset 0x44 に VPR を積み、規則 RZWQX により MVE 実行が `CONTROL.FPCA` を立てる。
  ThreadX 側は callee-saved の `{s16-s31}` を保存する（`__ARM_FP` 下、拡張フレームがある時）。
  `check_mve_predication.py` は**削除**（そもそも #66 のとおり 1 命令も検出できなかった）。
  [!] **成立条件は `FPCCR.ASPEN`** で、これはブートローダ継承値。**カーネル入場前に
  強制 → 読み戻し → 駄目なら halt**（`port/threadx/fp_enforce.c`。判断は純関数で
  ホストテスト済み、`check_placement_budget.py` がシンボル存在を要求し、
  `cmake/fixtures/` の P2 が「呼び出しを消すとゲートが落ちる」ことを実証する）。
  **継承 `LSPACT` は拒否**（クリアしない — 所有していないフレームの帳簿）。
  実機で見る手段は **`mve` コマンド**（高優先度の汚染役スレッドを立て、
  q4-q7 と VPR がコンテキストスイッチを跨いで生き残ることを確認する。
  sleep だけでは誰も上書きしないので**壊れた系でも通ってしまう**）。
  **CoreMark の TU だけ `-fno-tree-vectorize` を残す** — MVE 禁止ではなく、
  公表値 3.13 CoreMark/MHz との**基準線の連続性**のため。
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
- **カメラ**（OV5647 / MIPI CSI、M-G3b / #35）: データパスは固定
  （640x480 RAW10 2 lane（センサ側でビニング済み）→ INP crop 無し → 4:2 binning →
  320x240 → HW5x5 demosaic BGGR → WDMA3。donor の OV5647 出荷構成）。
  **IMX219 は #54 で削除**。ソフト自動露出（`cam_ae_step`）・`camera depth`・
  `camera exposure` の frame_lines 引数も一緒に消えた。`camera auto` は
  **センサのオンチップ AEC + このポートのソフト WB** の意味。
  **WDMA3 はプレーナ B/G/R で、パックはソフト**（HXCSC は入力アンパッカー）。
  **[!] DMA が触るバッファは TCM 不可**（`.cam_raw` / `.cam_slots` は SRAM NOLOAD、
  placement gate が pin）。**WDMA3 の landing buffer は 2 面**（#59。450 KB、
  arm はイテレーション先頭で他面へ。チャネルアドレスを書くのは `cam_wdma3.c` だけ・
  xDMA disable 中のみ・マスクは WDMA3 専用ペアで save/restore —
  `hx_drv_xdma_set_mask()` 不可。監査は fail-closed で緩め不可）。
  **frame-ready 後・読取前に完了面だけ全長 invalidate**
  （ベンダのグルーはやっていない）。**停止は単一ルーチン、再開はバリア
  （静止 → クリア → 再 arm）** — callback は status しか持たないので世代番号では
  遅延イベントを弾けない。**エラーはフレームより優先・未知の負値は terminal**
  （#59 以降は publish 直前にも sticky ラッチを再読する）。
  `lcd_blit` は BE / pipeline は LE で、swap は `lcd_blit_le()` が持つ。
  **EPK 容量 32**（`GROVE_EPK_WRAP_MAX` == `TX_GLUE_EPK_MAX_IRQ`）。
  **Timer0 の割込み到達は probe で検証済み**（M-G3a 申し送りを解消。PRIMASK 外で
  実行し、失敗したら bring-up ごと拒否）。詳細は board README。
- ポストビルドゲート **4 本**（`boards/grove-vision-ai-v2/cmake/`。#42 で MVE 述語
  スキャンを削除し、#88 で NOR seam を足した）: イメージ整合
  （生成 `.img` と ELF の突き合わせ + `.rodata` 内のコマンドレジストリ検証）/
  配置・予算（ITCM/DTCM headroom、ベクタ常駐、静的スタック、禁止シンボル残存、
  **必須シンボル残存**（#42。`--gc-sections` が未呼び出し関数を落とすので、
  存在＝呼ばれている）、測定・表示バッファの常駐）/
  **timer seam**（`check_timer_seam.py`。カメラアーカイブ込みの `seam_probe` リンクで
  「ベンダ timer コードが 1 バイトも残らない」ことを検査）/
  **NOR seam**（`check_nor_seam.py`、#88。ベンダの erase/program に届いてよいのは
  `port/sdk_seam/nor_seam.c` だけ。判定は **ELF ではなく ld の map**）。
  negative test はいずれも `cmake/fixtures/run_fixture_tests.py`。
  **外す・弱める変更は不可**（f746/wio のゲートと同格）。
- **推論（`nn` / #44）**: TFLM は**ソースからビルド**（プリビルトは CMSIS-NN 版のみ＝MVE を
  持ち込む）。op resolver は `AddEthosU()` 1 個で **CPU カーネル 0 本**。実リンク量 15,360 B。
  **[!] フラッシュのメモリマップ読み出し窓はアプリが開ける** — リセット時は死んでおり、
  しかもフォルトも 0xFF も返さず**窓全体が同一レジスタにエイリアス**する
  （`hx_lib_spi_eeprom_open` + `enable_XIP`）。その open は **DMAC1 の IRQ 133 を有効化する**
  （EPK スナップショットが実測で捕捉。番号を列挙せず測る方式の存在理由）。
  **[!] `lib_spi_eeprom.a` の erase/write 系と、任意オペコード送出 4 本
  （`Send_Op_code` / `Send_Op_Read_Data` の spi/qspi 両形、#87）は禁止シンボル** — このフラッシュには
  ブートローダが載る（wio のセクタ0 と同格）。`setWriteEnable` のみ QUAD 有効化に必要なので許可。
  [!] **ただしこの absence 検査は defence in depth であって証明ではない**（#87）— 読み出し経路が
  `hx_drv_spi_mst_get_dev` / `hx_drv_dmac_get_dev` / `DMA_send` を既に引き込んでおり、
  禁止リストのどの名前にも触れずに WREN + 任意オペコードを組める。**「リストが通った」を
  「書込み能力が無い」と読まない**。**#88 Part D の seam / ゲートを足しても同じ**で、
  変わったのは問いが「イメージに在るか」から「誰が届いてよいか」になったことだけ。
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
- **モデルは blob の名前で開く（`nn model load --name <name>` / #93 = #49 Step 4a）**:
  **[!] `npu_open()` は長さを取り、`GetModel()` の前に境界付き FlatBuffer verifier を通す。**
  `GetModel()` は cast で以後の accessor は offset を辿るだけ。**blob の CRC は「届いた
  バイト列」に対するもの**なので、PC 側で既に壊れていたモデルは CRC を通って無傷で着く。
  順序は **範囲 → 長さ → identifier → verifier → ペイロード走査**。長さには**下限も要る**
  （identifier を `raw+4` から読む）。**生アドレス形にも長さ必須**
  （`nn model load --addr <addr> <len>`。「窓の残り全部」は境界検査にならない）。
  **[!] verifier の limits は明示で、既定を使わない** — 既定は depth 64 / table 100 万、
  生成 verifier は再帰し**シェルスタックは 4,096 B**（実測: 再帰フレームは 24 B 以下＝
  1 段 ~64 B なので既定の 64 段は ~4.1 KB ＝スタック全部）。実モデルは **depth 4 / table 19**、
  出荷値は 16 / 4096（「Vela が全面 offload していないモデル」は op resolver で
  refuse されるべきで、ここで malformed と報告してはいけない）。
  **limits と呼び出しは `port/npu/npu_verify.h` の 1 箇所**でホストゲートが同じものを
  リンクする（既定 limits でホストが通し実機が落ちる、を作らない）。ITCM +12,000 B。
  **[!] `nn model load --name <name>` はリースを切らさない**: nn ゲート → **`npu_hw_init()` を先に**
  （`NOR_LEASE_NPU` 確保）→ 内側で**全スロット走査**（候補は **VALID のみ**・**重複拒否**・
  0 件 / BUSY / FAULT / MAP を**別々に分類**し「見つからない」に畳まない・**読めない
  スロットが 1 つでもあれば拒否**）→ **`blob_verify_leased()` で CRC** → `npu_open()` →
  **途中失敗は必ず `npu_hw_deinit()`**。`blob_stat_leased` / `blob_verify_leased` は
  **呼び出し元のトークンを取り live か検査する**（「誰かがリースを持っている」は他人の
  寿命の話）。`blob_verify_leased` が **`NOR_LEASE_BLOB` も取るのは窓ではなく
  `blob_stage_buf` の排他**のため。
  **[!] ホスト側の検証ゲート（`verify_vela_model`）を外さない** — デバイスの境界検査は
  置き換えにならず（単一サブグラフ / 全 op が Ethos-U / int8 I/O / offline plan /
  アリーナ / BlazeFace の shape を見ない）、しかも**書込みの後**に走るので malformed でも
  既に ~40 秒の消去と転送を消費している。公式経路は
  **`build/<board>/send_verified_model.sh`**（picocom の `--send-cmd`）で
  **staging コピー → 検証 → 同一ファイル送信**、**`--profile cls|det` は明示引数**
  （ファイル名から推測しない）、**出力は stderr**（YMODEM 線に流さない）、
  **ホスト C++ 不在は fail-closed**。
- **ライブ推論オーバーレイ（`nn stream` / #48、#99）**: 推論は**カメラ producer スレッド上・
  sink の `consume()` 内**で走る（モデルが読むのは完了済みの landing buffer で、
  次の frame-ready まで面は flip しない — #59 で 2 面化して「consume まで」から
  「イテレーション全体」に強まった = 枠は必ず表示中のフレームのもの）。順序は**推論（パネルガード無し）→ ガードを 1 回
  取って stage/draw/present**（`lcd_blit_le_overlay()` の callback は staging と DMA の間。
  ガードは再帰的なので再入は deadlock ではなく**進行中トランザクションの破壊**になる）。
  [!] **`camera_stream_stop()` は成功時のみ join を保証する**（`CAM_OK` = producer 停止済み。
  timeout は何も証明しない）。**全ての呼び出し元は `CAM_OK` の時だけ detach する**
  （`camera preview` も同様。従来は無条件 detach で、これは既存のバグだった）。
  [!] **センサーバスの所有者は mutex 保持下で決める**（#74 / #77。producer は API mutex を
  取らないので、コンソールをバスから遠ざけているのは**状態検査**の方。`camera_stream_start()` が
  **保持下で** `CAM_ST_STREAMING` を publish する以上、**acquire より前の検査は無価値**）。
  入口は **`cam_bus_enter()` 1 本**で、`cam_bus_decide()` が**「誰が持っているか」を返す**
  （「何をすべきか」ではない）。`CAM_OK` ⇒ **mutex 保持** + owner は DIRECT / PRODUCER のみ。
  **bring-up はヘルパに入れない**。**`CAM_ST_LOST` は保持下で到達する**ので direct に落とさない
  （`cam_bringup()` がポートを再構築する）。判定は**状態を列挙**する（「STREAMING 以外」は
  fail open）。詳細は board README。
  [!] **stop だけが API mutex を有界待ちする**（#65。他の入口は `TX_NO_WAIT` のまま —
  両方向に戻さない）。**poison の判定は待ちの向こう側でもう一度行う**（`CAM_ST_LOST` は
  「not streaming」でもあるので、近道を前に置くと起きていない stop を成功と報告する）。
  この順序は `port/camera/cam_state.c` の純関数が**唯一の判断点**で、
  `camera_stream_stop()` 側に近道を書き戻さない（実機では作れない分岐なので、
  `test/test_cam_stop.c` だけが検査できる）。取得できなかった場合は
  **`CAM_ERR_LOCKED`（-8。このボード固有）で poison しない**（何も聞いていない）。
  未確認の join は **`CAM_ST_LOST`**（**再起動まで全ハードウェア操作を拒否**。
  `CAM_ST_FAULTED` は「次の bring-up で作り直す」= ここでは最悪の動作なので使えない）で、
  この経路では **detach も teardown も `nn` gate 解放もしない**（overlay/sink の状態が
  全て静的だから成立する）。overlay の stop-pending は **join 要求より前に**立て、
  前処理前 / invoke 直前 / 推論後の 3 点で見る（実行中の invoke は取り消せない）。
  **推論タイムアウトの定義は `port/npu/npu_hw.h` の 1 箇所**（1000 ticks。board.cmake が
  parse する。従来は 2 箇所に書かれ片方が dead だった）。producer スタックは **8 KiB**
  （`Invoke()` を載せたため。実測 high-water で確認する）。
  **[!] EPK は 31/32 に達する**（camera 26 + UART0 1 + LCD 2 + QSPI/U55 2）。
  `nn run` はパネルを上げないので、5 者が同時に載るのは `nn stream` が初。
- **顔検出（`nn run` / #45, #48）**: BlazeFace-front 128 を Ethos-U55 で。
  **op resolver は `<1>` のまま維持する。[!] 理由は「CPU op がキャッシュ的に危険」ではない**
  （#46 で消えた） — **CMSIS-NN（= Helium）を持ち込まない / Vela が全面 offload していない
  モデルを `AllocateTensors` でうるさく落とす**という設計判断。境界の型変換 5 個
  （先頭 QUANTIZE + 末尾 DEQUANTIZE 4）は**ファイル側で剥がす**
  （`scripts/tflite_strip_boundary.cc`。テンソルは削除せず孤児のまま = 番号の振り直しが不要）。
  剥がすと Vela は **CPU operators = 0**。入力量子化 scale 1/255・zp -128 は
  前処理の `pixel - 128` と一致し、`nn run` / `nn stream` は入口でそれを**検査して拒否する**。
  **[!] 入力は 240x240（フレーム中央の最大正方形）を「スケール」する**（#48。従来は 128x128 の
  ただの中央 crop で、実用距離では検出できないほど画角が狭かった）。前処理は
  `port/npu/nn_preproc.c`（half-pixel 中心の bilinear・固定小数・**ホストテスト必須**）。
  **[!] サンプリングは半ピクセル項を持ち、箱の「辺」は持たない** — 同一の変換の
  2 つの表現で、辺に sampling の規約を当てると全部の箱が半ソースピクセルずれる
  （顔相手には見えない）。**負座標は数学的 floor**（C の 0 方向切り捨ては upscale で 1 ずれる）。
  `nn run` の箱は**フレーム座標**で表示する（overlay と同じ関数を通すので
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
  **[!] フラッシュ配置は「予約」で宣言し、検査する**。**#93 以降 cmd_nn.c はアドレスを
  持たず**（`nn model load --name <name>` が blob ヘッダから取る）、**#94 でモデル予約そのものが消えた**:
  `GROVE_MODEL_{CLS,DET}_{FILE,ADDR,RESERVED}` / `model-cls` / `model-det` / `blob-tail` /
  `--target flash-model-*` は**削除済みで、復活させない**。パーティションは
  **firmware / blob / slot-header の 3 つ**。
  `cmake/check_flash_partitions.py` は**予約どうしの非重複を
  成果物ゼロでも検査**し、**存在必須なのは今から書く成果物だけ**。
  **[!] 全ファイルを要求してはいけない** — 検出モデルはライセンス上コミットできないので、
  クリーンなツリーでは**ただのファーム焼き（`--target flash`）が止まる**（実際に一度そうした）。
  比較は**ファイル範囲ではなく破壊フットプリント**（xmodem の 128 B パディング + 消去ブロック
  丸め **4 KB**）。**[!] 粒度は #88 で確定した実測値** — 常駐 2nd BL の range eraser は
  `addr & ~0xFFF` を 4 KB 刻みで歩き、enum 0 しか渡さない。`0x52`/`0xD8`/chip erase は
  1 度も発行されない。**全 receiver への上界ではなくこの経路の実測値**なので、将来
  大きいブロックで消す writer が現れたらこの丸めは守らない。
  **モデルの送信は staging コピーに対して 検査 → `verify_vela_model` → 送信**を
  同一ファイルで行う（検証を README の手順に残さない。ホスト C++ が無ければ skip せず拒否）。
  #94 で flash ターゲットが消えた後もこの鎖は `build/<board>/send_verified_model.sh` に残る。
  **[!] firmware 予約は 2 MB で、ブートローダ自身の算術から導出する**（#85。A/B 2 スロット ×
  `Image max size 0x100000`。`GROVE_FW_SLOT_SIZE` × `GROVE_FW_SLOTS` で、`0x200000` を
  ベタ書きしてコメントで説明しない）。**[!] `GROVE_FLASH_SIZE` / `GROVE_ERASE_GRAN` / `GROVE_SLOT_HDR_COPIES` /
  `GROVE_FW_SLOT_SIZE` / `GROVE_FW_SLOTS` は実測値でありノブではない** — `cmake/flash_geometry.cmake`
  が非キャッシュ変数として持ち、食い違う `-D` は configure 時に **FATAL_ERROR**。
  CACHE にすると**レイアウトの宣言とその検査が同じ値から出ているので、`-D` 1 つで規則と検証が
  一緒に動いて検査は OK のまま通る**（4 通り全部実測。adversarial review #85 の指摘）。
  **キャッシュ化に戻す変更は不可。** 拒否そのものが強制なので `test/test_flash_geometry.py` が
  実 `cmake` で 6 通りを落として確認する。
  **[!] `GROVE_ERASE_GRAN` と `GROVE_SLOT_HDR_COPIES` は別の事実で、束ね直さない**（#88）。
  スロットヘッダ予約を「1 消去ブロック」から導くと、粒度を実測の 4 KB へ絞った瞬間に予約が
  1 セクタに縮み、**backup ヘッダ（`flash_end-0x2000`）が blob（当時の blob-tail）に
  落ちる** — `flash_geometry.cmake` が fail-open として名指しで却下している当のもの。
  予約は 2 スロット分なので、**1 イメージが 1 スロットに
  収まることは `--image-max` で別に検査する**（無いと 1〜2 MB のイメージがビルドを通り、
  実機で `ERR_IMAGE_SZ` になる）。**焼き先はスロット交替**なので `0x0` も `0x100000` も
  ファームで、どちらか一方が「正」ではない。
  **`0x200000..0xFFE000` は blob 予約の一本**（14,671,872 B。#94 = #49 Step 4b で
  モデル予約と blob-tail を畳んだ）。**境界付き writer は #88 で入り**（`nor erase`/
  `nor write` と `nor_write_program()`）、**実データの書き手は #92 で入った**。
  （`0xB70000..0xB7B000` の 44 KB は #88 で blob に入り、実機の `nor scan` で全 0xFF を確認済み。）
  **[!] 4a / 4b に割ってあったのは順序が本体だったから** — 4b が予約を消すと
  writable interval が `0xFFE000` まで伸び、**スロット API だけでなく生の
  `nor erase`/`nor write` も旧モデル領域へ届く**。だから 4a（#93）で読み手を先に
  動かし、**両モデルが store で verify・名前で open・実行できることを実機で確認してから**
  4b（#94）で地図を動かした。**この順序を後から緩めない。**
  **[!] 旧モデルのコピーは消えていない** — `0xB7B000` / `0xD20000` のバイトは
  そのまま残る（`nn model load --addr 0x3AB7B000 1704672` で読める）。
  **スロット表は #94 で全面 re-carve した**（`0x200000` から大きい順:
  4M x1 / 2M x2 / 1M x3 / 512K x3 / 256K x2 = 11 スロット、13 MB、`0xF00000` で終端。
  `0xF00000..0xFFE000` の 1,040,384 B は**意図的に未 carve**で、需要が出たときに
  append で足す場所として空けてある）。
  **[!] 全面 re-carve は規則ではなく一度きりの支払い** — identity は基底アドレスなので
  通常許されるのは **append だけ**（動いたスロットは、そこに入っていた blob の
  ヘッダが名乗る基底を表の誰も持たなくなり到達不能になる）。#94 でこれをやったのは、
  当時 store に載っていたのが `cls`（基底 `0x200000` が変わらないので**そのまま生き残る**）と
  `det` + テストファイル 1 つだけで、**再送コストが小さい唯一の瞬間だった**から。
  **次の re-carve はその時点で載っている物を全部払う。以後は append。**
  `test/test_blob_map.c` は**表全体を要素ごとに pin** し、連続性とクラス降順も検査する。
  **[!] 4 MB クラスは「実機が動かせる大きさ」で決めてあり、今載っている物とは無関係**
  （最大は 1,704,672 B のモデル）。モデルサイズを縛るのは**スロットだけ** —
  flatbuffer は XIP 窓から in-place で読まれてコピーされず、`npu_open()` の長さ上限は
  窓の残り、アリーナは重みではなく feature map のサイズで決まる（実証: **164,512 B の
  det の方が 1,704,672 B の cls よりアリーナが大きい** — 394,800 vs 385,748）。
  **[!] 表示は各スロットの「ヘッダセクタ」に何が載っているかだけで決まる**
  （旧パーティションの位置ではない）。再フラッシュ後の予測: 0 = `valid`(cls) /
  2・3 = `empty`（ペイロードに座礁した `test-small` / `det`。**`empty` は「ヘッダが無い」で
  あって「空きフラッシュ」ではない**が効く場面で、`blob write` は fresh として取り全部消す）/
  5・6 = `invalid`（旧 MobileNet が `0xC00000` / `0xD00000` のヘッダセクタを覆う。
  `blob erase <slot>` が要る）。**`model-cls` 予約の終端 `0xD20000` はモデル実体の終端
  ではない**（128 KB 先）ので、予約から推測すると外す。
  **[!] `det` と `test-small` は #94 の後に送り直した**（ヘッダが `0xAC0000` /
  `0x900000` にあり、もうスロット基底ではないため）。
  **現在の配置: `cls` = slot 1 `0x600000`（payload `0x3A601000`、crc32 `8E679A3F`）/
  `det` = slot 9 `0xE80000`（payload `0x3AE81000`、crc32 `F6DA1D1E`）。**
  cls を 4 MB スロットから退かしてあるのは、そこを「他に入らないモデル」用に空けておくため。
  実測: cls = **30 NOR トランザクション**（#49 Step 2 の予算どおり）/ det = **6**。
  **[!] blob の移動は `erase` → `write` の順**。`cls` が slot 0 で VALID のまま
  `blob write cls 1` は `DUPLICATE` で拒否される（1 名前 2 スロットの状態は作らない）。
  `blob erase` はヘッダセクタだけなので、2 コマンドの間もペイロードは生アドレスで読める
  （`nn model load --addr <payload> <len>`）。
  **最終ブロック `0xFFE000..0x1000000` は `slot-header` 予約で、絶対に書かない** —
  正体は**ブートローダのスロットヘッダ**（#85 で 1st BL の逆アセンブルにより判明）。
  `flash_end - 0x1000` と `- 0x2000`（2nd BL が焼込み後に書く backup）に 20 バイト:
  `"HIMAXWE2"` + u32 スロットオフセット + u32 + u16 + 先頭 18 バイトの u16 チェックサム。
  **アドレスはランタイム検出したフラッシュサイズからの計算・magic は `movw`/`movt` 生成**
  なので、リテラル検索では見つからない。壊してもハードブリックではない（フォールバックで
  スロット 0）が、**アクティブがスロット 1 のときは黙って前のビルドで起動する**。**地図はパートの全バイトを
  claim する** — 未宣言の run は空き容量ではなく、次のパーティションを置かれても
  止められない容量。
  [!] **ここは空ではない** — 工場 SenseCraft の FlashDB KVDB（`0x300000`）とデータ
  （`0x400000`/`0x500000`）が載っており、**最初の書込みで恒久的に消える**
  （2026-08-23 ユーザー決定で了承済み）。**我々の `lib/flashdb` とは別物**（wio は
  `FDB_WRITE_GRAN=8`、実機は `32`）。占有は 17 点のサンプリングしか見ていないので、
  **実際に書き始める前に read-only の走査が要る**。
  デコーダは **3 ボード共有の `svc/blazeface.c`**（#97）で、**推論シングルトン非依存**
  （`svc/tensor.h` の記述子配列を受け取る）。**全 896 アンカーを必ず走査**し候補は
  上限付き top-N（donor は満杯で打ち切るためピークが前半の最大になり、後方 384 群の
  最強顔を落とす）。出力 4 本は **shape で探す**。**4 本の scale/zp は全部違う**
  （8x8 のスコアは zp 126 / scale 1.22 で実質 3 値）ので共有の脱量子化定数を作らない。
  **int8 と float32 の両方**を扱う（Grove は int8、他 2 ボードは float32 で、
  float32 は affine を通さない — 2 ボードは未量子化テンソルに scale 0 を publish する）。
  [!] **共有 TU は可変記憶域を 1 バイトも持たない。** 閾値は普通の RAM に board が
  静的確保し、候補バッファは board が**自分の配置属性を付けて**注入する
  （wio `.psram_ai` / f746 `.sdram.ai` / Grove 素の `.bss`）。**両方を 1 つに
  まとめてはいけない** — 前 2 者は NOLOAD なので初期化子が載らず、しかもウォーム
  リセットを跨いで前の値が残るため「たまたま動く」形で壊れる。強制は
  `cmake/check_no_mutable_storage.py`（**ボードごとの監査コンパイル**。ホスト 1 回では
  `#if defined(__arm__)` 下の記憶域を見逃す。負のテストは `cmake/fixtures/`)。**外さない。**
  [!] **負値を 1 つに畳まない** — `-1` は「モデル非認識」専用（#57）で、未初期化と
  引数不正は別コード。**どれも「0 faces」ではない**（f746 の worker は畳んでいた）。
  診断は**毎回の結果**として返し、board が箱と一緒に publish する。
  [!] **停止は走行中の推論を取り消せない**ので、セッションに**世代番号**を持たせ、
  ワーカーは**フレームを arm / claim した時点**で控え、publish のロック内で照合する
  （`svc/nn_det_record.c`。実機では決定論的に注入できないのでホストテストが唯一の検査）。
  詳細は board README。
- **[!] plugin container（#101 = #78 Step 1a）**: モデルと、その出力を解釈するコードを
  1 blob で運ぶ。**Step 1a はロードも実行もしない** — 検証して `nn info` に出すだけ。
  説明は board README。破ってはいけないこと:
  - **`svc/plugin_load.c` は呼び出し可能なポインタを返さない**（`plugin_view` は整数
    オフセットとコピー済みバイトのみ）。「実行しない」は規律ではなく**型の性質**。
  - **ゲートは plugin ELF にも適用する**（対象外にしない）。ただし
    **メモリ安全性も渡したポインタの使用範囲も証明せず、MMIO 検査は存在しない**
    （リテラルは定数と区別できず、ペリフェラルが SRAM と同じ 0x34 窓に居る）。
    **plugin は board code と同格の信頼された native code**。
    **ダイジェストは署名ではない**（転送後の同一性のみ。由来は packer の
    プロセス制約が担保）。
  - **container は組んでから検査し、その同一ファイルを送る**。ホストは
    `verify_container` で**デバイスと同じ `svc/plugin_load.c`** を走らせる（#93 の
    「ホストで通り実機で落ちる」の再発防止）。**`--profile` / `--slot` は必須**で、
    後者はホストが知り得ない（`blob write` は**サイズヘッダ前にスロット全体を消去する**）。
  - **[!] モデル区画は 16 バイト整列**。`npu_payload.c` の 4 は flatbuffer の規則で、
    **Ethos-U ドライバは全ベースアドレスに 16 を要求する**。container 以前は
    4 KB 整列の payload アドレスに載っていたので**事故的に満たされていた**。
  - **`.plugin` は固定絶対アドレス**（`0x341E0000..0x34200000`、上端アンカー）。
    prelink されるので動かすと既存 plugin が全て無効。**ldscript と placement gate が
    独立に宣言する。**
  - **スタック上限は provisional で、1a を通った plugin は「実行して安全」ではない**
    （呼び出し地点の深さ・例外フレームの取り分・余裕が未測定。決めるのは Step 1b）。
- **[!] ベンダの NOR 書込み経路へ届いてよいのは seam だけ**（#88 Part D）。
  内側 4 本（`hx_lib_qspi_eeprom_{erase_sector,write,erase_all,word_write}`）を
  `-Wl,--wrap` で `port/sdk_seam/nor_seam.c` に寄せる。**外側 `hx_lib_spi_eeprom_*` を
  wrap しても駄目**（薄いフォワーダなので内側が直に届く）。**`erase_all` と
  `word_write` の wrapper は `__real_*` を名指ししない** — それがベンダ実装を GC させ、
  この 2 本の absence 検査を恒久的に保つ。書けるのは **`blob` だけ**、消去は
  **4 KB / enum 0 のみ**、`NOR_ST_WRITING` 以外は拒否。
  [!] **判定は ELF ではなく ld の map**（GC 後の ELF に出自は残らず、
  `spi_eeprom_comm.o` は既にリンク入力でその外側フォワーダが内側名を参照する。
  オブジェクト単位の規則は正しいリンクでも常時 fail する）。map は
  **PRE_LINK で消し BYPRODUCTS で宣言**、入力マニフェストは `$<TARGET_OBJECTS:>` から
  生成して map の `LOAD` と突き合わせ、**LTO とアドレス取得は拒否**。
  [!] **ベンダの戻り値は成否を報告しない**（`erase_sector` は WP 解除の結果で、
  その `clear_write_protect` の出口は `movs r0,#0` の 1 つだけ / `write` は定数 0）。
  **唯一の真実は読み戻し**で、それは writer の責務。ただし**負の値は wire に出る前の
  拒否**（`-28` = 窓が落ちていない / `-50` = WEL が 21 回で立たない）。
  **Part C 着地に伴い `hx_lib_qspi_eeprom_{erase_sector,write}` と
  `hx_lib_spi_eeprom_clear_write_protect` の 3 名は FORBIDDEN から外れた。戻さない。**
- **[!] 書込みトランザクションは 1 本で、途中で返らない**（#88 Part C。
  `port/nor/nor_write.c` が seam の**唯一の認可呼び出し元**で、`board.cmake` は
  **ディレクトリでなくオブジェクト**を名指しする）: claim → 窓を落とす（SCU 読み戻しで
  確定）→ **JEDEC 再読（canary）** → 操作 → 窓を戻す → **読み戻し照合** → commit。
  **窓の復帰と commit は操作が失敗しても必ず走る。**
  [!] **canary が liveness の唯一の手段** — ベンダの write 経路は全て
  `DMA_send_recv` のタイムアウト無しスピンで、窓を落とした直後の 1 本目で実際に
  コンソールが固まったことがある。**1 本目を「何も変えない read_ID」にする**
  （`nor cycle` がその前後だけを実行する非破壊コマンド）。有界化はできない。
  [!] **読み戻し不一致は terminal `FAULTED`** — 「配列が受け付けなかった」と
  「窓が嘘をついている」を区別できず、後者なら `nn` がその場で parse するモデルを含む
  以後の全読み出しが疑わしい。**wire 前の拒否と transport 無応答は fault させない**
  （曖昧さが無い）。照合対象は**ベンダが受け付けた prefix だけ**で、
  256 B ページ分割が「どこで止まったか」を正確にする（長いバッファだと
  ベンダは複数ページ書いてから失敗し得る）。読む前に**自分で invalidate する**。
  [!] **staging バッファの理由は DMA reachability ではない** — ベンダの `write` は
  `word_switch` 経路で**呼び出し元バッファを in-place で byte-swap する**
  （TCM 禁止則は SSPI/WDMA3 の話）。
  [!] **1 トランザクションは無データでもステータスレジスタを 2 回書く**
  （窓の down/up が QE を落として立てる）。`nor cycle` も無料ではない。
  [!] **transport は 32 bit ワード内のバイトを反転する**（#92。実測: `b7 0c 4b 73` を
  書くと `73 4b 0c b7` が載る）。`nor_write.c` が**自前のページバッファで戻し、
  短い末尾は 0xFF でワード境界までパディング**する（0xFF は何も書かない）。
  **ベンダの `word_switch` に任せない** — `word_switch_func` は**長さが 4 の倍数で
  ないと黙って何もせず**、呼び出し元はその戻り値を見ない。**program アドレスは
  4 バイト整列必須**（`nor_span` と seam の両方が拒否）。
  [!] **定数バイトのテストはこの種の壊れ方を原理的に検出できない** — #88 は
  `nor write` を 0xA5 で埋めており、64 KB が 2 回とも「verified」で通っていた
  （実装順 6 の所要測定もこれ）。**既定は可変パターン**。
  「通った」を「経路が正しい」と読まない。
  [!] **未消去への program は拒否**（#92。窓を落とす前に対象を読んで判定し、
  `NOR_WRITE_REFUSED` で返す。窓を落とさないので SR 書込みも発生しない）。
  範囲の取り違えという**普通の操作ミスでポートが terminal になる**のを避けるため。
  **読み戻し不一致の terminal は残す**（そちらは曖昧さがある）。
- **[!] blob（アセットストア）の規則**（#92 / #49 Step 2）: **スロット表は
  `nor_seam_limits` の consumer**で第二の宣言を作らない（`blob_map_check()` が
  `lo`/`hi`/`unit` を引数で受ける）/ **identity は基底アドレスで添字ではない** /
  **読み手は `NOR_LEASE_BLOB` を取り、読む前に自分で invalidate する** /
  **`empty` は「ヘッダが無い」であって「空きフラッシュ」ではない**（工場データが
  スロット 0/1 に載っている）/ ヘッダは **2 プログラムページで magic は最後に単独**
  （このダイにデータシートが無いので部分再書込みに依存しない）/ **body は自スロット
  基底を持つ** / 書込みは **予約 → スロット選択（予約の下で）→ announce → 消去 →
  コンソール確保 → 受信 → body → magic → verify → 単一出口**で、**消去が OK の時
  だけ受信を始める** / **coordinator は操作を vtable で受ける**（解放がちょうど 1 回
  であることをホストから注入して検査する。実機では作れない）/ **キャンセルの結果は
  `dmesg` にしか出ない** / **受信中はローカル Ctrl+C が無い**（0x03 はファイルの一部。
  送り手を起動しないと 120 秒待つ）。実測は 1,704,672 B が **30 トランザクション**。
- **[!] 外付け NOR のライフサイクルは `port/nor/` が所有する**（#86）。QSPI/XIP の立ち上げと
  **IRQ 133（DMAC1 combined）の EPK wrapset は `port/nor/` のもの**で、NPU の snapshot は
  その後に取る。**NPU 側に戻してはいけない** — 戻すと `nn model unload` の unwrap が IRQ 133 を
  disable し、片方向ラッチが「初期化済み」と言い続ける（#86 の欠陥そのもの）。
  リースは **`npu_hw_init` が取得し `npu_hw_deinit` が解放**する（`npu_open`/`npu_close` は
  触らない）。トークンは**成功するまでローカル**で、`hw_ready` と同時にのみコミットする。
  [!] **ベンダの `enable_XIP` は MPU を再構成して戻り値を検査しない**ので、読み戻しは
  こちら側の責任。**JEDEC ID は XIP 前にしか読めない**（read-ID も同じ XIP ガードを持つ）。
  `nor` に **生オペコードを足さない**（境界付き `write`/`erase`/`cycle` は
  #88 Part C/E で着地済み。足すなら writer 経由で `nor_span.c` の判断を通す）。
  [!] **XIP 窓を読む者は全員リースを持つ**（#90。`NPU`/`SCAN`/`DEVMEM` の 3 スロット）。
  `devmem` は無リースで、立っていない窓を読むと **16 MB が 1 レジスタにエイリアスして
  嘘を印字**した（実機で観測）。背景アクセスの足元で XIP を落とす方は実在するが有界。
  **acquire が窓を立てるので 1 つで両方閉じる。**
  [!] **単一インスタンスの拒否はコンソールから再現不能**（bg は低優先度 + NO_TIME_SLICE）。
  ホストテストが唯一の検査で、実機手順に書くと**理由の違う pass** を報告することになる。
  **XIP probe は writable interval の外に置く**（`_Static_assert` で強制。#90 以前は blob 内）。
  [!] **probe は読む前に自分で invalidate する**（#88）— ベンダの `enable_XIP` は
  **base から 512 B しか無効化しない**（probe B も writer が変えた範囲も外）。
  [!] **`NOR_ST_WRITING`**（#88）: **state と reader マスクは同一クリティカルセクションで読み、
  GO を得た者が publish してから抜ける**。**`NOR_ST_OFF` は BUSY**（bring-up は reader の仕事）。
  [!] **`NOR_ST_RESERVED` と予約トークン**（#91）: **トランザクションを跨ぐ所有権**。
  commit は XIP ではなく **RESERVED に戻す**（XIP を publish すると隙に reader が入る）。
  **リースの 4 枠目にはしない** / **state と owner は同時に publish** /
  **owner と state の不整合は terminal**（`RESERVED + owner==0` を「トークン違い」に
  すると永久 BUSY）/ **予約は全ての出口で返す**（契約は協調的 kill まで）。
  **`nor info` はリースを取らない**（予約中に「なぜ busy か」を言えなくなるため。
  レジスタは XIP / RESERVED でのみ採取し、`WRITING` では「未採取」と言う）。
  **長い消去はトランザクションを割らず**、`erase_run()` にコールバックを注入する
  （**窓が落ちた状態で走る**。**1 ユニット消し終えてから**呼ぶ — 先頭がヘッダセクタ）。
  [!] ただし**「read-only」と書かない** — 配列は触らないが、初回 bring-up はベンダの
  quad-enable 経由で **NOR の不揮発ステータスレジスタ（QE ビット）を書く**（#86 の
  adversarial review 指摘）。`nn model load` が従来からやっていることで新規ではないが、
  診断コマンドから到達可能になった。
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
