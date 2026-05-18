/*
 * spc_player.c -- SPC700 Music/SFX Engine Emulation
 *
 * This file is a C reimplementation of the SPC700 audio coprocessor program
 * from The Legend of Zelda: A Link to the Past (SNES, 1991). On the original
 * hardware, the SPC700 runs independently with its own 64KB of RAM, executing
 * a custom music bytecode format to drive the S-DSP's 8 BRR-decoded voices.
 *
 * The engine implements:
 *   - Pattern-based music sequencing with subroutine calls and looping
 *   - 8-channel BRR sample playback via the S-DSP
 *   - ADSR and GAIN envelope control per voice
 *   - Vibrato (periodic pitch modulation) and tremolo (periodic volume mod)
 *   - Pitch slides (portamento) and pitch envelopes
 *   - Volume and pan fading with fixed-point interpolation
 *   - Echo/reverb with configurable FIR filter coefficients
 *   - 4-port bidirectional communication with the main SNES CPU
 *   - Sound effect playback on channels 6-7 (port 1) and dynamically
 *     allocated channels (ports 2-3), overlaying the music
 *
 * Communication protocol:
 *   Port 0: Music commands (play song, pause, fade, etc.)
 *   Port 1: High-priority SFX (channels 6-7, e.g., sword swings)
 *   Port 2: Medium-priority SFX (dynamically allocated channels)
 *   Port 3: Low-priority SFX (dynamically allocated channels)
 *
 * Memory map (SPC700 RAM):
 *   0x0000-0x03FF: Engine state variables (mirrors SpcPlayer struct fields)
 *   0x0800-0x0FFF: Engine code (in original ROM; not used in C reimpl)
 *   0x1178+:       Extended volume curve table
 *   0x17C0-0x17FF: Port 1 SFX pointer table
 *   0x1800-0x189D: Port 1 SFX chain table
 *   0x189E-0x18DC: Port 2 SFX chain table
 *   0x18DD-0x191B: Port 2 SFX echo flag table
 *   0x191C-0x1999: Port 3 SFX pointer table
 *   0x199A-0x19D7: Port 3 SFX chain table
 *   0x19D8+:       Port 3 SFX echo flag table
 *   0x3C00-0x3CFF: Sample directory (DIR page at 0x3C)
 *   0x3D00-0x3DFF: Instrument definition table (6 bytes per instrument)
 *   0x3E00-0x3EFF: SFX instrument table (9 bytes per entry)
 *   0xD000+:       Song pointer table (2 bytes per song)
 *
 * Related files:
 *   spc_player.h   -- SpcPlayer and Channel struct definitions
 *   snes/dsp.h     -- S-DSP emulation (BRR decode, envelope, mixing)
 *   snes/dsp_regs.h -- DSP register address constants
 *   audio.c        -- High-level audio output and MSU-1 support
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "types.h"

#include "snes/spc.h"
#include "snes/dsp_regs.h"

#include "spc_player.h"

/*
 * MemMap -- Mapping entry between a Channel struct field and its SPC700 RAM address.
 *
 * The original SPC700 code stores per-channel variables at fixed RAM locations.
 * This table allows bidirectional synchronization between the C struct layout
 * and the original RAM layout. The high bit of org_off (0x8000) indicates a
 * 16-bit (2-byte) field; otherwise it is 8-bit (1-byte). The actual RAM address
 * is (org_off & 0x7FFF) + channel_index * 2 (channels are interleaved at
 * 2-byte stride in the original memory map).
 */
typedef struct MemMap {
  uint16 off, org_off;
} MemMap;

/*
 * MemMapSized -- Mapping entry for SpcPlayer global variables to SPC700 RAM.
 *
 * Similar to MemMap but for non-per-channel (global) fields. Includes an
 * explicit size field since globals can be 1 or 2 bytes without the high-bit
 * encoding trick used in MemMap.
 */
typedef struct MemMapSized {
  uint16 off, org_off, size;
} MemMapSized;

/*
 * kChannel_Maps -- Per-channel field-to-RAM-address mapping table.
 *
 * Each entry maps a Channel struct field offset to its corresponding SPC700
 * RAM base address. The actual address for channel N is:
 *   (org_off & 0x7FFF) + N * 2
 * Bit 15 of org_off set means the field is 16-bit (2 bytes); clear means 8-bit.
 */
static const MemMap kChannel_Maps[] = {
  {offsetof(Channel, pattern_order_ptr_for_chan), 0x8030},
  {offsetof(Channel, note_ticks_left), 0x70},
  {offsetof(Channel, note_keyoff_ticks_left), 0x71},
  {offsetof(Channel, subroutine_num_loops), 0x80},
  {offsetof(Channel, volume_fade_ticks), 0x90},
  {offsetof(Channel, pan_num_ticks), 0x91},
  {offsetof(Channel, pitch_slide_length), 0xa0},
  {offsetof(Channel, pitch_slide_delay_left), 0xa1},
  {offsetof(Channel, vibrato_hold_count), 0xb0},
  {offsetof(Channel, vib_depth), 0xb1},
  {offsetof(Channel, tremolo_hold_count), 0xc0},
  {offsetof(Channel, tremolo_depth), 0xc1},
  {offsetof(Channel, vibrato_change_count), 0x100},
  {offsetof(Channel, note_length), 0x200},
  {offsetof(Channel, note_gate_off_fixedpt), 0x201},
  {offsetof(Channel, channel_volume_master), 0x210},
  {offsetof(Channel, instrument_id), 0x211},
  {offsetof(Channel, instrument_pitch_base), 0x8220},
  {offsetof(Channel, saved_pattern_ptr), 0x8230},
  {offsetof(Channel, pattern_start_ptr), 0x8240},
  {offsetof(Channel, pitch_envelope_num_ticks), 0x280},
  {offsetof(Channel, pitch_envelope_delay), 0x281},
  {offsetof(Channel, pitch_envelope_direction), 0x290},
  {offsetof(Channel, pitch_envelope_slide_value), 0x291},
  {offsetof(Channel, vibrato_count), 0x2a0},
  {offsetof(Channel, vibrato_rate), 0x2a1},
  {offsetof(Channel, vibrato_delay_ticks), 0x2b0},
  {offsetof(Channel, vibrato_fade_num_ticks), 0x2b1},
  {offsetof(Channel, vibrato_fade_add_per_tick), 0x2c0},
  {offsetof(Channel, vibrato_depth_target), 0x2c1},
  {offsetof(Channel, tremolo_count), 0x2d0},
  {offsetof(Channel, tremolo_rate), 0x2d1},
  {offsetof(Channel, tremolo_delay_ticks), 0x2e0},
  {offsetof(Channel, channel_transposition), 0x2f0},
  {offsetof(Channel, channel_volume), 0x8300},
  {offsetof(Channel, volume_fade_addpertick), 0x8310},
  {offsetof(Channel, volume_fade_target), 0x320},
  {offsetof(Channel, final_volume), 0x321},
  {offsetof(Channel, pan_value), 0x8330},
  {offsetof(Channel, pan_add_per_tick), 0x8340},
  {offsetof(Channel, pan_target_value), 0x350},
  {offsetof(Channel, pan_flag_with_phase_invert), 0x351},
  {offsetof(Channel, pitch), 0x8360},
  {offsetof(Channel, pitch_add_per_tick), 0x8370},
  {offsetof(Channel, pitch_target), 0x380},
  {offsetof(Channel, fine_tune), 0x381},
  {offsetof(Channel, sfx_sound_ptr), 0x8390},
  {offsetof(Channel, sfx_which_sound), 0x3a0},
  {offsetof(Channel, sfx_arr_countdown), 0x3a1},
  {offsetof(Channel, sfx_note_length_left), 0x3b0},
  {offsetof(Channel, sfx_note_length), 0x3b1},
  {offsetof(Channel, sfx_pan), 0x3d0},
};

/*
 * kSpcPlayer_Maps -- Global SpcPlayer field-to-RAM-address mapping table.
 *
 * Maps each SpcPlayer struct field to its original SPC700 RAM address
 * and size. Used by CopyVariablesToRam/CopyVariablesFromRam for
 * bidirectional synchronization between the C struct and raw RAM.
 */
static const MemMapSized kSpcPlayer_Maps[] = {
  {offsetof(SpcPlayer, new_value_from_snes), 0x0, 4},
  {offsetof(SpcPlayer, port_to_snes), 0x4, 4},
  {offsetof(SpcPlayer, last_value_from_snes), 0x8, 4},
  {offsetof(SpcPlayer, counter_sf0c), 0xc, 1},
  {offsetof(SpcPlayer, _always_zero), 0xe, 2},
  {offsetof(SpcPlayer, temp_accum), 0x10, 2},
  {offsetof(SpcPlayer, ttt), 0x12, 1},
  {offsetof(SpcPlayer, did_affect_volumepitch_flag), 0x13, 1},
  {offsetof(SpcPlayer, addr0), 0x14, 2},
  {offsetof(SpcPlayer, addr1), 0x16, 2},
  {offsetof(SpcPlayer, lfsr_value), 0x18, 2},
  {offsetof(SpcPlayer, is_chan_on), 0x1a, 1},
  {offsetof(SpcPlayer, fast_forward), 0x1b, 1},
  {offsetof(SpcPlayer, sfx_start_arg_pan), 0x20, 1},
  {offsetof(SpcPlayer, sfx_sound_ptr_cur), 0x2c, 2},
  {offsetof(SpcPlayer, music_ptr_toplevel), 0x40, 2},
  {offsetof(SpcPlayer, block_count), 0x42, 1},
  {offsetof(SpcPlayer, sfx_timer_accum), 0x43, 1},
  {offsetof(SpcPlayer, chn), 0x44, 1},
  {offsetof(SpcPlayer, key_ON), 0x45, 1},
  {offsetof(SpcPlayer, key_OFF), 0x46, 1},
  {offsetof(SpcPlayer, cur_chan_bit), 0x47, 1},
  {offsetof(SpcPlayer, reg_FLG), 0x48, 1},
  {offsetof(SpcPlayer, reg_NON), 0x49, 1},
  {offsetof(SpcPlayer, reg_EON), 0x4a, 1},
  {offsetof(SpcPlayer, reg_PMON), 0x4b, 1},
  {offsetof(SpcPlayer, echo_stored_time), 0x4c, 1},
  {offsetof(SpcPlayer, echo_parameter_EDL), 0x4d, 1},
  {offsetof(SpcPlayer, reg_EFB), 0x4e, 1},
  {offsetof(SpcPlayer, global_transposition), 0x50, 1},
  {offsetof(SpcPlayer, main_tempo_accum), 0x51, 1},
  {offsetof(SpcPlayer, tempo), 0x52, 2},
  {offsetof(SpcPlayer, tempo_fade_num_ticks), 0x54, 1},
  {offsetof(SpcPlayer, tempo_fade_final), 0x55, 1},
  {offsetof(SpcPlayer, tempo_fade_add), 0x56, 2},
  {offsetof(SpcPlayer, master_volume), 0x58, 2},
  {offsetof(SpcPlayer, master_volume_fade_ticks), 0x5a, 1},
  {offsetof(SpcPlayer, master_volume_fade_target), 0x5b, 1},
  {offsetof(SpcPlayer, master_volume_fade_add_per_tick), 0x5c, 2},
  {offsetof(SpcPlayer, vol_dirty), 0x5e, 1},
  {offsetof(SpcPlayer, percussion_base_id), 0x5f, 1},
  {offsetof(SpcPlayer, echo_volume_left), 0x60, 2},
  {offsetof(SpcPlayer, echo_volume_right), 0x62, 2},
  {offsetof(SpcPlayer, echo_volume_fade_add_left), 0x64, 2},
  {offsetof(SpcPlayer, echo_volume_fade_add_right), 0x66, 2},
  {offsetof(SpcPlayer, echo_volume_fade_ticks), 0x68, 1},
  {offsetof(SpcPlayer, echo_volume_fade_target_left), 0x69, 1},
  {offsetof(SpcPlayer, echo_volume_fade_target_right), 0x6a, 1},
  {offsetof(SpcPlayer, sfx_channel_index), 0x3c0, 1},
  {offsetof(SpcPlayer, current_bit), 0x3c1, 1},
  {offsetof(SpcPlayer, dsp_register_index), 0x3c2, 1},
  {offsetof(SpcPlayer, echo_channels), 0x3c3, 1},
  {offsetof(SpcPlayer, byte_3C4), 0x3c4, 1},
  {offsetof(SpcPlayer, byte_3C5), 0x3c5, 1},
  {offsetof(SpcPlayer, echo_fract_incr), 0x3c7, 1},
  {offsetof(SpcPlayer, sfx_channel_index2), 0x3c8, 1},
  {offsetof(SpcPlayer, sfx_channel_bit), 0x3c9, 1},
  {offsetof(SpcPlayer, pause_music_ctr), 0x3ca, 1},
  {offsetof(SpcPlayer, port2_active), 0x3cb, 1},
  {offsetof(SpcPlayer, port2_current_bit), 0x3cc, 1},
  {offsetof(SpcPlayer, port3_active), 0x3cd, 1},
  {offsetof(SpcPlayer, port3_current_bit), 0x3ce, 1},
  {offsetof(SpcPlayer, port1_active), 0x3cf, 1},
  {offsetof(SpcPlayer, port1_current_bit), 0x3e0, 1},
  {offsetof(SpcPlayer, byte_3E1), 0x3e1, 1},
  {offsetof(SpcPlayer, sfx_play_echo_flag), 0x3e2, 1},
  {offsetof(SpcPlayer, sfx_channels_echo_mask2), 0x3e3, 1},
  {offsetof(SpcPlayer, port1_counter), 0x3e4, 1},
  {offsetof(SpcPlayer, channel_67_volume), 0x3e5, 1},
  {offsetof(SpcPlayer, cutk_always_zero), 0x3ff, 1},
};

// Forward declaration: PlayNote is called from the pattern parser but defined
// later because it depends on WritePitch which must be defined first.
static void PlayNote(SpcPlayer *p, Channel *c, uint8 note);

/*
 * Dsp_Write -- Write a value to a DSP register, optionally logging it.
 *
 * @p:     Player state
 * @reg:   DSP register address (0x00-0x7F, see dsp_regs.h)
 * @value: Byte value to write to the register
 *
 * If a DspRegWriteHistory is attached (for debugging/verification),
 * each write is recorded so it can be compared against the original
 * SPC700 implementation's register write sequence.
 */
