# Wio Lite AI -- standalone USB DFU bootloader

A **TinyUSB standard-DFU bootloader** for the STM32H725AEI6.  It lives in
internal flash sector 0 (`0x08000000`, 128 KB) and uses `dfu-util` to program
applications into the **internal-flash app partition** (`0x08020000`, sectors 1-3
= 384 KB).  Sources are in this directory; the reference build lands in
`build/wio-lite-ai/boot-reference/boot.{elf,bin,hex}`.

> **[!] This tree is FROZEN, and it is built here only as a reference build.**
> Nothing in this repository can flash it -- see "Flashing the bootloader" below
> for why that is deliberate.  The seven sources are byte-identical to the donor
> repository `owhinata/wio-lite-ai` at commit `09468bb`, their SHA256s are pinned
> in `../cmake/boot_manifest.sha256`, and the linked image is checked against a
> golden hash on every build by `../cmake/check_boot_safety.py`.  This README is
> the one file that was allowed to change: it was translated, because this
> repository writes documentation in English.
>
> **Issue numbers in this directory (`#25`, `#19`, `#39`) are the DONOR's**, from
> `owhinata/wio-lite-ai`, not this repository's.  They are left in place because
> changing them would change the bytes.

> **2026-08-01 (donor issue `owhinata/wio-lite-ai#25`)**: the app moved from
> execute-in-place on the external OCTOSPI2 flash (`0x70000000`, 54 MB/s) to the
> **internal flash** (`0x08020000`, 905 MB/s).  With that, **OCTOSPI2 was removed
> from the bootloader entirely** (`octospi.c` deleted, no memory-mapped bring-up,
> no jump to an external app).  What is left is "DFU into internal flash, then
> jump to the internal app".  DFU still exposes a single alternate setting
> (alt 0 = internal), so the `dfu-util -a 0 -D app.bin` command line did not
> change -- only the destination did.

## Status

**The image built here has never been run on hardware, and this repository makes
no claim that it has.**  It is a *reproducibility baseline*: rebuilding the donor
at `09468bb` in a clean build directory produces exactly these bytes, so a HAL,
TinyUSB, toolchain or build-system change that would have altered the bootloader
is caught the moment it happens.

    sha256  97ba7060d3fcd45aa3c1f585746add46e047240c7c0440c6514e50f6d0337bdf
    size    30,524 B  (23.3% of the 128 KB sector-0 budget)
    size(1) text 30328 / data 192 / bss 4424

The golden values live in `../board.cmake` (`BOOT_GOLDEN_SHA256`,
`BOOT_GOLDEN_SIZE`) and are enforced by the gate.

### Donor-era hardware verification (a DIFFERENT image)

The log below belongs to the donor repository and to a **different build**: the
pre-`#25` XIP bootloader, 30,332 B, whose DFU alternate name was
`@0x70000000`.  It is kept because it is the evidence that this design works on
the board, but it is **not** evidence about the 30,524 B image described above --
no readback hash was ever recorded for that one.  The two must not be conflated.

> **2026-07-12 (donor)**: TinyUSB changed from the copy vendored inside tinyuf2
> to the **`hathach/tinyusb` 0.21.0 submodule** (0.21.0 folded `usbd_control.c`
> into `usbd.c`).  boot.bin 29,496 -> 30,332 B.  Reprogrammed to `0x08000000`
> with a known-good Discovery ST-Link in `mode=UR`: download verified, readback
> byte-identical, RDP 0xAA (not bricked).  **The whole path -- boot flow, DFU
> enumeration, DFU end to end -- was re-verified on the board** with no
> regression from tinyusb 0.21.0.

That verification, on board #2, covered:

- SWD/UR still alive after programming, RDP 0xAA (no bricking), readback
  byte-identical.
