# ThreadX Shell — Codex 向けプロジェクト指示

マルチボード対応の **Eclipse ThreadX + シェルコンソール** ファームウェア（`stm32f746g-disco` と
`wio-lite-ai` の shell を 1 コアに統合、CMake + Ninja）。このファイルは Codex が**毎回読む前提の不変
条件表**で、**レビュー時はここを最優先の判定基準にする**。置くのは「破ると BLOCKING になること」だけで、
**説明・実測値・アドレス・経緯は `boards/<board>/README.md` が正**。人間向けの同じ規則は `CLAUDE.md` で、
**不変条件を変えたら両方を直す**（上限 300 行を `shell/test/run_host_tests.sh` が強制する）。

| ボード | MCU | 書込 | 説明の正 |
|---|---|---|---|
| STM32F746G-DISCO | STM32F746NGH6（M7、クロックは自前設定） | ST-Link | `boards/f746g-disco/README.md` |
| Wio Lite AI | STM32H725AEI6（M7、**DFU boot から継承**） | **DFU のみ** | `boards/wio-lite-ai/README.md` |
| Grove Vision AI V2 | Himax HX6538（dual M55 + U55、app = CM55M / **Secure**、**bootloader から継承**） | **UART xmodem のみ** | `boards/grove-vision-ai-v2/README.md` |

## [!] 不変条件（違反はそれだけで BLOCKING）

1. **レイヤリング**: 一方向依存 **HAL/CMSIS/ThreadX（`lib/`）← port（ボード別）← shell ← app**。
   shell コアはボード非依存で、`#ifdef <BOARD>` やペリフェラル直叩きを core/cmds に入れない（ボード差は
   transport 抽象 `struct cli_transport_api` と port 側グルーで吸収）。ボード固有物は `boards/<board>/`
   で **Wio の boot ツリーは独立**。ボード所有コードのホストテストは `boards/<board>/test/host_tests.sh`
   に置き `shell/test/` にボード参照を持ち込まない。**ドキュメントも同じで board README が正。**
2. **共有コアに触れる変更は全対応ボードで成立すること。** 片方のボードだけを見て LGTM しない。
3. **upstream submodule（`lib/` 配下）と Grove の Himax SDK ツリーは read-only**（調整は port 側で）。
4. **shell の常設状態は静的割当**で、init / dispatch / 出力経路は heap を要求しない。board 固有コマンドの
   ペイロードは board が bounded heap・排他・失敗処理を明示的に提供する場合のみ heap 可。スタック
   サイズ・優先度は `cli_config.h` で `_Static_assert` を通すこと。
5. **ビルドは `_ref/` を読まない**（git 管理外なので参照するとクローンしただけでは configure できない）。

### 6. Wio Lite AI（ブリックリスク。提案は特に厳格に）

- **[!] app はクロックツリーを再設定しない**（system/PLL、FLASH ACR、電源供給選択・VOS）。書き換えると
  低速クロックに落ちるのに latency は高速用のまま残る。bus clock gate と kernel mux は許可。**例外は
  board README が名指しする 2 つだけ。** `SystemInit` は **FPU + VTOR + TCM 初期化のみ**（VTOR は
  リンカの `g_pfnVectors` から取る）。
- **[!] boot ツリーと ROM リンカスクリプトは不変。** 内蔵 Flash セクタ0 に DFU ブートローダが常駐し、
  焼き直しはブリック本番で**現存する実機は 1 枚しかない**。**boot の `iflash.c` のセクタ範囲チェックは
  セクタ0 を守る唯一の砦**で緩める変更は不可。app はセクタ1-3 から実行し書込は DFU のみ。
  **書換え耐久は有限で自動ループで焼く提案は不可。DFU フォールバック**（erased/invalid app は必ず DFU に
  入る）を app 側から壊す変更も、**オプションバイト / RDP / DBGMCU / SWD 端子**に触れる提案も不可。
  boot は**参照ビルドとしてのみ**ビルドし、**セクタ0 に書けるターゲットも `dfu-boot` も新設しない**。
  boot は app のヘッダを include せず LTO 有効化も不可。boot ソース / ROM ldscript の変更は manifest と
  golden hash の更新とレビュー済み例外を要する。
