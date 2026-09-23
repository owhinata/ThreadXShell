# ThreadX Shell プロジェクト

マルチボード対応の **Eclipse ThreadX + シェルコンソール** ファームウェア。`../stm32f746g-disco` と
`../wio-lite-ai` の shell 実装を統合し、複数ボードを 1 つの shell コアで支える。CMake + Ninja、
HAL/CMSIS/ThreadX は upstream ミラー submodule、ツールチェーンは初回 configure で自動取得。

**このファイルは「破ってはいけないこと」だけを置く。** 説明は `boards/<board>/README.md`、経緯は永続
メモリ（`~/.claude/projects/.../memory/`）。上限 350 行を `run_host_tests.sh` が強制する（`AGENTS.md` は 300）。

## 対応ボード

| ボード | MCU | クロック | コンソール | フラッシュ書込 |
|---|---|---|---|---|
| STM32F746G-DISCO | STM32F746NGH6 / Cortex-M7 | 自前設定 | VCP: USART1 | ST-Link（`--target flash`） |
| Wio Lite AI | STM32H725AEI6 / Cortex-M7 | **DFU boot から継承** | USB CDC（TinyUSB） | **DFU のみ**（`--target flash` = `dfu-shell`） |
| Grove Vision AI V2 | Himax HX6538 / dual Cortex-M55 + Ethos-U55（app は CM55M / Secure） | **bootloader から継承** | UART0（CH343P ブリッジ） | **UART xmodem のみ**（手動フロー） |

ピン・ボーレート・実測クロックは各 board README。**ボード追加時に必ず更新するもの**: この表 /
「ボード固有ルール」節 / `AGENTS.md` / `.claude/settings.json` の upstream ブロックリスト /
`.claude/skills/codex-{review,debug}` / **`boards/<board>/README.md`**（説明の正）/
**`submodules.cmake`**（そのボードの submodule sentinel。トップの fetch はここから導出。共有リスト
は無い）/ `test/host_tests.sh`（**`test/` を作ったら必須** — 無いとランナーが fail する）。

## 現行の作業目標

shell をボード非依存コアとして統合する当初の目標は達成済み。以降は**ボードを足しながらそのコアを
ボード非依存に保つ**段階にある。ビルドは `-DBOARD=<board>` で **1 ビルドディレクトリ = 1 ボード
（既定なし）**、ビルド / フラッシュと boot ツリーの正は**本リポジトリ**（元リポジトリではない）。

**M4 = 全ボードビルドのスクリプト/CI 化は保留**。着手条件は枚数やマイルストーンではなく、**手動運用が
実際に破れたとき**（全ボードビルドを飛ばして共有コアの回帰を実際に踏んだとき / 自分以外がこの
リポジトリにコミットするようになったとき）。**ボードが増えたことだけを理由にしない。**

## アーキテクチャ / レイヤリング

一方向依存を守る: **HAL/CMSIS/ThreadX（`lib/`）← port（ボード別グルー）← shell ← app**。
構成は `shell/` / `svc/`（freestanding サービス層）/ `asset/`（plugin ソースと container ツール）/
`lib/`（upstream、read-only）/ `cmake/`（共有ビルド規則とゲート）/ `boards/<board>/`（port ldscript
src cmds svc cmake test README。wio のみ boot も）。

- **shell コアはボード非依存**。`#ifdef <BOARD>` やペリフェラル直叩きを core/cmds に入れず、ボード差は
  transport 抽象（`struct cli_transport_api`）と port 側グルーで吸収する。
- **共有コアに触れる変更は全対応ボードのビルドが通ることを確認してからコミットする**（手打ちで確認）。
- **shell の常設状態は静的割当**で init / dispatch / 出力経路は heap を要求しない。board 固有コマンドの
  ペイロードは board が bounded heap・排他・失敗処理を明示的に提供する場合のみ heap 可。スタック
  サイズと優先度は `cli_config.h` の既定を踏襲し `_Static_assert` を通す。
- upstream submodule（`lib/`）は read-only で、必要な調整は port 側で吸収する。**boot ツリー
  （`boards/wio-lite-ai/boot/`）は独立**で、app / shell とソースを共有しない。

