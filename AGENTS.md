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
| Grove Vision AI V2 | Himax HX6538 WiseEye2 (dual M55 + U55; app = CM55M / **Secure**, 400 MHz **bootloader から継承**) | UART0 (CH343P ブリッジ) 921600 | **UART xmodem のみ** |

## [!] 不変条件（違反はそれだけで BLOCKING）

1. **レイヤリング**: 一方向依存 **HAL/CMSIS/ThreadX（`lib/`）← port（ボード別）← shell ← app**。
   shell コアはボード非依存 — `#ifdef <BOARD>` やペリフェラル直叩きを shell の core/cmds に
   入れない。ボード差は transport 抽象（`struct cli_transport_api`）と port 側グルーで吸収する。
   ボード固有物は `boards/<board>/`（port/ ldscript/ src/）に置く。
   **Wio の DFU ブートローダ（`boards/wio-lite-ai/boot/`）は独立ツリー**で、app / shell と
   ソースを共有しない。
   `shell/test/` も同じ規律の下にある — ボード所有のコードを実ヘッダ込みでホストコンパイル
   するテストは `boards/<board>/test/host_tests.sh`（`shell/test/run_host_tests.sh` が
   同じフラグで呼ぶ）に置き、`shell/test/` にボード参照を持ち込まない。
   **ドキュメントも同じ**: ボード固有の説明（手順 / ピン / メモリマップ / ハマりどころ /
   復旧手順）は **`boards/<board>/README.md` が正**。このファイルとルート `README.md` は
   要点・不変条件・リンクに留め、同じ事実を写経しない（食い違いの温床になる）。

2. **共有コアに触れる変更は全対応ボードで成立すること。** 片方のボードだけを見て LGTM しない。

2a. **`nn` は 3 ボード共有の 1 コマンド（#50）。** `shell/cmds/cmd_nn.c` が唯一の実装で、
   契約は `svc/nn_svc.h`、ボード側は `boards/*/port/*/nn_svc_*.c` のアダプタ。以下は
   **緩めた時点で BLOCKING**:
   - **共有 TU（`cmd_nn.c` / `nn_cmd_core.c`）は可変記憶域を持たない。**
     `check_no_mutable_storage.py` を**ボードごとの監査コンパイル**で当てる
     （#97 のデコーダと同一機構）。**ホストの結果は当てにならない** — host では
     コマンド表が `.data.rel.ro` に落ちて偽陽性になり、実機の cortex-m 向けでは
     `READONLY` になる。状態を持ってよいのはアダプタだけ。
   - **capability マクロは性質であってボード名ではない**
     （`boards/<board>/svc/nn_svc_config.h`）。`#ifdef <BOARD>` の言い換えを作らない。
     バックエンド依存の能力は `CONFIG_NN_BACKEND` に従う（f746/wio の `model load`）。
   - **status と claim disposition は別フィールドで、4 値を畳まない。**
     `none`（解放するな。ボードが巻き戻し済み）/ `caller`（1 回だけ解放）/
     `retryable`（解放権限なし。worker が片付けうる。stop の再試行が正解）/
     `terminal`（解放権限なし。再起動しかない）。**`retryable` と `terminal` の混同は
     実害**（前者で再起動させる / 後者で永久に再試行させる）。
     **判断できないボードは `terminal` に fail-closed。**
     disposition は「いま誰が持っているか」ではなく**呼び出し側の解放権限**
     （wio では worker とコンソールが最後の 1 人を競うので、読んだ時点の保持は保証できない）。
   - **モデル指定はタグ付き**（`--name` / `--slot` / `--path` / `builtin` /
     `--addr <a> <len>`）で、**裸の文字列は拒否**。同じ語が Grove では blob 名、
     f746 では SD パス、wio では無意味なので、受理は shell にボード知識を戻すこと。
     **`--addr` の長さは必須**（FlatBuffer verifier の境界。「窓の残り全部」は不可）。
   - **port のアダプタは `struct cli_instance` を取らない。** 印字・待ち・キャンセル判定・
     ファイル読みが要るものは `boards/<board>/cmds/` に置き、**下へ関数ポインタで渡す**
     （`nn_svc_cancel_fn` / `nn_svc_read_fn`）。port が cmds/ を名指ししない。
   - **ライブ推論は 3 ボードとも `nn stream start/stop/stats`**（#99 で統一、`preview` は
     削除済み。復活させない）。`start` は非ブロッキングで、待ちは共有コマンドの
     `--frames <n>` が 1 実装で持つ。
   - **[!] stream には世代がある。** `start` が返す generation を待ち手が持ち、`stop` は
     **遷移を claim するのと同じクリティカルセクション内で**照合する。`NN_STREAM_GEN_ANY`
     は操作者の `nn stream stop` 専用で **待ち手は渡さない**（渡すと他人の stream を畳んで
     カメラ / NPU / バスガードを奪う）。**照合と claim は 1 呼び出し**（分けると 2 者が同じ stream に入り、
     負けた方が後から後継 stream を畳む）。機械は `svc/nn_stream_life.c` の 1 本。
   - **[!] start の admission も機械が持つ。** 下位 worker を触る**前に** STARTING を
     claim し、失敗なら abort。後から記録すると re-arm が進行中の stop を上書きする。
     `commit()` は STARTING 以外を拒否（LOST の蘇生防止）、finish/retry/poison も
     STOPPING 以外を拒否。**worker のカウンタは世代と一致しない**ので、poll は
     commit 時に latch した基準を引く（wio の re-arm はカウンタを継続する）。
     re-arm は decode record も retire する。**遷移が拒否されたら wrapper の副作用も
     走らせない**（成否を返す。claim 解放を無条件にすると不変条件違反で fail open）。
   - **[!] poll は 2 相 + 遷移カウンタ。** 数値は自分のロックを持つ側から来るので割込み
     禁止下では集められず、世代と状態だけでは retryable な stop を跨いだ読みを弾けない。
   - **[!] Grove の teardown 分類**（`port/npu/nn_stream_state.c`、純関数・ホストテスト）:
     カメラの `CAM_ERR_LOCKED` と detach の `CAM_ERR_BUSY` は **retryable**、join timeout /
     poison / 恒久拒否は **terminal**。**terminal に畳み直さない。**

2b. **`svc/frame_pipeline` の sink registry: attach は拒否する、直列化は呼び出し元（#72 / #79）。**
   `frame_pipeline_attach()` は **未 drain の sink（pin を持ったまま）** と
   **既に link 済みの sink** を拒否する。**緩めない** — 前者を通すと sink の pin カウントだけが
   0 になり、pipeline 側の slot refcount は上がったままでリングが恒久的に 1 スロット短くなる
   （#72 の本体）。後者は `s->_next = s` を作り、**以後の registry 走査が終わらない**。
   拒否は**全部 `open()` の前**に決まる（副作用ゼロ。`open()` はボードが consume() の読む
   状態をリセットする場所なので、未 drain の sink に対して呼んではいけない）。
   `open()` の負値は**単一のコアエラーに正規化する** — 透過するとコアが自分の戻り値を
   所有できない（ボードが同じ値を返せる）。判定順は
   **already-linked → pins → capacity**（満杯時の再 attach を FULL と誤報しないため）。
   [!] **並行性はコアが持つ（#79）。** attach は `open()` の前に sink を claim し、
   detach は `close()` を跨いで claim する。**claim 済みの sink は全入口が拒否する**。
   sink の所有状態は `UNOWNED -> ATTACHING -> ATTACHED -> DETACHING -> DRAINING -> UNOWNED` で、
   **DRAINING を出る条件は `_pins == 0 && _callbacks == 0`**（「pin が返った」ではない ——
   `publish()` は `consume()` をロック外で呼び、**戻ってから統計を書く**ので、
   pin が 0 でもコアはまだ sink に触っている。#72 の put-last 規則の裏返し）。
   - **state は唯一の真実ではない。** registry membership・owner・2 つのカウンタは独立した
     事実で、3 入口とも**共通の整合検査**を通す。冗長に見える行が噛む ——
     `ATTACHED + owner NULL` は破壊的 detach へ直行してボードの `close()` を呼ぶし、
     `UNOWNED + linked` は detach に裸の pin 数を返させる
   - **3 入口は同じ破損を同じ分類にする。** port が行動を変えるのは
     **retryable（transition / not-quiescent）か terminal か**だけ。1 つの入口だけが
     破損を retryable と呼ぶと、**port が壊れた sink を永久に retry する**
   - `detach()` は**非負の pin 数 または 負の error**を返す。**非負（0 を含む）は
     それ単独では teardown 許可ではない**
   - **走査は有界**（cycle は state error。ループしたリストで exactly once を数えるのは
     この issue が閉じたハングの再現方法）
   - **caller に残る条件は 3 つ**: `set_format()` は attach と重ならない /
     sink は同時に 1 つの pipeline にしか属さず全 sink-scoped 呼び出しは owner を使う /
     `init()` は完全に quiescent なときだけ。**「全 registry 操作を直列化する」とは書かない** ——
     Grove の `camera_unsubscribe()` は API mutex を取らないので現状の説明として嘘になる

3. **upstream submodule（`lib/` 配下）は read-only。** HAL / CMSIS / ThreadX 系 / TinyUSB /
   CoreMark ほか。編集は不可、調整は port 側で。

