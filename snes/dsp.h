/*
 * dsp.h — Sony S-DSP (Digital Signal Processor) emulator.
 *
 * The S-DSP lives inside the SNES APU and is responsible for all audio output.
 * It provides 8 independent voices, each capable of playing BRR-compressed
 * 4-bit ADPCM samples with pitch control and ADSR/gain envelopes. Voices
 * can be pitch-modulated by the previous voice's output. A global noise
 * generator can replace any voice's sample source.
 *
 * Mixed output passes through an echo buffer with an 8-tap FIR filter,
 * providing reverb/delay effects. The echo buffer lives in APU RAM and
 * is sized by the EDL register (up to ~240ms at 32kHz).
 *
 * The DSP processes one sample every 32 APU cycles, producing stereo
 * output at 32040 Hz (the SNES native sample rate).
 */
#ifndef DSP_H
#define DSP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "dsp_regs.h"
typedef struct Dsp Dsp;

#include "saveload.h"

// Per-voice state for one of the DSP's 8 channels.
// Each voice independently decodes BRR samples, applies pitch/envelope,
// and contributes to the stereo mix and optional echo buffer.
typedef struct DspChannel {
  // -- Pitch --
  uint16_t pitch;          // 14-bit pitch value (P register); higher = faster playback
  uint16_t pitchCounter;   // Fractional sample position; upper bits index decodeBuffer
  bool pitchModulation;    // When true, previous voice's output modulates this voice's pitch
  // -- BRR sample decoding --
  int16_t decodeBuffer[19]; // Decoded PCM ring: 16 samples per BRR block + 3 for Gaussian interp
  uint8_t srcn;            // Source number (indexes the sample directory at dirPage)
  uint16_t decodeOffset;   // Current position within the BRR block being decoded
  uint8_t previousFlags;   // Header flags from the last decoded BRR block (loop/end bits)
  int16_t old;             // Previous decoded sample (for BRR filter prediction)
  int16_t older;           // Two-samples-ago decoded value (for BRR filter prediction)
  bool useNoise;           // When true, substitute the global noise generator for this voice
  // -- ADSR / Envelope / Gain --
  uint16_t adsrRates[4];  // Rate values for attack, decay, sustain, and gain phases
  uint16_t rateCounter;   // Counter for envelope rate timing (compared against rate table)
  uint8_t adsrState;      // Envelope phase: 0=attack, 1=decay, 2=sustain, 3=gain, 4=release
  uint16_t sustainLevel;  // Envelope level at which decay transitions to sustain
  bool useGain;           // When true, use manual gain mode instead of ADSR
  uint8_t gainMode;       // Gain mode: 0=linear dec, 1=exp dec, 2=linear inc, 3=bent inc
  bool directGain;        // When true, gainValue is applied directly (no ramping)
  uint16_t gainValue;     // Fixed envelope level for direct gain mode
  uint16_t gain;          // Current envelope level (0-0x7FF); scales sample amplitude
  // -- Key on / Key off --
  bool keyOn;             // Pending key-on event (starts voice from sample beginning)
  bool keyOff;            // Pending key-off event (enters release phase, envelope → 0)
  // -- Output --
  int16_t sampleOut;      // Final interpolated+enveloped sample before volume scaling
  int8_t volumeL;         // Left channel volume (-128 to +127)
  int8_t volumeR;         // Right channel volume (-128 to +127)
  bool echoEnable;        // When true, this voice's output feeds into the echo buffer
} DspChannel;