static void Dsp_Write(SpcPlayer *p, uint8_t reg, uint8 value) {
  DspRegWriteHistory *hist = p->reg_write_history;
  if (hist) {
    if (hist->count < 256) {  // Cap at 256 entries to prevent buffer overflow
      hist->addr[hist->count] = reg;
      hist->val[hist->count] = value;
      hist->count++;
    }
  }
  if (p->dsp)
    dsp_write(p->dsp, reg, value);
}

/*
 * Not_Implemented -- Placeholder for unimplemented SPC700 engine features.
 *
 * Triggers an assertion failure in debug builds. This should never be reached
 * during normal gameplay; it guards against executing unhandled music commands.
 */
static void Not_Implemented() {
  assert(0);
  printf("Not Implemented\n");
}

/*
 * SpcDivHelper -- Fixed-point signed division for smooth parameter fading.
 *
 * @a:  Signed 9-bit numerator (difference between target and current value).
 *      Bit 8 acts as the sign bit: if set, the value is negated.
 * @b:  Unsigned 8-bit denominator (number of ticks over which to fade).
 *
 * Returns a 16-bit fixed-point result (8.8 format) representing the per-tick
 * increment needed to reach the target value in 'b' ticks. This mirrors the
 * SPC700's DIV YA,X instruction behavior, which produces both quotient and
 * remainder, then combines them into a smooth fractional increment.
 *
 * If b is zero, returns the maximum magnitude (0xFF in each part) to prevent
 * division by zero, matching the original SPC700 behavior where DIV by zero
 * produces 0xFF.
 */
static uint16 SpcDivHelper(int a, uint8 b) {
  int org_a = a;
  if (a & 0x100)       // Bit 8 indicates negative difference
    a = -a;            // Work with absolute value for the division
  int q = b ? (a & 0xff) / b : 0xff;               // Integer part of per-tick delta
  int r = b ? (a & 0xff) % b : (a & 0xff);          // Remainder for fractional part
  int t = (q << 8) + (b ? ((r << 8) / b & 0xff) : 0xff);  // Combine into 8.8 fixed-point
  return (org_a & 0x100) ? -t : t;  // Restore original sign
}

/*
 * Chan_DoAnyFade -- Apply one tick of a parameter fade (volume, pitch, pan, etc).
 *
 * @p:      Pointer to the 16-bit parameter being faded (8.8 fixed-point)
 * @add:    Per-tick increment (from SpcDivHelper)
 * @target: Final target value (integer part, high byte)
 * @cont:   Remaining ticks; if zero, snap directly to the target value
 *
 * When cont reaches zero (final tick), the parameter is set exactly to
 * target << 8 to avoid accumulated rounding error. Otherwise, the
 * per-tick delta is added for smooth interpolation.
 */
static inline void Chan_DoAnyFade(uint16 *p, uint16 add, uint8 target, uint8 cont) {
  if (!cont)
    *p = target << 8;
  else
    *p += add;
}

/*
 * SetupEchoParameter_EDL -- Configure the DSP echo delay length.
 *
 * @p: Player state
 * @a: New echo delay length in 16ms units (0-15). Each unit adds 2048 samples
 *     of delay buffer (32KB max at EDL=15).
 *
 * Changing EDL requires temporarily disabling echo to prevent audio glitches.
 * The echo buffer is a ring buffer at address ESA in SPC RAM; changing EDL
 * resizes it. This function disables echo output and feedback, then writes
 * the new EDL value. Echo will be re-enabled after echo_stored_time catches
 * up to the new EDL value (handled in the main loop).
 */
static void SetupEchoParameter_EDL(SpcPlayer *p, uint8 a) {
  p->echo_parameter_EDL = a;
  if (a != p->last_written_edl) {
    // Compute how long to wait before re-enabling echo, based on the old
    // delay length. This ensures the echo buffer has been fully overwritten
    // with the new size before re-enabling output.
    a = (p->last_written_edl & 0xf) ^ 0xff;
    if (p->echo_stored_time & 0x80)
      a += p->echo_stored_time;
    p->echo_stored_time = a;

    // Disable all echo processing while changing the delay length to avoid
    // reading stale echo data that could cause pops or glitches
    Dsp_Write(p, EON, 0);     // Disable echo on all channels
    Dsp_Write(p, EFB, 0);     // Zero echo feedback to silence the buffer
    Dsp_Write(p, EVOLR, 0);   // Mute echo right output
    Dsp_Write(p, EVOLL, 0);   // Mute echo left output
    Dsp_Write(p, FLG, p->reg_FLG | 0x20);  // Bit 5: disable echo writes to RAM

    p->last_written_edl = p->echo_parameter_EDL;
    Dsp_Write(p, EDL, p->echo_parameter_EDL);  // Set new echo delay length
  }
  // ESA = echo buffer start address (page). The formula places the echo buffer
  // at the top of RAM, growing downward: larger EDL values push ESA lower.
  // (EDL * 8) ^ 0xFF inverts to count down from 0xFF, + 0xD1 offsets the base.
  Dsp_Write(p, ESA, (p->echo_parameter_EDL * 8 ^ 0xff) + 0xd1);
}

/*
 * WriteVolumeToDsp -- Compute and write left/right volume to DSP voice registers.
 *
 * @p:      Player state
 * @c:      Channel whose DSP volume registers will be updated
 * @volume: Pan position in 8.8 fixed-point. 0x0A00 = center (pan value 10).
 *          Higher values pan left; the right volume is computed as (0x1400 - volume).
 *
 * The volume curve is non-linear: a 22-entry lookup table maps linear pan
 * position to a perceptual volume curve (approximating equal-power panning).
 * For pan values >= 21, an extended curve from RAM at 0x1178 is used instead.
 * The fractional part of the volume interpolates between adjacent table entries.
 * The result is scaled by the channel's final_volume and optionally phase-inverted.
 */
static void WriteVolumeToDsp(SpcPlayer *p, Channel *c, uint16 volume) {
  // Non-linear volume curve: maps 0-21 to a perceptually even loudness ramp.
  // Entry 0 = silence, entry 21 = maximum (127). This approximates equal-power
  // panning so that center pan doesn't sound quieter than full left or right.
  static const uint8 kVolumeTable[22] = {0, 1, 3, 7, 13, 21, 30, 41, 52, 66, 81, 94, 103, 110, 115, 119, 122, 124, 125, 126, 127, 127};
  if (p->is_chan_on & p->cur_chan_bit)
    return;  // SFX has taken over this channel; don't touch its DSP volume
  // Loop twice: i=0 for left volume, i=1 for right volume.
  // After the first iteration, volume is flipped to (0x1400 - volume) so that
  // the second pass computes the complementary pan side.
  for (int i = 0; i < 2; i++) {
    int j = volume >> 8;  // Integer part of pan position = table index
    uint8 t;
    if (j >= 21) {
      // Extended curve from RAM for pan values beyond the built-in table
      t = p->ram[j + 0x1178] + ((p->ram[j + 0x1179] - p->ram[j + 0x1178]) * (uint8)volume >> 8);
    } else {
      // Linear interpolation between adjacent table entries using the
      // fractional part of volume (low byte) for smooth sub-step panning
      t = kVolumeTable[j] + ((kVolumeTable[j + 1] - kVolumeTable[j]) * (uint8)volume >> 8);
    }


    t = t * c->final_volume >> 8;  // Scale by the channel's computed final volume
    if ((c->pan_flag_with_phase_invert << i) & 0x80)
      t = -t;  // Phase invert: negate the volume for this side (signed DSP register)
    Dsp_Write(p, V0VOLL + i + c->index * 16, t);  // Write VOLL (i=0) or VOLR (i=1)
    volume = 0x1400 - volume;  // Flip pan: what was left becomes right's complement
  }
}

/*
 * WritePitch -- Convert a note pitch to a DSP frequency and write it.
 *
 * @p:     Player state
 * @c:     Channel whose pitch registers will be updated
 * @pitch: Note pitch in 8.8 fixed-point. High byte = semitone (0x00-0x7F),
 *         low byte = fine-tune fraction between semitones.
 *
 * The conversion process:
 * 1. Apply pitch correction for extreme ranges (high notes > 0x34, low < 0x13)
 * 2. Split the corrected pitch into octave (q) and semitone within octave (r)
 * 3. Look up the base frequency from a 13-entry chromatic scale table and
 *    interpolate between adjacent semitones using the fractional part
 * 4. Shift the frequency by octave relative to octave 6 (the reference octave)
 * 5. Scale by the instrument's base pitch multiplier
 * 6. Write the 14-bit result to the DSP PITCHL/PITCHH registers
 */
static void WritePitch(SpcPlayer *p, Channel *c, uint16 pitch) {
  // Chromatic scale frequencies for one octave (C through C+1), used as a lookup
  // table. Entry 12 (=4286) is the next octave's C, enabling interpolation at B.
  // These values correspond to DSP pitch register units at octave 6.
  static const uint16 kBaseNoteFreqs[13] = {2143, 2270, 2405, 2548, 2700, 2860, 3030, 3211, 3402, 3604, 3818, 4045, 4286};
  // Apply pitch correction for notes at extreme ends of the range to compensate
  // for tuning inaccuracies in the original engine's frequency table
  if ((pitch >> 8) >= 0x34) {
    pitch += (pitch >> 8) - 0x34;     // Sharpen high notes progressively
  } else if ((pitch >> 8) < 0x13) {
    pitch += (uint8)(((pitch >> 8) - 0x13) * 2) - 256;  // Flatten low notes
  }

  uint8 pp = (pitch >> 8) & 0x7f;  // Mask to 7-bit semitone range
  uint8 q = pp / 12, r = pp % 12;  // q = octave number, r = semitone within octave
  // Interpolate between adjacent semitone frequencies using the fractional byte
  uint16 t = kBaseNoteFreqs[r] + ((uint8)(kBaseNoteFreqs[r + 1] - kBaseNoteFreqs[r]) * (uint8)pitch >> 8);
  t *= 2;  // Base frequencies are stored at half scale
  // Shift frequency to the correct octave. Octave 6 is the reference; lower
  // octaves halve the frequency for each step below 6.
  while (q != 6)
    t >>= 1, q++;

  // Scale by the instrument's pitch base, which adjusts for the sample's
  // native playback rate (different samples are recorded at different pitches)
  t = c->instrument_pitch_base * t >> 8;
  if (!(p->cur_chan_bit & p->is_chan_on)) {  // Skip if SFX owns this channel
    uint8 reg = c->index * 16;  // Each voice occupies 16 DSP register addresses
    Dsp_Write(p, reg + V0PITCHL, t & 0xff);  // Low 8 bits of 14-bit pitch
    Dsp_Write(p, reg + V0PITCHH, t >> 8);    // High 6 bits of 14-bit pitch
  }
}

/*
 * Music_ResetChan -- Reset all 8 channels and global music state to defaults.
 *
 * @p: Player state
 *
 * Called when loading a new song or re-initializing the music engine. Sets
 * each channel to full volume, center pan (10), no instrument, no vibrato,
 * no tremolo, no transposition, and no pitch envelope. Also clears all
 * global fade states and sets the default master volume (0xC0) and tempo (0x20).
 */
static void Music_ResetChan(SpcPlayer *p) {
  // Iterate all 8 channels in reverse (7 down to 0), matching the original
  // SPC700 code's iteration order which uses a bitmask shifted right
  Channel *c = &p->channel[7];
  p->cur_chan_bit = 0x80;  // Start with channel 7 bitmask
  do {
    HIBYTE(c->channel_volume) = 0xff;  // Full channel volume (high byte of 8.8)
    c->pan_flag_with_phase_invert = 10;  // Center pan (10 out of 20 range)
    c->pan_value = 10 << 8;             // Center pan in 8.8 fixed-point
    c->instrument_id = 0;
    c->fine_tune = 0;
    c->channel_transposition = 0;
    c->pitch_envelope_num_ticks = 0;
    c->vib_depth = 0;
    c->tremolo_depth = 0;
  } while (c--, p->cur_chan_bit >>= 1);
  p->master_volume_fade_ticks = 0;
  p->echo_volume_fade_ticks = 0;
  p->tempo_fade_num_ticks = 0;
  p->global_transposition = 0;
  p->block_count = 0;
  p->percussion_base_id = 0;
  HIBYTE(p->master_volume) = 0xc0;  // Default master volume ~75%
  HIBYTE(p->tempo) = 0x20;          // Default tempo (moderate speed)
}

/*
 * Channel_SetInstrument -- Load an instrument's DSP parameters into a channel.
 *
 * @p:          Player state
 * @c:          Target channel
 * @instrument: Instrument ID. Values 0-127 index the instrument table directly.
 *              Values with bit 7 set (>= 0x80) are percussion notes: the
 *              actual instrument index is computed by adding 54 + percussion_base_id.
 *
 * Each instrument occupies 6 bytes in the table at RAM 0x3D00:
 *   [0]: Source number (SRCN) -- sample directory index. If bit 7 is set,
 *        the channel uses noise instead: bits 0-4 set the noise clock rate.
 *   [1]: ADSR1 register (attack/decay rates and ADSR enable flag)
 *   [2]: ADSR2 register (sustain level and sustain rate)
 *   [3]: GAIN register (used when ADSR is disabled)
 *   [4-5]: Pitch base multiplier (16-bit, big-endian) for tuning the sample
 */
static void Channel_SetInstrument(SpcPlayer *p, Channel *c, uint8 instrument) {
  c->instrument_id = instrument;
  if (instrument & 0x80)
    instrument = instrument + 54 + p->percussion_base_id;  // Remap percussion to inst table
  const uint8 *ip = p->ram + instrument * 6 + 0x3d00;  // 6 bytes per instrument at 0x3D00
  if (p->is_chan_on & p->cur_chan_bit)
    return;  // SFX owns this channel; don't change its DSP state
  uint8 reg = c->index * 16;  // DSP register base for this voice
  if (ip[0] & 0x80) {
    // noise: SRCN byte bit 7 means use noise generator instead of BRR sample.
    // Bits 0-4 set the noise clock frequency in the FLG register.
    p->reg_FLG = (p->reg_FLG & 0x20) | ip[0] & 0x1f;  // Preserve echo disable, set noise rate
    p->reg_NON |= p->cur_chan_bit;  // Enable noise for this channel
    Dsp_Write(p, reg + V0SRCN, 0);  // SRCN=0; sample data is irrelevant for noise
  } else {
    Dsp_Write(p, reg + V0SRCN, ip[0]);  // Set the BRR sample source number
  }
  Dsp_Write(p, reg + V0ADSR1, ip[1]);  // Attack/decay rates
  Dsp_Write(p, reg + V0ADSR2, ip[2]);  // Sustain level/rate
  Dsp_Write(p, reg + V0GAIN, ip[3]);   // GAIN mode (used when ADSR disabled)
  c->instrument_pitch_base = ip[4] << 8 | ip[5];  // 16-bit pitch multiplier (big-endian)
}

