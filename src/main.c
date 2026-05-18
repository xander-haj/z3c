/*
 * src/main.c
 *
 * SDL2-based platform entry point and host shell for the zelda3 reimplementation.
 *
 * Responsibilities of this file:
 *   - Process startup: parse the command line, locate and load zelda3.ini,
 *     load assets (zelda3_assets.dat or BPS-patched ROM), and bring up the
 *     core game runtime via ZeldaInitialize() in zelda_rtl.c.
 *   - Window/renderer bring-up: open the SDL window, choose between the SDL
 *     renderer (kSdlRendererFuncs in this file) and the OpenGL renderer
 *     (provided by opengl.c via OpenGLRenderer_Create), and route every PPU
 *     frame through the selected RendererFuncs vtable.
 *   - Audio bring-up: open an SDL audio device, run the AudioCallback on
 *     SDL's audio thread, and pull mixed PCM from ZeldaRenderAudio (audio.c)
 *     while serializing all APU/RTL access through g_audio_mutex (also
 *     exposed to the rest of the engine as ZeldaApuLock / ZeldaApuUnlock).
 *   - Main loop: pump SDL events, fold keyboard + gamepad state into the
 *     SNES joypad-1 bitfield, drive ZeldaRunFrame() once per simulated
 *     frame, present the PPU output via DrawPpuFrameWithPerf(), and pace
 *     to ~60 FPS using a 17/17/16 ms triangular delay schedule.
 *   - Input mapping: translate SDL key/button events into the kKeys_*
 *     command space (see config.c) and dispatch them through HandleCommand,
 *     including save-state slots, replay control, cheats, fullscreen
 *     toggling, window scaling, volume, and the on-screen perf overlay.
 *   - Misc helpers: a tiny bitmap-font number renderer for the FPS overlay,
 *     a custom HitTest callback that fakes border-drag/resize for the
 *     borderless-window mode, an atan2 approximation used to translate
 *     analog-stick deflection into 8-way d-pad output, and the optional
 *     ZSPR Link-graphics override loader.
 *
 * This file is the only platform-aware source in src/. Everything below
 * zelda_rtl is portable C; everything in this file talks to SDL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL.h>
#ifdef _WIN32
#include "platform/win32/volume_control.h"
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "snes/ppu.h"

#include "types.h"
#include "variables.h"

#include "zelda_rtl.h"
#include "zelda_cpu_infra.h"

#include "config.h"
#include "assets.h"
#include "load_gfx.h"
#include "util.h"
#include "audio.h"

/* Build-time toggle. When true, main() skips LoadRom() so the host can run
 * the asset-driven C reimplementation without ever loading the original
 * SNES ROM. Left at false in normal builds; flipping it disables the
 * 65C816 fallback path entirely. */
static bool g_run_without_emu = 0;

/* Forward declarations for the static helpers defined later in this file.
 * Grouped here so main() can call them in any order without needing the
 * definitions to be lexically earlier. */
// Forwards
static bool LoadRom(const char *filename);
static void LoadLinkGraphics();
static void RenderNumber(uint8 *dst, size_t pitch, int n, bool big);
static void HandleInput(int keyCode, int modCode, bool pressed);
static void HandleCommand(uint32 j, bool pressed);
static int RemapSdlButton(int button);
static void HandleGamepadInput(int button, bool pressed);
static void HandleGamepadAxisInput(int gamepad_id, int axis, int value);
static void OpenOneGamepad(int i);
static void HandleVolumeAdjustment(int volume_adjustment);
static void LoadAssets();
static void SwitchDirectory();

/* Default values used when the parsed config (zelda3.ini) leaves a setting
 * unspecified or out of range. kMaxWindowScale caps integer window
 * upscaling at 10x to keep ChangeWindowScale() from picking absurd sizes
 * on huge displays. kDefaultFreq/Channels/Samples seed the SDL_AudioSpec
 * if the INI does not override them. */
enum {
  kDefaultFullscreen = 0,
  kMaxWindowScale = 10,
  kDefaultFreq = 44100,
  kDefaultChannels = 2,
  kDefaultSamples = 2048,
};

/* Window title used both for the SDL window and as the prefix for the
 * "title with FPS" overlay when g_config.display_perf_title is enabled. */
static const char kWindowTitle[] = "The Legend of Zelda: A Link to the Past";
/* Initial SDL_CreateWindow flag set. Modified at startup to OR in
 * SDL_WINDOW_FULLSCREEN(_DESKTOP) / SDL_WINDOW_OPENGL based on config,
 * and at runtime to toggle SDL_WINDOW_BORDERLESS via shift+double-click. */
static uint32 g_win_flags = SDL_WINDOW_RESIZABLE;
/* The single top-level SDL window. Stashed in a global so HitTestCallback
 * and the various command handlers can mutate it without threading it
 * through every call. */
static SDL_Window *g_window;

/* Runtime UI state shared across the event pump and the frame loop.
 *   g_paused        - true while the simulation is frozen (SDL audio is
 *                     also paused so it does not loop the last block).
 *   g_turbo         - true while a turbo key is held; the loop drops every
 *                     non-16th frame to fast-forward.
 *   g_replay_turbo  - whether replays auto-fast-forward; toggled by a key.
 *   g_cursor        - mouse cursor visibility, hidden in fullscreen. */
static uint8 g_paused, g_turbo, g_replay_turbo = true, g_cursor = true;
/* Integer window scale (1x..kMaxWindowScale). Adjusted by ChangeWindowScale. */
static uint8 g_current_window_scale;
/* SNES joypad-1 bits derived from gamepad input. Cleared whenever the
 * keyboard supplies its own d-pad direction so the two sources cannot
 * fight each other. */
static uint8 g_gamepad_buttons;
/* SNES joypad-1 bits derived from keyboard input. ORed with
 * g_gamepad_buttons before being handed to ZeldaRunFrame. */
static int g_input1_state;
/* Whether to render the on-screen FPS counter via RenderNumber. */
static bool g_display_perf;
/* Smoothed FPS value (rolling average over 64 frames) used by both the
 * on-screen overlay and the optional window-title display. */
static int g_curr_fps;
/* Bitmask of kPpuRenderFlags_* selecting renderer features (new renderer,
 * 4x4 mode 7, 240-line height, no sprite limits). Built once at startup
 * from g_config and toggled by kKeys_ToggleRenderer. */
static int g_ppu_render_flags = 0;
/* Logical SNES framebuffer dimensions. Width depends on the extended
 * aspect ratio (256 + 2*extra), height is 224 or 240. */
static int g_snes_width, g_snes_height;
/* Software mixer attenuation applied in AudioCallback when the host
 * does not expose a system-volume API. SDL_MIX_MAXVOLUME == no attenuation. */
static int g_sdl_audio_mixer_volume = SDL_MIX_MAXVOLUME;
/* Vtable of renderer entry points. Pointed at either kSdlRendererFuncs
 * (defined in this file) or the OpenGL implementation in opengl.c. */
static struct RendererFuncs g_renderer_funcs;
/* Bitmask of currently-pressed gamepad buttons. Acts as a "modifier"
 * state so HandleGamepadInput can find chord-mapped commands in
 * config.c's gamepad bind table. */
static uint32 g_gamepad_modifiers;
/* Sticky cache of the last command resolved per gamepad button. The
 * release event needs to fire the same command as the matching press
 * even if the chord modifiers have since changed. */
static uint16 g_gamepad_last_cmd[kGamepadBtn_Count];

/* Die: fatal-error sink used everywhere in the engine for unrecoverable
 * conditions (missing assets, corrupt files, OS resource failures, etc.).
 * On a Windows release build it pops up an error message box so users
 * launching from Explorer can see what happened; otherwise it logs the
 * message to stderr. Always terminates the process and never returns.
 *
 * Parameters:
 *   error - human-readable description displayed verbatim to the user.
 */
void NORETURN Die(const char *error) {
#if defined(NDEBUG) && defined(_WIN32)
  /* Release builds on Windows surface a GUI dialog so users without a
   * console see the failure reason. Debug builds rely on the stderr
   * print below to keep the dialog from interrupting the debugger. */
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kWindowTitle, error, NULL);
#endif
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}

/* ChangeWindowScale: bumps the integer SNES->screen upscaling factor up
 * or down by `scale_step`, clamps it against both kMaxWindowScale and the
 * largest scale that still fits inside the current display's usable
 * bounds (taking the OS title-bar/borders into account), resizes the
 * window, and recenters it on the mouse cursor.
 *
 * No-op when the window is in any fullscreen, minimized, or maximized
 * state - those states own the window geometry and we must not fight them.
 *
 * Parameters:
 *   scale_step - +1 to grow, -1 to shrink. Clamped to [1, max_scale].
 */
