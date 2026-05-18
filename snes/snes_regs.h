/*
 * snes_regs.h — SNES hardware register address constants.
 *
 * Every named constant maps to a memory-mapped I/O address that the main CPU
 * uses to communicate with PPU, APU, DMA, and internal control logic. These
 * addresses appear on the A-bus when the CPU reads or writes the corresponding
 * hardware function. The names follow the official SNES developer documentation.
 *
 * Register ranges:
 *   0x2100-0x2133  PPU write registers (display control, BG/OBJ config, scroll,
 *                  VRAM/CGRAM/OAM access, windowing, color math, mode 7)
 *   0x2134-0x213F  PPU read registers (multiplication result, latches, status)
 *   0x2140-0x2143  APU communication ports (main CPU ↔ SPC700)
 *   0x2180-0x2183  WRAM data/address ports (sequential RAM access)
 *   0x4016-0x4017  Joypad serial data lines
 *   0x4200-0x420D  CPU control registers (NMI/IRQ enable, timers, DMA trigger,
 *                  multiplication/division hardware, memory speed)
 *   0x4210-0x421F  CPU status/read registers (NMI flag, IRQ flag, H/V blank
 *                  status, math results, auto-joypad read data)
 *   0x4300-0x437F  DMA channel registers (8 channels × 16 bytes each)
 */
#pragma once