/*
 * ComputePitchAdd -- Calculate the per-tick pitch delta for a pitch slide.
 *
 * @c:     Channel with an active pitch slide
 * @pitch: Target pitch (semitone, 7-bit). The current pitch high byte is the
 *         source. The difference is divided by pitch_slide_length ticks.
 */
static void ComputePitchAdd(Channel *c, uint8 pitch) {
  c->pitch_target = pitch & 0x7f;
  c->pitch_add_per_tick = SpcDivHelper(c->pitch_target - (c->pitch >> 8), c->pitch_slide_length);
}

/*
 * PitchSlideToNote_Check -- Check if the next pattern byte is a pitch slide
 *                           command (0xF9) and apply it inline.
 *
 * @p: Player state
 * @c: Channel being processed
 *
 * This is called after playing a note to handle the case where a pitch slide
 * command immediately follows the note. The slide will begin after a delay
 * and glide the pitch to the target note over pitch_slide_length ticks.
 * If a slide is already active (pitch_slide_length != 0), the check is skipped.
 */
static void PitchSlideToNote_Check(SpcPlayer *p, Channel *c) {
  if (c->pitch_slide_length || p->ram[c->pattern_order_ptr_for_chan] != 0xf9)
    return;

  if (p->cur_chan_bit & p->is_chan_on) {
    c->pattern_order_ptr_for_chan += 4;  // Skip the 4-byte slide command (SFX owns channel)
    return;
  }
  // Read the 3 slide parameters that follow the 0xF9 command byte:
  //   byte 1: delay before slide starts
  //   byte 2: number of ticks for the slide
  //   byte 3: target note (adjusted by global and channel transposition)
  c->pattern_order_ptr_for_chan++;
  c->pitch_slide_delay_left = p->ram[c->pattern_order_ptr_for_chan++];
  c->pitch_slide_length = p->ram[c->pattern_order_ptr_for_chan++];
  ComputePitchAdd(c, p->ram[c->pattern_order_ptr_for_chan++] + p->global_transposition + c->channel_transposition);
}

/*
 * kEffectByteLength -- Number of parameter bytes following each effect command.
 *
 * Indexed by (effect_byte - 0xE0). Effect commands range from 0xE0 to 0xFA.
 * This table tells the parser how many additional bytes to consume after the
 * command opcode, enabling the WantWriteKof lookahead to skip over effects
 * without interpreting them.
 */
static const uint8 kEffectByteLength[27] = {1, 1, 2, 3, 0, 1, 2, 1, 2, 1, 1, 3, 0, 1, 2, 3, 1, 3, 3, 0, 1, 3, 0, 3, 3, 3, 1};

/*
 * HandleEffect -- Execute a music pattern effect command.
 *
 * @p:      Player state
 * @c:      Channel on which the effect occurs
 * @effect: Effect opcode (0xE0-0xFA). Each effect has 0-3 parameter bytes
 *          that immediately follow in the pattern data stream.
 *
 * Effect command summary:
 *   0xE0: Set instrument           0xE1: Set pan
 *   0xE2: Pan fade                 0xE3: Vibrato on
 *   0xE4: Vibrato off              0xE5: Set master volume
 *   0xE6: Master volume fade       0xE7: Set tempo
 *   0xE8: Tempo fade               0xE9: Global transposition
 *   0xEA: Channel transposition    0xEB: Tremolo on
 *   0xEC: Tremolo off              0xED: Set channel volume
 *   0xEE: Channel volume fade      0xEF: Call subroutine
 *   0xF0: Vibrato fade out         0xF4: Fine tune
 *   0xF5: Echo on                  0xF6: Echo off
 *   0xF7: Echo parameters (EDL, EFB, FIR filter)
 *   0xF8: Echo volume fade         0xF9: Pitch slide to note
 *   0xFA: Set percussion base ID
 */
static void HandleEffect(SpcPlayer *p, Channel *c, uint8 effect) {
  // Read the first parameter byte if this effect takes any arguments
  uint8 arg = kEffectByteLength[effect - 0xe0] ? p->ram[c->pattern_order_ptr_for_chan++] : 0;

  switch (effect) {
  case 0xe0:  // Set instrument: arg = instrument ID
    Channel_SetInstrument(p, c, arg);
    break;
  case 0xe1:  // Set pan: arg = pan position (0-20) with phase invert flags in upper bits
    c->pan_flag_with_phase_invert = arg;
    c->pan_value = (arg & 0x1f) << 8;  // Mask to 5-bit pan, convert to 8.8 fixed-point
    break;
  case 0xe2:  // Pan fade: arg = duration, next byte = target pan position
    c->pan_num_ticks = arg;
    c->pan_target_value = p->ram[c->pattern_order_ptr_for_chan++];
    c->pan_add_per_tick = SpcDivHelper(c->pan_target_value - (c->pan_value >> 8), arg);
    break;
  case 0xe3: // vibrato on: arg = delay, then rate, then depth
    c->vibrato_delay_ticks = arg;
    c->vibrato_rate = p->ram[c->pattern_order_ptr_for_chan++];
    c->vibrato_depth_target = c->vib_depth = p->ram[c->pattern_order_ptr_for_chan++];
    c->vibrato_fade_num_ticks = 0;  // No fade-in; vibrato starts at full depth
    break;
  case 0xe4: // vibrato off: immediately zero depth and disable fade
    c->vibrato_depth_target = c->vib_depth = 0;
    c->vibrato_fade_num_ticks = 0;
    break;
  case 0xe5:  // Set master volume: arg = new master volume level
    // Only set if music is not paused and no volume override is active
    if (!p->pause_music_ctr && !p->byte_3E1)
      p->master_volume = arg << 8;
    break;
  case 0xe6:  // Master volume fade: arg = duration, next byte = target
    p->master_volume_fade_ticks = arg;
    p->master_volume_fade_target = p->ram[c->pattern_order_ptr_for_chan++];
    p->master_volume_fade_add_per_tick = SpcDivHelper(p->master_volume_fade_target - (p->master_volume >> 8), arg);
    break;
  case 0xe7:  // Set tempo: arg = new tempo value
    p->tempo = arg << 8;
    break;
  case 0xe8:  // Tempo fade: arg = duration, next byte = target tempo
    p->tempo_fade_num_ticks = arg;
    p->tempo_fade_final = p->ram[c->pattern_order_ptr_for_chan++];
    p->tempo_fade_add = SpcDivHelper(p->tempo_fade_final - (p->tempo >> 8), arg);
    break;
  case 0xe9:  // Global transposition: arg = semitone offset for all channels
    p->global_transposition = arg;
    break;
  case 0xea:  // Channel transposition: arg = semitone offset for this channel only
    c->channel_transposition = arg;
    break;
  case 0xeb:  // Tremolo on: arg = delay, then rate, then depth
    c->tremolo_delay_ticks = arg;
    c->tremolo_rate = p->ram[c->pattern_order_ptr_for_chan++];
    c->tremolo_depth = p->ram[c->pattern_order_ptr_for_chan++];
    break;
  case 0xec:  // Tremolo off: zero depth to disable
    c->tremolo_depth = 0;
    break;
  case 0xed:  // Set channel volume: arg = volume level
    c->channel_volume = arg << 8;
    break;
  case 0xee:  // Channel volume fade: arg = duration, next byte = target volume
    c->volume_fade_ticks = arg;
    c->volume_fade_target = p->ram[c->pattern_order_ptr_for_chan++];
    c->volume_fade_addpertick = SpcDivHelper(c->volume_fade_target - (c->channel_volume >> 8), arg);
    break;
  case 0xef:  // Call subroutine: arg + next byte = target address, then loop count
    // Save the return address and jump to the subroutine pattern
    c->pattern_start_ptr = p->ram[c->pattern_order_ptr_for_chan++] << 8 | arg;
    c->subroutine_num_loops = p->ram[c->pattern_order_ptr_for_chan++];
    c->saved_pattern_ptr = c->pattern_order_ptr_for_chan;  // Return address
    c->pattern_order_ptr_for_chan = c->pattern_start_ptr;   // Jump to subroutine
    break;
  case 0xf0:  // Vibrato fade out: arg = number of ticks to fade vibrato to zero
    c->vibrato_fade_num_ticks = arg;
    // Calculate how much depth to remove per tick to reach zero
    c->vibrato_fade_add_per_tick = arg ? c->vib_depth / arg : 0xff;
    break;
  case 0xf4:  // Fine tune: arg = pitch offset in sub-semitone units
    c->fine_tune = arg;
    break;
  case 0xf5:  // Echo on: arg = channel enable mask, then left vol, right vol
    p->reg_EON = p->echo_channels = arg;  // Set which channels feed into echo
    p->echo_volume_left = p->ram[c->pattern_order_ptr_for_chan++] << 8;
    p->echo_volume_right = p->ram[c->pattern_order_ptr_for_chan++] << 8;
    p->reg_FLG &= ~0x20;  // Clear bit 5 to enable echo writes to RAM
    break;
  case 0xf6:  // Echo off: mute echo output and disable echo writes
    p->echo_volume_left = 0;
    p->echo_volume_right = 0;
    p->reg_FLG |= 0x20;  // Set bit 5 to disable echo buffer writes
    break;
  case 0xf7: {  // Echo parameters: arg = EDL, then EFB, then FIR filter preset index
    // 4 preset FIR filter coefficient sets (8 taps each). These shape the echo's
    // frequency response. Preset 0 is a pass-through (no filtering), while
    // presets 1-3 create different reverb coloring effects.
    static const int8_t kEchoFirParameters[] = {
      127, 0, 0, 0, 0, 0, 0, 0,
      88, -65, -37, -16, -2, 7, 12, 12,
      12, 33, 43, 43, 19, -2, -13, -7,
      52, 51, 0, -39, -27, 1, -4, -21,
    };
    SetupEchoParameter_EDL(p, arg);  // Configure echo delay length
    p->reg_EFB = p->ram[c->pattern_order_ptr_for_chan++];  // Echo feedback coefficient
    // Select one of 4 FIR filter presets and write all 8 coefficients.
    // FIR registers are spaced 16 apart (FIR0=0x0F, FIR1=0x1F, ..., FIR7=0x7F)
    const int8_t *ep = kEchoFirParameters + p->ram[c->pattern_order_ptr_for_chan++] * 8;
    for (int i = 0; i < 8; i++)
      Dsp_Write(p, FIR0 + i * 16, *ep++);
    break;
  }
  case 0xf8:  // Echo volume fade: arg = duration, then left target, right target
    p->echo_volume_fade_ticks = arg;
    p->echo_volume_fade_target_left = p->ram[c->pattern_order_ptr_for_chan++];
    p->echo_volume_fade_target_right = p->ram[c->pattern_order_ptr_for_chan++];
    p->echo_volume_fade_add_left = SpcDivHelper(p->echo_volume_fade_target_left - (p->echo_volume_left >> 8), arg);
    p->echo_volume_fade_add_right = SpcDivHelper(p->echo_volume_fade_target_right - (p->echo_volume_right >> 8), arg);
    break;
  case 0xf9:  // Pitch slide: arg = delay, then slide length, then target note
    c->pitch_slide_delay_left = arg;
    c->pitch_slide_length = p->ram[c->pattern_order_ptr_for_chan++];
    // Target note is offset by both global and channel transposition
    ComputePitchAdd(c, p->ram[c->pattern_order_ptr_for_chan++] + p->global_transposition + c->channel_transposition);
    break;
  case 0xfa:  // Set percussion base: arg = offset added to percussion instrument IDs
    p->percussion_base_id = arg;
    break;
  default:
    Not_Implemented();
  }
}

/*
 * WantWriteKof — Should the channel emit a key-off DSP write right now?
 *
 * Look-ahead scanner over the music pattern stream. The duration-encoded
 * "gate off" time has expired or is about to, but key-off must NOT be
 * written if the very next musical event is a tie/legato note (0xc8)
 * because that would chop the held note. This function walks the stream
 * from the current read pointer until it can decide.
 *
 * Walks the byte stream skipping notes' duration/velocity prefixes (every
 * non-0x80-bit byte after a "length" byte) and handles the same control
 * codes as the main parser:
 *   - 0x00          : end of pattern, follow the subroutine-loop or chain
 *                     to the next phrase
 *   - 0xc8          : tie/legato sentinel → don't write KOF, return false
 *   - 0xef          : pattern call (subroutine), follow the absolute
 *                     pointer stored in the next two bytes
 *   - 0xe0..0xfe    : effect command, skip kEffectByteLength bytes
 *   - any note byte : real new note coming → write KOF, return true
 *
 * Does not mutate channel state; ptr/loops are scratch copies.
 *
 * @return true if a key-off DSP write is appropriate, false if the next
 *         event ties into the currently sounding note.
 */
static bool WantWriteKof(SpcPlayer *p, Channel *c) {
  int loops = c->subroutine_num_loops;
  int ptr = c->pattern_order_ptr_for_chan;

  for (;;) {
    uint8 cmd = p->ram[ptr++];
    if (cmd == 0) {
      if (loops == 0)
        return true;
      ptr = (--loops == 0) ? c->saved_pattern_ptr : c->pattern_start_ptr;
    } else {
      while (!(cmd & 0x80))
        cmd = p->ram[ptr++];
      if (cmd == 0xc8)
        return false;
      if (cmd == 0xef) {
        ptr = p->ram[ptr + 0] | p->ram[ptr + 1] << 8;
      } else if (cmd >= 0xe0) {
        ptr += kEffectByteLength[cmd - 0xe0];
      } else {
        return true;
      }
    }
  }
}

/*
 * HandleTremolo — Per-tick tremolo (amplitude modulation) processor stub.
 *
 * Tremolo modulates a channel's effective volume on a per-tick basis using
 * the tremolo_rate / tremolo_depth / tremolo_count state. ALttP's music
 * data never exercises this path, so the original SNES routine is unused
 * in this game — the call always lands in Not_Implemented(). Kept here
 * so the call graph stays intact in case data is found that triggers it
 * (e.g. via a future romhack or alternate song bank).
 */
static void HandleTremolo(SpcPlayer *p, Channel *c) {
  Not_Implemented();
}