### [!] `nn` は 3 ボード共有の 1 コマンド

`shell/cmds/cmd_nn.c` + 契約 `svc/nn_svc.h` + ボードごとの `nn_svc_*.c` アダプタ。

- **共有 TU は可変記憶域を 1 バイトも持たない**（状態はアダプタ、バッファは持ち主が配置属性つきで注入）。
  強制は `cmake/check_no_mutable_storage.py` の**ボードごとの監査コンパイル**（ホストの答えは別物）。
- **ファーム監査は列挙せずビルドから導出する**（`cmake/shared_storage_gate.cmake`。例外は
  `svc/ymodem.c` のみ）。**導出・両方向照合・失敗伝播を弱めない。**
- **capability マクロは「性質」で「ボード名」ではない**。バックエンド依存は `CONFIG_NN_BACKEND` に従う。
- **status と claim disposition は別フィールドで `retryable` と `terminal` を畳まない**（**判断できない
  ボードは `terminal` に fail-closed**）。disposition は**呼び出し側の解放権限**である。
- **モデル指定はタグ付きで裸の文字列は拒否する**（同じ語がボードごとに別物）。`--addr` の長さは必須。
- **port のアダプタは `struct cli_instance` を取らない**（印字・待ち・キャンセルは `boards/*/cmds/`）。
- **ライブ推論は 3 ボードとも `nn stream start/stop/stats`**（`preview` は復活させない）。`start` は非
  ブロッキングで、待ちは `--frames <n>` が 1 実装で持つ。
- **[!] stream には世代があり、`stop` は遷移を claim するのと同じクリティカルセクション内で照合する**。
  `NN_STREAM_GEN_ANY` は操作者専用で**待ち手は渡さない**。機械は `svc/nn_stream_life.c` の 1 本。
- **[!] start の admission も機械が持つ** — **worker を触る前に STARTING を claim し失敗なら abort**。
  `commit()` は STARTING 以外、`finish/retry/poison` は STOPPING 以外を拒否する。
- **[!] worker のカウンタは世代と一致しない**ので stats は commit 時に基準を latch する（re-arm は
  decode record も retire）。**遷移が拒否されたら wrapper の副作用も走らせない**（解放は成功時のみ）。
- **[!] poll は 2 相 + 遷移カウンタ**（数値は他ロック配下なので**割込み禁止下では集められない**。
  世代と状態だけでは retryable な stop を跨いだ読みを弾けない）。
- **[!] retryable / terminal の分類は「その時点で何ができるか」で決まる**。表はボードが出し
  （純関数・ホストテスト必須）、**未文書コードは `nn_stream_disp_of()` が terminal に fail-closed**。
- **[!] 負値を 1 つに畳まない**（「モデル非認識」/ 未初期化 / 引数不正は別コードで**どれも「0 件」ではない**）。
- **[!] 停止は走行中の推論を取り消せない** — worker は arm 時点の世代を控え、publish のロック内で照合
  する（`svc/nn_det_record.c`）。

### [!] plugin container と asset（3 ボード共有部）

モデルとその出力を解釈するコードを 1 blob で運ぶ。plugin は board code と同格の**信頼された native
code** で、ゲートが証明するのはスタック上限だけ（**メモリ安全性も MMIO も証明しない**）。

- **`svc/plugin_load.c` は呼び出し可能なポインタを返さない**（整数オフセットとコピー済みバイトのみ）。
  「実行しない」は型の性質なので関数ポインタを足さない。
- **ゲートは plugin ELF にも適用する**（対象外にしない）。**ダイジェストは署名ではない**。
- **container は組んでから検査し、その同一ファイルを送る**（ホストは `verify_container` で**デバイスと
  同じ `svc/plugin_load.c`** を走らせる）。**通るまで公開しない**（private path で組む）。
- **モデル区画は 16 バイト整列**（flatbuffer の 4 ではなく Ethos-U ドライバの要求）。
- **オフセット → アドレスの変換は `plugin_run_slot()` の 1 箇所**で、**実行前に MPU を読み戻して
  fail-closed**（純関数・ホストテスト必須）。**窓を守る新しい機構は作らない**。
