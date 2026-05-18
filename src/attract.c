/*
 * attract.c — Demo/Attract Mode and Title Screen Sequences
 *
 * Implements the idle attract mode that plays when the player sits at the
 * title screen without pressing Start. This includes several dramatized
 * story vignettes that preview the game's narrative:
 *
 *   - Polka dots scrolling background with legend text overlays
 *   - Mode 7 world map zoom-out effect
 *   - Throne room scene (king and advisors)
 *   - Zelda's prison cell (Zelda behind bars with patrolling soldiers)
 *   - Maiden warp / Agahnim altar scene (crystal sealing ceremony)
 *
 * Each scene is loaded as a self-contained dungeon room or overworld area,
 * with sprite simulation for soldiers, Zelda, and the maiden. The attract
 * module also handles fade-in/fade-out transitions between scenes and
 * provides a skip-to-file-select mechanism when the player presses Start.
 *
 * The main entry point is Module14_Attract(), dispatched from the game's
 * top-level module router in misc.c.
 *
 * Original SNES ROM addresses are noted in function signatures where known.
 */

/* Runtime bridge providing SNES hardware emulation wrappers */
#include "zelda_rtl.h"
/* SNES WRAM variable mappings (global game state) */
#include "variables.h"
/* SNES PPU/APU register address constants */
#include "snes/snes_regs.h"
/* Tileset, palette, and CHR graphics loading routines */
#include "load_gfx.h"
/* Dungeon room loading and rendering infrastructure */
#include "dungeon.h"
/* Sprite framework (coordinate helpers, OAM allocation) */
#include "sprite.h"
/* Shared ending/intro sequence declarations */
#include "ending.h"
/* Text rendering and dialogue message system */
#include "messaging.h"
/* Public declarations for this module's exported functions */
#include "attract.h"
/* Sprite-specific animation routines (soldier guards) */
#include "sprite_main.h"

/*
 * Mode 7 zoom scaling tables for the world map attract sequence.
 * kMapMode_Zooms1 controls the horizontal scaling factor per scanline,
 * producing the perspective zoom-out effect. Values decrease from 375
 * toward 258, creating a smooth deceleration curve as the camera pulls
 * back from the map. Each entry corresponds to one scanline of the
 * 240-line display. Multiplied by the timer_for_mode7_zoom countdown
 * in Attract_ControlMapZoom() to animate the zoom over time.
 */
const uint16 kMapMode_Zooms1[240] = {
  375, 374, 373, 373, 372, 371, 371, 370, 369, 369, 368, 367, 367, 366, 365, 365,
  364, 363, 363, 361, 361, 360, 359, 359, 358, 357, 357, 356, 355, 355, 354, 354,
  353, 352, 352, 351, 351, 350, 349, 349, 348, 348, 347, 346, 346, 345, 345, 344,
  343, 343, 342, 342, 341, 341, 340, 339, 339, 338, 338, 337, 337, 336, 335, 335,
  334, 334, 333, 333, 332, 332, 331, 331, 330, 330, 328, 327, 327, 326, 326, 325,
  325, 324, 324, 323, 323, 322, 322, 321, 321, 320, 320, 319, 319, 318, 318, 317,
  317, 316, 316, 315, 315, 314, 314, 313, 313, 312, 312, 311, 311, 310, 310, 309,
  309, 309, 308, 308, 307, 307, 306, 306, 305, 305, 304, 304, 303, 303, 303, 302,
  302, 301, 301, 300, 300, 299, 299, 299, 298, 298, 297, 297, 295, 295, 294, 294,
  294, 293, 293, 292, 292, 292, 291, 291, 290, 290, 289, 289, 289, 288, 288, 287,
  287, 287, 286, 286, 285, 285, 285, 284, 284, 283, 283, 283, 282, 282, 281, 281,
  281, 280, 280, 279, 279, 279, 278, 278, 278, 277, 277, 276, 276, 276, 275, 275,
  275, 274, 274, 273, 273, 273, 272, 272, 272, 271, 271, 271, 270, 270, 269, 269,
  269, 268, 268, 268, 267, 267, 267, 266, 266, 266, 265, 265, 265, 264, 264, 264,
  263, 263, 262, 262, 262, 261, 261, 261, 260, 260, 260, 259, 259, 259, 258, 258,
};
/*
 * kMapMode_Zooms2 is the companion vertical scaling table for Mode 7.
 * These values are smaller than kMapMode_Zooms1, producing the
 * characteristic SNES Mode 7 perspective distortion where distant
 * scanlines are more compressed than near ones. Range: 136 down to 94.
 */
const uint16 kMapMode_Zooms2[240] = {
  136, 136, 135, 135, 135, 135, 135, 134, 134, 134, 133, 133, 133, 133, 132, 132,
  132, 132, 132, 131, 131, 131, 130, 130, 130, 130, 130, 129, 129, 129, 129, 129,
  128, 128, 128, 127, 127, 127, 127, 127, 126, 126, 126, 126, 126, 125, 125, 125,
  124, 124, 124, 124, 124, 124, 123, 123, 123, 123, 123, 122, 122, 122, 121, 121,
  121, 121, 121, 121, 120, 120, 120, 120, 120, 120, 119, 119, 119, 118, 118, 118,
  118, 118, 118, 117, 117, 117, 117, 117, 117, 116, 116, 116, 116, 115, 115, 115,
  115, 115, 115, 114, 114, 114, 114, 114, 114, 113, 113, 113, 113, 112, 112, 112,
  112, 112, 112, 112, 111, 111, 111, 111, 111, 111, 110, 110, 110, 110, 110, 109,
  109, 109, 109, 109, 109, 108, 108, 108, 108, 108, 108, 108, 107, 107, 107, 107,
  107, 106, 106, 106, 106, 106, 106, 106, 105, 105, 105, 105, 105, 105, 105, 104,
  104, 104, 104, 104, 103, 103, 103, 103, 103, 103, 103, 103, 102, 102, 102, 102,
  102, 102, 102, 101, 101, 101, 101, 101, 101, 100, 100, 100, 100, 100, 100, 100,
  100,  99,  99,  99,  99,  99,  99,  99,  99,  98,  98,  98,  98,  98,  97,  97,
  97,  97,  97,  97,  97,  97,  97,  96,  96,  96,  96,  96,  96,  96,  96,  96,
  95, 95, 95, 95, 95, 95, 95, 94, 94, 94, 94, 94, 94, 94, 94, 94,
};
/*
 * Legend graphics tile map data for the four background image panels
 * shown during the polka-dot scrolling attract sequence. Each array
 * contains RLE-compressed BG3 tile map entries that spell out portions
 * of the game's backstory legend. The 0x61/0x62 prefix bytes encode
 * VRAM destination addresses, and 0xff terminates the stream.
 * These are transferred to VRAM via Attract_BuildNextImageTileMap().
 */
