/*
 * audio.c — High-level audio engine for the Zelda 3 C reimplementation.
 *
 * This module sits between the game logic and the low-level sound output,
 * providing two parallel music playback paths:
 *
 *   1. SPC700 emulation (spc_player.c) — faithful reproduction of the
 *      original SNES APU, used when no MSU-1 pack is installed.
 *
 *   2. MSU-1 streaming — plays pre-recorded CD-quality music from
 *      external files, supporting both raw PCM (.pcm, 44.1 kHz stereo)
 *      and Opus-compressed (.opuz, 48 kHz stereo) formats.
 *
 * MSU-1 is a community-created enhancement chip specification originally
 * designed for SNES flash cartridges. It allows CD-quality audio tracks
 * to replace the console's native synthesized music. This file implements
 * a software MSU-1 player that can optionally use the "MSU Deluxe" mod,
 * which remaps track numbers per-overworld-area and per-entrance to give
 * each location a unique musical theme.
 *
 * When MSU-1 is active, the SPC player is paused (command 0xF0) so the
 * two audio paths do not conflict. The game's volume transition commands
 * (0xF1-0xF3) are translated into smooth floating-point volume ramps
 * applied to the MSU stream during mixing.
 *
 * A lock-free circular queue (g_apu_write_ents) decouples the game
 * thread's APU register writes from the audio callback thread that
 * consumes them, preventing timing-dependent glitches.
 *
 * Key data flow:
 *   Game logic -> zelda_apu_write() -> ZeldaPushApuState() -> queue
 *   Audio callback -> ZeldaRenderAudio() -> ZeldaPopApuState() -> SPC/MSU
 *
 * Related files:
 *   - audio.h           Public API declarations
 *   - spc_player.c/h    SPC700 music/SFX emulator (the "native" path)
 *   - config.h/c        User configuration (msu_path, audio_freq, msuvolume)
 *   - zelda_rtl.c/h     Runtime bridge providing ZeldaApuLock/Unlock
 *   - variables.h       Game state variables (music_unk1, overworld_area_index)
 *   - features.h        Feature flags (kFeatures0_MiscBugFixes)
 */

// Standard library and project headers
#include "audio.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "features.h"
// SNES APU I/O port register addresses (APUI00..APUI03)
#include "snes/snes_regs.h"
// SPC700 emulated music/SFX player
#include "spc_player.h"
// Opus audio codec for .opuz MSU file decoding
#include "third_party/opus-1.3.1-stripped/opus.h"
// Runtime configuration (audio_freq, msu_path, msuvolume, resume_msu)
#include "config.h"
// ROM asset data tables (kEntranceData_musicTrack)
#include "assets.h"

// Persistent playback position saved into the game snapshot (msu_resume_info / msu_resume_info_alt).
// All fields together allow the MSU player to reopen a track and seek exactly to the
// position that was playing when the snapshot was taken, including for Opus (.opuz) files
// where byte-offset alone is insufficient (packet boundaries must be located).
// This needs to hold a lot more things than with just PCM
typedef struct MsuPlayerResumeInfo {
  uint32 tag;                   // Magic bytes from bytes 0-3 of the MSU file, used to detect if
                                //   the file on disk matches the one that was playing at save time.
  uint32 offset;                // Byte offset into the file (PCM: sample index * 4; Opus: byte offset).
  uint32 samples_until_repeat;  // Remaining samples before the loop point at the time of the snapshot.
  uint16 range_cur, range_repeat; // Opus: current and loop-target segment header offsets in the file.
  uint64 initial_packet_bytes;  // To verify we seeked right
  uint8 orig_track;        // Using the old zelda track numbers
  uint8 actual_track;      // The MSU track index we're playing (Different if using msu deluxe)
} MsuPlayerResumeInfo;

// Lifecycle states for MsuPlayer.state.
enum {
  kMsuState_Idle = 0,           // No file open; MSU is inactive (SPC plays normally).
  kMsuState_FinishedPlaying = 1,// Track ended without a loop point; file has been closed.
  kMsuState_Resuming = 2,       // File is open and seeking to the snapshot position.
  kMsuState_Playing = 3,        // File is open and actively streaming into the mix.
};

// All runtime state for the single global MSU-1 music stream.
typedef struct MsuPlayer {
  FILE *f;                          // Open file handle for the current .pcm or .opuz track.
  uint32 buffer_size, buffer_pos;   // Valid sample range [buffer_pos, buffer_size) in buffer[].
  uint32 preskip, samples_until_repeat; // Opus preskip (decoder warmup samples to discard);
                                    //   samples remaining before the loop boundary.
  uint32 total_samples_in_file, repeat_position; // PCM: total stereo sample pairs and loop start offset.
  uint32 cur_file_offs;             // Current file byte position (advanced as packets/samples are read).
  MsuPlayerResumeInfo resume_info;  // Live copy of the resume state, kept up-to-date during playback.
  uint8 enabled;                    // kMsuEnabled_* flags (PCM, Opuz, MsuDeluxe).
  uint8 state;                      // One of kMsuState_*.
  float volume, volume_step, volume_target; // Current volume, per-sample step, and ramp destination
                                    //   (all in [0.0, 1.0] * msuvolume scale).
  uint16 range_cur, range_repeat;   // Opus only: byte offsets of the current and repeat segment headers.
  OpusDecoder *opus;                // Opus decoder context, NULL when playing raw PCM.
  int16 buffer[960 * 2];            // Decoded stereo interleaved sample buffer (max one Opus frame = 960
                                    //   samples at 48 kHz; reused as scratch space during Opus parsing).
} MsuPlayer;

