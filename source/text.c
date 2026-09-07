/* Dynamic text rendering with FreeType over the console's shared fonts. */

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "error.h"
#include "text.h"

/* The engine measures against a 26 px "Sans" face; matching that keeps
 * layout consistent with the packaged atlases. */
#define BASE_FONT_SIZE 26.0f

#define MAX_FACES 6

static FT_Library g_library;
static FT_Face g_faces[MAX_FACES];
static int g_face_count;
static bool g_ready;
static Mutex g_lock;

static void load_shared_font(PlSharedFontType type) {
  if (g_face_count >= MAX_FACES) return;
  PlFontData data;
  if (R_FAILED(plGetSharedFontByType(&data, type))) return;
  FT_Face face;
  if (FT_New_Memory_Face(g_library, data.address, (FT_Long)data.size, 0, &face) != 0)
    return;
  g_faces[g_face_count++] = face;
}

bool text_init(void) {
  mutexInit(&g_lock);
  if (R_FAILED(plInitialize(PlServiceType_User))) {
    trace("pl:u unavailable; dynamic text will be blank");
    return false;
  }
  if (FT_Init_FreeType(&g_library) != 0) {
    trace("FreeType init failed; dynamic text will be blank");
    return false;
  }

  /* Standard first so Latin and kana resolve there; the others cover the
   * scripts it omits. */
  load_shared_font(PlSharedFontType_Standard);
  load_shared_font(PlSharedFontType_ChineseSimplified);
  load_shared_font(PlSharedFontType_ExtChineseSimplified);
  load_shared_font(PlSharedFontType_ChineseTraditional);
  load_shared_font(PlSharedFontType_KO);
  load_shared_font(PlSharedFontType_NintendoExt);

  g_ready = g_face_count > 0;
  trace("text: %d shared font faces loaded", g_face_count);
  return g_ready;
}

/* Decode one UTF-8 code point, advancing the cursor. */
static uint32_t next_codepoint(const char **cursor) {
  const unsigned char *p = (const unsigned char *)*cursor;
  uint32_t code;
  if (*p < 0x80u) {
    code = *p++;
  } else if ((*p & 0xE0u) == 0xC0u) {
    code = (uint32_t)(*p++ & 0x1Fu) << 6;
    if (*p) code |= *p++ & 0x3Fu;
  } else if ((*p & 0xF0u) == 0xE0u) {
    code = (uint32_t)(*p++ & 0x0Fu) << 12;
    if (*p) code |= (uint32_t)(*p++ & 0x3Fu) << 6;
    if (*p) code |= *p++ & 0x3Fu;
  } else {
    code = (uint32_t)(*p++ & 0x07u) << 18;
    if (*p) code |= (uint32_t)(*p++ & 0x3Fu) << 12;
    if (*p) code |= (uint32_t)(*p++ & 0x3Fu) << 6;
    if (*p) code |= *p++ & 0x3Fu;
  }
  *cursor = (const char *)p;
  return code;
}

/* Pick the first loaded face that actually has a glyph for this code point. */
static FT_Face face_for(uint32_t code, FT_UInt *glyph_index) {
  for (int i = 0; i < g_face_count; i++) {
    const FT_UInt index = FT_Get_Char_Index(g_faces[i], code);
    if (index) {
      *glyph_index = index;
      return g_faces[i];
    }
  }
  *glyph_index = 0;
  return g_face_count ? g_faces[0] : NULL;
}

static void set_size(FT_Face face, float pixels) {
  FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(pixels < 1.0f ? 1.0f : pixels));
}

void text_measure(const char *utf8, float out[5]) {
  out[0] = 0.0f;
  out[1] = 0.0f;
  out[2] = 0.0f;
  out[3] = 0.0f;
  out[4] = 0.0f;
  if (!g_ready || !utf8 || !*utf8) return;

  mutexLock(&g_lock);

  float pen = 0.0f;
  float min_x = 0.0f, max_x = 0.0f, top = 0.0f, bottom = 0.0f;
  bool first = true;

  for (const char *cursor = utf8; *cursor;) {
    FT_UInt glyph_index;
    const uint32_t code = next_codepoint(&cursor);
    FT_Face face = face_for(code, &glyph_index);
    if (!face || !glyph_index) continue;

    set_size(face, BASE_FONT_SIZE);
    if (FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT) != 0) continue;

    const FT_Glyph_Metrics *metrics = &face->glyph->metrics;
    const float bearing_x = (float)metrics->horiBearingX / 64.0f;
    const float glyph_width = (float)metrics->width / 64.0f;
    const float bearing_y = (float)metrics->horiBearingY / 64.0f;
    const float glyph_height = (float)metrics->height / 64.0f;

    const float left = pen + bearing_x;
    const float right = left + glyph_width;
    if (first || left < min_x) min_x = left;
    if (first || right > max_x) max_x = right;
    if (first || bearing_y > top) top = bearing_y;
    if (first || bearing_y - glyph_height < bottom) bottom = bearing_y - glyph_height;
    first = false;

    pen += (float)face->glyph->advance.x / 64.0f;
  }

  mutexUnlock(&g_lock);

  out[0] = min_x;
  out[1] = max_x;
  out[2] = top;
  out[3] = bottom;
  out[4] = pen;
}

static void blend_glyph(uint8_t *pixels, int width, int height,
                        const FT_Bitmap *bitmap, int origin_x, int origin_y) {
  for (unsigned int row = 0; row < bitmap->rows; row++) {
    const int y = origin_y + (int)row;
    if (y < 0 || y >= height) continue;
    for (unsigned int column = 0; column < bitmap->width; column++) {
      const int x = origin_x + (int)column;
      if (x < 0 || x >= width) continue;

      uint8_t coverage;
      if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
        const uint8_t byte = bitmap->buffer[row * (unsigned)bitmap->pitch + column / 8];
        coverage = (byte & (0x80u >> (column % 8))) ? 255u : 0u;
      } else {
        coverage = bitmap->buffer[row * (unsigned)bitmap->pitch + column];
      }
      if (!coverage) continue;

      /* White premultiplied-by-nothing text; the engine tints it later. */
      uint8_t *pixel = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
      const uint8_t existing = pixel[3];
      const uint8_t alpha = coverage > existing ? coverage : existing;
      pixel[0] = 255;
      pixel[1] = 255;
      pixel[2] = 255;
      pixel[3] = alpha;
    }
  }
}

void text_render(void *pixels, int width, int height, const char *const *strings,
                 const float *positions, int count, float scale) {
  memset(pixels, 0, (size_t)width * (size_t)height * 4);
  if (!g_ready || !strings || !positions || count <= 0) return;

  const float size = BASE_FONT_SIZE * (scale > 0.0f ? scale : 1.0f);

  mutexLock(&g_lock);
  for (int i = 0; i < count; i++) {
    const char *text = strings[i];
    if (!text) continue;
    float pen_x = positions[i * 2 + 0];
    const float baseline_y = positions[i * 2 + 1];

    for (const char *cursor = text; *cursor;) {
      FT_UInt glyph_index;
      const uint32_t code = next_codepoint(&cursor);
      FT_Face face = face_for(code, &glyph_index);
      if (!face || !glyph_index) continue;

      set_size(face, size);
      if (FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER) != 0) continue;

      blend_glyph(pixels, width, height, &face->glyph->bitmap,
                  (int)(pen_x + (float)face->glyph->bitmap_left),
                  (int)(baseline_y - (float)face->glyph->bitmap_top));
      pen_x += (float)face->glyph->advance.x / 64.0f;
    }
  }
  mutexUnlock(&g_lock);
}