static const uint8 kAttract_Legendgraphics_0[157+1] = {
  0x61, 0x65, 0x40, 0x28,    0, 0x35, 0x61, 0x85, 0x40, 0x28, 0x10, 0x35, 0x61, 0xa5,    0, 0x29,
     1, 0x35,    2, 0x35,    1, 0x35,    2, 0x35,    1, 0x35,    2, 0x35,    1, 0x35,    2, 0x35,
     1, 0x35,    3, 0x31,    3, 0x71,    2, 0x35,    1, 0x35,    2, 0x35,    1, 0x35,    2, 0x35,
     1, 0x35,    2, 0x35,    1, 0x35,    2, 0x35,    1, 0x35, 0x61, 0xc5,    0, 0x29, 0x11, 0x35,
  0x12, 0x35, 0x11, 0x35, 0x12, 0x35, 0x11, 0x35, 0x12, 0x35, 0x11, 0x35, 0x12, 0x35, 0x11, 0x35,
  0x13, 0x35, 0x13, 0x75, 0x12, 0x35, 0x11, 0x35, 0x12, 0x35, 0x11, 0x35, 0x12, 0x35, 0x11, 0x35,
  0x12, 0x35, 0x11, 0x35, 0x12, 0x35, 0x11, 0x35, 0x61, 0xe5,    0, 0x29, 0x20, 0x35, 0x21, 0x35,
  0x20, 0x35, 0x21, 0x35, 0x20, 0x35, 0x21, 0x35, 0x20, 0x35, 0x21, 0x35, 0x20, 0x35, 0x21, 0x35,
  0x20, 0x35, 0x21, 0x35, 0x20, 0x35, 0x21, 0x35, 0x20, 0x35, 0x21, 0x35, 0x20, 0x35, 0x21, 0x35,
  0x20, 0x35, 0x21, 0x35, 0x20, 0x35, 0x62,    5, 0x40, 0x28,    0, 0xb5, 0xff, 0x61,
};
static const uint8 kAttract_Legendgraphics_1[237+1] = {
  0x61, 0x65, 0x40, 0x28,    0, 0x35, 0x61, 0x85,    0, 0x13, 0x10, 0x35, 0x4e, 0x75, 0x6e, 0x35,
  0x10, 0x35, 0x4e, 0x35, 0x10, 0x35, 0x4c, 0x35, 0x10, 0x35, 0x4e, 0x75, 0x49, 0x35, 0x61, 0x8f,
  0x40,    8, 0x10, 0x35, 0x61, 0x94,    0,  0xb, 0x4e, 0x75, 0x6e, 0x35, 0x10, 0x35, 0x4e, 0x35,
  0x10, 0x35, 0x4c, 0x35, 0x61, 0xa5,    0, 0x29, 0x5f, 0x75, 0x5e, 0x75, 0x7e, 0x35, 0x7f, 0x35,
  0x5e, 0x35, 0x5f, 0x35, 0x4d, 0x35, 0x5f, 0x75, 0x5e, 0x75, 0x4a, 0x35, 0x4b, 0x35, 0x10, 0x35,
  0x49, 0x75, 0x10, 0x35, 0x5f, 0x75, 0x5e, 0x75, 0x7e, 0x35, 0x7f, 0x35, 0x5e, 0x35, 0x5f, 0x35,
  0x4d, 0x35, 0x61, 0xc5,    0, 0x29, 0x50, 0x35, 0x51, 0x35, 0x52, 0x35, 0x53, 0x35, 0x54, 0x35,
  0x55, 0x35, 0x56, 0x35, 0x57, 0x35, 0x58, 0x35, 0x59, 0x35, 0x5a, 0x35, 0x5b, 0x35, 0x5c, 0x35,
  0x5d, 0x35, 0x50, 0x35, 0x51, 0x35, 0x52, 0x35, 0x53, 0x35, 0x54, 0x35, 0x55, 0x35, 0x56, 0x35,
  0x61, 0xe5,    0, 0x29, 0x60, 0x35, 0x61, 0x35, 0x62, 0x35, 0x63, 0x35, 0x64, 0x35, 0x65, 0x35,
  0x66, 0x35, 0x67, 0x35, 0x68, 0x35, 0x69, 0x35, 0x6a, 0x35, 0x6b, 0x35, 0x6c, 0x35, 0x6d, 0x35,
  0x60, 0x35, 0x61, 0x35, 0x62, 0x35, 0x63, 0x35, 0x64, 0x35, 0x65, 0x35, 0x66, 0x35, 0x62,    5,
     0, 0x29, 0x70, 0x35, 0x71, 0x35, 0x72, 0x35, 0x73, 0x35, 0x74, 0x35, 0x75, 0x35, 0x76, 0x35,
  0x77, 0x35, 0x78, 0x35, 0x79, 0x35, 0x7a, 0x35, 0x7b, 0x35, 0x7c, 0x35, 0x7d, 0x35, 0x70, 0x35,
  0x71, 0x35, 0x72, 0x35, 0x73, 0x35, 0x74, 0x35, 0x75, 0x35, 0x76, 0x35, 0xff, 0x61,
};
static const uint8 kAttract_Legendgraphics_2[199+1] = {
  0x61, 0x65, 0x40, 0x28,    0, 0x35, 0x61, 0x85, 0x40, 0x28, 0x10, 0x35, 0x61, 0xa5,    0, 0x1d,
  0x22, 0x35, 0x23, 0x35, 0x10, 0x35, 0x22, 0x35, 0x23, 0x35, 0x10, 0x35, 0x22, 0x35, 0x23, 0x35,
  0x10, 0x35, 0x22, 0x35, 0x23, 0x35, 0x10, 0x35, 0x10, 0x75, 0x23, 0x75, 0x22, 0x75, 0x61, 0xb4,
  0x40,    6, 0x10, 0x35, 0x61, 0xb8,    0,    3, 0x23, 0x75, 0x22, 0x75, 0x61, 0xc5,    0, 0x29,
     4, 0x35,    5, 0x35,    6, 0x35,    4, 0x35,    5, 0x35,    6, 0x35,    4, 0x35,    5, 0x35,
     6, 0x35,    4, 0x35,    5, 0x35,    6, 0x35,    6, 0x75,    5, 0x75,    4, 0x75, 0x10, 0x75,
  0x23, 0x75, 0x22, 0x75,    6, 0x75,    5, 0x75,    4, 0x75, 0x61, 0xe5,    0, 0x29, 0x14, 0x35,
  0x15, 0x35, 0x16, 0x35, 0x14, 0x35, 0x15, 0x35, 0x16, 0x35, 0x14, 0x35, 0x15, 0x35, 0x16, 0x35,
  0x14, 0x35, 0x15, 0x35, 0x16, 0x35, 0x16, 0x75, 0x15, 0x75, 0x14, 0x75,    6, 0x75,    5, 0x75,
     4, 0x75, 0x16, 0x75, 0x15, 0x75, 0x14, 0x75, 0x62,    5,    0, 0x29, 0x24, 0x35, 0x25, 0x35,
  0x26, 0x35, 0x24, 0x35, 0x25, 0x35, 0x26, 0x35, 0x24, 0x35, 0x25, 0x35, 0x26, 0x35, 0x24, 0x35,
  0x25, 0x35, 0x26, 0x35, 0x26, 0x75, 0x25, 0x75, 0x24, 0x75, 0x26, 0x75, 0x25, 0x75, 0x24, 0x75,
  0x26, 0x75, 0x25, 0x75, 0x24, 0x75, 0xff, 0x61,
};
static const uint8 kAttract_Legendgraphics_3[265+1] = {
  0x61, 0x65,    0, 0x29,    0, 0x35,    0, 0x35, 0x1b, 0x35, 0x30, 0x35, 0x31, 0x35, 0x32, 0x35,
     0, 0x35,    0, 0x35,    0, 0x35, 0x33, 0x35, 0x41, 0x35, 0x41, 0x75, 0x33, 0x75,    0, 0x75,
     0, 0x75,    0, 0x75, 0x32, 0x75, 0x31, 0x75, 0x30, 0x75, 0x1b, 0x75,    0, 0x75, 0x61, 0x85,
  0x40, 0x1e, 0x10, 0x35, 0x61, 0x86,    0,    9, 0x34, 0x35,  0xb, 0x35, 0x40, 0x35, 0x41, 0x35,
  0x42, 0x35, 0x61, 0x95,    0,    9, 0x42, 0x75, 0x41, 0x75, 0x40, 0x75,  0xb, 0x75, 0x34, 0x75,
  0x61, 0xa5,    0, 0x29, 0x43, 0x35, 0x44, 0x35,    7, 0x35,    8, 0x35,    9, 0x35,  0xa, 0x35,
  0x10, 0x35,  0xc, 0x35,  0xd, 0x35,  0xe, 0x35,  0xf, 0x35,  0xf, 0x75,  0xe, 0x75,  0xd, 0x75,
   0xc, 0x75, 0x10, 0x75,  0xa, 0x75,    9, 0x75,    8, 0x75,    7, 0x75, 0x44, 0x75, 0x61, 0xc5,
     0, 0x29, 0x35, 0x35, 0x36, 0x35, 0x17, 0x35, 0x18, 0x35, 0x19, 0x35, 0x1a, 0x35, 0x10, 0x35,
  0x1c, 0x35, 0x1d, 0x35, 0x1e, 0x35, 0x1f, 0x35, 0x1f, 0x75, 0x1e, 0x75, 0x1d, 0x75, 0x1c, 0x75,
  0x10, 0x75, 0x1a, 0x75, 0x19, 0x75, 0x18, 0x75, 0x17, 0x75, 0x36, 0x75, 0x61, 0xe5,    0, 0x29,
  0x45, 0x35, 0x46, 0x35, 0x27, 0x35, 0x28, 0x35, 0x29, 0x35, 0x2a, 0x35, 0x2b, 0x35, 0x2c, 0x35,
  0x2d, 0x35, 0x2e, 0x35, 0x2f, 0x35, 0x2f, 0x75, 0x2e, 0x75, 0x2d, 0x75, 0x2c, 0x75, 0x2b, 0x75,
  0x2a, 0x75, 0x29, 0x75, 0x28, 0x75, 0x27, 0x75, 0x46, 0x75, 0x62,    5,    0, 0x29, 0x47, 0x35,
  0x48, 0x35, 0x37, 0x35, 0x38, 0x35, 0x39, 0x35, 0x3a, 0x35, 0x3b, 0x35, 0x3c, 0x35, 0x3d, 0x35,
  0x3e, 0x35, 0x3f, 0x35, 0x3f, 0x75, 0x3e, 0x75, 0x3d, 0x75, 0x3c, 0x75, 0x3b, 0x75, 0x3a, 0x75,
  0x39, 0x75, 0x38, 0x75, 0x37, 0x75, 0x48, 0x75, 0xff, 0x0
};
/*
 * Attract_DrawSpriteSet2 — Batch-write attract mode sprite entries to OAM.
 *
 * Iterates backward through an array of AttractOamInfo entries, placing
 * each sprite into the OAM buffer at the current attract_oam_idx offset
 * (starting at slot 64 to avoid conflicts with HUD sprites). Positions
 * are relative to attract_x_base/attract_y_base, allowing the caller
 * to reposition an entire sprite group by changing the base coordinates.
 *
 * Parameters:
 *   p — Pointer to an array of AttractOamInfo structs (x, y, char, flags, ext)
 *   n — Number of OAM entries to write
 */
void Attract_DrawSpriteSet2(const AttractOamInfo *p, int n) {
  OamEnt *oam = &oam_buf[attract_oam_idx + 64];
  attract_oam_idx += n;
  for (; n--; oam++)
    SetOamPlain(oam, attract_x_base + p[n].x, attract_y_base + p[n].y, p[n].c, p[n].f, p[n].e);
}

/*
 * Attract_ZeldaPrison_Case0 — Initial phase of the Zelda prison vignette.
 *
 * Draws Zelda standing in her cell (6 OAM entries: shadow tiles + body).
 * Increments attract_var5 to advance to the next sub-phase once the
 * guard patrol delay (attract_var4) expires. The VRAM destination
 * counter decrements every other frame, scrolling the background
 * slightly to simulate camera drift. Sets attract_var7 to a negative
 * offset used for background tile positioning.
 */
void Attract_ZeldaPrison_Case0() {
  static const AttractOamInfo kZeldaPrison_Oams0[] = {
    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x84, 0x3b, 2},
    {16,  0, 0x84, 0x7b, 2},
    { 0, 16, 0xa4, 0x3b, 2},
    {16, 16, 0xa4, 0x7b, 2},
  };
  if (!attract_var4)
    attract_var5++;
  if (frame_counter & 1)
    attract_vram_dst--;
  attract_x_base = 0x58;
  attract_y_base = attract_var9;
  Attract_DrawSpriteSet2(kZeldaPrison_Oams0, 6);
  attract_var7 = 0xf8d9;
}

