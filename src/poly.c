/*
 * poly.c - 3D Polyhedron Renderer for the Triforce Animation
 *
 * Implements a software 3D rendering pipeline used during the intro sequence and
 * credits to display the spinning Triforce. The pipeline performs:
 *   1. Rotation via a precomputed sin/cos lookup table and a 3x3 rotation matrix
 *   2. Perspective projection (divide x,y by z depth)
 *   3. Backface culling via cross-product winding order test
 *   4. Scanline rasterization with edge-walking into a bitmap buffer
 *
 * All arithmetic uses fixed-point integers to match the original SNES 65C816
 * implementation, which had no floating-point hardware. The renderer supports
 * two polyhedron models: an octahedron (diamond shape, model 0) and a
 * triangular prism (the individual Triforce piece, model 1).
 *
 * The output is written into polyhedral_buffer[], a planar bitmap that is
 * later DMA'd to VRAM by the NMI handler.
 *
 * Related files:
 *   poly.h      - Public declarations for the polyhedron renderer
 *   variables.h - WRAM-mapped global state (poly_* variables)
 *   zelda_rtl.h - Runtime library bridging SNES emulation and C
 */

/* Engine includes */
#include "poly.h"
#include "zelda_rtl.h"
#include "variables.h"

/*
 * Precomputed sine/cosine lookup table in Q6 fixed-point format (range -64..+64).
 *
 * The table stores 256 entries of sine followed by a partial repeat to allow
 * cosine lookups via a +64 offset (cos(x) = sin(x + 90 degrees)). With 256
 * entries per full period, each index step equals 360/256 = 1.40625 degrees.
 *
 * Entry i = round(64 * sin(i * 2 * pi / 256)).
 *
 * The extra 64 entries (indices 256..319) duplicate indices 0..63 so that
 * cosine lookups (index + 64) never exceed the array bounds even for the
 * largest sine index of 255.
 */
static const int8 kPolySinCos[320] = {
  0,   2,   3,   5,   6,   8,   9,  11,  12,  14,  16,  17,  19,  20,  22,  23,
  24,  26,  27,  29,  30,  32,  33,  34,  36,  37,  38,  39,  41,  42,  43,  44,
  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  56,  57,  58,  59,
  59,  60,  60,  61,  61,  62,  62,  62,  63,  63,  63,  64,  64,  64,  64,  64,
  64,  64,  64,  64,  64,  64,  63,  63,  63,  62,  62,  62,  61,  61,  60,  60,
  59,  59,  58,  57,  56,  56,  55,  54,  53,  52,  51,  50,  49,  48,  47,  46,
  45,  44,  43,  42,  41,  39,  38,  37,  36,  34,  33,  32,  30,  29,  27,  26,
  24,  23,  22,  20,  19,  17,  16,  14,  12,  11,   9,   8,   6,   5,   3,   2,
  0,  -2,  -3,  -5,  -6,  -8,  -9, -11, -12, -14, -16, -17, -19, -20, -22, -23,
  -24, -26, -27, -29, -30, -32, -33, -34, -36, -37, -38, -39, -41, -42, -43, -44,
  -45, -46, -47, -48, -49, -50, -51, -52, -53, -54, -55, -56, -56, -57, -58, -59,
  -59, -60, -60, -61, -61, -62, -62, -62, -63, -63, -63, -64, -64, -64, -64, -64,
  -64, -64, -64, -64, -64, -64, -63, -63, -63, -62, -62, -62, -61, -61, -60, -60,
  -59, -59, -58, -57, -56, -56, -55, -54, -53, -52, -51, -50, -49, -48, -47, -46,
  -45, -44, -43, -42, -41, -39, -38, -37, -36, -34, -33, -32, -30, -29, -27, -26,
  -24, -23, -22, -20, -19, -17, -16, -14, -12, -11,  -9,  -8,  -6,  -5,  -3,  -2,
  0,   2,   3,   5,   6,   8,   9,  11,  12,  14,  16,  17,  19,  20,  22,  23,
  24,  26,  27,  29,  30,  32,  33,  34,  36,  37,  38,  39,  41,  42,  43,  44,
  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  56,  57,  58,  59,
  59,  60,  60,  61,  61,  62,  62,  62,  63,  63,  63,  64,  64,  64,  64,  64,
};