static MsuPlayer g_msu_player;

static void MsuPlayer_Open(MsuPlayer *mp, int orig_track, bool resume_from_snapshot);

// Per-track loop flag for raw PCM (.pcm) MSU tracks. Indexed by actual_track (0-47).
// 1 = the track has a loop point (seek to repeat_position when end of file is reached).
// 0 = the track plays once and transitions to kMsuState_FinishedPlaying.
// Tracks without loop points are typically one-shot jingles (fanfares, item stingers).
static const uint8 kMsuTracksWithRepeat[48] = {
  1,0,1,1,1,1,1,1,0,1,0,1,1,1,1,0,
  1,1,1,0,1,1,1,1,1,1,1,1,1,0,1,1,
  1,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,
};

static const uint8 kIsMusicOwOrDungeon[] = {
  0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, // 1 = ow songs : 2 = lw, 5 = forest, 7 = town, 9 = dark world, 13 = mountain
  2, 2, 2, 0, 0, 0, 2, 2, 0, 0, 0, 2, 0, 0, 0, 0, // 2 = indoor songs : 16, 17, 18, 22, 23, 27
};


// MSU Deluxe overworld track remapping table. Indexed by BYTE(overworld_area_index).
// Maps each of the 160 overworld areas to an MSU track number, providing a unique theme
// per area instead of the 4 broad themes the base game uses. The table is split into
// three geographic blocks (light world, dark world, special) and two 64-entry halves.
static const uint8 kMsuDeluxe_OW_Songs[] = {
  37, 37, 42, 38, 38, 38, 38, 39, 37, 37, 42, 38, 38, 38, 38, 41, // lw
  42, 42, 42, 42, 42, 42, 40, 40, 43, 43, 42, 47, 47, 42, 45, 45,
  43, 43, 43, 47, 47, 42, 45, 45, 112, 112, 48, 42, 42, 42, 42, 45,
  44, 44, 48, 48, 48, 46, 46, 46, 44, 44, 44, 48, 48, 46, 46, 46,
  49, 49, 51, 50, 50, 50, 50, 50, 49, 49, 51, 50, 50, 50, 50, 51, // dw
  51, 51, 51, 51, 51, 51, 51, 51, 52, 52, 51, 56, 56, 51, 54, 54,
  52, 52, 52, 56, 56, 51, 54, 54, 58, 52, 57, 51, 51, 51, 51, 54,
  53, 53, 57, 57, 57, 55, 55, 110, 53, 53, 57, 57, 57, 55, 55, 110,
  37, 41, 41, 42, 42, 42, 42, 42, 42, 41, 41, 42, 42, 42, 42, 42, // special
  42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
};

// MSU Deluxe dungeon/interior entrance track remapping table. Indexed by which_entrance.
// Maps each of the 133 entrance indices to an MSU track number for a unique dungeon theme.
// Value 242 (0xF2) means "no override for this entrance" — use the standard track remapping
// (or the original SPC music if MSU doesn't cover that entrance). Checked in both
// RemapMsuDeluxeTrack (for kIsMusicOwOrDungeon == 2 songs) and ZeldaGetEntranceMusicTrack
// (to substitute a fade-out command with the standard indoor track 16 when MSU is active).
static const uint8 kMsuDeluxe_Entrance_Songs[] = {
   59,  59,  60,  61,  61,  61,  62,  62,  63,  64,  64,  64, 105,  65,  65,  66,
   66,  62,  67,  62,  62,  68,  62,  62,  68,  68,  62,  62,  62,  62,  62,  62,
   62,  62,  62,  62,  69,  70,  71,  72,  73,  73,  73, 106, 102,  74,  62,  62,
   75,  75,  76,  77,  78,  68,  79,  80,  81,  62,  62,  62,  82,  75, 242,  59,
   59,  76, 242, 242, 242,  96,  83,  99,  59, 242, 242, 242,  84,  95, 104,  62,
   85,  62,  62,  86, 242,  67, 103,  83,  83,  87,  76,  88,  81,  98,  81,  88,
   83,  89,  75,  97,  90,  91,  91, 100,  92,  93,  92, 242,  93, 107,  62,  75,
   62,  67,  62, 242, 242, 242,  73,  73,  73,  73, 102, 114,  81,  76,  62,  67,
   62,  61,  94,  62, 103,
};


// Remap an track number into a potentially different track number (used for msu deluxe)
static uint8 RemapMsuDeluxeTrack(MsuPlayer *mp, uint8 track) {
  if (!(mp->enabled & kMsuEnabled_MsuDeluxe) || track >= sizeof(kIsMusicOwOrDungeon))
    return track;
  switch (kIsMusicOwOrDungeon[track]) {
  case 1:
    return BYTE(overworld_area_index) < sizeof(kMsuDeluxe_OW_Songs) ? kMsuDeluxe_OW_Songs[BYTE(overworld_area_index)] : track;
  case 2:
    if (which_entrance >= sizeof(kMsuDeluxe_Entrance_Songs) || kMsuDeluxe_Entrance_Songs[which_entrance] == 242)
      return track;
    return kMsuDeluxe_Entrance_Songs[which_entrance];
  default:
    return track;
  }
}

// Returns true if the given game music track number is currently playing.
// When MSU Deluxe is active, compares the requested track's remapped MSU number
// against the actual_track field of the live resume info (so callers using the
// original track number still get the correct answer even though a different
// MSU file number is playing). Falls back to comparing against music_unk1
// (the SPC player's current track register) when MSU is idle.
bool ZeldaIsPlayingMusicTrack(uint8 track) {
  MsuPlayer *mp = &g_msu_player;
  if (mp->state != kMsuState_Idle && mp->enabled & kMsuEnabled_MsuDeluxe)
    return RemapMsuDeluxeTrack(mp, track) == mp->resume_info.actual_track;
  else
    return track == music_unk1;
}

