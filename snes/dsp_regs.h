/*
 * dsp_regs.h — S-DSP (Sony Digital Signal Processor) register address map.
 *
 * The SNES DSP has 128 registers (0x00–0x7F) organized in a regular pattern:
 * 8 voice channels occupy addresses at 0x10-byte intervals (voice N at 0xN0),
 * with per-voice registers for volume, pitch, sample source, ADSR, gain, and
 * output readback. Global registers are interleaved at offsets 0x0C–0x0D and
 * 0x0F within each voice block, controlling master volume, echo, FIR filter
 * coefficients, key on/off, noise, pitch modulation, and the echo buffer.
 *
 * Per-voice registers (offsets from voice base 0xN0):
 *   +0 VOLL   Left volume (-128..+127)
 *   +1 VOLR   Right volume (-128..+127)
 *   +2 PITCHL Pitch (14-bit, low byte). Higher = higher frequency.
 *   +3 PITCHH Pitch high byte (bits 0-5)
 *   +4 SRCN   Source number — index into the sample directory (DIR page)
 *   +5 ADSR1  ADSR enable, decay rate, attack rate
 *   +6 ADSR2  Sustain level, sustain rate
 *   +7 GAIN   Gain mode/value (used when ADSR is disabled)
 *   +8 ENVX   Current envelope value (read-only, 7-bit)
 *   +9 OUTX   Current sample output after envelope (read-only, signed 8-bit)
 */
#ifndef DSP_REGS_H
#define DSP_REGS_H

enum DspReg {
  /* ── Voice 0 registers ── */
  V0VOLL = 0x00,
  V0VOLR = 0x01,
  V0PITCHL = 0x02,
  V0PITCHH = 0x03,
  V0SRCN = 0x04,
  V0ADSR1 = 0x05,
  V0ADSR2 = 0x06,
  V0GAIN = 0x07,
  V0ENVX = 0x08,
  V0OUTX = 0x09,
  /* ── Global registers interleaved in voice 0 block ── */
  MVOLL = 0x0C,   // Master volume left
  EFB = 0x0D,     // Echo feedback volume (how much echo feeds back into itself)
  FIR0 = 0x0F,    // FIR filter coefficient 0 (echo uses 8-tap FIR filter)
  /* ── Voice 1 registers ── */
  V1VOLL = 0x10,
  V1VOLR = 0x11,
  V1PL = 0x12,
  V1PH = 0x13,
  V1SRCN = 0x14,
  V1ADSR1 = 0x15,
  V1ADSR2 = 0x16,
  V1GAIN = 0x17,
  V1ENVX = 0x18,
  V1OUTX = 0x19,
  MVOLR = 0x1C,   // Master volume right
  FIR1 = 0x1F,    // FIR filter coefficient 1
  /* ── Voice 2 registers ── */
  V2VOLL = 0x20,
  V2VOLR = 0x21,
  V2PL = 0x22,
  V2PH = 0x23,
  V2SRCN = 0x24,
  V2ADSR1 = 0x25,
  V2ADSR2 = 0x26,
  V2GAIN = 0x27,
  V2ENVX = 0x28,
  V2OUTX = 0x29,
  EVOLL = 0x2C,   // Echo volume left
  PMON = 0x2D,    // Pitch modulation enable (bit N = voice N modulated by N-1)
  FIR2 = 0x2F,    // FIR filter coefficient 2
  /* ── Voice 3 registers ── */
  V3VOLL = 0x30,
  V3VOLR = 0x31,
  V3PL = 0x32,
  V3PH = 0x33,
  V3SRCN = 0x34,
  V3ADSR1 = 0x35,
  V3ADSR2 = 0x36,
  V3GAIN = 0x37,
  V3ENVX = 0x38,
  V3OUTX = 0x39,
  EVOLR = 0x3C,   // Echo volume right
  NON = 0x3D,     // Noise enable (bit N = voice N uses noise instead of sample)
  FIR3 = 0x3F,    // FIR filter coefficient 3
  /* ── Voice 4 registers ── */
  V4VOLL = 0x40,
  V4VOLR = 0x41,
  V4PL = 0x42,
  V4PH = 0x43,
  V4SRCN = 0x44,
  V4ADSR1 = 0x45,
  V4ADSR2 = 0x46,
  V4GAIN = 0x47,
  V4ENVX = 0x48,
  V4OUTX = 0x49,
  KON = 0x4C,     // Key-on: writing a 1 starts the voice from its sample start
  EON = 0x4D,     // Echo enable (bit N = voice N output feeds into echo buffer)
  FIR4 = 0x4F,    // FIR filter coefficient 4
  /* ── Voice 5 registers ── */
  V5VOLL = 0x50,
  V5VOLR = 0x51,
  V5PL = 0x52,
  V5PH = 0x53,
  V5SRCN = 0x54,
  V5ADSR1 = 0x55,
  V5ADSR2 = 0x56,
  V5GAIN = 0x57,
  V5ENVX = 0x58,
  V5OUTX = 0x59,
  KOF = 0x5C,     // Key-off: writing a 1 begins the release phase of the envelope
  DIR = 0x5D,     // Sample directory page (address >> 8 in APU RAM)
  FIR5 = 0x5F,    // FIR filter coefficient 5
  /* ── Voice 6 registers ── */
  V6VOLL = 0x60,
  V6VOLR = 0x61,
  V6PL = 0x62,
  V6PH = 0x63,
  V6SRCN = 0x64,
  V6ADSR1 = 0x65,
  V6ADSR2 = 0x66,
  V6GAIN = 0x67,
  V6ENVX = 0x68,
  V6OUTX = 0x69,
  FLG = 0x6C,     // Flags: soft-reset, mute, echo writes disable, noise rate
  ESA = 0x6D,     // Echo buffer start address (page in APU RAM)
  FIR6 = 0x6F,    // FIR filter coefficient 6
  /* ── Voice 7 registers ── */
  V7VOLL = 0x70,
  V7VOLR = 0x71,
  V7PL = 0x72,
  V7PH = 0x73,
  V7SRCN = 0x74,
  V7ADSR1 = 0x75,
  V7ADSR2 = 0x76,
  V7GAIN = 0x77,
  V7ENVX = 0x78,
  V7OUTX = 0x79,
  /* ── Global registers in voice 7 block ── */
  ENDX = 0x7C,    // End flags (read-only): bit N set when voice N reaches BRR end
  EDL = 0x7D,     // Echo delay — ring buffer size in 2048-sample increments
  FIR7 = 0x7F,    // FIR filter coefficient 7
};
#endif  // DSP_REGS_H