- **上限は実測から導出する。超えられない上限は上限ではない。導出値 0 は「未測定」ではない**
  （absent の綴りは slot 側で、`stack_limit == 0` の拒否は明示的に書く）。**許容値は slot に到達しうる
  全スレッドの最も浅い天井に対して宣言し、深さは callback 入口で測る**（表はボード、深さの正は README）。
- **[!] 宣言は c を含まずファームが足す**（walk を変えたら会計の版）。**c は出荷 ELF で検査し読み戻す**。
- **plugin のビルド規則は共有**（`cmake/add_plugin.cmake`）で**ボードは自分の事実だけを引数で渡す**。
  **owned source root は helper が導出する**（受け取る形自体が fail-open）。**リンク入力も列挙する**
  （`ARCH_FLAGS` は `-m*` のみ）。**監査の success stamp は compile 前に消す**。**ヘッダも依存（depfile）**。
- **image gate も共有**（`cmake/check_plugin_image.py`）。予約 / 禁止シンボル表 / veneer base cost は
  `add_plugin()` の**必須引数**で、**ゲートに告げる予約は MEMORY fragment と別の宣言**。TU の ABI も照合。
- **target word は 2 端で検査する**（firmware の `_Static_assert` + gate の `.ARM.attributes`）。
- **[!] ゲートは送信時ではなくビルド時にある**（`--target asset-<name>`）。送信は打ったパスをそのまま送る
  ので**閉じ手は CRC32 と `blob list` の照合**。「ビルド時に検査済み」を「何も起きない」と書き換えない。
- **モデルは commit と SHA256 で pin**し、消えたら **fail closed**。fetch は build 時で `asset-*` は ALL 外。
- **ファームと plugin は別成果物で間違いは両方向**（焼いても container は更新されず、逆も焼き直し不要）。

## 開発ワークフロー

**コード修正 → 全対応ボードのビルド → フラッシュ → ユーザーが実機で確認 → ドキュメント／メモリ
更新 → コミット**、を小さく繰り返す。**動作確認前にコミットしない。ドキュメント／メモリ更新を忘れない。**

### メインは管理、作業は subagent

**メインのセッションは管理だけを担当し、各 Issue の作業は subagent に任せる。** メインのコンテキストは
進め方の判断・ユーザーとのやり取り・報告の裏取りに使い、実装の詳細や Codex の出力で埋めない。

- **メイン**: Issue と範囲を決める / subagent を起動し報告を**実物で裏取り**する / 選択肢と CONCERN の
  採否をユーザーに聞く / plan mode と `ExitPlanMode` / ビルド・テスト・差分の再確認 / 実機確認の依頼 /
  commit・merge・push / Issue コメントとクローズ。**subagent**: 下調べ / plan 素案 / `codex-review`
  skill による plan レビュー / 実装 / 実装後の Codex レビュー（**実装者とは別の fresh subagent**）。
- [!] **subagent は `model: opus` で起動する**（継承させない）。読むだけの下調べは `sonnet` 可、迷ったら opus。
- subagent への指示に毎回書くこと: CLAUDE.md を最初に読む / git の add・commit・push をしない /
  **marker に触らない（touch はメインだけ）** / **実機に焼かない** / `lib/`・SDK・boot を編集しない /
  plan と実物が食い違ったら止まって報告する / 「plan から外れた点・自分で決めた点」を必ず書く。
- **Codex を回す subagent は同時に 1 つまで**。subagent が実装している間はメインが plan mode に入らない。
  実装は plan のコミット単位で 1 段ずつ止めさせ、メインが確認してコミットしてから次へ。

### Plan + Codex review ワークフロー

**Phase 系 / architecture を変える plan は、plan 確定前と実装後の両方で codex review を実施する。**
対象: shell コアの構造・transport 抽象・コマンド API / ボードポート追加、クロック・メモリ・割込み優先度の
構造変更 / ThreadX 統合方針 / 新規ペリフェラル、DMA・キャッシュ構成、リンカスクリプト・起動フロー /
複数レイヤに跨る変更 / Wio のクロック継承・メモリ配置・USB CDC（ブリックリスク）/ **boot ツリーに
触れる一切の変更**（原則やらない）。