// Bug-compatible variant of ZeldaIsPlayingMusicTrack. When the MiscBugFixes feature
// flag is clear (emulating the original SNES behaviour), compares against
// last_music_control instead of music_unk1. In the original game, some track-change
// checks ran before music_unk1 was updated, so they incorrectly compared against the
// previously-written control byte. The corrected path uses music_unk1 uniformly.
bool ZeldaIsPlayingMusicTrackWithBug(uint8 track) {
  MsuPlayer *mp = &g_msu_player;
  if (mp->state != kMsuState_Idle && mp->enabled & kMsuEnabled_MsuDeluxe)
    return RemapMsuDeluxeTrack(mp, track) == mp->resume_info.actual_track;
  else
    return track == (enhanced_features0 & kFeatures0_MiscBugFixes ? music_unk1 : last_music_control);
}

// Returns the music track command for entrance i, potentially patched for MSU Deluxe.
// i: entrance index into kEntranceData_musicTrack[].
// In the base game, some entrances use track 242 (0xF2) as a "fade out music" command.
// When MSU Deluxe is active and the current entrance has a valid MSU Deluxe track
// (kMsuDeluxe_Entrance_Songs[which_entrance] != 242), that fade-out is replaced with
// track 16 (the generic indoor track), allowing the MSU player's own cross-fade to take
// over instead of silencing the music entirely.
uint8 ZeldaGetEntranceMusicTrack(int i) {
  MsuPlayer *mp = &g_msu_player;
  uint8 rv = kEntranceData_musicTrack[i];

  // For some entrances the original performs a fade out, while msu deluxe has new tracks.
  if (mp->state != kMsuState_Idle && mp->enabled & kMsuEnabled_MsuDeluxe) {
    if (rv == 242 && kMsuDeluxe_Entrance_Songs[which_entrance] != 242)
      rv = 16;
  }

  return rv;
}

// Volume ramp targets and speeds for the four SPC music control commands 0xF1-0xF3
// plus the default "full volume" ramp used when opening a new track (index 3).
//   Index 0 (0xF1): fade out to silence  — target 0,   step 7  (fast fade).
//   Index 1 (0xF2): fade to half volume  — target 64,  step 3  (slow fade).
//   Index 2 (0xF3): restore full volume  — target 255, step 3  (slow restore).
//   Index 3 (implicit): immediate full   — target 255, step 24 (near-instant).
// These integer values are converted to floating-point per-sample ramps in ZeldaEnableMsu.
static const uint8 kVolumeTransitionTarget[4] = { 0, 64, 255, 255};
static const uint8 kVolumeTransitionStep[4] = { 7, 3, 3, 24};
// These are precomputed in the config parse
static float kVolumeTransitionStepFloat[4];
static float kVolumeTransitionTargetFloat[4];

// Main audio track command router. Called by the game whenever it would write a
// music control byte to APUI00. Routes the command to MSU or SPC:
//   music_ctrl in [0x00, 0xEF]: open (or reopen) the corresponding MSU track file.
//   music_ctrl in [0xF1, 0xF3]: set a volume ramp on the MSU stream (fade out/in).
//   music_ctrl == 0xF0: no-op on the MSU side (SPC pause command — MSU handles it).
// Always forwards the command to zelda_apu_write for the SPC player:
//   If MSU is playing: forwards 0xF0 (pause SPC so it doesn't interfere with MSU).
//   If MSU is idle:    forwards the original music_ctrl so SPC plays normally.
// Acquires the APU lock around all state changes to prevent races with the audio thread.
void ZeldaPlayMsuAudioTrack(uint8 music_ctrl) {
  MsuPlayer *mp = &g_msu_player;
  if (!mp->enabled) {
    mp->resume_info.tag = 0;
    zelda_apu_write(APUI00, music_ctrl);
    return;
  }
  ZeldaApuLock();
  if ((music_ctrl & 0xf0) != 0xf0)
    MsuPlayer_Open(mp, music_ctrl, false);
  else if (music_ctrl >= 0xf1 && music_ctrl <= 0xf3) {
    mp->volume_target = kVolumeTransitionTargetFloat[music_ctrl - 0xf1];
    mp->volume_step = kVolumeTransitionStepFloat[music_ctrl - 0xf1];
  }

  if (mp->state == 0) {
    zelda_apu_write(APUI00, music_ctrl);
  } else {
    zelda_apu_write(APUI00, 0xf0);  // pause spc player
  }
  ZeldaApuUnlock();
}

// Closes the MSU file and tears down the Opus decoder if one is active.
// Transitions state to kMsuState_Idle unless the track ended naturally
// (kMsuState_FinishedPlaying), in which case the state is preserved so callers
// can distinguish "no track loaded" from "track finished without looping".
// Clears resume_info so a stale snapshot cannot accidentally resume into a
// closed file.
static void MsuPlayer_CloseFile(MsuPlayer *mp) {
  if (mp->f)
    fclose(mp->f);
  opus_decoder_destroy(mp->opus);
  mp->opus = NULL;
  mp->f = NULL;
  if (mp->state != kMsuState_FinishedPlaying)
    mp->state = kMsuState_Idle;
  memset(&mp->resume_info, 0, sizeof(mp->resume_info));
}