- **Boot flow (as it was then)**: reset -> clock + OCTOSPI2 memory-map -> app
  validation -> jump.  The jumped-to app's CDC dump of the clock tree matched the
  original TinyUF2's almost exactly; the only difference was one bit of
  `PLLCFGR.PLL3VCOSEL` (MEDIUM vs WIDE), and both ranges are valid at a 240 MHz
  VCO, so it was harmless.
- **DFU mode** (reset with PF1 held): `dfu-util -l` listed
  `name="Wio Lite AI app @0x70000000"` (or `"OCTOSPI2 FAIL id=XXXXXX"` when
  OCTOSPI2 bring-up failed).  The red LED (PC13) is on throughout DFU mode.
- **End to end**: `dfu-util -D blink.bin` -> written to 0x70000000 -> manifest ->
  automatic reboot -> blink running, observed on the board.  Proof that the bytes
  that arrived were the bytes that ran.

## How it works

After reset `main()` brings up the clock tree itself (`clock.c`) and then decides:

```
PF1 held?            --yes--> DFU
internal app valid?  --yes--> jump 0x08020000
                       no  --> DFU
```

- **DFU mode**: enumerates as a composite DFU + CDC device.  Downloads are
  written through `iflash.c` into the app partition (`0x08020000`, sectors 1-3).
  After the manifest phase it reboots into the new app.  The red LED is on.
- **Jump**: set VTOR and MSP, branch to the reset vector.  SysTick is stopped
  first: apps such as blink leave `SysTick_Handler` as the default infinite-loop
  handler, and a stray tick would hang them.

**An app counts as "valid"** when vector[0] (MSP) points into on-chip RAM and
vector[1] (Reset) is inside `0x08020000..0x0807FFFF` with the Thumb bit set.
Reading erased flash returns all-ones **without raising an ECC error**
(RM0468 sec 4.3.10), so a board that has never had an app written is safe to
test this way.  An old external-XIP `.bin` (reset vector at `0x700xxxxx`) is
rejected here too.

DFU mode is the safe fallback: an erased or invalid app always lands in it, so
the board can always be reloaded.  **The option bytes, RDP, DBGMCU and the SWD
pins are never touched**, so even a bad configuration can still be reprogrammed
over SWD.

### Internal-flash programming design (donor issue `owhinata/wio-lite-ai#25`)