/*
 * Attract_ZeldaPrison_Case1 — Second phase of the Zelda prison scene.
 *
 * Displays a timed text message while Zelda is shown in her cell. Once
 * the text finishes (oam_priority_value reaches 0), Zelda scrolls upward
 * (attract_var9 decrements toward 0x6e). After reaching position, the
 * scene fades out by decrementing INIDISP_copy on odd frames. The five
 * sets of 6 OAM entries represent different Zelda animation frames,
 * selected based on attract_var10 thresholds (0xC0/0xB8/0xB0/0xA0)
 * to animate her looking around the cell.
 */
void Attract_ZeldaPrison_Case1() {
  int k;
  static const AttractOamInfo kZeldaPrison_Oams1[] = {
    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x84, 0x3b, 2},
    {16,  0, 0x84, 0x7b, 2},
    { 0, 16, 0xa4, 0x3b, 2},
    {16, 16, 0xa4, 0x7b, 2},

    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0xc4, 0x3b, 2},
    {16,  0, 0xc2, 0x3b, 2},
    { 0, 16, 0xe4, 0x3b, 2},
    {16, 16, 0xe6, 0x3b, 2},

    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x88, 0x3b, 2},
    {16,  0, 0x8a, 0x3b, 2},
    { 0, 16, 0xa8, 0x3b, 2},
    {16, 16, 0xaa, 0x3b, 2},

    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x82, 0x3b, 2},
    {16,  0, 0x82, 0x7b, 2},
    { 0, 16, 0xa2, 0x3b, 2},
    {16, 16, 0xa2, 0x7b, 2},

    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x80, 0x3b, 2},
    {16,  0, 0x80, 0x7b, 2},
    { 0, 16, 0xa0, 0x3b, 2},
    {16, 16, 0xa0, 0x7b, 2},
  };
  if (attract_var10 < 0x80 && (Attract_ShowTimedTextMessage(), oam_priority_value != 0)) {
    k = 4;
  } else if (attract_var9 != 0x6e) {
    attract_var9--;
    k = 0;
  } else {
    if (attract_var10 < 31 && !(attract_var10 & 1))
      INIDISP_copy--;
    if (!--attract_var10) {
      attract_sequence++;
      attract_state -= 2;
      return;
    }

    k = attract_var10 >= 0xc0 ? 0 :
      attract_var10 >= 0xb8 ? 1 :
      attract_var10 >= 0xb0 ? 2 :
      attract_var10 >= 0xa0 ? 3 : 4;
  }
  if (frame_counter & 1)
    attract_vram_dst--;
  attract_x_base = 0x58;
  attract_y_base = attract_var9;
  Attract_DrawSpriteSet2(&kZeldaPrison_Oams1[k * 6], 6);
}

/*
 * Attract_ZeldaPrison_DrawA — Draw the prison guard's walking sprite.
 *
 * Renders a 2-tile guard sprite at the current attract base position.
 * The guard bobs vertically by 1 pixel based on attract_var1 bit 3,
 * simulating a walking animation. The 'ext' byte encodes whether
 * the X position exceeds 8 bits (attract_x_base_hi), needed for
 * the SNES OAM extended size/position table.
 */
void Attract_ZeldaPrison_DrawA() {
  OamEnt *oam = &oam_buf[64 + attract_oam_idx];
  uint8 ext = attract_x_base_hi ? 3 : 2;
  int j = (attract_var1 >> 3) & 1;
  SetOamPlain(oam + 0, attract_x_base, attract_y_base + j, 6, 0x3d, ext);
  SetOamPlain(oam + 1, attract_x_base, attract_y_base + 10, j ? 10 : 8, 0x3d, ext);
  attract_oam_idx += 2;
}

/*
 * Attract_MaidenWarp_Case0 — Pre-animation phase for the maiden warp scene.
 *
 * Waits for attract_var11 to become nonzero (set when the altar maiden
 * reaches her starting position), then advances attract_var5 to begin
 * the crystal sealing animation.
 */
void Attract_MaidenWarp_Case0() {
  if (attract_var11)
    attract_var5++;
}

/*
 * Attract_MaidenWarp_Case1 — Crystal sealing animation expanding outward.
 *
 * Draws the maiden inside a crystal that grows from 2 to 14 OAM entries
 * over time. Two animation frames alternate every 4 real frames (k * 14
 * selects between palette variants). The crystal expansion is driven by
 * attract_var21, which increments after attract_var20 counts down to 0.
 * Sound effects trigger at specific thresholds: the warp sound at the
 * start (0x27) and the sealing flash at phase 6 (0x2b with palette flash).
 */
void Attract_MaidenWarp_Case1() {
  static const AttractOamInfo kZeldaPrison_MaidenWarpCase1_Oam[] = {
    { 0,  0, 0xce, 0x35, 0},
    {28,  0, 0xce, 0x35, 0},
    {-2,  3, 0x26, 0x75, 0},
    {30,  3, 0x26, 0x35, 0},
    {-2, 11, 0x36, 0x75, 0},
    {30, 11, 0x36, 0x35, 0},
    { 0, 16, 0x26, 0x75, 0},
    {28, 16, 0x26, 0x35, 0},
    { 0, 24, 0x36, 0x75, 0},
    {28, 24, 0x36, 0x35, 0},
    { 2, 16, 0x20, 0x35, 2},
    {18, 16, 0x20, 0x75, 2},
    { 2, 32, 0x20, 0xb5, 2},
    {18, 32, 0x20, 0xf5, 2},
    { 0,  0, 0xce, 0x37, 0},
    {28,  0, 0xce, 0x37, 0},
    {-2,  3, 0x26, 0x77, 0},
    {30,  3, 0x26, 0x37, 0},
    {-2, 11, 0x36, 0x77, 0},
    {30, 11, 0x36, 0x37, 0},
    { 0, 16, 0x26, 0x77, 0},
    {28, 16, 0x26, 0x37, 0},
    { 0, 24, 0x36, 0x77, 0},
    {28, 24, 0x36, 0x37, 0},
    { 2, 16, 0x22, 0x37, 2},
    {18, 16, 0x22, 0x77, 2},
    { 2, 32, 0x22, 0xb7, 2},
    {18, 32, 0x22, 0xf7, 2},
  };
  int k = frame_counter >> 2 & 1;
  static const uint8 kAttract_MaidenWarp_Case1_Num[8] = { 2, 2, 2, 6, 6, 10, 10, 14 };
  attract_x_base = 110;
  attract_y_base = 72;
  Attract_DrawSpriteSet2(kZeldaPrison_MaidenWarpCase1_Oam + k * 14, kAttract_MaidenWarp_Case1_Num[attract_var21 >> 1 & 7]);

  if (!attract_var21 && attract_var20 == 0x70)
    sound_effect_2 = 0x27;

  if (attract_var21 == 15) {
    attract_var5++;
  } else {
    if (attract_var21 == 6) {
      intro_times_pal_flash = 0x90;
      sound_effect_2 = 0x2b;
    }
    if (attract_var20)
      attract_var20--;
    else
      attract_var21++;
  }
}

/*
 * Attract_MaidenWarp_Case2 — Crystal sealing animation collapsing inward.
 *
 * Reverses the expansion from Case1: the crystal shrinks from 14 OAM
 * entries back down to 4 as attract_var21 decrements. Once fully
 * collapsed (attract_var21 == 0), attract_var19 counts down to advance
 * to the next sub-phase. The offset into the OAM array is calculated
 * as (14 - n) to draw from the innermost tiles outward.
 */
void Attract_MaidenWarp_Case2() {
  static const uint8 kMaidenWarp_Case2_Num[8] = { 4, 4, 8, 8, 12, 12, 14, 14 };
  static const AttractOamInfo kAttract_MaidenWarpCase2_Oam[] = {
    { 0,  0, 0xce, 0x35, 0},
    {28,  0, 0xce, 0x35, 0},
    {-2,  3, 0x26, 0x75, 0},
    {30,  3, 0x26, 0x35, 0},
    {-2, 11, 0x36, 0x75, 0},
    {30, 11, 0x36, 0x35, 0},
    { 0, 16, 0x26, 0x75, 0},
    {28, 16, 0x26, 0x35, 0},
    { 0, 24, 0x36, 0x75, 0},
    {28, 24, 0x36, 0x35, 0},
    { 2, 16, 0x20, 0x35, 2},
    {18, 16, 0x20, 0x75, 2},
    { 2, 32, 0x20, 0xb5, 2},
    {18, 32, 0x20, 0xf5, 2},
    { 0,  0, 0xce, 0x37, 0},
    {28,  0, 0xce, 0x37, 0},
    {-2,  3, 0x26, 0x77, 0},
    {30,  3, 0x26, 0x37, 0},
    {-2, 11, 0x36, 0x77, 0},
    {30, 11, 0x36, 0x37, 0},
    { 0, 16, 0x26, 0x77, 0},
    {28, 16, 0x26, 0x37, 0},
    { 0, 24, 0x36, 0x77, 0},
    {28, 24, 0x36, 0x37, 0},
    { 2, 16, 0x22, 0x37, 2},
    {18, 16, 0x22, 0x77, 2},
    { 2, 32, 0x22, 0xb7, 2},
    {18, 32, 0x22, 0xf7, 2},
  };
  attract_x_base = 110;
  attract_y_base = 72;
  int k = frame_counter >> 2 & 1;
  int n = kMaidenWarp_Case2_Num[attract_var21 >> 1 & 7];
  Attract_DrawSpriteSet2(kAttract_MaidenWarpCase2_Oam + k * 14 + (14 - n), n);
  if (attract_var21 == 0) {
    if (!--attract_var19)
      attract_var5++;
  } else {
    attract_var21--;
  }
}

/*
 * Attract_MaidenWarp_Case3 — Maiden disappears after crystal sealing.
 *
 * Shows the maiden sprite briefly at two alternating X positions (0x78
 * and 0x70) based on attract_var21 bit 3, then triggers a vanish sound
 * effect (sfx 51) at phase 6 and sets attract_var15 to mark her gone.
 * After 0x40 frames, sets attract_var21 to 224 and advances the phase.
 * The first OAM entry is a sparkle effect, the second pair is the
 * maiden's body tiles.
 */