void ChangeWindowScale(int scale_step) {
  /* Refuse to resize when the window is not in a normal floating state -
   * SDL_SetWindowSize on a fullscreen/maximized window is undefined and
   * fights the window manager. */
  if ((SDL_GetWindowFlags(g_window) & (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_MAXIMIZED)) != 0)
    return;
  /* Pick the display the window currently lives on; fall back to the
   * primary display if SDL cannot determine it (e.g. window not yet shown). */
  int screen = SDL_GetWindowDisplayIndex(g_window);
  if (screen < 0) screen = 0;
  int max_scale = kMaxWindowScale;
  SDL_Rect bounds;
  /* bt is sentinel-initialized to -1; the recenter block below uses
   * (bt >= 0) to distinguish "we have real border data" from "we have
   * no display info, just center on the screen". */
  int bt = -1, bl, bb, br;
  // note this takes into effect Windows display scaling, i.e., resolution is divided by scale factor
  if (SDL_GetDisplayUsableBounds(screen, &bounds) == 0) {
    // this call may take a while before it is reported by Windows (or not at all in my testing)
    if (SDL_GetWindowBordersSize(g_window, &bt, &bl, &bb, &br) != 0) {
      // guess based on Windows 10/11 defaults
      bl = br = bb = 1;
      bt = 31;
    }
    // Allow a scale level slightly above the max that fits on screen
    /* The +g_snes_/4 bias rounds the integer division up by ~25%, so a
     * scale that overflows the usable area by less than a quarter of the
     * SNES frame is still considered "fits" - this lets users pick a
     * slightly oversized scale on borderline displays. */
    int mw = (bounds.w - bl - br + g_snes_width / 4) / g_snes_width;
    int mh = (bounds.h - bt - bb + g_snes_height / 4) / g_snes_height;
    max_scale = IntMin(mw, mh);
  }
  /* Clamp the requested scale into [1, max_scale]. The lower bound of 1
   * prevents scrolling past the smallest legible window. */
  int new_scale = IntMax(IntMin(g_current_window_scale + scale_step, max_scale), 1);
  g_current_window_scale = new_scale;
  int w = new_scale * g_snes_width;
  int h = new_scale * g_snes_height;

  //SDL_RenderSetLogicalSize(g_renderer, w, h);
  SDL_SetWindowSize(g_window, w, h);
  if (bt >= 0) {
    // Center the window on top of the mouse
    /* Center on the cursor but clamp so the window stays inside the
     * usable display area accounting for OS borders on each side. */
    int mx, my;
    SDL_GetGlobalMouseState(&mx, &my);
    int wx = IntMax(IntMin(mx - w / 2, bounds.x + bounds.w - bl - br - w), bounds.x + bl);
    int wy = IntMax(IntMin(my - h / 2, bounds.y + bounds.h - bt - bb - h), bounds.y + bt);
    SDL_SetWindowPosition(g_window, wx, wy);
  } else {
    /* No display info - let SDL pick a centered position on the
     * default display. */
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
}

/* Width in pixels of the invisible drag/resize border around a borderless
 * window. Anything inside this band of the window edge is reported to SDL
 * as a resize hit zone, which lets the user grab and resize the borderless
 * window with the mouse as if it had real OS borders. */
#define RESIZE_BORDER 20
/* HitTestCallback: SDL hit-test hook installed via SDL_SetWindowHitTest.
 * Tells SDL which screen regions of our (potentially borderless) window
 * should be treated as draggable, resizable, or normal client area.
 *
 * Behavior:
 *   - In any fullscreen mode, every pixel is normal client area; we never
 *     want SDL to start a window-drag or resize on a fullscreen window.
 *   - Holding Ctrl turns the entire window into a drag handle, so users
 *     can move a borderless window without exposing the title bar.
 *   - Otherwise the outer RESIZE_BORDER pixels on each edge map to the
 *     matching SDL resize directions (corners take priority over edges).
 *
 * Parameters:
 *   win  - the SDL window receiving the hit test (always g_window).
 *   pt   - mouse position in window-relative coordinates.
 *   data - unused user pointer (we passed NULL on registration).
 *
 * Returns the SDL_HITTEST_* code SDL should act on.
 */
static SDL_HitTestResult HitTestCallback(SDL_Window *win, const SDL_Point *pt, void *data) {
  uint32 flags = SDL_GetWindowFlags(win);
  /* Fullscreen windows must report HITTEST_NORMAL everywhere - any
   * draggable/resizable region would let SDL try to move a window the
   * window manager owns. */
  if ((flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0 || (flags & SDL_WINDOW_FULLSCREEN) != 0)
    return SDL_HITTEST_NORMAL;

  /* Ctrl-anywhere acts as a global drag handle for borderless mode. */
  if ((SDL_GetModState() & KMOD_CTRL) != 0)
    return SDL_HITTEST_DRAGGABLE;

  int w, h;
  SDL_GetWindowSize(win, &w, &h);

  /* Edge / corner detection. The y-axis is checked first so the top and
   * bottom RESIZE_BORDER bands cover the corners; only when y is in the
   * middle band do we look at the left/right edges. */
  if (pt->y < RESIZE_BORDER) {
    return (pt->x < RESIZE_BORDER) ? SDL_HITTEST_RESIZE_TOPLEFT :
           (pt->x >= w - RESIZE_BORDER) ? SDL_HITTEST_RESIZE_TOPRIGHT : SDL_HITTEST_RESIZE_TOP;
  } else if (pt->y >= h - RESIZE_BORDER) {
    return (pt->x < RESIZE_BORDER) ? SDL_HITTEST_RESIZE_BOTTOMLEFT :
           (pt->x >= w - RESIZE_BORDER) ? SDL_HITTEST_RESIZE_BOTTOMRIGHT : SDL_HITTEST_RESIZE_BOTTOM;
  } else {
    if (pt->x < RESIZE_BORDER) {
      return SDL_HITTEST_RESIZE_LEFT;
    } else if (pt->x >= w - RESIZE_BORDER) {
      return SDL_HITTEST_RESIZE_RIGHT;
    }
  }
  return SDL_HITTEST_NORMAL;
}

/* DrawPpuFrameWithPerf: presents one fully-rendered PPU frame, with
 * optional rolling-average FPS measurement. Called once per simulated
 * frame from the main loop after ZeldaRunFrame.
 *
 * Flow:
 *   1. Ask the PPU what render scale it currently needs (1x normally,
 *      4x when 4x4 mode-7 enhancement is enabled).
 *   2. Lock the renderer's back buffer via the RendererFuncs vtable.
 *   3. Render the PPU into that buffer; if perf measurement is on, time
 *      the call with SDL_GetPerformanceCounter and fold it into a
 *      64-sample rolling average so g_curr_fps is stable.
 *   4. Optionally overlay the FPS digit string in the top-left corner.
 *   5. Hand the buffer back to the renderer to present on screen.
 */
static void DrawPpuFrameWithPerf() {
  int render_scale = PpuGetCurrentRenderScale(g_zenv.ppu, g_ppu_render_flags);
  uint8 *pixel_buffer = 0;
  int pitch = 0;

  g_renderer_funcs.BeginDraw(g_snes_width * render_scale,
                             g_snes_height * render_scale,
                             &pixel_buffer, &pitch);
  /* Two paths: the timed path measures one PPU frame and folds it into
   * a 64-entry rolling average (the running sum is maintained in
   * `average` and updated incrementally as new samples replace old).
   * The untimed path is the steady-state hot path when no perf overlay
   * is requested. */
  if (g_display_perf || g_config.display_perf_title) {
    static float history[64], average;
    static int history_pos;
    uint64 before = SDL_GetPerformanceCounter();
    ZeldaDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
    uint64 after = SDL_GetPerformanceCounter();
    /* Convert elapsed counter ticks into "frames per second this single
     * frame achieved" - frequency / delta == samples per second. */
    float v = (double)SDL_GetPerformanceFrequency() / (after - before);
    /* Incremental rolling-sum update: subtract the value being evicted
     * and add the new sample, so we never re-sum the whole window. */
    average += v - history[history_pos];
    history[history_pos] = v;
    history_pos = (history_pos + 1) & 63;
    g_curr_fps = average * (1.0f / 64);
  } else {
    ZeldaDrawPpuFrame(pixel_buffer, pitch, g_ppu_render_flags);
  }
  /* Draw the FPS digits one render-scale row down from the top so the
   * "big" font (used at 4x render scale) does not get clipped. */
  if (g_display_perf)
    RenderNumber(pixel_buffer + pitch * render_scale, pitch, g_curr_fps, render_scale == 4);
  g_renderer_funcs.EndDraw();
}

/* Mutex serializing every access to the engine's audio/APU state.
 * Held by the SDL audio thread inside AudioCallback, by the main thread
 * around ZeldaRunFrame and most save/load/reset commands, and exposed
 * to the rest of the engine via ZeldaApuLock / ZeldaApuUnlock so audio
 * code anywhere in the project can use the same lock without depending
 * on SDL types. */
static SDL_mutex *g_audio_mutex;
/* Cached PCM buffer used by AudioCallback. ZeldaRenderAudio writes one
 * full block of frames at a time into g_audiobuffer; g_audiobuffer_cur
 * and g_audiobuffer_end track how much of that block remains unread by
 * SDL. When _cur catches up to _end the next callback invocation pulls
 * a fresh block from the engine. */
static uint8 *g_audiobuffer, *g_audiobuffer_cur, *g_audiobuffer_end;
/* Number of stereo (or mono) frames the engine produces per render call.
 * Computed at startup as 534 * sample_rate / 32000 - this preserves the
 * native ~32kHz SPC700 block size when resampled to the host rate. */
static int g_frames_per_block;
/* Channel count negotiated with the SDL audio device (1 or 2). */
static uint8 g_audio_channels;

/* AudioCallback: SDL audio thread entry point. Pulls PCM frames out of
 * the engine's audio mixer (audio.c via ZeldaRenderAudio) and copies
 * them into SDL's stream buffer until SDL's request is satisfied.
 *
 * Threading: runs on SDL's dedicated audio thread, which is why every
 * audio.c / spc_player.c access in this file goes through g_audio_mutex.
 *
 * Volume handling: when the software mixer is at max volume we memcpy
 * directly to avoid any per-sample work; otherwise we use SDL's
 * format-aware mixer to attenuate. The destination buffer must be
 * zeroed first because SDL_MixAudioFormat *adds* into it.
 *
 * Parameters:
 *   userdata - unused (we passed NULL when opening the device).
 *   stream   - destination PCM buffer SDL will play.
 *   len      - byte length of `stream`. May span multiple engine blocks.
 */
static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len) {
  if (SDL_LockMutex(g_audio_mutex)) Die("Mutex lock failed!");
  /* Pull-loop: top up the local buffer with a fresh engine block whenever
   * it drains, then copy as many bytes as fit into SDL's request.
   * Repeat until SDL's stream is full. */
  while (len != 0) {
    if (g_audiobuffer_end - g_audiobuffer_cur == 0) {
      ZeldaRenderAudio((int16*)g_audiobuffer, g_frames_per_block, g_audio_channels);
      g_audiobuffer_cur = g_audiobuffer;
      g_audiobuffer_end = g_audiobuffer + g_frames_per_block * g_audio_channels * sizeof(int16);
    }
    int n = IntMin(len, g_audiobuffer_end - g_audiobuffer_cur);
    if (g_sdl_audio_mixer_volume == SDL_MIX_MAXVOLUME) {
      /* Fast path: no software volume scaling, just copy. */
      memcpy(stream, g_audiobuffer_cur, n);
    } else {
      /* Slow path: SDL_MixAudioFormat sums into the destination, so we
       * must clear it first or we would mix into stale buffer contents. */
      SDL_memset(stream, 0, n);
      SDL_MixAudioFormat(stream, g_audiobuffer_cur, AUDIO_S16, n, g_sdl_audio_mixer_volume);
    }
    g_audiobuffer_cur += n;
    stream += n;
    len -= n;
  }

  /* Tell the engine to drop any frames it has queued that we will never
   * play (happens when the host sleeps or hitches and the queue grew).
   * Keeps audio latency bounded across the lifetime of the process. */
  ZeldaDiscardUnusedAudioFrames();
  SDL_UnlockMutex(g_audio_mutex);
}

// State for sdl renderer
/* SDL renderer back end (used unless the user picked the OpenGL backend
 * in zelda3.ini). g_renderer is the renderer object, g_texture is the
 * streaming target the PPU writes into, and g_sdl_renderer_rect tracks
 * the rect that was last locked so EndDraw knows what to copy out. */
static SDL_Renderer *g_renderer;
static SDL_Texture *g_texture;
static SDL_Rect g_sdl_renderer_rect;

/* SdlRenderer_Init: bring up the SDL renderer back end on `window`.
 * Picks software vs accelerated based on g_config.output_method, sets
 * up logical-size scaling so the SNES image keeps its aspect ratio
 * unless the user opted out, optionally enables linear filtering, and
 * allocates the streaming texture (4x oversized when 4x4 mode-7
 * enhancement is requested).
 *
 * Returns true on success, false (with an error printed) on failure -
 * a false return propagates out of main() as a process exit.
 */
static bool SdlRenderer_Init(SDL_Window *window) {

  /* Shader support lives in glsl_shader.c which only the OpenGL renderer
   * wires up. Warn (don't fail) so users who switch back to the SDL
   * backend with a shader still in their INI know it is being ignored. */
  if (g_config.shader)
    fprintf(stderr, "Warning: Shaders are supported only with the OpenGL backend\n");

  /* Pick software vs accelerated rendering. The accelerated path also
   * requests vsync; the software path does not (it is meant as a last
   * resort fallback when GPU drivers misbehave). */
  SDL_Renderer *renderer = SDL_CreateRenderer(g_window, -1,
                                              g_config.output_method == kOutputMethod_SDLSoftware ? SDL_RENDERER_SOFTWARE :
                                              SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (renderer == NULL) {
    printf("Failed to create renderer: %s\n", SDL_GetError());
    return false;
  }
  SDL_RendererInfo renderer_info;
  SDL_GetRendererInfo(renderer, &renderer_info);
  /* Debug-only dump of every texture format the chosen backend can
   * actually accept - useful when porting to a new platform whose
   * driver might not support ARGB8888 streaming. */
  if (kDebugFlag) {
    printf("Supported texture formats:");
    for (int i = 0; i < renderer_info.num_texture_formats; i++)
      printf(" %s", SDL_GetPixelFormatName(renderer_info.texture_formats[i]));
    printf("\n");
  }
  g_renderer = renderer;
  /* Logical-size scaling lets SDL pillarbox/letterbox automatically so
   * the SNES aspect ratio survives an arbitrary window size. The user
   * can disable it to allow free stretching. */
  if (!g_config.ignore_aspect_ratio)
    SDL_RenderSetLogicalSize(renderer, g_snes_width, g_snes_height);
  if (g_config.linear_filtering)
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

  /* When 4x4 mode-7 enhancement is on the PPU renders directly into a
   * 4x oversized buffer, so the texture must match. Otherwise we render
   * at native SNES resolution and let the GPU upscale. */
  int tex_mult = (g_ppu_render_flags & kPpuRenderFlags_4x4Mode7) ? 4 : 1;
  g_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                g_snes_width * tex_mult, g_snes_height * tex_mult);
  if (g_texture == NULL) {
    printf("Failed to create texture: %s\n", SDL_GetError());
    return false;
  }
  return true;
}

/* SdlRenderer_Destroy: tear down the texture and renderer pair created
 * by SdlRenderer_Init. Called once during shutdown via the RendererFuncs
 * vtable; safe to call exactly once after a successful Init. */
static void SdlRenderer_Destroy() {
  SDL_DestroyTexture(g_texture);
  SDL_DestroyRenderer(g_renderer);
}

/* SdlRenderer_BeginDraw: lock the streaming texture for direct CPU
 * writing and hand the locked region back to the caller as a raw
 * pixel pointer + row pitch. The PPU writes pixels through this pointer.
 *
 * Parameters:
 *   width  - width of the region to lock (allows the PPU to render at
 *            either native or 4x oversized size into the same texture).
 *   height - height of the region to lock.
 *   pixels - out: base pointer of the locked pixel data.
 *   pitch  - out: row stride in bytes.
 */
static void SdlRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  g_sdl_renderer_rect.w = width;
  g_sdl_renderer_rect.h = height;
  if (SDL_LockTexture(g_texture, &g_sdl_renderer_rect, (void **)pixels, pitch) != 0) {
    printf("Failed to lock texture: %s\n", SDL_GetError());
    return;
  }
}

