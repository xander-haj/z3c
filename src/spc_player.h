/*
 * spc_player.h - SPC700 music engine emulation for the Zelda 3
 *                reimplementation.
 *
 * The SNES audio subsystem runs independently on the SPC700 coprocessor
 * with its own 64KB RAM, 8-channel DSP, and three hardware timers. This
 * file defines the high-level music player that interprets the game's
 * custom music bytecode format to drive the DSP channels.
 *
 * Architecture:
 *   - Channel: Per-voice state for one of 8 DSP channels. Tracks the
 *     current note, volume envelope, pitch bend, vibrato, tremolo, pan,
 *     and pattern playback position. Channels 0-5 are music; 6-7 are
 *     typically used for sound effects.
 *
 *   - SpcPlayer: The master player state machine. Holds global tempo,
 *     master volume, echo parameters, transposition, the DSP instance,
 *     port I/O state for CPU<->APU communication, and all 8 channels.
 *     Also contains the full 64KB SPC RAM image.
 *
 * The music format uses a pattern-order system: a top-level sequence
 * lists pattern pointers; each pattern contains note/command bytecodes.
 * Subroutine calls allow pattern reuse. The sound effect system runs
 * independently on dedicated channels and can temporarily override
 * music channel state.
 *
 * Related files: spc_player.c (implementation), snes/dsp.h (DSP emulation),
 *                audio.c (high-level audio mixing with MSU-1 support)
 */
#include <stddef.h>
#include "snes/dsp.h"

/*
 * Channel - Per-voice state for a single DSP channel.
 *
 * Each channel independently tracks its position in the music pattern,
 * current note timing, volume/pan/pitch envelopes, vibrato/tremolo
 * oscillators, and sound effect overlay state. The SPC700 music engine
 * updates all 8 channels each tick (driven by the tempo accumulator).
 *
 * Size: 56 bytes per channel, 8 channels = 448 bytes total.
 */