- **[!] RAM 配置ポリシー**: AXI-SRAM = バスマスタから見える必要があるものだけ（**DMA が届く唯一の
  RAM**）/ DTCM = CPU 専用 / ITCM = ISR コード。**DMA1/DMA2・SDMMC1 IDMA は TCM に届かず、DTCM の
  DMA バッファは fault せず無言で転送されない。** **唯一の明文化した例外は AXI-SRAM 上端の `.plugin`
  予約**（prelink なので動かすと既存 plugin が全て無効）。heap の天井は `__heap_end` で `__ram_end` を
  再定義しない。予約は ldscript / `plugin_memory.ld` / board.cmake のゲート引数 /
  `check_plugin_reservation.py` の **4 箇所で独立に宣言し、1 つの変数から生成しない**。
- **plugin の差し替えは backend が成功してから**（先だと前の plugin を壊す）。bare model は必ず unload。
- **[!] decode と draw を隔てる構造が無い**ので**結果リース**で囲い、順序は**常にリース → フレーム
  ロック**、worker は **decode と publish の全体**を保持、**パネルは待たず**飛ばして**数える**。
  **plugin に入る経路は全部リースを取る**（param・admission・load/unload も）。**リース保持は
  「描いてよい」ではない。**
- **report は snapshot と同じ保護区間で採取しバッファは呼び出し側のフレーム。長さは状態ではない。**
  **[!] painter は全部 CPU。** **[!] スタックは 2 つの量**で veneer の下のファーム側コストは**実測して
  導出する**（過大が安全側。**コールバック自身のフレームだけでは「crossing の下」にならない**）。
- **[!] デコーダは container でしか届かない**（**常駐デコーダを戻さない**）。素の `.tflite` は
  **`nn run` でテンソルをそのまま報告**、**`nn stream start` は拒否**、**`nn thresh` は none**（set は
  **state** で拒否）。**`null` backend も同じ答え。**
- **admission は `nn run` と共有**なので**shape の問いは no-plugin で通す**（refuse すると素のモデルの
  `nn run` が消える）。**stream を止めるのは `nn_active_can_draw()` 1 本。** **[!] この規則には穴がある
  （#120）** — re-arm 早期 return は `require_draw` を見ず `nn run` は stream を claim しない。
- **worker が非同期**なので「誰も解釈していない」も**世代規則の下で publish する**。**`nn dets` は
  record を読むだけ、panel は kind も見る。`nn info` の claim は開いているモデルに従い**、reload 後の
  状態は `nn_model_reload()` 自身の戻り値で決める。**監査は `AUDIT_SHARED` だけで、f746g-disco はまだ
  常駐デコーダを持つ（変えない）。**

### 7. `nn` は 3 ボード共有の 1 コマンド

`cmd_nn.c` が唯一の実装、契約は `svc/nn_svc.h`、ボード側は `nn_svc_*.c`。**緩めた時点で BLOCKING。**

- **共有 TU（`cmd_nn.c` / `nn_cmd_core.c`）は可変記憶域を持たない**（状態はアダプタだけ）。強制は
  `cmake/check_no_mutable_storage.py` を**ボードごとの監査コンパイル**で当てること — **ホストの結果は
  当てにならない**（host ではコマンド表が `.data.rel.ro` に落ちて偽陽性になる）。
- **ファーム監査は列挙せずビルドから導出する**（`cmake/shared_storage_gate.cmake`。対象は実際に
  コンパイルする `svc/` の TU と上記 2 本で、唯一の既存例外は `svc/ymodem.c`）。評価済み CMake
  target/source と compile DB を両方向照合し実引数で監査コンパイルする。**導出・照合・失敗伝播を
  弱めない。** plugin の実オブジェクト監査は別に維持する。詳細は `cmake/README.md`。
- **capability マクロは性質であってボード名ではない**（バックエンド依存は `CONFIG_NN_BACKEND` に従う）。
- **status と claim disposition は別フィールドで 4 値を畳まない**（`none` / `caller` / `retryable` /
  `terminal`）。**混同は実害**で**判断できないボードは `terminal` に fail-closed**。disposition は
  保持者ではなく**呼び出し側の解放権限**である。
- **モデル指定はタグ付きで裸の文字列は拒否**（同じ語がボードごとに別物を指す）。**`--addr` の長さは必須。**
- **port のアダプタは `struct cli_instance` を取らない**（印字・待ち・キャンセルは `boards/*/cmds/` で、
  **下へ関数ポインタで渡す**）。
- **ライブ推論は 3 ボードとも `nn stream start/stop/stats`**（`preview` は復活させない）。`start` は
  非ブロッキングで、待ちは共有コマンドの `--frames <n>` が 1 実装で持つ。