/* SdlRenderer_EndDraw: unlock the streaming texture, blit it onto the
 * back buffer, and present. SDL_RenderPresent vsyncs to ~60Hz when the
 * accelerated/vsync renderer was selected; the manual delay loop in
 * main() compensates when vsync is unavailable. */
static void SdlRenderer_EndDraw() {

//  uint64 before = SDL_GetPerformanceCounter();
  SDL_UnlockTexture(g_texture);
//  uint64 after = SDL_GetPerformanceCounter();
//  float v = (double)(after - before) / SDL_GetPerformanceFrequency();
//  printf("%f ms\n", v * 1000);
  SDL_RenderClear(g_renderer);
  SDL_RenderCopy(g_renderer, g_texture, &g_sdl_renderer_rect, NULL);
  SDL_RenderPresent(g_renderer); // vsyncs to 60 FPS?
}

/* RendererFuncs vtable for the plain-SDL backend. main() copies this
 * into g_renderer_funcs unless the user picked an OpenGL output method,
 * in which case OpenGLRenderer_Create populates the vtable instead. */
static const struct RendererFuncs kSdlRendererFuncs  = {
  &SdlRenderer_Init,
  &SdlRenderer_Destroy,
  &SdlRenderer_BeginDraw,
  &SdlRenderer_EndDraw,
};

/* Defined in opengl.c. Populates `funcs` with an OpenGL-backed
 * implementation of the same vtable; `use_opengl_es` selects between
 * desktop OpenGL and OpenGL ES. */
void OpenGLRenderer_Create(struct RendererFuncs *funcs, bool use_opengl_es);

/* SDL on some platforms (notably Windows) macro-replaces `main` with
 * `SDL_main`. We undef it here so the linker sees a plain main that the
 * platform shim can call directly. */
#undef main
/* main: process entry point.
 *
 * High-level startup sequence:
 *   1. Parse args - optional `--config <file>` overrides the default
 *      INI search; otherwise SwitchDirectory walks up the cwd looking
 *      for zelda3.ini so the binary can be launched from any subfolder.
 *   2. Load configuration, assets, and the optional ZSPR Link sprite.
 *   3. Initialize the engine (ZeldaInitialize) and pick the runtime
 *      PPU geometry / render flags from the config.
 *   4. Bring up SDL video, audio, gamepads, and the chosen renderer.
 *   5. Optionally load a ROM (when LoadRom is needed for the 65C816
 *      reference path) and the SRAM file.
 *   6. Run the main loop until the user closes the window.
 *   7. Tear everything down cleanly so saves get flushed and SDL exits.
 *
 * Returns 0 on a clean shutdown, 1 if SDL/window/renderer init failed.
 */