/*
 * CalcVibratoAddPitch — Apply one tick of vibrato modulation to the pitch.
 *
 * Converts a vibrato phase value (0..0xff, a sawtooth-like counter
 * advanced by vibrato_rate per tick) into a signed pitch delta and
 * writes it to the DSP.
 *
 * Phase folding: shifting left by 2 then folding via the XOR-with-0xff
 * when bit 8 is set converts the sawtooth into a triangle wave that ramps
 * 0..0xff and back to 0 each half-cycle, giving a smooth bend up/down
 * around the base pitch.
 *
 * Depth scaling has two modes:
 *   - depth >= 0xf1: very fine — multiply by depth's low nibble (0..0xe)
 *     for sub-cent micro-vibrato (used for the "shimmer" patches).
 *   - depth <  0xf1: standard — multiply by depth, >> 8 to scale back into
 *     a reasonable pitch-units delta.
 *
 * The high bit of `value` (the original phase) picks the sign so the
 * modulation alternates above and below the base pitch each half-period.
 */
static void CalcVibratoAddPitch(SpcPlayer *p, Channel *c, uint16 pitch, uint8 value) {
  int t = value << 2;
  t ^= (t & 0x100) ? 0xff : 0;
  int r = (c->vib_depth >= 0xf1) ?
      (uint8)t * (c->vib_depth & 0xf) :
      (uint8)t * c->vib_depth >> 8;
  WritePitch(p, c, pitch + (value & 0x80 ? -r : r));
}

/*
 * HandlePanAndSweep — Sub-tick interpolator for pan / pitch / vibrato.
 *
 * Runs on the "fine" timer between full music ticks when the music tempo
 * accumulator hasn't yet overflowed. Smoothly interpolates the channel's
 * volume (pan) and pitch by p->main_tempo_accum-scaled fractions of
 * their per-tick deltas so envelope motion is continuous instead of
 * stepping audibly at the music-tick rate.
 *
 * did_affect_volumepitch_flag is a per-call dirty bit: set to 0x80 by any
 * branch that produced a delta, then checked at the end to decide whether
 * to push the new value into the DSP. This avoids redundant DSP writes
 * when nothing changed (DSP writes are expensive in SPC700 cycles).
 *
 * If vibrato is active and out of its initial delay window, the function
 * returns early via CalcVibratoAddPitch — vibrato writes its own pitch.
 * Otherwise the dirty bit is consulted and WritePitch is called only when
 * pitch_slide is mid-fade.
 */
static void HandlePanAndSweep(SpcPlayer *p, Channel *c) {
  p->did_affect_volumepitch_flag = 0;
  if (c->tremolo_depth) {
    c->tremolo_hold_count = c->tremolo_delay_ticks;
    HandleTremolo(p, c);
  }

  uint16 volume = c->pan_value;

  if (c->pan_num_ticks) {
    p->did_affect_volumepitch_flag = 0x80;
    volume += p->main_tempo_accum * (int16_t)c->pan_add_per_tick / 256;
  }

  if (p->did_affect_volumepitch_flag)
    WriteVolumeToDsp(p, c, volume);

  p->did_affect_volumepitch_flag = 0;
  uint16 pitch = c->pitch;
  if (c->pitch_slide_length && !c->pitch_slide_delay_left) {
    p->did_affect_volumepitch_flag |= 0x80;
    pitch += p->main_tempo_accum * (int16_t)c->pitch_add_per_tick / 256;
  }

  if (c->vib_depth && c->vibrato_delay_ticks == c->vibrato_hold_count) {
    CalcVibratoAddPitch(p, c, pitch, (p->main_tempo_accum * c->vibrato_rate >> 8) + c->vibrato_count);
    return;
  }

  if (p->did_affect_volumepitch_flag)
    WritePitch(p, c, pitch);
}

/*
 * HandleNoteTick — Per-tick state advance for an actively-playing channel.
 *
 * Runs once per full music tick on each channel currently sustaining a
 * note. Three things may happen:
 *
 *   1. Gate-off countdown. note_keyoff_ticks_left was set when the note
 *      began to (note_length * note_gate_off_fixedpt / 256), i.e. the
 *      fraction of the note's duration that should be held before release.
 *      When that countdown hits zero (or we're at "the last 2 ticks of
 *      the note"), the note transitions to release. WantWriteKof gates
 *      the actual DSP KOF write so we don't break ties/legato.
 *
 *   2. Pitch envelope step. If pitch_slide_length is non-zero, the
 *      channel is mid-bend toward pitch_target. The delay-counter
 *      pitch_slide_delay_left is consumed first (matches the original
 *      SNES code's "wait N ticks before starting the slide" feature),
 *      then Chan_DoAnyFade advances c->pitch by pitch_add_per_tick.
 *
 *   3. Vibrato. If vibrato is configured AND we've passed the initial
 *      delay window (vibrato_hold_count reached vibrato_delay_ticks),
 *      run one vibrato sample: optionally fade the depth toward the
 *      target via vibrato_change_count/fade tables, advance the phase
 *      counter by vibrato_rate, then apply via CalcVibratoAddPitch.
 *      Otherwise just increment vibrato_hold_count to walk through the
 *      delay window without modulating.
 *
 * The dirty-bit pattern (did_affect_volumepitch_flag = 0x80) is the same
 * one used by HandlePanAndSweep — it avoids redundant pitch writes when
 * none of the conditions actually changed anything this tick.
 */
static void HandleNoteTick(SpcPlayer *p, Channel *c) {
  if (c->note_keyoff_ticks_left != 0 && (--c->note_keyoff_ticks_left == 0 || c->note_ticks_left == 2)) {
    if (WantWriteKof(p, c) && !(p->cur_chan_bit & p->is_chan_on))
      Dsp_Write(p, KOF, p->cur_chan_bit);
  }

  p->did_affect_volumepitch_flag = 0;
  if (c->pitch_slide_length) {
    if (c->pitch_slide_delay_left) {
      c->pitch_slide_delay_left--;
    } else if (!(p->is_chan_on & p->cur_chan_bit)) {
      p->did_affect_volumepitch_flag = 0x80;
      Chan_DoAnyFade(&c->pitch, c->pitch_add_per_tick, c->pitch_target, --c->pitch_slide_length);
    }
  }

  uint16 pitch = c->pitch;

  if (c->vib_depth) {
    if (c->vibrato_delay_ticks == c->vibrato_hold_count) {
      if (c->vibrato_change_count == c->vibrato_fade_num_ticks) {
        c->vib_depth = c->vibrato_depth_target;
      } else {
        c->vib_depth = (c->vibrato_change_count++ == 0 ? 0 : c->vib_depth) + c->vibrato_fade_add_per_tick;
      }
      c->vibrato_count += c->vibrato_rate;
      CalcVibratoAddPitch(p, c, pitch, c->vibrato_count);
      return;
    }
    c->vibrato_hold_count++;
  }
  
  if (p->did_affect_volumepitch_flag)
    WritePitch(p, c, pitch);
}

/*
 * CalcFinalVolume — Multiply the per-stage volume scalars into final_volume.
 *
 * Combines four volume factors into the single 0..0xff value sent to the
 * DSP through WriteVolumeToDsp:
 *   - master_volume         : global music volume (16-bit, hi byte used)
 *   - vol                   : the caller-supplied scalar (tremolo output or
 *                             0xff = "no tremolo today")
 *   - channel_volume_master : the note-velocity entry from kNoteVol picked
 *                             when the note started
 *   - channel_volume        : the pattern-controlled "vN" volume command
 *
 * The final `t * t >> 8` step is a perceptual-loudness curve approximation
 * (squaring the linear product) so soft notes don't sound disproportionate
 * to loud ones — mirrors the SPC700 routine's own approach to compensating
 * for the DSP's linear volume scale.
 */
void CalcFinalVolume(SpcPlayer *p, Channel *c, uint8 vol) {
  int t = (p->master_volume >> 8) * vol >> 8;
  t = t * c->channel_volume_master >> 8;
  t = t * (c->channel_volume >> 8) >> 8;
  c->final_volume = t * t >> 8;
}

/*
 * CalcTremolo — Build the per-tick volume scalar from tremolo state (stub).
 *
 * Same situation as HandleTremolo: ALttP's data never reaches this branch,
 * so the routine stays as a Not_Implemented() landing pad. Listed for call
 * graph completeness.
 */
void CalcTremolo(SpcPlayer *p, Channel *c) {
  Not_Implemented();
}

/*
 * Chan_HandleTick — Volume / pan envelope tick for one music channel.
 *
 * Runs after HandleNoteTick / HandlePanAndSweep have settled the per-tick
 * pitch state. Responsible for:
 *
 *   - Volume fade. volume_fade_ticks is a countdown; while non-zero each
 *     tick advances channel_volume toward volume_fade_target via
 *     volume_fade_addpertick (passing `true` for the cont flag lets
 *     Chan_DoAnyFade tick freely without retriggering).
 *
 *   - Tremolo. If configured and past the initial delay window, advances
 *     tremolo_count by tremolo_rate (with a phase-clamp for full-depth
 *     0xff that wraps to 0x80) and falls into CalcTremolo. Otherwise
 *     CalcFinalVolume(p, c, 0xff) is the no-tremolo path that just
 *     produces the steady volume.
 *
 *   - Pan slide. pan_num_ticks countdown does Chan_DoAnyFade on
 *     pan_value toward pan_target_value at pan_add_per_tick units/tick.
 *
 * vol_dirty is a bitmask over the 8 channels — bits OR'd in here trigger
 * the final WriteVolumeToDsp call. Setting it from multiple branches and
 * checking once at the bottom coalesces what would otherwise be 2-3
 * separate DSP writes per channel per tick.
 */
static void Chan_HandleTick(SpcPlayer *p, Channel *c) {
  if (c->volume_fade_ticks) {
    c->volume_fade_ticks--;
    p->vol_dirty |= p->cur_chan_bit;
    Chan_DoAnyFade(&c->channel_volume, c->volume_fade_addpertick, c->volume_fade_target, true);
  }
  if (c->tremolo_depth) {
    if (c->tremolo_delay_ticks == c->tremolo_hold_count) {
      p->vol_dirty |= p->cur_chan_bit;
      if (c->tremolo_count & 0x80 && c->tremolo_depth == 0xff) {
        c->tremolo_count = 0x80;
      } else {
        c->tremolo_count += c->tremolo_rate;
      }
      CalcTremolo(p, c);
    } else {
      c->tremolo_hold_count++;
      CalcFinalVolume(p, c, 0xff);
    }
  } else {
    CalcFinalVolume(p, c, 0xff);
  }

  if (c->pan_num_ticks) {
    c->pan_num_ticks--;
    p->vol_dirty |= p->cur_chan_bit;
    Chan_DoAnyFade(&c->pan_value, c->pan_add_per_tick, c->pan_target_value, true);
  }

  if (p->vol_dirty & p->cur_chan_bit)
    WriteVolumeToDsp(p, c, c->pan_value);
}

/*
 * Port0_HandleMusic — Main music driver tick + SNES port 0 command handler.
 *
 * Port 0 (APUI00) is the music command channel; the CPU writes track-select
 * and control values here. This function reads the latest command, branches
 * on it, and (in the "continue current music" path) runs one full music
 * tick across all 8 channels.
 *
 * Command codes:
 *   0     : "continue" — drive a music tick. Inner state machine reads the
 *           pattern stream, parses notes and effects, and runs the per-tick
 *           handlers. The handle_cmd_00 label is the shared landing spot
 *           from the fade-in / volume-fade variants below.
 *   0xff  : "load new music" — original SNES feature unused by ALttP's
 *           in-game path (handled elsewhere via SpcPlayer_Upload).
 *   0xf0  : "pause music" — keyoff every active channel, clear port_to_snes
 *           so the song is marked stopped. The HandleCmd_0xf0_PauseMusic
 *           label is reused by the volume-fade end path.
 *   0xf1  : "fade out then pause" — set a 0x80-tick master-volume ramp to 0
 *           and arm pause_music_ctr so when the ramp finishes the pause
 *           handler fires. Falls through to handle_cmd_00 so the current
 *           tick still plays while fading.
 *   0xf2  : "duck volume" — backup current master volume hi byte into
 *           byte_3E1 and clamp to 0x70. Used during dialogue / SFX-heavy
 *           moments. Falls through.
 *   0xf3  : "restore volume" — undo 0xf2 by reinstalling byte_3E1.
 *   else  : "start track N" — read the per-track pointer table at 0xD000,
 *           reset accumulators, seed counter_sf0c = 2 so the next two ticks
 *           are setup (Music_ResetChan etc.), and keyoff anything not
 *           currently playing (so a fresh track replaces an old one cleanly).
 *
 * Inside the "continue" path:
 *   - next_phrase label : the top-level phrase walker; reads 16-bit values
 *     from music_ptr_toplevel. Special values 0x80/0x81 toggle fast-forward
 *     (used by sequence-skip), 0 ends the song, and (val >> 8 == 0) values
 *     drive the block-repeat machinery — decrement block_count, branch to
 *     the next 16-bit pointer if non-zero.
 *   - Per-phrase setup: for each of 8 channels load its pattern pointer
 *     from the phrase table, reset subroutine/fade state, and assign the
 *     default instrument if none was set.
 *   - label_a : the per-tick body. For each channel that has a pattern
 *     pointer: if note_ticks_left expires, parse the next byte stream
 *     (note + length + effects), call PlayNote, and arm the gate-off
 *     countdown. Otherwise tick the existing note via HandleNoteTick.
 *     fast_forward skips both PlayNote and HandleNoteTick — only state
 *     advances, no audio is rendered (used for the "skip song intro" fast
 *     scroll past silent intros).
 *   - Global per-tick processing: tempo fade, echo volume fade, master
 *     volume fade. Each is the same countdown-then-snap-to-target pattern.
 *   - Final pass: Chan_HandleTick for each active channel to settle
 *     volume / pan envelopes and emit DSP writes.
 *
 * The kNoteVol[16] and kNoteGateOffPct[8] tables map the duration-encoded
 * note byte's velocity/gate nibbles into actual volume/release values
 * (note-velocity nibble × gate-off-percentage nibble = the per-note dynamics).
 */