// Opens an MSU track file and prepares the player for streaming.
// orig_track: the game's original music track number (before MSU Deluxe remapping).
// resume_from_snapshot: if true, restores from msu_resume_info (save-state load path);
//   if false, checks msu_resume_info_alt for an overworld-return resume opportunity.
//
// Resume logic (non-snapshot path):
//   When re-entering the overworld (main_module_index == 9) and the new track's
//   actual MSU number matches what was playing before, restores the playback position
//   from msu_resume_info_alt so the overworld music continues seamlessly. Otherwise
//   copies the current resume_info into msu_resume_info_alt for future use.
//
// File format detection (via the 4-byte magic tag):
//   'MSU1' (little-endian 0x4D535531): raw PCM — reads repeat_position from bytes 4-7,
//     computes total_samples_in_file from the file size, seeks to cur_file_offs.
//   'OPUz' (little-endian 0x4F505566): Opus container — creates an OpusDecoder at 48 kHz
//     stereo and seeks to cur_file_offs when resuming.
//   Anything else: treated as a read error; closes the file and returns.
//
// Sets volume_target / volume_step to the "full volume" ramp (index 3) for new tracks.
// Sets state to kMsuState_Resuming if the file tag matches the resume_info tag,
// otherwise kMsuState_Playing (start from the beginning or the computed offset).
static void MsuPlayer_Open(MsuPlayer *mp, int orig_track, bool resume_from_snapshot) {
  MsuPlayerResumeInfo resume;
  int actual_track = RemapMsuDeluxeTrack(mp, orig_track);

  if (!resume_from_snapshot) {
    resume.tag = 0;
    // Attempt to resume MSU playback when exiting back to the overworld.
    if (main_module_index == 9 &&
        actual_track == ((MsuPlayerResumeInfo *)msu_resume_info_alt)->actual_track && g_config.resume_msu) {
      memcpy(&resume, msu_resume_info_alt, sizeof(mp->resume_info));
    }
    if (mp->state >= kMsuState_Resuming)
      memcpy(msu_resume_info_alt, &mp->resume_info, sizeof(mp->resume_info));
  } else {
    memcpy(&resume, msu_resume_info, sizeof(mp->resume_info));
  }

  mp->volume_target = kVolumeTransitionTargetFloat[3];
  mp->volume_step = kVolumeTransitionStepFloat[3];

  mp->state = kMsuState_Idle;
  MsuPlayer_CloseFile(mp);
  if (actual_track == 0)
    return;
  char fname[256], buf[8];
  snprintf(fname, sizeof(fname), "%s%d.%s", g_config.msu_path ? g_config.msu_path : "", actual_track, mp->enabled & kMsuEnabled_Opuz ? "opuz" : "pcm");
  printf("Loading MSU %s\n", fname);
  mp->f = fopen(fname, "rb");
  if (mp->f == NULL)
    goto READ_ERROR;
  setvbuf(mp->f, NULL, _IOFBF, 16384);
  if (fread(buf, 1, 8, mp->f) != 8) READ_ERROR: {
    fprintf(stderr, "Unable to read MSU file %s\n", fname);
    MsuPlayer_CloseFile(mp);
    return;
  }
  uint32 file_tag = *(uint32 *)(buf + 0);
  mp->repeat_position = *(uint32 *)(buf + 4);
  mp->state = (resume.actual_track == actual_track && resume.tag == file_tag) ? kMsuState_Resuming : kMsuState_Playing;
  if (mp->state == kMsuState_Resuming) {
    memcpy(&mp->resume_info, &resume, sizeof(mp->resume_info));
  } else {
    mp->resume_info.orig_track = orig_track;
    mp->resume_info.actual_track = actual_track;
    mp->resume_info.tag = file_tag;
    mp->resume_info.range_cur = 8;
  }
  mp->cur_file_offs = mp->resume_info.offset;
  mp->samples_until_repeat = mp->resume_info.samples_until_repeat;
  mp->range_cur = mp->resume_info.range_cur;
  mp->range_repeat = mp->resume_info.range_repeat;
  mp->buffer_size = mp->buffer_pos = 0;
  mp->preskip = 0;
  if (file_tag == (('Z' << 24) | ('U' << 16) | ('P' << 8) | 'O')) {
    mp->opus = opus_decoder_create(48000, 2, NULL);
    if (!mp->opus)
      goto READ_ERROR;
    if (mp->state == kMsuState_Resuming)
      fseek(mp->f, mp->cur_file_offs, SEEK_SET);
  } else if (file_tag == (('1' << 24) | ('U' << 16) | ('S' << 8) | 'M')) {
    fseek(mp->f, 0, SEEK_END);
    mp->total_samples_in_file = (ftell(mp->f) - 8) / 4;
    mp->samples_until_repeat = mp->total_samples_in_file - mp->cur_file_offs;
    fseek(mp->f, mp->cur_file_offs * 4 + 8, SEEK_SET);
  } else {
    goto READ_ERROR;
  }
}

// Adds n stereo sample pairs from src into dst at a fixed volume level.
// volume == 1.0f: no scaling needed — adds directly to avoid the integer multiply.
// Otherwise scales via a 16.16 fixed-point multiply (volume * 65536 → uint32)
// and right-shifts by 16 to produce the final sample contribution.
// dst and src are interleaved stereo (L at [i*2+0], R at [i*2+1]).
static void MixToBufferWithVolume(int16 *dst, const int16 *src, size_t n, float volume) {
  if (volume == 1.0f) {
    for (size_t i = 0; i < n; i++) {
      dst[i * 2 + 0] += src[i * 2 + 0];
      dst[i * 2 + 1] += src[i * 2 + 1];
    }
  } else {
    uint32 vol = (int32)(65536 * volume);
    for (size_t i = 0; i < n; i++) {
      dst[i * 2 + 0] += src[i * 2 + 0] * vol >> 16;
      dst[i * 2 + 1] += src[i * 2 + 1] * vol >> 16;
    }
  }
}

