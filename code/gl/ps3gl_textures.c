/* ps3gl_textures.c -- GL-to-RSX: texture allocation, format conversion, binding. */

#include "ps3gl.h"
#include <stdio.h>
#include <stdlib.h>
#include "../sys/ps3_step.h"

/* PS3 bring-up: synced trace of the first texture allocations. */
static int ps3gl_alloc_trace = 0;
#if defined(__PS3__)
#define PS3GL_ATRACE(...) do { if (ps3gl_alloc_trace < 60) { ps3gl_alloc_trace++; PS3_BootTracef("TEX: " __VA_ARGS__); } } while (0)
#else
#define PS3GL_ATRACE(...) ((void)0)
#endif

/* Init / Shutdown */

void ps3gl_textures_init(void)
{
    for (int i = 0; i < PS3GL_MAX_TEXTURES; i++) {
        ps3gl.textures[i].glname = -1;
        ps3gl.textures[i].data   = NULL;
    }
    ps3gl.tex_next_name = 1;
}

static uint32_t mip_total_size(uint32_t pitch, uint32_t w0, uint32_t h0);
static uint32_t compressed_mip_total_size(uint32_t pitch, uint32_t w0, uint32_t h0, uint8_t format);
static uint32_t compressed_mip_level_offset(uint32_t pitch, uint32_t h0, int level, uint8_t format);
static uint32_t texture_block_bytes(uint8_t format)
{
    return format == GCM_TEXTURE_FORMAT_DXT1 ? 8u : 16u;
}


static uint32_t texture_pitch(const ps3gl_texture_t *t)
{
    uint32_t row_bytes;
    if (t->compressed)
        row_bytes = (((uint32_t)t->width + 3u) / 4u) * texture_block_bytes(t->gcm_format);
    else
        row_bytes = (uint32_t)t->width * t->bpp;
    return (row_bytes + 63u) & ~63u;
}

/* RSX memory held by textures, for the log (ps3_prof.c) */
static void tex_free_data(ps3gl_texture_t *t)
{
    uint32_t pitch = texture_pitch(t);
    uint32_t size = t->compressed
        ? compressed_mip_total_size(pitch, t->width, t->height, t->gcm_format)
        : mip_total_size(pitch, t->width, t->height);

    ps3gl_tex_bytes = ps3gl_tex_bytes > size ? ps3gl_tex_bytes - size : 0;
    rsxFree(t->data);
    t->data = NULL;
}

void ps3gl_textures_shutdown(void)
{
    for (int i = 0; i < PS3GL_MAX_TEXTURES; i++) {
        if (ps3gl.textures[i].data) {
            tex_free_data(&ps3gl.textures[i]);
        }
        ps3gl.textures[i].glname = -1;
    }
}

/* Lookup / Allocate */

ps3gl_texture_t *ps3gl_texture_find(GLuint name)
{
    /* Simple linear scan -- 4096 slots is small enough */
    for (int i = 0; i < PS3GL_MAX_TEXTURES; i++) {
        if (ps3gl.textures[i].glname == (int)name)
            return &ps3gl.textures[i];
    }
    return NULL;
}

ps3gl_texture_t *ps3gl_texture_alloc(GLuint name)
{
    for (int i = 0; i < PS3GL_MAX_TEXTURES; i++) {
        if (ps3gl.textures[i].glname == -1) {
            ps3gl_texture_t *t = &ps3gl.textures[i];
            memset(t, 0, sizeof(*t));
            t->glname     = (int)name;
            t->wrap_s     = GCM_TEXTURE_CLAMP_TO_EDGE;
            t->wrap_t     = GCM_TEXTURE_CLAMP_TO_EDGE;
            t->min_filter = GCM_TEXTURE_LINEAR;
            t->mag_filter = GCM_TEXTURE_LINEAR;
            t->dirty      = 1;
            return t;
        }
    }
    printf("[ps3gl] WARNING: texture pool exhausted\n");
    return NULL;
}