4. **shell の常設状態は静的割当。** 共有 shell コア（インスタンス / スタック / ジョブプール）と
   transport の常設状態は静的割当で、init / dispatch / 出力経路は heap を要求しない。
   board 固有コマンドのペイロードは、board が bounded heap・排他（`malloc_lock`）・失敗処理を
   明示的に提供する場合に限り heap を使用できる（wio の coremark が実例）。
   スタックサイズ・優先度は `cli_config.h`、`_Static_assert` を通すこと。

5. **Wio Lite AI: app はクロックツリーを再設定しない。**
   app は boot から継承した system/PLL クロックツリー（クロックソース、D1/D2/D3 プリスケーラ、
   PLL1/PLL2、および下記例外以外の PLL3 設定）、FLASH ACR、電源供給選択（SMPS/LDO）・VOS を
   再設定しない。書き換えると全部壊れる（HSI 64 MHz に落ちるのに latency は 550 MHz 用のまま）。
   ペリフェラルの bus clock gate と kernel clock mux の設定は許可する。
   **例外は次の 2 つのみ**:
   (a) `ltdc_clock_init()` が USB クロック供給前に行う 3 フィールド・成功パス計 4 書込み
   （`RCC_CR.PLL3ON` clear / `RCC_PLL3DIVR.DIVR3` 更新 / `RCC_PLLCFGR.DIVR3EN` set /
   `RCC_CR.PLL3ON` set。RM0468 §8.7.1 / §8.7.11 / §8.7.16）、
   (b) `HAL_PWREx_EnableUSBVoltageDetector()` による `PWR_CR3.USB33DEN` set（RM0468 §6.8.4）。
   継承値は 550 MHz / PLL3Q 48 MHz USB / FLASH latency 3（DFU ブートローダ =
   本リポジトリの `boards/wio-lite-ai/boot/` が構成）。`SystemInit` は
   **FPU + VTOR + TCM 初期化のみ**（RCC / PWR / FLASH ACR は触らない）。
   VTOR はリンカの `g_pfnVectors` から取る（ハードコード不可）。

6. **Wio Lite AI: boot ツリー（`boards/wio-lite-ai/boot/`）と ROM リンカスクリプト
   （`STM32H725AEIx_ROM.ld`）は不変。**
   内蔵 Flash セクタ0 `0x08000000`（128KB）に DFU ブートローダが常駐する。ここを焼き直す
   操作はブリック本番で、**現存する実機は 1 枚しかない**（board #1 は恒久文鎮化済み）。
   **boot の `iflash.c` の書込先セクタ範囲チェックはセクタ0 を守る唯一の砦**で、緩める変更は不可。
   app は `0x08020000`（セクタ1-3, 384KB）から実行し、書込は DFU 経由のみ。
   **書換え耐久 ~10k サイクル** — 自動ループで焼く提案は不可。DFU フォールバック
   （erased/invalid app は必ず DFU モードに入る）を app 側から壊す変更も不可。
   オプションバイト / RDP / DBGMCU / SWD 端子（PA13/PA14）に触れる提案も不可。
   boot は **参照ビルドとしてのみ**ビルドする（`boot` → `boot-reference/`）。
   **セクタ0 に書けるターゲットの新設は不可。`dfu-boot` の新設も不可**（boot を app
   パーティションに焼くターゲットになる）。boot は app のヘッダを一切 include しない
   （`boot_iface` が `${BOARD_DIR}/include` を持たないので include すればコンパイルエラー）。
   boot ターゲットの LTO 有効化も不可（ゲートが読む呼び出しグラフの辺が消える）。
   boot ソース / ROM ldscript の変更は `cmake/boot_manifest.sha256` と golden hash の
   両方の更新を伴い、レビュー済み例外を要する。
   ブート経路・継承クロック・ゲートの中身は `boards/wio-lite-ai/README.md`、
   復旧手順は `boards/wio-lite-ai/boot/README.md`。

7. **Wio Lite AI: RAM 配置ポリシー**: AXI-SRAM（320KB @ 0x24000000）= バスマスタから見える
   必要があるものだけ（**DMA が届く唯一の RAM**）/ DTCM（128KB @ 0x20000000）= CPU 専用 /
   ITCM = ISR コード。**DMA1/DMA2・SDMMC1 IDMA は TCM に届かない**（RM0468
   §2.1.2/§2.1.5/§2.1.6）。**DTCM の DMA バッファは fault せず無言で転送されない。**

8. **リンカスクリプトの `ASSERT` は LTO 下で空振りする。** 配置保証はポストリンクの
   residency チェックスクリプトで行う。配置を変える変更はこのゲートを維持すること。
   - **wio-lite-ai** は LTO を使うので、`check_itcm_residency.py` /
     `check_dtcm_residency.py` / `check_psram_ai_residency.py` が唯一の砦。
   - **f746g-disco** は逆に **LTO を禁止**する（`board.cmake` が `-flto` /
     `CMAKE_INTERPROCEDURAL_OPTIMIZATION` を per-config 変種込みで FATAL_ERROR にする）。
     ldscript の ASSERT 群が invariant の本体だから。加えて `check_f746_layout.py` が
     シンボル常駐 / ベクタ / float ランタイムを実イメージで検査する。
     **どちらのボードでも、この 2 系統のゲートを外す・弱める変更は不可。**
   - **wio-lite-ai の boot ツリー**は `check_boot_safety.py`（不変条件 6）。
     `boot_precheck`（ソース manifest + compile command 監査）と POST_BUILD（リンク済み
     イメージ検査）の 2 段で、`boot_image` は毎ビルド再リンクさせて迂回経路を消してある。
     この always-relink（`boot_precheck` の stamp → `boot_image` の `LINK_DEPENDS`）を
     外す変更は、ゲートを無効化するのと同じ。negative test は
     `cmake/fixtures/run_fixture_tests.py`。

8b. **f746g-disco: メモリ配置ポリシー**: DTCM 64KB @ 0x20000000 = D-cache を経由しない
   もの（reset 跨ぎのログリング `g_log`、membench の DTCM 行）/ SRAM1 = D-cache 管理を
   1 バッファに閉じ込める SDMMC DMA バウンス（`sd_bounce`）/ SDRAM 8MB @ 0xC0000000 は
   MPU Normal non-cacheable で、FMC 内部バンクごとに用途が固定されている
   （bank0 = LTDC スキャンアウト面と固定居住者 / bank1 = カメラ DMA アリーナ 2MB /
   bank2 = ETH ディスクリプタ + プール / bank3 = NN アリーナ、上半分 1MB は reloc モデルの
   実行窓 0xC0700000）。**バンクをまたぐ配置変更は FE / キャッシュコヒーレンシに直結する**。
   `.sdram` は単一出力セクションで境界シンボルが常設されるため、ASSERT だけでは
   属性の脱落を検出できない — だから `check_f746_layout.py` のシンボル常駐検査がある。
   詳細（クロック / コンソール / バンク割当ての理由 / ゲート）は
   `boards/f746g-disco/README.md`。

8c. **f746g-disco: 3 つの割込みハンドラは強シンボルでなければならない**
   （`PendSV_Handler` / `SysTick_Handler` / `USART1_IRQHandler`）。stock CMSIS startup は
   3 つとも `.weak` + `Default_Handler`（無限ループ）エイリアスを供給するので、
   実装が落ちてもリンクは通り「定義されている」ようにも見える。
   `check_f746_layout.py` が strong `T` / `Default_Handler` 非同値 / `.isr_vector` の
   該当 slot 一致の 3 条件で検査する。