| | |
|---|---|
| Sector layout | One bank, 128 KB x 4 (RM0468 Table 15).  Sector 0 = bootloader, 1-3 = app |
| Sector 0 protection | `iflash_erase_sector()` **rejects anything outside 1..3 before it reaches the HAL**.  Program offsets are relative to `0x08020000`, so no argument can reach sector 0.  The HAL's own `IS_FLASH_SECTOR` accepts up to `FLASH_SECTOR_TOTAL=8` (it assumes the 1 MB part), which is why this file does its own bound |
| Device check | The internal path is enabled only when `FLASHSIZE_BASE` reports 512 KB.  Otherwise DFU downloads are refused and no jump is taken |
| Write granularity | 32 B (256-bit) flash word plus 10-bit ECC.  A non-virgin word cannot be overwritten (RM0468 sec 4.3.9), so: erase, then 32 B-aligned writes, with a short tail padded to 0xFF |
| **Vector-last commit** | The first 32 B (MSP/Reset) are written last.  If a transfer is interrupted the MSP is still the erased `0xFFFFFFFF`, so the next boot is guaranteed to enter DFU.  With no external fallback left, this is the only thing that makes an interrupted transfer non-fatal |
| Session check | A commit only happens for one continuous transfer that started at `block 0` (`off == next_off` is enforced).  State is discarded on abort or error |
| Same-bank programming | The bootloader runs from sector 0 while erasing sectors 1-3.  Reads are queued and served afterwards, so the instruction fetch **stalls and resumes** (RM0468 sec 4.3.8, same as ST's own H723 example).  **Nothing at all runs during the erase, interrupts included** |
| DFU poll timeout | 2500 ms for the blocks that carry an erase, 10 ms otherwise.  Measured on board #2: a 128 KB erase takes **888 ms**, a 1 KB write 2.685 ms |

## Building

```
cmake -B build/wio-lite-ai -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-toolchain.cmake \
      -DBOARD=wio-lite-ai
cmake --build build/wio-lite-ai --target boot
# -> build/wio-lite-ai/boot-reference/boot.{elf,bin,hex,map}
```

`boot` is part of the default build, so an ordinary `cmake --build
build/wio-lite-ai` builds and checks it too.  The artefacts go into
`boot-reference/` rather than next to `shell.bin` so that a tab-completed
`dfu-util -D ...` in the build directory cannot reach them.

### The safety gate

`../cmake/check_boot_safety.py` runs twice per build and its checks are described
in full at the top of that file.  In short:

| | |
|---|---|
| C1 | vectors at `0x08000000`; the initial MSP is 8-byte aligned and inside an internal RAM; the reset vector is inside sector 0, Thumb-tagged, and is `Reset_Handler` |
| C2 | the image fits sector 0 and no LOAD segment leaves the 128 KB window |
| C3 | no option-byte / RDP / DBGMCU / low-power symbols or constants; the flash-driver symbols are an allowlist, not a denylist |
| C4 | the required flash-writing edges exist, and `HAL_FLASHEx_Erase` / `HAL_FLASH_Program` / `HAL_FLASH_Unlock` are reached from `iflash_erase_sector` and `iflash_program` and nowhere else.  Their addresses must also not be materialised indirectly -- neither as a data word nor as a `movw`/`movt` pair, which is how a caller would reach one without leaving an edge to find |
| C5 | the DFU class is actually in the image |
| LTO | no object boot_image links carries LTO intermediate representation.  Checked on the objects themselves (`.gnu.lto_*` sections), not only by scanning the compile commands for `-flto`: a spec file, a compiler launcher, or an object rebuilt outside Ninja all leave a clean command line.  This matters because cross-translation-unit inlining of the HAL flash writers into `iflash.c` would delete exactly the edges C4 checks |
| C6a | the seven sources and the ROM linker script match `boot_manifest.sha256`, and no file has been added to or removed from this directory |
| C6b | the linked image matches the golden hash |

`check_boot_safety.py` replaces the four objdump commands the donor's README
asked a human to run before every flash.  Two of those four had to be dropped
rather than automated:

- the constant grep looked for `52002008|52002018|...`, the absolute addresses of
  `OPTKEYR`/`OPTCR`.  Those literals never appear: the HAL writes through
  `WRITE_REG(FLASH->OPTKEYR, ...)`, i.e. base + offset, so the only literal in
  the image is the bank base `0x52002000` -- which occurs seven times
  legitimately.  **The grep passed because it could not fail.**  C3 scans for the
  option-byte unlock keys `0x08192A3B` / `0x4C5D6E7F` instead, which the hardware
  requires and which therefore cannot be avoided.
- the instruction-pattern check (`cmp.w r2, #512` and the unsigned sector-range
  compare) is not reimplemented.  Small-immediate instruction patterns are the
  most codegen-fragile thing there is; the first GCC update to break one would
  teach everybody to ignore a red gate.  What it was really trying to assert --
  that `iflash_erase_sector`'s range check is still in the source -- is a property
  of the SOURCE, and the honest mechanisation of that is C6a's hash.

**Known gap, recorded rather than papered over:** nothing checks that the
bootloader does not reconfigure the SWD pins (PA13/PA14).  `HAL_GPIO_Init()`
takes a struct built at run time, so no static check can answer it, and a pattern
match would fail open at the first codegen change.  A gate that passes silently
is worse than no gate, so this one is left to source immutability (C6a) and the
golden hash (C6b).  For the record, the GPIOs this bootloader touches are
PA11/PA12 (USB), PC13 (red LED) and PF1 (USER button).

To exercise the gate's own failure paths:

```
# from the repository root, after a build
python3 boards/wio-lite-ai/cmake/fixtures/run_fixture_tests.py \
        --build-dir build/wio-lite-ai --board-dir boards/wio-lite-ai
```

Each fixture is a real bootloader image that satisfies every check except the one
it is built to break, and the harness confirms each one's shape with `nm` /
`objdump` before running the gate on it -- checking a fixture with the gate's own
verdict would be a circular test.  Assertions are on the exit code *and* the
diagnostic ID, so a check that starts failing for a different reason than
intended is itself a test failure.

## Flashing the bootloader (ST-Link mode=UR) -- this is the brick, be careful

[!] **There is deliberately no build target for this.**  Writing sector 0 is the
one operation that can brick the board, and exactly one board is left; a build
system that CAN brick eventually will.  The procedure stays prose on purpose, and
running it is a decision, not a command.

[!] **Use the known-good Discovery ST-Link (below) with `mode=UR`.**

The read-only preparation is written out so it can be run as-is.  The write is
not, and that is deliberate: a copy-pasteable block that ends in a sector-0 write
is itself a path to the irreversible operation, however many warnings surround
it.  **This repository's reference build is deliberately not bound to any of
this** -- `boot-reference/boot.bin` has never run on hardware, so it is not the
image to reach for in a recovery.

```bash
# Wherever STM32CubeProgrammer is installed on this machine.
CLI=${STM32_PROGRAMMER_CLI:-/opt/STM32CubeProgrammer/bin/STM32_Programmer_CLI}

# 1. Confirm the good probe (expect SN 51FF.., FW V2J46S0)
"$CLI" --list

# 2. Safety gate: UR connect + device ID 0x483 + RDP 0xAA (read-only)
"$CLI" -c port=SWD mode=UR reset=HWrst -ob displ

# 3. Back up what is there (doubles as a connection-stability test)
"$CLI" -c port=SWD mode=UR reset=HWrst -r 0x08000000 0x80000 backup.bin

# 5. AFTER the write (see below), re-check UR immediately -- did it survive?
"$CLI" -c port=SWD mode=UR reset=HWrst -ob displ    # still RDP 0xAA = OK
```

Step 4 -- the irreversible one -- takes the shape below.  The placeholder is not
a formality: the image has to be one somebody has decided to program, named
explicitly, and steps 1-3 have to have passed first.

```text
<programmer> -c port=SWD mode=UR reset=HWrst -w <IMAGE-YOU-HAVE-DECIDED-TO-FLASH> 0x08000000 -v
```

### Why there is no `dfu-boot` target either

`dfu-<target>` exists for the app firmwares, and it would have been one line to
add one here.  It would flash the **bootloader into the app partition**.
Measured against this tree, that would: be kept out of sector 0 by `iflash.c`'s
range check; erase **sector 1 only** (the erase in `dfu_callbacks.c` runs when
`offset % IFLASH_SECTOR_SIZE (0x20000) == 0`, and a 30,524 B transfer only ever
reaches offset 0, so sectors 2 and 3 are untouched); write the boot payload minus
its first 32 B there; and then have the commit **refused**, because a bootloader
reset vector is outside the app window.  Net result: the app stops booting, DFU
still recovers it, and one erase cycle of a ~10k budget is spent.  Cheap to write
down, expensive to discover on the bench.

## Flashing an app over DFU

1. **Hold PF1 (USER) and reset** -- the bootloader stays in DFU mode (red LED on,
   `dfu-util -l` shows `name="Wio Lite AI app @0x08020000"`).
   - To reset over SWD instead, hold PF1 and run
     `"$CLI" -c port=SWD mode=UR --start`.
2. `dfu-util -d 0483:df11 -a 0 -D <app>.bin`
3. After the manifest phase it reboots automatically into the new app.
4. Optional readback: `dfu-util -d 0483:df11 -a 0 -U readback.bin`.

The VID/PID is **0483:DF11**, ST's standard DFU ID -- appropriate here, since
this is an STM32, and both dfu-util and CubeProgrammer recognise it.  The
descriptor declares manifestation-**intolerant**, because the device reboots
itself.

## Safety, bricking and recovery

- **Board #1 is permanently bricked** (RDP2, debug permanently disabled).  A bad
  bootloader was written to `0x08000000`; it cannot be recovered.
- **Board #2 is the development target** (boot @ 0x08000000 sector 0, app @
  0x08020000 sectors 1-3).  It is the only one left.
