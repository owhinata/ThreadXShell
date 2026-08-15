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
   領域も書く**（Himax 標準。W25Q128JW ~100k 耐久、自動ループ焼き不可。復旧 =
   boot ROM + BOOT_OPT + factory image）。ポストリンクゲート 3 本
   （`check_image_coherence.py` = 生成 .img と ELF の突き合わせ + .rodata 内
   コマンドレジストリ / `check_placement_budget.py` = 配置・予算・ベンチバッファの
   常駐・禁止シンボル / `check_mve_predication.py` = 述語 MVE を禁止。[!] この
   ゲートの前提は誤り — Armv8-M ARM の PushStack/PopStack は `HaveMve()` の下で
   VPR を退避・復元し、規則 RZWQX により MVE 命令は `CONTROL.FPCA` を立てるので
   ハードウェアが保存する（issue #42）。fail-closed なので当面は維持）を
   外す・弱める変更は不可。**LTO 不使用**（実測で ITCM が 3,616 B 増える。
   ITCM の 63% が IR を持たないプリビルトで元が取れない）。

   **SRAM 窓は 2 領域**（#29）: `0x3401F000` は 2nd bootloader の実行窓で
   **NOLOAD 専用**、loadable は `0x3404D000` 以上。「CONTENTS を持つセクションが
   低位窓に降りていないか」は **ldscript には書けない規則**（ld は NOBITS を
   区別しない）ので配置ゲートが ELF のフラグで検査する。

   **推論（#44）**: **`lib_spi_eeprom.a` の erase/write 系は禁止シンボル**
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
   **フラッシュ配置は宣言 + 検査**: モデルごとに名前付き変数
   （`GROVE_MODEL_{CLS,DET}_{FILE,ADDR,RESERVED}`）を持ち、アドレスは cmd_nn.c へ
   コンパイル定義で渡す（1 個の無名アドレスを使い回さない）。
   **配置は「予約」であってファイルではない** — `cmake/check_flash_partitions.py` は
   予約どうしの非重複と 16 MB 収容を**成果物が 1 つも無くても**検査し、
   **存在必須なのは今から書く成果物だけ**にする（全ファイルを要求すると、
   コミットできない検出モデルのせいで**ただのファーム焼きが止まる**。
   守っていない操作を止めるゲートは消される）。比較は**ファイル範囲ではなく
   破壊フットプリント**（xmodem の 128 B パディング + 消去ブロック丸め **64 KB**
   ＝この NOR の最大消去単位という保守側の境界）。
   **モデルの flash ターゲットは staging コピーに対して検査 → `verify_vela_model` →
   同じファイルを送信**の順で、検証を README の手順に出さない（ホスト C++ が
   無ければ skip せず拒否）。**これらのゲートを外す・緩める変更は不可**。
   デコーダ（`port/npu/models/blazeface.c`）は **npu シングルトンに依存せず**
   `npu_tensor` の配列を受け取る（ホストテストが本物をコンパイルできる条件）。
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
   ベンチマークの翻訳単位は `-fno-tree-vectorize`（自動ベクトル化が述語 MVE を出す）。
   membench のバッファは NOLOAD セクションなので **測定前に明示的に全書き込み**が要る
   （startup の copy/zero を通らない。TCM の ECC 未初期化読み出しも避ける）。
   MPU / キャッシュ属性は firmware で decode せず**生レジスタをダンプ**する
   （継承状態で TRM も無い。SRAM 行を cacheable と断定しない）。

8f. **grove-vision-ai-v2: カメラ（IMX219 / #35）。**
   データパスは固定（3280x2464 RAW10 2 lane → INP crop 3200x2400 → 10:2 binning →
   4:2 subsample → 320x240 → HW5x5 demosaic BGGR → WDMA3）。
   **WDMA3 出力はプレーナ B/G/R**（インタリーブ RGB565 ではない。HXCSC は入力
   アンパッカーであってパッカーではない）。パックはソフト
   （`port/camera/cam_convert.c`、`-fno-tree-vectorize`）。
   - **DMA が触るバッファを TCM に置かない**（fault せず無言で転送されない）。
     `.cam_raw` / `.cam_slots` は SRAM NOLOAD で、`check_placement_budget.py` の
     RESIDENCY がシンボル→サイズ→セクション→領域を pin する。**外さない。**
   - **WDMA3 バッファは frame-ready 後・CPU 読取前に全長 invalidate する**
     （ベンダのグルーは 32B の JPEG サイズ語しか invalidate しない。真似しない）。
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
   - **EPK 容量は `GROVE_EPK_WRAP_MAX` == `TX_GLUE_EPK_MAX_IRQ` == 32**
     （`_Static_assert` で結んである。片方だけ動かさない）。
     **[!] `nn preview`（#48）で 31/32 に達する**（camera 26 + UART0 1 + LCD 2 +
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
   - SDK ツリーは read-only。IMX219 のモードテーブル `.i` は**コピーせず SDK から
     include** する（pin した SHA に紐付けるため）。

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