static void Port0_HandleMusic(SpcPlayer *p) {
  Channel *c;
  uint8 a = p->new_value_from_snes[0];
  int t;

  if (a == 0) {
handle_cmd_00:
    if (p->port_to_snes[0] == 0)
      return;
    if (p->pause_music_ctr != 0 && --p->pause_music_ctr == 0)
      goto HandleCmd_0xf0_PauseMusic;
    if (p->counter_sf0c == 0)
      goto label_a;
    if (--p->counter_sf0c != 0) {
      Music_ResetChan(p);
      return;
    }
next_phrase:
    for (;;) {
      t = WORD(p->ram[p->music_ptr_toplevel]);
      p->music_ptr_toplevel += 2;
      if ((t >> 8) != 0)
        break;
      if (t == 0)
        goto HandleCmd_0xf0_PauseMusic;
      if (t == 0x80) {
        p->fast_forward = 0x80;
      } else if (t == 0x81) {
        p->fast_forward = 0;
      } else {
        if (sign8(--p->block_count))
          p->block_count = t;
        t = WORD(p->ram[p->music_ptr_toplevel]);
        p->music_ptr_toplevel += 2;
        if (p->block_count != 0)
          p->music_ptr_toplevel = t;
      }
    }
    for (int i = 0; i < 8; i++)
      p->channel[i].pattern_order_ptr_for_chan = WORD(p->ram[t]), t += 2;

    c = p->channel, p->cur_chan_bit = 1;
    do {
      if (HIBYTE(c->pattern_order_ptr_for_chan) && c->instrument_id == 0)
        Channel_SetInstrument(p, c, 0);
      c->subroutine_num_loops = 0;
      c->volume_fade_ticks = 0;
      c->pan_num_ticks = 0;
      c->note_ticks_left = 1;
    } while (c++, p->cur_chan_bit <<= 1);
label_a:
    p->vol_dirty = 0;
    c = p->channel, p->cur_chan_bit = 1;
    do {
      if (!HIBYTE(c->pattern_order_ptr_for_chan))
        continue;
      if (!--c->note_ticks_left) {
        for (;;) {
          uint8 cmd = p->ram[c->pattern_order_ptr_for_chan++];
          if (cmd == 0) {
            if (!c->subroutine_num_loops)
              goto next_phrase;
            c->pattern_order_ptr_for_chan = (--c->subroutine_num_loops == 0) ? c->saved_pattern_ptr : c->pattern_start_ptr;
            continue;
          }
          if (!(cmd & 0x80)) {
            static const uint8 kNoteVol[16] = { 25, 50, 76, 101, 114, 127, 140, 152, 165, 178, 191, 203, 216, 229, 242, 252 };
            static const uint8 kNoteGateOffPct[8] = { 50, 101, 127, 152, 178, 203, 229, 252 };
            c->note_length = cmd;
            cmd = p->ram[c->pattern_order_ptr_for_chan++];
            if (!(cmd & 0x80)) {
              c->note_gate_off_fixedpt = kNoteGateOffPct[cmd >> 4 & 7];
              c->channel_volume_master = kNoteVol[cmd & 0xf];
              cmd = p->ram[c->pattern_order_ptr_for_chan++];
            }
          }
          if (cmd >= 0xe0) {
            HandleEffect(p, c, cmd); 
            continue;
          }
          if (!p->fast_forward && !(p->is_chan_on & p->cur_chan_bit))
            PlayNote(p, c, cmd);
          c->note_ticks_left = c->note_length;
          t = c->note_ticks_left * c->note_gate_off_fixedpt >> 8;
          c->note_keyoff_ticks_left = (t != 0) ? t : 1;
          PitchSlideToNote_Check(p, c);
          break;
        }
      } else if (!p->fast_forward) {
        HandleNoteTick(p, c);
        PitchSlideToNote_Check(p, c);
      }
    } while (c++, p->cur_chan_bit <<= 1);
    if (p->tempo_fade_num_ticks)
      p->tempo = (--p->tempo_fade_num_ticks == 0) ? p->tempo_fade_final << 8 : p->tempo + p->tempo_fade_add;
    if (p->echo_volume_fade_ticks) {
      p->echo_volume_left += p->echo_volume_fade_add_left;
      p->echo_volume_right += p->echo_volume_fade_add_right;
      if (--p->echo_volume_fade_ticks == 0) {
        p->echo_volume_left = p->echo_volume_fade_target_left << 8;
        p->echo_volume_right = p->echo_volume_fade_target_right << 8;
      }
    }
    if (p->master_volume_fade_ticks) {
      p->master_volume = (--p->master_volume_fade_ticks == 0) ? p->master_volume_fade_target << 8 : p->master_volume + p->master_volume_fade_add_per_tick;
      p->vol_dirty = 0xff;
    }
    c = p->channel, p->cur_chan_bit = 1;
    do {
      if (HIBYTE(c->pattern_order_ptr_for_chan))
        Chan_HandleTick(p, c);
    } while (c++, p->cur_chan_bit <<= 1);
  } else if (a == 0xff) {
    // Load new music
    Not_Implemented();
  } else if (a == 0xf1) { // continue music
    p->master_volume_fade_ticks = 0x80;
    p->pause_music_ctr = 0x80;
    p->master_volume_fade_target = 0;
    p->master_volume_fade_add_per_tick = SpcDivHelper(0 - (p->master_volume >> 8), 0x80);

    goto handle_cmd_00;
  } else if (a == 0xf2) {
    if (p->byte_3E1 != 0)
      return;
    p->byte_3E1 = HIBYTE(p->master_volume);
    HIBYTE(p->master_volume) = 0x70;
    goto handle_cmd_00;
  } else if (a == 0xf3) {
    if (p->byte_3E1 == 0)
      return;
    HIBYTE(p->master_volume) = p->byte_3E1;
    p->byte_3E1 = 0;
    goto handle_cmd_00;
  } else if (a == 0xf0) HandleCmd_0xf0_PauseMusic: {
    p->key_OFF = p->is_chan_on ^ 0xff;
    p->port_to_snes[0] = 0;
    p->cur_chan_bit = 0;
  } else {
    p->pause_music_ctr = 0;
    p->byte_3E1 = 0;
    p->port_to_snes[0] = a;
    p->music_ptr_toplevel = WORD(p->ram[0xD000 + (a - 1) * 2]);
    p->counter_sf0c = 2;
    p->key_OFF |= p->is_chan_on ^ 0xff;
  }

}

/*
 * Asl — Arithmetic shift left of a byte in place, returning the old MSB.
 *
 * Names the 65C816 ASL instruction's behavior: shift left and return the
 * value rotated out of bit 7. Used by the Port1/2/3 channel loops to walk
 * the active-channel bitmask one bit at a time — each iteration tests the
 * MSB (the channel that was originally bit 7..0) and shifts the next bit
 * into place. Equivalent to "if (bit & top) { ... } bit <<= 1;" but
 * matches the SNES code's compact ASL+BCS pattern.
 */
static inline uint8 Asl(uint8 *p) {
  uint8 old = *p;
  *p <<= 1;
  return old >> 7;
}

/*
 * Sfx_TurnOffChannel — Release one SFX channel back to music ownership.
 *
 * Called when an SFX stream hits its terminator (0x00). Clears the SFX
 * occupancy bit in every port mask and the global is_chan_on, then
 * reinstalls the channel's *music* instrument so when the music driver
 * next steps on this channel it sounds correct again (the SFX engine
 * may have written different instrument parameters into the DSP).
 *
 * If the channel was supposed to have echo enabled for music
 * (echo_channels bit set) but the SFX had disabled echo on it (reg_EON
 * bit clear), re-enable echo on the DSP and clear the SFX echo-mask
 * tracking bit. This restores the echo routing the music expects.
 */
static void Sfx_TurnOffChannel(SpcPlayer *p, Channel *c) {
  c->sfx_which_sound = 0;
  p->is_chan_on &= ~p->current_bit;
  p->port1_active &= ~p->current_bit;
  p->port2_active &= ~p->current_bit;
  p->port3_active &= ~p->current_bit;
  Channel_SetInstrument(p, c, c->instrument_id);
  if (p->echo_channels & p->current_bit && !(p->reg_EON & p->current_bit)) {
    p->reg_EON |= p->current_bit;
    Dsp_Write(p, EON, p->reg_EON);
    p->sfx_channels_echo_mask2 &= ~p->current_bit;
  }
}

/*
 * Write_KeyOn — Emit a key-on with prior key-off cleared.
 *
 * The DSP's KOF register is a level-sensitive sustain. Writing 0 first
 * is required so a previously-asserted KOF on the same channel doesn't
 * immediately retrigger the release envelope after KON takes effect.
 * The two-write sequence matches the original SPC700 routine.
 */
static void Write_KeyOn(SpcPlayer *p, uint8 bit) {
  Dsp_Write(p, KOF, 0);
  Dsp_Write(p, KON, bit);
}

/*
 * PlayNote — Begin sounding `note` on a channel.
 *
 * Triggered by both the music driver (Port0_HandleMusic) and the SFX
 * driver (Sfx_ChannelTick). Note byte encoding:
 *   < 0xc8        : real note pitch (transposition added below)
 *   0xc8          : tie (handled by caller, never reaches here as a value)
 *   0xc9          : note-off
 *   0xca..0xff    : instrument change — switch patch via Channel_SetInstrument,
 *                   then collapse to 0xa4 (a mid-range note) so the new patch
 *                   is auditioned. Used for "patch + note" combined commands.
 *
 * Notes 0xc8 and 0xc9 are early-returned because they're not real pitches.
 * The same is true if the channel is already actively sounding (the bit
 * in is_chan_on is set) — PlayNote will not interrupt itself.
 *
 * Steady-state setup for a new note:
 *   - Compute the 16-bit pitch as (note + global_transposition +
 *     channel_transposition) << 8 | fine_tune. The shift gives 256
 *     fractional units between semitones for fine tuning / vibrato.
 *   - Reset vibrato phase: vibrato_count = vibrato_fade_num_ticks << 7
 *     so vibrato fades in over its configured ramp. Other vibrato/tremolo
 *     counters zero out.
 *   - Schedule key-on via vol_dirty + key_ON bit masks. The actual KON
 *     register write happens in Spc_Loop_Part1.
 *   - Pitch envelope: if pitch_envelope_num_ticks > 0, the note will
 *     glide. pitch_envelope_direction picks "up to target" (start low,
 *     ramp up) or "down to target" (start at target, ramp from start);
 *     ComputePitchAdd derives the per-tick step size.
 *
 * The commented-out printf blocks are leftover debugging probes from the
 * original C port — kept for context but never executed.
 */
static void PlayNote(SpcPlayer *p, Channel *c, uint8 note) {
  if (note >= 0xca) {
    Channel_SetInstrument(p, c, note);
    note = 0xa4;
  }

//  if (c->index == 0) {
//    if (note == 0xc8) {
//      printf("-+-\n");
//    } else if (note == 0xc9) {
//      printf("---\n");
//    }
//  }

  if (note >= 0xc8 || p->is_chan_on & p->cur_chan_bit)
    return;

  //static const char *const kNoteNames[] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
  //if (c->index==0)
  //  printf("%s%d\n", kNoteNames[(note & 0x7f) % 12], (note & 0x7f) / 12 + 1);

  c->pitch = ((note & 0x7f) + p->global_transposition + c->channel_transposition) << 8 | c->fine_tune;
  c->vibrato_count = c->vibrato_fade_num_ticks << 7;
  c->vibrato_hold_count = 0;
  c->vibrato_change_count = 0;
  c->tremolo_count = 0;
  c->tremolo_hold_count = 0;
  p->vol_dirty |= p->cur_chan_bit;
  p->key_ON |= p->cur_chan_bit;
  c->pitch_slide_length = c->pitch_envelope_num_ticks;
  if (c->pitch_slide_length) {
    c->pitch_slide_delay_left = c->pitch_envelope_delay;
    if (!c->pitch_envelope_direction)
      c->pitch -= c->pitch_envelope_slide_value << 8;
    ComputePitchAdd(c, (c->pitch >> 8) + c->pitch_envelope_slide_value);
  }
  WritePitch(p, c, c->pitch);
}

/*
 * Sfx_MaybeDisableEcho — Strip echo from a channel while SFX uses it.
 *
 * Most SFX sound bad with the music's echo applied (the echo tail blurs
 * impact sounds and sword clinks). Two conditions disable echo for the
 * current channel:
 *   - The music engine has not requested that SFX keep echo on
 *     (port_to_snes[0] & 0x10 clear), OR
 *   - This channel is explicitly tagged in sfx_channels_echo_mask2 as
 *     "echo-suppressed for the duration of this SFX."
 *
 * Uses XOR rather than AND-mask-NOT so the operation is a no-op if the
 * bit is already clear — saves a redundant DSP write.
 */
static void Sfx_MaybeDisableEcho(SpcPlayer *p) {
  if (!(p->port_to_snes[0] & 0x10) || p->current_bit & p->sfx_channels_echo_mask2) {
    if (p->current_bit & p->reg_EON) {
      p->reg_EON ^= p->current_bit;
      Dsp_Write(p, EON, p->reg_EON);
    }
  }
}

/*
 * Sfx_ChannelTick — Parse and step one SFX channel's command stream.
 *
 * The SFX byte-stream format is similar to but simpler than music:
 *   - Length byte (< 0x80) sets sfx_note_length, optionally followed by a
 *     velocity/pan byte (< 0x80). For Port 1 (SFX2 / chord-style SFX with
 *     channels 6+7), the velocity byte directly writes V0VOLL/V0VOLR
 *     instead of going through the volume curve — needed for unisons.
 *     The exception path (`p->channel_67_volume` zero) handles a fade-out
 *     where volume should track the master rather than the SFX command.
 *   - 0x00 terminator: hand the channel back to music via Sfx_TurnOffChannel.
 *   - 0xe0 + idx: load 9-byte patch from 0x3E00 + idx*9 — writes
 *     VOLL/VOLR/PITCHL/PITCHH/SRCN/ADSR1/ADSR2/GAIN to the DSP plus
 *     instrument_pitch_base for downstream Note->pitch conversion.
 *   - 0xf1 / 0xf9: pitch-slide commands. 0xf9 also triggers a fresh PlayNote
 *     + Write_KeyOn first. Both then load delay/length/target and fall
 *     into the note_continue path so the slide starts this tick.
 *   - 0xff: restart sound — re-read sound pointer from the per-port
 *     table at 0x17C0 (port 1) so looping SFX (siren-style) repeat.
 *   - any other (>= 0x80): real note byte. PlayNote, key on, arm the
 *     note-length countdown.
 *
 * The note_continue label is the shared post-note path that advances any
 * pitch slide already in flight. p->cur_chan_bit = 0 there is a hack that
 * forces WritePitch to always actually write (it checks cur_chan_bit
 * against is_chan_on as a "still playing" gate).
 *
 * @param is_continue  true when re-entering the same SFX command
 *                     mid-execution (continued note tick); false when
 *                     starting a fresh SFX from scratch.
 */