/*
 * 3D point with int8 components. The compact byte-per-axis layout matches
 * the original SNES code, which stored each model's vertices as a flat
 * stream of signed bytes in ROM. Coordinates are model-local: rotation
 * and translation happen later in the pipeline.
 */
typedef struct Vertex3 {
  int8 x, y, z;
} Vertex3;

/*
 * Model 0 vertices: an octahedron (a "diamond" — two square pyramids joined
 * base-to-base). Indices 0 and 1 are the top/bottom apexes on the Y axis at
 * +/-65; indices 2..5 are the four equatorial corners arranged at distance
 * 40 on the X/Z plane. This model is used as the rotating diamond shape
 * during the Triforce intro animation.
 */
static const Vertex3 kPoly0_Vtx[6] = {
  {  0,  65,   0},
  {  0, -65,   0},
  {  0,   0, -40},
  {-40,   0,   0},
  {  0,   0,  40},
  { 40,   0,   0},
};

/*
 * Model 0 face list: 8 triangular faces, one per pyramid side. Each face
 * record is laid out as
 *   <vertex_count = 3>, <v0>, <v1>, <v2>, <color_index>
 * The trailing color_index is read by Polyhedral_SetForegroundColor and
 * shifted/clamped into a lookup into kPoly_RasterColors so each face gets
 * a distinct shaded color suggesting lighting on a rotating diamond.
 */
static const uint8 kPoly0_Polys[40] = {
  3, 0, 5, 2, 4,
  3, 0, 2, 3, 1,
  3, 0, 3, 4, 2,
  3, 0, 4, 5, 3,
  3, 1, 2, 5, 4,
  3, 1, 3, 2, 1,
  3, 1, 4, 3, 2,
  3, 1, 5, 4, 3,
};

/*
 * Model 1 vertices: a triangular prism — the shape of a single Triforce
 * piece. Indices 0..2 form the front triangular face at z = +10 and
 * indices 3..5 form the back triangular face at z = -10, giving the
 * solid 20 units of depth. The triangle is roughly 80 wide x 80 tall.
 */
static const Vertex3 kPoly1_Vtx[6] = {
  {  0,  40,  10},
  { 40, -40,  10},
  {-40, -40,  10},
  {  0,  40, -10},
  {-40, -40, -10},
  { 40, -40, -10},
};

/*
 * Model 1 face list: 2 triangular end caps and 3 quadrilateral side faces.
 * The leading byte gives the vertex count per face (3 or 4), and each face
 * record is
 *   <vertex_count>, <v0>, <v1>, <v2>[, <v3>], <color_index>
 * so quads carry one extra vertex index before the color. The generic
 * n-gon loop in Polyhedral_DrawPolyhedron consumes either shape uniformly.
 */
static const uint8 kPoly1_Polys[28] = {
  3, 0, 1, 2, 7,
  3, 3, 4, 5, 6,
  4, 0, 3, 5, 1, 5,
  4, 1, 5, 4, 2, 4,
  4, 3, 0, 2, 4, 3,
};

/*
 * Per-model render configuration. num_vtx / num_poly drive the vertex
 * transform loop in Polyhedral_OperateRotation and the face draw loop in
 * Polyhedral_DrawPolyhedron. vtx_val / polys_val are the original SNES
 * ROM addresses of the matching tables; they are written into the
 * poly_fromlut_ptr2 / poly_fromlut_ptr4 WRAM mirror slots so save-states
 * round-trip byte-identical with the original assembly's state. The
 * vertex / poly fields are the C-side data sources actually read by
 * the renderer.
 */
typedef struct PolyConfig {
  uint8 num_vtx, num_poly;
  uint16 vtx_val, polys_val;
  const Vertex3 *vertex;
  const uint8 *poly;
} PolyConfig;

/*
 * Two-model registry. Index 0 is the octahedron (intro diamond), index 1 is
 * the triangular prism (single Triforce piece). poly_which_model selects
 * between them via Polyhedral_SetShapePointer.
 */
static const PolyConfig kPolyConfigs[2] = {
  {6, 8, 0xff98, 0xffaa, kPoly0_Vtx, kPoly0_Polys},
  {6, 5, 0xffd2, 0xffe4, kPoly1_Vtx, kPoly1_Polys},
};