/* GL functions */

void ps3gl_GenTextures(GLsizei n, GLuint *textures)
{
    PS3GL_FRONT_CHECK();
    if (!textures || n <= 0) return;
    for (int i = 0; i < n; i++) {
        textures[i] = ps3gl.tex_next_name++;
    }
}

void ps3gl_DeleteTextures(GLsizei n, const GLuint *textures)
{
    PS3GL_FRONT_CHECK();
    if (!textures) return;
    for (int i = 0; i < n; i++) {
        ps3gl_texture_t *t = ps3gl_texture_find(textures[i]);
        if (t) {
            /* Unbind from any TMU */
            for (int j = 0; j < PS3GL_MAX_TMUS; j++) {
                if (ps3gl.tmu[j].bound == t) {
                    ps3gl.tmu[j].bound = NULL;
                    ps3gl.tmu[j].dirty = 1;
                }
            }
            if (t->data) {
                tex_free_data(t);
            }
            t->glname = -1;
        }
    }
}

void ps3gl_BindTexture(GLenum target, GLuint texture)
{
    PS3GL_FRONT_CHECK();
    (void)target; /* only GL_TEXTURE_2D supported */
    if (texture == 0) {
        if (ps3gl.tmu[ps3gl.active_tmu].bound != NULL)
            ps3gl.tmu[ps3gl.active_tmu].dirty = 1;
        ps3gl.tmu[ps3gl.active_tmu].bound = NULL;
        return;
    }

    ps3gl_texture_t *t = ps3gl_texture_find(texture);
    if (!t) {
        t = ps3gl_texture_alloc(texture);
    }
    if (ps3gl.tmu[ps3gl.active_tmu].bound != t)
        ps3gl.tmu[ps3gl.active_tmu].dirty = 1;
    ps3gl.tmu[ps3gl.active_tmu].bound = t;
}

/* Pixel format helpers */

static int gl_format_bpp(GLenum format)
{
    switch (format) {
    case GL_ALPHA:
    case GL_LUMINANCE:
    case GL_RED:
        return 1;
    case GL_LUMINANCE_ALPHA:
        return 2;
    case GL_RGB:
    case GL_RGB8:
        return 3;
    case GL_RGBA:
    case GL_RGBA8:
    default:
        return 4;
    }
}

/* Match the GL1 renderer's requested precision to a native RSX texture format. */
static void gl_storage_format(GLint internalformat, uint8_t *bpp, uint8_t *gcm_format)
{
    switch (internalformat) {
    case GL_RGB:
    case GL_RGB5:
    case 3: /* legacy GL three-component internal format */
        *bpp = 2;
        *gcm_format = GCM_TEXTURE_FORMAT_R5G6B5;
        break;
    case GL_RGBA4:
        *bpp = 2;
        *gcm_format = GCM_TEXTURE_FORMAT_A4R4G4B4;
        break;
    default:
        *bpp = 4;
        *gcm_format = GCM_TEXTURE_FORMAT_A8R8G8B8;
        break;
    }
}

static void decode_pixel(const uint8_t *src, int src_bpp, GLenum src_format,
                         uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a)
{
    switch (src_bpp) {
    case 1:
        if (src_format == GL_ALPHA) {
            *r = *g = *b = 255;
            *a = src[0];
        } else {
            *r = *g = *b = src[0];
            *a = 255;
        }
        break;
    case 2:
        *r = *g = *b = src[0];
        *a = src[1];
        break;
    case 3:
        *r = src[0];
        *g = src[1];
        *b = src[2];
        *a = 255;
        break;
    default:
        *r = src[0];
        *g = src[1];
        *b = src[2];
        *a = src[3];
        break;
    }
}