- **[!] stream には世代がある。** 待ち手は `start` の generation を持ち、`stop` は**遷移を claim するのと
  同じクリティカルセクション内で**照合する（`NN_STREAM_GEN_ANY` は操作者専用で**待ち手は渡さない**）。
  機械は `svc/nn_stream_life.c` の 1 本。
- **[!] start の admission も機械が持つ** — worker を触る**前に** STARTING を claim し失敗なら abort。
  `commit()` は STARTING 以外を、`finish/retry/poison` は STOPPING 以外を拒否する。**worker のカウンタは
  世代と一致しない**ので poll は commit 時に latch した基準を引く。**遷移が拒否されたら wrapper の
  副作用も走らせない。**
- **[!] poll は 2 相 + 遷移カウンタ**（数値は他ロック配下なので割込み禁止下では集められず、世代と
  状態だけでは retryable な stop を跨いだ読みを弾けない）。
- **[!] 分類表はボードが持ち既定は fail-closed**（Grove の `nn_stream_state.c` では `CAM_ERR_LOCKED` と
  `CAM_ERR_BUSY` が **retryable**）。**terminal に畳み直さない。**
- **[!] デコーダの負値を 1 つに畳まない**（どれも「0 件」ではない）。**停止は推論を取り消せない**ので
  worker は arm 時点の世代を控え publish のロック内で照合（`svc/nn_det_record.c`）。

### 8. `svc/frame_pipeline` の sink registry: attach は拒否する、直列化は呼び出し元

- `frame_pipeline_attach()` は**未 drain の sink** と**既に link 済みの sink** を拒否する。**緩めない**
  — 前者を通すとリングが恒久的に 1 スロット短くなり、後者は `s->_next = s` を作り走査が終わらない。
  拒否は**全部 `open()` の前**に決まり、`open()` の負値は**単一のコアエラーに正規化**、判定順は
  **already-linked → pins → capacity**。**走査は有界**（cycle は state error）。
- **[!] 並行性はコアが持つ。** attach は `open()` の前に、detach は `close()` を跨いで sink を claim し、
  **claim 済みの sink は全入口が拒否する**。**DRAINING を出る条件は `_pins == 0 && _callbacks == 0`**
  （`publish()` は `consume()` をロック外で呼び、戻ってから統計を書く）。
- **state は唯一の真実ではない**（membership・owner・2 カウンタは独立した事実で、3 入口とも共通の整合
  検査を通す）。**3 入口は同じ破損を同じ分類にする**（1 つだけが retryable と呼ぶと port が壊れた sink を
  永久に retry する）。`detach()` の**非負の pin 数は単独では teardown 許可ではない**。
- **caller に残る条件は 3 つ**（`set_format()` は attach と重ならない / sink は同時に 1 pipeline のみ /
  `init()` は quiescent なときだけ）。**「全 registry 操作を直列化する」とは書かない。**

### 9. plugin container と asset（3 ボード共有部）

plugin は board code と同格の**信頼された native code**。ゲートが証明するのは**スタック上限だけ**で、
**メモリ安全性も、渡したポインタの使用範囲も、MMIO も証明しない**。

- **`svc/plugin_load.c` は呼び出し可能なポインタを 1 つも返さない**（整数オフセットとコピー済みバイト
  のみ）。「実行しない」は規律ではなく**型の性質**。**ゲートは plugin ELF にも適用し対象外にしない。
  ダイジェストは署名ではない**（由来は packer のプロセス制約が担保する）。
- **container は組んでから検査し、その同一ファイルを送る**。ホストは `verify_container` で**デバイスと
  同じ `svc/plugin_load.c`** を走らせ、**全ゲート通過まで公開しない**。**`--profile` と `--slot` は必須**
  で前者はファイル名から推測しない。**`--profile` はチェック集合の選択であって識別ではない**ので非対称
  — 対処は**拒否ではなく警告**で、**`cls` 側に出力 shape 固定を足さない**。
- **[!] モデル区画は 16 バイト整列**（flatbuffer の 4 ではなく Ethos-U ドライバの要求）。
  **[!] オフセット → アドレスの変換は `plugin_run_slot()` の 1 箇所**で、**実行前に MPU を読み戻して
  fail-closed**: Armv8-M に「番号の大きいリージョンが勝つ」規則は無く、`limit` は最後の 32 B を含み、
  MAIR は完全復号、リージョン数は `MPU_TYPE.DREGION` で**読めた表より大きければ clamp せず拒否**。判定は
  純関数でホストテスト必須。**検査から実行までの窓に新しい機構を作らない。**