/*
 * Foreground color palette for face fill. Each entry packs two 16-bit
 * planar bit patterns side-by-side:
 *   low 16 bits  -> poly_raster_color0 (XORed into bitplanes 0 and 1)
 *   high 16 bits -> poly_raster_color1 (XORed into bitplanes 2 and 3)
 * Together they reproduce a 4bpp tile color when written into the 2KB
 * polyhedral_buffer. Polyhedral_SetForegroundColor picks the index in
 * the [1..7] range (index 0 is never written because it would leave the
 * face transparent).
 */
static const uint32 kPoly_RasterColors[16] = {
  0x00, 0xff, 0xff00, 0xffff,
  0xff0000, 0xff00ff, 0xffff00, 0xffffff,
  0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff,
  0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};

/*
 * Per-end pixel masks for scanline fill. A polygon's left and right edges
 * fall at sub-tile pixel positions; these tables pick which of the 8 pixels
 * inside an 8x8 tile column are actually covered by the polygon. Indexed
 * by the low 3 bits of the fractional x coordinate. The byte is duplicated
 * in both halves of each uint16 because every tile is 8 pixels wide on each
 * of two adjacent bitplane pairs.
 */
static const uint16 kPoly_LeftSideMask[8] = {0xffff, 0x7f7f, 0x3f3f, 0x1f1f, 0xf0f, 0x707, 0x303, 0x101};
static const uint16 kPoly_RightSideMask[8] = {0x8080, 0xc0c0, 0xe0e0, 0xf0f0, 0xf8f8, 0xfcfc, 0xfefe, 0xffff};

/*
 * Signed fixed-point divide that mimics the original SNES routine's
 * normalization. To keep the dividend within the SNES math unit's range,
 * b (and a in lock-step) is scaled down by powers of two until b fits in
 * 8 bits, after which the divide proceeds. The result preserves the sign
 * of a. Used by the perspective projection where dividing x or y by depth
 * z could otherwise overflow. poly_tmp0 / poly_tmp1 are kept in sync with
 * the SNES code's scratch slots so save-states match exactly.
 */
uint16 Poly_Divide(uint16 a, uint16 b) {
  poly_tmp1 = sign16(a) ? -a : a;
  poly_tmp0 = b;
  while (poly_tmp0 >= 256)
    poly_tmp0 >>= 1, poly_tmp1 >>= 1;
  int q = poly_tmp1 / poly_tmp0;
  return sign16(a) ? -q : q;
}

/*
 * Per-frame entry point. Runs the full render pipeline once:
 *   1. Clear the offscreen bitmap.
 *   2. Bind the active model's tables (octahedron or prism).
 *   3. Rebuild the rotation matrix from the current poly_a / poly_b angles.
 *   4. Rotate and project every vertex into 2D screen space.
 *   5. Walk the face list, backface-cull, and rasterize visible faces.
 * The resulting bitmap is later DMA'd to VRAM by the NMI handler so the
 * spinning Triforce appears on screen during the intro / credits.
 */
void Poly_RunFrame() {
  Polyhedral_EmptyBitMapBuffer();
  Polyhedral_SetShapePointer();
  Polyhedral_SetRotationMatrix();
  Polyhedral_OperateRotation();
  Polyhedral_DrawPolyhedron();
}

/*
 * Chooses which model to render this frame and seeds the working state
 * with the matching vertex / face table references. poly_var1 derives a
 * perspective offset from poly_config1 (used later as the additive z
 * bias in Polyhedral_RotatePoint). The poly_fromlut_ptr2 / ptr4 fields
 * are written with the original SNES ROM addresses so that any code
 * inspecting the WRAM mirror sees the same values the assembly would.
 */
void Polyhedral_SetShapePointer() {  // 89f83d
  poly_var1 = poly_config1 * 2 + 0x80;
  poly_tmp0 = poly_which_model * 2;

  const PolyConfig *poly_config = &kPolyConfigs[poly_which_model];
  poly_config_num_vertex = poly_config->num_vtx;
  poly_config_num_polys = poly_config->num_poly;
  poly_fromlut_ptr2 = poly_config->vtx_val;
  poly_fromlut_ptr4 = poly_config->polys_val;
}

