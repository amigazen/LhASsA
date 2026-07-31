/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_charset.c - Unlha32 UTF-16LE and OEM codepage helpers for Amiga Latin-1.
 *
 * Catalog and AmigaDOS paths are ISO-8859-1.  UTF-16LE archive names
 * (ext 0x44/0x45/0x46) and MS-DOS OEM pages are converted here; the
 * public lh.library API never exposes raw UTF-16.
 */

#include "lh_internal.h"

/*
 * CP437 (IBM PC) high bytes 0x80..0xFF mapped to ISO-8859-1 where a
 * Latin-1 glyph exists; otherwise '_'.  Low 7-bit ASCII is identity.
 */
static const unsigned char lh_cp437_to_latin1[128] = {
    /* 80-8F */
    0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7,
    0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,
    /* 90-9F */
    0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9,
    0xFF, 0xD6, 0xDC, 0xA2, 0xA3, 0xA5, '_',  '_',
    /* A0-AF */
    0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1, 0xAA, 0xBA,
    0xBF, '_',  0xAC, 0xBD, 0xBC, 0xA1, 0xAB, 0xBB,
    /* B0-BF box/shades */
    '_',  '_',  '_',  '_',  '_',  '_',  '_',  '_',
    '_',  '_',  '_',  '_',  '_',  '_',  '_',  '_',
    /* C0-CF */
    '_',  '_',  '_',  '_',  '_',  '_',  '_',  '_',
    '_',  '_',  '_',  '_',  '_',  '_',  '_',  '_',
    /* D0-DF */
    '_',  '_',  '_',  '_',  '_',  '_',  '_',  '_',
    '_',  '_',  '_',  '_',  '_',  '_',  '_',  '_',
    /* E0-EF */
    0x61, 0xDF, 0x47, 0x70, 0x53, 0x73, 0xB5, 0x74,
    0x46, 0x54, 0x4F, 0x64, 0x38, 0x66, 0x65, 0x6E,
    /* F0-FF */
    0x3D, 0xB1, 0x3D, 0x3D, 0x28, 0x29, 0xF7, 0x3D,
    0xB0, 0xB7, 0xB7, 0x76, 0x6E, 0xB2, '_',  0xA0
};

static int lh_os_needs_oem(unsigned char os_id)
{
    return (os_id == LH_OS_MSDOS || os_id == LH_OS_WIN95);
}

static unsigned char lh_utf16_unit_to_latin1(unsigned short u)
{
    if (u == LH_UTF16_PATH_SEP || u == (unsigned short)'/'
        || u == (unsigned short)'\\') {
        return (unsigned char)'/';
    }
    if (u <= 0x00FFu) {
        return (unsigned char)u;
    }
    return (unsigned char)'_';
}

lh_status lh_utf16le_to_latin1_buf(
    const unsigned char *src,
    size_t src_len,
    char *dst,
    size_t dst_size,
    size_t *out_len)
{
    size_t i;
    size_t o;

    if (out_len) {
        *out_len = 0;
    }
    if (!src || !dst || dst_size == 0) {
        return LH_ERR_INVALID_ARG;
    }
    if ((src_len & 1u) != 0) {
        return LH_ERR_BAD_HEADER;
    }

    o = 0;
    for (i = 0; i + 1u < src_len; i += 2u) {
        unsigned short u;

        u = (unsigned short)(src[i] | ((unsigned short)src[i + 1u] << 8));
        /* Skip low surrogates; high surrogates become '_'. */
        if (u >= 0xD800u && u <= 0xDBFFu) {
            if (i + 3u < src_len) {
                i += 2u; /* consume low surrogate pair */
            }
            if (o + 1u >= dst_size) {
                return LH_ERR_FILENAME_TOO_LONG;
            }
            dst[o++] = '_';
            continue;
        }
        if (u >= 0xDC00u && u <= 0xDFFFu) {
            if (o + 1u >= dst_size) {
                return LH_ERR_FILENAME_TOO_LONG;
            }
            dst[o++] = '_';
            continue;
        }
        if (o + 1u >= dst_size) {
            return LH_ERR_FILENAME_TOO_LONG;
        }
        dst[o++] = (char)lh_utf16_unit_to_latin1(u);
    }
    dst[o] = '\0';
    if (out_len) {
        *out_len = o;
    }
    return LH_OK;
}

lh_status lh_utf16le_to_latin1(
    const unsigned char *src,
    size_t src_len,
    char **out_str)
{
    char *buf;
    size_t need;
    size_t got;
    lh_status st;

    if (!out_str) {
        return LH_ERR_INVALID_ARG;
    }
    *out_str = NULL;
    if (!src) {
        return LH_ERR_INVALID_ARG;
    }
    if ((src_len & 1u) != 0) {
        return LH_ERR_BAD_HEADER;
    }

    need = (src_len / 2u) + 1u;
    buf = (char *)malloc(need);
    if (!buf) {
        return LH_ERR_NO_MEMORY;
    }
    st = lh_utf16le_to_latin1_buf(src, src_len, buf, need, &got);
    if (st != LH_OK) {
        free(buf);
        return st;
    }
    (void)got;
    *out_str = buf;
    return LH_OK;
}

lh_status lh_latin1_to_utf16le(
    const char *src,
    unsigned char **out_bytes,
    size_t *out_len)
{
    size_t n;
    size_t i;
    unsigned char *buf;

    if (!out_bytes || !out_len) {
        return LH_ERR_INVALID_ARG;
    }
    *out_bytes = NULL;
    *out_len = 0;
    if (!src) {
        return LH_ERR_INVALID_ARG;
    }

    n = strlen(src);
    buf = (unsigned char *)malloc(n * 2u + 2u);
    if (!buf) {
        return LH_ERR_NO_MEMORY;
    }
    for (i = 0; i < n; i++) {
        unsigned char c;

        c = (unsigned char)src[i];
        buf[i * 2u] = c;
        buf[i * 2u + 1u] = 0;
    }
    *out_bytes = buf;
    *out_len = n * 2u;
    return LH_OK;
}

lh_status lh_latin1_path_to_utf16le(
    const unsigned char *src,
    size_t src_len,
    unsigned char **out_bytes,
    size_t *out_len)
{
    size_t i;
    unsigned char *buf;

    if (!out_bytes || !out_len) {
        return LH_ERR_INVALID_ARG;
    }
    *out_bytes = NULL;
    *out_len = 0;
    if (!src && src_len > 0) {
        return LH_ERR_INVALID_ARG;
    }

    buf = (unsigned char *)malloc(src_len * 2u + 2u);
    if (!buf) {
        return LH_ERR_NO_MEMORY;
    }
    for (i = 0; i < src_len; i++) {
        unsigned char c;
        unsigned short u;

        c = src[i];
        if (c == (unsigned char)LH_PATH_SEP || c == (unsigned char)'/') {
            u = LH_UTF16_PATH_SEP;
        } else {
            u = (unsigned short)c;
        }
        buf[i * 2u] = (unsigned char)(u & 0xffu);
        buf[i * 2u + 1u] = (unsigned char)((u >> 8) & 0xffu);
    }
    *out_bytes = buf;
    *out_len = src_len * 2u;
    return LH_OK;
}

void lh_oem_to_latin1_inplace(unsigned char os_id, char *s)
{
    unsigned char *p;

    if (!s || !lh_os_needs_oem(os_id)) {
        return;
    }
    for (p = (unsigned char *)s; *p != '\0'; p++) {
        if (*p >= 0x80u) {
            *p = lh_cp437_to_latin1[*p - 0x80u];
        }
    }
}