/* PPU is big-endian: these packed words have the byte order expected by RSX. */
static void store_pixel(uint8_t *dst, uint8_t dst_bpp, uint8_t gcm_format,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (dst_bpp == 2) {
        uint16_t packed;
        if (gcm_format == GCM_TEXTURE_FORMAT_R5G6B5) {
            packed = (uint16_t)(((uint16_t)(r >> 3) << 11) |
                                ((uint16_t)(g >> 2) << 5) |
                                (uint16_t)(b >> 3));
        } else {
            packed = (uint16_t)(((uint16_t)(a >> 4) << 12) |
                                ((uint16_t)(r >> 4) << 8) |
                                ((uint16_t)(g >> 4) << 4) |
                                (uint16_t)(b >> 4));
        }
        dst[0] = (uint8_t)(packed >> 8);
        dst[1] = (uint8_t)packed;
        return;
    }

    /* RSX A8R8G8B8 in big-endian memory: byte order A R G B. */
    dst[0] = a;
    dst[1] = r;
    dst[2] = g;
    dst[3] = b;
}

/* Every mip row uses the base level's 64-byte-aligned pitch. */
static void convert_pixels(uint8_t *dst, uint32_t dst_stride, const uint8_t *src,
                           int width, int height, int src_bpp, GLenum src_format,
                           uint8_t dst_bpp, uint8_t gcm_format)
{
    if (dst_bpp == 4 && src_bpp == 4) {
        for (int row = 0; row < height; row++) {
            uint32_t *drow = (uint32_t *)(dst + (uint32_t)row * dst_stride);
            const uint8_t *srow = src + (size_t)row * width * 4;
            for (int col = 0; col < width; col++) {
                drow[col] = ((uint32_t)srow[col * 4 + 3] << 24) |
                            ((uint32_t)srow[col * 4 + 0] << 16) |
                            ((uint32_t)srow[col * 4 + 1] << 8) |
                            (uint32_t)srow[col * 4 + 2];
            }
        }
        return;
    }

    for (int row = 0; row < height; row++) {
        uint8_t *drow = dst + (uint32_t)row * dst_stride;
        const uint8_t *srow = src + (size_t)row * width * src_bpp;
        for (int col = 0; col < width; col++) {
            uint8_t r, g, b, a;
            decode_pixel(srow + (size_t)col * src_bpp, src_bpp, src_format, &r, &g, &b, &a);
            store_pixel(drow + (size_t)col * dst_bpp, dst_bpp, gcm_format, r, g, b, a);
        }
    }
}

/* Mip chain helpers. RSX linear textures describe the whole chain from ONE width/height/
 * pitch/offset -- every level uses the BASE level's pitch, only row count shrinks. NOT tightly packed, don't "fix" it into being so. */

static uint32_t mip_level_dim(uint32_t base, int level)
{
    uint32_t d = base >> level;
    return d ? d : 1;
}

static int mip_num_levels(uint32_t w0, uint32_t h0)
{
    uint32_t maxdim = (w0 > h0) ? w0 : h0;
    int levels = 1;
    while (maxdim > 1) { maxdim >>= 1; levels++; }
    return levels;
}

/* Byte offset of `level`'s data within the packed mip buffer, given the
 * base level's row pitch (shared by every level) and height. */
static uint32_t mip_level_offset(uint32_t pitch, uint32_t h0, int level)
{
    uint32_t off = 0;
    for (int i = 0; i < level; i++)
        off += pitch * mip_level_dim(h0, i);
    return off;
}

static uint32_t mip_total_size(uint32_t pitch, uint32_t w0, uint32_t h0)
{
    return mip_level_offset(pitch, h0, mip_num_levels(w0, h0));
}

static uint32_t compressed_mip_level_offset(uint32_t pitch, uint32_t h0,
                                            int level, uint8_t format)
{
    uint32_t off = 0;
    for (int i = 0; i < level; i++) {
        uint32_t h = mip_level_dim(h0, i);
        uint32_t rows = (h + 3u) / 4u;
        if (!rows) rows = 1;
        off += pitch * rows;
    }
    (void)format;
    return off;
}