- **[!] plugin の fault は `CAM_ST_LOST` に行かず、記録して即リセット**（リセットが teardown。handler は
  publish 済みメタデータしか読まない）。
- **[!] スタック上限は実測から導出する。超えられない上限は上限ではない**（非同期予約の前提は
  `FPCCR.TS == 0` の強制。**導出値 0 は「未測定」ではない**）。**[!] slot に到達しうる全スレッドの最も
  浅い天井に対して宣言し、深さは callback 入口で測る**（表はボード、深さの正は README）。**[!] ファームは
  古い宣言と現在の宣言を区別できない** — veneer コストを変えたら既存 container は pack し直して送り直す。
- **[!] painter の予算はガード保持時間に比例する仕事の上界**であって `draw()` 内の任意計算の上界では
  ない（課金は**フレームバッファを触る前**）。**輪郭は外接面積ではなく実際に書く store 数で課金する**
  （外接面積だと近距離の顔 1 つで箱が黙って消える）。共有してよいのは幾何規則（`svc/rect_geom.c`）
  だけで**期待値は共有せず実ループの store を数える**。
- **[!] 分岐点は `port/npu/nn_active.c` の 1 つだけ**（一発デコード / stream の admission・decode・draw /
  **閾値** / report が全部そこを通る）。**plugin は自分の閾値を持つ**ので片方だけ繋ぐと `nn thresh` が
  届かず、**両者に同じ閾値を与える differential test はこれを見逃す**。**幾何も 1 つで decode 結果は
  private**。
- **[!] plugin のビルド規則は共有**（`cmake/add_plugin.cmake`）で**ボードは自分の事実だけを引数で渡す**。
  **owned source root は helper が導出し引数で受け取らない**（受け取る形自体が fail-open）。**リンク
  入力も列挙する**（渡せるのは `ARCH_FLAGS` の `-m*` だけ）。**success stamp は compile 前に消す。**
- **[!] image gate も共有**（`cmake/check_plugin_image.py`）。ボード固有の 3 つ（**予約 / 禁止シンボル
  表 / `VENEER_BASE_COST`**）は `add_plugin()` の**必須引数**。**ゲートに告げる予約を MEMORY fragment と
  同じ変数から作らない**（検査対象から期待値を読むゲートは何でも通す）。
- **[!] target word は 2 端で検査する**（firmware の `_Static_assert` + gate の `.ARM.attributes`）。
  **CMSE ビットは image に記録されないので firmware の assert が唯一の検査**。`__ARM_FP` 単独で FPU を
  決めず、写像できない組は `#error`。
- **[!] アセットは `--target asset-<name>` が作る。ゲートは送信時ではなくビルド時にある** — 送信は打った
  パスをそのまま送り**それがその成果物かは誰も検査しない**ので、閉じ手は receipt の **CRC32** を転送後に
  `blob list` と突き合わせること。**モデルは commit + SHA256 で pin**（Git LFS 不在だとポインタが
  exit 0 で置かれる）、**pin が消えたら fail closed**。**ファームと plugin は別成果物で間違いは両方向。**

### 10. 配置ゲート: リンカスクリプトの `ASSERT` は LTO 下で空振りする

配置保証はポストリンクのチェックで行う。**どのボードでも、以下を外す・弱める変更は不可。**

- **wio-lite-ai**（LTO を使う）: `check_{itcm,dtcm,psram_ai}_residency.py` と
  `check_plugin_reservation.py` が唯一の砦（`ALLOWED_VENEER_TARGETS` は**理由つきで**増やす）。
- **f746g-disco**: 逆に **LTO を禁止**する（`board.cmake` が per-config 変種込みで FATAL_ERROR。
  ldscript の ASSERT 群が invariant の本体だから）。加えて `check_f746_layout.py` がシンボル常駐 /
  ベクタ / float ランタイムを実イメージで検査する。
- **wio-lite-ai の boot ツリー**: `check_boot_safety.py`（precheck + POST_BUILD）。`boot_image` の
  always-relink を外す変更はゲートの無効化と同じ。negative test は `cmake/fixtures/`。