8d. **grove-vision-ai-v2: Himax SDK は read-only、ThreadX は Secure 単一モード、
   ゲート 3 本を外さない。**
   SDK は submodule ではなく configure 時の pin fetch（`cmake/himax_sdk.cmake`、
   933810cc）。`boards/grove-vision-ai-v2/sdk/` は lib/ と同じ read-only。ドライバは
   プリビルト（libdriver.a）で ISR を実行時にベクタテーブルへ登録する。
   app は **XIP ではない**（2nd bootloader が ITCM/DTCM へ展開）、クロックは継承
   （SCU 読み戻しが唯一の真実）、**全空間 Secure（SEC_ONLY、SAU 無効）で
   `TX_SINGLE_MODE_SECURE` 必須**。優先度は 3-bit（PendSV=7 / SysTick=6）。
   `platform_driver_init()` は PRIMASK 下 + カーネル入場前に IRQ 0..200 を
   disable/clear（プリビルトが IRQ を勝手に開くため）。**毎回の flash は bootloader
   領域も書く**（Himax 標準。耐久 ~100k は**回路図の W25Q128JWSIQ 由来で、実装品は
   Zbit ZB25LQ128C**（#89。刻印と JEDEC `5e 50 18` で一致）。**耐久は未確認**。消去単位は
   **4 KB のみ実測済み**（#88。2nd BL が毎回の flash で発行する）で 32/64 KB は未実証。自動ループ焼き不可。復旧 =
   boot ROM + BOOT_OPT + factory image）。ポストリンクゲート 3 本
   （`check_image_coherence.py` = 生成 .img と ELF の突き合わせ + .rodata 内
   コマンドレジストリ / `check_placement_budget.py` = 配置・予算・ベンチバッファの
   常駐・禁止シンボル・**必須シンボル**）＋ **#88 で 4 本目 `check_nor_seam.py`**
   （NOR 書込み経路に触れてよいのは `port/sdk_seam/nor_seam.c` だけ）を
   外す・弱める変更は不可。
   [!] **4 本目の `check_mve_predication.py` は #42 で削除した** — 前提
   （移植が VPR を保存しない）が誤りで、実際は**ハードウェアが保存する**うえ、
   そのスキャンは #66 のとおり 1 命令も検出できなかった。代わりに立っているのは
   **強制**の方: `FPCCR.ASPEN` をカーネル入場前に set → 読み戻し → 駄目なら halt
   （`port/threadx/fp_enforce.c`。継承 `LSPACT` も拒否。判断は純関数でホストテスト、
   `check_placement_budget.py` がシンボルを要求し、`cmake/fixtures/` の P2 が
   「呼び出しを消すと落ちる」ことを実証する）。**MVE は解禁済み**で、実機で
   確認する手段は `mve` コマンド。**LTO 不使用**（実測で ITCM が 3,616 B 増える。
   ITCM の 63% が IR を持たないプリビルトで元が取れない）。

   **SRAM 窓は 2 領域**（#29）: `0x3401F000` は 2nd bootloader の実行窓で
   **NOLOAD 専用**、loadable は `0x3404D000` 以上。「CONTENTS を持つセクションが
   低位窓に降りていないか」は **ldscript には書けない規則**（ld は NOBITS を
   区別しない）ので配置ゲートが ELF のフラグで検査する。

   **推論（#44）**: **`lib_spi_eeprom.a` の erase/write 系と、任意オペコード送出 4 本
   （`Send_Op_code` / `Send_Op_Read_Data` の spi/qspi 両形）は禁止シンボル**
   （このフラッシュにブートローダが載る。wio のセクタ0 と同格。`setWriteEnable`
   のみ QUAD 有効化に必要なので明示的に許可）。**アリーナのキャッシュ保守は
   「範囲ごと」にしない**（#46） — TFLM は 16 B 整列・ラインは 32 B で、外側丸めが隣の
   半ラインを巻き込む。潰すのは **`ethosu_invalidate_dcache()` だけ**（完了セマフォより前に
   呼ばれる）で、`ethosu_flush_dcache()` は本物のまま。引き渡しは `ethosu_inference_begin/end`
   に置き、アリーナ**全体**を clean / invalidate する。成功条件は
   **`job.state == DONE` かつ `job.result == OK`**。異常時はリセットの**成功を確認してから**
   invalidate、失敗なら fail-stop。**呼び出し側でキャッシュ保守を足さない**
   （TFLM は `Invoke()` 復帰前にアリーナを書く）。**`npu_open()` のペイロード検査**
   （`COMMAND_STREAM` が 1 個かつ最後 / 対象は `custom_options` ではなく入力テンソル 0 /
   `is_variable()` は拒否）は緩めない。

   **モデルは blob の名前で開く（#93 / #49 Step 4a）**:
   [!] **`npu_open()` は長さを取り、`GetModel()` の前に境界付き FlatBuffer verifier を
   通す**。`GetModel()` は cast で、以後の accessor は全て offset を辿る —
   **blob の CRC は「届いたバイト列」に対するもの**なので、PC 側で既に壊れていた
   モデルは CRC を通って無傷で着く。順序は **範囲 → 長さ → identifier → verifier →
   ペイロード走査**。長さには**下限も要る**（identifier を `raw+4` から読むため）。
   **生アドレス形にも長さ必須**（`nn open --addr <addr> <len>`。「窓の残り全部」は
   境界検査にならない）。**verifier の limits は明示**（既定は depth 64 / table 100 万で、
   生成 verifier は再帰し**シェルスタックは 4,096 B**。実測: 再帰の各フレームは 24 B 以下
   ＝ 1 段 ~64 B なので既定 64 段は ~4.1 KB ＝スタック全部。実モデルは **depth 4 /
   table 19**、出荷値は 16 / 4096）。**limits と verifier 呼び出しは
   `port/npu/npu_verify.h` の 1 箇所**で、ホストゲートが同じものをリンクする
   （既定 limits でホストが通し実機が落ちる、を作らない）。
   `NPU_ERR_MODEL_FORMAT` は magic / schema / payload と別。
   [!] **`nn open <name>` はリースを切らさない**: nn ゲート → **`npu_hw_init()` を先に**
   （`NOR_LEASE_NPU` を確保）→ その内側で**全スロット走査**（候補は VALID のみ・
   **重複拒否**・0 件 / BUSY / FAULT / MAP を別々に分類し「見つからない」に畳まない・
   **読めないスロットが 1 つでもあれば拒否**）→ **`blob_verify_leased()` で CRC** →
   `npu_open()` → **途中失敗は必ず `npu_hw_deinit()`**。
   `blob_stat_leased` / `blob_verify_leased` は**呼び出し元のトークンを取り、live か
   検査する**（「誰かがリースを持っている」は他人の寿命の話）。
   `blob_verify_leased` は **`NOR_LEASE_BLOB` も取る** — 窓ではなく
   **`blob_stage_buf` の排他**のため。
   [!] **ホスト側の検証ゲートを外さない**（`verify_vela_model`）。デバイス側の境界検査は
   置き換えにならない（単一サブグラフ / 全 op が Ethos-U / int8 I/O / offline plan /
   アリーナ / BlazeFace の shape は見ない）し、**デバイスの検査は書込みの後**で、
   malformed でも既に ~40 秒の消去と転送を消費している。公式経路は
   **`build/<board>/send_verified_model.sh`**（picocom の `--send-cmd`）:
   **staging コピー → 検証 → 同一ファイル送信**、**`--profile cls|det` は明示引数**
   （ファイル名から推測しない）、**出力は stderr**（YMODEM 線に流さない）、
   **ホスト C++ 不在は fail-closed**（skip しない）。
   [!] **`--profile` はチェック集合の選択であってモデルの識別ではない**（#95）。
   `det` は `cls` + BlazeFace の 4 出力 shape なので**非対称**で、
   `--profile cls` に det を渡すと通って送信される（実際に初回運用で起きた。
   ラッパーは PC 側起動・スロット名はボード側入力なので検知できない）。
   対処は**拒否ではなく警告** — 4 shape を持つのに `--blazeface` が無ければ
   `verify_vela_model` が `[!]` 行を出す（shape 一致は強い証拠であって証明ではないので、
   拒否するとゲートが establish できない identity を主張することになる）。
   **`cls` 側に shape 固定を足さない**（分類器に固定の出力契約は無く、`1x10` を
   pin すると正当な分類器を弾く）。
   [!] **verifier が言うのは「offset が宣言された長さの内側に落ちる」ことだけ** —
   **U55 のコマンドストリームは読まない**（opaque なバイト列。`npu_payload.c` が見るのは
   アクションの封筒であって中の命令ではなく、ドライバは整列検査の後そのまま device へ渡す。
   #93 以前からそうで、これは新しい検査の限界であって退行ではない）/ **Vela 出力として
   妥当かも言わない**（ホストゲートの仕事）/ **生 `--addr` 形は窓で境界され、スロットでは
   境界されない**（意図的に store を迂回するので、隣のスロットへ伸びる長さは
   バイト列が verify すれば通る。16 MB エイリアスの外へは出られない。
   スロット隔離が要るなら名前で開く）。
   フラッシュのメモリマップ読み出し窓は**アプリが開ける**（開けるまで窓全体が
   同一レジスタにエイリアスし、フォルトも 0xFF も返さない）。その open は
   **DMAC1 の IRQ 133 を有効化する** — EPK は番号を列挙せず ISER スナップショットで
   測ること。

   **推論の前処理（#48）**: 入力は **240x240（フレーム中央の最大正方形）を SCALE** する
   （従来は 128x128 の中央 crop で、実用距離では画角が狭すぎた）。実装は
   `port/npu/nn_preproc.c` — 依存ゼロ（HW も ThreadX も singleton も無し）なので
   **ホストテストが本体を直接叩く**。**外さない・薄めない**: この 3 つはどれも目視で
   気付けない。(a) **half-pixel 中心**の bilinear、(b) **箱の「辺」には半ピクセル項を
   付けない**（サンプリングの規約を辺に当てると全部の箱が半ソースピクセルずれる。
   同一変換の 2 表現なので `nn detect` の表示も overlay と同じ関数を通す）、
   (c) **負座標は数学的 floor**（C の 0 方向切り捨ては upscale で 1 ずれる）。
   Q8 の重みは `w1 = f` / `w0 = 256 - f` で**構成上必ず和が 256**なので
   累算は `255*256*256` = 2^24 に収まる（32 bit で足りる根拠がこれ）。
   デコーダの float は clamp も finite 保証もしないので、**int 化の前に
   非有限を弾き範囲を clamp する**。

   **ライブ overlay（#48）**: 推論は **camera producer スレッド・`consume()` 内**。
   順序は **推論（パネルガード無し）→ ガード 1 回で stage/draw/present**。
   overlay callback は**パネルガード保持中**なので block / sleep / 推論 / 他ロック /
   LCD API 再入は禁止（ガードは再帰的 = 再入は deadlock ではなくトランザクション破壊）。
   唯一の例外は純関数 `lcd_rect_wire()`。stop-pending は **join 要求より前**に立て、
   前処理前 / invoke 直前 / 推論後の 3 点で見る。
   **推論タイムアウトの定義は `port/npu/npu_hw.h` の 1 箇所**（board.cmake が parse。
   2 箇所に書かない — 以前は片方が dead だった）。

   **顔検出（#45）**: op resolver は **`MicroMutableOpResolver<1>` のまま**維持する。
   理由は「キャッシュ的に危険だから」ではない（#46 で消えた） — **CMSIS-NN（= Helium）を
   持ち込まない / Vela が全面 offload していないモデルを `AllocateTensors` で
   うるさく落とす**という設計判断である。境界の型変換は**ファイル側で剥がす**
   （`scripts/tflite_strip_boundary.cc`）。
   **モデルは blob のアセットで、固定アドレスの予約を持たない**（#93 / #94）。
   `nn open <name>` がスロットから読む。`GROVE_MODEL_*` / `--target flash-model-*` /
   `model-cls` / `model-det` / `blob-tail` は **#94 で削除済み。復活させない**。
   **配置は「予約」であってファイルではない** — `cmake/check_flash_partitions.py` は
   予約どうしの非重複と 16 MB 収容を**成果物が 1 つも無くても**検査し、
   **存在必須なのは今から書く成果物だけ**にする（全ファイルを要求すると、
   コミットできない検出モデルのせいで**ただのファーム焼きが止まる**。
   守っていない操作を止めるゲートは消される）。比較は**ファイル範囲ではなく
   破壊フットプリント**（xmodem の 128 B パディング + 消去ブロック丸め **4 KB**
   ＝常駐 2nd BL が実際に発行する唯一の消去単位。#88 で逆アセンブルにより確定。
   `0x52`/`0xD8`/chip erase は 1 度も発行されない。**全 receiver への上界ではなく
   この経路の実測値**）。
   **モデルの送信は staging コピーに対して検査 → `verify_vela_model` →
   同じファイルを送信**の順で、検証を README の手順に出さない（ホスト C++ が
   無ければ skip せず拒否）。#94 で `--target flash-model-*` が消えた後もこの鎖は
   `build/<board>/send_verified_model.sh`（picocom の `--send-cmd`）に残る。
   **これらのゲートを外す・緩める変更は不可**。
   **firmware 予約は 2 MB で、ブートローダ自身の算術から導出する**（#85。A/B 2 スロット
   x `Image max size 0x100000`。`GROVE_FW_SLOT_SIZE` x `GROVE_FW_SLOTS`。`0x200000` を
   ベタ書きしてコメントで説明しない）。**[!] `GROVE_FLASH_SIZE` / `GROVE_ERASE_GRAN` / `GROVE_SLOT_HDR_COPIES` /
   `GROVE_FW_SLOT_SIZE` / `GROVE_FW_SLOTS` は実測値でノブではない** — `cmake/flash_geometry.cmake`
   が非キャッシュで持ち、食い違う `-D` は configure 時に FATAL_ERROR。**CACHE に戻すと
   宣言と検査が同じ値から出るため `-D` 1 つで両方動き、検査は OK のまま通る**（4 通り実測）。
   **キャッシュ化に戻す変更は不可**。
   **[!] `GROVE_ERASE_GRAN` と `GROVE_SLOT_HDR_COPIES` は別の事実で、束ね直さない**（#88）。
   スロットヘッダ予約を「1 消去ブロック」から導くと、粒度を実測の 4 KB へ絞った瞬間に
   予約が 1 セクタに縮み、**backup ヘッダが blob（当時の blob-tail）に落ちる**
   （`flash_geometry.cmake` が fail-open として名指しで却下している当のもの）。
   **[!] 外付け NOR のライフサイクルは `port/nor/` 所有**（#86）。QSPI/XIP の立ち上げと
   **IRQ 133 の EPK wrapset は `port/nor/` のもの**で、NPU の snapshot はその後に取る。
   **NPU 側へ戻すと `nn close` が IRQ 133 を disable する**（#86 の欠陥そのもの）。
   リースは `npu_hw_init` 取得 / `npu_hw_deinit` 解放（`npu_open`/`npu_close` は触らない）で、
   トークンは成功時にのみ `hw_ready` と同時コミット。ベンダの `enable_XIP` は MPU を
   再構成して戻り値を検査しないので読み戻しはこちら持ち。**JEDEC ID は XIP 前にしか読めない**。
   `nor` に **生オペコードを足さない**（境界付き `write`/`erase` は #88 Part C/E で
   着地済み。追加するなら writer 経由で、`nor_span.c` の判断を通す）。
   [!] **NOR 書込み seam（#88 Part D）**: 内側 4 本
   （`hx_lib_qspi_eeprom_{erase_sector,write,erase_all,word_write}`）を `-Wl,--wrap` で
   `port/sdk_seam/nor_seam.c` へ寄せる。**外側 `hx_lib_spi_eeprom_*` ではなく内側**を
   wrap する（外側は薄いフォワーダで、外側だけ wrap すると内側が直に届く）。
   **`erase_all` と `word_write` の wrapper は `__real_*` を名指ししない** — これが
   ベンダ実装を GC させ、`check_placement_budget.py` の absence をこの 2 本について
   恒久的に有効に保つ。書けるのは **`blob` だけ**（#94 以降は
   `0x200000..0xFFE000` の一本。`slot-header` は含めない）、消去は
   **4 KB / enum 0 のみ**、境界は減算ベース、`NOR_ST_WRITING` 以外は拒否。
   [!] **ゲートは ELF ではなく ld の map で live/discarded を判定する** — GC 後の ELF に
   入力セクションの出自は残らず、しかも `spi_eeprom_comm.o` は `open`/`read_ID`/
   `enable_XIP` のため**既にリンク入力**で、その外側フォワーダが内側名への
   リロケーションを持つ。だからオブジェクト単位の規則は正しいリンクでも常時 fail し、
   かといって許可すると別 TU が外側を生かすだけで穴が開く。**単位は入力セクション。**
   map は **PRE_LINK で消し BYPRODUCTS で宣言**する（古い map は別のリンクについて
   答える）。**入力マニフェストは `$<TARGET_OBJECTS:>` から生成**し、map の `LOAD` と
   突き合わせて未計上を拒否。**LTO は拒否**（IR にはリロケーションが無く、
   リンカの実入力は `/tmp/cc*.ltrans.o` になる）。**アドレス取得のリロケーションも拒否**。
   [!] **これは defence in depth であって能力の証明ではない**（#87）— 読み出し経路が
   `hx_drv_spi_mst_get_dev` / `hx_drv_dmac_get_dev` / `DMA_send` 系を既に引き込んでおり、
   禁止・監査のどの名前にも触れずに WREN + 任意オペコードを組める。
   [!] **ベンダの戻り値は成否を報告しない**（`erase_sector` は WP 解除の結果で、
   その `clear_write_protect` は出口が `movs r0,#0` の 1 つだけ / `write` は定数 0）。
   **唯一の真実は読み戻し**で、それは writer（`port/nor/nor_write.c`）の責務。
   ただし**負の値は wire に出る前の拒否**なので意味がある（`-28` = 窓が落ちていない、
   `-50` = WEL が 21 回で立たなかった）。
   **Part C 着地に伴い `hx_lib_qspi_eeprom_{erase_sector,write}` と
   `hx_lib_spi_eeprom_clear_write_protect` の 3 名は FORBIDDEN から外れた**
   （`erase_all` / `word_write` は外さない。**戻す変更は不可**）。
   [!] **書込みトランザクションは 1 本の手続きで、途中で返らない**（#88 Part C）:
   claim → 窓を落とす（SCU 読み戻しで確定）→ **JEDEC 再読（canary）** →
   操作 → 窓を戻す → **読み戻し照合** → commit。**窓の復帰と commit は
   操作が失敗しても必ず走る。**
   [!] **canary は liveness のための唯一の手** — ベンダの write 経路は全部
   `DMA_send_recv` のタイムアウト無しスピンで、窓を落とした直後の最初の 1 本で
   実際にコンソールが固まったことがある。**最初の 1 本を「何も変えない read_ID」に
   する**（`nor cycle` がその preamble/postamble だけを実行する）。
   [!] **読み戻し不一致は terminal `FAULTED`** — 「配列が受け付けなかった」と
   「窓が嘘をついている」を区別できず、後者なら以後の全読み出し（`nn` が
   その場で parse するモデルを含む）が疑わしくなる。**ベンダが wire 前に拒否した
   場合と transport 無応答は fault させない**（曖昧さが無いので `incomplete` /
   `no transport` として窓を戻して継続）。
   [!] **照合対象はベンダが受け付けた prefix だけ**。ページ分割（256 B 境界を
   跨がない）が「どこで止まったか」を正確にする — 長いバッファを渡すと
   ベンダは複数ページを書いてから失敗し得るので `done` が嘘になる。
   [!] **staging バッファの理由は DMA reachability ではない** — ベンダの `write` は
   `uint8_t *` を取り `word_switch` 経路で**呼び出し元バッファを in-place で
   byte-swap する**。TCM 禁止則は SSPI/WDMA3 の話（ベンダは自分の DTCM プールへ
   memcpy してから DMA する）。
   [!] **1 トランザクションは無データでもステータスレジスタを 2 回書く** —
   窓を落とすと `set_quad_mode` が QE を落とし、戻すと立てる（値が同じなら
   書かないので 2 回で頭打ち）。`nor cycle` も無料ではない。
   途中でリセットしても QE=0 は工場状態かつ 2nd BL が焼込み後に残す状態なので起動する。
   [!] **XIP 窓を読む者は全員リースを持つ**（#90。`NPU` / `SCAN` / `DEVMEM` の 3 スロット）。
   `devmem` は無リースだった — 立っていない窓を読むと fault も 0xFF も返さず
   **16 MB 全体が 1 レジスタにエイリアスして嘘の内容を印字**した（実機で観測）。
   writer が背景アクセスの足元で XIP を落とす方は実在するが有界（dump は
   `CLI_DEVMEM_DUMP_MAX_LEN` 上限、bg は前景より低優先度 + NO_TIME_SLICE）。
   **acquire が窓を立てる操作なので 1 つで両方閉じる。**
   [!] **単一インスタンスの拒否はコンソールから再現不能** — ホストテストが唯一の検査。
   **XIP probe は writable interval の外**（`_Static_assert`。#90 以前は blob 内の 0xB00000）。
   [!] **probe は読む前に自分で invalidate する**（#88）— ベンダの `enable_XIP` は
   **base から 512 B しか無効化しない**ので、probe B も writer が変えた範囲も含まれない。
   [!] **`NOR_ST_WRITING`**（#88）: write は XIP を落とすので readers を締め出す。
   **state と reader マスクは同一クリティカルセクションで読み、GO を得た者が
   publish してから抜ける**（別々だと間に `nor_acquire` が入る）。
   **`NOR_ST_OFF` は BUSY で「bring it up」ではない** — bring-up は reader の仕事。
   [!] **`NOR_ST_RESERVED` と予約トークン**（#91）: **トランザクションを跨いで持つ
   所有権**。commit は `NOR_ST_XIP` ではなく **RESERVED に戻す**（XIP を publish
   するとトランザクションの隙に reader が入る。`blob write` は 30 トランザクション
   から成り、その間 YMODEM が前景を塞ぐので背景ジョブが実際に走る）。
   **リースの 4 枠目にはしない**（mask に足すと writer が自分を BUSY にする）。
   **state と owner は同時に publish する**。**owner と state の不整合は terminal**
   — `RESERVED + owner==0` を「トークン違い」で拒否すると誰も解放できず永久 BUSY。
   **予約は全ての出口で返す**（トークンはローカル・単一解放点・console 解放は内側）。
   契約は**シェルの協調的 kill まで**で `tx_thread_terminate()` は対象外。
   [!] **`nor info` はリースを取らない**（#91）— エイリアスを読まないので不要な上、
   予約中に取れず「なぜ busy か」を言う唯一のコマンドが使えなくなっていた。
   `OFF` のときだけ bring-up 目的で取る。**レジスタは窓が上がっている状態
   （XIP / RESERVED）でのみ採取し、`WRITING` では古い値を出さず「未採取」と言う**。
   **スナップショットは state 込みで単一クリティカルセクション**。
   [!] **長い消去はトランザクションを割らない**（#91）— 割ると窓の down/up ごとに
   不揮発ステータスレジスタが 2 回増え、無界スピンを踏む機会も増える。
   **1 トランザクションのまま `erase_run()` にコールバックを注入**する。
   コールバックは**窓が落ちた状態**で走る（許可は `cli_cancel_requested` /
   `cli_print` / `log_write` のみ。エイリアス読み・リース取得・`nor_write_*` 再入は
   禁止）。**呼ぶのは 1 ユニット消し終えた後**（先頭はヘッダセクタなので、前で
   中断できると古いヘッダが残る）。**cancel とベンダ拒否はどちらも INCOMPLETE だが
   保証が違う** — 「消去が失敗した＝空になった」とは読まない。
   [!] **「read-only」とは書かない** — 配列は触らないが初回 bring-up は QE ビット
   （NOR の不揮発ステータスレジスタ）を書く。`nn open` も従来から同じ。
   `nor scan` は**全バイト読む**（サンプリングは偽の隙間を作り、占有を過少報告する）。
   予約は 2 スロット分だが**1 イメージは 1 スロットに
   収まる必要がある**ので `--image-max` で別に検査する（無いと 1〜2 MB のイメージが
   ビルドを通り、実機で `ERR_IMAGE_SZ` になる）。**焼き先はスロット交替なので
   `0x0` も `0x100000` も「ファーム」であり、どちらか一方ではない。**
   `0x200000..0xFFE000` は **blob 予約の一本**（14,671,872 B。#94 でモデル予約と
   `blob-tail` を畳んだ結果）。**境界付き writer は #88 Part C/E で入り**、
   **実データの書き手は #92 で入った**。
   **読み側は #92 で入った**（`blob list`/`info`/`read`/`free`）:
   **スロット表は `nor_seam_limits` の consumer で、第二の宣言を作らない**
   （`blob_map_check()` が `lo`/`hi`/`unit` を引数で受ける）/
   **identity は基底アドレスで添字ではない**（永続する物に添字を書かない）/
   **読み手は `NOR_LEASE_BLOB` を取り、読む前に自分で invalidate する**
   （ベンダの XIP 復帰は窓先頭 512 B しか無効化しない）/
   **`empty` は「ヘッダが無い」であって「空きフラッシュ」ではない**
   （実機 baseline: スロット 1 だけ `invalid` = 工場データがヘッダセクタを覆う）。
   **書き側も #92 で入った**（`blob write`/`verify`/`erase`）:
   [!] **QSPI の書込み経路は 32 bit ワード内のバイトを反転する**。`nor_write.c` が
   自前のページバッファで戻し、**短い末尾は 0xFF でワード境界までパディング**する
   （ベンダの `word_switch_func` は長さが 4 の倍数でないと**黙って何もしない**）。
   **program アドレスは 4 バイト整列必須**（`nor_span` と seam の両方が拒否）/
   [!] **定数バイトのテストではこの種の壊れ方は原理的に見えない** — `nor write` の
   既定はアドレスで種を振った可変パターン。**「通った」を「経路が正しい」と読まない** /
   [!] **未消去への program は拒否**（terminal fault ではない。窓を落とす前に読んで判定）。
   **読み戻し不一致の terminal は残す** / 消去は**スロット選択の後・コンソール確保の前**、
   **消去が OK の時だけ受信を始める** / **警告文はスロットが決まってから出す**
   （拒否の前に「全部消えます」と言わない）/ **キャンセルの結果は `dmesg` にしか出ない** /
   **受信中はローカル Ctrl+C が無い**（0x03 はファイルの一部）。
   （`0xB70000..0xB7B000` の 44 KB は #88 で blob に入り、実機の `nor scan` で
   **全 0xFF を確認済み**。）**スロット表は #94 で全面 re-carve した**
   （`0x200000` から大きい順: 4M x1 / 2M x2 / 1M x3 / 512K x3 / 256K x2 = 11 スロット、
   13 MB、`0xF00000` 終端。`0xF00000..0xFFE000` の 1,040,384 B は**意図的に未 carve**）。
   [!] **全面 re-carve は規則ではなく一度きりの支払い** — identity は基底アドレスなので
   通常許されるのは **append だけ**。#94 でやったのは、当時 store にあったのが
   `cls`（基底 `0x200000` のままで生き残る）+ `det` + テスト 1 件で**再送が安い唯一の
   瞬間だった**から。**次の re-carve はその時点の中身を全部払う。以後は append。**
   `test/test_blob_map.c` が表全体を要素ごとに pin し、連続性とクラス降順も見る。
   [!] **4 MB クラスは実機が動かせる大きさで決めてあり、今の中身とは無関係**。
   モデルサイズを縛るのは**スロットだけ**（flatbuffer は XIP 窓から in-place で読まれ
   コピーされない / アリーナは重みでなく feature map で決まる — **164,512 B の det の方が
   1,704,672 B の cls よりアリーナが大きい**）。
   [!] **旧モデルのバイトは消えていない。表示は各スロットの「ヘッダセクタ」だけで決まる**
   （旧パーティションの位置ではない）: 再フラッシュ後は 0=`valid`(cls) /
   2・3=`empty`（座礁した `test-small`/`det` がペイロードに。`empty` は「ヘッダが無い」で
   あって「空きフラッシュ」ではない）/ 5・6=`invalid`（旧 MobileNet がヘッダセクタを覆う。
   `blob erase` が要る）。**`det` と `test-small` は送り直した。**
   **現在: `cls` = slot 1 `0x600000` / `det` = slot 9 `0xE80000`**（cls を 4 MB スロットから
   退かして、そこは「他に入らないモデル」用に空けてある）。実測 cls 30 / det 6 トランザクション。
   [!] **blob の移動は `erase` → `write`** — VALID なまま別スロットへ書くと `DUPLICATE` で拒否。
   **最終ブロック `0xFFE000..0x1000000` は `slot-header` 予約で絶対に書かない** —
   **ブートローダのスロットヘッダ**（`flash_end - 0x1000` と `- 0x2000` に 20 バイト、
   magic `"HIMAXWE2"` + チェックサム）。壊すとフォールバックで**黙って前のビルドが起動する**。
   **地図はパートの全バイトを claim する**（未宣言の run は空きではなく無防備な容量）。
   [!] **ここは空ではなく、工場 SenseCraft の FlashDB KVDB（`0x300000`）とデータ
   （`0x400000`/`0x500000`）が載っている**。最初の書込みで恒久的に消える
   （2026-08-23 ユーザー決定で了承済み）。**我々の `lib/flashdb` とは別物**
   （wio は `FDB_WRITE_GRAN=8`、実機は `32`）。占有の確認は 17 点のサンプリングでしか
   していないので、**実際に書き始める前に read-only の走査が要る**。
   デコーダは **3 ボード共有の `svc/blazeface.c`**（#97）で、**npu シングルトンに
   依存せず** `svc/tensor.h` の記述子配列を受け取る（ホストテストが本物を
   コンパイルできる条件）。`port/npu/nn_decoder.c` が `npu_tensor` からの変換と
   **状態・候補バッファの所有**を持つ — **共有 TU は可変記憶域を 1 バイトも
   持たない**（各ボードが自分の配置とゲートを保てる条件で、
   `cmake/check_no_mutable_storage.py` が監査コンパイルで強制する。**緩めない**）。
   **全 896 アンカーを必ず走査**し、候補は上限付き top-N にする — 満杯で打ち切ると
   ピークスコアが前半の最大になり、NMS が「最初の 64 個」を見る。
   出力 4 本は **shape で探す**（生成順は文書順と違い、Vela 前後でも変わる）。
   4 本の scale/zp は**全部違う**ので、共有の脱量子化定数を作らない。