void Attract_MaidenWarp_Case3() {
  static const AttractOamInfo kAttract_MaidenWarpCase3_Oam[] = {
    { 0,  0, 0xc6, 0x3d, 2},
    { 0,  0, 0x24, 0x35, 2},
    {16,  0, 0x24, 0x75, 2},
  };
  static const uint8 kMaidenWarp_Case3_Xbase[2] = { 0x78, 0x70 };

  if (attract_var21 == 6) {
    attract_var15++;
    sound_effect_1 = 51;
  } else if (attract_var21 == 0x40) {
    attract_var21 = 224;
    attract_var5++;
  } else if (attract_var21 < 0xf) {
    int k = attract_var21 >> 3 & 1;
    attract_x_base = kMaidenWarp_Case3_Xbase[k];
    attract_y_base = 0x60;
    Attract_DrawSpriteSet2(kAttract_MaidenWarpCase3_Oam + k, k ? 2 : 1);
  }
  attract_var21++;
}

/*
 * Attract_MaidenWarp_Case4 — Final fade-out of the maiden warp scene.
 *
 * Shows the concluding text message, then fades out by decrementing
 * INIDISP_copy on even frames when attract_var21 < 31. Once attract_var21
 * reaches 0, sets attract_var22 to signal the entire Agahnim altar
 * dramatization is complete.
 */
void Attract_MaidenWarp_Case4() {
  Attract_ShowTimedTextMessage();
  if (!oam_priority_value) {
    if (attract_var21 < 31 && !(attract_var21 & 1)) {
      INIDISP_copy--;
    }
    if (!--attract_var21)
      attract_var22++;
  }
}

/*
 * Dungeon_LoadAndDrawEntranceRoom — Load and render a dungeon room for
 * use as a background in attract mode scenes.
 *
 * Loads the dungeon entrance specified by index 'a', clears torch state,
 * draws the room tiles, and resets the player/torch background. This
 * reuses the dungeon loading infrastructure to create static scene
 * backdrops for the attract vignettes (e.g., entrance 0x73 = Zelda's
 * prison, 0x74 = throne room, 0x75 = Agahnim altar).
 *
 * Parameters:
 *   a — Dungeon entrance index from the ROM entrance table
 */
void Dungeon_LoadAndDrawEntranceRoom(uint8 a) {  // 82c533
  attract_room_index = a;
  Dungeon_LoadEntrance();
  dung_num_lit_torches = 0;
  hdr_dungeon_dark_with_lantern = 0;
  Dungeon_LoadAndDrawRoom();
  Dungeon_ResetTorchBackgroundAndPlayer();
}

/*
 * Dungeon_SaveAndLoadLoadAllPalettes — Configure tilesets and load all
 * palette sets needed for a dungeon-based attract scene.
 *
 * Sets the main and auxiliary tile theme indices, initializes tilesets,
 * then loads the complete set of sprite and background palettes needed
 * for the scene. The palette_aux_or_main offset of 0x200 selects the
 * auxiliary palette buffer. Increments flag_update_cgram_in_nmi to
 * ensure the palette data is transferred to the PPU during the next
 * vertical blank.
 *
 * Parameters:
 *   a — Tile theme index for both main and auxiliary tileset slots
 *   k — Sprite graphics pack index
 */
void Dungeon_SaveAndLoadLoadAllPalettes(uint8 a, uint8 k) {  // 82c546
  sprite_graphics_index = k;
  main_tile_theme_index = a;
  aux_tile_theme_index = a;
  InitializeTilesets();
  overworld_palette_aux_or_main = 0x200;
  flag_update_cgram_in_nmi++;
  Palette_BgAndFixedColor_Black();
  Palette_Load_Sp0L();
  Palette_Load_SpriteMain();
  Palette_Load_Sp5L();
  Palette_Load_Sp6L();
  Palette_Load_SpriteEnvironment_Dungeon();
  Palette_Load_HUD();
  Palette_Load_DungeonSet();
}

/*
 * Module14_Attract — Top-level state machine for the attract/demo mode.
 *
 * Called every frame while the game module is 14 (attract mode). Checks
 * for Start/A button presses to skip directly to file select (state 9),
 * but only when the screen is visible and not in a loading/fade state.
 * The state machine cycles through:
 *   0: Fade out from previous screen
 *   1: Initialize attract graphics (legend text, palettes, HDMA)
 *   2,6: Fade out current scene
 *   3,7: Load next scene (polka dots, world map, throne, prison, altar)
 *   4: Fade in new scene
 *   5,8: Enact/dramatize the current story scene with animation
 *   9: Skip to file select (player pressed Start)
 *
 * The attract_sequence counter tracks which vignette is active (0-5).
 */
void Module14_Attract() {  // 8cedad
  uint8 st = attract_state;
  /* Allow skipping to file select only when screen is visible (not blanked
   * or force-blanked at 128), not in initial state, and not mid-fade.
   * 0x90 checks Start (0x10) and A (0x80) on the high byte of joypad input. */
  if (INIDISP_copy && INIDISP_copy != 128 && st && st != 2 && st != 6 && filtered_joypad_H & 0x90)
    attract_state = st = 9;

  switch (st) {
  case 0: Attract_Fade(); break;
  case 1: Attract_InitGraphics(); break;
  case 2: Attract_FadeOutSequence(); break;
  case 3: Attract_LoadNewScene(); break;
  case 4: Attract_FadeInSequence(); break;
  case 5: Attract_EnactStory(); break;
  case 6: Attract_FadeOutSequence(); break;
  case 7: Attract_LoadNewScene(); break;
  case 8: Attract_EnactStory(); break;
  case 9: Attract_SkipToFileSelect(); break;
  }
}

/*
 * Attract_Fade — Fade out the screen before initializing attract graphics.
 *
 * Runs the Triforce polyhedral animation and sword flash effect while
 * decrementing screen brightness (INIDISP_copy) each frame. Once fully
 * dark, enables force blank, disables the polyhedral NMI thread and
 * IRQ rendering, then advances to the graphics init state.
 */
void Attract_Fade() {  // 8cede6
  Intro_HandleAllTriforceAnimations();
  intro_did_run_step = 0;
  is_nmi_thread_active = 0;
  Intro_PeriodicSwordAndIntroFlash();
  if (INIDISP_copy) {
    INIDISP_copy--;
    return;
  }
  EnableForceBlank();
  irq_flag = 255;
  is_nmi_thread_active = 0;
  nmi_flag_update_polyhedral = 0;
  attract_state++;
}

/*
 * Attract_InitGraphics — One-time initialization for the attract sequence.
 *
 * Clears attract state variables, erases tile maps, loads BG3 character
 * graphics for the legend text, configures palettes (overworld bg, HUD,
 * Link's armor). Sets up the polka-dot scrolling background via
 * Attract_BuildBackgrounds(), configures the dialogue system for the
 * first legend text (message 0x112), and initializes HDMA windowing
 * effects that create the text overlay region. Starts the attract
 * background music (track 6) and skips ahead 3 states to begin the
 * first scene dramatization.
 */
void Attract_InitGraphics() {  // 8cee0c
  /* Clear all attract-mode working variables (0x51 bytes starting from
   * attract_var12) to ensure clean state for the new attract cycle. */
  memset(&attract_var12, 0, 0x51);
  EraseTileMaps_normal();
  Attract_LoadBG3GFX();
  overworld_palette_mode = 4;
  hud_palette = 1;
  overworld_palette_aux_or_main = 0;
  Palette_Load_HUD();
  overworld_palette_aux_or_main = 0x200;
  Palette_Load_OWBGMain();
  Palette_Load_HUD();
  Palette_Load_LinkArmorAndGloves();
  /* Set palette index 0x1d to white (0x3800 in SNES BGR555 format) for
   * the text overlay background color. */
  main_palette_buffer[0x1d] = 0x3800;
  flag_update_cgram_in_nmi++;
  /* Offset BG3 vertically by 20 pixels to position the legend text area */
  BYTE(BG3VOFS_copy2) = 20;
  Attract_BuildBackgrounds();
  messaging_module = 0;
  /* Message 0x112 is the first page of the attract backstory legend */
  dialogue_message_index = 0x112;
  BG2VOFS_copy2 = 0;
  /* 0x1010 = ~4112 frames total for the polka-dot legend display phase */
  attract_legend_ctr = 0x1010;
  /* Skip 3 states ahead: bypass the scene-load states since we already
   * built the polka-dot background inline above. */
  attract_state += 3;
  /* Configure HDMA channels for window position tables that create the
   * rectangular text overlay region on BG3. WH0/WH2 are SNES window
   * position registers. */
  HdmaSetup(0xCFA87, 0xCFA94, 1, (uint8)WH0, (uint8)WH2, 0);
  HDMAEN_copy = 0xc0;

  /* Configure windowing: disable BG1/BG2 windows, enable object window
   * masking (0xb0), apply window to main screen BG layers 1+2. */
  W12SEL_copy = 0;
  W34SEL_copy = 0;
  WOBJSEL_copy = 0xb0;
  TMW_copy = 3;
  TSW_copy = 0;
  /* Fixed color data for the subtractive color math: RGB components
   * with low intensity (0x05 each) to create a dark overlay effect. */
  COLDATA_copy0 = 0x25;
  COLDATA_copy1 = 0x45;
  COLDATA_copy2 = 0x85;
  /* CGWSEL 0x10: color math on main screen inside window only.
   * CGADSUB 0xa3: subtract fixed color from BG1+BG2+backdrop. */
  CGWSEL_copy = 0x10;
  CGADSUB_copy = 0xa3;

  /* Start the attract mode music (track 6 = prologue theme) */
  music_control = 6;
  attract_legend_flag++;
}