1. **Plan 確定前**: **`codex-review` skill** で 3 面（設計 / MCU 実機能（対象ボードの RM 照合）/
   HW リソース競合）review。BLOCKING / CONCERN を全解消してから `ExitPlanMode`。
2. **実装後**（commit 前）: branch の diff を review → BLOCKING 解消 → 実機 verify 依頼 → commit。

[!] **1 つの diff に掛けるレビューは 1 本だけ**（後者は前者を含むので重複するのは待ち時間だけ）。
**既定は `/codex:review`**。**`/codex:adversarial-review <focus>` を選ぶのは、変更がチェック機構・
ゲート・安全機構そのものを足す/変えるとき** — 汎用レビュアーは「この検査は騙せるか」を見ない。
**focus は 1〜2 問に絞り**、「この検査を通過したまま X できるか」の形で疑う面を名指しする。

入口: **plan = `codex-review` skill**（marker を更新する唯一の経路）/ 実装後の差分 =
`/codex:review`、検査機構を足す差分は `/codex:adversarial-review <focus>` / 原因追跡 = `codex-debug`
skill または `/codex:rescue`。Codex 呼び出しは codex plugin のランタイムに一本化（MCP server は
使わない）。毎回 120s を超えるので `run_in_background: true` で起動して待つ（`/codex:status`）。
**Codex の指摘は鵜呑みにしない** — ローカルの実物・対象ボードの RM で裏を取ってから報告する。

**プロジェクト不変条件は `AGENTS.md` に置く**（`/codex:review` の内蔵レビュアーは focus text を
受け取れないので、Codex に伝える経路はそこしかない）。**不変条件を変えたら両方を直す。**

**ゲートの強制**: `ExitPlanMode` の PreToolUse hook が `~/.claude/.threadx-shell-plan-codex-reviewed`
marker を確認し、無い/古い（2h 超）と block する。trivial plan で skip する場合も **user 承認を
得てから** touch する。

## Git ワークフロー

**PR は作らない。** Issue 駆動で feature/fix ブランチを切り、ローカル `main` に `--ff-only` merge →
push → Issue へ対応コメント → Issue クローズ。リポジトリは `owhinata/ThreadXShell`。

- **ブランチ**: `feat/`, `fix/`, `docs/`, `build/`, `refactor/`, `chore/`, `style/` prefix で
  `<prefix>/<N>-short-description`（`<N>` は Issue 番号）。ベースは常に `main`
- **コミット**: conventional commits `type: #N short description` で **subject に Issue 番号**を含める
  （リンク生成＋オートクローズ判定のため）。末尾に `Co-Authored-By: Claude ...` を付与
- [!] **コミットメッセージと README は英語で書く**（会話・Issue・コード内コメントは日本語でよい）
- 動作確認していない変更を commit / push しない

```bash
gh issue create --repo owhinata/ThreadXShell --title "..." --body "..."  # Summary/Environment/Notes
git checkout -b feat/<N>-short-description && git commit -m "type: #<N> short description"
git checkout main && git merge --ff-only feat/<N>-short-description && git push origin main
gh issue comment <N> --repo owhinata/ThreadXShell --body "..."  # コミットレンジ <base>..<head> 必須
gh issue close <N> --repo owhinata/ThreadXShell && git branch -d feat/<N>-short-description
```

- **PR は作らない**（`gh pr create` / `gh pr merge` 禁止）。直 push なので `--ff-only` 厳守、**force push 禁止**
- Issue を立てずにコミットしない。Epic / 親 Issue はクローズキーワードを使わず `#<epic>` 参照のみ
- 元リポジトリの Issue は `owhinata/wio-lite-ai#N` / `owhinata/stm32f746g-disco#N` の完全形で書く
- **upstream submodule（`STMicroelectronics/`, `eclipse-threadx/`, `hathach/tinyusb`,
  `eembc/coremark`, `armink/FlashDB`, `mlcommons/tiny`）へ `gh` で書き込まない**（PR/issue/comment とも
  PreToolUse hook がブロックする）。コードも編集せず、port 側のグルーで吸収する