/*
 * Builds the rotation matrix from two angles, poly_a and poly_b (each is
 * a 0..255 index into kPolySinCos that selects a Q6 sine, with +64 picking
 * the cosine). poly_e0..e3 cache the four cross-products of sin/cos pairs
 * so Polyhedral_RotatePoint can apply the matrix per-vertex without
 * recomputing them. The `>> 8 << 2` collapses the Q6 * Q6 = Q12 product
 * back down to Q10, matching the precision the original SNES code carries.
 */
void Polyhedral_SetRotationMatrix() {  // 89f864
  poly_sin_a = kPolySinCos[poly_a];
  poly_cos_a = kPolySinCos[poly_a + 64];
  poly_sin_b = kPolySinCos[poly_b];
  poly_cos_b = kPolySinCos[poly_b + 64];
  poly_e0 = (int16)poly_sin_b * (int8)poly_sin_a >> 8 << 2;
  poly_e1 = (int16)poly_cos_b * (int8)poly_cos_a >> 8 << 2;
  poly_e2 = (int16)poly_cos_b * (int8)poly_sin_a >> 8 << 2;
  poly_e3 = (int16)poly_sin_b * (int8)poly_cos_a >> 8 << 2;
}

/*
 * Walks every vertex of the active model in reverse order (last to first,
 * matching the original assembly's countdown loop), rotates each into
 * camera space, perspective-projects it to screen space, and stores the
 * result in poly_arr_x[] / poly_arr_y[]. poly_base_x / poly_base_y shift
 * the result to the desired on-screen center. The Y output is subtracted
 * rather than added because the bitmap's Y axis grows downward.
 */
void Polyhedral_OperateRotation() {  // 89f8fb
  const PolyConfig *poly_config = &kPolyConfigs[poly_which_model];
  const int8 *src = &poly_config->vertex[0].x;
  int i = poly_config_num_vertex;
  src += i * 3;
  do {
    src -= 3, i -= 1;
    poly_fromlut_x = src[2];
    poly_fromlut_y = src[1];
    poly_fromlut_z = src[0];
    Polyhedral_RotatePoint();
    Polyhedral_ProjectPoint();
    poly_arr_x[i] = poly_base_x + poly_f0;
    poly_arr_y[i] = poly_base_y - poly_f1;
  } while (i);
}

/*
 * Applies the precomputed rotation matrix to a single (x, y, z) vertex
 * loaded from poly_fromlut_x / y / z. The outputs are
 *   poly_f0 = rotated x in screen space (pre-perspective)
 *   poly_f1 = rotated y
 *   poly_f2 = rotated z + poly_var1 (the constant perspective bias that
 *             pushes the model away from the camera so the divide in
 *             Polyhedral_ProjectPoint never sees z = 0)
 * The matrix is laid out so the rotation effectively orbits the model
 * around two axes governed by poly_a and poly_b.
 */
void Polyhedral_RotatePoint() {  // 89f931
  int x = (int8)poly_fromlut_x;
  int y = (int8)poly_fromlut_y;
  int z = (int8)poly_fromlut_z;

  poly_f0 =   (int16)poly_cos_b * z                         - (int16)poly_sin_b * x;
  poly_f1 =   (int16)poly_e0    * z + (int16)poly_cos_a * y + (int16)poly_e2    * x;
  poly_f2 = ((int16)poly_e3 * z >> 8) - ((int16)poly_sin_a * y >> 8) + ((int16)poly_e1 * x >> 8) + poly_var1;
}

/*
 * Perspective projection: screen_x = rotated_x / depth, screen_y = rotated_y
 * / depth. Vertices further from the camera (larger f2) collapse toward
 * the screen center. Poly_Divide does the overflow-safe fixed-point divide.
 */
void Polyhedral_ProjectPoint() {  // 89f9d6
  poly_f0 = Poly_Divide(poly_f0, poly_f2);
  poly_f1 = Poly_Divide(poly_f1, poly_f2);
}

/*
 * Walks the active model's face list and rasterizes each visible face.
 * For each face:
 *   1. Read the leading vertex count and copy the projected (x, y) pairs
 *      from poly_arr_x / poly_arr_y into the scratch buffer poly_xy_coords.
 *      poly_xy_coords[0] holds 2 * vertex_count so later code can index by
 *      stride and use it as a wrap-around length.
 *   2. Read the trailing color_index into poly_raster_color_config.
 *   3. Run a 2D cross-product test on the first two edges
 *      (Polyhedral_CalculateCrossProduct). A positive result means the
 *      face winds counter-clockwise in screen space and is front-facing.
 *   4. If front-facing, pick a shaded color and draw the polygon.
 * Faces whose cross product is non-positive are simply skipped (backface
 * culled).
 */