int main(int argc, char** argv) {
  argc--, argv++;
  const char *config_file = NULL;
  /* `--config <path>` lets the user point at an arbitrary INI file
   * (useful for shipping multiple presets). When absent, fall back to
   * walking the cwd to find a co-located zelda3.ini. */
  if (argc >= 2 && strcmp(argv[0], "--config") == 0) {
    config_file = argv[1];
    argc -= 2, argv += 2;
  } else {
    SwitchDirectory();
  }
  ParseConfigFile(config_file);
  LoadAssets();
  LoadLinkGraphics();

  ZeldaInitialize();
  /* Apply extended-aspect-ratio config to the PPU. The SNES image is
   * 256 px wide; extra columns get rendered on each side, capped at the
   * PPU's compiled-in maximum. Width grows by 2x the per-side extra. */
  g_zenv.ppu->extraLeftRight = UintMin(g_config.extended_aspect_ratio, kPpuExtraLeftRight);
  g_snes_width = (g_config.extended_aspect_ratio * 2 + 256);
  /* Height is the SNES standard 224 unless the user opted into the
   * extended 240-line render mode. */
  g_snes_height = (g_config.extend_y ? 240 : 224);


  // Delay actually setting those features in ram until any snapshots finish playing.
  /* g_wanted_zelda_features stages the requested feature bits; the
   * engine commits them to live state once any in-flight save-state
   * playback finishes, so loading an old replay never sees a half-
   * applied feature flag. */
  g_wanted_zelda_features = g_config.features0;

  /* Pack each boolean config flag into the corresponding PPU render-
   * flag bit. The (bool * BIT) idiom yields BIT when the bool is set
   * and 0 otherwise without needing a chain of conditionals. */
  g_ppu_render_flags = g_config.new_renderer * kPpuRenderFlags_NewRenderer |
                       g_config.enhanced_mode7 * kPpuRenderFlags_4x4Mode7 |
                       g_config.extend_y * kPpuRenderFlags_Height240 |
                       g_config.no_sprite_limits * kPpuRenderFlags_NoSpriteLimits;
  ZeldaEnableMsu(g_config.enable_msu);
  ZeldaSetLanguage(g_config.language);

  /* Two flavors of fullscreen: 1 == "borderless desktop" (matches the
   * desktop resolution, recommended) and 2 == "exclusive mode" (changes
   * the display mode). The XOR toggles the bit so the rest of the code
   * can treat both with a single g_win_flags check. */
  if (g_config.fullscreen == 1)
    g_win_flags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
  else if (g_config.fullscreen == 2)
    g_win_flags ^= SDL_WINDOW_FULLSCREEN;

  // Window scale (1=100%, 2=200%, 3=300%, etc.)
  /* 0 in the INI means "use the default", which is 2x. Anything larger
   * than kMaxWindowScale is clamped so a typo cannot open a 10000x window. */
  g_current_window_scale = (g_config.window_scale == 0) ? 2 : IntMin(g_config.window_scale, kMaxWindowScale);

  // audio_freq: Use common sampling rates (see user config file. values higher than 48000 are not supported.)
  /* Below 11025 the resampler artifacts get unbearable; above 48000 the
   * SPC700/DSP path has no extra fidelity to offer. Out-of-range values
   * snap to kDefaultFreq (44100). */
  if (g_config.audio_freq < 11025 || g_config.audio_freq > 48000)
    g_config.audio_freq = kDefaultFreq;

  // Currently, the SPC/DSP implementation only supports up to stereo.
  if (g_config.audio_channels < 1 || g_config.audio_channels > 2)
    g_config.audio_channels = kDefaultChannels;

  // audio_samples: power of 2
  /* Power-of-two test: a value with no bits in common with (value - 1)
   * is exactly one bit, i.e. a power of two. SDL prefers power-of-two
   * audio buffer sizes for cleaner ring-buffer math. */
  if (g_config.audio_samples <= 0 || ((g_config.audio_samples & (g_config.audio_samples - 1)) != 0))
    g_config.audio_samples = kDefaultSamples;

  // set up SDL
  /* Three SDL subsystems matter to us: video for the window/renderer,
   * audio for the SPC mixer, and game controller for hot-plugged pads. */
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    printf("Failed to init SDL: %s\n", SDL_GetError());
    return 1;
  }

  /* Window dimensions: use the explicit width/height from the INI when
   * both are nonzero, otherwise derive from the current integer scale. */
  bool custom_size  = g_config.window_width != 0 && g_config.window_height != 0;
  int window_width  = custom_size ? g_config.window_width  : g_current_window_scale * g_snes_width;
  int window_height = custom_size ? g_config.window_height : g_current_window_scale * g_snes_height;

  /* Wire up the renderer vtable. The OpenGL backend additionally needs
   * SDL_WINDOW_OPENGL on the window flags so SDL creates a GL context. */
  if (g_config.output_method == kOutputMethod_OpenGL ||
      g_config.output_method == kOutputMethod_OpenGL_ES) {
    g_win_flags |= SDL_WINDOW_OPENGL;
    OpenGLRenderer_Create(&g_renderer_funcs, (g_config.output_method == kOutputMethod_OpenGL_ES));
  } else {
    g_renderer_funcs = kSdlRendererFuncs;
  }

  SDL_Window* window = SDL_CreateWindow(kWindowTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, window_width, window_height, g_win_flags);
  if(window == NULL) {
    printf("Failed to create window: %s\n", SDL_GetError());
    return 1;
  }
  g_window = window;
  /* Install our custom hit-test so the borderless mode supports
   * dragging and edge-resizing without OS-drawn borders. */
  SDL_SetWindowHitTest(window, HitTestCallback, NULL);

  if (!g_renderer_funcs.Initialize(window))
    return 1;

  SDL_AudioDeviceID device = 0;
  SDL_AudioSpec want = { 0 }, have;
  g_audio_mutex = SDL_CreateMutex();
  if (!g_audio_mutex) Die("No mutex");

  /* Audio bring-up. We pass `&have` so SDL can renegotiate the format
   * (sample rate, channel count) - the engine then uses the values it
   * actually got back, not the ones we asked for. */
  if (g_config.enable_audio) {
    want.freq = g_config.audio_freq;
    want.format = AUDIO_S16;
    want.channels = g_config.audio_channels;
    want.samples = g_config.audio_samples;
    want.callback = &AudioCallback;
    device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (device == 0) {
      printf("Failed to open audio device: %s\n", SDL_GetError());
      return 1;
    }
    g_audio_channels = have.channels;
    /* Engine block size: 534 samples per frame at the SPC's native
     * 32000Hz, scaled into the host sample rate. Multiplying by have.freq
     * before dividing keeps the integer math precise. */
    g_frames_per_block = (534 * have.freq) / 32000;
    g_audiobuffer = malloc(g_frames_per_block * have.channels * sizeof(int16));
  }

  /* Optional ROM load (only meaningful when the 65C816 reference path
   * is enabled at build time). The asset-only path skips this entirely. */
  if (argc >= 1 && !g_run_without_emu)
    LoadRom(argv[0]);

  /* Make sure the saves directory exists before SaveLoadSlot tries to
   * write into it. The Windows CRT spelling differs from POSIX mkdir. */
#if defined(_WIN32)
  _mkdir("saves");
#else
  mkdir("saves", 0755);