static uint32_t compressed_mip_total_size(uint32_t pitch, uint32_t w0,
                                          uint32_t h0, uint8_t format)
{
    uint32_t total = 0;
    int levels = mip_num_levels(w0, h0);
    for (int i = 0; i < levels; i++) {
        uint32_t h = mip_level_dim(h0, i);
        uint32_t rows = (h + 3u) / 4u;
        if (!rows) rows = 1;
        total += pitch * rows;
    }
    (void)format;
    return total;
}

/* Build gcmTexture descriptor */
static void build_gcm_texture(ps3gl_texture_t *t)
{
    memset(&t->gcm_tex, 0, sizeof(t->gcm_tex));
    t->gcm_tex.format    = t->gcm_format | GCM_TEXTURE_FORMAT_LIN;
    t->gcm_tex.mipmap    = t->num_levels ? t->num_levels : 1;
    t->gcm_tex.dimension = GCM_TEXTURE_DIMS_2D;
    t->gcm_tex.cubemap   = GCM_FALSE;
    t->gcm_tex.remap     = PS3GL_TEX_REMAP_IDENTITY;
    t->gcm_tex.width     = t->width;
    t->gcm_tex.height    = t->height;
    t->gcm_tex.depth     = 1;
    t->gcm_tex.location  = GCM_LOCATION_RSX;
    t->gcm_tex.pitch     = texture_pitch(t);
    t->gcm_tex.offset    = t->offset;
}

extern void ps3_log(const char *msg);
static int ps3gl_teximg_diag_count = 0;

/* RSX linear texture offsets require 128-byte alignment; row pitches use 64. */
#define PS3GL_TEXTURE_ALIGN 128

void ps3gl_TexImage2D(GLenum target, GLint level, GLint internalformat,
                      GLsizei width, GLsizei height, GLint border,
                      GLenum format, GLenum type, const void *pixels)
{
    PS3GL_FRONT_CHECK();
    (void)target; (void)border; (void)type;

    if (level < 0) return;

    ps3gl_texture_t *t = ps3gl.tmu[ps3gl.active_tmu].bound;
    if (!t) return;

    /* Log first 30 texture uploads for diagnostics (make DIAG=1) */
#ifdef PS3_DIAG
    if (ps3gl_teximg_diag_count < 30) {
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "PS3GL_TEXIMG: tex=%d level=%d %dx%d intfmt=0x%x fmt=0x%x type=0x%x px=%p",
                 t->glname, (int)level, (int)width, (int)height,
                 (unsigned)internalformat, (unsigned)format,
                 (unsigned)type, pixels);
        ps3_log(dbg);
        ps3gl_teximg_diag_count++;
    }
