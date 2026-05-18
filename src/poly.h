/*
 * poly.h - 3D polyhedron renderer for the Triforce cutscene
 *
 * Implements a software 3D rendering pipeline used exclusively for the
 * spinning Triforce animation shown during the title screen and ending
 * sequence. The SNES had no 3D hardware, so the original game implemented
 * a complete polygon renderer in 65C816 assembly: vertex transformation,
 * perspective projection, backface culling via cross products, and
 * scanline-based polygon fill.
 *
 * This C reimplementation preserves the original rendering pipeline:
 *   1. Load shape data (vertex/face definitions) for the Triforce model
 *   2. Build a rotation matrix from the current animation angle
 *   3. Transform each vertex through rotation then perspective projection
 *   4. For each face, compute the cross product for backface culling
 *   5. Rasterize visible faces into a bitmap buffer using scanline fill
 *
 * The renderer operates entirely in fixed-point integer math (no floats),
 * matching the original SNES implementation's numeric precision.
 */
#pragma once
#include "types.h"

/* ---------------------------------------------------------------------------
 * Math utilities
 * --------------------------------------------------------------------------- */

// Fixed-point integer division of |a| by |b|, returning a 16-bit quotient.
// Used throughout the projection and rasterization stages.
uint16 Poly_Divide(uint16 a, uint16 b);

/* ---------------------------------------------------------------------------
 * Frame-level entry point
 * --------------------------------------------------------------------------- */

// Runs one frame of the Triforce animation: updates rotation angle,
// transforms all vertices, culls backfaces, and rasterizes visible faces
// into the output bitmap buffer.
void Poly_RunFrame();

/* ---------------------------------------------------------------------------
 * Shape and transformation setup
 * --------------------------------------------------------------------------- */

// Loads the pointer to the current polyhedron shape definition (vertex
// coordinates and face index lists) for the Triforce model.
void Polyhedral_SetShapePointer();

// Computes the 3x3 rotation matrix from the current animation angle.
// The matrix is stored in global state for use by OperateRotation/RotatePoint.
void Polyhedral_SetRotationMatrix();

// Applies the rotation matrix to all vertices in the shape definition,
// producing rotated 3D coordinates for subsequent projection.
void Polyhedral_OperateRotation();

// Transforms a single vertex through the rotation matrix, writing the
// rotated (x, y, z) coordinates back into the vertex work buffer.
void Polyhedral_RotatePoint();

// Projects a rotated 3D vertex onto the 2D screen plane using perspective
// division (dividing x and y by z + viewer distance).
void Polyhedral_ProjectPoint();

/* ---------------------------------------------------------------------------
 * Polyhedron drawing pipeline
 * --------------------------------------------------------------------------- */

// Top-level draw routine: iterates all faces of the polyhedron, performs
// backface culling, and rasterizes each visible face.
void Polyhedral_DrawPolyhedron();

// Sets the foreground color used for face rasterization based on the
// current face's orientation and the scene's light direction.
void Polyhedral_SetForegroundColor();

// Computes the 2D cross product of two edge vectors of the current face.
// A negative result means the face is back-facing and should be culled.
int16 Polyhedral_CalculateCrossProduct();

// Sets the color bitmask for pixel output, controlling which palette
// color |c| is used when filling the face's scanlines.
void Polyhedral_SetColorMask(int c);

// Clears the scanline bitmap buffer to transparent before rendering a
// new frame, ensuring no residual pixels from the previous frame remain.
void Polyhedral_EmptyBitMapBuffer();

// Rasterizes a single face by walking its edges and filling horizontal
// scanlines between the left and right edge boundaries.
void Polyhedral_DrawFace();

// Fills a single horizontal scanline between the current left and right
// edge positions with the active foreground color.
void Polyhedral_FillLine();

// Advances the left edge of the current scanline by one step along the
// polygon edge. Returns true if the edge has been fully traversed.
bool Polyhedral_SetLeft();

// Advances the right edge of the current scanline by one step along the
// polygon edge. Returns true if the edge has been fully traversed.
bool Polyhedral_SetRight();