// Adds n stereo sample pairs from src into dst while linearly ramping the volume
// from `volume` by `volume_step` per sample. Uses 48.16 fixed-point arithmetic
// (multiplier 2^48 = 281474976710656.0) for high precision without floating-point
// per-sample operations in the inner loop. `ideal_target` is unused (reserved for
// future clamp-at-target logic; the ramp caller ensures it stops at the right frame).
static void MixToBufferWithVolumeRamp(int16 *dst, const int16 *src, size_t n, float volume, float volume_step, float ideal_target) {
  int64 vol = volume * 281474976710656.0f;
  int64 step = volume_step * 281474976710656.0f;
  for (size_t i = 0; i < n; i++) {
    uint32 v = (vol >> 32);
    dst[i * 2 + 0] += src[i * 2 + 0] * v >> 16;
    dst[i * 2 + 1] += src[i * 2 + 1] * v >> 16;
    vol += step;
  }
}

// Volume-aware dispatch mixer. Applies the player's current volume ramp to n
// stereo sample pairs from src into dst, updating mp->volume along the way.
// If a ramp is in progress (volume != volume_target):
//   Determines the sign of the step (fade in or fade out), projects the new
//   volume after n samples, then clamps it at volume_target if it would overshoot.
//   Sends the ramp segment to MixToBufferWithVolumeRamp, then sends any remaining
//   samples (after the ramp reaches the target) to MixToBufferWithVolume at the
//   steady-state volume.
// If no ramp is in progress: delegates entirely to MixToBufferWithVolume.
static void MixToBuffer(MsuPlayer *mp, int16 *dst, const int16 *src, uint32 n) {
  if (mp->volume != mp->volume_target) {
    float step = mp->volume < mp->volume_target ? mp->volume_step : -mp->volume_step;
    float new_vol = mp->volume + step * n;
    uint32 curn = n;
    if (step >= 0 ? new_vol >= mp->volume_target : new_vol < mp->volume_target) {
      uint32 maxn = (uint32)((mp->volume_target - mp->volume) / step);
      curn = UintMin(maxn, curn);
      new_vol = mp->volume_target;
    }
    float vol = mp->volume;
    mp->volume = new_vol;
    MixToBufferWithVolumeRamp(dst, src, curn, vol, step, new_vol);
    dst += curn * 2, src += curn * 2, n -= curn;
  }
  MixToBufferWithVolume(dst, src, n, mp->volume);
}