- **grove-vision-ai-v2**: ポストビルド 5 本（`check_image_coherence.py` / `check_placement_budget.py` /
  `check_timer_seam.py` / `check_nor_seam.py` / `check_output_vocabulary.py`）。LTO は使わない。
  `check_mve_predication.py` は削除済み（1 命令も検出できなかった）。**戻さない。**

### 11. STM32F746G-DISCO

- **メモリ配置**: DTCM = D-cache を経由しないもの / SRAM1 = SDMMC DMA バウンス / SDRAM は **FMC 内部
  バンクごとに用途固定**（**またぐ変更は FE とキャッシュコヒーレンシに直結**）。ASSERT では `.sdram` の
  属性脱落を検出できないので `check_f746_layout.py` のシンボル常駐検査が見る。
- **3 つの割込みハンドラは強シンボルでなければならない**（`PendSV_Handler` / `SysTick_Handler` /
  `USART1_IRQHandler`）— stock CMSIS が weak な `Default_Handler` を供給するので落ちてもリンクは通る。
  ゲートは strong `T` / `Default_Handler` 非同値 / `.isr_vector` slot 一致の 3 条件で見る。
- **`CLI_INSTANCE_TIME_SLICE=0`（TX_NO_TIME_SLICE）を維持**する（CPU-bound コマンドが多重実行に非再入。
  スライス有効化は再入ガード整備とセット）。
- **カメラ subscriber の drain と owner lifecycle**: `camera_frame_put()` は**全 `consume()` の最後の
  文**、`CAM_OWN_DRAINING` は `camera_unsubscribe()` の**前**、直列化（PRIMASK）は作業を跨がない。判定は
  `cam_drain.c` / `cam_own.c` の純関数が**唯一の判断点**で**両方 fail-closed**、**`default:` を足さない**。
  **DONE は `pins == 0` ちょうどだけ。失敗は呼び出し元まで返す。**

### 12. Grove Vision AI V2

**外付け NOR にブートローダが載っている。NOR 関連は wio のセクタ0 と同格。** 根拠・実測値・状態遷移表は
board README が正。

- **[!] ブートローダ領域とスロットヘッダ予約は絶対に書かない**（壊すと黙って前のビルドが起動する）。
  **地図は全バイトを claim** し、**工場データは最初の書込みで消える**ので書く前に read-only の全走査を。
  **[!] 毎回の flash はブートローダ領域も書き直すので自動ループで焼かない。**
- **[!] ベンダの NOR erase/program へ届いてよいのは `nor_seam.c` だけ**（`--wrap` は内側 4 本。
  **`erase_all` と `word_write` の wrapper は `__real_*` を名指ししない**）。**blob だけ・実測した 1 粒度
  のみ・`NOR_ST_WRITING` 以外は拒否。FORBIDDEN から外した 3 名は戻さない。[!] seam ゲートは ELF では
  なく ld の map で判定**（LTO は拒否）。**能力の証明ではなく defence in depth。**
- **[!] 書込みは 1 トランザクションで途中で返らない**（`port/nor/nor_write.c` が唯一の認可呼び出し元）:
  claim → 窓 down → **JEDEC 再読（canary）** → 操作 → 窓 up → **読み戻し照合** → commit。**窓の復帰と
  commit は失敗しても必ず走る。canary が liveness の唯一の手段。**
- **[!] 読み戻し不一致は terminal `FAULTED`。wire 前の拒否と transport 無応答は fault させない。照合は
  受け付けた prefix だけ。未消去への program は拒否。ベンダの戻り値は成否を報告しない**（真実は読み戻し。
  **負の値は wire 前の拒否**）。**[!] QSPI は 32 bit ワード内のバイトを反転する**ので writer が自前の
  ページで戻す（**4 バイト整列必須**）。**定数バイトのテストでは見えない**ので既定は可変。
- **[!] XIP 窓を読む者は全員リースを持ち、probe は writable interval の外で読む前に自分で invalidate
  する。`nor` に生オペコードを足さない。「read-only」とは書かない**（bring-up が QE を書く）。**NOR の
  ライフサイクルと QSPI/XIP と EPK wrapset は `port/nor/` 所有**（NPU へ戻すと unload が IRQ を切る）。
- **[!] 弱めない**: **`NOR_ST_WRITING` は state と reader マスクを同一クリティカルセクションで読む /
  `NOR_ST_RESERVED` は跨ぐ所有権で commit は RESERVED に戻し owner と同時に publish、不整合は terminal、
  予約は全出口で返す / `nor info` はリースを取らない / blob の identity は基底アドレス、`empty` は
  「空きフラッシュ」ではない、全面 re-carve は一度きりで以後 append のみ。**