## ThreadX 統合の共通教訓（全ボード共通）

- **SysTick > PendSV**（優先度）。同一だと idle 時 PendSV スピンを tick が割り込めず tick 停止 →
  スリープ中スレッドが起床しないデッドロック。PendSV は最低優先度。
- ThreadX が `PendSV_Handler` を供給する（`stm32xxxx_it.c` と競合させない）。割込みは TX 生成後に有効化。
- クリティカルセクションは **PRIMASK ベース**（`TX_PORT_USE_BASEPRI` 未定義）なので ISR からの
  `tx_event_flags_set` 等は preempt できず安全。
- `__disable_irq` 下の `tx_application_define` で `HAL_GetTick` 依存の init を呼ばない（**ただし実際に
  マスクしているか確認してから適用する** — 現在どのボードもマスクしていない）。

## ボード固有ルール

各節は破ってはいけないことだけ。**説明の正は `boards/<board>/README.md`**。

### STM32F746G-DISCO

- **LTO 禁止**（ldscript の ASSERT 群が配置 invariant の本体で、LTO はそれが依拠するシンボル名を改名
  する）。`board.cmake` の FATAL_ERROR と `boards/f746g-disco/cmake/check_f746_layout.py` を外さない。
- **SDRAM は FMC 内部バンクで用途固定**（またぐ変更は FE とキャッシュ直結）。ASSERT の境界を緩めない。
- **3 つの割込みハンドラ（PendSV / SysTick / USART1）は強シンボルであること** — stock CMSIS が weak な
  `Default_Handler` エイリアスを供給するので、落ちてもリンクは通る。
- **`CLI_INSTANCE_TIME_SLICE=0`（TX_NO_TIME_SLICE）を維持**する（CPU-bound コマンドは多重実行に非再入で、
  その間他コンソールが応答しないのが**期待挙動**）。
- **`.sdram.ai` のスクラッチはボードが注入する**（常駐の require は `check_f746_layout.py` が持つ）。
- **カメラの 3 subscriber は各自 sink を drain する**（`camera_frame_put()` は全 `consume()` の最後の
  文。所有権の終わりは quiescence = `_pins == 0 && _callbacks == 0`）。
- リファレンス: RM0385 / UM1907（`_ref/f746g-disco/_ref/`、read-only）。

### Wio Lite AI（[!] ブリック安全則あり）

復旧手順は `boards/wio-lite-ai/boot/README.md`。**現存する実機は board #2 のみ**（#1 は恒久文鎮化）。

- [!] **boot ツリーと ROM リンカスクリプトは不変**。内蔵 Flash セクタ0 の boot を焼き直す操作は
  **ブリック本番**で、必要なら codex-review（3 面）+ 監査 + バックアップを経てユーザーに依頼する。
  **boot の `iflash.c` のセクタ範囲チェックはセクタ0 を守る唯一の砦**で、緩めない。
- [!] **boot は「参照ビルド」としてのみビルドし、セクタ0 に書けるターゲットも `dfu-boot` も作らない**。
  ゲートは `boards/wio-lite-ai/cmake/check_boot_safety.py` で、**外す・弱める変更は不可**。
- [!] **DFU フォールバックの安全網を app 側から壊さない**（erased/invalid app は必ず DFU に入る性質）。
  **書換え耐久は有限で自動ループで焼き直さない。** 書込は **DFU のみ**、`flash` は app 専用。
- [!] **app はクロックツリーを再設定しない**（system/PLL・FLASH ACR・電源供給選択・VOS）。bus clock
  gate と kernel mux は可。**例外は board README が名指しする 2 つだけ**。`SystemInit` はカスタム版。
- [!] **オプションバイト / RDP / DBGMCU / SWD 端子は絶対に触らない。**
- [!] **RAM 配置**: AXI-SRAM = バスマスタから見える必要があるものだけ / DTCM = CPU 専用 / ITCM = ISR。
  **DMA は TCM に届かず、fault せず無言で転送されない**。**例外は AXI-SRAM 上端の `.plugin` 予約 1 つ**
  （prelink なので動かすと全 container 無効）で、**4 箇所で独立に宣言**し 1 変数から生成しない。
  heap の天井は `__heap_end` で `__ram_end` を再定義しない（1 シンボルに 2 事実を載せない）。