// Core MSU streaming loop. Fills audio_samples stereo sample pairs into audio_buffer
// by reading and decoding from the open MSU file. Called from ZeldaRenderAudio while
// the APU lock is held.
//
// Inner loop: repeats until audio_samples is satisfied.
//   If the decode buffer (mp->buffer[buffer_pos..buffer_size]) is exhausted:
//     Opus path:
//       If samples_until_repeat == 0: seek to the next segment header (range_cur).
//         Read the 10-byte segment header: {file_offs(4), samples(4), preskip+flags(2)}.
//         preskip bit 14 = mark this segment as the repeat target (range_repeat = range_cur).
//         preskip bit 15 = loop back to range_repeat; otherwise advance range_cur by 10.
//         If range_cur was 0 at the start: track has ended → FINISHED_PLAYING.
//       Read the next Opus packet (2-byte length header + packet body), decode it.
//       If state == kMsuState_Resuming: verify initial_packet_bytes matches the snapshot
//         (ensures a resumed save-state hasn't opened a different file).
//       Advance cur_file_offs and update resume_info for the next snapshot.
//       Skip frames until preskip is consumed (Opus decoder warmup).
//     PCM path:
//       If samples_until_repeat == 0 and no loop flag: FINISHED_PLAYING.
//       Otherwise seek to repeat_position and reset samples_until_repeat.
//       Read up to 960 samples from the file.
//     After decoding: clamp n to samples_until_repeat, advance buffer pointers.
//   Mix the available decoded samples into audio_buffer via MixToBuffer,
//   then advance audio_buffer and decrement audio_samples.
void MsuPlayer_Mix(MsuPlayer *mp, int16 *audio_buffer, int audio_samples) {
  int r;

  do {
    if (mp->buffer_size - mp->buffer_pos == 0) {
      if (mp->opus != NULL) {
        if (mp->samples_until_repeat == 0) {
          if (mp->range_cur == 0) FINISHED_PLAYING: {
            mp->state = kMsuState_FinishedPlaying;
            MsuPlayer_CloseFile(mp);
            return;
          }
          opus_decoder_ctl(mp->opus, OPUS_RESET_STATE);
          fseek(mp->f, mp->range_cur, SEEK_SET);
          uint8 *file_data = (uint8 *)mp->buffer;
          if (fread(file_data, 1, 10, mp->f) != 10) READ_ERROR: {
            fprintf(stderr, "MSU read/decode error!\n");
            zelda_apu_write(APUI00, mp->resume_info.orig_track);
            MsuPlayer_CloseFile(mp);
            return;
          }
          uint32 file_offs = *(uint32 *)&file_data[0];
          assert((file_offs & 0xF0000000) == 0);
          uint32 samples_until_repeat = *(uint32 *)&file_data[4];
          uint16 preskip = *(uint32 *)&file_data[8];
          mp->samples_until_repeat = samples_until_repeat;
          mp->preskip = preskip & 0x3fff;
          if (preskip & 0x4000)
            mp->range_repeat = mp->range_cur;
          mp->range_cur = (preskip & 0x8000) ? mp->range_repeat : mp->range_cur + 10;
          mp->cur_file_offs = file_offs;
          mp->resume_info.range_repeat = mp->range_repeat;
          mp->resume_info.range_cur = mp->range_cur;
          fseek(mp->f, file_offs, SEEK_SET);
        }
        assert(mp->samples_until_repeat != 0);
        for (;;) {
          uint8 *file_data = (uint8 *)mp->buffer;
          *(uint64 *)file_data = 0;
          if (fread(file_data, 1, 2, mp->f) != 2)
            goto READ_ERROR;
          int size = *(uint16 *)file_data & 0x7fff;
          if (size > 1275)
            goto READ_ERROR;
          int n = (*(uint16 *)file_data >> 15);
          if (fread(&file_data[2], 1, size, mp->f) != size)
            goto READ_ERROR;
          // Verify if the snapshot matches the file on disk.
          uint64 initial_file_data = *(uint64 *)file_data;
          if (mp->state == kMsuState_Resuming) {
            mp->state = kMsuState_Playing;
            if (mp->resume_info.initial_packet_bytes != initial_file_data)
              goto READ_ERROR;
          }
          mp->resume_info.initial_packet_bytes = initial_file_data;
          mp->resume_info.samples_until_repeat = mp->samples_until_repeat + mp->preskip;
          mp->resume_info.offset = mp->cur_file_offs;
          mp->cur_file_offs += 2 + size;
          file_data[1] = 0xfc;
          r = opus_decode(mp->opus, &file_data[2 - n], size + n, mp->buffer, 960, 0);
          if (r <= 0)
            goto READ_ERROR;
          if (r > mp->preskip)
            break;
          mp->preskip -= r;
        }
      } else {
        if (mp->samples_until_repeat == 0) {
          if (mp->resume_info.actual_track < sizeof(kMsuTracksWithRepeat) && !kMsuTracksWithRepeat[mp->resume_info.actual_track])
            goto FINISHED_PLAYING;
          mp->samples_until_repeat = mp->total_samples_in_file - mp->repeat_position;
          if (mp->samples_until_repeat == 0)
            goto READ_ERROR; // impossible to make progress
          mp->cur_file_offs = mp->repeat_position;
          fseek(mp->f, mp->cur_file_offs * 4 + 8, SEEK_SET);
        }
        r = UintMin(960, mp->samples_until_repeat);
        if (fread(mp->buffer, 4, r, mp->f) != r)
          goto READ_ERROR;
        mp->resume_info.offset = mp->cur_file_offs;
        mp->cur_file_offs += r;
      }
      uint32 n = UintMin(r - mp->preskip, mp->samples_until_repeat);
      mp->samples_until_repeat -= n;
      mp->buffer_pos = mp->preskip;
      mp->buffer_size = mp->buffer_pos + n;
      mp->preskip = 0;
    }
#if 0
    if (mp->samples_to_play > 44100 * 5) {
      mp->buffer_pos = mp->buffer_size;
    }
#endif
    int nr = IntMin(audio_samples, mp->buffer_size - mp->buffer_pos);
    int16 *buf = mp->buffer + mp->buffer_pos * 2;
    mp->buffer_pos += nr;

#if 0
    static int t;
    for (int i = 0; i < nr; i++) {
      buf[i * 2 + 0] = buf[i * 2 + 1] = 5000 * sinf(2 * 3.1415 * t++ / 440);
    }
#endif
    MixToBuffer(mp, audio_buffer, buf, nr);

#if 0
    static FILE *f;
    if (!f)f = fopen("out.pcm", "wb");
    fwrite(audio_buffer, 4, nr, f);
    fflush(f);
#endif
    audio_samples -= nr, audio_buffer += nr * 2;
  } while (audio_samples != 0);
}

// Maintain a queue cause the snes and audio callback are not in sync.
// One entry captures the state of all four SNES APU I/O ports (APUI00-APUI03)
// at a single game frame, forming one slot in the lock-free circular queue.
struct ApuWriteEnt {
  uint8 ports[4];
};
// g_apu_write_ents: circular buffer of 16 frame snapshots.
// g_apu_write: accumulator for the current game frame (flushed by ZeldaPushApuState).
// g_apu_write_ent_pos: write head (modulo 16). g_apu_write_count: unread entries.
// g_apu_total_write: total pushes since last reset (used by discard logic).
static struct ApuWriteEnt g_apu_write_ents[16], g_apu_write;
static uint8 g_apu_write_ent_pos, g_apu_write_count, g_apu_total_write;

// Records a single APU I/O port write into the current frame accumulator.
// adr: port address (masked to low 2 bits → 0-3 for APUI00-APUI03).
// val: the byte value the game is writing to that port.
// Does not flush immediately; ZeldaPushApuState queues the full frame.
void zelda_apu_write(uint32_t adr, uint8_t val) {
  g_apu_write.ports[adr & 0x3] = val;
}

// Commits the current game frame's accumulated APU port state into the circular queue.
// Called once per game frame (after all zelda_apu_write calls for that frame).
// Caps g_apu_write_count at 16 to prevent overflow; increments g_apu_total_write
// unconditionally so ZeldaDiscardUnusedAudioFrames can detect stale queue entries.
// Acquires/releases the APU lock so it is safe to call from the game thread while
// ZeldaRenderAudio is running on the audio callback thread.
void ZeldaPushApuState() {
  ZeldaApuLock();
  g_apu_write_ents[g_apu_write_ent_pos++ & 0xf] = g_apu_write;
  if (g_apu_write_count < 16)
    g_apu_write_count++;
  g_apu_total_write++;
  ZeldaApuUnlock();
}