- **[!] flash geometry は実測値でノブではない**（非キャッシュ。**CACHE 化は不可** — 宣言と検査が同じ値
  から出る）。**消去粒度とヘッダ複製数は束ね直さない。firmware 予約はブートローダの算術から導出**し
  **1 イメージ 1 スロットは `--image-max` で検査**。**`check_flash_partitions.py` は予約の非重複を成果物
  ゼロでも検査し存在必須は今から書く物だけ。削除済みの予約・ターゲットを復活させない。**
- **Himax SDK は pin fetch でツリーは read-only。** app は **XIP ではない**（**ITCM 溢れはリンク
  エラー**）。**SRAM 窓は 2 領域**で低位は **NOLOAD 専用**（配置ゲートが ELF のフラグで検査する）。
  **DMA が触るバッファを TCM に置かない。クロックは継承**（SCU 読み戻しが唯一の真実）。**SEC_ONLY なので
  `TX_SINGLE_MODE_SECURE` 必須**、優先度は 3-bit（**M7 の値を流用しない**）。`platform_driver_init()` は
  PRIMASK 下 + 入場前に IRQ 停止。
- **[!] TIMER2 は EPK 専有で触ってよいのは `tx_glue.c` の bring-up 1 箇所だけ。`hx_drv_timer_*` は API
  丸ごと禁止シンボル**（例外は init のみ。接頭辞で塞ぐ）。**緩めない。** プリビルト参照分は**ゲート緩和
  ではなくリンカ `--wrap` の seam**で吸収し、**`tx_glue_profile_ok()` が毎回再検証する。[!] EPK の会計
  対象 IRQ は集合で「有効だが未ラップ」を作らない**（番号不明は **ISER で実測**）。**時間源を混ぜない。**
- **[!] WFI と EPK はコンパイル時スイッチ**なので前提は**強制**する（読み戻して駄目なら fail-stop。
  `hx_lib_pm_*` は禁止接頭辞）。**[!] MVE は解禁済みだが `FPCCR.ASPEN` は入場前に強制 → 読み戻し →
  halt**（**継承 `LSPACT` は拒否**）。**CoreMark の TU だけ `-fno-tree-vectorize`**（基準線の連続性）。
- **[!] WDMA3 のチャネルアドレスを書くのは `cam_wdma3.c` だけ、かつ xDMA disable 中**（マスクは専用
  ペアで**全出口で復元**、arm 時の監査は **fail-closed**）。**完了した面だけを読取前に全長 invalidate。
  停止は単一ルーチン、再開はバリア**（**クリアの前に必ず停止**）。**エラーはフレームより優先・未知の
  負値は terminal。**
- **[!] `camera_stream_stop()` は成功時のみ join を保証する**ので**呼び出し元は `CAM_OK` の時だけ
  detach する**。未確認 join は **`CAM_ST_LOST`**（**`FAULTED` で代用しない**）で、detach も teardown も
  所有権解放も**回収路**も無い。**[!] stop だけが API mutex を有界待ちし**（他は `TX_NO_WAIT`）、
  **poison の判定は待ちの向こう側でもう一度**行う（取得失敗は poison しない）。
- **[!] センサーバスの所有者は mutex 保持下で決める**（**acquire より前の検査は無価値**）。入口は
  **`cam_bus_enter()` 1 本**、契約は **`CAM_OK` ⇒ mutex 保持 + owner は DIRECT か PRODUCER のみ**。
  **bring-up はヘルパに入れない。判定は状態を列挙し `default:` を足さない。cmds/ は producer が消費する
  データパス設定を書かない**（境界は**消費のされ方**）。
- **op resolver は 1 個のまま**（**CMSIS-NN を持ち込まない**）。**境界の型変換はファイル側で剥がす。
  [!] `npu_open()` は長さを取り `GetModel()` の前に境界付き verifier を通す**（**範囲 → 長さ →
  identifier → verifier → 走査**。**長さには下限も要り生アドレス形にも必須**、**limits は呼び出しと
  ともに `npu_verify.h` の 1 箇所**）。**ペイロード検査も緩めない**（`COMMAND_STREAM` が 1 個かつ
  最後 / 対象は**入力テンソル 0** / `is_variable()` は拒否）。