8e. **grove-vision-ai-v2: TIMER2 は EPK 専有、WFI の前提は強制する（#25）。**
   `thread` の cpu%（Execution Profile Kit）の時間源は **Himax TIMER2**。
   **触ってよいのは `port/threadx/tx_glue.c` の bring-up 1 箇所だけ**で、SCU
   （クロック許可 / 分周 / CPU 所有）と TIMER2 の 4 レジスタを MMIO 直叩きし、
   **RELOAD 全 1・割込み不許可の自由走行**にする。RELOAD が全 1 なのは時間源の要件
   （ダウンカウンタの反転が mod 2^32 アップカウンタになるのは全 1 のときだけ）。
   **ベンダの `hx_drv_timer_*` は API 丸ごと禁止シンボル**（例外は
   `hx_drv_timer_init` のみ — SDK の platform init が全 9 個に対して呼ぶが base を
   記録するだけ）。名前リストでは `hx_drv_timer_hw_start(TIMER_ID_2, ...)` が抜けるので
   接頭辞で塞ぐ。**この禁止を緩めてはならない。**
   プリビルトのカメラ系アーカイブがこの 4 シンボル
   （`hw_start` / `hw_stop` / `cm55x_delay_ms` / `_us`）を参照するが、対処は
   **ゲートの緩和ではなくリンカ `--wrap` による board 所有の seam**
   （`port/sdk_seam/timer_seam.c`、#30）。`__real_*` は呼ばない ので最終 ELF に
   禁止接頭辞のシンボルは 1 つも残らず、ゲートも本項の不変条件もそのまま維持される。
   seam は **id != TIMER_ID_0 と再現しない設定をレジスタ非書込みで拒否**し、
   `hw_stop()` は ISR から呼ばれ得るので **ISR-safe**（ログ / mutex / TX API /
   fail-stop ループを入れない）。**引数認識ゲートは採らない**（tail-call・
   address-taken relocation・関数ポインタ・veneer を追う脆い全プログラム解析になる）。
   加えて **`tx_glue_profile_ok()` が毎回実行時に再検証する**
   （TIMER2 の CTRL/RELOAD と計数、登録した全ベクタの同一性、
   **有効 IRQ 集合 ⊆ 登録集合**、EPK ネストカウンタが 0）。
   ビルド時ゲートは唯一の砦ではなく多層防御の一枚。
   **EPK の会計対象 IRQ は集合**（`tx_glue_profile_register_irq()`、#30）。
   **「有効だが未ラップ」というカテゴリを作らない** — 有効にするなら必ずラップして
   登録し、駄目なら**無効のままステータスをポーリング**する。ベンダ由来で
   IRQ 番号が事前に分からない周辺は、**実測して決める**
   （`port/sdk_seam/epk_irq_wrap.c`: ISER をスナップショット →
   PRIMASK 下でベンダ bring-up → 増えた線を全部ラップ・登録。
   1 本でも失敗したら bring-up ごと諦める）。
   時間源の分担: EPK = TIMER2（スリープ中も進む必要がある）/ udelay と membench =
   DWT CYCCNT / CoreMark = `tx_time_get()`。混ぜない。
   **`TX_ENABLE_WFI` も `TX_EXECUTION_PROFILE_ENABLE` もコンパイル時スイッチ**で
   実行時に降りられない。だから WFI は前提を**強制**する（カーネル入場前に
   `SCB->SCR` の SLEEPDEEP / SLEEPONEXIT を clear → 読み戻し → 駄目なら fail-stop。
   `TX_LOW_POWER` は使わない）。`hx_lib_pm_*`（Himax PM）は禁止シンボル接頭辞。
   EPK 側は「信用できない」ことを**言える**ようにするのが唯一の手段 —
   ベンダ UART0 ベクタのラップに失敗したら**ベクタを戻してコンソールを生かし**、
   共有 `thread` が `--` と理由を出す（`cli_thread_cpu_source_ok` 弱シンボル）。
   ベンチマークは絶対値を出すので、入口で ThreadX tick と SCU の CM55M 周波数を
   検査して駄目なら実行拒否し、実行後に再読み出しして動いていたら警告する
   （`cmds/bench_gate.c`）。DWT を tick で較正するのは循環（SysTick reload が同じ値
   由来）なので採らない。**SCU の値が「正しい」ことは証明できない**（独立した時間源が
   このボードには無い）ので、結果は「検証済みの絶対値」ではなく
   「明示したクロックの下での実測値」として出す。
   **CoreMark の翻訳単位だけ `-fno-tree-vectorize`** を残す（#42）。MVE 禁止では
   なく、公表値 3.13 CoreMark/MHz との**基準線の連続性**のため — 外すなら測り直して
   比較記述を全部書き直すところまでが 1 セット。
   membench のバッファは NOLOAD セクションなので **測定前に明示的に全書き込み**が要る
   （startup の copy/zero を通らない。TCM の ECC 未初期化読み出しも避ける）。
   MPU / キャッシュ属性は firmware で decode せず**生レジスタをダンプ**する
   （継承状態で TRM も無い。SRAM 行を cacheable と断定しない）。