// Dequeues the oldest pending APU frame and copies its port bytes into the SPC player's
// input_ports array so the SPC700 emulator processes them during the next render.
// Called from ZeldaRenderAudio (under the APU lock). If the queue is empty,
// the SPC player retains its previous input_ports value (no-op).
static void ZeldaPopApuState() {
  if (g_apu_write_count != 0)
    memcpy(g_zenv.player->input_ports, &g_apu_write_ents[(g_apu_write_ent_pos - g_apu_write_count--) & 0xf], 4);
}

// Prevents the APU queue from accumulating stale duplicate frames when the audio
// callback runs faster than the game loop (e.g., during frame-rate drops).
// If the oldest queued entry is identical to the SPC player's current input_ports
// (meaning the game hasn't changed the APU state), and 16 or more consecutive
// identical frames have been pushed (g_apu_total_write >= 16), discard one queue
// entry (g_apu_write_count--) and reset the counter to 14. This keeps the audio
// callback's queue depth from growing without bound while still allowing short
// bursts of identical writes (normal during music loops).
// Resets g_apu_total_write to 0 if the APU state changes, so the countdown restarts.
void ZeldaDiscardUnusedAudioFrames() {
  if (g_apu_write_count != 0 && memcmp(g_zenv.player->input_ports, &g_apu_write_ents[(g_apu_write_ent_pos - g_apu_write_count) & 0xf], 4) == 0) {
    if (g_apu_total_write >= 16) {
      g_apu_total_write = 14;
      g_apu_write_count--;
    }
  } else {
    g_apu_total_write = 0;
  }
}

// Clears all pending entries from the APU circular queue. Called after a save-state
// load or reset to prevent stale pre-load APU commands from being fed to the SPC
// player alongside the freshly-restored SPC state.
static void ZeldaResetApuQueue() {
  g_apu_write_ent_pos = g_apu_total_write = g_apu_write_count = 0;
}

uint8_t zelda_read_apui00() {
  // This needs to be here because the ancilla code reads
  // from the apu and we don't want to make the core code
  // dependent on the apu timings, so relocated this value
  // to 0x648.
  return g_ram[kRam_APUI00];
}

// Reads from one of the four SPC700 output ports (port_to_snes[0..3]).
// These are the bytes the SPC700 program writes back to the SNES CPU to signal
// completion of music commands (e.g., track-loaded acknowledgement).
// adr is masked to 2 bits, yielding port indices 0-3.
uint8_t zelda_apu_read(uint32_t adr) {
  return g_zenv.player->port_to_snes[adr & 0x3];
}

// Main audio callback, called by the platform audio driver each time it needs samples.
// audio_buffer: interleaved output buffer (stereo: L/R pairs; mono: single channel).
// samples: number of sample frames requested (each frame = 1 or 2 int16 values).
// channels: 1 (mono) or 2 (stereo).
// Execution sequence (all under APU lock):
//   1. ZeldaPopApuState  — feeds the oldest queued APU command to the SPC700.
//   2. SpcPlayer_GenerateSamples — runs the SPC700 emulator for one audio frame.
//   3. dsp_getSamples    — reads the DSP output (SPC SFX + music) into audio_buffer.
//   4. MsuPlayer_Mix     — if an MSU track is open and output is stereo, overlays
//                          the decoded MSU stream on top of the SPC output in-place.
void ZeldaRenderAudio(int16 *audio_buffer, int samples, int channels) {
  ZeldaApuLock();
  ZeldaPopApuState();
  SpcPlayer_GenerateSamples(g_zenv.player);
  dsp_getSamples(g_zenv.player->dsp, audio_buffer, samples, channels);
  if (g_msu_player.f && channels == 2)
    MsuPlayer_Mix(&g_msu_player, audio_buffer, samples);
  ZeldaApuUnlock();
}

// Returns true if any music is currently active.
// MSU path: returns true when state is Playing or Resuming (not Idle or FinishedPlaying).
// SPC path: returns true when the SPC700 output port 0 is non-zero (the SPC program
//   writes 0 to port_to_snes[0] when music stops, non-zero while a track is loaded).
bool ZeldaIsMusicPlaying() {
  if (g_msu_player.state != kMsuState_Idle) {
    return g_msu_player.state != kMsuState_FinishedPlaying;
  } else {
    return g_zenv.player->port_to_snes[0] != 0;
  }
}