#endif

    if (level == 0) {
        uint8_t new_bpp, new_gcm_format;
        gl_storage_format(internalformat, &new_bpp, &new_gcm_format);

        /* A redefinition may change precision even when dimensions stay fixed. */
        if (t->data && (t->compressed || t->width != (uint16_t)width || t->height != (uint16_t)height ||
                        t->bpp != new_bpp || t->gcm_format != new_gcm_format)) {
            tex_free_data(t);
        }

        t->width      = (uint16_t)width;
        t->height     = (uint16_t)height;
        t->bpp        = new_bpp;
        t->gcm_format = new_gcm_format;
        t->compressed = 0;
        t->num_levels = 0;

        if (!t->data) {
            uint32_t pitch = texture_pitch(t);
            uint32_t size = mip_total_size(pitch, (uint32_t)width, (uint32_t)height);
            PS3GL_ATRACE("tex %d %dx%d bpp=%d: rsxMemalign(64, %u)",
                         (int)t->glname, (int)width, (int)height,
                         (int)t->bpp, (unsigned)size);
            t->data = (uint8_t *)rsxMemalign(PS3GL_TEXTURE_ALIGN, size);
            PS3GL_ATRACE("tex %d: rsxMemalign -> %p", (int)t->glname, (void *)t->data);
            if (!t->data) {
                char msg[160];
                if (ps3gl_tex_failed++ < 16) {
                    snprintf(msg, sizeof(msg),
                             "[ps3gl] WARNING: out of RSX memory: texture %d, %u bytes bpp=%u "
                             "(textures hold %u KB)",
                             t->glname, size, (unsigned)t->bpp, ps3gl_tex_bytes / 1024);
                    ps3_log(msg);
                }
                return;
            }
            ps3gl_tex_bytes += size;
            PS3GL_ATRACE("tex %d: before rsxAddressToOffset", (int)t->glname);
            {
                int ps3_off_ret = rsxAddressToOffset(t->data, &t->offset);
                PS3GL_ATRACE("tex %d: rsxAddressToOffset ret=0x%x offset=0x%x",
                             (int)t->glname, (unsigned)ps3_off_ret, (unsigned)t->offset);
            }
        }
    }

    if (!t->data) return;
    if (level >= mip_num_levels(t->width, t->height)) return;

    uint32_t pitch = texture_pitch(t);
    uint32_t lvl_off = mip_level_offset(pitch, t->height, level);

    if (pixels) {
        int src_bpp = gl_format_bpp(format);
        PS3GL_ATRACE("tex %d level %d: convert srcbpp=%d dstbpp=%d",
                     (int)t->glname, (int)level, src_bpp, (int)t->bpp);
        convert_pixels(t->data + lvl_off, pitch, (const uint8_t *)pixels,
                       width, height, src_bpp, format, t->bpp, t->gcm_format);
    } else {
        memset(t->data + lvl_off, 0, pitch * mip_level_dim(t->height, level));
    }

    if ((uint8_t)(level + 1) > t->num_levels)
        t->num_levels = (uint8_t)(level + 1);

    build_gcm_texture(t);
    t->dirty = 1;
}

void ps3gl_TexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff,
                         GLsizei w, GLsizei h, GLenum format, GLenum type,
                         const void *pixels)
{
    PS3GL_FRONT_CHECK();
    (void)target; (void)type;
    if (level != 0 || !pixels) return;

    ps3gl_texture_t *t = ps3gl.tmu[ps3gl.active_tmu].bound;
    if (!t || !t->data) return;

    int src_bpp = gl_format_bpp(format);
    const uint8_t *src = (const uint8_t *)pixels;
    uint32_t dst_pitch = texture_pitch(t);

    for (int row = 0; row < h; row++) {
        int dy = yoff + row;
        if (dy < 0 || dy >= t->height) continue;
        for (int col = 0; col < w; col++) {
            int dx = xoff + col;
            if (dx < 0 || dx >= t->width) continue;

            uint8_t r, g, b, a;
            size_t si = ((size_t)row * w + col) * src_bpp;
            decode_pixel(src + si, src_bpp, format, &r, &g, &b, &a);
            store_pixel(t->data + (size_t)dy * dst_pitch + (size_t)dx * t->bpp,
                        t->bpp, t->gcm_format, r, g, b, a);
        }
    }

    t->dirty = 1;
}

/* Texture parameters */
/* Upload native S3TC blocks directly into RSX memory. The RSX samples the
 * compressed blocks without a PPU-side RGBA expansion. */
static int compressed_format_to_gcm(GLenum format, uint8_t *gcm_format)
{
    switch (format) {
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        *gcm_format = GCM_TEXTURE_FORMAT_DXT1;
        return 1;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
        *gcm_format = GCM_TEXTURE_FORMAT_DXT23;
        return 1;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        *gcm_format = GCM_TEXTURE_FORMAT_DXT45;
        return 1;
    default:
        return 0;
    }
}