- [!] **リンカスクリプトの `ASSERT` は LTO 下で空振りする**。最終ガードはポストリンクの residency
  チェック 4 本（`check_{itcm,dtcm,psram_ai}_residency.py` / `check_plugin_reservation.py`）で、外さない。
- **デコーダは container でしか届かない**（**常駐デコーダを戻さない**）。素の `.tflite` は `nn run` が
  テンソルをそのまま報告、`nn stream start` は拒否、`nn thresh` は none。**`null` backend も同じ答え**。
- **admission は `nn run` と共有**なので**shape の問いは no-plugin で通す**（refuse すると素のモデルの
  `nn run` が消える）。**stream を止めるのは `nn_active_can_draw()` 1 本。**
  **[!] この規則には現に穴がある（#120）** — re-arm 早期 return が `require_draw` を見ず、`nn run` は
  stream のライフサイクルを claim しない。**#120 が片付いたらこの併記を消す。**
- **worker は非同期**なので「誰も解釈していない」も**世代規則の下で publish する**（しないと `nn run`
  が timeout する）。**panel は `valid` だけでなく kind も見る**。
- **plugin の差し替えは backend が成功してから**（先だと前の plugin を壊す）。bare model は必ず unload。
- **decode と draw を隔てるものが無い**ので**結果リース**で囲う: **常にリース → フレームロック**、
  worker は decode と publish の全体を保持、**パネルは待たない**。**plugin に入る経路は全部取る**
  （param・admission・load/unload も）。**リース保持は「描いてよい」ではない**。
- **report は snapshot と同じ保護区間で採取し、バッファは呼び出し側のフレーム**。**長さは状態ではない**。
- **painter は全部 CPU**（DMA2D に静止を確かめる機構が無い）。**輪郭は書く画素数で課金する**。
- **スタックは 2 つの量**（入口の空き / veneer の下）。不足ならスレッドを広げる。
- **`nn info` の claim は開いているモデルに従い**、reload 後の状態は `nn_model_reload()` の戻り値で
  決める（無ロックの `nn info` が別コンソールから呼ぶので後から問い合わせない）。
- リファレンス: RM0468 / PM0253 / 基板 schematic（`_ref/wio-lite-ai/`、read-only）。

### Grove Vision AI V2

外付け NOR にブートローダが載っている。**NOR 関連は wio のセクタ0 と同格**。

- [!] **ブートローダ領域とスロットヘッダ予約は書かない**（壊すと黙って前のビルドで起動する）。
  **地図はパートの全バイトを claim する**（未宣言の run は空きではなく止められない容量）。
  **[!] 毎回の flash はブートローダ領域も書き直す。自動ループで焼かない。**
- [!] **erase/program へ届いてよいのは `nor_seam.c` だけ**（判定は **ld の map**）。**blob のみ・
  1 粒度のみ・`NOR_ST_WRITING` 以外は拒否。**
- [!] **書込みは 1 トランザクションで途中で返らない**（`port/nor/nor_write.c` が唯一の認可呼び出し元）:
  claim → 窓 down → **JEDEC 再読（canary）** → 操作 → 窓 up → 読み戻し照合 → commit。
  **窓の復帰と commit は失敗しても必ず走る。**
- [!] **読み戻し不一致は terminal `FAULTED`**（窓が嘘をついている可能性と切り分けられない）。
  **wire 前の拒否と transport 無応答は fault させない。未消去への program は拒否。**
- [!] **工場データは最初の書込みで恒久的に消える**。占有確認は**書く前の read-only 走査**で。
- **XIP 窓を読む者は全員リースを持ち、`nor` に生オペコードを足さない。ライフサイクルと IRQ wrapset は
  `port/nor/` の所有で NPU 側へ戻さない。「read-only」と書かない**（bring-up が QE ビットを書く）。
