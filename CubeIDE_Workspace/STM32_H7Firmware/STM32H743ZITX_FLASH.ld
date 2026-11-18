/*
 ******************************************************************************
 * @file    STM32H743ZITX_FLASH.ld
 * @brief   Linker script for STM32H743ZITx (2048 KB Flash, 1056 KB RAM total).
 *
 * ── STM32H743ZI physical SRAM map ──────────────────────────────────────────
 *
 *  Region    Base         Size    DMA-capable  Used for
 *  ──────────────────────────────────────────────────────────────────────────
 *  DTCMRAM   0x20000000  128 KB  NO  (CPU)    g_activations (TinyML), heap, main stack
 *  RAM_D1    0x24000000  512 KB  YES (DMA2)   .data, .bss, frameBuffer,
 *                                             cvBinBuffer
 *  RAM_D2    0x30000000  288 KB  YES           cvTmpBuffer, cvBgBuffer
 *  RAM_D3    0x38000000   64 KB  YES (BDMA)   not used
 *  ITCMRAM   0x00000000   64 KB  NO           not used
 *
 * ── RAM_D1 budget (480×272 capture) ────────────────────────────────────────
 *
 *  .data + .bss             ~28 KB
 *  frameBuffer   480×272×2 = 261 120 B  (255.0 KB)  .dcmi_buf  DMA target
 *  cvBinBuffer   480×272   = 130 560 B  (127.5 KB)  .dcmi_buf  CV work_bin
 *  ─────────────────────────────────────────────────────────────────────────
 *  DMA buffers total        391 680 B  (382.5 KB) + ~28 KB data ≈ 410 KB ✅
 *
 * ── RAM_D2 budget (480×272) ────────────────────────────────────────────────
 *
 *  cvTmpBuffer   480×272   = 130 560 B  (127.5 KB)  .cvtmb_buf  CV tmp_buf
 *  cvBgBuffer    480×272   = 130 560 B  (127.5 KB)  .cvbg_buf   bgsub ref
 *  ─────────────────────────────────────────────────────────────────────────
 *  Total                    261 120 B  (255.0 KB)  of 288 KB   ✅ (33 KB free)
 *
 *  Both buffers are CPU-only (no DMA on RAM_D2 required by this application).
 *  MPU Region 2 marks RAM_D2 (0x30000000, 256 KB) as Non-Cacheable.
 *  The two buffers (255 KB) have to stay inside these 256 KB.
 *
 * ── DTCM budget ─────────────────────────────────────────────────────────────
 *
 *  g_activations  AI_NETWORK_DATA_ACTIVATIONS_SIZE  (41 152 B, X-CUBE-AI report)  .tinyml_buf
 *  heap + main stack reserve                          4 096 B
 *  ─────────────────────────────────────────────────────────────────────────
 *  Total                   45 248 B  (~44.2 KB)  of 128 KB   ✅  (the main stack
 *  grows down from the top of DTCM)
 *
 *  The 57 876 B in the X-CUBE-AI report are the total RAM of the network
 *  including its runtime (activations 41 152 B + runtime 16 724 B); only the
 *  activations are placed in DTCM.
 *
 *  X-CUBE-AI only requires CPU access. DTCM is not cached, so no MPU region
 *  and no cache maintenance are needed. The same buffer in RAM_D3 (a
 *  Non-Cacheable MPU region) made an inference 3x slower (157 ms instead of
 *  53 ms, measured on the board).
 *  g_activations is memset to 0 by tinyml_init() before every
 *  ai_network_create_and_init() call because NOLOAD sections are not zeroed
 *  by startup code.
 *
 * ── MPU (main.c MPU_Config) ─────────────────────────────────────────────────
 *  Region 0: 0x00000000 / 4 GB     — No-access background region
 *  Region 1: RAM_D1 0x24000000 / 512 KB — Non-Cacheable (TEX=1 C=0 B=0)
 *  Region 2: RAM_D2 0x30000000 / 256 KB — Non-Cacheable (TEX=1 C=0 B=0)
 *  Region 3: RAM_D3 0x38000000 /  64 KB — Non-Cacheable (TEX=1 C=0 B=0)
 *
 * ── Why cvBinBuffer and cvTmpBuffer cannot be merged or reduced ─────────────
 *
 *  The CV pipeline (cv_run_rgb565) uses both buffers simultaneously in every
 *  stage and writes proc_w * proc_h bytes into each:
 *    Stage 1: grayscale  → tmp_buf[y*proc_w + x]
 *    Stage 3: filter     → work_bin[] from tmp_buf[]
 *    Stage 4: threshold  → tmp_buf[0..n-1]
 *    Stage 5: morphology → alternates between work_bin and tmp_buf as src/dst
 *  Both must be allocated for proc_w_MAX * proc_h_MAX = 480 * 272 = 130 560 B.
 *
 ******************************************************************************
 */

ENTRY(Reset_Handler)