8f. **grove-vision-ai-v2: カメラ（OV5647 / #35, #54）。**
   データパスは固定（640x480 RAW10 2 lane（センサ側でビニング済み）→ INP crop 無し →
   4:2 binning → 320x240 → HW5x5 demosaic BGGR → WDMA3）。
   **IMX219 は #54 で削除済み**。ソフト自動露出（`cam_ae_step`）・`camera depth`・
   frame_lines 引数（0x0160/0x0161）も一緒に消えた。`camera auto` は
   **センサのオンチップ AEC + このポートのソフト WB**。
   センサ記述子の関数ポインタ seam は 1 エントリでも維持する
   （register map は部品ごとに全く違い、SCCB は未実装レジスタも ACK するため）。
   **WDMA3 出力はプレーナ B/G/R**（インタリーブ RGB565 ではない。HXCSC は入力
   アンパッカーであってパッカーではない）。パックはソフト
   （`port/camera/cam_convert.c`、`-fno-tree-vectorize`）。
   - **DMA が触るバッファを TCM に置かない**（fault せず無言で転送されない）。
     `.cam_raw` / `.cam_slots` は SRAM NOLOAD で、`check_placement_budget.py` の
     RESIDENCY がシンボル→サイズ→セクション→領域を pin する。**外さない。**
   - **WDMA3 バッファは frame-ready 後・CPU 読取前に、完了した面だけ全長 invalidate する**
     （ベンダのグルーは 32B の JPEG サイズ語しか invalidate しない。真似しない。
     #59 以降 landing buffer は 2 面で、DMA が書いている側の面には触れない）。
   - **[!] WDMA3 のチャネルアドレスを書くのは `cam_wdma3.c` だけ、かつ xDMA を
     disable してから**（enable 中の書換えは根拠が無い。#59）。disable を跨ぐ
     マスクは **WDMA3 専用の `hx_drv_xdma_get/set_WDMA3INTMask` ペア**で行い、
     `hx_drv_xdma_set_mask()` は使わない（両マスクレジスタを丸ごと書き潰す）。
     マスクは fault 経路も含む**全ての出口で復元**する。arm 時のステータス監査は
     fail-closed（acknowledge してよいのは premature-disable のみ・カウント必須・
     再読出しで clean を要求）。**緩める変更は不可。**
   - **停止は単一ルーチン `cam_imx219_full_stop()` に収束させる**（正常停止 /
     timeout / terminal / bring-up 失敗の 4 経路とも）。**再開はバリア**:
     フル停止で静止させてから clear → pending clear → semaphore drain → 再 arm。
     **クリアの前に必ず停止**（callback は status だけで世代を持たないので、
     走ったままクリアすると遅延イベントが新ストリームの初フレームに化ける）。
   - **エラーはフレームより優先**（sticky ラッチを先に見る）。**未知の負値は terminal**。
   - **DP/CSIRX 構成は swreset を跨いで信用しない** — フル停止のたびに未構成へ倒し、
     次の start で再構成する。
   - `lcd_blit()` は **wire order (BE)** を要求し、pipeline の `FRAME_FMT_RGB565` は
     **LE**。swap は `lcd_blit_le()`（ドライバ所有）。**slot を wire order で publish して
     format を偽らない。**
   - [!] **`camera_stream_stop()` は成功時のみ join を保証する（#48）。**
     `CAM_OK` = producer 停止済み。**timeout は何も証明しない**（待ちは有界で、
     sink は `consume()` 内で推論を回せる）。**全呼び出し元は `CAM_OK` の時だけ
     detach する。** publish() は sink を pre-pin して lock を離してから
     consume() を呼ぶので、走っている sink の unlink は pipeline が耐えられない。
     未確認 join は **`CAM_ST_LOST`** = **再起動まで全ハードウェア操作を拒否**
     （`cam_api_enter()` で、mutex を取る前に）。**`CAM_ST_FAULTED` で代用しない**
     — あれは「次の bring-up で作り直す」状態で、ここでは最悪の動作になる。
     この経路では detach も teardown も所有権解放もしない。
     `camera_stream_stats()` は mutex もハードウェアも触らないので拒否理由は必ず読める。
   - [!] **stop だけが API mutex を有界待ちする（#65）。** 他の入口は `TX_NO_WAIT` の
     まま。**両方向に戻さない** — 元の `TX_NO_WAIT` では単なるロック競合が
     「producer 未確認」として返り、preview が自分の sink を捨てていた。
     **poison の判定は待ちの向こう側でもう一度**（`CAM_ST_LOST` は「not streaming」でも
     あるので、近道を前に置くと**起きていない stop を `CAM_OK` として報告する**）。
     この順序は `port/camera/cam_state.c` の `cam_stop_decide()` が**唯一の判断点**で、
     `camera_stream_stop()` に近道を書き戻すと `test/test_cam_stop.c` が
     検査できなくなる（実機では作れない分岐）。取得失敗は **`CAM_ERR_LOCKED`（-8、
     このボード固有）で poison しない** — 何も聞いていないので何も証明していない。
     判定は**両 enum とも fail-closed**（「HELD でなければ拒否」/ 成功を返す状態は
     列挙する）。**`default:` を足して塞がない** — メンバ追加時に `-Wall` が鳴るのと
     "future member" ベクタが落ちるのが検知経路。
   - [!] **センサーバスの所有者は mutex 保持下で決める（#74 / #77）。** producer は
     API mutex を取らないので、コンソールをバスから遠ざけているのは mutex ではなく
     **状態検査**。そして `camera_stream_start()` は **mutex 保持下で**
     `CAM_ST_STREAMING` を publish するので、**acquire より前に取った検査は無価値**
     （stream start 丸ごとに追い越され、`cam_bringup()` が「もう上がっている」と返して
     fall-through が producer の持つ CIS ドライバを叩く。`TX_NO_WAIT` では閉じない）。
     入口は **`cam_bus_enter()` 1 本**で、`cam_state.c` の `cam_bus_decide()` が
     **「何をすべきか」ではなく「誰がバスを持っているか」**を返す
     （probe / capture / VTS read-back / **stream start** は producer を拒否へ、
     4 つの setter は queue へ）。**`camera_stream_start()` も必ずここを通す** ——
     poison を direct に落とすと `cam_bringup()` が**ポートを再構築**する（setter の
     I2C 1 回とは被害が違う）。**sink 予約は代わりにならない**:
     `camera bench` は `camera_stream_start(NULL)` で sink 無しなので registry が空。
     契約は **`CAM_OK` ⇒ mutex 保持 + owner は DIRECT か PRODUCER のみ /
     負値 ⇒ 非保持**（ThreadX の mutex は再帰的なので、入れ子取得は
     デッドロックせず「1 回の put で保持が残る」形で壊れる）。
     **bring-up はヘルパに入れない** — `camera_set_auto()` の
     「上がらなくても CAM_OK」と「そもそも入れなかった」を別の答えにするため。
     [!] **`CAM_ST_LOST` は保持下で到達する**（poison 検査は mutex の前なので、
     preflight と acquire の間に stop の join 失敗が挟まる）。ここを direct に落とすと
     `cam_bringup()` が**ポートを再構築**する = #48 が防ぐ最悪の動作。
     [!] **判定は「STREAMING 以外」ではなく状態を列挙する** —— #65 と違って順序の
     ハザードではなく（enum は同時に両方にならない）、**広い検査**が fail open する。
     `default:` は書かない。**新しい `CAM_ERR_BUSY` の意味はコマンドごとに違う**:
     probe と read-back は「stream または API」、4 つの setter は
     **API のみ**（stream 中は queue = 成功なので「preview を止めろ」は誤誘導）。
     queue の書き込みは **`TX_DISABLE` で値とビットを一括**（mutex は producer に対して
     何も守らない）。**`cam_raw_mode` は mutex 保持区間にスコープする** ——
     `cam_step_dp()` が読み、producer もタイムアウト再起動でそこを通るので、
     API に入る前に立てると**拒否された `camera raw` でも走行中の stream を
     one-shot 用に再構成し得る**。`CAM_BUS_DIRECT` を得た後にだけ立て、
     `cam_api_exit()` が落とす（`volatile`）。
     [!] **`cmds/` は producer が消費するデータパス設定を書かない（#80）。**
     `camera bayer` が `cam_dp_set_bayer()` を直接呼んでいた —— レジスタを触らないので
     I2C / bring-up を探す掃除では見えず、**「次の capture / preview で効く」という
     印字が嘘**になっていた（producer が再起動経路で拾う）。入口は
     `camera_set_bayer()` で、**queue ではなく拒否**（phase を読むのはデータパス構成時で
     フレームごとではない = どちらにせよライブでは効かない）。
     `cmds/` が触ってよい `cam_dp_*` は**読み取りと幾何定数だけ**。
     [!] **ただし規則は「producer が消費する状態は全部所有権を通す」ではない。**
     `cam_wb`（`camera wb` / `black` / `sat` / `gamma`）は**意図的にライブ**で、
     所有権を要求すると走行中の色調整ができなくなる（#67 がまさにそれを要る）。
     境界は**消費のされ方**: bayer は**データパス構成時**に読まれるので、遅れて効く =
     嘘になる。`cam_wb` は `cam_tone_sync()` が**フレームごとに 1 回スナップショット**して
     LUT を組むので、最悪でも 1 フレームが新旧混在になって次で収束する。
     **ここに所有権を足さない。**
     **`cam_auto_on` を書くのは API mutex を保持したスレッドだけ** ——
     `cam_manual_control_taken()` を `cam_api_exit()` の**後**に置くと、
     `tx_mutex_put()` はスケジューリング点なので、その隙に入った `camera auto on` の
     結果を上書きして「センサーは auto、フラグは off」を作る（#39 の食い違いの裏返し）。
   - [!] **teardown の失敗にコンソールからの回収路を足さない（#75、決定済み）。**
     失敗時に「全部握ったまま」が正解で、持ち主のいない sink を安全に外せると主張するのは
     #48 が検討して否定した内容。**#79 が唯一の実害ケース（retryable な detach 失敗が
     恒久 `SINK_LOST` にラッチ）を根で直した**ので、残るのは
     「保持者が 8 秒 wedge した `CAM_ERR_LOCKED`」1 行だけで、そこでは producer が
     **止まっていない**（回収する対象が無い）。drain timeout の行は sink が既に unlink
     済みで予約も切れており、失うのは preview だけ。**証拠は `dmesg`** —— Ctrl+C 経路では
     コンソールに出せない（`cancel_req` 中の出力は共有コアが捨てる）が、
     `camera_stream_stop()` は両方の失敗を `LOG_ERR` でリングに落とす。
   - [!] **`camera_unsubscribe()` はストリーム中も拒否する（#65）。** poison だけを
     見ていたのでは backstop になっていない（`publish()` は pre-pin して lock を
     離してから `consume()` を呼ぶので、ストリーム中の unlink は進行中の配送と競合し、
     直後の drain は古い受け渡しカウントを見て idle と判定し得る）。
     **API mutex は取らないまま**でよいが、理由を間違えない —— **core は競合 detach を
     拒否しない**（`frame_pipeline_detach()` は ATTACHED な sink をカメラが streaming でも
     unlink する。拒否するのは遷移中と未返却 callback だけで、`cam_state` を知らない）。
     安全にしているのは **呼び出し元 1 箇所 + 確認済み stop の後だけ + sink 予約**
     （unlink までは registry に残り、`frame_pipeline_sink_count() != 0` が start を弾く、#63）。
   - `cam_state` は **volatile**。stop が mutex 待ちの**後にもう一度読む**ため
     （アドレスを取らない file-static はレジスタに保持され得る）。
   - **EPK 容量は `GROVE_EPK_WRAP_MAX` == `TX_GLUE_EPK_MAX_IRQ` == 32**
     （`_Static_assert` で結んである。片方だけ動かさない）。
     **[!] `nn stream`（#48/#99）で 31/32 に達する**（camera 26 + UART0 1 + LCD 2 +
     QSPI/U55 2。UART の DMA fallback を使うと 32）。余裕は無い。fail-closed なので
     症状は「camera が上がらない」であって黙った誤計上ではない。
     measure-then-wrap は **2 ラウンド**（間の I2C モードテーブルは PRIMASK 外。
     1 ラウンドにすると ~10 ms 割込み禁止で tick を落とす）。
   - **Timer0 の割込み到達 probe（`grove_timer_seam_probe_delivery()`）を外さない。**
     `hw_start` は「カウンタが回る」しか証明しない。probe は **PRIMASK 外・IRQ 有効**で
     実行し、失敗したら **camera bring-up ごと拒否**する。両方向の host test あり
     （`test/test_timer_probe.c`、probe はラッチするので 2 プロセス）。
   - **4 つの `__wrap_hx_drv_timer_*` は `noipa`**。`check_timer_seam.py` が名前で
     逆アセンブルを検査するので、`.part.0` に分割されるとゲートが空振りする。
   - SDK ツリーは read-only。センサのモードテーブル `.i` は**コピーせず SDK から
     include** する（pin した SHA に紐付けるため）。