- **blob の identity は基底アドレス**で、表の**全面 re-carve は一度きり・以後 append のみ**。
  **`empty` は「ヘッダが無い」であって「空きフラッシュ」ではない。**
- **flash geometry は実測値でノブではない**（非キャッシュ。**CACHE 化に戻さない** — 規則と検査が同じ値
  から出る）。**消去粒度とヘッダ複製数は束ね直さない。**
- **`check_flash_partitions.py` は成果物ゼロでも予約の非重複を検査し、存在必須は今から書く物だけ**（全
  ファイル要求はファーム焼きを止める）。削除済みのモデル予約・ターゲットは復活させない。
- **Himax SDK は pin fetch で `lib/` と同じ read-only**。**公開 TRM が無い**ので主張は SVD と SDK 実装で。
- **ポストビルドゲート 5 本**（image coherence / placement / timer seam / `check_nor_seam.py` /
  `check_output_vocabulary.py`）**を外す・弱めない**（負のテストは `cmake/fixtures/`）。**LTO 不使用。**
- **低位 SRAM 窓は NOLOAD 専用**（配置ゲートが ELF のフラグで検査）。**DMA が触るバッファは TCM 不可。
  app はクロックを設定しない**（SCU 読み戻しが唯一の真実。SysTick reload は実行時値から）。
- **SEC_ONLY なので `TX_SINGLE_MODE_SECURE` 必須**。優先度は 3-bit で**M7 の値を流用しない**。
  **`platform_driver_init()` は PRIMASK 下**、カーネル入場前に IRQ を disable + pending clear。
- **TIMER2 は EPK 専有**（`hx_drv_timer_*` は禁止シンボルで、プリビルト参照分は**ゲート緩和ではなく
  `--wrap` の seam**）。**時間源の分担を混ぜない。**
- **EPK の会計対象 IRQ は集合で「有効だが未ラップ」を作らない**（番号不明の周辺は実測してラップする）。
- **WFI の前提は検査ではなく強制**（駄目なら fail-stop）。`hx_lib_pm_*` は禁止接頭辞。**MVE は解禁済み
  だが `FPCCR.ASPEN` は入場前に強制 → 読み戻し → halt**、**継承 `LSPACT` は拒否**。
  **CoreMark の TU だけ `-fno-tree-vectorize`**（公表値との基準線の連続性）。**ベンチは入口で tick と
  SCU を検査**し駄目なら実行拒否、実行後に再読み出しして警告する。
- **カメラのデータパスは固定。停止は単一ルーチン、再開はバリア。未知の負値は terminal。**
- **WDMA3 のチャネルアドレスを書くのは `cam_wdma3.c` だけ、かつ xDMA disable 中**（マスクは専用ペア）。
- **バスの所有者は mutex 保持下で決める**（入口は `cam_bus_enter()` 1 本）。**判定は状態を列挙する**
  （広い検査は fail open）。**`CAM_ST_LOST` は保持下で到達する**ので direct に落とさない。
- **stop だけが API mutex を有界待ちし**（他は `TX_NO_WAIT`）、**poison は待ちの向こう側で判定する**。
- **`camera_stream_stop()` は成功時のみ join を保証し、呼び出し元は成功時だけ detach する。** 未確認の
  join は `CAM_ST_LOST`（`FAULTED` で代用しない）で、detach も teardown も回収路も無い。**`cmds/` は
  producer が消費するデータパス設定を書かない**（境界は**消費のされ方**。毎フレーム読む物は可）。
- **op resolver は 1 個のまま**（CMSIS-NN 排除 / 全面 offload でないモデルを落とす）。**境界の型変換は
  ファイル側で剥がす。ペイロード検査も緩めない**（`COMMAND_STREAM` が 1 個かつ最後 / 入力テンソル 0 /
  `is_variable()` 拒否）。
- **`npu_open()` は長さを取り `GetModel()` の前に境界付き verifier を通す**（範囲 → 長さ → identifier →
  verifier → 走査。**長さには下限も要る**）。**limits は呼び出しとともに `npu_verify.h` の 1 箇所。**