/*
 * Attract_FadeInStep — Gradually increase screen brightness for scene fade-in.
 *
 * Increments INIDISP_copy (screen brightness 0-15) every other frame,
 * using link_speed_setting as a frame skip counter. Once brightness
 * reaches maximum (15), sets attract_var18 to signal the fade is complete.
 * This is the non-advancing variant used within scene dramatizations.
 */
void Attract_FadeInStep() {  // 8ceea6
  if (INIDISP_copy != 15) {
    if (sign8(--link_speed_setting)) {
      INIDISP_copy++;
      link_speed_setting = 1;
    }
  } else {
    attract_var18++;
  }
}

/*
 * Attract_FadeInSequence — Fade in a new attract scene, advancing state.
 *
 * Same brightness ramp as Attract_FadeInStep, but advances attract_state
 * once full brightness is reached. Used during scene transitions.
 */
void Attract_FadeInSequence() {  // 8ceeba
  if (INIDISP_copy != 15) {
    if (sign8(--link_speed_setting)) {
      INIDISP_copy++;
      link_speed_setting = 1;
    }
  } else {
    attract_state++;
  }
}

/*
 * Attract_FadeOutSequence — Fade out the current attract scene.
 *
 * Decrements screen brightness every other frame. Once fully dark,
 * enables force blank, erases tile maps, and advances attract_state
 * to begin loading the next scene.
 */
void Attract_FadeOutSequence() {  // 8ceecb
  if (INIDISP_copy != 0) {
    if (sign8(--link_speed_setting)) {
      INIDISP_copy--;
      link_speed_setting = 1;
    }
  } else {
    EnableForceBlank();
    EraseTileMaps_normal();
    attract_state++;
  }
}

/*
 * Attract_LoadNewScene — Dispatch to the loader for the current scene.
 *
 * Routes to the appropriate scene preparation function based on
 * attract_sequence (0 = polka dots, 1 = world map, 2 = throne room,
 * 3 = Zelda's prison, 4 = maiden warp, 5 = end of story / reset).
 */
void Attract_LoadNewScene() {  // 8ceee5
  switch (attract_sequence) {
  case 0: AttractScene_PolkaDots(); break;
  case 1: AttractScene_WorldMap(); break;
  case 2: AttractScene_ThroneRoom(); break;
  case 3: Attract_PrepZeldaPrison(); break;
  case 4: Attract_PrepMaidenWarp(); break;
  case 5: AttractScene_EndOfStory(); break;
  }
}

/*
 * AttractScene_PolkaDots — Minimal setup for the polka-dot scrolling scene.
 *
 * The polka-dot background was already built during Attract_InitGraphics(),
 * so this just resets the legend graphics index and advances state with
 * brightness at 0 (will fade in next frame).
 */
void AttractScene_PolkaDots() {  // 8ceef8
  attract_next_legend_gfx = 0;
  attract_state++;
  INIDISP_copy = 0;
}

/*
 * AttractScene_WorldMap — Initialize the Mode 7 world map zoom sequence.
 *
 * Switches to SNES BG Mode 7 for the hardware affine transform effect.
 * Loads the Light World overworld map tiles, sets the Mode 7 center
 * point (M7X/M7Y) and initial scroll position. Initializes the zoom
 * timer to 255 frames and pre-computes the first frame of HDMA scaling
 * data via Attract_ControlMapZoom(). Uses subtractive color math
 * (CGWSEL 0x80 + CGADSUB 0x21) to darken the map edges.
 */
void AttractScene_WorldMap() {  // 8ceeff
  zelda_ppu_write(BG1SC, 0x13);
  zelda_ppu_write(BG2SC, 0x3);
  CGWSEL_copy = 0x80;
  CGADSUB_copy = 0x21;
  BGMODE_copy = 7;
  WorldMap_LoadLightWorldMap();
  M7Y_copy = 0xed;
  M7X_copy = 0x100;
  BG1HOFS_copy = 0x80;
  BG1VOFS_copy = 0xc0;
  timer_for_mode7_zoom = 255;
  Attract_ControlMapZoom();
  attract_var10 = 1;
  attract_state++;
  INIDISP_copy = 0;
}

/*
 * AttractScene_ThroneRoom — Load the Hyrule Castle throne room scene.
 *
 * Disables HDMA, configures additive color math, loads common sprite
 * graphics, then loads dungeon entrance 0x74 (throne room) as the
 * background. Saves and restores attract_var12 and attract_state across
 * the dungeon load because the loading routines clobber those addresses.
 * Sets up dungeon palettes, initializes the dialogue system for message
 * 0x113 (throne room narrative), and sets attract_var13 as a fade-out
 * countdown timer (0xE0 frames).
 */
void AttractScene_ThroneRoom() {  // 8cef4e
  HDMAEN_copy = 0;
  CGWSEL_copy = 2;
  CGADSUB_copy = 0x20;
  misc_sprites_graphics_index = 10;
  LoadCommonSprites();
  uint16 bak0 = attract_var12;
  uint16 bak1 = WORD(attract_state);
  Dungeon_LoadAndDrawEntranceRoom(0x74);
  WORD(attract_state) = bak1;
  attract_var12 = bak0;
  palette_main_indoors = 0;
  palette_sp0l = 0;
  palette_sp5l = 14;
  palette_sp6l = 3;
  Dungeon_SaveAndLoadLoadAllPalettes(0, 0x7e);

  main_palette_buffer[0x1d] = 0x3800;
  messaging_module = 0;
  dialogue_message_index = 0x113;
  attract_var10 = 2;
  attract_var13 = 0xe0;
  oam_priority_value = 0x210;

  Attract_PrepFinish();
}

/*
 * Attract_PrepFinish — Common finalization after loading an attract scene.
 *
 * Advances attract_state, sets brightness to 0 for fade-in, resets the
 * BG3 vertical offset, and masks BG2 scroll positions to 9 bits (0x1FF)
 * to keep them within the valid SNES tile map address range.
 */
void Attract_PrepFinish() {  // 8cefc0
  attract_state++;
  INIDISP_copy = 0;
  BYTE(BG3VOFS_copy2) = 0;
  BG2HOFS_copy &= 0x1ff;
  BG2VOFS_copy &= 0x1ff;
  BG2HOFS_copy2 &= 0x1ff;
  BG2VOFS_copy2 &= 0x1ff;
}

/*
 * Attract_PrepZeldaPrison — Load the Zelda prison cell scene background.
 *
 * Loads dungeon entrance 0x73 (the prison cell in Hyrule Castle basement),
 * configures indoor palette set 2 with appropriate sprite palettes, and
 * initializes all the working variables for the prison scene animation
 * (Zelda's vertical position at 148, guard patrol counter, walk cycle
 * state, etc.). Sets dialogue message 0x114 for the prison narrative.
 */
void Attract_PrepZeldaPrison() {  // 8cefe3
  CGWSEL_copy = 0;
  CGADSUB_copy = 0;

  uint16 bak0 = attract_var12;
  uint16 bak1 = WORD(attract_state);
  Dungeon_LoadAndDrawEntranceRoom(0x73);
  WORD(attract_state) = bak1;
  attract_var12 = bak0;

  palette_main_indoors = 2;
  palette_sp0l = 0;
  palette_sp5l = 14;
  palette_sp6l = 3;
  Dungeon_SaveAndLoadLoadAllPalettes(1, 0x7f);
  main_palette_buffer[0x1d] = 0x3800;

  messaging_module = 0;
  dialogue_message_index = 0x114;

  attract_var9 = 148;
  attract_vram_dst = 0x68;
  attract_var1 = 0;
  attract_var3 = 0;
  attract_x_base_hi = 0;
  attract_var17 = 0;
  attract_var18 = 0;
  attract_var10 = 255;
  oam_priority_value = 0x240;
  Attract_PrepFinish();
}

/*
 * Attract_PrepMaidenWarp — Load the Agahnim altar / maiden warp scene.
 *
 * Loads dungeon entrance 0x75 (the altar room where Agahnim seals the
 * maidens into crystals). Loads a full set of palettes twice — first the
 * main palette buffer, then via Dungeon_SaveAndLoadLoadAllPalettes for
 * the auxiliary buffer. Copies white (0x3800) to both palette buffers at
 * index 0x1d. Initializes all maiden warp animation variables and sets
 * dialogue message 0x115 for the altar narrative. The oam_priority_value
 * of 0xC0 places attract sprites behind the dungeon background.
 */
void Attract_PrepMaidenWarp() {  // 8cf058
  uint16 bak0 = attract_var12;
  uint16 bak1 = WORD(attract_state);
  Dungeon_LoadAndDrawEntranceRoom(0x75);
  WORD(attract_state) = bak1;
  attract_var12 = bak0;

  palette_main_indoors = 0;
  palette_sp0l = 0;
  palette_sp5l = 14;
  palette_sp6l = 3;

  overworld_palette_aux_or_main = 0;
  Palette_Load_Sp0L();
  Palette_Load_SpriteMain();
  Palette_Load_Sp5L();
  Palette_Load_Sp6L();
  Palette_Load_SpriteEnvironment_Dungeon();
  Palette_Load_HUD();
  Palette_Load_DungeonSet();
  Dungeon_SaveAndLoadLoadAllPalettes(2, 0x7f);
  aux_palette_buffer[0x1d] = main_palette_buffer[0x1d] = 0x3800;

  messaging_module = 0;
  dialogue_message_index = 0x115;
  attract_var10 = 255;
  BYTE(attract_vram_dst) = 112;
  attract_var19 = 112;
  attract_var20 = 112;
  attract_var1 = 8;
  attract_var17 = 0;
  attract_var21 = 0;
  attract_var15 = 0;
  attract_var18 = 0;
  attract_var5 = 0;
  attract_var11 = 0;

  oam_priority_value = 0xc0;
  Attract_PrepFinish();
}