#endif

  ZeldaReadSram();

  /* Open every gamepad SDL already knows about. New ones plugged in
   * later are picked up by SDL_CONTROLLERDEVICEADDED in the event loop. */
  for (int i = 0; i < SDL_NumJoysticks(); i++)
    OpenOneGamepad(i);

  bool running = true;
  SDL_Event event;
  /* Frame-pacing bookkeeping. lastTick is the wall-clock target for the
   * next frame; curTick samples real time each iteration; frameCtr
   * indexes the 17/17/16 ms triangle pattern; audiopaused mirrors
   * g_paused so we only call SDL_PauseAudioDevice on transitions. */
  uint32 lastTick = SDL_GetTicks();
  uint32 curTick = 0;
  uint32 frameCtr = 0;
  bool audiopaused = true;

  /* Autosave-resume: synthesize a "load slot 0" command so the game
   * boots straight back into the user's last session. */
  if (g_config.autosave)
    HandleCommand(kKeys_Load + 0, true);

  /* Main game loop. Each iteration: drain pending OS events, advance
   * one simulation frame (unless paused/turbo-skipped), present, sleep
   * until the next 60Hz tick. */
  while(running) {
    /* Event pump: route every SDL event to the appropriate handler. */
    while(SDL_PollEvent(&event)) {
      switch(event.type) {
      case SDL_CONTROLLERDEVICEADDED:
        /* Hot-plug: open the newly-attached gamepad so it produces events. */
        OpenOneGamepad(event.cdevice.which);
        break;
      case SDL_CONTROLLERAXISMOTION:
        HandleGamepadAxisInput(event.caxis.which, event.caxis.axis, event.caxis.value);
        break;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP: {
        /* Translate SDL's controller button enum into our internal
         * kGamepadBtn_* index; -1 means we do not bind that button. */
        int b = RemapSdlButton(event.cbutton.button);
        if (b >= 0)
          HandleGamepadInput(b, event.type == SDL_CONTROLLERBUTTONDOWN);
        break;
      }
      case SDL_MOUSEWHEEL:
        /* Ctrl+wheel acts as a window-scale shortcut, mirroring the
         * keyboard kKeys_WindowBigger/Smaller bindings. */
        if (SDL_GetModState() & KMOD_CTRL && event.wheel.y != 0)
          ChangeWindowScale(event.wheel.y > 0 ? 1 : -1);
        break;
      case SDL_MOUSEBUTTONDOWN:
        /* Shift+double-click toggles borderless mode (only when not
         * fullscreen, since fullscreen owns the window decoration). */
        if (event.button.button == SDL_BUTTON_LEFT && event.button.state == SDL_PRESSED && event.button.clicks == 2) {
          if ((g_win_flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == 0 && (g_win_flags & SDL_WINDOW_FULLSCREEN) == 0 && SDL_GetModState() & KMOD_SHIFT) {
            g_win_flags ^= SDL_WINDOW_BORDERLESS;
            SDL_SetWindowBordered(g_window, (g_win_flags & SDL_WINDOW_BORDERLESS) == 0);
          }
        }
        break;
      case SDL_KEYDOWN:
        HandleInput(event.key.keysym.sym, event.key.keysym.mod, true);
        break;
      case SDL_KEYUP:
        HandleInput(event.key.keysym.sym, event.key.keysym.mod, false);
        break;
      case SDL_QUIT:
        /* Window close button -> exit the loop and run the shutdown path. */
        running = false;
        break;
      }
    }

    /* Audio pause sync: only call SDL_PauseAudioDevice on transitions
     * so the SDL audio thread is not woken every frame just to be told
     * its state didn't change. */
    if (g_paused != audiopaused) {
      audiopaused = g_paused;
      if (device)
        SDL_PauseAudioDevice(device, audiopaused);
    }

    /* While paused, sleep ~one frame's worth and skip the rest of the
     * loop body so the simulation freezes but the event pump still runs. */
    if (g_paused) {
      SDL_Delay(16);
      continue;
    }

    // Clear gamepad inputs when joypad directional inputs to avoid wonkiness
    /* Bits 0xf0 of input1_state are the keyboard d-pad. If the keyboard
     * is supplying a direction, drop the gamepad direction so the two
     * sources don't fight (e.g. neutralizing each other on diagonals). */
    int inputs = g_input1_state;
    if (g_input1_state & 0xf0)
      g_gamepad_buttons = 0;
    inputs |= g_gamepad_buttons;

    /* Run one full simulated frame. The audio mutex is held across the
     * call because ZeldaRunFrame mutates APU state that the audio
     * thread will read in its next AudioCallback. */
    SDL_LockMutex(g_audio_mutex);
    bool is_replay = ZeldaRunFrame(inputs);
    SDL_UnlockMutex(g_audio_mutex);

    frameCtr++;

    /* Turbo / replay-turbo frame skipping. When turbo is active we only
     * present every 16th frame; during a fast-forwarded replay the rate
     * jumps to every 128th. The XOR makes turbo and is_replay mutually
     * exclusive so the two modes don't compound. */
    if ((g_turbo ^ (is_replay & g_replay_turbo)) && (frameCtr & (g_turbo ? 0xf : 0x7f)) != 0) {
      continue;
    }

    DrawPpuFrameWithPerf();

    /* Optional FPS-in-window-title display. */
    if (g_config.display_perf_title) {
      char title[60];
      snprintf(title, sizeof(title), "%s | FPS: %d", kWindowTitle, g_curr_fps);
      SDL_SetWindowTitle(g_window, title);
    }

    // if vsync isn't working, delay manually
    curTick = SDL_GetTicks();

    /* Manual frame pacing. The 17/17/16 ms triangle averages to
     * 16.666... ms per frame which lands exactly on 60 FPS over any
     * three-frame window. Both overshoot and undershoot are clamped to
     * a 500 ms window so a long stall (window drag, debugger break)
     * does not snowball into a multi-second catch-up burst. */
    if (!g_config.disable_frame_delay) {
      static const uint8 delays[3] = { 17, 17, 16 }; // 60 fps
      lastTick += delays[frameCtr % 3];

      if (lastTick > curTick) {
        uint32 delta = lastTick - curTick;
        if (delta > 500) {
          lastTick = curTick - 500;
          delta = 500;
        }
//        printf("Sleeping %d\n", delta);
        SDL_Delay(delta);
      } else if (curTick - lastTick > 500) {
        /* We are more than half a second behind schedule - give up on
         * catching up and resync to "now" so the next sleep target is
         * realistic again. */
        lastTick = curTick;
      }
    }
  }
  /* Symmetric autosave on shutdown so the next launch can resume. */
  if (g_config.autosave)
    HandleCommand(kKeys_Save + 0, true);

  // clean sdl
  /* Tear-down order matters: pause and close the audio device first so
   * AudioCallback can no longer fire while we destroy the buffers and
   * mutex it depends on, then drop the renderer/window/SDL itself. */
  if (g_config.enable_audio) {
    SDL_PauseAudioDevice(device, 1);
    SDL_CloseAudioDevice(device);
  }

  SDL_DestroyMutex(g_audio_mutex);
  free(g_audiobuffer);

  g_renderer_funcs.Destroy();

  SDL_DestroyWindow(window);
  SDL_Quit();
  //SaveConfigFile();
  return 0;
}

/* RenderDigit: blits a single decimal digit (0-9) directly into a 32bpp
 * ARGB pixel buffer using a tiny built-in 6x10 bitmap font.
 *
 * Each kFont entry is one bitmap row encoded as a byte; bits set
 * in that byte light pixels for that row from least- to most-
 * significant bit. This is just enough font to render the FPS counter
 * without pulling in a real text renderer.
 *
 * Parameters:
 *   dst   - top-left pixel of the destination glyph (already offset).
 *   pitch - row stride in bytes of the destination buffer.
 *   digit - 0..9, indexes into kFont in groups of 10 rows.
 *   color - 32-bit ARGB color to write into lit pixels.
 *   big   - when true each pixel is doubled in both axes (used at the
 *           4x render scale so the digits remain readable).
 */
static void RenderDigit(uint8 *dst, size_t pitch, int digit, uint32 color, bool big) {
  static const uint8 kFont[] = {
    0x1c, 0x36, 0x63, 0x63, 0x63, 0x63, 0x63, 0x63, 0x36, 0x1c,
    0x18, 0x1c, 0x1e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7e,
    0x3e, 0x63, 0x60, 0x30, 0x18, 0x0c, 0x06, 0x03, 0x63, 0x7f,
    0x3e, 0x63, 0x60, 0x60, 0x3c, 0x60, 0x60, 0x60, 0x63, 0x3e,
    0x30, 0x38, 0x3c, 0x36, 0x33, 0x7f, 0x30, 0x30, 0x30, 0x78,
    0x7f, 0x03, 0x03, 0x03, 0x3f, 0x60, 0x60, 0x60, 0x63, 0x3e,
    0x1c, 0x06, 0x03, 0x03, 0x3f, 0x63, 0x63, 0x63, 0x63, 0x3e,
    0x7f, 0x63, 0x60, 0x60, 0x30, 0x18, 0x0c, 0x0c, 0x0c, 0x0c,
    0x3e, 0x63, 0x63, 0x63, 0x3e, 0x63, 0x63, 0x63, 0x63, 0x3e,
    0x3e, 0x63, 0x63, 0x63, 0x7e, 0x60, 0x60, 0x60, 0x30, 0x1e,
  };
  /* Each glyph is 10 rows of font data starting at digit*10. */
  const uint8 *p = kFont + digit * 10;
  if (!big) {
    /* Walk every row, then walk the bits in that row low->high until
     * the byte is empty. The loop terminates as soon as `v` becomes 0,
     * which skips the unused high bits without an explicit width. */
    for (int y = 0; y < 10; y++, dst += pitch) {
      int v = *p++;
      for (int x = 0; v; x++, v >>= 1) {
        if (v & 1)
          ((uint32 *)dst)[x] = color;
      }
    }
  } else {
    /* Doubled pixels: each lit bit produces a 2x2 block of pixels.
     * dst advances by two rows per iteration; each x writes both
     * columns of two adjacent rows so the glyph stays solid. */
    for (int y = 0; y < 10; y++, dst += pitch * 2) {
      int v = *p++;
      for (int x = 0; v; x++, v >>= 1) {
        if (v & 1) {
          ((uint32 *)dst)[x * 2 + 1] = ((uint32 *)dst)[x * 2] = color;
          ((uint32 *)(dst+pitch))[x * 2 + 1] = ((uint32 *)(dst + pitch))[x * 2] = color;
        }
      }
    }
  }
}

/* RenderNumber: draws a decimal integer at the top-left of `dst` using
 * the bitmap font from RenderDigit. Renders the number twice in two
 * passes - first a dark gray "shadow" offset down-and-right by one
 * pixel-row+column, then white on top - giving a cheap drop-shadow
 * outline so the FPS counter stays legible against any background.
 *
 * Parameters:
 *   dst   - top-left pixel of the area to draw into.
 *   pitch - row stride in bytes.
 *   n     - integer to render (formatted with %d).
 *   big   - 2x glyph mode (used at 4x render scale).
 */