- **`nn model load --name` はリースを切らさない**（`npu_hw_init()` が先 → 走査 → CRC → `npu_open()`、失敗は
  必ず `npu_hw_deinit()`）。**候補は VALID のみ・重複拒否・失敗理由は別々・読めなければ拒否。**
- **ホストの `verify_vela_model` を外さない**（代替にならない）。**C++ 不在は fail-closed。**
- **アリーナの保守は範囲ごとでなく全体を 2 点で**（潰すのは `ethosu_invalidate_dcache()` だけ、成功条件は
  state と result の**両方**、異常時はリセット成功を確認してから）。**呼び出し側で保守しない。**
- **推論は producer スレッド・`consume()` 内**で**推論（ガード無し）→ ガード 1 回で stage/draw/present**
  （callback 中の block / sleep / 推論 / 他ロック / LCD 再入は禁止）。**タイムアウトは `npu_hw.h` に 1 つ。**
- **`nn stream` はデコーダが無い時点で拒否する**（shape や draw の前）。**wio と揃えようとしない。
  `nn_input_quant_ok()` は常駐デコーダの前提条件で plugin は縛られない。**
- **ファームの印字は「種」を名乗らない**（ゲートは **`.rodata`** を読む。**literal を regex で見ない**）。
  **フォントは plugin 側**で `text()` を足さない。**`nn dets` は record を読むだけ**で `valid` を作らない。

## SWD デバッグ（共通）

- GDB はシステムの **`gdb-multiarch`**（toolchain 同梱 gdb は `libncursesw.so.5` 欠如で不可）。サーバは
  OpenOCD（`interface/stlink.cfg` + `target/stm32f7x.cfg` or `stm32h7x.cfg`、:3333）か `st-util`（:4242）。
- コンソールと `st-flash`/読み出しが同じ `/dev/ttyACM*` を奪い合うと文字化けする。
- Wio の boot 書込/復旧は `boards/wio-lite-ai/boot/README.md`、ST-Link の個体情報は
  `../wio-lite-ai/CLAUDE.md`（良品 Discovery ST-Link のみ mode=UR 可）。

## リファレンス（`_ref/`）

`_ref/` は git 管理外（`.gitignore` 済）の「ローカルで読むための資料」専用。**公開環境に含めない —
ビルド（CMake）・`scripts/`・git 管理下のファイルから `_ref/` を一切参照しない**。参照した瞬間、
クローンしただけでは configure できないリポジトリになる。C コード中の言及は出典コメントのみ可。
中身は `_ref/{f746g-disco,wio-lite-ai,grove-vision-ai-v2}/` で、各 board README のリファレンス行を見る。

## ドキュメント

- **`README.md` と各モジュールの README は英語で書く**。変更と同時に更新する。
- リポジトリ内ファイルの記号は **基本 ASCII**。絵文字は使わない（強調マーカーは `[!]`）。

### [!] ボード固有の説明は `boards/<board>/README.md` に書く

**各ボードは `README.md` を持ち、そのボードの説明の正とする**（手順・ピン・メモリマップ・ブート経路・
ハマりどころ・復旧手順・ゲート・未確認事項）。**分担**: 「どう動くか」は board README、「破っては
いけない不変条件」は CLAUDE.md と `AGENTS.md`、ルートの `README.md` は対応ボード表と共通手順のみ。
**同じ事実を 3 箇所に写経しない** — 増えるほど食い違って、どれが正か分からなくなる。

### [!] Issue を閉じるときに足してよい量

このファイルは Issue ごとの作業記録ではない。1 つの Issue が終わったとき:

- **CLAUDE.md / `AGENTS.md` に足してよいのは、新しい不変条件 1 つにつき 1 行**（何をしないか +
  理由 1 句 + 詳細のポインタ）。不変条件が増えていないなら 0 行。
- **実測値・アドレス・CRC・サイズ・タイミングは board README**（該当する節に、英語で）。
- **経緯・何が失敗したか・なぜそう決めたかは永続メモリ。**
- **上限は `shell/test/run_host_tests.sh` が強制する**（CLAUDE.md 350 行 / `AGENTS.md` 300 行）。足した
  くて入らないなら、それは足すものを間違えている。