typedef struct Channel {
  // --- Pattern Playback ---
  uint16 pattern_order_ptr_for_chan;  // Current position in the pattern order list
  uint8 note_ticks_left;             // Ticks remaining for the current note
  uint8 note_keyoff_ticks_left;      // Ticks until key-off (note release) fires
  uint8 subroutine_num_loops;        // Remaining loop count for pattern subroutine calls
  // --- Volume Envelope ---
  uint8 volume_fade_ticks;           // Ticks remaining in a volume fade operation
  // --- Pan Envelope ---
  uint8 pan_num_ticks;               // Ticks remaining in a pan slide operation
  // --- Pitch Slide ---
  uint8 pitch_slide_length;          // Total length of the current pitch slide (portamento)
  uint8 pitch_slide_delay_left;      // Delay ticks before pitch slide begins
  // --- Vibrato (periodic pitch oscillation) ---
  uint8 vibrato_hold_count;          // Initial hold before vibrato starts oscillating
  uint8 vib_depth;                   // Current vibrato depth (amplitude of pitch wobble)
  // --- Tremolo (periodic volume oscillation) ---
  uint8 tremolo_hold_count;          // Initial hold before tremolo starts oscillating
  uint8 tremolo_depth;               // Current tremolo depth (amplitude of volume wobble)
  // --- Shared Oscillator State ---
  uint8 vibrato_change_count;        // Counter for vibrato direction changes
  // --- Note Timing ---
  uint8 note_length;                 // Default note duration for this channel
  uint8 note_gate_off_fixedpt;       // Gate-off fraction (fixed-point note release timing)
  // --- Volume ---
  uint8 channel_volume_master;       // Master volume for this channel (0-127)
  // --- Instrument ---
  uint8 instrument_id;               // Current instrument/sample index
  uint16 instrument_pitch_base;      // Base pitch for the current instrument
  // --- Pattern Subroutine Stack ---
  uint16 saved_pattern_ptr;          // Return address saved when entering a subroutine
  uint16 pattern_start_ptr;          // Start of the current pattern (for loop-to-start)
  // --- Pitch Envelope ---
  uint8 pitch_envelope_num_ticks;    // Ticks remaining in the pitch envelope
  uint8 pitch_envelope_delay;        // Initial delay before pitch envelope starts
  uint8 pitch_envelope_direction;    // Direction: 0 = up, 1 = down
  uint8 pitch_envelope_slide_value;  // Pitch delta per tick during envelope
  // --- Vibrato Parameters ---
  uint8 vibrato_count;               // Current position in the vibrato waveform cycle
  uint8 vibrato_rate;                // Speed of vibrato oscillation (lower = slower)
  uint8 vibrato_delay_ticks;         // Delay ticks before vibrato activates on a new note
  uint8 vibrato_fade_num_ticks;      // Ticks for vibrato depth to fade in
  uint8 vibrato_fade_add_per_tick;   // Depth increment per tick during vibrato fade-in
  uint8 vibrato_depth_target;        // Target depth that vibrato fades toward
  // --- Tremolo Parameters ---
  uint8 tremolo_count;               // Current position in the tremolo waveform cycle
  uint8 tremolo_rate;                // Speed of tremolo oscillation
  uint8 tremolo_delay_ticks;         // Delay ticks before tremolo activates on a new note
  // --- Transposition ---
  uint8 channel_transposition;       // Semitone offset applied to all notes on this channel
  // --- Volume State ---
  uint16 channel_volume;             // Current channel volume (16-bit for smooth fading)
  uint16 volume_fade_addpertick;     // Volume delta per tick during a fade
  uint8 volume_fade_target;          // Target volume at end of fade
  uint8 final_volume;                // Computed final volume after all modifiers
  // --- Pan State ---
  uint16 pan_value;                  // Current pan position (16-bit for smooth sliding)
  uint16 pan_add_per_tick;           // Pan delta per tick during a pan slide
  uint8 pan_target_value;            // Target pan position at end of slide
  uint8 pan_flag_with_phase_invert;  // Phase inversion flags combined with pan mode
  // --- Pitch State ---
  uint16 pitch;                      // Current pitch value written to DSP
  uint16 pitch_add_per_tick;         // Pitch delta per tick during portamento/slide
  uint8 pitch_target;                // Target pitch for the current slide
  uint8 fine_tune;                   // Fine-tuning offset in cents
  // --- Sound Effect Overlay ---
  // When a sound effect plays on this channel, these fields override
  // the music state temporarily. Music resumes when the SFX ends.
  uint16 sfx_sound_ptr;              // Pointer to the current SFX data stream
  uint8 sfx_which_sound;             // Identifier of the active sound effect
  uint8 sfx_arr_countdown;           // Countdown for SFX note arpeggio sequence
  uint8 sfx_note_length_left;        // Ticks remaining for the current SFX note
  uint8 sfx_note_length;             // Default note length for SFX playback
  uint8 sfx_pan;                     // Pan override for the sound effect
  // --- Channel Identity ---
  uint8 index;                       // This channel's index (0-7)
} Channel;

/*
 * SpcPlayer - Master state for the SPC700 music/sound engine.
 *
 * This structure holds the complete audio engine state: the DSP hardware
 * emulator, CPU<->APU port I/O, global music parameters (tempo, volume,
 * echo), sound effect dispatch state, and all 8 channel instances. The
 * 64KB ram[] array at the end holds the full SPC700 address space,
 * including the uploaded music data, sample directory, and BRR samples.
 *
 * Communication with the main CPU uses 4 bidirectional I/O ports.
 * Port 0 carries command bytes; ports 1-3 carry sound effect triggers
 * and music selection. The engine processes commands each tick.
 */
