/*
 * glsl_shader.h — Multi-Pass GLSL Shader Pipeline
 *
 * Implements a configurable multi-pass GLSL shader system for post-processing
 * the emulated SNES video output before it reaches the display. This allows
 * players to apply CRT simulation, scanline effects, pixel-art upscaling
 * (e.g., xBRZ, HQ4x), color correction, and other visual filters that
 * approximate the look of original SNES hardware on modern displays.
 *
 * The system is inspired by the RetroArch/libretro .glslp shader preset
 * format, supporting:
 *   - Up to 20 chained shader passes, each with independent scale and FBO
 *   - External lookup textures (LUTs) for color grading and dithering
 *   - Tunable float parameters exposed as uniforms to each shader pass
 *   - Per-pass framebuffer configuration (float, sRGB, mipmap)
 *   - Feedback from previous frames for motion-adaptive effects
 *   - Both desktop OpenGL and OpenGL ES contexts
 *
 * Shader presets are loaded from .glslp text files that specify the pass
 * chain, scaling rules, and parameter defaults.
 *
 * Related files:
 *   glsl_shader.c  — Implementation of shader compilation, FBO management,
 *                     uniform binding, and the multi-pass render loop
 *   opengl.c       — OpenGL context setup and the main render call that
 *                     invokes GlslShader_Render each frame
 *   config.c       — User configuration that specifies which shader preset
 *                     to load at startup
 */
#ifndef ZELDA3_GLSL_SHADER_H_
#define ZELDA3_GLSL_SHADER_H_

#include "types.h"

// --- Pipeline Capacity Limits ------------------------------------------------

enum {
  kGlslMaxPasses = 20,    // Maximum number of shader passes in a preset chain
  kGlslMaxTextures = 10,  // Maximum number of external LUT textures per preset
};

// --- Scale Type Enumeration --------------------------------------------------

/*
 * GLSLScaleType — Determines how a shader pass's output framebuffer is sized.
 * Each pass can independently choose its scaling reference, allowing the
 * pipeline to progressively upscale from native SNES resolution (256x224)
 * through intermediate sizes to the final viewport.
 */
enum GLSLScaleType {
  GLSL_NONE,      // No explicit scaling — use the default (same as previous pass)
  GLSL_SOURCE,    // Scale relative to the input texture size of this pass
  GLSL_VIEWPORT,  // Scale relative to the final display viewport size
  GLSL_ABSOLUTE   // Scale to an absolute pixel size (width/height in pixels)
};

// --- Uniform Location Bundles ------------------------------------------------

/*
 * GlslTextureUniform — OpenGL uniform locations for a single texture input.
 *
 * Each texture accessible to a shader (the main input, the original source,
 * previous frames, previous passes) needs four uniforms: the sampler itself,
 * the input size (pre-filter dimensions), the texture size (actual GPU
 * texture dimensions, which may differ due to power-of-two padding), and
 * the texture coordinate attribute location.
 */
typedef struct GlslTextureUniform {
  int Texture;     // Uniform location for the sampler2D
  int InputSize;   // Uniform location for the vec2 input dimensions
  int TextureSize; // Uniform location for the vec2 texture dimensions
  int TexCoord;    // Attribute location for the texture coordinates
} GlslTextureUniform;

/*
 * GlslUniforms — Complete set of uniform locations for one shader pass.
 *
 * Aggregates all the uniform locations that a single shader pass might
 * reference. The naming convention follows the RetroArch GLSL shader spec:
 *   - Top:      The direct input texture (output of the previous pass)
 *   - Orig:     The original unprocessed source texture (pass 0 input)
 *   - Prev[]:   Previous frame textures for temporal effects (up to 7 frames)
 *   - Pass[]:   Output textures from earlier passes in the current frame
 *   - PassPrev[]: Output textures from earlier passes in the previous frame
 *   - Texture[]:  External LUT textures (loaded from image files)
 */
typedef struct GlslUniforms {
  GlslTextureUniform Top;   // Current pass input (previous pass output)
  int OutputSize;            // Uniform for this pass's output dimensions
  int FrameCount, FrameDirection; // Frame counter and playback direction (+1/-1)
  int LUTTexCoord;           // Texture coordinate attribute for LUT sampling
  int VertexCoord;           // Vertex position attribute location
  GlslTextureUniform Orig;   // Original unprocessed SNES framebuffer
  GlslTextureUniform Prev[7]; // Previous frame history (Prev, Prev1..Prev6)
  GlslTextureUniform Pass[kGlslMaxPasses];     // Current frame's earlier passes
  GlslTextureUniform PassPrev[kGlslMaxPasses]; // Previous frame's earlier passes
  int Texture[kGlslMaxTextures]; // External LUT texture sampler locations
} GlslUniforms;

// --- Shader Pass Configuration -----------------------------------------------

/*
 * GlslPass — Configuration and GPU state for one shader pass in the chain.
 *
 * Each pass compiles a GLSL shader, renders into its own framebuffer object
 * (FBO) at a configured resolution, and feeds its output to the next pass.
 * The final pass renders directly to the screen.
 */