void ps3gl_CompressedTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                GLsizei width, GLsizei height, GLint border,
                                GLsizei imageSize, const void *data)
{
    PS3GL_FRONT_CHECK();
    if (target != GL_TEXTURE_2D || level < 0 || border != 0 ||
        width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        imageSize < 0)
        return;

    ps3gl_texture_t *t = ps3gl.tmu[ps3gl.active_tmu].bound;
    if (!t) return;

    uint8_t gcm_format;
    if (!compressed_format_to_gcm(internalformat, &gcm_format)) return;

    if (level == 0) {
        if (t->data && (!t->compressed || t->width != (uint16_t)width ||
                        t->height != (uint16_t)height || t->gcm_format != gcm_format))
            tex_free_data(t);

        t->width = (uint16_t)width;
        t->height = (uint16_t)height;
        t->bpp = 0;
        t->gcm_format = gcm_format;
        t->compressed = 1;
        t->num_levels = 0;

        if (!t->data) {
            uint32_t pitch = texture_pitch(t);
            uint32_t size = compressed_mip_total_size(pitch, t->width, t->height, t->gcm_format);
            t->data = (uint8_t *)rsxMemalign(PS3GL_TEXTURE_ALIGN, size);
            if (!t->data) {
                if (ps3gl_tex_failed++ < 16)
                    PS3_BootTracef("ps3gl: DXT texture allocation failed (%dx%d, %u bytes)",
                                   width, height, (unsigned)size);
                return;
            }
            memset(t->data, 0, size);
            ps3gl_tex_bytes += size;
            if (rsxAddressToOffset(t->data, &t->offset) != 0) {
                tex_free_data(t);
                return;
            }
        }
    } else if (!t->data || !t->compressed ||
               level >= mip_num_levels(t->width, t->height) ||
               width != (GLsizei)mip_level_dim(t->width, level) ||
               height != (GLsizei)mip_level_dim(t->height, level) ||
               level > t->num_levels) {
        return;
    }

    if (!t->data) return;

    const uint32_t block_bytes = texture_block_bytes(t->gcm_format);
    const uint32_t blocks_x = ((uint32_t)width + 3u) / 4u;
    const uint32_t blocks_y = ((uint32_t)height + 3u) / 4u;
    const uint32_t source_pitch = blocks_x * block_bytes;
    const uint32_t required = source_pitch * blocks_y;
    if (!data || (uint32_t)imageSize < required) return;

    const uint32_t pitch = texture_pitch(t);
    const uint32_t level_off = compressed_mip_level_offset(pitch, t->height, level, t->gcm_format);
    uint8_t *dst = t->data + level_off;
    memset(dst, 0, pitch * blocks_y);
    for (uint32_t row = 0; row < blocks_y; row++)
        memcpy(dst + row * pitch, (const uint8_t *)data + row * source_pitch, source_pitch);

    if ((uint8_t)(level + 1) > t->num_levels)
        t->num_levels = (uint8_t)(level + 1);
    build_gcm_texture(t);
    t->dirty = 1;
}

void ps3gl_CompressedTexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff,
                                   GLsizei width, GLsizei height, GLenum format,
                                   GLsizei imageSize, const void *data)
{
    PS3GL_FRONT_CHECK();
    if (target != GL_TEXTURE_2D || level < 0 || xoff < 0 || yoff < 0 ||
        width <= 0 || height <= 0 || imageSize < 0 || !data)
        return;

    ps3gl_texture_t *t = ps3gl.tmu[ps3gl.active_tmu].bound;
    uint8_t gcm_format;
    if (!t || !t->data || !t->compressed ||
        !compressed_format_to_gcm(format, &gcm_format) || gcm_format != t->gcm_format ||
        level >= t->num_levels)
        return;

    const uint32_t level_w = mip_level_dim(t->width, level);
    const uint32_t level_h = mip_level_dim(t->height, level);
    if ((uint32_t)xoff + (uint32_t)width > level_w ||
        (uint32_t)yoff + (uint32_t)height > level_h)
        return;
    if ((xoff & 3) || (yoff & 3) ||
        ((width & 3) && (uint32_t)(xoff + width) != level_w) ||
        ((height & 3) && (uint32_t)(yoff + height) != level_h))
        return;

    const uint32_t block_bytes = texture_block_bytes(t->gcm_format);
    const uint32_t blocks_x = ((uint32_t)width + 3u) / 4u;
    const uint32_t blocks_y = ((uint32_t)height + 3u) / 4u;
    const uint32_t source_pitch = blocks_x * block_bytes;
    if ((uint32_t)imageSize < source_pitch * blocks_y) return;

    const uint32_t pitch = texture_pitch(t);
    uint8_t *dst = t->data + compressed_mip_level_offset(pitch, t->height, level, t->gcm_format)
                 + ((uint32_t)yoff / 4u) * pitch
                 + ((uint32_t)xoff / 4u) * block_bytes;
    for (uint32_t row = 0; row < blocks_y; row++)
        memcpy(dst + row * pitch, (const uint8_t *)data + row * source_pitch, source_pitch);
    t->dirty = 1;
}