static void RenderNumber(uint8 *dst, size_t pitch, int n, bool big) {
  char buf[32], *s;
  int i;
  sprintf(buf, "%d", n);
  /* Pass 1: dark gray shadow, offset by (+1 row, +1 col) - the (<< big)
   * doubles the offset when the big-font mode is in use. */
  for (s = buf, i = 2 * 4; *s; s++, i += 8 * 4)
    RenderDigit(dst + ((pitch + i + 4) << big), pitch, *s - '0', 0x404040, big);
  /* Pass 2: white foreground at the natural digit position. */
  for (s = buf, i = 2 * 4; *s; s++, i += 8 * 4)
    RenderDigit(dst + (i << big), pitch, *s - '0', 0xffffff, big);
}

/* Forward declaration: HandleCommand wraps this in audio-mutex acquisition
 * for any command that touches engine state the audio thread also reads. */
static void HandleCommand_Locked(uint32 j, bool pressed);

/* HandleCommand: top-level dispatch for an internal command id.
 *
 * Three command classes are handled inline (without taking the audio
 * mutex) because they only touch host-side state:
 *   - kKeys_Controls_Last and below: SNES joypad buttons. These get
 *     remapped through kKbdRemap into the SNES bit positions and
 *     ORed into g_input1_state.
 *   - kKeys_Turbo: directly toggles the turbo flag.
 *
 * Anything else - save/load slots, reset, fullscreen, perf overlay,
 * volume - is forwarded into HandleCommand_Locked under the audio
 * mutex because it can mutate engine/APU state.
 *
 * Parameters:
 *   j       - command id from kKeys_* enum.
 *   pressed - true on press, false on release.
 */
static void HandleCommand(uint32 j, bool pressed) {
  if (j <= kKeys_Controls_Last) {
    /* kKbdRemap maps logical control indices into SNES joypad bit
     * positions. The two zero entries cover unused command slots so
     * those bits are never set. */
    static const uint8 kKbdRemap[] = { 0, 4, 5, 6, 7, 2, 3, 8, 0, 9, 1, 10, 11 };
    if (pressed)
      g_input1_state |= 1 << kKbdRemap[j];
    else
      g_input1_state &= ~(1 << kKbdRemap[j]);
    return;
  }

  if (j == kKeys_Turbo) {
    g_turbo = pressed;
    return;
  }

  // Everything that might access audio state
  // (like SaveLoad and Reset) must have the lock.
  SDL_LockMutex(g_audio_mutex);
  HandleCommand_Locked(j, pressed);
  SDL_UnlockMutex(g_audio_mutex);
}

/* ZeldaApuLock / ZeldaApuUnlock: thin wrappers around g_audio_mutex
 * exposed to the rest of the engine (audio.c, save/load, music
 * commands) so portable C code can serialize against the SDL audio
 * thread without including SDL headers itself. */
void ZeldaApuLock() {
  SDL_LockMutex(g_audio_mutex);
}

void ZeldaApuUnlock() {
  SDL_UnlockMutex(g_audio_mutex);
}


/* HandleCommand_Locked: dispatch for commands that touch engine/APU
 * state. Always called with g_audio_mutex held.
 *
 * Only fires on press (release events for these commands are no-ops).
 * The leading slot ranges form a "switch table" by command id range:
 *   kKeys_Load[_Last]      -> SaveLoadSlot load    (slots 0..N)
 *   kKeys_Save[_Last]      -> SaveLoadSlot save    (slots 0..N)
 *   kKeys_Replay[_Last]    -> SaveLoadSlot replay  (slots 0..N)
 *   kKeys_LoadRef[_Last]   -> reference loads      (slots 256..)
 *   kKeys_ReplayRef[_Last] -> reference replays    (slots 256..)
 *
 * The 256 offset puts the "reference" save-state slots in a separate
 * numeric range so SaveLoadSlot can tell developer-reference saves
 * (used for regression testing) apart from user-facing slots.
 *
 * Anything past those ranges falls into the switch over individual
 * one-off commands (cheats, fullscreen toggle, reset, pause, scale,
 * volume, perf overlay, etc.).
 */
static void HandleCommand_Locked(uint32 j, bool pressed) {
  if (!pressed)
    return;
  if (j <= kKeys_Load_Last) {
    SaveLoadSlot(kSaveLoad_Load, j - kKeys_Load);
  } else if (j <= kKeys_Save_Last) {
    SaveLoadSlot(kSaveLoad_Save, j - kKeys_Save);
  } else if (j <= kKeys_Replay_Last) {
    SaveLoadSlot(kSaveLoad_Replay, j - kKeys_Replay);
  } else if (j <= kKeys_LoadRef_Last) {
    SaveLoadSlot(kSaveLoad_Load, 256 + j - kKeys_LoadRef);
  } else if (j <= kKeys_ReplayRef_Last) {
    SaveLoadSlot(kSaveLoad_Replay, 256 + j - kKeys_ReplayRef);
  } else {
    switch (j) {
    /* Cheat hotkeys forward into PatchCommand (zelda_rtl.c), which
     * stuffs the corresponding RAM patch into the running game. The
     * single-character codes are the same ones the developer console
     * accepts. */
    case kKeys_CheatLife: PatchCommand('w'); break;
    case kKeys_CheatEquipment: PatchCommand('W'); break;
    case kKeys_CheatKeys: PatchCommand('o'); break;
    case kKeys_CheatWalkThroughWalls: PatchCommand('E'); break;
    case kKeys_ClearKeyLog: PatchCommand('k'); break;
    case kKeys_StopReplay: PatchCommand('l'); break;
    case kKeys_Fullscreen:
      /* Toggle borderless fullscreen and hide/show the cursor in lock-
       * step so the cursor is invisible while playing fullscreen but
       * comes back when returning to windowed mode. */
      g_win_flags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
      SDL_SetWindowFullscreen(g_window, g_win_flags & SDL_WINDOW_FULLSCREEN_DESKTOP);
      g_cursor = !g_cursor;
      SDL_ShowCursor(g_cursor);
      break;
    case kKeys_Reset:
      ZeldaReset(true);
      break;
    case kKeys_Pause: g_paused = !g_paused; break;
    case kKeys_PauseDimmed:
      g_paused = !g_paused;
      // SDL_RenderPresent may not be called more than once per frame.
      // Seems to work on Windows still. Temporary measure until it's fixed.
      /* Windows-only: when entering the dimmed-pause state, draw a
       * 60%-opacity black overlay over the last frame so the user sees
       * a clearly "paused" screen. Other platforms use plain pause to
       * avoid the double-present SDL caveat noted above. */
#ifdef _WIN32
      if (g_paused) {
        SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 159);
        SDL_RenderFillRect(g_renderer, NULL);
        SDL_RenderPresent(g_renderer);
      }
#endif
      break;
    case kKeys_ReplayTurbo: g_replay_turbo = !g_replay_turbo; break;
    case kKeys_WindowBigger: ChangeWindowScale(1); break;
    case kKeys_WindowSmaller: ChangeWindowScale(-1); break;
    case kKeys_DisplayPerf: g_display_perf ^= 1; break;
    case kKeys_ToggleRenderer: g_ppu_render_flags ^= kPpuRenderFlags_NewRenderer; break;
    case kKeys_VolumeUp:
    case kKeys_VolumeDown: HandleVolumeAdjustment(j == kKeys_VolumeUp ? 1 : -1); break;
    /* Any other command id is a programmer error - the kKeys_* enum
     * grew without a corresponding case here. */
    default: assert(0);
    }
  }
}

/* HandleInput: keyboard event entry point. Looks up the user's binding
 * for the (keyCode, modifiers) pair via config.c and forwards the
 * resolved command id (if any) into HandleCommand.
 *
 * Parameters:
 *   keyCode - SDL keysym code (SDLK_*).
 *   keyMod  - bitmask of pressed modifier keys (KMOD_*).
 *   pressed - true on key down, false on key up.
 */
static void HandleInput(int keyCode, int keyMod, bool pressed) {
  int j = FindCmdForSdlKey(keyCode, keyMod);
  if (j != 0)
    HandleCommand(j, pressed);
}

/* OpenOneGamepad: opens device index `i` as an SDL game controller if
 * SDL recognizes it. Called once per joystick at startup and again from
 * the event loop on SDL_CONTROLLERDEVICEADDED. Failure is logged but
 * not fatal - one bad pad shouldn't prevent the game from running. */
static void OpenOneGamepad(int i) {
  if (SDL_IsGameController(i)) {
    SDL_GameController *controller = SDL_GameControllerOpen(i);
    if (!controller)
      fprintf(stderr, "Could not open gamepad %d: %s\n", i, SDL_GetError());
  }
}

/* RemapSdlButton: translate SDL's SDL_GameControllerButton enum into
 * our internal kGamepadBtn_* index used by config.c's binding tables.
 * Returns -1 for any button we deliberately don't bind. */