typedef struct GlslPass {
  char *filename;              // Path to the .glsl shader source file
  uint8 scale_type_x, scale_type_y; // GLSLScaleType for horizontal/vertical
  bool float_framebuffer;      // Use GL_RGBA32F instead of GL_RGBA8 for HDR
  bool srgb_framebuffer;       // Enable sRGB encoding on the FBO output
  bool mipmap_input;           // Generate mipmaps on the input texture
  float scale_x, scale_y;     // Scale multipliers (meaning depends on scale_type)
  uint wrap_mode;              // GL texture wrap mode (clamp, repeat, mirror)
  uint frame_count_mod;        // FrameCount modulo value (0 = no modulo)
  uint frame_count;            // Current frame count (may be modulo-reduced)
  uint gl_program, gl_fbo;     // Compiled shader program and framebuffer object
  uint filter;                 // GL texture filter (GL_NEAREST or GL_LINEAR)
  uint gl_texture;             // Output texture attached to this pass's FBO
  uint16 width, height;        // Output framebuffer dimensions in pixels
  GlslUniforms unif;           // Resolved uniform locations for this pass
} GlslPass;

// --- Texture With Size -------------------------------------------------------

/*
 * GlTextureWithSize — Lightweight handle pairing an OpenGL texture with its
 * dimensions. Used for the source texture input and the previous-frame
 * history ring buffer, where only the texture ID and size are needed
 * (no FBO or shader state).
 */
typedef struct GlTextureWithSize {
  uint gl_texture;    // OpenGL texture name (ID)
  uint16 width, height; // Texture dimensions in pixels
} GlTextureWithSize;

// --- External Lookup Textures ------------------------------------------------

/*
 * GlslTexture — An external image loaded as a GPU texture for use as a
 * lookup table (LUT) in shader passes. Common uses include color grading
 * tables, dithering patterns, and CRT phosphor masks. Stored as a linked
 * list so presets can define a variable number of LUTs.
 */
typedef struct GlslTexture {
  struct GlslTexture *next;  // Next texture in the linked list (NULL = end)
  char *id;                  // String identifier referenced in shader source
  char *filename;            // Path to the image file (PNG, BMP, etc.)
  uint filter;               // GL texture filter mode
  uint gl_texture;           // OpenGL texture name after upload
  uint wrap_mode;            // GL wrap mode (clamp, repeat, mirror)
  bool mipmap;               // Whether to generate mipmaps for this texture
  int width;                 // Image width in pixels
  int height;                // Image height in pixels
} GlslTexture;

// --- Tunable Shader Parameters -----------------------------------------------

/*
 * GlslParam — A user-tunable floating-point parameter exposed as a uniform
 * to shader passes. Parameters allow runtime adjustment of effect intensity
 * (e.g., scanline strength, CRT curvature, color temperature) without
 * recompiling shaders. Stored as a linked list; each parameter tracks its
 * uniform location in every pass so it can be bound efficiently.
 */
typedef struct GlslParam {
  struct GlslParam *next;    // Next parameter in the linked list (NULL = end)
  char *id;                  // Parameter name matching the uniform declaration
  bool has_value;            // Whether a non-default value has been set
  float value;               // Current parameter value
  float min;                 // Minimum allowed value (for UI sliders/validation)
  float max;                 // Maximum allowed value
  uint uniform[kGlslMaxPasses]; // Uniform location in each pass's shader program
} GlslParam;

// --- Top-Level Shader Pipeline -----------------------------------------------

/*
 * GlslShader — Root object representing an entire multi-pass shader pipeline.
 *
 * Owns the array of passes, the linked lists of textures and parameters,
 * the shared vertex buffer, and the previous-frame ring buffer. Created
 * from a .glslp preset file and destroyed when the shader is unloaded or
 * the application exits.
 */
typedef struct GlslShader {
  int n_pass;                 // Number of active shader passes in the chain
  GlslPass *pass;             // Array of n_pass+1 entries (index 0 = source pass)
  GlslParam *first_param;    // Head of the tunable parameters linked list
  GlslTexture *first_texture; // Head of the LUT textures linked list
  uint *gl_vao;               // Vertex array object(s) for pass rendering
  uint gl_vbo;                // Shared vertex buffer (fullscreen quad)
  uint frame_count;           // Global frame counter incremented each render
  int max_prev_frame;         // How many previous frames to retain (0-7)
  GlTextureWithSize prev_frame[8]; // Ring buffer of previous frame textures
} GlslShader;

// --- Public API --------------------------------------------------------------

/*
 * GlslShader_CreateFromFile — Parse a .glslp shader preset file, compile all
 * GLSL shader passes, create FBOs, load LUT textures, and resolve all uniform
 * locations. Returns a fully initialized pipeline ready for rendering.
 *
 * @param filename   Path to the .glslp preset file
 * @param opengl_es  true to compile for OpenGL ES (mobile/embedded), false
 *                   for desktop OpenGL
 * @return           Allocated GlslShader on success, NULL on failure
 */
GlslShader *GlslShader_CreateFromFile(const char *filename, bool opengl_es);

/*
 * GlslShader_Destroy — Release all GPU resources (programs, FBOs, textures,
 * VAOs, VBOs) and free all memory owned by the shader pipeline.
 *
 * @param gs  Shader pipeline to destroy (safe to pass NULL)
 */
void GlslShader_Destroy(GlslShader *gs);

/*
 * GlslShader_Render — Execute the full multi-pass shader pipeline for one
 * frame. Binds the source texture, runs each pass in sequence (rendering
 * into intermediate FBOs), and outputs the final pass to the specified
 * viewport rectangle on the default framebuffer.
 *
 * @param gs               Shader pipeline to execute
 * @param tex              Source texture (the raw SNES framebuffer)
 * @param viewport_x       Viewport left edge in pixels
 * @param viewport_y       Viewport bottom edge in pixels
 * @param viewport_width   Viewport width in pixels
 * @param viewport_height  Viewport height in pixels
 */
void GlslShader_Render(GlslShader *gs, GlTextureWithSize *tex, int viewport_x, int viewport_y, int viewport_width, int viewport_height);


#endif  // ZELDA3_GLSL_SHADER_H_