// Restores the complete audio engine state after a save-state load or emulator reset.
// Must be called while the APU lock is held. is_reset: true when restarting from
// the beginning (calls SpcPlayer_Initialize to wipe the SPC700 state), false for
// a mid-game save-state load (SPC RAM is already loaded from the snapshot).
//
// Steps:
//   1. SpcPlayer_CopyVariablesFromRam — rebuilds SPC700 CPU registers from the RAM dump.
//   2. Restores timer_cycles = 0 (not stored in the snapshot).
//   3. Restores input_ports from SPC RAM address 0x410 (where ZeldaSaveMusicStateToRam_Locked
//      persisted the last APU write), and mirrors that into g_apu_write.
//   4. MSU: if enabled, resets volume to 0 (will ramp up) and calls MsuPlayer_Open with
//      resume_from_snapshot=true to reopen the track at the saved position. If the snapshot
//      was taken during a volume transition, reconstructs the mid-transition ramp parameters
//      from msu_volume and last_music_control so the fade continues from where it left off.
//      Sends 0xF0 to the SPC if the MSU is now active (pauses SPC music).
//   5. ZeldaResetApuQueue — discards any pre-load queued APU commands.
void ZeldaRestoreMusicAfterLoad_Locked(bool is_reset) {
  // Restore spc variables from the ram dump.
  SpcPlayer_CopyVariablesFromRam(g_zenv.player);
  // This is not stored in the snapshot
  g_zenv.player->timer_cycles = 0;

  // Restore input ports state
  SpcPlayer *spc_player = g_zenv.player;
  memcpy(spc_player->input_ports, &spc_player->ram[0x410], 4);
  memcpy(g_apu_write.ports, spc_player->input_ports, 4);

  if (is_reset) {
    SpcPlayer_Initialize(g_zenv.player);
  }

  MsuPlayer *mp = &g_msu_player;
  if (mp->enabled) {
    mp->volume = 0.0;
    MsuPlayer_Open(mp, (music_unk1 == 0xf1) ? ((MsuPlayerResumeInfo*)msu_resume_info)->orig_track : 
                   music_unk1, true);

    // If resuming in the middle of a transition, then override
    // the volume with that of the transition.
    if (last_music_control >= 0xf1 && last_music_control <= 0xf3) {
      uint8 target = kVolumeTransitionTarget[last_music_control - 0xf1];
      if (target != msu_volume) {
        float f = kVolumeTransitionTargetFloat[3] * (1.0f / 255);
        mp->volume = msu_volume * f;
        mp->volume_target = target * f;
        mp->volume_step = kVolumeTransitionStepFloat[last_music_control - 0xf1];
      }
    }

    if (g_msu_player.state)
      zelda_apu_write(APUI00, 0xf0);  // pause spc player
  }
  ZeldaResetApuQueue();
}

// Persists the audio engine state into the game RAM snapshot so it can be restored
// by ZeldaRestoreMusicAfterLoad_Locked. Must be called while the APU lock is held.
//
// Steps:
//   1. SpcPlayer_CopyVariablesToRam — writes SPC700 CPU registers into the SPC RAM dump.
//   2. Stores the most recently written APU port bytes (g_apu_write.ports) into SPC RAM
//      at address 0x410 (a free memory range), because SpcPlayer_CopyVariablesToRam does
//      not persist input_ports and the queue may contain a more recent write than the SPC
//      has consumed yet.
//   3. Converts mp->volume (float [0,1]) to an 8-bit integer (msu_volume) for the snapshot.
//   4. Copies the live resume_info into msu_resume_info for the snapshot.
void ZeldaSaveMusicStateToRam_Locked() {
  SpcPlayer_CopyVariablesToRam(g_zenv.player);
  // SpcPlayer.input_ports is not saved to the SpcPlayer ram by SpcPlayer_CopyVariablesToRam,
  // in any case, we want to save the most recently written data, and that might still
  // be in the queue. 0x410 is a free memory location in the SPC ram, so store it there.
  SpcPlayer *spc_player = g_zenv.player;
  memcpy(&spc_player->ram[0x410], g_apu_write.ports, 4);

  msu_volume = g_msu_player.volume * 255;
  memcpy(msu_resume_info, &g_msu_player.resume_info, sizeof(g_msu_player.resume_info));
}

// Configures the MSU player subsystem at startup.
// enable: bitmask of kMsuEnabled_* flags (0 = MSU disabled; use SPC for everything).
//   kMsuEnabled_Opuz: decode .opuz (Opus) files at 48 kHz instead of raw PCM at 44.1 kHz.
//   kMsuEnabled_MsuDeluxe: activate the per-area and per-entrance track remapping tables.
// Warns to stderr if the configured audio_freq doesn't match the required sample rate.
// Precomputes kVolumeTransitionStepFloat / kVolumeTransitionTargetFloat from the integer
// tables, scaling them by g_config.msuvolume (0-100%) and g_config.audio_freq so that
// the per-sample volume step produces the correct fade speed regardless of sample rate.
//   volscale   = msuvolume / (255 * 100)  — maps 0-255 × 0-100% to 0.0-1.0.
//   stepscale  = msuvolume * 60 / (256 * 100 * audio_freq) — steps per sample for a
//                60-Hz game clock; divides the integer step by 256 and the frame rate.
void ZeldaEnableMsu(uint8 enable) {
  g_msu_player.volume = 1.0f;
  g_msu_player.enabled = enable;
  if (enable & kMsuEnabled_Opuz) {
    if (g_config.audio_freq != 48000)
      fprintf(stderr, "Warning: MSU Opuz requires: AudioFreq = 48000\n");
  } else if (enable) {
    if (g_config.audio_freq != 44100)
      fprintf(stderr, "Warning: MSU requires: AudioFreq = 44100\n");
  }

  float volscale = g_config.msuvolume * (1.0f / 255 / 100);
  float stepscale = g_config.msuvolume * (60.0f / 256 / 100) / g_config.audio_freq;
  for (size_t i = 0; i != countof(kVolumeTransitionStepFloat); i++) {
    kVolumeTransitionStepFloat[i] = kVolumeTransitionStep[i] * stepscale;
    kVolumeTransitionTargetFloat[i] = kVolumeTransitionTarget[i] * volscale;
  }
}

// Uploads a compiled SPC music bank binary to the SPC700 player.
// p: pointer to the packed music data (track headers + instrument samples + sequence data)
//    produced by the SNES music compiler. Called from game module 8 (LoadSongBank)
//    whenever the dungeon or overworld music set changes.
// Acquires the APU lock around SpcPlayer_Upload so the audio callback does not attempt
// to render samples while the SPC RAM is being overwritten.
void LoadSongBank(const uint8 *p) {  // 808888
  ZeldaApuLock();
  SpcPlayer_Upload(g_zenv.player, p);
  ZeldaApuUnlock();
}