static void Sfx_ChannelTick(SpcPlayer *p, Channel *c, bool is_continue) {
  uint8 cmd;

  if (is_continue) {
    Sfx_MaybeDisableEcho(p);
    p->sfx_channel_index = c->index * 2;
    p->sfx_sound_ptr_cur = c->sfx_sound_ptr;
    if (--c->sfx_note_length_left)
      goto note_continue;
    p->sfx_sound_ptr_cur++;
  }

  for (;;) {
    p->dsp_register_index = p->sfx_channel_index * 8;

    cmd = p->ram[p->sfx_sound_ptr_cur];
    if (cmd == 0) {
      Sfx_TurnOffChannel(p, c);
      return;
    }

    if (!(cmd & 0x80)) {
      c->sfx_note_length = cmd;
      cmd = p->ram[++p->sfx_sound_ptr_cur];
      if (!(cmd & 0x80)) {
        if (p->port1_active & p->current_bit) {
          if (cmd == 0 || !p->channel_67_volume) {
            uint8 volume = cmd;
            Dsp_Write(p, p->dsp_register_index + V0VOLL, cmd);
            cmd = p->ram[++p->sfx_sound_ptr_cur];
            if (cmd & 0x80) {
              Dsp_Write(p, p->dsp_register_index + V0VOLR, volume);
            } else {
              Dsp_Write(p, p->dsp_register_index + V0VOLR, cmd);
              cmd = p->ram[++p->sfx_sound_ptr_cur];
            }
          } else {
            cmd = p->ram[++p->sfx_sound_ptr_cur];
          }
        } else {
          c->final_volume = cmd * 2;
          c->pan_flag_with_phase_invert = 10;
          WriteVolumeToDsp(p, c, (p->sfx_start_arg_pan & 0x80 ? 16 : p->sfx_start_arg_pan & 0x40 ? 4 : 10) << 8);
          cmd = p->ram[++p->sfx_sound_ptr_cur];
        }
      }
    }
    // cmd_parsed
    if (cmd == 0xe0) {
      const uint8 *ip = p->ram + 0x3E00 + (p->ram[++p->sfx_sound_ptr_cur] * 9);
      uint8 reg = c->index * 16;
      Dsp_Write(p, reg + V0VOLL, ip[0]);
      Dsp_Write(p, reg + V0VOLR, ip[1]);
      Dsp_Write(p, reg + V0PITCHL, ip[2]);
      Dsp_Write(p, reg + V0PITCHH, ip[3]);
      Dsp_Write(p, reg + V0SRCN, ip[4]);
      Dsp_Write(p, reg + V0ADSR1, ip[5]);
      Dsp_Write(p, reg + V0ADSR2, ip[6]);
      Dsp_Write(p, reg + V0GAIN, ip[7]);
      c->instrument_pitch_base = ip[8] << 8;
      p->sfx_sound_ptr_cur++;
    } else if (cmd == 0xf9 || cmd == 0xf1) {
      if (cmd == 0xf9) {
        PlayNote(p, c, p->ram[++p->sfx_sound_ptr_cur]);
        Write_KeyOn(p, p->current_bit);
      }
      c->pitch_slide_delay_left = p->ram[++p->sfx_sound_ptr_cur];
      c->pitch_slide_length = p->ram[++p->sfx_sound_ptr_cur];
      ComputePitchAdd(c, p->ram[++p->sfx_sound_ptr_cur]);
      c->sfx_note_length_left = c->sfx_note_length;
      goto note_continue;
    } else if (cmd == 0xff) {
      p->sfx_sound_ptr_cur = c->sfx_sound_ptr = WORD(p->ram[0x17C0 + (c->sfx_which_sound - 1) * 2]);
    } else {
      PlayNote(p, c, cmd);
      Write_KeyOn(p, p->current_bit);
      c->sfx_note_length_left = c->sfx_note_length;
note_continue:
      p->did_affect_volumepitch_flag = 0;
      if (c->pitch_slide_length) {
        p->did_affect_volumepitch_flag = 0x80;
        Chan_DoAnyFade(&c->pitch, c->pitch_add_per_tick, c->pitch_target, --c->pitch_slide_length);
        p->cur_chan_bit = 0;  // force change through
        WritePitch(p, c, c->pitch);
      } else if (c->sfx_note_length_left == 2) {
        Dsp_Write(p, KOF, p->current_bit);
      }
      break;
    }
  }
  c->sfx_sound_ptr = p->sfx_sound_ptr_cur;
}

/*
 * Port1_Play_Inner — Trigger a 2-channel "chord" SFX on channels 6 and 7.
 *
 * Port 1 (APUI01) sends SFX index N. These sounds are intentionally
 * coupled — they always play on the high-priority slot pair (channel 7
 * primary + optional channel 6 partner) so the rest of the channel mix
 * is unaffected. The pairing is used for two-voice unison sounds like
 * Link's sword slash or text "blip" pairs that need to sound thicker
 * than a single-channel SFX.
 *
 * Layout:
 *   - 0x1800 + N - 1  : partner-sound table. If non-zero, allocate
 *                       channel 6 with that companion sound's index. If
 *                       zero, only channel 7 plays (single-voice SFX).
 *   - port1_active    : bitmask of which of the two channels are now
 *                       owned by Port 1 (0x80, 0x40, or 0xc0).
 *   - sfx_arr_countdown = 3 : 3-tick delay before the first command-stream
 *                       parse, matching the original timing.
 *
 * sfx_channels_echo_mask2 is OR'd with 0xc0 so the echo router knows to
 * suppress echo on both channels while they're playing this SFX; ports
 * 2 and 3 active masks are AND'd with 0x3f to surrender any prior
 * occupancy on channels 6/7 they may have had.
 */
static void Port1_Play_Inner(SpcPlayer *p) {
  p->port1_counter = 0;
  Channel *c = &p->channel[7];
  c->sfx_which_sound = p->new_value_from_snes[1];
  c->sfx_arr_countdown = 3;
  c->pitch_envelope_num_ticks = 0;
  p->port1_active = 0x80;
  p->is_chan_on |= 0x80;
  Dsp_Write(p, KOF, 0x80);
  p->new_value_from_snes[1] = p->ram[0x1800 + p->new_value_from_snes[1] - 1];
  if (!p->new_value_from_snes[1])
    return;
  c--;
  c->sfx_which_sound = p->new_value_from_snes[1];
  c->sfx_arr_countdown = 3;
  c->pitch_envelope_num_ticks = 0;
  p->port1_active = 0x40;
  p->is_chan_on |= 0x40;
  Dsp_Write(p, KOF, 0x40);
  p->port1_active = 0xc0;
  p->sfx_channels_echo_mask2 |= 0xc0;
  p->port2_active &= 0x3f;
  p->port3_active &= 0x3f;
}

/*
 * Port1_StartNewSound — Per-tick service routine for Port 1 SFX.
 *
 * Two phases:
 *   1. port1_counter fade-out. If non-zero, the channel is in its fade-out
 *      window. Each tick the volume on both channels 6 and 7 drops by 1
 *      (counter >> 1). When it reaches zero, force-launch the "fade
 *      complete" sentinel sound (index 5) so the channel transitions
 *      cleanly back to silence before being returned to music.
 *   2. Active SFX tick. Walk channels 7 → 6 (port1_current_bit shifts
 *      down via Asl). For each set bit, advance the per-channel SFX
 *      command stream via Sfx_ChannelTick. The sfx_arr_countdown gates
 *      the initial 3-tick warmup period before the first stream byte
 *      is parsed (during which sfx_sound_ptr is fetched from 0x17C0).
 *
 * The do/while termination `p->current_bit >>= 1) != 0x10` stops the
 * loop after channels 7 and 6 — Port 1 only uses those two slots.
 */
static void Port1_StartNewSound(SpcPlayer *p) {
  if (p->port1_counter != 0) {
    if (--p->port1_counter == 0) {
      p->new_value_from_snes[1] = 5;
      Port1_Play_Inner(p);
      p->new_value_from_snes[1] = 0;
      return;
    }
    p->channel_67_volume = p->port1_counter >> 1;
    Dsp_Write(p, V7VOLL, p->channel_67_volume);
    Dsp_Write(p, V7VOLR, p->channel_67_volume);
    Dsp_Write(p, V6VOLL, p->channel_67_volume);
    Dsp_Write(p, V6VOLR, p->channel_67_volume);
  }
  p->port1_current_bit = p->port1_active;
  if (!p->port1_current_bit)
    return;
  Channel *c = &p->channel[7];
  p->current_bit = 0x80;
  do {
    if (Asl(&p->port1_current_bit)) {
      p->sfx_channel_index = c->index * 2;
      p->dsp_register_index = c->index * 16;
      p->sfx_start_arg_pan = c->sfx_pan;
      if (!c->sfx_arr_countdown) {
        if (c->sfx_which_sound)
          Sfx_ChannelTick(p, c, true);
      } else {
        p->sfx_channel_index = c->index * 2;
        if (!--c->sfx_arr_countdown) {
          p->sfx_sound_ptr_cur = c->sfx_sound_ptr = WORD(p->ram[0x17C0 + (c->sfx_which_sound - 1) * 2]);
          Sfx_ChannelTick(p, c, false);
        }
      }
    }
  } while (c--, (p->current_bit >>= 1) != 0x10);
}

/*
 * Port1_HandleCmd — Decode an incoming Port 1 command from the SNES CPU.
 *
 * Command byte semantics:
 *   - High bit clear, non-zero : start SFX of that index. The "a == 5"
 *     special case is the fade-complete sentinel — it only launches if
 *     port1_active is non-zero (i.e. there's a SFX in flight that needs
 *     to terminate cleanly); otherwise it's silently dropped to avoid
 *     a spurious blip when nothing is playing.
 *   - High bit set            : "begin fade-out" — start the 0x78-tick
 *     volume fade in Port1_StartNewSound. Only triggers if port1_active
 *     is non-zero so it can't kick off a fade with no source.
 *
 * port_to_snes[1] is echoed back to APU port 1 so the CPU can see that
 * the request was accepted and which sound is current.
 */
static void Port1_HandleCmd(SpcPlayer *p) {
  uint8 a = p->new_value_from_snes[1];
  if (!(a & 0x80)) {
    if (a != 0) {
      p->port_to_snes[1] = a;
      if (a != 5 || p->port1_active)
        Port1_Play_Inner(p);
    }
  } else {
    p->port_to_snes[1] = a;
    if (p->port1_active)
      p->port1_counter = 0x78;
  }
}

/*
 * Port2_StartNewSound — Per-tick service for Port 2 SFX (channels 0..7).
 *
 * Port 2 (APUI02) is the general-purpose SFX bus, accepting up to 8
 * concurrent sounds spread across any free channels. Unlike Port 1, the
 * iteration walks all 8 channels (loop terminates when current_bit
 * shifts to 0). Otherwise identical pattern: Asl picks the active bit,
 * sfx_arr_countdown gates the 3-tick warmup, and on first parse the
 * per-port pointer table at 0x1820 is consulted.
 */
static void Port2_StartNewSound(SpcPlayer *p) {
  p->port2_current_bit = p->port2_active;
  if (!p->port2_current_bit)
    return;
  Channel *c = &p->channel[7];
  p->current_bit = 0x80;
  do {
    if (Asl(&p->port2_current_bit)) {
      p->sfx_channel_index = c->index * 2;
      p->dsp_register_index = c->index * 16;
      p->sfx_start_arg_pan = c->sfx_pan;
      if (!c->sfx_arr_countdown) {
        if (c->sfx_which_sound)
          Sfx_ChannelTick(p, c, true);
      } else {
        p->sfx_channel_index = c->index * 2;
        if (!--c->sfx_arr_countdown) {
          p->sfx_sound_ptr_cur = c->sfx_sound_ptr = WORD(p->ram[0x1820 + (c->sfx_which_sound - 1) * 2]);
          Sfx_ChannelTick(p, c, false);
        }
      }
    }
  } while (c--, p->current_bit >>= 1);
}

/*
 * Port2_AllocateChan — Pick a channel for a new Port 2 SFX request.
 *
 * Two-pass channel allocator walked high-to-low (channel 7 → 0) so
 * higher-numbered channels are preferred (they're typically idle while
 * music plays on the low channels).
 *
 * Pass 1 — "Replace same-SFX": if a channel is already playing this exact
 *   sound+pan combo, re-use it (restart). This prevents fast-fire SFX
 *   (e.g. arrow shoots) from stacking up.
 * Pass 2 — "Take an idle channel": pick the first channel not in is_chan_on.
 *
 * The assert(0) is dead code in practice — Port2_HandleCmd's outer loop
 * already gates on `p->is_chan_on != 0xff` so we're guaranteed at least
 * one idle channel.
 *
 * The 0x18dd table provides the per-SFX "this sound wants echo" flag
 * which controls whether the channel keeps echo enabled after allocation.
 */
static Channel *Port2_AllocateChan(SpcPlayer *p) {
  p->sfx_play_echo_flag = p->ram[0x18dd + (p->new_value_from_snes[2] & 0x3f) - 1];
  Channel *c = &p->channel[7];
  p->current_bit = 0x80;
  do {
    if (p->port2_active & p->current_bit && c->sfx_which_sound + c->sfx_pan == p->new_value_from_snes[2])
      goto found_channel;
  } while (c--, p->current_bit >>= 1);
  c = &p->channel[7];
  p->current_bit = 0x80;
  do {
    if (!(p->is_chan_on & p->current_bit))
      goto found_channel;
  } while (c--, p->current_bit >>= 1);
  assert(0);  // unreachable
found_channel:
  p->sfx_channel_index2 = p->sfx_channel_index = c->index * 2;
  p->sfx_channel_bit = p->current_bit;
  p->is_chan_on |= p->current_bit;
  if (p->sfx_play_echo_flag)
    p->sfx_channels_echo_mask2 |= p->current_bit;
  Sfx_MaybeDisableEcho(p);
  return c;
}

/*
 * Port2_HandleCmd — Spawn one or more SFX from the Port 2 command byte.
 *
 * Command byte encoding:
 *   bits 0..5 : SFX index (1..63)
 *   bits 6..7 : pan select (00 center, 01 right, 10 left, 11 both)
 *
 * Loop semantics: a single command can spawn a chain of related sounds.
 * After spawning, the routine reads the chain-next index from
 *   0x189e + sfx_which_sound - 1
 * If non-zero, allocate another channel and spawn that one too. The
 * chain ends when the next-index byte is zero or all 8 channels are
 * busy. This implements multi-voice SFX (e.g. boss roars composed of
 * a fundamental + harmonics) without bloating Port 2's bandwidth.
 *
 * KOF write at the end of each iteration releases any held envelope on
 * the newly-allocated channel so the next note starts cleanly. The
 * actual note will be issued from Sfx_ChannelTick on the next tick.
 */