typedef struct SpcPlayer {
  // --- DSP Interface ---
  DspRegWriteHistory *reg_write_history;  // Log of DSP register writes (for debugging)
  uint8 timer_cycles;                     // Cycle counter for SPC700 timer emulation
  Dsp *dsp;                               // Pointer to the 8-channel DSP emulator instance

  // --- CPU <-> APU Port I/O ---
  // The SNES CPU and SPC700 communicate through 4 shared ports.
  // Each side has its own read/write view of these ports.
  uint8 new_value_from_snes[4];           // Latest values written by the SNES CPU
  uint8 port_to_snes[4];                  // Values this engine writes back to the CPU
  uint8 last_value_from_snes[4];          // Previous CPU values (for change detection)

  // --- Internal Temporaries ---
  uint8 counter_sf0c;                     // General-purpose counter (tick subdivision)
  uint16 _always_zero;                    // Reserved; always reads as zero
  uint16 temp_accum;                      // Temporary accumulator for calculations
  uint8 ttt;                              // Temporary scratch register
  uint8 did_affect_volumepitch_flag;      // Set when volume/pitch was modified this tick

  // --- Address Registers ---
  uint16 addr0;                           // General-purpose address register 0
  uint16 addr1;                           // General-purpose address register 1

  // --- Noise ---
  uint16 lfsr_value;                      // Linear feedback shift register for noise gen

  // --- Channel Control ---
  uint8 is_chan_on;                        // Bitmask of currently active channels
  uint8 fast_forward;                      // When nonzero, skips rendering for fast seek

  // --- Sound Effect Dispatch ---
  uint8 sfx_start_arg_pan;                // Pan value passed when starting a new SFX
  uint16 sfx_sound_ptr_cur;               // Current read position in the SFX data stream

  // --- Music Sequencer ---
  uint16 music_ptr_toplevel;               // Pointer to the current song's pattern order list
  uint8 block_count;                       // Number of pattern blocks in the current song

  // --- SFX Timer ---
  uint8 sfx_timer_accum;                  // Timer accumulator for SFX tick rate

  // --- Per-Tick Working State ---
  uint8 chn;                              // Index of the channel being processed this tick
  uint8 key_ON;                           // Bitmask of channels to key-on this tick
  uint8 key_OFF;                          // Bitmask of channels to key-off this tick
  uint8 cur_chan_bit;                      // Bit mask for the channel being processed (1<<chn)

  // --- DSP Global Register Shadows ---
  // These mirror the DSP's global registers; the engine writes them
  // to the DSP when they change to avoid redundant register writes.
  uint8 reg_FLG;                          // DSP FLG register (noise clock, echo enable, mute)
  uint8 reg_NON;                          // DSP NON register (noise channel enable bitmask)
  uint8 reg_EON;                          // DSP EON register (echo channel enable bitmask)
  uint8 reg_PMON;                         // DSP PMON register (pitch modulation enable)

  // --- Echo Configuration ---
  uint8 echo_stored_time;                 // Cached echo delay time for comparison
  uint8 echo_parameter_EDL;               // Echo delay length in 16ms units (DSP EDL)
  uint8 reg_EFB;                          // Echo feedback coefficient (DSP EFB register)

  // --- Global Transposition ---
  uint8 global_transposition;             // Semitone offset applied to all channels

  // --- Tempo ---
  uint8 main_tempo_accum;                 // Fractional tempo accumulator (tick trigger at overflow)
  uint16 tempo;                           // Current tempo value (higher = faster)
  uint8 tempo_fade_num_ticks;             // Ticks remaining in a tempo fade
  uint8 tempo_fade_final;                 // Target tempo at end of fade
  uint16 tempo_fade_add;                  // Tempo delta per tick during fade

  // --- Master Volume ---
  uint16 master_volume;                   // Global master volume (affects all channels)
  uint8 master_volume_fade_ticks;         // Ticks remaining in a master volume fade
  uint8 master_volume_fade_target;        // Target master volume at end of fade
  uint16 master_volume_fade_add_per_tick; // Master volume delta per tick during fade
  uint8 vol_dirty;                        // Flag: set when volume needs recalculation

  // --- Percussion ---
  uint8 percussion_base_id;               // Base instrument ID for percussion mapping

  // --- Echo Volume ---
  uint16 echo_volume_left;                // Current echo volume for the left channel
  uint16 echo_volume_right;               // Current echo volume for the right channel
  uint16 echo_volume_fade_add_left;       // Echo volume delta per tick (left)
  uint16 echo_volume_fade_add_right;      // Echo volume delta per tick (right)
  uint8 echo_volume_fade_ticks;           // Ticks remaining in echo volume fade
  uint8 echo_volume_fade_target_left;     // Target echo volume (left)
  uint8 echo_volume_fade_target_right;    // Target echo volume (right)

  // --- SFX Channel Management ---
  uint8 sfx_channel_index;                // Primary SFX channel being used
  uint8 current_bit;                      // Current channel bitmask for iteration
  uint8 dsp_register_index;               // DSP register address for indexed writes
  uint8 echo_channels;                    // Channels with echo currently enabled

  // --- Miscellaneous State ---
  uint8 byte_3C4;                         // Internal state byte (purpose from original ROM)
  uint8 byte_3C5;                         // Internal state byte (purpose from original ROM)
  uint8 echo_fract_incr;                  // Fractional echo parameter increment
  uint8 sfx_channel_index2;               // Secondary SFX channel for multi-channel effects
  uint8 sfx_channel_bit;                  // Bitmask for the active SFX channel
  uint8 pause_music_ctr;                  // Counter: when nonzero, music playback is paused

  // --- Port Activity Tracking ---
  // Tracks which ports have pending commands to process.
  uint8 port2_active;                     // Nonzero if port 2 has unprocessed data
  uint8 port2_current_bit;                // Current processing bit for port 2 commands
  uint8 port3_active;                     // Nonzero if port 3 has unprocessed data
  uint8 port3_current_bit;                // Current processing bit for port 3 commands
  uint8 port1_active;                     // Nonzero if port 1 has unprocessed data
  uint8 port1_current_bit;                // Current processing bit for port 1 commands
  uint8 byte_3E1;                         // Internal state byte (purpose from original ROM)

  // --- SFX Echo Control ---
  uint8 sfx_play_echo_flag;               // Whether the current SFX should use echo
  uint8 sfx_channels_echo_mask2;          // Secondary echo mask for SFX channels

  // --- Port Counters ---
  uint8 port1_counter;                    // Sequence counter for port 1 handshake protocol

  // --- Channel 6/7 Volume ---
  uint8 channel_67_volume;                // Shared volume for SFX channels 6 and 7

  // --- Safety / Debug ---
  uint8 cutk_always_zero;                 // Always zero; may guard against stale key-off
  uint8 last_written_edl;                 // Last EDL value written to DSP (avoids redundant writes)

  // --- Input Port Mirror ---
  uint8 input_ports[4];                   // Mirrored copy of the 4 input port values

  // --- Channel Array ---
  Channel channel[8];                     // State for all 8 DSP channels

  // --- SPC700 RAM ---
  uint8 ram[65536];                       // Full 64KB SPC700 address space
} SpcPlayer;

// -----------------------------------------------------------------------
// SpcPlayer Public API
// -----------------------------------------------------------------------

// Allocates and returns a new SpcPlayer instance with zeroed state
SpcPlayer *SpcPlayer_Create();
// Runs one tick of the music engine and generates audio samples via the DSP
void SpcPlayer_GenerateSamples(SpcPlayer *p);
// Initializes the player state, DSP, and loads the boot program into RAM
void SpcPlayer_Initialize(SpcPlayer *p);
// Uploads music/sample data from the main CPU into SPC700 RAM
void SpcPlayer_Upload(SpcPlayer *p, const uint8_t *data);
// Copies structured variables from SPC700 RAM into the SpcPlayer struct fields
// (synchronizes the C struct with the raw RAM after an upload or state change)
void SpcPlayer_CopyVariablesFromRam(SpcPlayer *p);
// Copies SpcPlayer struct fields back into SPC700 RAM addresses
// (synchronizes the raw RAM with the C struct before DSP processing)
void SpcPlayer_CopyVariablesToRam(SpcPlayer *p);