/*
 * AttractScene_EndOfStory — Conclude the attract sequence and return
 * to the title/file select screen.
 *
 * Sets up HDMA for the conclusion transition, then calls Death_Func31
 * to reset the game state back to Module 0 (intro/title screen).
 */
void AttractScene_EndOfStory() {  // 8cf0dc
  Attract_SetUpConclusionHDMA();
  Death_Func31();
}

/*
 * Death_Func31 — Reset game state to restart the intro/title sequence.
 *
 * Disables NMI core updates, darkens the screen, reloads overworld
 * palettes, zeroes all scroll/Mode 7 registers, fades music (0xF1),
 * and sets the module index back to 0 (intro) with sub-module 10
 * to restart from the appropriate intro phase.
 */
void Death_Func31() {  // 8cf0e2
  nmi_disable_core_updates++;
  Intro_InitializeMemory_darken();
  Overworld_LoadAllPalettes();
  BYTE(BG3VOFS_copy2) = 0;
  M7Y_copy = 0;
  M7X_copy = 0;
  BG1HOFS_copy = 0;
  BG1VOFS_copy = 0;
  BG2HOFS_copy = 0;
  BG2VOFS_copy = 0;
  music_control = 0xF1;
  attract_sequence = 0;
  main_module_index = 0;
  submodule_index = 10;
  subsubmodule_index = 10;
}

/*
 * Attract_EnactStory — Dispatch to the per-frame dramatization handler
 * for the current attract scene.
 *
 * Routes based on attract_sequence to the appropriate animation handler:
 * polka dots (scrolling BG + legend text), world map (Mode 7 zoom),
 * throne room (king + advisor sprites), prison (Zelda + guard patrol),
 * or Agahnim altar (maiden crystal sealing ceremony).
 */
void Attract_EnactStory() {  // 8cf115
  switch (attract_sequence) {
  case 0: AttractDramatize_PolkaDots(); break;
  case 1: AttractDramatize_WorldMap(); break;
  case 2: Attract_ThroneRoom(); break;
  case 3: AttractDramatize_Prison(); break;
  case 4: AttractDramatize_AgahnimAltar(); break;
  }
}

/*
 * AttractDramatize_PolkaDots — Animate the polka-dot background with
 * scrolling legend text overlay.
 *
 * Scrolls BG1 and BG2 in opposite diagonal directions every 4 frames
 * to create a parallax kaleidoscope effect with the tiled polka-dot
 * pattern. Loads legend image tile maps when attract_legend_flag is set
 * (alternating between the 4 legend graphic panels). Suppresses player
 * input during text display. Counts down attract_legend_ctr; when it
 * expires, advances to the next scene. Fades out during the last 24
 * frames.
 */
void AttractDramatize_PolkaDots() {  // 8cf126
  if (!(frame_counter & 3)) {
    BYTE(BG1VOFS_copy)++;
    BYTE(BG1HOFS_copy)++;
    BYTE(BG2VOFS_copy)++;
    BYTE(BG2HOFS_copy)--;
  }

  if (attract_legend_flag) {
    Attract_BuildNextImageTileMap();
    attract_legend_flag = 0;
    attract_next_legend_gfx += 2;
  }
  joypad1L_last = 0;
  filtered_joypad_L = 0;
  filtered_joypad_H = 0;
  RenderText();
  if (!--attract_legend_ctr) {
    attract_sequence++;
    attract_state -= 3;
  } else {
    if (attract_legend_ctr < 0x18 && attract_legend_ctr & 1)
      INIDISP_copy--;
  }
}

/*
 * AttractDramatize_WorldMap — Animate the Mode 7 world map zoom-out.
 *
 * Decrements timer_for_mode7_zoom each frame (throttled by attract_var10)
 * and recalculates the per-scanline HDMA zoom table. The zoom starts
 * fully zoomed in (timer=255) and pulls back to an overview. Fades out
 * during the last 15 timer ticks. When the timer reaches 0, switches
 * back to BG Mode 9 (regular tiled mode) and advances to the next scene.
 */
void AttractDramatize_WorldMap() {  // 8cf176
  if (timer_for_mode7_zoom != 0) {
    if (timer_for_mode7_zoom < 15)
      INIDISP_copy--;
    if (!--attract_var10) {
      attract_var10 = 1;
      timer_for_mode7_zoom -= 1;
      Attract_ControlMapZoom();
    }
  } else {
    EnableForceBlank();
    BGMODE_copy = 9;
    EraseTileMaps_normal();
    attract_sequence++;
    attract_state -= 2;
  }
}

/*
 * Attract_ThroneRoom — Animate the throne room scene with king and
 * advisor sprites scrolling into view.
 *
 * Draws two sprite groups: the king on the throne (4-tile body at base
 * position 0) and an advisor/guard pair (6-tile figure at position 1).
 * Both scroll upward as BG2VOFS decrements. Once the scroll reaches 0,
 * displays the timed text message, then fades out over attract_var13
 * frames. The OAM entries use mirrored tile pairs (0x3b/0x7b flags)
 * to create symmetrical character sprites from shared tile data.
 */
void Attract_ThroneRoom() {  // 8cf1c8
  static const AttractOamInfo kThroneRoom_Oams[] = {
    {16, 16, 0x2a, 0x7b, 2},
    { 0, 16, 0x2a, 0x3b, 2},
    {16,  0, 0x0a, 0x7b, 2},
    { 0,  0, 0x0a, 0x3b, 2},
    { 0,  0, 0x0c, 0x31, 2},
    {16,  0, 0x0e, 0x31, 2},
    {32,  0, 0x0c, 0x71, 2},
    { 0, 16, 0x2c, 0x31, 2},
    {16, 16, 0x2e, 0x31, 2},
    {32, 16, 0x2c, 0x71, 2},
  };
  static const uint8 kThroneRoom_OamOffs[3] = { 0, 4, 10 };
  static const int8 kAttract_ThroneRoom_Xbase[2] = { 80, 104 };
  static const int8 kAttract_ThroneRoom_Ybase[2] = { 88, 32 };
  attract_oam_idx = 0;
  if (!attract_var15) {
    if (INIDISP_copy != 15)
      INIDISP_copy++;
    else
      attract_var15++;
  }
  if (!BG2VOFS_copy) {
    Attract_ShowTimedTextMessage();
    if (!oam_priority_value) {
      if (attract_var13 < 31 && !(attract_var13 & 1))
        INIDISP_copy--;
      if (!--attract_var13) {
        attract_sequence++;
        attract_state++;
        return;
      }
    }
  } else {
    BG2VOFS_copy--;
    BG1VOFS_copy--;
  }
  for (int i = 1; i >= 0; i--) {
    const AttractOamInfo *oamp = &kThroneRoom_Oams[kThroneRoom_OamOffs[i]];
    int n = kThroneRoom_OamOffs[i + 1] - kThroneRoom_OamOffs[i];
    uint16 y = kAttract_ThroneRoom_Ybase[i] - BG2VOFS_copy;
    if (!sign16(y + 32)) {
      attract_x_base = kAttract_ThroneRoom_Xbase[i];
      attract_y_base = y;
      Attract_DrawSpriteSet2(oamp, n);
    }
  }

  attract_var7 = 0xf8a7;
}

/*
 * AttractDramatize_Prison — Per-frame animation for Zelda's prison scene.
 *
 * Handles fade-in, draws Zelda in her cell, animates a guard walking
 * back and forth (with soldier simulation), and dispatches to sub-phases
 * (Case0/Case1) for the detailed animation sequence. The guard's X
 * position oscillates using a 16-entry sinusoidal lookup table
 * (kAttract_ZeldaPrison_Tab0). Two soldiers are placed at relative
 * offsets from the guard position, with walk-cycle graphics advancing
 * every 8 frames. A footstep sound effect (sfx 4) plays when the guard
 * reverses direction and the position is within the visible range.
 */
void AttractDramatize_Prison() {  // 8cf27a
  static const uint8 kAttract_ZeldaPrison_Tab0[16] = { 0, 1, 2, 3, 4, 5, 5, 5, 4, 4, 3, 3, 2, 2, 1, 1 };
  static const int8 kZeldaPrison_Soldier_X[2] = { 32, -12 };
  static const int8 kZeldaPrison_Soldier_Y[2] = { 24, 24 };
  static const uint8 kZeldaPrison_Soldier_Dir[2] = { 1, 1 };
  static const uint8 kZeldaPrison_Soldier_Flags[2] = { 9, 7 };

  attract_oam_idx = 0;
  if (!attract_var18)
    Attract_FadeInStep();
  attract_x_base = 56;
  Attract_DrawZelda();
  if (attract_var10 >= 192) {
    attract_y_base = 112;
    if (sign8(--attract_var17))
      attract_var17 = 0xf;
    int t = attract_vram_dst + kAttract_ZeldaPrison_Tab0[attract_var17];
    attract_x_base_hi = t >> 8;
    attract_x_base = t;
    Attract_ZeldaPrison_DrawA();

    for (int k = 1; k >= 0; k--) {
      SpritePrep_ResetProperties(k * 2);
      uint16 x = kZeldaPrison_Soldier_X[k] + attract_vram_dst + 0x100;
      attract_var4 = x;
      Sprite_SimulateSoldier(k * 2,
                             x, attract_y_base + kZeldaPrison_Soldier_Y[k],
                             kZeldaPrison_Soldier_Dir[k], kZeldaPrison_Soldier_Flags[k], attract_var3);
    }

    if (!(++attract_var1 & 7)) {
      if (attract_var3 == 2) {
        attract_var3 = 0xff;
        if (!HIBYTE(attract_vram_dst) && attract_var1 & 8)
          sound_effect_2 = 4;
      }
      attract_var3++;
    }
  }

  switch (attract_var5) {
  case 0: Attract_ZeldaPrison_Case0(); break;
  case 1: Attract_ZeldaPrison_Case1(); break;
  }
}