- The rules that keep it alive: an internal bootloader must **not change PA13/14
  (SWD), must not touch DBGMCU, must not enter a low-power mode, and must never
  go near the option bytes or RDP.**  `check_boot_safety.py` C3 checks all of
  those it can check statically; see the recorded gap above for the one it
  cannot.
- The only AIRCR write is `NVIC_SystemReset`'s `0x05FA0004`.  Since donor issue
  `owhinata/wio-lite-ai#25` the bootloader does erase and program internal flash,
  but through the single path in `iflash.c`, and sector 0 is structurally out of
  reach.  `HAL_FLASH_OB_Unlock` (`FLASH_OPTKEYR`) and `OPTCR` are never used.
- **Restoring TinyUF2**: the donor kept a 512 KB backup of the factory contents
  (`wio_flash_backup_20260710_225512.bin`, md5 `615ac2df..`); writing it to
  `0x08000000` restores the original state.  That file is donor-local reference
  material and is **not part of this repository**.

## Debuggers

- **Known-good Discovery ST-Link** (SN `51FF72064987505349271187`, FW `V2J46S0`):
  CubeProgrammer `mode=UR` (hard connect-under-reset) is proven to work with it.
  **Use this one for flashing and recovery.**
- V2 clone (`0483:3748`, FW `V2J17S4`): ordinary SWD works, but **UR does not**.
  Do not use it to flash.