8g. **f746g-disco: カメラ subscriber の drain と owner lifecycle（#72）。**
   GUI preview / `nn stream` / `net mjpeg` は 1 つの base capture の subscriber で、
   `camera_unsubscribe()` は **base を止めずに** detach する。`publish()` は sink を
   pre-pin して lock を離してから `consume()` を呼ぶので、unlink を跨いだ配送が実在する。
   - **`camera_frame_put()` は全 `consume()` の最後の文**（3 sink とも）。これが drain の
     唯一の根拠で、後ろに仕事を足すと **pin カウントは 0 のまま** owner がその仕事の
     読んでいるものを解放する。**証明できるのは「関数が返った」ではなく
     「そのコールバックが owner の所有物にもう触らない」** — sink が静的だから足りる。
   - **`CAM_OWN_DRAINING` は `camera_unsubscribe()` の前に入る。** 逆にすると drain 区間が
     丸ごと無防備で、そこに入った start が sink を再 attach し、
     `frame_pipeline_attach()` が `_pins` をリセットして**証拠を消す**。
   - **直列化（PRIMASK）は作業を跨いで保持しない**: 取る → 遷移 → 離す → drain と
     teardown → 取り直して commit。跨ぐと `tx_thread_sleep()` を跨ぎ、
     `nncam_lock` / `ltdc_lock` とロック順が逆転して teardown が deadlock になる。
   - **判定は `port/camera/cam_drain.c` と `cam_own.c` の純関数が唯一の判断点**。
     owner 側に近道を書き戻さない（実機で作れない分岐なので
     `test/test_cam_drain.c` / `test_cam_own.c` だけが検査できる）。**両方 fail-closed**
     （カウントを先に見て deadline は境界だけ / 未知の状態は refuse）。
     **`default:` を足して塞がない** — メンバ追加時に `-Wall` が鳴るのと
     "future member" ベクタが落ちるのが検知経路。
   - **DONE は `pins == 0` ちょうどだけ。`pins < 0` を released 扱いにしない。**
     `unpin_locked()` は 0 で飽和するのでパイプラインは負を作れず、負は
     「誰かが sink の帳簿を書いた」しか意味しない。それを「解放済み」と読むのは
     説明のつかない値の上で teardown を通す fail-open。
   - **遅延 start（GUIX preview）はイベントに claim を持たせる。** 既に RUNNING の
     `gui start` は AUTOSTART を post しない（claim の無いイベントは、`gui stop` が
     drain を終えた後に届いて sink を再 attach しうる）。ハンドラ側も
     `cam_own_start_claimed()` で自分の claim が生きていることを確認してから subscribe する。
   - **worker は session を返してから「parked」を公開する**（nn の
     `nncam_release_session()` → `nncam_active = 0` の順）。逆にすると、stop が parked を
     見て IDLE を commit → 新しい start が新 session を acquire → 旧 worker の release が
     **新しい session を解放する**。
   - **失敗は呼び出し元まで返す**: `CAM_DRAIN_PINNED` のとき GUI は `guix_stop()` を
     呼ばず「display returned to lcd」と言わない。`nn_camera_stop()` は -7、
     `nx_mjpeg_stop()` は `NX_MJPEG_PINS` を返し、**次の start を拒否し続ける**
     （retryable だが両方向に fail-closed）。詳細は `boards/f746g-disco/README.md`。