/*
 * AttractDramatize_AgahnimAltar — Per-frame animation for the Agahnim
 * maiden-sealing ceremony scene.
 *
 * Manages the full crystal sealing sequence: maiden approach, crystal
 * formation expanding/collapsing, maiden vanish, and concluding text.
 * Six soldiers stand guard around the altar at fixed positions. The
 * maiden sprite walks downward from the top of the altar (attract_vram_dst
 * decrements from 0x70 to 0x60), then attract_var11 triggers the warp
 * animation. Handles screen flash effects and palette flash sounds
 * during the sealing. The attract_var22 flag signals completion, causing
 * the scene to advance and wrap back around to the polka-dot sequence.
 * Eight animation frames of Zelda (kZeldaPrison_MaidenWarp2, 6 tiles
 * each) cycle based on attract_var17, showing her shifting restlessly.
 */
void AttractDramatize_AgahnimAltar() {  // 8cf423
  if (attract_var22) {
    attract_sequence++;
    attract_state -= 2;
    return;
  }
  attract_oam_idx = 0;
  HandleScreenFlash();
  if (!attract_var18)
    Attract_FadeInStep();
  if (attract_var17 != 255)
    attract_var17++;
  if (intro_times_pal_flash & 4)
    sound_effect_2 = 0x2b;
  switch (attract_var5) {
  case 0: Attract_MaidenWarp_Case0(); break;
  case 1: Attract_MaidenWarp_Case1(); break;
  case 2: Attract_MaidenWarp_Case2(); break;
  case 3: Attract_MaidenWarp_Case3(); break;
  case 4: Attract_MaidenWarp_Case4(); break;
  }

  static const uint8 kMaidenWarp_Soldier_X[6] = { 48, 192, 48, 192, 80, 160 };
  static const uint8 kMaidenWarp_Soldier_Y[6] = { 112, 112, 152, 152, 192, 192 };
  static const uint8 kMaidenWarp_Soldier_Dir[6] = { 0, 1, 0, 1, 3, 3 };
  static const uint8 kMaidenWarp_Soldier_Flags[6] = { 9, 9, 9, 9, 7, 9 };
  for (int k = 5; k >= 0; k--) {

    SpritePrep_ResetProperties(k);
    Sprite_SimulateSoldier(k, kMaidenWarp_Soldier_X[k], kMaidenWarp_Soldier_Y[k],
                           kMaidenWarp_Soldier_Dir[k], kMaidenWarp_Soldier_Flags[k], 0);
  }

  if (attract_var17 >= 0xa0) {
    if (BYTE(attract_vram_dst) != 0x60) {
      if (!--attract_var1) {
        BYTE(attract_vram_dst)--;
        attract_var1 = 8;
      }
    } else {
      attract_var11++;
    }
  }

  if (attract_var15 == 0) {
    static const AttractOamInfo kZeldaPrison_MaidenWarp0[] = {
      { 0,  0, 0x03, 0x3d, 2},
      { 8,  0, 0x04, 0x3d, 2},
      { 0,  0, 0x00, 0x3d, 2},
      { 8,  0, 0x01, 0x3d, 2},
    };

    attract_x_base = 116;
    attract_y_base = attract_vram_dst;
    Attract_DrawSpriteSet2(kZeldaPrison_MaidenWarp0 + (BYTE(attract_vram_dst) == 0x70 ? 0 : 2), 2);
    static const uint8 kAttract_MaidenWarp_Xbase[8] = { 4, 4, 3, 3, 2, 2, 1, 0 };
    static const AttractOamInfo kZeldaPrison_MaidenWarp1[] = {
      { 0,  0, 0x6c, 0x38, 2},
      { 0,  0, 0x6c, 0x38, 2},
      { 0,  0, 0x6c, 0x38, 2},
      { 0,  0, 0x6c, 0x38, 2},
      { 0,  0, 0x6c, 0x38, 2},
      { 2,  0, 0x6c, 0x38, 2},
      { 0,  0, 0x6c, 0x38, 2},
      { 2,  0, 0x6c, 0x38, 2},
      { 0,  0, 0x6c, 0x38, 2},
      { 4,  0, 0x6c, 0x38, 2},
      { 0,  0, 0x6c, 0x38, 2},
      { 4,  0, 0x6c, 0x38, 2},
      { 0,  0, 0x6c, 0x38, 2},
      { 6,  0, 0x6c, 0x38, 2},
      { 0,  0, 0x6c, 0x38, 2},
      { 8,  0, 0x6c, 0x38, 2},
    };
    int k = 7;
    if (BYTE(attract_vram_dst) < 0x68)
      k = (BYTE(attract_vram_dst) - 0x68) & 7;
    attract_x_base = 0x74 + kAttract_MaidenWarp_Xbase[k];
    attract_y_base = 0x76;
    Attract_DrawSpriteSet2(kZeldaPrison_MaidenWarp1 + k * 2, 2);

  }
  static const AttractOamInfo kZeldaPrison_MaidenWarp2[] = {
    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x82, 0x3b, 2},
    {16,  0, 0x82, 0x7b, 2},
    { 0, 16, 0xa2, 0x3b, 2},
    {16, 16, 0xa2, 0x7b, 2},
    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x80, 0x3b, 2},
    {16,  0, 0x82, 0x7b, 2},
    { 0, 16, 0xa0, 0x3b, 2},
    {16, 16, 0xa2, 0x7b, 2},
    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x82, 0x3b, 2},
    {16,  0, 0x82, 0x7b, 2},
    { 0, 16, 0xa2, 0x3b, 2},
    {16, 16, 0xa2, 0x7b, 2},
    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x82, 0x3b, 2},
    {16,  0, 0x80, 0x7b, 2},
    { 0, 16, 0xa2, 0x3b, 2},
    {16, 16, 0xa0, 0x7b, 2},
    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x82, 0x3b, 2},
    {16,  0, 0x82, 0x7b, 2},
    { 0, 16, 0xa2, 0x3b, 2},
    {16, 16, 0xa2, 0x7b, 2},
    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x80, 0x3b, 2},
    {16,  0, 0x82, 0x7b, 2},
    { 0, 16, 0xa0, 0x3b, 2},
    {16, 16, 0xa2, 0x7b, 2},
    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x82, 0x3b, 2},
    {16,  0, 0x82, 0x7b, 2},
    { 0, 16, 0xa2, 0x3b, 2},
    {16, 16, 0xa2, 0x7b, 2},
    { 5, 25, 0x6c, 0x38, 2},
    {11, 25, 0x6c, 0x38, 2},
    { 0,  0, 0x80, 0x3b, 2},
    {16,  0, 0x80, 0x7b, 2},
    { 0, 16, 0xa0, 0x3b, 2},
    {16, 16, 0xa0, 0x7b, 2},
  };
  int k = attract_var17 >> 5 & 7;
  attract_x_base = 112;
  attract_y_base = 70;
  Attract_DrawSpriteSet2(kZeldaPrison_MaidenWarp2 + k * 6, 6);

}

/*
 * Attract_SkipToFileSelect — Handle the player pressing Start during
 * attract mode to skip directly to the file select screen.
 *
 * Fades the screen to black, then reconfigures BG tile map addresses,
 * sets up HDMA for the conclusion transition, resets all scroll and
 * Mode 7 registers, and calls FadeMusicAndResetSRAMMirror to transition
 * to the file select module.
 */
void Attract_SkipToFileSelect() {  // 8cf700
  if (--INIDISP_copy)
    return;
  EnableForceBlank();
  zelda_ppu_write(BG1SC, 0x13);
  zelda_ppu_write(BG2SC, 0x3);
  Attract_SetUpConclusionHDMA();
  M7Y_copy = 0;
  M7X_copy = 0;
  BG1HOFS_copy = 0;
  BG1VOFS_copy = 0;
  BG3VOFS_copy2 = 0;
  FadeMusicAndResetSRAMMirror();
}

/*
 * Attract_BuildNextImageTileMap — Load the next legend image tile map
 * into the VRAM upload buffer.
 *
 * Copies one of the four kAttract_Legendgraphics arrays into working
 * RAM at 0x1002, then flags nmi_load_bg_from_vram to trigger a DMA
 * transfer during the next NMI. The attract_next_legend_gfx index
 * selects which panel (0-3) to load, cycling through the backstory
 * illustrations.
 */
void Attract_BuildNextImageTileMap() {  // 8cf73e
  static const uint8 *const kAttract_LegendGraphics_pointers[4] = {
    kAttract_Legendgraphics_0,
    kAttract_Legendgraphics_1,
    kAttract_Legendgraphics_2,
    kAttract_Legendgraphics_3,
  };
  static const uint16 kAttract_LegendGraphics_sizes[4] = { 157+1, 237+1, 199+1, 265+1 };
  int i = attract_next_legend_gfx >> 1;
  memcpy(&g_ram[0x1002], kAttract_LegendGraphics_pointers[i], kAttract_LegendGraphics_sizes[i]);
  nmi_load_bg_from_vram = 1;
}

/*
 * Attract_ShowTimedTextMessage — Render the current text message overlay and
 * tick down the on-screen message timer.
 *
 * Saves BG2VOFS_copy2 into attract_var12 so the text renderer can anchor
 * the message at the correct scroll position. Clears joypad1L_last and
 * both filtered joypad bytes to prevent a stale button press from skipping
 * the message before it has been displayed. Calls RenderText() to render
 * the currently-queued dialogue line into the OAM buffer. If oam_priority_value
 * is non-zero (the message is in its fade-in phase), decrements it each frame
 * until the text reaches full opacity.
 */
void Attract_ShowTimedTextMessage() {  // 8cf766
  attract_var12 = BG2VOFS_copy2;
  BYTE(joypad1L_last) = 0;
  BYTE(filtered_joypad_L) = 0;
  BYTE(filtered_joypad_H) = 0;
  RenderText();
  if (oam_priority_value)
    oam_priority_value--;
}