static uint8_t gl_to_gcm_wrap(GLint param)
{
    switch (param) {
    case GL_REPEAT:        return GCM_TEXTURE_REPEAT;
    case GL_CLAMP:         return GCM_TEXTURE_CLAMP;
    case GL_CLAMP_TO_EDGE: return GCM_TEXTURE_CLAMP_TO_EDGE;
    default:               return GCM_TEXTURE_REPEAT;
    }
}

static uint8_t gl_to_gcm_filter(GLint param)
{
    switch (param) {
    case GL_NEAREST:                return GCM_TEXTURE_NEAREST;
    case GL_LINEAR:                 return GCM_TEXTURE_LINEAR;
    case GL_NEAREST_MIPMAP_NEAREST: return GCM_TEXTURE_NEAREST_MIPMAP_NEAREST;
    case GL_LINEAR_MIPMAP_NEAREST:  return GCM_TEXTURE_LINEAR_MIPMAP_NEAREST;
    case GL_NEAREST_MIPMAP_LINEAR:  return GCM_TEXTURE_NEAREST_MIPMAP_LINEAR;
    case GL_LINEAR_MIPMAP_LINEAR:   return GCM_TEXTURE_LINEAR_MIPMAP_LINEAR;
    default:                        return GCM_TEXTURE_LINEAR;
    }
}

static void tex_param(GLenum pname, GLint param)
{
    ps3gl_texture_t *t = ps3gl.tmu[ps3gl.active_tmu].bound;
    if (!t) return;

    switch (pname) {
    case GL_TEXTURE_WRAP_S:     t->wrap_s     = gl_to_gcm_wrap(param); break;
    case GL_TEXTURE_WRAP_T:     t->wrap_t     = gl_to_gcm_wrap(param); break;
    case GL_TEXTURE_MIN_FILTER: t->min_filter = gl_to_gcm_filter(param); break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = gl_to_gcm_filter(param); break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT:
    case GL_GENERATE_MIPMAP:
        break;
    default: break;
    }
    t->dirty = 1;
}

void ps3gl_TexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    PS3GL_FRONT_CHECK();
    (void)target;
    tex_param(pname, (GLint)param);
}

void ps3gl_TexParameteri(GLenum target, GLenum pname, GLint param)
{
    PS3GL_FRONT_CHECK();
    (void)target;
    tex_param(pname, param);
}

/* TexEnv */

void ps3gl_TexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    PS3GL_FRONT_CHECK();
    (void)target;
    if (pname != GL_TEXTURE_ENV_MODE) return;

    int mode;
    GLenum p = (GLenum)(int)param;
    switch (p) {
    case GL_MODULATE:       mode = PS3GL_TENV_MODULATE; break;
    case GL_REPLACE:        mode = PS3GL_TENV_REPLACE; break;
    case GL_DECAL:          mode = PS3GL_TENV_DECAL; break;
    case GL_ADD:            mode = PS3GL_TENV_ADD; break;
    case GL_BLEND:          mode = PS3GL_TENV_BLEND; break;
    default:                mode = PS3GL_TENV_MODULATE; break;
    }
    if (ps3gl.tmu[ps3gl.active_tmu].texenv != mode)
        ps3gl.tmu[ps3gl.active_tmu].dirty = 1;
    ps3gl.tmu[ps3gl.active_tmu].texenv = mode;
}