void Polyhedral_DrawPolyhedron() {  // 89fa4f
  const PolyConfig *poly_config = &kPolyConfigs[poly_which_model];
  const uint8 *src = poly_config->poly;
  do {
    poly_num_vertex_in_poly = *src++;
    BYTE(poly_tmp0) = poly_num_vertex_in_poly;
    poly_xy_coords[0] = poly_num_vertex_in_poly * 2;

    int i = 1;
    do {
      int j = *src++;
      poly_xy_coords[i + 0] = poly_arr_x[j];
      poly_xy_coords[i + 1] = poly_arr_y[j];
      i += 2;
    } while (--BYTE(poly_tmp0));

    poly_raster_color_config = *src++;
    int order = Polyhedral_CalculateCrossProduct();
    if (order > 0) {
      Polyhedral_SetForegroundColor();
      Polyhedral_DrawFace();
    }
  } while (--poly_config_num_polys);
}

/*
 * Picks the shaded fill color for the current face and installs it. The
 * face's raw color_index (poly_tmp0, set during Polyhedral_DrawPolyhedron)
 * is scaled by a brightness shift derived from the model: 1 for the
 * octahedron, and a poly_config1-dependent value for the prism so the
 * Triforce pieces brighten/darken across the convergence animation. The
 * result is clamped to [1..7] to avoid index 0 (transparent) and indices
 * outside the kPoly_RasterColors palette.
 */
void Polyhedral_SetForegroundColor() {  // 89faca
  uint8 t = poly_which_model ? (poly_config1 >> 5) : 0;
  uint8 a = (poly_tmp0 << (t + 1)) >> 8;
  Polyhedral_SetColorMask(a <= 1 ? 1 : a >= 7 ? 7 : a);
}

/*
 * Computes the 2D cross product of the first two edges of the face's
 * projected polygon: (v1 - v0) x (v2 - v1). A positive result indicates
 * counter-clockwise winding in screen space, which under this renderer's
 * convention means the face is visible (front-facing). A non-positive
 * result means the face is hidden behind the rest of the model and must
 * be culled. Operating on screen-space (x, y) pairs avoids needing the
 * pre-projection z coordinate for the visibility test.
 */
int16 Polyhedral_CalculateCrossProduct() {  // 89fb24
  int16 a = poly_xy_coords[3] - poly_xy_coords[1];
  poly_tmp0 = a * (int8)(poly_xy_coords[6] - poly_xy_coords[4]);
  a = poly_xy_coords[5] - poly_xy_coords[3];
  poly_tmp0 -= a * (int8)(poly_xy_coords[4] - poly_xy_coords[2]);
  return poly_tmp0;
}

/*
 * Stashes the chosen palette color as a planar bit pair so the scanline
 * fill code can XOR it into the bitmap. poly_raster_color0 holds the low
 * 16 bits of the entry (bitplanes 0/1) and poly_raster_color1 holds the
 * high 16 bits (bitplanes 2/3); together they reproduce the 4bpp color.
 */
void Polyhedral_SetColorMask(int c) {  // 89fcae
  uint32 v = kPoly_RasterColors[c];
  poly_raster_color0 = v;
  poly_raster_color1 = v >> 16;
}

/*
 * Wipes the 2KB scratch bitmap that the renderer draws into. Run at the
 * start of every frame so leftover pixels from the previous frame's
 * polygon don't bleed through. The buffer is later DMA'd to VRAM by the
 * NMI handler to put the rendered shape on screen.
 */
void Polyhedral_EmptyBitMapBuffer() {  // 89fd04
  memset(polyhedral_buffer, 0, 0x800);
}