- **SWD limitation**: reconfiguring the H7 PLLs drops the SWD connection, so a
  debugger cannot follow the app through it.  In-app debugging therefore relies
  on **printf over USB CDC** (the DFU-mode banner and `[tick]`).

## Known couplings in the frozen sources

These are donor-era defects.  They are **not fixed**, because fixing them would
change the bytes and forfeit the only property this tree has -- reproducibility.
They are written down so that the next person meets them here rather than in a
build failure:

- `main.c`'s `_sbrk` has no upper bound.  (The app-side equivalent WAS fixed, in
  this repository.)  It is harmless here: `end` is `0x24000c00` against a RAM top
  of `0x24050000` (~317 KB of slack), `.bss` is 4,424 B, and `main.c` sets
  `_IONBF`, so stdout takes no buffer either.
- `usb_descriptors.c` uses `memcpy`/`strlen` without including `<string.h>`,
  relying on `tusb.h` pulling it in transitively.  Under GCC 15's gnu23 default
  an implicit declaration is a **hard error**, so if TinyUSB ever drops that
  transitive include the build breaks.  When that happens, the answer is a
  reviewed exception or a TinyUSB pin -- not a quietly added `#include`.

---

# Hardware reference

Register values measured just before TinyUF2 jumped to the app.  The standalone
init reproduces them (`clock.c`; `octospi.c` before donor issue
`owhinata/wio-lite-ai#25`).  All values are hex.

## Measured dump (from the app-first firmware's CDC output)