/* ── PPU write registers (0x2100–0x2133) ──────────────────────────────────── */
#define INIDISP 0x2100
// Screen brightness and forced-blank control are combined in INIDISP
#define OBSEL 0x2101    // Object (sprite) size and tile base address select
#define OAMADDL 0x2102  // OAM address low byte
#define OAMADDH 0x2103  // OAM address high bit and priority rotation
#define OAMDATA 0x2104  // OAM data write port (auto-increments address)
#define BGMODE 0x2105   // BG mode (0-7) and tile size bits for each layer
#define MOSAIC 0x2106   // Mosaic pixel size and which BG layers use mosaic
// BG tilemap base address and size (wider/higher bits) for each layer
#define BG1SC 0x2107
#define BG2SC 0x2108
#define BG3SC 0x2109
#define BG4SC 0x210a
// BG character (tile data) base address — two layers share each register
#define BG12NBA 0x210b
#define BG34NBA 0x210c
// BG scroll registers — horizontal and vertical offset for each layer
// Written twice (low then high byte) using the scroll latch mechanism
#define BG1HOFS 0x210d
#define BG1VOFS 0x210e
#define BG2HOFS 0x210f
#define BG2VOFS 0x2110
#define BG3HOFS 0x2111
#define BG3VOFS 0x2112
#define BG4HOFS 0x2113
#define BG4VOFS 0x2114
// VRAM access — address increment mode, address, and data write ports
#define VMAIN 0x2115    // VRAM address increment mode (low/high byte trigger, step size)
#define VMADDL 0x2116   // VRAM address low byte
#define VMADDH 0x2117   // VRAM address high byte
#define VMDATAL 0x2118  // VRAM data write low byte
#define VMDATAH 0x2119  // VRAM data write high byte
// Mode 7 rotation/scaling matrix and center coordinates
#define M7SEL 0x211a   // Mode 7 settings (screen flip, out-of-range fill mode)
#define M7A 0x211b     // Mode 7 matrix parameter A (cosine, horizontal scale)
#define M7B 0x211c     // Mode 7 matrix parameter B (sine)
#define M7C 0x211d     // Mode 7 matrix parameter C (sine)
#define M7D 0x211e     // Mode 7 matrix parameter D (cosine, vertical scale)
#define M7X 0x211f     // Mode 7 center X coordinate
#define M7Y 0x2120     // Mode 7 center Y coordinate
// CGRAM (color palette) access
#define CGADD 0x2121    // CGRAM address (palette index, 0-255)
#define CGDATA 0x2122   // CGRAM data write port (15-bit BGR, two writes per color)
// Window mask settings — which layers use which windows, inversion bits
#define W12SEL 0x2123   // Window mask for BG1 and BG2
#define W34SEL 0x2124   // Window mask for BG3 and BG4
#define WOBJSEL 0x2125  // Window mask for OBJ and color math
// Window position registers (left/right edges for windows 1 and 2)
#define WH0 0x2126      // Window 1 left edge
#define WH1 0x2127      // Window 1 right edge
#define WH2 0x2128      // Window 2 left edge
#define WH3 0x2129      // Window 2 right edge
// Window logic operators (AND/OR/XOR/XNOR between windows 1 and 2)
#define WBGLOG 0x212a   // Window logic for BG layers
#define WOBJLOG 0x212b  // Window logic for OBJ and color math
// Main/sub screen designation and window masking for screen layers
#define TM 0x212c       // Main screen layer enable (bits 0-4: BG1-4, OBJ)
#define TS 0x212d       // Sub screen layer enable
#define TMW 0x212e      // Main screen window mask enable
#define TSW 0x212f      // Sub screen window mask enable
// Color math control — how main and sub screens are combined
#define CGWSEL 0x2130   // Color math clip mode and prevent mode
#define CGADSUB 0x2131  // Color math add/subtract enable per layer, half-color
#define COLDATA 0x2132  // Fixed color for color math (R/G/B written separately)
#define SETINI 0x2133   // Screen mode settings (interlace, overscan, hi-res)
/* ── PPU read registers (0x2134–0x213F) ───────────────────────────────────── */
// Mode 7 multiplication result (M7A × M7B, 24-bit signed)
#define MPYL 0x2134     // Multiplication result low byte
#define MPYM 0x2135     // Multiplication result mid byte
#define MPYH 0x2136     // Multiplication result high byte
#define SLHV 0x2137     // Latch H/V counter (read to capture current beam position)
#define RDOAM 0x2138    // OAM data read port
#define RDVRAML 0x2139  // VRAM data read low byte
#define RDVRAMH 0x213a  // VRAM data read high byte
#define RDCGRAM 0x213b  // CGRAM data read port
#define OPHCT 0x213c    // Latched horizontal beam position counter
#define OPVCT 0x213d    // Latched vertical beam position counter
#define STAT77 0x213e   // PPU1 status (time-over, range-over, PPU1 version)
#define STAT78 0x213f   // PPU2 status (interlace field, external latch, PPU2 version)
/* ── APU communication ports (0x2140–0x2143) ──────────────────────────────── */
// Four bidirectional I/O ports between the main 65C816 CPU and the SPC700
#define APUI00 0x2140
#define APUI01 0x2141
#define APUI02 0x2142
#define APUI03 0x2143
/* ── WRAM access ports (0x2180–0x2183) ────────────────────────────────────── */
// Sequential access to the 128KB work RAM through a single data port
#define WMDATA 0x2180   // WRAM data read/write (auto-increments address)
#define WMADDL 0x2181   // WRAM address low byte
#define WMADDM 0x2182   // WRAM address mid byte
#define WMADDH 0x2183   // WRAM address high bit (bit 0 selects upper/lower 64KB)
/* ── Joypad serial data (0x4016–0x4017) ───────────────────────────────────── */
#define JOYA 0x4016     // Joypad port 1 serial data / latch line
#define JOYB 0x4017     // Joypad port 2 serial data
/* ── CPU control / write registers (0x4200–0x420D) ────────────────────────── */
#define NMITIMEN 0x4200 // NMI/IRQ enable and auto-joypad read enable
#define WRIO 0x4201     // Programmable I/O port (active-low PPU latch on bit 7)
// Hardware multiplication (8×8 → 16-bit result, available in RDMPYL/H)
#define WRMPYA 0x4202   // Multiplicand A (8-bit)
#define WRMPYB 0x4203   // Multiplicand B (8-bit, writing triggers multiply)
// Hardware division (16÷8 → quotient in RDDIVL/H, remainder in RDMPYL/H)
#define WRDIVL 0x4204   // Dividend low byte
#define WRDIVH 0x4205   // Dividend high byte
#define WRDIVB 0x4206   // Divisor (writing triggers divide)
// H/V IRQ timer target values (9-bit each)
#define HTIMEL 0x4207   // H-count timer low byte
#define HTIMEH 0x4208   // H-count timer high bit
#define VTIMEL 0x4209   // V-count timer low byte
#define VTIMEH 0x420a   // V-count timer high bit
// DMA trigger registers
#define MDMAEN 0x420b   // General-purpose DMA channel enable (bit N = channel N)
#define HDMAEN 0x420c   // HDMA channel enable (bit N = channel N)
#define MEMSEL 0x420d   // ROM access speed (bit 0: 0=slow 2.68MHz, 1=fast 3.58MHz)
/* ── CPU status / read registers (0x4210–0x421F) ─────────────────────────── */
#define RDNMI 0x4210    // NMI flag (bit 7, cleared on read) and CPU version
#define TIMEUP 0x4211   // IRQ flag (bit 7, cleared on read)
#define HVBJOY 0x4212   // H/V blank status and auto-joypad busy flag
#define RDIO 0x4213     // Programmable I/O port read
#define RDDIVL 0x4214   // Division quotient low byte
#define RDDIVH 0x4215   // Division quotient high byte
#define RDMPYL 0x4216   // Multiply result low byte (or division remainder low)
#define RDMPYH 0x4217   // Multiply result high byte (or division remainder high)
// Auto-joypad read results — 16-bit button state for each of 4 controllers
#define JOY1L 0x4218
#define JOY1H 0x4219
#define JOY2L 0x421a
#define JOY2H 0x421b
#define JOY3L 0x421c
#define JOY3H 0x421d
#define JOY4L 0x421e
#define JOY4H 0x421f