_estack         = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);
_Min_Heap_Size = 0x800 ;   /* 2 KB minimum heap  */
_Min_Stack_Size = 0x800 ;   /* 2 KB minimum stack */

MEMORY
{
  FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
  DTCMRAM  (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D1   (xrw) : ORIGIN = 0x24000000, LENGTH = 512K
  RAM_D2   (xrw) : ORIGIN = 0x30000000, LENGTH = 288K
  RAM_D3   (xrw) : ORIGIN = 0x38000000, LENGTH = 64K
  ITCMRAM  (xrw) : ORIGIN = 0x00000000, LENGTH = 64K
}

SECTIONS
{
  /* ── Flash: interrupt vector table ──────────────────────────────────────── */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* ── Flash: code ─────────────────────────────────────────────────────────── */
  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  /* ── Flash: read-only data ───────────────────────────────────────────────── */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab (READONLY) :
  {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >FLASH

  .ARM (READONLY) :
  {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >FLASH

  .preinit_array (READONLY) :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .init_array (READONLY) :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .fini_array (READONLY) :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
    . = ALIGN(4);
  } >FLASH

  /* ── RAM_D1: initialised variables (.data) ──────────────────────────────────
   * LMA in Flash, VMA in RAM_D1.
   * Startup code copies _sidata → _sdata.._edata at boot. */
  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } >RAM_D1 AT> FLASH

  /* ── RAM_D1: zero-initialised variables (.bss) ──────────────────────────── */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM_D1

  /* ── DTCMRAM (0x20000000, 128 KB) — TinyML activation buffer ───────────────
   *
   * The Cortex-M7 reaches DTCM in one cycle without the D-cache, and the
   * activations are only used by the CPU (X-CUBE-AI does not use DMA).
   * In RAM_D3 (non-cacheable by the MPU) every access goes through the bus.
   * NOLOAD: not zeroed by startup; tinyml_init() clears g_activations before
   * every ai_network_create_and_init(). It is placed before the heap/stack
   * reserve so that the newlib heap starts behind it.
   */
  .tinyml_buf (NOLOAD) :
  {
    . = ALIGN(32);
    __tinyml_buf_start__ = .;
    KEEP(*(.tinyml_buf))
    KEEP(*(.tinyml_buf*))
    . = ALIGN(32);
    __tinyml_buf_end__ = .;
  } >DTCMRAM

  /* ── DTCMRAM: heap + main stack reserve ─────────────────────────────────── */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >DTCMRAM

  /* ── RAM_D1 (AXI SRAM, 0x24000000, 512 KB) — DMA image buffers ─────────────
   *
   * NOLOAD: not zeroed by startup; buffers are always written before first use.
   * Non-Cacheable via MPU Region 1 (TEX=1 C=0 B=0).
   * DMA2 (DCMI) writes frameBuffer directly; no CPU cache maintenance needed.
   *
   * Contents:
   *   frameBuffer  480×272×2 = 261 120 B  (255.0 KB)  DCMI DMA write target
   *   cvBinBuffer  480×272   = 130 560 B  (127.5 KB)  CV work_bin (gray/binary)
   *   Total                   391 680 B  (382.5 KB)  of 512 KB  ✅
   */
  .dcmi_buf (NOLOAD) :
  {
    . = ALIGN(32);
    __dcmi_buf_start__ = .;
    KEEP(*(.dcmi_buf))
    KEEP(*(.dcmi_buf*))
    . = ALIGN(32);
    __dcmi_buf_end__ = .;
  } >RAM_D1

  /* ── RAM_D2 (AHB SRAM, 0x30000000, 288 KB) — CV scratch buffers ─────────────
   *
   * NOLOAD: not zeroed by startup; written before first use.
   * Non-Cacheable via MPU Region 2 (TEX=1 C=0 B=0).
   * No DMA access required for either buffer (CPU-only CV pipeline).
   *
   * .cvtmb_buf — CV pipeline temporary scratch (filter, threshold, morphology)
   *   cvTmpBuffer  480×272 = 130 560 B  (127.5 KB)
   *
   * .cvbg_buf  — background reference frame (cv_capture_background / CV BGCAP)
   *   cvBgBuffer   480×272 = 130 560 B  (127.5 KB)
   *
   * RAM_D2 total: 261 120 B (255 KB) of 288 KB  ✅  (33 KB free)
   */
  .cvtmb_buf (NOLOAD) :
  {
    . = ALIGN(32);
    __cvtmb_buf_start__ = .;
    KEEP(*(.cvtmb_buf))
    KEEP(*(.cvtmb_buf*))
    . = ALIGN(32);
    __cvtmb_buf_end__ = .;
  } >RAM_D2

  .cvbg_buf (NOLOAD) :
  {
    . = ALIGN(32);
    __cvbg_buf_start__ = .;
    KEEP(*(.cvbg_buf))
    KEEP(*(.cvbg_buf*))
    . = ALIGN(32);
    __cvbg_buf_end__ = .;
  } >RAM_D2

  /DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
  .ARM.attributes 0 : { *(.ARM.attributes) }
}