static void Port2_HandleCmd(SpcPlayer *p) {
  while (p->new_value_from_snes[2] != 0 && p->is_chan_on != 0xff) {
    Channel *c = Port2_AllocateChan(p);
    c->sfx_pan = p->new_value_from_snes[2] & 0xc0;
    c->sfx_which_sound = p->new_value_from_snes[2] & 0x3f;
    c->sfx_arr_countdown = 3;
    c->pitch_envelope_num_ticks = 0;
    p->port2_active |= p->current_bit;
    Dsp_Write(p, KOF, p->current_bit);
    p->new_value_from_snes[2] = p->ram[0x189e + c->sfx_which_sound - 1];
  }
}


/*
 * Port3_StartNewSound — Per-tick service for Port 3 SFX (ambient/world).
 *
 * Port 3 (APUI03) is a second SFX bus used in parallel with Port 2; ALttP
 * uses it primarily for environment ambience (rain, fire, waterfalls)
 * which need to play simultaneously with discrete event SFX on Port 2.
 * Identical structure to Port2_StartNewSound, the only difference being
 * the per-port pointer table at 0x191C (vs Port 2's 0x1820).
 */
static void Port3_StartNewSound(SpcPlayer *p) {
  p->port3_current_bit = p->port3_active;
  if (!p->port3_current_bit)
    return;
  Channel *c = &p->channel[7];
  p->current_bit = 0x80;
  do {
    if (Asl(&p->port3_current_bit)) {
      p->sfx_channel_index = c->index * 2;
      p->dsp_register_index = c->index * 16;
      p->sfx_start_arg_pan = c->sfx_pan;
      if (!c->sfx_arr_countdown) {
        if (c->sfx_which_sound)
          Sfx_ChannelTick(p, c, true);
      } else {
        p->sfx_channel_index = c->index * 2;
        if (!--c->sfx_arr_countdown) {
          p->sfx_sound_ptr_cur = c->sfx_sound_ptr = WORD(p->ram[0x191C + (c->sfx_which_sound - 1) * 2]);
          Sfx_ChannelTick(p, c, false);
        }
      }
    }
  } while (c--, p->current_bit >>= 1);
}

/*
 * Port3_AllocateChan — Channel allocator for a Port 3 SFX request.
 *
 * Mirrors Port2_AllocateChan exactly except the echo-flag table is at
 * 0x19d8 (no -1 bias — different table format than Port 2's at 0x18dd).
 * Same two-pass allocation: try to reclaim a channel already running the
 * same sound+pan, then fall back to the first idle channel.
 */
static Channel *Port3_AllocateChan(SpcPlayer *p) {
  p->sfx_play_echo_flag = p->ram[0x19d8 + (p->new_value_from_snes[3] & 0x3f)];
  Channel *c = &p->channel[7];
  p->current_bit = 0x80;
  do {
    if (p->port3_active & p->current_bit && c->sfx_which_sound + c->sfx_pan == p->new_value_from_snes[3])
      goto found_channel;
  } while (c--, p->current_bit >>= 1);
  c = &p->channel[7];
  p->current_bit = 0x80;
  do {
    if (!(p->is_chan_on & p->current_bit))
      goto found_channel;
  } while (c--, p->current_bit >>= 1);
  assert(0);  // unreachable
found_channel:
  p->sfx_channel_index2 = p->sfx_channel_index = c->index * 2;
  p->sfx_channel_bit = p->current_bit;
  p->is_chan_on |= p->current_bit;
  if (p->sfx_play_echo_flag)
    p->sfx_channels_echo_mask2 |= p->current_bit;
  Sfx_MaybeDisableEcho(p);
  return c;
}

/*
 * Port3_HandleCmd — Spawn one or more SFX from the Port 3 command byte.
 *
 * Same encoding as Port 2 (low 6 bits = index, top 2 bits = pan) and
 * same chain-of-sounds pattern. The chain-next table for Port 3 lives
 * at 0x199a. Loop terminates when the next index is 0 or all channels
 * are occupied.
 */
static void Port3_HandleCmd(SpcPlayer *p) {
  while (p->new_value_from_snes[3] != 0 && p->is_chan_on != 0xff) {
    Channel *c = Port3_AllocateChan(p);
    c->sfx_pan = p->new_value_from_snes[3] & 0xc0;
    c->sfx_which_sound = p->new_value_from_snes[3] & 0x3f;
    c->sfx_arr_countdown = 3;
    c->pitch_envelope_num_ticks = 0;
    p->port3_active |= p->current_bit;
    Dsp_Write(p, KOF, p->current_bit);
    p->new_value_from_snes[3] = p->ram[0x199a + c->sfx_which_sound - 1];
  }
}

/*
 * ReadPortFromSnes — Edge-detect a CPU port write so commands fire once.
 *
 * The SNES CPU writes to APU ports asynchronously and the SPC700 firmware
 * must only act when the value *changes* — otherwise a value that sits
 * latched in input_ports[N] would retrigger every tick. This routine
 * captures the new value into new_value_from_snes[port] only on edges
 * (input_ports != last_value), and clears it to 0 on no-change ticks
 * so the per-port handlers see "no command this tick" as the default.
 */
static void ReadPortFromSnes(SpcPlayer *p, int port) {
  uint8 old = p->last_value_from_snes[port];
  p->last_value_from_snes[port] = p->input_ports[port];
  if (p->input_ports[port] != old)
    p->new_value_from_snes[port] = p->input_ports[port];
  else
    p->new_value_from_snes[port] = 0;
}

/*
 * Spc_Loop_Part1 — Per-tick DSP register flush ("write side" of the loop).
 *
 * Pushes accumulated key-on / key-off / global DSP state to the DSP at
 * the start of every audio tick. Ordering is meaningful:
 *
 *   1. KOF = key_OFF  : assert release on channels that ended last tick.
 *   2. PMON / NON     : pitch-modulation and noise enable masks (only
 *                       matter on channels where they changed).
 *   3. KOF = 0        : clear KOF before KON, otherwise the new note
 *                       would immediately go into release.
 *   4. KON = key_ON   : start all new notes scheduled this tick.
 *
 * Echo block: written only if echo_stored_time hasn't set the MSB sentinel
 * (which marks "echo not yet ready / disabled"). FLG carries the echo /
 * mute / noise-clock state. The inner `if` waits until echo_stored_time
 * equals echo_parameter_EDL — meaning the echo delay-line buffer has
 * filled enough that programmable EON/EFB/EVOL writes will produce
 * meaningful audio rather than feeding silence back into the FIR filter.
 *
 * key_OFF/key_ON cleared at the end so the next tick starts fresh.
 */
static void Spc_Loop_Part1(SpcPlayer *p) {
  Dsp_Write(p, KOF, p->key_OFF);
  Dsp_Write(p, PMON, p->reg_PMON);
  Dsp_Write(p, NON, p->reg_NON);
  Dsp_Write(p, KOF, 0);
  Dsp_Write(p, KON, p->key_ON);
  if (!(p->echo_stored_time & 0x80)) {
    Dsp_Write(p, FLG, p->reg_FLG);
    if (p->echo_stored_time == p->echo_parameter_EDL) {
      Dsp_Write(p, EON, p->reg_EON);
      Dsp_Write(p, EFB, p->reg_EFB);
      Dsp_Write(p, EVOLR, p->echo_volume_right >> 8);
      Dsp_Write(p, EVOLL, p->echo_volume_left >> 8);
    }
  }
  p->key_OFF = p->key_ON = 0;
}

/*
 * Spc_Loop_Part2 — Per-tick state advance ("logic side" of the loop).
 *
 * Drives all four ports plus music tempo. Two independent accumulators
 * gate "is this tick a SFX/music tick yet?":
 *
 *   sfx_timer_accum  += ticks * 0x38 — when it overflows past 256, run
 *     one full SFX cycle (Port 1 → 2 → 3 in priority order, each
 *     followed by ReadPortFromSnes to re-arm edge detection for next time).
 *     The 0x38 rate gives roughly 56/256 ≈ 22% per SPC tick so SFX
 *     advance at a fraction of the audio rate.
 *
 *   main_tempo_accum += ticks * (tempo >> 8) — when it overflows, run a
 *     music tick (Port0_HandleMusic) and re-arm Port 0 edge detection.
 *     The tempo can be faded via tempo_fade_* so this rate is dynamic.
 *
 *   else branch — if tempo hasn't overflowed but a song is playing,
 *     run HandlePanAndSweep on each active channel so pan/pitch/vibrato
 *     interpolate smoothly between full music ticks (this is what
 *     produces the audibly-fluid vibrato instead of staircase steps).
 *
 * Inside the SFX path, the echo_stored_time progression
 *   `(++echo_fract_incr & 1) == 0 → echo_stored_time++`
 * advances at half the SFX tick rate, slowly walking
 * echo_stored_time up to echo_parameter_EDL so the echo buffer
 * primes correctly after a song change.
 *
 * @param ticks  Number of SPC ticks that have elapsed since the last
 *               call (driven by SpcPlayer_GenerateSamples's >> 6 counter).
 */
static void Spc_Loop_Part2(SpcPlayer *p, uint8 ticks) {
  int t = p->sfx_timer_accum + (uint8)(ticks * 0x38);
  p->sfx_timer_accum = t;
  if (t >= 256) {
    Port1_StartNewSound(p);
    Port1_HandleCmd(p);
    ReadPortFromSnes(p, 1);

    Port2_StartNewSound(p);
    Port2_HandleCmd(p);
    ReadPortFromSnes(p, 2);

    Port3_StartNewSound(p);
    Port3_HandleCmd(p);
    ReadPortFromSnes(p, 3);

    if (p->echo_stored_time != p->echo_parameter_EDL && !(++p->echo_fract_incr & 1))
      p->echo_stored_time++;
  }

  t = p->main_tempo_accum + (uint8)(ticks * HIBYTE(p->tempo));
  p->main_tempo_accum = t;
  if (t >= 256) {
    Port0_HandleMusic(p);
    ReadPortFromSnes(p, 0);
  } else if (p->port_to_snes[0]) {
    Channel *c = p->channel;
    for (p->cur_chan_bit = 1; p->cur_chan_bit != 0; p->cur_chan_bit <<= 1, c++) {
      if (HIBYTE(c->pattern_order_ptr_for_chan))
        HandlePanAndSweep(p, c);
    }
  }
}

/*
 * Interrupt_Reset — Reset the SPC player to the initial post-boot state.
 *
 * Modeled on the SPC700 firmware's reset/boot routine that fires when
 * the SNES first hands control to the APU. Steps:
 *   - dsp_reset clears all 128 DSP registers and the voice state.
 *   - memset zeros the "live" portion of SpcPlayer past
 *     new_value_from_snes. The fields before that (the dsp pointer, ram
 *     buffer, history hooks) survive because they're external resources
 *     that don't belong in the reset.
 *   - Tag each Channel's index so later code can use c->index without
 *     pointer arithmetic from p->channel.
 *   - SetupEchoParameter_EDL(p, 1) allocates a minimal one-block echo
 *     buffer so the FLG.EFEN bit is meaningful even before the song
 *     overrides it.
 *   - reg_FLG |= 0x20 sets the echo-disable bit (so reset state is silent
 *     until music explicitly turns echo back on).
 *   - MVOLL/MVOLR = 0x60 is a moderate master volume — about 75% of
 *     full-scale; matches the SNES boot ROM default.
 *   - DIR = 0x3c points the sample directory page at $3C00, the
 *     conventional ALttP sample table location.
 *   - tempo hi = 16 is a default that will be overwritten by the first
 *     song's tempo command (kept as a sane fallback for the silent gap).
 */
static void Interrupt_Reset(SpcPlayer *p) {
  dsp_reset(p->dsp);

  memset(&p->new_value_from_snes, 0, sizeof(SpcPlayer) - offsetof(SpcPlayer, new_value_from_snes));
  for (int i = 0; i < 8; i++)
    p->channel[i].index = i;
  SetupEchoParameter_EDL(p, 1);
  p->reg_FLG |= 0x20;
  Dsp_Write(p, MVOLL, 0x60);
  Dsp_Write(p, MVOLR, 0x60);
  Dsp_Write(p, DIR, 0x3c);
  HIBYTE(p->tempo) = 16;
  p->timer_cycles = 0;
}

/*
 * SpcPlayer_Create — Heap-allocate and minimally initialize an SpcPlayer.
 *
 * Called once at engine startup. The SpcPlayer struct contains the 64KB
 * APU RAM image embedded inline so we malloc the full thing in one shot.
 * dsp_init binds the DSP to that same RAM buffer (so DSP voice fetches
 * read directly from p->ram[]). reg_write_history is nulled because the
 * production build doesn't capture writes; the debug build sets it later.
 *
 * @return  Owned SpcPlayer pointer. Caller must SpcPlayer_Initialize
 *          before generating any samples.
 */
SpcPlayer *SpcPlayer_Create() {
  SpcPlayer *p = (SpcPlayer *)malloc(sizeof(SpcPlayer));
  p->dsp = dsp_init(p->ram);
  p->reg_write_history = 0;
  return p;
}

/*
 * SpcPlayer_Initialize — Drive a full reset + first DSP flush.
 *
 * Called once after creation and again whenever the engine wants a clean
 * audio slate (e.g. before loading a new SPC dump). Spc_Loop_Part1 here
 * isn't optional — it pushes the reset's MVOLL/MVOLR/FLG values into the
 * DSP so the very first generated sample isn't full-volume garbage.
 */
void SpcPlayer_Initialize(SpcPlayer *p) {
  Interrupt_Reset(p);
  Spc_Loop_Part1(p);
}

/*
 * SpcPlayer_CopyVariablesToRam — Marshal C struct state back into APU RAM.
 *
 * The original ALttP SPC700 firmware kept all driver state inside the 64KB
 * APU RAM at fixed addresses. This C port mirrors that state into struct
 * fields for cache locality and debugger friendliness, but save-states and
 * the SPC-compare debugging path need the RAM layout to match the
 * original byte-for-byte. This routine walks the kChannel_Maps and
 * kSpcPlayer_Maps descriptor arrays and writes each field back to its
 * canonical APU RAM offset.
 *
 * kChannel_Maps describes per-channel fields: each is offset by i*2 from
 * the table's base for channel i. The high bit of org_off (0x8000) marks
 * a 16-bit field (so memcpy uses size 2); otherwise 1 byte.
 *
 * kSpcPlayer_Maps describes player-wide globals at fixed RAM offsets,
 * including explicit sizes for fields that aren't 1 or 2 bytes.
 */