static int RemapSdlButton(int button) {
  switch (button) {
  case SDL_CONTROLLER_BUTTON_A: return kGamepadBtn_A;
  case SDL_CONTROLLER_BUTTON_B: return kGamepadBtn_B;
  case SDL_CONTROLLER_BUTTON_X: return kGamepadBtn_X;
  case SDL_CONTROLLER_BUTTON_Y: return kGamepadBtn_Y;
  case SDL_CONTROLLER_BUTTON_BACK: return kGamepadBtn_Back;
  case SDL_CONTROLLER_BUTTON_GUIDE: return kGamepadBtn_Guide;
  case SDL_CONTROLLER_BUTTON_START: return kGamepadBtn_Start;
  case SDL_CONTROLLER_BUTTON_LEFTSTICK: return kGamepadBtn_L3;
  case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return kGamepadBtn_R3;
  case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return kGamepadBtn_L1;
  case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return kGamepadBtn_R1;
  case SDL_CONTROLLER_BUTTON_DPAD_UP: return kGamepadBtn_DpadUp;
  case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return kGamepadBtn_DpadDown;
  case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return kGamepadBtn_DpadLeft;
  case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return kGamepadBtn_DpadRight;
  default: return -1;
  }
}

/* HandleGamepadInput: a gamepad button press or release.
 *
 * Maintains g_gamepad_modifiers as a sticky bitmap of currently-held
 * buttons so chord bindings (e.g. "Select+A == quick save") can be
 * resolved at press time, and remembers the resolved command in
 * g_gamepad_last_cmd[button] so the matching release dispatches the
 * same command even after the chord state has changed.
 *
 * The first line is a deduplication guard: if the new state already
 * matches the stored modifier bit (e.g. SDL fired a duplicate event),
 * do nothing.
 *
 * Parameters:
 *   button  - kGamepadBtn_* index returned by RemapSdlButton.
 *   pressed - true on press, false on release.
 */
static void HandleGamepadInput(int button, bool pressed) {
  if (!!(g_gamepad_modifiers & (1 << button)) == pressed)
    return;
  g_gamepad_modifiers ^= 1 << button;
  if (pressed)
    g_gamepad_last_cmd[button] = FindCmdForGamepadButton(button, g_gamepad_modifiers);
  if (g_gamepad_last_cmd[button] != 0)
    HandleCommand(g_gamepad_last_cmd[button], pressed);
}

/* HandleVolumeAdjustment: bumps the audio output volume up or down by
 * one step. On platforms with a real per-application volume mixer
 * (currently Windows via volume_control.c) we move the system volume
 * in 5% steps from 0..100. Everywhere else we attenuate inside
 * AudioCallback via g_sdl_audio_mixer_volume in 1/16th-of-max steps.
 *
 * Parameters:
 *   volume_adjustment - +1 to raise, -1 to lower.
 */
static void HandleVolumeAdjustment(int volume_adjustment) {
#if SYSTEM_VOLUME_MIXER_AVAILABLE
  int current_volume = GetApplicationVolume();
  int new_volume = IntMin(IntMax(0, current_volume + volume_adjustment * 5), 100);
  SetApplicationVolume(new_volume);
  printf("[System Volume]=%i\n", new_volume);
#else
  /* SDL_MIX_MAXVOLUME >> 4 yields 16 steps from 0..max so a single
   * key press is a perceptible but not jarring volume change. */
  g_sdl_audio_mixer_volume = IntMin(IntMax(0, g_sdl_audio_mixer_volume + volume_adjustment * (SDL_MIX_MAXVOLUME >> 4)), SDL_MIX_MAXVOLUME);
  printf("[SDL mixer volume]=%i\n", g_sdl_audio_mixer_volume);
#endif
}

/* ApproximateAtan2: a branch-light atan2 approximation used by the
 * analog-stick-to-d-pad mapper. Returns the angle of (y, x) normalized
 * into [0, 4) - i.e. one full revolution maps to 4.0 instead of 2*pi -
 * which lets HandleGamepadAxisInput convert directly into the eight
 * 45-degree d-pad segments by scaling and indexing.
 *
 * The approximation is the rational form
 *     normalized_atan(x) ~ (b x + x^2) / (1 + 2 b x + x^2)
 * extended into the four quadrants by sign-bit fixups on x and y.
 * Maximum error is about 0.16 degrees, which is far better than the
 * 45-degree resolution we ultimately need.
 *
 * The sign bits of x and y are extracted as raw IEEE bits to avoid a
 * call to copysignf, and reapplied via XOR onto the first-quadrant
 * result. The +0.000001f in the denominator prevents a divide-by-zero
 * at the (0,0) origin.
 *
 * Parameters:
 *   y, x - cartesian coordinates (matches atan2(y, x) ordering).
 */
static float ApproximateAtan2(float y, float x) {
  uint32 sign_mask = 0x80000000;
  float b = 0.596227f;
  // Extract the sign bits
  uint32 ux_s = sign_mask & *(uint32 *)&x;
  uint32 uy_s = sign_mask & *(uint32 *)&y;
  // Determine the quadrant offset
  /* Compose the integer quadrant index 0..3 from the two sign bits and
   * convert to a float so it can be added to the in-quadrant angle. */
  float q = (float)((~ux_s & uy_s) >> 29 | ux_s >> 30);
  // Calculate the arctangent in the first quadrant
  float bxy_a = b * x * y;
  if (bxy_a < 0.0f) bxy_a = -bxy_a;  // avoid fabs
  float num = bxy_a + y * y;
  float atan_1q = num / (x * x + bxy_a + num + 0.000001f);
  // Translate it to the proper quadrant
  /* Reapply the sign bits via XOR so the first-quadrant approximation
   * is reflected/rotated into the correct quadrant. */
  uint32_t uatan_2q = (ux_s ^ uy_s) | *(uint32 *)&atan_1q;
  return q + *(float *)&uatan_2q;
}

/* HandleGamepadAxisInput: process one analog-axis sample.
 *
 * Two responsibilities:
 *   1. Map the left analog stick into eight 45-degree d-pad segments
 *      (centered on the cardinal directions, rotated by 22.5 degrees so
 *      pure cardinals stay clean and diagonals get equal-sized fans).
 *      A circular dead zone of magnitude 10000 prevents accidental
 *      direction triggers when the stick is near rest.
 *   2. Map the analog L2/R2 triggers into discrete L2/R2 button events
 *      with hysteresis (a value < 12000 is "released", >= 16000 is
 *      "pressed", and anything in between leaves the previous state
 *      intact to avoid chatter).
 *
 * The "active gamepad" logic remembers which controller last produced
 * a meaningful stick input. Other controllers are ignored until they
 * deflect their stick past +/-16000 - this stops a second player's
 * idle stick from spamming events at the active player.
 *
 * Parameters:
 *   gamepad_id - SDL controller instance id (joystick id).
 *   axis       - SDL_CONTROLLER_AXIS_*.
 *   value      - axis value in SDL's signed 16-bit range.
 */