```
--- RCC ---
CR=3F03C025 CFGR=0000001B PLLCKSELR=00519022 PLLCFGR=01FF093D
PLL1DIVR=0104002B PLL2DIVR=00010309 PLL3DIVR=0104022F
D1CFGR=00000048 D2CFGR=00000440 D3CFGR=00000040
D1CCIPR=00000020 D2CCIP2R=00200000
--- PWR --- CR1=F000C000 CR3=05010044 D3CR=00002000   --- FLASH ACR=00000033
--- OCTOSPI2 --- CR=30400381 DCR1=00170008 DCR2=00000002
  CCR=03032301 TCR=00000004 IR=000000EB ABR=00000000
--- OCTOSPIM --- CR=0 P1CR=03010111 P2CR=07050333   (= reset values)
--- GPIO MODER/OTYPER/OSPEEDR/PUPDR/AFRL/AFRH ---
PF FFEAAEF3 00000000 003FF300 00000004 AA090000 000009AA
PG FEAFEFFA 00000000 03F0300F 00000000 0A000099 00039300
```

## Clock tree (25 MHz HSE crystal -> sysclk = PLL1)

- Supply and voltage: direct SMPS (`PWR_DIRECT_SMPS_SUPPLY`) + VOS0.  FLASH
  ACR=0x33 (latency 3 + WRHIGHFREQ, for 550 MHz; both agree with the reset
  values).
- PLLCKSELR=00519022: PLLSRC=HSE, DIVM1=2, DIVM2=25, DIVM3=5.
- **PLL1**: M2 (12.5 MHz) N44 -> VCO 550; P/1 -> **sysclk 550 (CPU)**; Q110; R275.
- **PLL2**: M25 (1 MHz) N266; R/1 -> **266 -> OCTOSPI2 kernel** (DCR2 divides by
  3, ~88.7 MHz).
- **PLL3**: M5 (5 MHz) N48 -> VCO 240; Q/5 -> **48 MHz -> USB**.
- D1CFGR=0x48: HPRE=/2 (AXI/AHB 275), D1CPRE=/1, D1PPRE=/2.
- D1CCIPR=0x20: OCTOSPISEL -> PLL2R.  D2CCIP2R=0x200000: USBSEL -> PLL3Q.

## OCTOSPI2 memory-map configuration (W25Q128 Quad I/O)

- DCR1=00170008 (DEVSIZE 16 MB), DCR2=2 (prescaler /3).
- Read: IR=0xEB (Fast Read Quad I/O), CCR=03032301 (instruction 1-line / address
  4-line 24-bit / mode byte 4-line ABR=0 / data 4-line, SIOO=0), TCR=4 dummy.
- CR=30400381 (EN, FMODE=11 memory-mapped, FSEL, FTHRES=3, APMS).
- Check or set the W25Q's Quad-Enable (QE, SR2 bit 1) before a quad read.

## OCTOSPI2 flash pins (schematic sheet 6, confirmed 2026-07-12)

The **W25Q128 application flash is the "QSPI2_\*" net = OCTOSPIM port 2**:

| Signal | Pin | AF |
|--------|-----|----|
| CLK    | PF4  | AF9 |
| NCS    | PG12 | AF3 |
| IO4    | PG0  | AF9 |
| IO5    | PG1  | AF9 |
| IO6    | PG10 | AF3 |
| IO7    | PG11 | AF9 |

(The quad data sits on port 2's upper nibble IO[7:4], which is what the P2CR
reset value 0x07050333 selects.  It matches the measured AFR: PF4=9, PG0/PG1=9,
PG10=3, PG11=9, PG12=3.  CS has a 10K pull-up, R90.)

The **"OSPI1_\*" nets are the PSRAM (OCTOSPI1 / port 1) = PF6-PF10, PG6 and so
on**.  The net names do not follow the peripheral numbers -- older notes had the
two the wrong way round.  **The code restores GPIO banks F and G wholesale**, so
both pin groups are covered and the mix-up had no effect; the table above is what
to use if the per-pin setup is ever narrowed.

## OCTOSPIM routing

A re-dump gives **CR=0 (MUXEN=0), P1CR=0x03010111, P2CR=0x07050333, i.e. the
reset values** (RM0468 sec 26.5.2) -- the default routing (OCTOSPI1 -> port 1,
OCTOSPI2 -> port 2, no muxing).  **OCTOSPIM is not touched.**