// Main DSP state — owns all 8 voices, noise generator, echo unit, and output buffer.
struct Dsp {
  uint8_t *apu_ram;        // Pointer to the APU's 64KB RAM (for BRR data and echo buffer)
  // -- Register mirror --
  uint8_t ram[0x80];       // Shadow copy of all 128 DSP registers (read back via $F2/$F3)
  // -- Voice channels --
  DspChannel channel[8];   // The 8 independent synthesis voices
  // -- Global settings --
  uint16_t dirPage;        // Sample directory base address in APU RAM (DIR register × 0x100)
  bool evenCycle;          // Toggles each DSP cycle; some operations only run on even cycles
  bool mute;               // When true, all audio output is silenced (FLG bit 6)
  bool reset;              // When true, all voices are muted and envelopes frozen (FLG bit 7)
  int8_t masterVolumeL;    // Master left volume (MVOLL register, -128 to +127)
  int8_t masterVolumeR;    // Master right volume (MVOLR register, -128 to +127)
  // -- Noise generator --
  int16_t noiseSample;     // Current noise output value (15-bit LFSR → signed sample)
  uint16_t noiseRate;      // Noise clock rate (FLG bits 0-4, indexes rate table)
  uint16_t noiseCounter;   // Counter for noise rate timing
  // -- Echo / reverb unit --
  bool echoWrites;         // When false, echo buffer writes are suppressed (FLG bit 5)
  int8_t echoVolumeL;      // Echo left output volume (EVOLL register)
  int8_t echoVolumeR;      // Echo right output volume (EVOLR register)
  int8_t feedbackVolume;   // Echo feedback coefficient (EFB register); controls decay
  uint16_t echoBufferAdr;  // Echo ring buffer start address in APU RAM (ESA × 0x100)
  uint16_t echoDelay;      // Echo buffer size in 2KB units (EDL register, 0-15)
  uint16_t echoRemain;     // Samples remaining before echo pointer wraps around
  uint16_t echoBufferIndex; // Current read/write position within the echo buffer
  uint8_t firBufferIndex;  // Circular index into the FIR filter history buffers
  int8_t firValues[8];     // 8-tap FIR filter coefficients (FIR0-FIR7 registers)
  int16_t firBufferL[8];   // FIR filter sample history for left channel
  int16_t firBufferR[8];   // FIR filter sample history for right channel
  // -- Audio output buffer --
  // Holds one video frame of audio: 534 stereo samples at 32040 Hz (≈16.6ms)
  int16_t sampleBuffer[534 * 2];
  uint16_t sampleOffset;   // Current write position in sampleBuffer (advances by 2 per sample)
};


// Records DSP register writes within a single frame for replay/synchronization.
// The APU captures each write's address and value so that the main thread can
// replay them in order when rendering audio for that frame.
typedef struct DspRegWriteHistory {
  uint32_t count;       // Number of register writes recorded this frame
  uint8_t addr[256];    // DSP register addresses written (0x00-0x7F)
  uint8_t val[256];     // Corresponding values written to each address
} DspRegWriteHistory;

// Allocate and return a new DSP, backed by the given 64KB APU RAM.
Dsp* dsp_init(uint8_t *apu_ram);
// Free the DSP and its allocated memory.
void dsp_free(Dsp* dsp);
// Reset all voices, envelopes, echo state, and registers to power-on defaults.
void dsp_reset(Dsp* dsp);
// Advance the DSP by one cycle: decode BRR, apply envelopes, mix voices,
// process echo/FIR, and write stereo samples to the output buffer.
void dsp_cycle(Dsp* dsp);
// Read a DSP register value from the register mirror (does not affect state).
uint8_t dsp_read(Dsp* dsp, uint8_t adr);
// Write a value to a DSP register; updates voice/global state accordingly.
void dsp_write(Dsp* dsp, uint8_t adr, uint8_t val);
// Copy the internal sample buffer into the caller's output buffer, resampling
// from 534 samples/frame to the requested samplesPerFrame and channel count.
void dsp_getSamples(Dsp* dsp, int16_t* sampleData, int samplesPerFrame, int numChannels);
// Serialize or deserialize all DSP state for save/load snapshots.
void dsp_saveload(Dsp *dsp, SaveLoadFunc *func, void *ctx);

#endif