9. **ビルドは `_ref/` を読まない。** `_ref/`（および `../*/_ref/`）は git 管理外の資料置き場。
   CMake / スクリプトが参照するとクローンしただけでは configure できなくなる。
   C コード中の言及は出典コメントのみ可。

## ThreadX 統合（全ボード共通）

- **SysTick > PendSV**（優先度）。同一だと idle 時 PendSV スピンを tick が割り込めず tick 停止 →
  デッドロック（F746 で実証済み）。PendSV は最低優先度。
- ThreadX が自前で `PendSV_Handler` を供給（`stm32xxxx_it.c` のものと競合させない）。
- クリティカルセクションは **PRIMASK ベース**（`TX_PORT_USE_BASEPRI` 未定義）。
- `__disable_irq` 下の `tx_application_define` で `HAL_GetTick` 依存の init を呼ばない。
  **ただし前提を確認してから適用すること**: 現在どちらのボードも `tx_kernel_enter()` の前で
  割込みをマスクしていない（wio は `src/main.c` で明示的に「`__disable_irq()` を置かない」と
  記録している）。ThreadX の `tx_initialize_kernel_enter.c` も `tx_application_define()` を
  TX_DISABLE で囲まない。SysTick は `HAL_Init()` 以降走り続け、両ボードの `SysTick_Handler` は
  `tx_timer_active` ゲートより**前**で `HAL_IncTick()` を無条件に呼ぶ。
  ∴ `tx_application_define` 内で `HAL_GetTick` ベースのタイムアウトは正常に期限切れする
  （f746g-disco の `eth_init()` / MDIO がこれに依存して fail-soft する）。
  この規則が効くのは「誰かが実際に割込みをマスクした場合」であって、マスクの有無を
  確認せずに違反と判定しない。
