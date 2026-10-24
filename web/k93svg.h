/* k93svg.h - clean-room SVG rasterizer + PNG encoder (Safari pipeline).
 *
 * Written from scratch against the PUBLISHED format specs (W3C SVG 1.1 /
 * PNG spec - format descriptions only; no renderer or encoder code was
 * consulted or copied). Everything here exists so the browser can convert
 * SVG to PNG on its own:
 *
 *   svg source -> k93svg_to_png() -> PNG bytes -> the browser's existing
 *   stb_image decoder -> one uniform image-cache path.
 *
 * SVG coverage: rect (incl. rx/ry corners), circle, ellipse, line,
 * polyline, polygon, path (M L H V C S Q T A Z + relative forms), groups,
 * presentation attributes + inline style=, transforms, fills (nonzero /
 * evenodd), strokes (width/caps, round joins), opacity, viewBox fitting.
 * Ceilings: gradients/url() fills, dash arrays, <text> glyphs, CSS <style>
 * blocks, miter joins (round-approximated) - all skipped/painted per the
 * fallbacks in k93svg.c.
 *
 * PNG encoder: own crc32/adler32 + a stored (uncompressed) deflate zlib
 * stream - zero dependencies, valid per spec, decoded by any PNG reader.
 * k93png_decode_stored() decodes exactly what k93png_encode() writes and
 * is used by selfcheck for a pixel-exact roundtrip; the browser proves
 * third-party interop through stb_image on every SVG image it loads. */
#ifndef K93SVG_H
#define K93SVG_H

#include <stddef.h>
#include <stdint.h>

/* SVG source -> RGBA8 raster (w*h*4, straight alpha, transparent bg). */
uint8_t *k93svg_to_rgba(const char *svg, size_t len, int *out_w, int *out_h);

/* SVG source -> PNG file bytes (k93png_encode on the raster above). */
uint8_t *k93svg_to_png(const char *svg, size_t len, size_t *out_len,
                       int *out_w, int *out_h);

/* RGBA8 (w*h*4) -> PNG file bytes (IHDR/IDAT/IEND, color type 6, 8 bit,
 * filter 0 rows, stored-deflate zlib stream). */
uint8_t *k93png_encode(const uint8_t *rgba, int w, int h, size_t *out_len);

/* Decode a PNG written by k93png_encode (stored deflate + filter 0 only).
 * Returns malloc'd RGBA8 or NULL. */
uint8_t *k93png_decode_stored(const uint8_t *png, size_t len,
                              int *out_w, int *out_h);

/* Standard CRC-32 (poly 0xEDB88320); selfcheck asserts the canonical
 * "123456789" -> 0xCBF43926 vector. */
uint32_t k93png_crc32(const void *data, size_t len);

#endif