## Clocks enabled (measured)

- AHB4ENR=0xFF -> GPIOA..GPIOH.  AHB1ENR bit 25 -> USB1_OTG_HS.
- AHB3ENR: OSPI1EN(14) + OSPI2EN(19) + IOMNGREN(21), plus PWR/SYSCFG.

---

# Development notes (non-obvious; do not lose these)

## USB on the H725

- There is a single USB peripheral, **USB1_OTG_HS**, run at full speed on the
  internal PHY.  CMSIS has no `USB2_OTG_FS` and no `OTG_FS_IRQn`.  When
  `USB2_OTG_FS` is undefined, TinyUSB's dwc2 port aliases rhport0 to the **OTG_HS
  base and OTG_HS_IRQn** -- so the interrupt is `OTG_HS_IRQHandler` calling
  `tud_int_handler(0)`.  GPIO is PA11/PA12 with `GPIO_AF10_OTG1_FS`.
- **Forcing full speed is the crux**: the OTG_HS core advertises an HS PHY in
  GHWCFG2, but this board only has the internal FS one.
  `CFG_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED` selects the `phy_fs_init()` path.
- The USB clock is PLL3Q at 48 MHz (`clock.c`).  VBUS detection is
  `HAL_PWREx_EnableUSBVoltageDetector()` alone -- USBREGEN is not needed, since
  VDD33USB is supplied externally.
- If a future configuration cannot inherit the clock tree, HSI48 + CRS
  (`stm32h7xx_ll_crs.c` is present, there is no HAL CRS) makes USB independent of
  the PLLs.  Not needed today.

## OCTOSPI2 programming (before donor issue `owhinata/wio-lite-ai#25`)

- Executing from internal flash means no RAM-resident code is needed: abort the
  memory map, issue W25Q commands indirectly (WREN 06 / SE 20 / PP 02 /
  RDSR 05,35 / WRSR2 31 / JEDEC 9F, 24-bit addresses), then restore the map.
  Caches must be off, for register/memory-map coherency.
- The DFU download callback programmed synchronously (blocking inside the
  callback), erasing and programming at every 4 KB boundary.  A
  `get_timeout` DNBUSY of 60 ms was what rate-limited the host's polling.

## Automatic reboot after the manifest phase

- `tud_dfu_manifest_cb` -> `boot_request_reboot()`, and the main loop calls
  `NVIC_SystemReset` **1500 ms later**.
- The descriptor declares manifestation-**intolerant**, because the device resets
  itself.
- **The delay has to exceed 1000 ms.**  dfu-util sees dfuMANIFEST, sleeps about a
  second, and re-reads GET_STATUS.  Rebooting after 300 ms made that re-read hit a
  device that had already gone, which surfaced as a `LIBUSB_ERROR_NO_DEVICE`
  warning.  Waiting 1500 ms lets the re-read see dfuMANIFEST-WAIT-RESET properly
  and the warning disappears.

## Reference code

The first two entries are donor-local reference material and are **not part of
this repository** -- nothing in the build reads them:

- TinyUF2: `ports/stm32h7/boards.c` (the model for the jump and the validity
  test).
- Internal-flash erase/program: STM32Cube_FW_H7's
  `Projects/NUCLEO-H723ZG/Examples/FLASH/FLASH_EraseProgram/` -- ST's own example
  for the same single-bank part, in which a flash-resident HAL erases a sector of
  the bank it is running from.

In this repository:

- The OCTOSPI2 implementation was deleted by donor issue
  `owhinata/wio-lite-ai#25`.  To read it again:
  `git show <pre-#25 commit>:boot/octospi.c` in `owhinata/wio-lite-ai`.
- TinyUSB's DFU class: `lib/tinyusb/src/class/dfu/dfu_device.{c,h}` (submodule,
  0.21.0).