static void HandleGamepadAxisInput(int gamepad_id, int axis, int value) {
  static int last_gamepad_id, last_x, last_y;
  if (axis == SDL_CONTROLLER_AXIS_LEFTX || axis == SDL_CONTROLLER_AXIS_LEFTY) {
    // ignore other gamepads unless they have a big input
    if (last_gamepad_id != gamepad_id) {
      if (value > -16000 && value < 16000)
        return;
      last_gamepad_id = gamepad_id;
      last_x = last_y = 0;
    }
    /* Update the cached x or y component without a redundant branch:
     * pick the field's address with the ternary, then assign through it. */
    *(axis == SDL_CONTROLLER_AXIS_LEFTX ? &last_x : &last_y) = value;
    int buttons = 0;
    /* Circular dead zone test: x^2 + y^2 vs radius^2 (10000^2). Doing
     * the comparison in squared units avoids a sqrt per axis event. */
    if (last_x * last_x + last_y * last_y >= 10000 * 10000) {
      // in the non deadzone part, divide the circle into eight 45 degree
      // segments rotated by 22.5 degrees that control which direction to move.
      // todo: do this without floats?
      static const uint8 kSegmentToButtons[8] = {
        1 << 4,           // 0 = up
        1 << 4 | 1 << 7,  // 1 = up, right
        1 << 7,           // 2 = right
        1 << 7 | 1 << 5,  // 3 = right, down
        1 << 5,           // 4 = down
        1 << 5 | 1 << 6,  // 5 = down, left
        1 << 6,           // 6 = left
        1 << 6 | 1 << 4,  // 7 = left, up
      };
      /* Convert the [0,4) atan2 result into a 0..255 byte angle so the
       * segment index is just (angle + half-segment) >> 5. The +16
       * rotates the segments by 22.5 degrees (one half-segment) so
       * pure cardinal directions land in the middle of their segment;
       * the +64 keeps the pre-shift value positive across the wrap. */
      uint8 angle = (uint8)(int)(ApproximateAtan2(last_y, last_x) * 64.0f + 0.5f);
      buttons = kSegmentToButtons[(uint8)(angle + 16 + 64) >> 5];
    }
    g_gamepad_buttons = buttons;
  } else if ((axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
    if (value < 12000 || value >= 16000)  // hysteresis
      HandleGamepadInput(axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? kGamepadBtn_L2 : kGamepadBtn_R2, value >= 12000);
  }
}

/* LoadRom: read the original SNES ROM at `filename` and hand it to the
 * embedded 65C816 emulator (zelda_cpu_infra.c) for the reference
 * execution path. Used only when builds include the emulator fallback;
 * the asset-only path skips this entirely.
 *
 * Returns true if EmuInitialize accepted the ROM. Aborts via Die() if
 * the file could not be read at all.
 */
static bool LoadRom(const char *filename) {
  size_t length = 0;
  uint8 *file = ReadWholeFile(filename, &length);
  if(!file) Die("Failed to read file");
  bool result = EmuInitialize(file, length);
  free(file);
  return result;
}

/* ParseLinkGraphics: validate and unpack a ZSPR (Zelda Sprite) v1 file
 * into the engine's Link graphics + palette tables.
 *
 * The ZSPR format starts with the four-byte signature "ZSPR" followed
 * by header fields at fixed offsets. We sanity-check the signature,
 * pull the pixel and palette region offsets/lengths out of the header,
 * verify they fit inside the file and that the pixel block is the
 * expected 0x7000 bytes (4bpp tile data), then memcpy directly into
 * kLinkGraphics, kPalette_ArmorAndGloves, and kGlovesColor.
 *
 * Returns true on a recognized, well-formed ZSPR; false otherwise so
 * the caller can fall back to the built-in Link graphics.
 */
static bool ParseLinkGraphics(uint8 *file, size_t length) {
  /* Need at least the fixed header bytes plus the ZSPR signature. */
  if (length < 27 || memcmp(file, "ZSPR", 4) != 0)
    return false;
  /* Header field offsets are fixed by the ZSPR v1 spec. */
  uint32 pixel_offs = DWORD(file[9]);
  uint32 pixel_length = WORD(file[13]);
  uint32 palette_offs = DWORD(file[15]);
  uint32 palette_length = WORD(file[19]);
  /* Cast to uint64 before adding lengths so a malicious header that
   * overflows uint32 cannot pass the bounds check. */
  if ((uint64)pixel_offs + pixel_length > length ||
      (uint64)palette_offs + palette_length > length ||
      pixel_length != 0x7000)
    return false;
  /* Defensive: catch a build-time mismatch between the asset table
   * sizes and the values this loader was written against. */
  if (kPalette_ArmorAndGloves_SIZE != 150 || kLinkGraphics_SIZE != 0x7000)
    Die("ParseLinkGraphics: Invalid asset sizes");
  memcpy(kLinkGraphics, file + pixel_offs, 0x7000);
  /* Older ZSPR files may omit the gloves color trailer. The two
   * length-guarded copies make both shapes work. */
  if (palette_length >= 120)
    memcpy(kPalette_ArmorAndGloves, file + palette_offs, 120);
  if (palette_length >= 124)
    memcpy(kGlovesColor, file + palette_offs + 120, 4);
  return true;
}

/* LoadLinkGraphics: optional sprite-swap step run during startup. If
 * the user pointed g_config.link_graphics at a ZSPR file, read it,
 * unpack it via ParseLinkGraphics, and overwrite the built-in Link
 * graphics in place. Failure to load is fatal because the user
 * explicitly asked for the override. */
static void LoadLinkGraphics() {
  if (g_config.link_graphics) {
    fprintf(stderr, "Loading Link Graphics: %s\n", g_config.link_graphics);
    size_t length = 0;
    uint8 *file = ReadWholeFile(g_config.link_graphics, &length);
    if (file == NULL || !ParseLinkGraphics(file, length))
      Die("Unable to load file");
    free(file);
  }
}


/* Asset table: parallel arrays of base pointer + length for every
 * compiled-in asset id. Populated once by LoadAssets() from the data
 * inside zelda3_assets.dat and consulted everywhere via
 * FindInAssetArray and the kAsset* helpers. */
const uint8 *g_asset_ptrs[kNumberOfAssets];
uint32 g_asset_sizes[kNumberOfAssets];

/* LoadAssets: bring up the global asset table from disk.
 *
 * Two file forms are supported:
 *   - zelda3_assets.dat - the compiled, ready-to-use asset blob.
 *     Loaded directly when present.
 *   - zelda3_assets.bps + zelda3.sfc - a BPS patch over the original
 *     ROM. ApplyBps() runs the patch in memory; the resulting buffer
 *     is treated as if it had been read from zelda3_assets.dat.
 *
 * After the blob is in memory, validate the header (signature plus
 * the kNumberOfAssets count guard), then walk the per-asset 4-byte
 * size table to fill in g_asset_ptrs / g_asset_sizes. The 4-byte
 * alignment step (offset = (offset + 3) & ~3) keeps each asset on a
 * 4-byte boundary so callers can read multi-byte fields directly.
 *
 * Finally, the optional kFeatures0_DimFlashes patch tweaks three
 * dungeon-floor palette bytes in place to soften flashing effects
 * for users who need it.
 */
static void LoadAssets() {
  size_t length = 0;
  uint8 *data = ReadWholeFile("zelda3_assets.dat", &length);
  if (!data) {
    /* Fall-back path: synthesize the asset blob from a BPS patch
     * applied to the user's original SNES ROM. Both files must be
     * present and the patch must apply cleanly. */
    size_t bps_length, bps_src_length;
    uint8 *bps, *bps_src;
    bps = ReadWholeFile("zelda3_assets.bps", &bps_length);
    if (!bps)
      Die("Failed to read zelda3_assets.dat. Please see the README for information about how you get this file.");
    bps_src = ReadWholeFile("zelda3.sfc", &bps_src_length);
    if (!bps_src)
      Die("Missing file: zelda3.sfc");
    data = ApplyBps(bps_src, bps_src_length, bps, bps_length, &length);
    if (!data)
      Die("Unable to apply zelda3_assets.bps. Please make sure you got the right version of 'zelda3.sfc'");
  }

  static const char kAssetsSig[] = { kAssets_Sig };

  /* Header validation: file must be large enough to contain the
   * signature, the trailing metadata, and the per-asset size table;
   * the signature must match; and the embedded kNumberOfAssets must
   * agree with the build so we never read a stale or mismatched blob. */
  if (length < 16 + 32 + 32 + 8 + kNumberOfAssets * 4 ||
      memcmp(data, kAssetsSig, 48) != 0 ||
      *(uint32*)(data + 80) != kNumberOfAssets)
    Die("Invalid assets file");

  /* Asset payloads start after the size table and an extra
   * format-version offset stored at byte 84. */
  uint32 offset = 88 + kNumberOfAssets * 4 + *(uint32 *)(data + 84);

  /* Walk the size table; each asset is recorded as its byte length and
   * its payload immediately follows the previous one (with 4-byte
   * alignment between entries). */
  for (size_t i = 0; i < kNumberOfAssets; i++) {
    uint32 size = *(uint32 *)(data + 88 + i * 4);
    offset = (offset + 3) & ~3;
    if ((uint64)offset + size > length)
      Die("Assets file corruption");
    g_asset_sizes[i] = size;
    g_asset_ptrs[i] = data + offset;
    offset += size;
  }

  if (g_config.features0 & kFeatures0_DimFlashes) { // patch dungeon floor palettes
    /* Hand-tuned darker replacements for three palette entries that
     * cause the most aggressive flashing in dungeon backgrounds. */
    kPalette_DungBgMain[0x484] = 0x70;
    kPalette_DungBgMain[0x485] = 0x95;
    kPalette_DungBgMain[0x486] = 0x57;
  }
}

// Go some steps up and find zelda3.ini
/* SwitchDirectory: search the current working directory and up to two
 * parent directories for zelda3.ini, and chdir into the one that
 * contains it. This lets the binary be launched from a deep build
 * subdirectory while still finding the assets in the project root.
 *
 * The walk is bounded to 3 steps so a misconfigured launcher cannot
 * accidentally walk up to the filesystem root.
 */
static void SwitchDirectory() {
  char buf[4096];
  /* Reserve 32 bytes at the end for the "/zelda3.ini" suffix we will
   * append below. */
  if (!getcwd(buf, sizeof(buf) - 32))
    return;
  size_t pos = strlen(buf);

  for (int step = 0; pos != 0 && step < 3; step++) {
    memcpy(buf + pos, "/zelda3.ini", 12);
    FILE *f = fopen(buf, "rb");
    if (f) {
      fclose(f);
      buf[pos] = 0;
      /* Only chdir if we actually moved up at least one level. step==0
       * means the file is already in the current directory, no chdir
       * needed. */
      if (step != 0) {
        printf("Found zelda3.ini in %s\n", buf);
        int err = chdir(buf);
        (void)err;
      }
      return;
    }
    /* Walk one directory up by trimming the last path component. The
     * inner while peels characters off until it hits a path separator. */
    pos--;
    while (pos != 0 && buf[pos] != '/' && buf[pos] != '\\')
      pos--;
  }
}

/* FindInAssetArray: thin wrapper that bundles an asset's pointer/size
 * pair into a MemBlk and asks FindIndexInMemblk for the `idx`-th sub-
 * record inside it. Used by the rest of the engine to grab a specific
 * entry from a packed asset (palette, tile group, etc.) by index. */
MemBlk FindInAssetArray(int asset, int idx) {
  return FindIndexInMemblk((MemBlk) { g_asset_ptrs[asset], g_asset_sizes[asset] }, idx);
}