/*
 * Rasterizes one polygon using a standard top-to-bottom edge-walking fill.
 * The polygon is split into a left and a right edge, each starting at the
 * vertex with the smallest y (the topmost in screen space). Each scanline
 * is filled between the two edges via Polyhedral_FillLine, then the y
 * cursor steps down. When an edge runs out of y range, the next edge
 * segment is set up by Polyhedral_SetLeft / Polyhedral_SetRight. The
 * routine exits when SetLeft / SetRight signal that no further segments
 * remain.
 *
 * poly_raster_dst_ptr is initialized using the SNES VRAM tile-row
 * addressing formula:
 *   ((min_y & 0x38) ^ ((min_y & 0x20) ? 0x24 : 0)) << 6
 * picks the 8-pixel tile row in the bitmap, and (min_y & 7) * 2 selects
 * the pixel row within that tile. The XOR with 0x24 implements the
 * SNES's non-linear two-page tile layout where the second page is at a
 * different offset than a flat (row * stride) calculation would give.
 *
 * The loop body advances poly_raster_dst_ptr by 2 bytes for each
 * scanline (the next pixel row inside the same tile), and rolls over
 * to the next tile row when the low byte hits 0x0e (= 7 pixel rows + 2).
 * The carry uses another XOR (with 0x19) to keep the addressing mapped
 * to SNES VRAM layout.
 */
void Polyhedral_DrawFace() {  // 89fd1e
  int n = poly_xy_coords[0];
  uint8 min_y = poly_xy_coords[n];
  int min_idx = n;
  while (n -= 2) {
    if (poly_xy_coords[n] < min_y)
      min_y = poly_xy_coords[n], min_idx = n;
  }
  poly_raster_dst_ptr = 0xe800 + (((min_y & 0x38) ^ (min_y & 0x20 ? 0x24 : 0)) << 6) + (min_y & 7) * 2;
  poly_cur_vertex_idx0 = poly_cur_vertex_idx1 = min_idx;
  poly_total_num_steps = poly_xy_coords[0] >> 1;
  poly_y0_cur = poly_y1_cur = poly_xy_coords[min_idx];
  poly_x0_cur = poly_x1_cur = poly_xy_coords[min_idx - 1];
  if (Polyhedral_SetLeft() || Polyhedral_SetRight())
    return;
  for (;;) {
    Polyhedral_FillLine();
    if (BYTE(poly_raster_dst_ptr) != 0xe) {
      poly_raster_dst_ptr += 2;
    } else {
      uint8 a = HIBYTE(poly_raster_dst_ptr) + 2;
      poly_raster_dst_ptr = (a ^ ((a & 8) ? 0 : 0x19)) << 8;
    }
    if (poly_y0_cur == poly_y0_trig) {
      poly_x0_cur = poly_x0_target;
      if (Polyhedral_SetLeft())
        return;
    }
    poly_y0_cur++;
    if (poly_y1_cur == poly_y1_trig) {
      poly_x1_cur = poly_x1_target;
      if (Polyhedral_SetRight())
        return;
    }
    poly_y1_cur++;
    poly_x0_frac += poly_x0_step;
    poly_x1_frac += poly_x1_step;
  }
}

/*
 * Fills one scanline of the polygon between the current left and right
 * edge x positions. The scanline is broken into 8-pixel tile columns and
 * three cases are handled:
 *   - Both edges fall in the same tile column: AND the two end masks
 *     together so only the covered pixels in that single tile are touched.
 *   - Edges span two or more tile columns: the right tile is masked with
 *     kPoly_RightSideMask, the middle tiles get a full XOR (no mask), and
 *     the left tile is masked with kPoly_LeftSideMask.
 *   - The right edge is to the left of the left edge (negative span):
 *     nothing is drawn — the polygon has no coverage on this scanline.
 *
 * The pointer arithmetic walks the bitmap right-to-left (ptr -= 0x10) per
 * tile column because the SNES tile layout has consecutive tile columns
 * 0x20 bytes apart, with bitplane pairs 16 bytes inside that block; ptr[0]
 * is bitplanes 0/1 and ptr[8] is bitplanes 2/3 for the same tile row.
 * Using XOR (`^=`) instead of OR lets the same code both set pixels (when
 * the buffer was cleared at frame start) and toggle them for any overlap
 * effects the original code might have relied on.
 */