/*
 * Attract_ControlMapZoom — Update the per-scanline HDMA zoom table for the
 * overworld map Mode 7 zoom effect used in the attract sequence.
 *
 * Iterates all 240 scanlines and scales the precomputed kMapMode_Zooms1 entry
 * for each line by the current timer_for_mode7_zoom value (0–255), using an
 * 8-bit fixed-point multiply (>> 8) to interpolate between no zoom and full
 * zoom. The result is written directly into hdma_table_dynamic[], which is
 * transferred to the SNES M7A/M7D registers by HDMA each frame, producing a
 * perspective-correct zoom animation without per-frame CPU cost per-pixel.
 */
void Attract_ControlMapZoom() {  // 8cf783
  for (int i = 240 - 1; i >= 0; i--)
    hdma_table_dynamic[i] = kMapMode_Zooms1[i] * timer_for_mode7_zoom >> 8;
}

/*
 * Attract_BuildBackgrounds — Configure PPU mode and populate the two VRAM
 * BG tile maps used by the attract-sequence legend/backstory images.
 *
 * Sets BGMODE = 9 (Mode 1 with BG3 priority), TM = 0x17 (BG1+BG2+BG3+OBJ
 * main screen), TS = 0 (no sub-screen). Remaps BG1SC to VRAM page 0x10 and
 * BG2SC to 0x00 to match the attract-mode tile layout.
 *
 * Fills the tile map in two passes, both writing into g_ram[0x1006] as a
 * staging buffer before calling Attract_TriggerBGDMA:
 *
 * Pass 1 (kAttract_CopyToVram_Tab0): Fills 128 words (0x80) by repeating
 *   groups of 4 tile entries in a stripe pattern. Each group of 4 consecutive
 *   k values shares the same 4-word entry from the table, cycling j through
 *   the group. The outer while exits when k reaches 0x80 and k & 0x1f == 0.
 *   This produces the 32-column wide BG1 tile stripe. DMA destination 0x1000.
 *
 * Pass 2 (kAttract_CopyToVram_Tab1): Fills 128 words with a 2-entry repeating
 *   pattern selected by whether k is in the low 32 or upper 32 range
 *   ((k & 0x20) >> 4 picks offset 0 or 2 into the 4-entry table). Produces
 *   the BG2 checkerboard palette tile map. DMA destination 0x0000.
 *
 * Resets attract_vram_dst = 0 after the second DMA so subsequent tile updates
 * start at the beginning of the VRAM tile map.
 */
void Attract_BuildBackgrounds() {  // 8cf7e6
  static const uint16 kAttract_CopyToVram_Tab0[16] = { 0x1a0, 0x9a6, 0x89a5, 0x1a0, 0x9a5, 0x1a0, 0x1a0, 0x89a6, 0x49a5, 0x1a0, 0x1a0, 0x49a5, 0x1a0, 0x89a5, 0xc9a5, 0x1a0 };
  static const uint16 kAttract_CopyToVram_Tab1[4] = { 0x9a1, 0x9a2, 0x9a3, 0x9a4 };

  BGMODE_copy = 9;
  TM_copy = 0x17;
  TS_copy = 0;

  zelda_ppu_write(BG1SC, 0x10);
  zelda_ppu_write(BG2SC, 0x0);

  {
    int k = 0;
    const uint16 *p = kAttract_CopyToVram_Tab0;
    uint16 *dst = (uint16 *)&g_ram[0x1006];
    do {
      int j = k & 3;
      do {
        dst[k++] = p[j++];
      } while (j & 3);
    } while (k & 0x1f || (p += 4, k != 0x80));
    Attract_TriggerBGDMA(0x1000);
  }

  {
    int k = 0;
    uint16 *dst = (uint16 *)&g_ram[0x1006];
    do {
      int j = k & 1;
      const uint16 *p = kAttract_CopyToVram_Tab1 + ((k & 0x20) >> 4);
      do {
        dst[k++] = p[j++];
      } while (j & 1);
    } while (k != 0x80);
    Attract_TriggerBGDMA(0);
  }
  attract_vram_dst = 0;
}

/*
 * Attract_TriggerBGDMA — Copy the attract-mode BG tile map staging buffer
 * into VRAM at the specified destination word address.
 *
 * dstv: VRAM word offset for the first BG tile map block (e.g., 0x1000 for
 *   BG1 or 0x0000 for BG2).
 *
 * Replicates the 256-byte staging buffer (g_ram[0x1006], accessed via the
 * vram pointer offset from g_zenv.vram) across 8 consecutive 0x80-word
 * (256-byte) blocks. This mirrors the hardware DMA that would broadcast the
 * same tile row pattern to all 8 row-groups of a 32×32 tile BG map, filling
 * the entire map with the repeated pattern in a single operation.
 * Each iteration advances dst by 0x80 words (128 words = 256 bytes).
 */
void Attract_TriggerBGDMA(uint16 dstv) {  // 8cf879
  uint16 *dst = &g_zenv.vram[dstv];
  for (int i = 0; i < 8; i++) {
    memcpy(dst, &g_ram[0x1006], 0x100);
    dst += 0x80;
  }
}

/*
 * Attract_DrawPreloadedSprite — Write a prebuilt multi-tile sprite into the
 * attract-mode OAM buffer (the second 64-entry block, oam_buf[64..127]).
 *
 * xp, yp: per-tile X and Y offset arrays relative to attract_x_base / attract_y_base.
 * cp:     per-tile character (CHR index) array.
 * fp:     per-tile flags byte array (palette, priority, flip bits).
 * ep:     per-tile extended-OAM byte array (size bit + high X bit).
 * n:      number of tiles minus 1 (inclusive upper bound; iterate n down to 0).
 *
 * Advances attract_oam_idx by n+1 so the next call starts at the next free slot.
 * Writes from slot (attract_oam_idx + 64) downward to (attract_oam_idx + 64 - n),
 * adding attract_x_base / attract_y_base as the sprite's screen anchor, so the
 * same precomputed relative-offset tables can be reused at any screen position.
 */
void Attract_DrawPreloadedSprite(const uint8 *xp, const uint8 *yp, const uint8 *cp, const uint8 *fp, const uint8 *ep, int n) {  // 8cf9b5
  OamEnt *oam = &oam_buf[attract_oam_idx + 64];
  attract_oam_idx += n + 1;
  do {
    SetOamPlain(oam, attract_x_base + xp[n], attract_y_base + yp[n], cp[n], fp[n], ep[n]);
  } while (oam++, --n >= 0);
}

/*
 * Attract_DrawZelda — Draw the two-tile Princess Zelda sprite in the attract
 * sequence intro cutscene.
 *
 * Places two 8×8 OAM entries starting at oam_buf[64 + attract_oam_idx]:
 *   Tile 0: CHR 0x28, flags 0x29 (palette 1, priority 2), at (0x60, attract_x_base).
 *   Tile 1: CHR 0x2A, flags 0x29, at (0x60, attract_x_base + 10) — right half.
 * Both tiles share the same screen Y (0x60) and OAM size flag 2 (16×16 px).
 * attract_x_base is repurposed here as the Y screen coordinate for Zelda's sprite
 * (the register is shared between X-base and Y-base across different attract phases).
 * Increments attract_oam_idx by 2.
 */
void Attract_DrawZelda() {  // 8cf9e8
  OamEnt *oam = &oam_buf[64 + attract_oam_idx];
  SetOamPlain(oam + 0, 0x60, attract_x_base, 0x28, 0x29, 2);
  SetOamPlain(oam + 1, 0x60, attract_x_base + 10, 0x2a, 0x29, 2);
  attract_oam_idx += 2;
}

/*
 * Sprite_SimulateSoldier — Place and animate a guard/soldier sprite as if it
 * were a live in-game sprite, used during the attract-sequence battle scene.
 *
 * k:     sprite slot index (0-15).
 * x, y:  world pixel coordinates to place the sprite.
 * dir:   facing direction (0=up, 1=down, 2=left, 3=right).
 * flags: OAM palette/priority flags. If flags == 9, the sprite is assigned
 *        type 0x41 (blue guard); otherwise type 0x43 (green guard).
 * gfx:   base graphics offset added to kSimulateSoldier_Gfx[dir] to select
 *        the correct walk-cycle frame for the given direction.
 *
 * Initialises the sprite's state to match what a freshly-spawned guard would
 * have: z = 0, direction, graphics frame, flags3 = 16 (standard guard flags),
 * objprio = 0, oam_flags = flags | 0x30 (OBJ size bits), flags2 = 7.
 * Points oam_cur_ptr and oam_ext_cur_ptr at the OAM entries for slot k
 * (main OAM at 0x800 + k*32 bytes; extended table at 0xA20 + k*8 bytes).
 * Calls Guard_HandleAllAnimation(k) to run one full animation tick, writing
 * the sprite's tiles into the OAM buffer without going through the normal
 * sprite dispatcher — allowing the attract mode to show combat animations
 * without allocating live sprite state.
 */
void Sprite_SimulateSoldier(int k, uint16 x, uint16 y, uint8 dir, uint8 flags, uint8 gfx) {  // 9deb84
  static const uint8 kSimulateSoldier_Gfx[4] = { 11, 4, 0, 7 };
  Sprite_SetX(k, x);
  Sprite_SetY(k, y);
  sprite_z[k] = 0;
  Sprite_Get16BitCoords(k);
  sprite_D[k] = sprite_head_dir[k] = dir;
  sprite_graphics[k] = kSimulateSoldier_Gfx[dir] + gfx;
  sprite_flags3[k] = 16;
  sprite_obj_prio[k] = 0;
  sprite_oam_flags[k] = flags | 0x30;
  sprite_type[k] = (flags == 9) ? 0x41 : 0x43;
  sprite_flags2[k] = 7;
  int oam_idx = k * 8;
  oam_cur_ptr = 0x800 + oam_idx * 4;
  oam_ext_cur_ptr = 0xa20 + oam_idx;
  Guard_HandleAllAnimation(k);
}