- **[!] `nn model load --name` はリースを切らさない**（`npu_hw_init()` が先 → 走査 → CRC → `npu_open()`
  → **失敗は必ず `npu_hw_deinit()`**）。候補は **VALID のみ・重複拒否・失敗理由は別々・読めなければ拒否。
  ホスト側の `verify_vela_model` を外さない**（**書込みの後**に走る。**C++ 不在は fail-closed**）。
- **[!] アリーナのキャッシュ保守は「範囲ごと」にしない。** 潰すのは **`ethosu_invalidate_dcache()`
  だけ**で、引き渡しは `ethosu_inference_begin/end` でアリーナ**全体**を、成功条件は **`job.state` と
  `job.result` の両方**、異常時はリセットの**成功を確認してから**。**呼び出し側で保守しない。推論は
  camera producer スレッド・`consume()` 内**で**推論（ガード無し）→ ガード 1 回で stage/draw/present**
  （callback 中の block / sleep / 推論 / LCD 再入は禁止）。**タイムアウトは `npu_hw.h` の 1 箇所。**
- **[!] `nn stream` の拒否の仕方はボードで違う。揃えない**（Grove は**デコーダが無い時点で**、wio は
  **shape は通し `can_draw` 1 本で**）。**[!] `nn_input_quant_ok()` は常駐デコーダの前提条件で plugin は
  縛られない。[!] ファームの印字は「種」を名乗らない** — ゲートは **`.rodata`** を negative scan する。
- **[!] Grove と wio のファームは共有デコーダをリンクしない** — **デコーダは container でしか届かない**
  （f746 はまだ持つ）。**常駐デコーダを戻さない。別フラグで組み直した監査対象を作らない。** 素の
  `.tflite` は**テンソルをそのまま報告**し、`nn thresh` は **`none`**、set は **`NN_SVC_ERR_STATE`**。
  デコーダは**全アンカーを走査**し出力は **shape で探す**。**[!] フォントは plugin 側で painter に
  `text()` を足さない**（ラスタライズは `decode()`、`draw()` は blit だけで**原点アンカー**。
  **`decode()` は冒頭で draw-valid を落とし成功時にだけ立てる**）。

## ThreadX 統合（全ボード共通）

- **SysTick > PendSV**（同一だと idle 時 PendSV スピンを tick が割り込めず tick 停止 → デッドロック）。
  PendSV は最低優先度で、ThreadX が自前で `PendSV_Handler` を供給する（`stm32xxxx_it.c` と競合させない）。
  クリティカルセクションは **PRIMASK ベース**。割込みは TX オブジェクト生成後に有効化。
- `__disable_irq` 下の `tx_application_define` で `HAL_GetTick` 依存の init を呼ばない。**ただし前提を
  確認してから適用すること** — 現在どのボードも `tx_kernel_enter()` の前で割込みをマスクせず ThreadX も
  `tx_application_define()` を TX_DISABLE で囲まないので、SysTick は走り続け `HAL_GetTick` ベースの
  タイムアウトは正常に期限切れする（f746 の `eth_init()` がこれに依存）。**マスクの有無を確認せずに
  違反と判定しない。**

## レビュー時の作法

- **「コンパイルが通る」は根拠にならない。** レジスタ/能力の主張は対象ボードの RM（F746 = RM0385 /
  H725 = RM0468。**Grove は公開 TRM が無い — SDK の `WE2_S.svd` と SDK 実装が正**）で、配線の主張は
  UM1907 / schematic で裏を取る。**実測値・メモリマップ・ピン・手順は board README が正**で、見ずに
  数値を作らない。裏が取れない推測は推測として明示する。
- 存在しないファイル・行・レジスタ・実機挙動を作らない（Wio の実機は 1 枚だけ）。
- 指摘には影響と具体的な修正案を付ける。

## ビルド / フラッシュ

1 ビルドディレクトリ = 1 ボード。`-DBOARD` に既定は無い（誤ったボードのイメージを黙って作らせないため）。
submodule は `boards/<board>/submodules.cmake` が宣言し fetch はそこから導出される。

```bash
cmake -B build/<board> -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake -DBOARD=<board>
cmake --build build/<board>
# 書込は --target flash: f746 = ST-Link / wio = DFU のみ（PF1 保持リセットで DFU へ）/
#                        grove = UART xmodem のみ（ターミナルを閉じ、プロンプトでリセット押下）
```