void ps3gl_TexEnvi(GLenum target, GLenum pname, GLint param)
{
    PS3GL_FRONT_CHECK();
    ps3gl_TexEnvf(target, pname, (GLfloat)param);
}

/* Multitexture */

void ps3gl_ActiveTextureARB(GLenum texture)
{
    PS3GL_FRONT_CHECK();
    int tmu = (int)(texture - GL_TEXTURE0_ARB);
    if (tmu >= 0 && tmu < PS3GL_MAX_TMUS)
        ps3gl.active_tmu = tmu;
}

void ps3gl_ClientActiveTextureARB(GLenum texture)
{
    int tmu = (int)(texture - GL_TEXTURE0_ARB);
    if (tmu >= 0 && tmu < PS3GL_MAX_TMUS)
        ps3gl.client_active_tmu = tmu;
}

void ps3gl_PixelStorei(GLenum pname, GLint param)
{
    (void)pname; (void)param;
}

static unsigned ps3gl_copytex_diag_count;
void ps3gl_CopyTexSubImage2D(GLenum target, GLint level, GLint xoff,
                              GLint yoff, GLint x, GLint y,
                              GLsizei w, GLsizei h)
{
    ps3gl_texture_t *t;
    PS3GL_FRONT_CHECK();
    t = ps3gl.tmu[ps3gl.active_tmu].bound;
    if (ps3gl_copytex_diag_count < 8) {
        PS3_BootTraceFastf("PS3GL_COPYTEX unsupported call=%u target=0x%x level=%d dst=%d,%d src=%d,%d size=%dx%d tex=%d bpp=%u fmt=0x%x",
                           ++ps3gl_copytex_diag_count, (unsigned)target, (int)level,
                           (int)xoff, (int)yoff, (int)x, (int)y, (int)w, (int)h,
                           t ? t->glname : -1, t ? (unsigned)t->bpp : 0u,
                           t ? (unsigned)t->gcm_format : 0u);
    }
}

/* Apply textures to RSX before draw */

void ps3gl_apply_textures(void)
{
    gcmContextData *ctx = ps3gl_get_ctx();
    if (!ctx) return;

    for (int i = 0; i < PS3GL_MAX_TMUS; i++) {
        ps3gl_tmu_t *tmu = &ps3gl.tmu[i];
        ps3gl_texture_t *t = tmu->bound;

        if (!tmu->enabled || !t || !t->data) {
            if (tmu->dirty) {
                rsxTextureControl(ctx, i, GCM_FALSE, 0, 0, 0);
                tmu->dirty = 0;
            }
            continue;
        }

        /* Skip re-bind if nothing changed on this TMU */
        if (!tmu->dirty && !t->dirty) continue;

        rsxLoadTexture(ctx, i, &t->gcm_tex);
        /* maxlod is 4.8 fixed point, NOT an integer level count -- get this wrong and the
         * texture binds but renders nothing. maxlod = (num_levels-1)<<8 so the sampler can reach every uploaded mip. */
        {
            uint16_t maxlod = (uint16_t)(((t->num_levels ? t->num_levels : 1) - 1) << 8);
            rsxTextureControl(ctx, i, GCM_TRUE, 0, maxlod, GCM_TEXTURE_MAX_ANISO_1);
        }
        rsxTextureFilter(ctx, i, 0, t->min_filter, t->mag_filter,
                         GCM_TEXTURE_CONVOLUTION_QUINCUNX);
        rsxTextureWrapMode(ctx, i, t->wrap_s, t->wrap_t,
                           GCM_TEXTURE_CLAMP_TO_EDGE,
                           GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
                           GCM_TEXTURE_ZFUNC_NEVER, 0);

        tmu->dirty = 0;
        t->dirty = 0;
    }
}