/* ── DMA channel registers (0x4300–0x437F) ────────────────────────────────── */
// 8 channels × 16 registers each. Each channel has:
//   DMAPn  (0x00): Transfer mode, direction, address step, HDMA indirect
//   BBADn  (0x01): B-bus destination address (PPU register low byte)
//   A1TnL/H (0x02-03): A-bus source address
//   A1Bn   (0x04): A-bus source bank
//   DASnL/H (0x05-06): Transfer size (DMA) or indirect address (HDMA)
//   DASn0  (0x07): HDMA indirect bank
//   A2AnL/H (0x08-09): HDMA table address (current row pointer)
//   NTRLn  (0x0A): HDMA line counter / repeat flag
//   UNUSEDn (0x0B/0F): Unused byte (readable/writable for open bus accuracy)
#define DMAP0 0x4300
#define BBAD0 0x4301
#define A1T0L 0x4302
#define A1T0H 0x4303
#define A1B0 0x4304
#define DAS0L 0x4305
#define DAS0H 0x4306
#define DAS00 0x4307
#define A2A0L 0x4308
#define A2A0H 0x4309
#define NTRL0 0x430a
#define UNUSED0 0x430b
#define MIRR0 0x430f
#define DMAP1 0x4310
#define BBAD1 0x4311
#define A1T1L 0x4312
#define A1T1H 0x4313
#define A1B1 0x4314
#define DAS1L 0x4315
#define DAS1H 0x4316
#define DAS10 0x4317
#define A2A1L 0x4318
#define A2A1H 0x4319
#define NTRL1 0x431a
#define UNUSED1 0x431b
#define MIRR1 0x431f
#define DMAP2 0x4320
#define BBAD2 0x4321
#define A1T2L 0x4322
#define A1T2H 0x4323
#define A1B2 0x4324
#define DAS2L 0x4325
#define DAS2H 0x4326
#define DAS20 0x4327
#define A2A2L 0x4328
#define A2A2H 0x4329
#define NTRL2 0x432a
#define UNUSED2 0x432b
#define MIRR2 0x432f
#define DMAP3 0x4330
#define BBAD3 0x4331
#define A1T3L 0x4332
#define A1T3H 0x4333
#define A1B3 0x4334
#define DAS3L 0x4335
#define DAS3H 0x4336
#define DAS30 0x4337
#define A2A3L 0x4338
#define A2A3H 0x4339
#define NTRL3 0x433a
#define UNUSED3 0x433b
#define MIRR3 0x433f
#define DMAP4 0x4340
#define BBAD4 0x4341
#define A1T4L 0x4342
#define A1T4H 0x4343
#define A1B4 0x4344
#define DAS4L 0x4345
#define DAS4H 0x4346
#define DAS40 0x4347
#define A2A4L 0x4348
#define A2A4H 0x4349
#define NTRL4 0x434a
#define UNUSED4 0x434b
#define MIRR4 0x434f
#define DMAP5 0x4350
#define BBAD5 0x4351
#define A1T5L 0x4352
#define A1T5H 0x4353
#define A1B5 0x4354
#define DAS5L 0x4355
#define DAS5H 0x4356
#define DAS50 0x4357
#define A2A5L 0x4358
#define A2A5H 0x4359
#define NTRL5 0x435a
#define UNUSED5 0x435b
#define MIRR5 0x435f
#define DMAP6 0x4360
#define BBAD6 0x4361
#define A1T6L 0x4362
#define A1T6H 0x4363
#define A1B6 0x4364
#define DAS6L 0x4365
#define DAS6H 0x4366
#define DAS60 0x4367
#define A2A6L 0x4368
#define A2A6H 0x4369
#define NTRL6 0x436a
#define UNUSED6 0x436b
#define MIRR6 0x436f
#define DMAP7 0x4370
#define BBAD7 0x4371
#define A1T7L 0x4372
#define A1T7H 0x4373
#define A1B7 0x4374
#define DAS7L 0x4375
#define DAS7H 0x4376
#define DAS70 0x4377
#define A2A7L 0x4378
#define A2A7H 0x4379
#define NTRL7 0x437a
#define UNUSED7 0x437b
#define MIRR7 0x437f