void Polyhedral_FillLine() {  // 89fdcf
  uint16 left = kPoly_LeftSideMask[(poly_x0_frac >> 8) & 7];
  uint16 right = kPoly_RightSideMask[(poly_x1_frac >> 8) & 7];
  poly_tmp2 = (poly_x0_frac >> 8) & 0x38;
  int d0 = ((poly_x1_frac >> 8) & 0x38);
  uint16 *ptr = (uint16*)&g_ram[poly_raster_dst_ptr + d0 * 4];
  if ((d0 -= poly_tmp2) == 0) {
    poly_tmp1 = left & right;
    ptr[0] ^= (ptr[0] ^ poly_raster_color0) & poly_tmp1;
    ptr[8] ^= (ptr[8] ^ poly_raster_color1) & poly_tmp1;
    return;
  }
  if (d0 < 0)
    return;
  int n = d0 >> 3;
  ptr[0] ^= (ptr[0] ^ poly_raster_color0) & right;
  ptr[8] ^= (ptr[8] ^ poly_raster_color1) & right;
  ptr -= 0x10;
  while (--n) {
    ptr[0] = poly_raster_color0;
    ptr[8] = poly_raster_color1;
    ptr -= 0x10;
  }
  ptr[0] ^= (ptr[0] ^ poly_raster_color0) & left;
  ptr[8] ^= (ptr[8] ^ poly_raster_color1) & left;
  poly_tmp1 = left, poly_raster_numfull = 0;
}

/*
 * Sets up the slope for the next segment of the left edge. Walks the
 * vertex list backwards from poly_cur_vertex_idx0 to find the next vertex
 * whose y is strictly greater than the current scanline; that vertex
 * becomes the new edge target.
 *
 * Returns true when the polygon is fully drawn — either the step counter
 * underflowed (every scanline has been emitted) or the next vertex's y
 * is above the current scanline (which means the edge wrapped past the
 * polygon's bottom).
 *
 * poly_x0_frac is the fractional x position in 8.8 fixed point; the
 * `| 0x80` adds 0.5 of bias so each step lands at the pixel center.
 * poly_x0_step is the dx/dy slope, computed by dividing dx by dy in
 * 8.8 fixed point and re-applying the sign of dx.
 */
bool Polyhedral_SetLeft() {  // 89feb4
  int i;
  for (;;) {
    if (sign8(--poly_total_num_steps))
      return true;
    i = poly_cur_vertex_idx0 - 2;
    if (i == 0)
      i = poly_xy_coords[0];
    if (poly_xy_coords[i] < poly_y0_cur)
      return true;
    if (poly_xy_coords[i] != poly_y0_cur)
      break;
    poly_x0_cur = poly_xy_coords[i - 1];
    poly_cur_vertex_idx0 = i;
  }
  poly_y0_trig = poly_xy_coords[i];
  poly_x0_target = poly_xy_coords[i - 1];
  poly_cur_vertex_idx0 = i;
  int t = poly_x0_target - poly_x0_cur, u = t;
  if (t < 0)
    t = -t;
  t = ((t & 0xff) << 8) / (uint8)(poly_y0_trig - poly_y0_cur);
  poly_x0_frac = (poly_x0_cur << 8) | 0x80;
  poly_x0_step = (u < 0) ? -t : t;
  return false;
}

/*
 * Mirror of Polyhedral_SetLeft for the right-side edge. Walks the vertex
 * list forward from poly_cur_vertex_idx1 to find the next vertex whose y
 * is strictly greater, computes the slope to it in 8.8 fixed point, and
 * stores it in poly_x1_step. poly_y1_trig marks the y at which the
 * caller (Polyhedral_DrawFace) must swap to the next edge segment.
 * Returns true when the polygon's right edge has been fully traversed.
 */
bool Polyhedral_SetRight() {  // 89ff1e
  int i;
  for (;;) {
    if (sign8(--poly_total_num_steps))
      return true;
    i = poly_cur_vertex_idx1;
    if (i == poly_xy_coords[0])
      i = 0;
    i += 2;
    if (poly_xy_coords[i] < poly_y1_cur)
      return true;
    if (poly_xy_coords[i] != poly_y1_cur)
      break;
    poly_x1_cur = poly_xy_coords[i - 1];
    poly_cur_vertex_idx1 = i;
  }
  poly_y1_trig = poly_xy_coords[i];
  poly_x1_target = poly_xy_coords[i - 1];
  poly_cur_vertex_idx1 = i;
  int t = poly_x1_target - poly_x1_cur, u = t;
  if (t < 0)
    t = -t;
  t = ((t & 0xff) << 8) / (uint8)(poly_y1_trig - poly_y1_cur);
  poly_x1_frac = (poly_x1_cur << 8) | 0x80;
  poly_x1_step = (u < 0) ? -t : t;
  return false;
}