void SpcPlayer_CopyVariablesToRam(SpcPlayer *p) {
  Channel *c = p->channel;
  for (int i = 0; i < 8; i++, c++) {
    for (const MemMap *m = &kChannel_Maps[0]; m != &kChannel_Maps[countof(kChannel_Maps)]; m++)
      memcpy(&p->ram[(m->org_off & 0x7fff) + i * 2], (uint8 *)c + m->off, m->org_off & 0x8000 ? 2 : 1);
  }
  for (const MemMapSized *m = &kSpcPlayer_Maps[0]; m != &kSpcPlayer_Maps[countof(kSpcPlayer_Maps)]; m++)
    memcpy(&p->ram[m->org_off], (uint8 *)p + m->off, m->size);
}

/*
 * SpcPlayer_CopyVariablesFromRam — Reverse of CopyVariablesToRam.
 *
 * Called after loading a save-state or an SPC dump: the APU RAM is the
 * authoritative source, so each mapped C field is re-read from its
 * canonical offset and copied back into the struct. Same iteration
 * pattern: per-channel fields with i*2 stride, then global fields with
 * explicit sizes.
 */
void SpcPlayer_CopyVariablesFromRam(SpcPlayer *p) {
  Channel *c = p->channel;
  for (int i = 0; i < 8; i++, c++) {
    for (const MemMap *m = &kChannel_Maps[0]; m != &kChannel_Maps[countof(kChannel_Maps)]; m++)
      memcpy((uint8 *)c + m->off, &p->ram[(m->org_off & 0x7fff) + i * 2], m->org_off & 0x8000 ? 2 : 1);
  }
  for (const MemMapSized *m = &kSpcPlayer_Maps[0]; m != &kSpcPlayer_Maps[countof(kSpcPlayer_Maps)]; m++)
    memcpy((uint8 *)p + m->off, &p->ram[m->org_off], m->size);
}


/*
 * SpcPlayer_GenerateSamples — Render one frame's worth (534 samples) of audio.
 *
 * The main audio driver entry point — called once per video frame (60Hz)
 * by the platform's audio pump (e.g. main.c's AudioCallback feeder).
 * Produces exactly 534 samples at 32kHz, which is enough for one NTSC
 * frame's worth of audio (32000/60 ≈ 533.3, rounded up; the audio mixer
 * then resamples to the host's actual output rate).
 *
 * Inner loop:
 *   - When timer_cycles ≥ 64, one full SPC700 timer-2 period has elapsed.
 *     Run Spc_Loop_Part2 (logic side, advances music/SFX state) followed
 *     by Spc_Loop_Part1 (DSP register flush). The `>> 6` shift is the
 *     timer-2 prescaler: every 64 DSP samples = 1 driver tick.
 *   - Compute how many samples can run before either the frame is full
 *     (534) or another timer tick is due (64 - timer_cycles).
 *   - Step the DSP that many samples via dsp_cycle.
 *   - Break out when the DSP's output buffer is full.
 *
 * The two assertions bound the per-call work to keep the audio thread
 * responsive — neither should fire under normal conditions.
 */
void SpcPlayer_GenerateSamples(SpcPlayer *p) {
  assert(p->timer_cycles <= 64);

  assert(p->dsp->sampleOffset <= 534);

  for (;;) {
    if (p->timer_cycles >= 64) {
      Spc_Loop_Part2(p, p->timer_cycles >> 6);
      Spc_Loop_Part1(p);
      p->timer_cycles &= 63;
    }

    // sample rate 32000
    int n = 534 - p->dsp->sampleOffset;
    if (n > (64 - p->timer_cycles))
      n = (64 - p->timer_cycles);

    p->timer_cycles += n;

    for (int i = 0; i < n; i++)
      dsp_cycle(p->dsp);

    if (p->dsp->sampleOffset == 534)
      break;
  }
}

/*
 * SpcPlayer_Upload — Stream a packet of sample/code data into APU RAM.
 *
 * Used to load the music bank (samples, patches, song pointers, driver
 * code) into the emulated APU's 64KB RAM. The packet format is the same
 * "chunk header + raw bytes" layout that the SNES boot ROM uses for IPL
 * transfers:
 *   uint16 numbytes      — bytes in this chunk (0 = end of stream)
 *   uint16 target        — destination APU RAM address
 *   uint8  data[numbytes]
 *
 * Before copying, three mute writes silence everything currently playing:
 *   EVOLL/EVOLR = 0  : kill echo output instantly
 *   KOF = 0xff       : release every channel
 * This prevents click/screech when the song-banks samples are overwritten
 * mid-playback.
 *
 * After the upload, all per-port state is zeroed so the next song-start
 * command on Port 0 / Port 1 / Port 2 / Port 3 starts from a clean slate.
 * `target & 0xffff` wraps writes that would go past the 64KB boundary,
 * matching the SPC700's 16-bit address wrap behavior.
 */
void SpcPlayer_Upload(SpcPlayer *p, const uint8_t *data) {
  Dsp_Write(p, EVOLL, 0);
  Dsp_Write(p, EVOLR, 0);
  Dsp_Write(p, KOF, 0xff);

  for (;;) {
    int numbytes = *(uint16 *)(data);
    if (numbytes == 0)
      break;
    int target = *(uint16 *)(data + 2);
    data += 4;
    do {
      p->ram[target++ & 0xffff] = *data++;
    } while (--numbytes);
  }
  p->pause_music_ctr = 0;
  p->port_to_snes[0] = 0;
  p->port1_active = 0;
  p->port2_active = 0;
  p->port3_active = 0;
  p->is_chan_on = 0;
  p->input_ports[0] = p->input_ports[1] = p->input_ports[2] = p->input_ports[3] = 0;
}

// =============================================================================
// Optional debug / verification harness — compares this C implementation
// against the reference Apu+Spc emulator (snes/apu.c) on a tick-by-tick basis.
// Build with WITH_SPC_PLAYER_DEBUGGING set to 1 to enable; production builds
// strip this section out entirely.
// =============================================================================
#define WITH_SPC_PLAYER_DEBUGGING 0

#if WITH_SPC_PLAYER_DEBUGGING

#include <SDL.h>

// Per-run scratch state for the debug harness. Globals (rather than locals)
// because their addresses must stay stable across the apu_cycle loop's
// many entry points into CompareSpcImpls.
static DspRegWriteHistory my_write_hist;  // Captures every Dsp_Write call
static SpcPlayer my_spc, my_spc_snapshot; // Live + rollback copies of our state
static int loop_ctr;                      // Tick counter, used as a probe label

/*
 * CompareSpcImpls — Diff our state vs the reference emulator's state.
 *
 * After each driver-tick boundary the harness calls this routine with:
 *   p     : our SpcPlayer after running this tick
 *   p_org : snapshot of our state before the tick (for diff annotation)
 *   apu   : the reference Apu, which independently ran the same tick
 *
 * Prep: copy a few scratch RAM regions (LFSR seed, stack, DSP regs, temp
 * regs, the byte at 0x44) from the reference into our RAM before
 * comparing — these are areas the SPC700 firmware uses internally that
 * we don't bother emulating identically. Then a byte-by-byte memcmp of
 * the first 0xc000 bytes (the rest is echo buffer / sample data that
 * isn't expected to match exactly).
 *
 * Then walk the DSP-write history lists in parallel and flag any mismatch
 * in order, address, or value. The harness only prints the first 16
 * differences per tick so a major divergence doesn't drown the log.
 *
 * @return  true if everything matched, false if differences were found.
 *          On false the caller rolls our state back from my_spc_snapshot
 *          and retries; on true both histories are reset for the next tick.
 */
bool CompareSpcImpls(SpcPlayer *p, SpcPlayer *p_org, Apu *apu) {
  SpcPlayer_CopyVariablesToRam(p);
  memcpy(p->ram + 0x18, apu->ram + 0x18, 2); //lfsr_value
  memcpy(p->ram + 0x110, apu->ram + 0x110, 256-16);  // stack
  memcpy(p->ram + 0xf1, apu->ram + 0xf1, 15);  // dsp regs
  memcpy(p->ram + 0x10, apu->ram + 0x10, 8);  // temp regs
  p->ram[0x44] = apu->ram[0x44]; // chn
  int errs = 0;
  for (int i = 0; i != 0xc000; i++) {  // skip compare echo etc
    if (p->ram[i] != apu->ram[i]) {
      if (errs < 16) {
        if (errs == 0)
          printf("@%d\n", loop_ctr);
        printf("%.4X: %.2X != %.2X (mine, theirs) orig %.2X\n", i, p->ram[i], apu->ram[i], p_org->ram[i]);
        errs++;
      }
    }
  }

  int n = my_write_hist.count < apu->hist.count ? apu->hist.count : my_write_hist.count;
  for (int i = 0; i != n; i++) {
    if (i >= my_write_hist.count || i >= apu->hist.count || my_write_hist.addr[i] != apu->hist.addr[i] || my_write_hist.val[i] != apu->hist.val[i]) {
      if (errs == 0)
        printf("@%d\n", loop_ctr);
      printf("%d: ", i);
      if (i >= my_write_hist.count) printf("[??: ??]"); else printf("[%.2x: %.2x]", my_write_hist.addr[i], my_write_hist.val[i]);
      printf(" != ");
      if (i >= apu->hist.count) printf("[??: ??]"); else printf("[%.2x: %.2x]", apu->hist.addr[i], apu->hist.val[i]);
      printf("\n");
      errs++;
    }
  }

  if (errs) {
    printf("Total %d errors\n", errs);
    return false;
  }

  apu->hist.count = 0;
  my_write_hist.count = 0;
  loop_ctr++;
  return true;
}

/*
 * RunAudioPlayer — Standalone audio test harness (debug build only).
 *
 * Loads "lightworld.spc" from disk, initializes SDL audio at 44.1kHz
 * stereo, and either:
 *   - run_both == false: runs this C SPC implementation only and queues
 *     the resulting samples to SDL for monitoring. The simpler / faster
 *     mode used while iterating on a single song.
 *   - run_both == true : runs both this implementation and the reference
 *     snes/apu.c emulator in parallel. Every time the reference SPC's PC
 *     hits the driver's tick boundary (0x878/0x879), snapshot our state,
 *     try to advance our state to match, and if CompareSpcImpls reports
 *     differences, roll back and retry. Once they agree, feed the audio
 *     from our DSP to SDL. This is how byte-perfect parity with the
 *     original ROM was achieved during initial porting.
 *
 * The audio queue size cap (`>= 736 * 4 * 3`) keeps about 50ms of audio
 * pending so the queue doesn't grow unbounded if the producer outpaces
 * the consumer. SDL_Delay(1) yields the thread while we wait.
 *
 * apu->inPorts[0] = 2 on the very first iteration starts the Light World
 * theme (track 2). The commented `2 + cycle_counter / 1000` would
 * cycle through tracks once per ~1000 ticks for sweep testing.
 */
void RunAudioPlayer() {
  if(SDL_Init(SDL_INIT_AUDIO) != 0) {
    printf("Failed to init SDL: %s\n", SDL_GetError());
    return;
  }
  
  SDL_AudioSpec want, have;
  SDL_AudioDeviceID device;
  SDL_memset(&want, 0, sizeof(want));
  want.freq = 44100;
  want.format = AUDIO_S16;
  want.channels = 2;
  want.samples = 2048;
  want.callback = NULL; // use queue
  device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if(device == 0) {
    printf("Failed to open audio device: %s\n", SDL_GetError());
    return;
  }
  int16_t* audioBuffer = (int16_t*)malloc(735 * 4); // *2 for stereo, *2 for sizeof(int16)
  SDL_PauseAudioDevice(device, 0);

  memset(&my_spc, 0, sizeof(my_spc));
  FILE *f = fopen("lightworld.spc", "rb");
  fread(my_spc.ram, 1, 65536, f);
  fclose(f);

  my_spc.reg_write_history = &my_write_hist;

  bool run_both = 0;// false;// false;

  if (!run_both) {
    SpcPlayer *p = &my_spc;
    Dsp *dsp = dsp_init(p->ram);
    dsp_reset(dsp);
    p->dsp = dsp;
    SpcPlayer_Initialize(p);

    p->input_ports[0] = 4;

    for (;;) {
      SpcPlayer_GenerateSamples(p);

      int16_t audioBuffer[736 * 2];
      dsp_getSamples(p->dsp, audioBuffer, 736, have.channels);
      SDL_QueueAudio(device, audioBuffer, 736 * 2 * have.channels);
      while (SDL_GetQueuedAudioSize(device) >= 736 * 4 * 3/* 44100 * 4 * 300*/)
        SDL_Delay(1);

    }

  } else {
    SpcPlayer *p = &my_spc;
    Dsp *dsp = dsp_init(p->ram);
    dsp_reset(dsp);
    p->dsp = dsp;

    Apu *apu = apu_init();
    apu_reset(apu);
    apu->spc->pc = 0x800;

    memcpy(apu->ram, my_spc.ram, 65536);

    CompareSpcImpls(&my_spc, &my_spc_snapshot, apu);

    uint64_t cycle_counter = 0;
    int tgt = 0x878;
    uint8 ticks_next = 0;
    bool apu_debug = false;
    bool is_initialize = true;
    for (;;) {
      if (apu_debug && apu->cpuCyclesLeft == 0) {
        char line[80];
        getProcessorStateSpc(apu, line);
        puts(line);
      }

      apu_cycle(apu);

      if (((apu->cycles - 1) & 0x1f) == 0)
        dsp_cycle(p->dsp);


      if (apu->spc->pc == tgt) {
        tgt ^= 0x878 ^ 0x879;
        if (tgt == 0x878) {
          uint8 ticks = ticks_next;
          ticks_next = apu->spc->y;
          my_spc_snapshot = my_spc;
          for (;;) {
            my_write_hist.count = 0;
            if (is_initialize)
              SpcPlayer_Initialize(&my_spc);
            else {
              Spc_Loop_Part2(&my_spc, ticks);
              Spc_Loop_Part1(&my_spc);
            }
            is_initialize = false;
            if (CompareSpcImpls(&my_spc, &my_spc_snapshot,apu))
              break;
            my_spc = my_spc_snapshot;
          }

          if (cycle_counter == 0)
            apu->inPorts[0] = my_spc.input_ports[0] = 2;// 2 + cycle_counter / 1000;
          cycle_counter++;
        }
      }

      if (p->dsp->sampleOffset == 534) {
        int16_t audioBuffer[736 * 2];
        dsp_getSamples(p->dsp, audioBuffer, 736, have.channels);
        SDL_QueueAudio(device, audioBuffer, 736 * 2 * have.channels);
        while (SDL_GetQueuedAudioSize(device) >= 736 * 4 * 3/* 44100 * 4 * 300*/) {
          SDL_Delay(1);
        }
      }
    }
  }
}
#endif  // WITH_SPC_PLAYER_DEBUGGING