- 割込みは TX オブジェクト生成後に有効化。

## ボード要点

- **STM32F746G-DISCO**: 216 MHz = HSE 25 MHz → PLL M25 N432 P2、VOS1 + over-drive、Flash 7WS。
  **PA9 は VCP_TX と OTG_FS_VBUS の共用**（UM1907 ソルダーブリッジ）。LED LD1 = PI1。
  メモリ: Flash 1MB @ 0x08000000 / ITCM 16KB / DTCM 64KB @ 0x20000000 / SRAM 256KB @ 0x20010000。
  I/D-cache 有効時、ITCM 配置の効果は ~0.6%。
  **udelay は DWT ではなく TIM2（2×PCLK1 = 108 MHz）** — コアは 216 MHz だが
  `CLI_CPU_CYCLES_PER_US=108` が正（EPK の time source と共用）。
  **`CLI_INSTANCE_TIME_SLICE=0`（TX_NO_TIME_SLICE）を維持する** — VCP + telnet の 2 インスタンスが
  同一優先度で並ぶが、coremark / membench / nn_run が静的状態と DWT CYCCNT を共有していて
  多重実行に非再入なため（ThreadXShell#4）。スライス有効化は再入ガード整備とセットで行う。
  FPU は**単精度のみ**（fpv5-sp-d16）— double は `__aeabi_d*` 経由。
- **Wio Lite AI**: USB は単一で **USB1_OTG_HS を FS（内蔵 PHY）動作**。CMSIS に
  `USB2_OTG_FS` / `OTG_FS_IRQn` は無く、TinyUSB(dwc2) は rhport0 を OTG_HS base +
  `OTG_HS_IRQHandler` にエイリアス（`tud_int_handler(0)`）。GPIO = PA11/PA12
  `GPIO_AF10_OTG1_FS`、USB クロック PLL3Q 48 MHz。LED0 = PC13 / LED1 = PF0 / USER = PF1
  （active-low、保持リセットで DFU）。`.axi_dma` 等 DMA 共有バッファは両端 32B align
  （D-Cache コヒーレンシ）。
- **Grove Vision AI V2**: USB-C は **CH343P USB-UART ブリッジ**（チップに USB は無い）で
  UART0（RX=PB0/TX=PB1、921600）へ。コンソールと xmodem 書込が同一ワイヤ。
  メモリ（Secure alias）: ITCM 256KB @0x10000000 / DTCM 256KB @0x30000000 /
  SRAM0+1 2MB @0x34000000 / SRAM2 384KB @0x36000000 / FLASH XIP 窓 @0x3A000000
  （M-G1 では devmem からも触らない — XIP 経路の残存状態が未検証）。
  UART0 IRQ=90、fallback 用 DMA3 combined IRQ=69、TIMER2 IRQ=36（EPK が専有し、
  割込みは常に無効）。`__NVIC_PRIO_BITS=3`。
  udelay = DWT CYCCNT + 実行時 SystemCoreClock。`CLI_CPU_CYCLES_PER_US=400` は
  実機確認済み（起動バナーが読み戻し値との不一致を常時警告する）。
  詳細（時間源の分担 / cpu% とベンダ ISR ラップ / WFI / ベンチの読み方）は
  `boards/grove-vision-ai-v2/README.md`。

## レビュー時の作法

- **「コンパイルが通る」は根拠にならない。** レジスタ/能力の主張は対象ボードの RM
  （F746 = RM0385 / H725 = RM0468。**Grove は公開 TRM が無い — SDK の WE2_S.svd と
  SDK 実装が正**）の節番号等で、配線の主張は UM1907 / schematic で裏を取る。
  裏が取れない推測は推測として明示する。
- 存在しないファイル・行・レジスタ・実機挙動を作らない。Wio の実機は 1 枚しかないので
  「焼いて試せばわかる」は安いコストではない。
- 指摘には影響（何がどう壊れるか）と具体的な修正案を付ける。

## ビルド / フラッシュ

1 ビルドディレクトリ = 1 ボード。`-DBOARD` に既定は無い（誤ったボードのイメージを黙って
作らせないため）。各ボードが必要とする submodule は `boards/<board>/submodules.cmake` が
宣言し、fetch はそこから導出される（wio の configure が F7 系 5 本を引かないための分割）。

```bash
cmake -B build/<board> -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake -DBOARD=<board>
cmake --build build/<board>
# f746g-disco: cmake --build build/f746g-disco --target flash   (ST-Link)
# wio-lite-ai: cmake --build build/wio-lite-ai --target flash    (DFU のみ。
#              dfu-shell のエイリアス。PF1 保持リセットで DFU モードに入ってから)
# grove-vision-ai-v2: cmake --build build/grove-vision-ai-v2 --target flash
#              (UART xmodem のみ。ターミナルを閉じ、プロンプトでリセットボタン押下。
#               初回 configure は Himax SDK ~480 MB を pin fetch する)
```
