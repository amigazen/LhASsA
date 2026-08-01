/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_header.c - LHA header read/write (levels 0-2).
 *
 * Level 0/1 header read follows lhex/header.c (split buffer + extension chain).
 */

#include "lh_internal.h"

#include <string.h>

#define LH_HDR_BUF      1024
#define LH_I_CHECKSUM   1
#define LH_I_METHOD     2
#define LH_I_PACKED     7
#define LH_I_ORIGINAL   11
#define LH_I_STAMP      15
#define LH_I_ATTR       19
#define LH_I_LEVEL      20
#define LH_I_NLEN       21
#define LH_I_NAME       22

int lh_debug_verbose = 0;

void lh_set_debug_verbose(int on)
{
    lh_debug_verbose = on ? 1 : 0;
}

/*
 * AmigaDOS uses '/' as the only path separator.  LHA type-2 paths use 0xFF;
 * MS-DOS / Windows archives (common on Aminet) use '\\'.  Left unchanged,
 * those characters become literal filename characters and no drawers appear.
 */
void lh_normalize_path_seps(char *path)
{
    char *p;

    if (path == NULL) {
        return;
    }
    for (p = path; *p != '\0'; p++) {
        if (*p == '\\' || *p == (char)LH_PATH_SEP) {
            *p = '/';
        }
    }
}

lh_u32 lh_read_le32(const unsigned char *p)
{
    /* Use lh_u32 shifts so 16-bit int builds cannot truncate high bytes. */
    return (lh_u32)(
        ((lh_u32)p[0])
      | ((lh_u32)p[1] << 8)
      | ((lh_u32)p[2] << 16)
      | ((lh_u32)p[3] << 24)
    );
}

void lh_write_le32(unsigned char *p, lh_u32 v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

lh_u16 lh_read_le16(const unsigned char *p)
{
    return (lh_u16)(((unsigned int)p[0]) | ((unsigned int)p[1] << 8));
}

void lh_write_le16(unsigned char *p, lh_u16 v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

void lh_hdr_meta_clear(lh_hdr_meta *m)
{
    if (!m) {
        return;
    }
    free(m->filename);
    free(m->comment);
    memset(m, 0, sizeof(*m));
}

#define LH_SNAP_MAGIC0 'L'
#define LH_SNAP_MAGIC1 'H'
#define LH_SNAP_MAGIC2 'S'
#define LH_SNAP_MAGIC3 'S'
#define LH_SNAP_BASE_LEN 64u

lh_status lh_snapshot_pack(
    const lh_snapshot *cp,
    unsigned char **out_bytes,
    size_t *out_len)
{
    size_t ver_len;
    size_t total;
    unsigned char *buf;

    if (!cp || !out_bytes || !out_len) {
        return LH_ERR_INVALID_ARG;
    }
    *out_bytes = NULL;
    *out_len = 0;

    ver_len = strlen(cp->ver);
    if (ver_len > LH_SNAP_VER_MAX) {
        ver_len = LH_SNAP_VER_MAX;
    }
    total = LH_SNAP_BASE_LEN + 1u + ver_len;
    buf = (unsigned char *)malloc(total);
    if (!buf) {
        return LH_ERR_NO_MEMORY;
    }
    buf[0] = (unsigned char)LH_SNAP_MAGIC0;
    buf[1] = (unsigned char)LH_SNAP_MAGIC1;
    buf[2] = (unsigned char)LH_SNAP_MAGIC2;
    buf[3] = (unsigned char)LH_SNAP_MAGIC3;
    buf[4] = cp->format ? cp->format : (unsigned char)LH_SNAP_FORMAT_V1;
    buf[5] = cp->flags;
    buf[6] = 0;
    buf[7] = 0;
    lh_write_le32(buf + 8, (lh_u32)cp->generation);
    lh_write_le32(buf + 12, (lh_u32)cp->size);
    memcpy(buf + 16, cp->snap_id, LH_SNAP_ID_LEN);
    memcpy(buf + 32, cp->sha256, LH_SNAP_SHA256_LEN);
    buf[64] = (unsigned char)ver_len;
    if (ver_len > 0) {
        memcpy(buf + 65, cp->ver, ver_len);
    }
    *out_bytes = buf;
    *out_len = total;
    return LH_OK;
}

lh_status lh_snapshot_unpack(
    const unsigned char *src,
    size_t src_len,
    lh_snapshot *cp)
{
    unsigned char ver_len;

    if (!src || !cp) {
        return LH_ERR_INVALID_ARG;
    }
    memset(cp, 0, sizeof(*cp));
    if (src_len < LH_SNAP_BASE_LEN + 1u) {
        return LH_ERR_BAD_HEADER;
    }
    if (src[0] != (unsigned char)LH_SNAP_MAGIC0
        || src[1] != (unsigned char)LH_SNAP_MAGIC1
        || src[2] != (unsigned char)LH_SNAP_MAGIC2
        || src[3] != (unsigned char)LH_SNAP_MAGIC3) {
        return LH_ERR_BAD_HEADER;
    }
    cp->format = src[4];
    if (cp->format != (unsigned char)LH_SNAP_FORMAT_V1) {
        return LH_ERR_UNSUPPORTED;
    }
    cp->flags = src[5];
    cp->generation = lh_read_le32(src + 8);
    cp->size = lh_read_le32(src + 12);
    memcpy(cp->snap_id, src + 16, LH_SNAP_ID_LEN);
    memcpy(cp->sha256, src + 32, LH_SNAP_SHA256_LEN);
    ver_len = src[64];
    if ((size_t)ver_len > LH_SNAP_VER_MAX
        || LH_SNAP_BASE_LEN + 1u + (size_t)ver_len > src_len) {
        return LH_ERR_BAD_HEADER;
    }
    if (ver_len > 0) {
        memcpy(cp->ver, src + 65, ver_len);
    }
    cp->ver[ver_len] = '\0';
    return LH_OK;
}

static lh_status lh_ext_set_snapshot(
    lh_hdr_meta *meta,
    const unsigned char *payload,
    size_t payload_len)
{
    lh_status st;

    st = lh_snapshot_unpack(payload, payload_len, &meta->snapshot);
    if (st != LH_OK) {
        return st;
    }
    meta->have_snapshot = 1;
    return LH_OK;
}

static unsigned char lh_hdr_checksum(const unsigned char *data, size_t len)
{
    unsigned char sum = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        sum = (unsigned char)(sum + data[i]);
    }
    return sum;
}

static void lh_hdr_debug(const char *tag, lh_stream *io, const lh_hdr_meta *meta, lh_status st)
{
    (void)tag;
    (void)io;
    (void)meta;
    (void)st;
    /* Verbose header tracing is host-only; library clients use lh.library LhErr. */
}

/* lhex-style buffer walk (data[0] unused; fields start at data[1]). */
static unsigned char *lh_get_ptr;

static void lh_setup_get(unsigned char *p)
{
    lh_get_ptr = p;
}

static unsigned int lh_get_byte(void)
{
    return (unsigned int)(*lh_get_ptr++ & 0xff);
}

static unsigned short lh_get_word(void)
{
    unsigned int b0;
    unsigned int b1;

    b0 = lh_get_byte();
    b1 = lh_get_byte();
    return (unsigned short)((b1 << 8) + b0);
}

static unsigned long lh_get_longword(void)
{
    unsigned long b0;
    unsigned long b1;
    unsigned long b2;
    unsigned long b3;

    b0 = (unsigned long)lh_get_byte();
    b1 = (unsigned long)lh_get_byte();
    b2 = (unsigned long)lh_get_byte();
    b3 = (unsigned long)lh_get_byte();
    return (b3 << 24) + (b2 << 16) + (b1 << 8) + b0;
}

static void lh_hdr_read_base_from_buf(lh_hdr_meta *meta, unsigned char *buf)
{
    unsigned char name_len;

    memcpy(meta->method_sig, buf + 2, LH_SIG_LEN);
    meta->method_sig[LH_SIG_LEN] = '\0';
    meta->method = lh_method_from_string((char *)meta->method_sig);
    meta->packed_size = lh_read_le32(buf + 7);
    meta->original_size = lh_read_le32(buf + 11);
    meta->timestamp = lh_read_le32(buf + 15);
    meta->attribute = buf[19];
    meta->header_level = buf[20];
    name_len = buf[21];
    if (name_len > 0) {
        meta->filename = (char *)malloc((size_t)name_len + 1u);
        if (meta->filename) {
            memcpy(meta->filename, buf + 22, name_len);
            meta->filename[name_len] = '\0';
        }
    }
    meta->is_directory = (meta->method == LH_METHOD_LHD);
}

static lh_status lh_hdr_skip_bytes(lh_stream *io, size_t n)
{
    unsigned char *scratch;
    size_t chunk;
    lh_status st;

    scratch = (unsigned char *)malloc(256);
    if (!scratch) {
        return LH_ERR_NO_MEMORY;
    }
    st = LH_OK;
    while (n > 0) {
        chunk = n;
        if (chunk > 256u) {
            chunk = 256u;
        }
        if (lh_stream_read(io, scratch, chunk) != chunk) {
            st = LH_ERR_TRUNCATED;
            break;
        }
        n -= chunk;
    }
    free(scratch);
    return st;
}

static int lh_mf_read(
    lh_stream *io,
    const unsigned char *mem,
    size_t mem_len,
    size_t *mem_pos,
    void *out,
    size_t n)
{
    unsigned char *dst;
    size_t avail;
    size_t take;

    dst = (unsigned char *)out;
    while (n > 0) {
        avail = 0;
        if (*mem_pos < mem_len) {
            avail = mem_len - *mem_pos;
        }
        take = n;
        if (take > avail) {
            take = avail;
        }
        if (take > 0) {
            memcpy(dst, mem + *mem_pos, take);
            *mem_pos += take;
            dst += take;
            n -= take;
        }
        if (n > 0) {
            if (lh_stream_read(io, dst, n) != n) {
                return 0;
            }
            dst += n;
            n = 0;
        }
    }
    return 1;
}

/* Promote dirname + basename into meta->filename (shared by ext readers). */
static lh_status lh_ext_combine_dir_file(
    lh_hdr_meta *meta,
    char *dirbuf,
    size_t dir_len)
{
    char *combined;
    size_t dlen;
    size_t flen;

    if (dir_len > 0 && meta->filename && meta->filename[0]) {
        dlen = dir_len;
        flen = strlen(meta->filename);
        combined = (char *)malloc(dlen + flen + 2u);
        if (!combined) {
            return LH_ERR_NO_MEMORY;
        }
        memcpy(combined, dirbuf, dlen);
        if (dlen > 0 && dirbuf[dlen - 1] != '/' && dirbuf[dlen - 1] != ':') {
            combined[dlen] = '/';
            dlen++;
        }
        memcpy(combined + dlen, meta->filename, flen + 1u);
        free(meta->filename);
        meta->filename = combined;
    } else if (dir_len > 0
        && (meta->filename == NULL || meta->filename[0] == '\0')) {
        /*
         * Level-2 -lhd- names often live only in type-0x02 / 0x45 with an
         * empty filename block.  Promote the directory path.
         */
        free(meta->filename);
        meta->filename = NULL;
        dlen = dir_len;
        while (dlen > 0 && (dirbuf[dlen - 1] == '/' || dirbuf[dlen - 1] == ':')) {
            dlen--;
        }
        if (dlen == 0) {
            return LH_ERR_BAD_HEADER;
        }
        meta->filename = (char *)malloc(dlen + 1u);
        if (!meta->filename) {
            return LH_ERR_NO_MEMORY;
        }
        memcpy(meta->filename, dirbuf, dlen);
        meta->filename[dlen] = '\0';
    }
    return LH_OK;
}

static void lh_dir_normalize_ff(char *dirbuf, size_t dir_len)
{
    size_t di;

    for (di = 0; di < dir_len; di++) {
        if (dirbuf[di] == LH_PATH_SEP) {
            dirbuf[di] = '/';
        }
    }
}

static lh_status lh_ext_set_legacy_name(
    lh_hdr_meta *meta,
    const unsigned char *payload,
    size_t payload_len)
{
    if (meta->have_utf16_name) {
        return LH_OK;
    }
    free(meta->filename);
    meta->filename = NULL;
    if (payload_len == 0) {
        return LH_OK;
    }
    meta->filename = (char *)malloc(payload_len + 1u);
    if (!meta->filename) {
        return LH_ERR_NO_MEMORY;
    }
    memcpy(meta->filename, payload, payload_len);
    meta->filename[payload_len] = '\0';
    return LH_OK;
}

static lh_status lh_ext_set_utf16_name(
    lh_hdr_meta *meta,
    const unsigned char *payload,
    size_t payload_len)
{
    char *s;
    lh_status st;

    st = lh_utf16le_to_latin1(payload, payload_len, &s);
    if (st != LH_OK) {
        return st;
    }
    free(meta->filename);
    meta->filename = s;
    meta->have_utf16_name = 1;
    return LH_OK;
}

static lh_status lh_ext_set_legacy_comment(
    lh_hdr_meta *meta,
    const unsigned char *payload,
    size_t payload_len)
{
    if (meta->have_utf16_comment) {
        return LH_OK;
    }
    free(meta->comment);
    meta->comment = NULL;
    if (payload_len == 0) {
        return LH_OK;
    }
    meta->comment = (char *)malloc(payload_len + 1u);
    if (!meta->comment) {
        return LH_ERR_NO_MEMORY;
    }
    memcpy(meta->comment, payload, payload_len);
    meta->comment[payload_len] = '\0';
    return LH_OK;
}

static lh_status lh_ext_set_utf16_comment(
    lh_hdr_meta *meta,
    const unsigned char *payload,
    size_t payload_len)
{
    char *s;
    lh_status st;

    st = lh_utf16le_to_latin1(payload, payload_len, &s);
    if (st != LH_OK) {
        return st;
    }
    free(meta->comment);
    meta->comment = s;
    meta->have_utf16_comment = 1;
    return LH_OK;
}

static lh_status lh_ext_set_legacy_dir(
    char *dirbuf,
    size_t dirbuf_size,
    size_t *dir_len,
    int have_utf16_dir,
    const unsigned char *payload,
    size_t payload_len)
{
    if (have_utf16_dir) {
        return LH_OK;
    }
    if (payload_len == 0 || payload_len >= dirbuf_size) {
        return LH_OK;
    }
    memcpy(dirbuf, payload, payload_len);
    dirbuf[payload_len] = '\0';
    *dir_len = payload_len;
    lh_dir_normalize_ff(dirbuf, *dir_len);
    return LH_OK;
}

static lh_status lh_ext_set_utf16_dir(
    lh_hdr_meta *meta,
    char *dirbuf,
    size_t dirbuf_size,
    size_t *dir_len,
    const unsigned char *payload,
    size_t payload_len)
{
    size_t got;
    lh_status st;

    st = lh_utf16le_to_latin1_buf(payload, payload_len, dirbuf, dirbuf_size, &got);
    if (st != LH_OK) {
        return st;
    }
    *dir_len = got;
    meta->have_utf16_dir = 1;
    return LH_OK;
}

/* Level-2 extensions live entirely in the header buffer (lha-114i: no fread). */
static lh_status lh_hdr_read_extensions_buf(
    lh_hdr_meta *meta,
    const unsigned char *mem,
    size_t mem_len,
    unsigned long *ext_total)
{
    size_t pos;
    unsigned short blk_len;
    unsigned char blk_type;
    size_t payload;
    char *dirbuf;
    size_t dir_len;
    unsigned long consumed;
    lh_status st;

    dirbuf = (char *)malloc(512);
    if (!dirbuf) {
        return LH_ERR_NO_MEMORY;
    }
    dirbuf[0] = '\0';
    dir_len = 0;
    pos = 0;
    consumed = 0;
    st = LH_OK;
    if (ext_total) {
        *ext_total = 0;
    }

    while (pos + 2u <= mem_len) {
        blk_len = lh_read_le16(mem + pos);
        pos += 2;
        consumed += 2;
        if (blk_len == 0) {
            break;
        }
        /* pos is at type; need blk_len-2 more bytes (type + payload). */
        if (blk_len < 3 || pos + (size_t)blk_len - 2u > mem_len) {
            st = LH_ERR_BAD_HEADER;
            break;
        }
        blk_type = mem[pos++];
        consumed += 1;
        payload = (size_t)blk_len - 3u;

        if (blk_type == LH_EXT_FILENAME) {
            st = lh_ext_set_legacy_name(meta, mem + pos, payload);
        } else if (blk_type == LH_EXT_PATH) {
            st = lh_ext_set_legacy_dir(dirbuf, 512, &dir_len,
                meta->have_utf16_dir, mem + pos, payload);
        } else if (blk_type == LH_EXT_COMMENT) {
            st = lh_ext_set_legacy_comment(meta, mem + pos, payload);
        } else if (blk_type == LH_EXT_UTF16_FILENAME) {
            st = lh_ext_set_utf16_name(meta, mem + pos, payload);
        } else if (blk_type == LH_EXT_UTF16_PATH) {
            st = lh_ext_set_utf16_dir(meta, dirbuf, 512, &dir_len,
                mem + pos, payload);
        } else if (blk_type == LH_EXT_UTF16_COMMENT) {
            st = lh_ext_set_utf16_comment(meta, mem + pos, payload);
        } else if (blk_type == LH_EXT_SNAPSHOT) {
            st = lh_ext_set_snapshot(meta, mem + pos, payload);
        } else {
            st = LH_OK;
        }
        if (st != LH_OK) {
            break;
        }
        /* 0x41 Windows FILETIME and other types: skip payload. */
        pos += payload;
        consumed += payload;
    }

    if (st == LH_OK) {
        st = lh_ext_combine_dir_file(meta, dirbuf, dir_len);
    }
    if (st == LH_OK && ext_total) {
        *ext_total = consumed;
    }
    free(dirbuf);
    return st;
}

static int lh_mf_skip(
    lh_stream *io,
    const unsigned char *mem,
    size_t mem_len,
    size_t *mem_pos,
    size_t n)
{
    unsigned char *scratch;
    size_t chunk;
    int ok;

    scratch = (unsigned char *)malloc(256);
    if (!scratch) {
        return 0;
    }
    ok = 1;
    while (n > 0) {
        chunk = n;
        if (chunk > 256u) {
            chunk = 256u;
        }
        if (!lh_mf_read(io, mem, mem_len, mem_pos, scratch, chunk)) {
            ok = 0;
            break;
        }
        n -= chunk;
    }
    free(scratch);
    return ok;
}

static lh_status lh_hdr_read_extensions(
    lh_stream *io,
    lh_hdr_meta *meta,
    const unsigned char *mem,
    size_t mem_len,
    unsigned long *ext_total)
{
    unsigned short blk_len;
    unsigned char blk_type;
    size_t payload;
    size_t mem_pos;
    char *dirbuf;
    size_t dir_len;
    unsigned long consumed;
    lh_status st;
    unsigned char *tmp;
    size_t left;
    size_t chunk;
    unsigned char *scratch;

    dirbuf = (char *)malloc(512);
    scratch = (unsigned char *)malloc(256);
    if (!dirbuf || !scratch) {
        free(dirbuf);
        free(scratch);
        return LH_ERR_NO_MEMORY;
    }
    dirbuf[0] = '\0';
    dir_len = 0;
    consumed = 0;
    mem_pos = 0;
    st = LH_OK;
    if (ext_total) {
        *ext_total = 0;
    }

    for (;;) {
        if (!lh_mf_read(io, mem, mem_len, &mem_pos, &blk_len, 2)) {
            st = LH_ERR_TRUNCATED;
            break;
        }
        consumed += 2;
        if (blk_len == 0) {
            break;
        }
        if (blk_len < 3) {
            st = LH_ERR_BAD_HEADER;
            break;
        }
        payload = (size_t)blk_len - 3u;
        if (!lh_mf_read(io, mem, mem_len, &mem_pos, &blk_type, 1)) {
            st = LH_ERR_TRUNCATED;
            break;
        }
        consumed += 1;

        if (blk_type == LH_EXT_FILENAME
            || blk_type == LH_EXT_COMMENT
            || blk_type == LH_EXT_UTF16_FILENAME
            || blk_type == LH_EXT_UTF16_COMMENT
            || blk_type == LH_EXT_PATH
            || blk_type == LH_EXT_UTF16_PATH
            || blk_type == LH_EXT_SNAPSHOT) {
            tmp = NULL;
            if (payload > 0) {
                tmp = (unsigned char *)malloc(payload);
                if (!tmp) {
                    st = LH_ERR_NO_MEMORY;
                    break;
                }
                if (!lh_mf_read(io, mem, mem_len, &mem_pos, tmp, payload)) {
                    free(tmp);
                    st = LH_ERR_TRUNCATED;
                    break;
                }
            }
            if (blk_type == LH_EXT_FILENAME) {
                st = lh_ext_set_legacy_name(meta, tmp, payload);
            } else if (blk_type == LH_EXT_PATH) {
                st = lh_ext_set_legacy_dir(dirbuf, 512, &dir_len,
                    meta->have_utf16_dir, tmp, payload);
            } else if (blk_type == LH_EXT_COMMENT) {
                st = lh_ext_set_legacy_comment(meta, tmp, payload);
            } else if (blk_type == LH_EXT_UTF16_FILENAME) {
                st = lh_ext_set_utf16_name(meta, tmp, payload);
            } else if (blk_type == LH_EXT_UTF16_PATH) {
                st = lh_ext_set_utf16_dir(meta, dirbuf, 512,
                    &dir_len, tmp, payload);
            } else if (blk_type == LH_EXT_UTF16_COMMENT) {
                st = lh_ext_set_utf16_comment(meta, tmp, payload);
            } else {
                st = lh_ext_set_snapshot(meta, tmp, payload);
            }
            free(tmp);
            if (st != LH_OK) {
                break;
            }
        } else if (payload > 0) {
            /* FILETIME 0x41 and unknown: skip. */
            left = payload;
            while (left > 0) {
                chunk = left;
                if (chunk > 256u) {
                    chunk = 256u;
                }
                if (!lh_mf_read(io, mem, mem_len, &mem_pos, scratch, chunk)) {
                    st = LH_ERR_TRUNCATED;
                    break;
                }
                left -= chunk;
            }
            if (st != LH_OK) {
                break;
            }
        }
        consumed += payload;
    }

    if (st == LH_OK) {
        st = lh_ext_combine_dir_file(meta, dirbuf, dir_len);
    }
    if (st == LH_OK && ext_total) {
        *ext_total = consumed;
    }
    free(dirbuf);
    free(scratch);
    return st;
}

static lh_status lh_hdr_read_level2(lh_stream *io, lh_hdr_meta *meta, unsigned char *buf)
{
    int header_size;
    unsigned long ext_total;
    lh_status st;
    int padding;
    size_t mem_len;

    (void)io;

    header_size = (int)buf[0] + (int)((unsigned int)buf[1] << 8);
    if (header_size < 26) {
        return LH_ERR_BAD_HEADER;
    }

    memcpy(meta->method_sig, buf + LH_I_METHOD, LH_SIG_LEN);
    meta->method_sig[LH_SIG_LEN] = '\0';
    meta->method = lh_method_from_string(meta->method_sig);
    meta->is_directory = (meta->method == LH_METHOD_LHD);

    meta->packed_size = lh_read_le32(buf + LH_I_PACKED);
    meta->original_size = lh_read_le32(buf + LH_I_ORIGINAL);
    meta->timestamp = lh_read_le32(buf + LH_I_STAMP);
    meta->attribute = buf[LH_I_ATTR];
    meta->header_level = 2;
    meta->has_crc = 1;
    meta->crc = lh_read_le16(buf + 21);
    meta->os_id = buf[23];

    /* Level-2 extension chain is wholly inside the header buffer. */
    mem_len = 0;
    if ((size_t)header_size > 24u) {
        mem_len = (size_t)header_size - 24u;
    }
    ext_total = 0;
    st = lh_hdr_read_extensions_buf(meta, buf + 24, mem_len, &ext_total);
    if (st != LH_OK) {
        return st;
    }

    /*
     * Trailing pad after the extension terminator is already in buf (we
     * slurped header_size bytes).  Do not read it from io — that would steal
     * packed-data bytes and desync the next member.  Classic LHA allows 0 or
     * 1 pad byte; accept a little more for defensive writers.
     */
    if (ext_total > mem_len) {
        return LH_ERR_BAD_HEADER;
    }
    padding = (int)(mem_len - ext_total);
    if (padding < 0 || padding > 20) {
        return LH_ERR_BAD_HEADER;
    }
    if (!meta->filename) {
        return LH_ERR_BAD_HEADER;
    }
    return LH_OK;
}

static lh_status lh_hdr_read_lhex(lh_stream *io, lh_hdr_meta *meta, unsigned char *data)
{
    int header_size;
    int name_length;
    int checksum;
    int header_level;
    int extend_type;
    int i;

    header_size = (int)data[0];
    name_length = 0;
    extend_type = 0;

    lh_setup_get(data + LH_I_CHECKSUM);
    checksum = (int)lh_get_byte();

    memcpy(meta->method_sig, data + LH_I_METHOD, LH_SIG_LEN);
    meta->method_sig[LH_SIG_LEN] = '\0';
    meta->method = lh_method_from_string(meta->method_sig);
    meta->is_directory = (meta->method == LH_METHOD_LHD);

    lh_setup_get(data + LH_I_PACKED);
    meta->packed_size = (unsigned long)lh_get_longword();
    meta->original_size = (unsigned long)lh_get_longword();
    meta->timestamp = (unsigned long)lh_get_longword();
    meta->attribute = (unsigned char)lh_get_byte();

    header_level = (int)lh_get_byte();
    meta->header_level = (unsigned char)header_level;

    if (header_level != 2) {
        if (lh_hdr_checksum(data + LH_I_METHOD, (size_t)header_size) != (unsigned char)checksum) {
            /* LhA warns and continues; do not fail the archive. */
        }
        name_length = (int)lh_get_byte();
        if (name_length > 0) {
            meta->filename = (char *)malloc((size_t)name_length + 1u);
            if (!meta->filename) {
                return LH_ERR_NO_MEMORY;
            }
            for (i = 0; i < name_length; i++) {
                meta->filename[i] = (char)lh_get_byte();
            }
            meta->filename[name_length] = '\0';
        }
    }

    if (header_size - name_length >= 24) {
        meta->has_crc = 1;
        meta->crc = lh_get_word();
        extend_type = (int)lh_get_byte();
        meta->os_id = (unsigned char)extend_type;
    } else if (header_size - name_length == 22) {
        meta->has_crc = 1;
        meta->crc = lh_get_word();
        extend_type = 0;
        meta->os_id = 0;
    } else if (header_size - name_length == 20) {
        meta->has_crc = 0;
        extend_type = 0;
        meta->os_id = 0;
    } else {
        return LH_ERR_BAD_HEADER;
    }

    if (extend_type == (int)'U' && header_level == 0) {
        (void)lh_get_byte();
        (void)lh_get_longword();
        (void)lh_get_word();
        (void)lh_get_word();
        (void)lh_get_word();
        return LH_OK;
    }

    if (header_level == 1) {
        int extend_size;

        extend_size = header_size - name_length - 25;
        while (extend_size-- > 0) {
            (void)lh_get_byte();
        }
    }

    if (header_level > 0) {
        unsigned char *ext_ptr;
        unsigned char *ext_lim;
        int blk_len;
        unsigned char blk_type;
        char *dirbuf;
        size_t dir_len;
        unsigned char *tmp;
        size_t plen;
        size_t j;
        lh_status est;

        dirbuf = (char *)malloc(512);
        if (!dirbuf) {
            return LH_ERR_NO_MEMORY;
        }
        dirbuf[0] = '\0';
        dir_len = 0;
        est = LH_OK;
        if (header_level != 2) {
            lh_setup_get(data + header_size);
            ext_ptr = lh_get_ptr;
            ext_lim = data + LH_HDR_BUF;
            while ((blk_len = (int)lh_get_word()) != 0) {
                if ((size_t)(ext_lim - lh_get_ptr) < (size_t)blk_len) {
                    est = LH_ERR_BAD_HEADER;
                    break;
                }
                if (lh_stream_read(io, lh_get_ptr, (size_t)blk_len) != (size_t)blk_len) {
                    est = LH_ERR_TRUNCATED;
                    break;
                }
                blk_type = (unsigned char)lh_get_byte();
                plen = 0;
                if (blk_len > 3) {
                    plen = (size_t)blk_len - 3u;
                }
                if (blk_type == LH_EXT_FILENAME
                    || blk_type == LH_EXT_PATH
                    || blk_type == LH_EXT_COMMENT
                    || blk_type == LH_EXT_UTF16_FILENAME
                    || blk_type == LH_EXT_UTF16_PATH
                    || blk_type == LH_EXT_UTF16_COMMENT
                    || blk_type == LH_EXT_SNAPSHOT) {
                    tmp = NULL;
                    if (plen > 0) {
                        tmp = (unsigned char *)malloc(plen);
                        if (!tmp) {
                            est = LH_ERR_NO_MEMORY;
                            break;
                        }
                        for (j = 0; j < plen; j++) {
                            tmp[j] = (unsigned char)lh_get_byte();
                        }
                    }
                    if (blk_type == LH_EXT_FILENAME) {
                        est = lh_ext_set_legacy_name(meta, tmp, plen);
                    } else if (blk_type == LH_EXT_PATH) {
                        est = lh_ext_set_legacy_dir(dirbuf, 512,
                            &dir_len, meta->have_utf16_dir, tmp, plen);
                    } else if (blk_type == LH_EXT_COMMENT) {
                        est = lh_ext_set_legacy_comment(meta, tmp, plen);
                    } else if (blk_type == LH_EXT_UTF16_FILENAME) {
                        est = lh_ext_set_utf16_name(meta, tmp, plen);
                    } else if (blk_type == LH_EXT_UTF16_PATH) {
                        est = lh_ext_set_utf16_dir(meta, dirbuf,
                            512, &dir_len, tmp, plen);
                    } else if (blk_type == LH_EXT_UTF16_COMMENT) {
                        est = lh_ext_set_utf16_comment(meta, tmp, plen);
                    } else {
                        est = lh_ext_set_snapshot(meta, tmp, plen);
                    }
                    free(tmp);
                    if (est != LH_OK) {
                        break;
                    }
                } else if (blk_type == 0x50) {
                    if (extend_type == (int)'U') {
                        (void)lh_get_word();
                    } else {
                        lh_setup_get(lh_get_ptr + plen);
                    }
                } else if (blk_type == 0x51) {
                    if (extend_type == (int)'U') {
                        (void)lh_get_word();
                        (void)lh_get_word();
                    } else {
                        lh_setup_get(lh_get_ptr + plen);
                    }
                } else if (blk_type == 0x54) {
                    if (extend_type == (int)'U') {
                        (void)lh_get_longword();
                    } else {
                        lh_setup_get(lh_get_ptr + plen);
                    }
                } else {
                    lh_setup_get(lh_get_ptr + plen);
                }
            }
            if (est == LH_OK && lh_get_ptr - ext_ptr != 2) {
                meta->packed_size -= (unsigned long)(lh_get_ptr - ext_ptr - 2);
            }
        }
        if (est == LH_OK) {
            est = lh_ext_combine_dir_file(meta, dirbuf, dir_len);
        }
        free(dirbuf);
        if (est != LH_OK) {
            return est;
        }
    }

    return LH_OK;
}

lh_status lh_hdr_read(lh_stream *io, lh_hdr_meta *meta, unsigned char write_level)
{
    unsigned char *buf;
    unsigned char first;
    int header_size;
    int header_level;
    size_t n;
    lh_status st;

    (void)write_level;

    if (!io || !meta) {
        return LH_ERR_INVALID_ARG;
    }
    lh_hdr_meta_clear(meta);
    buf = (unsigned char *)malloc(LH_HDR_BUF);
    if (!buf) {
        return LH_ERR_NO_MEMORY;
    }
    memset(buf, 0, LH_HDR_BUF);

    /*
     * Classic LHA (lha-1 get_header): read a 21-byte common prefix so the
     * level byte at offset 20 is always available, then branch.  Level-2
     * sizes are 16-bit LE in bytes 0..1; treating byte0 as a level-0 size
     * rejects Snapshot headers whose size low byte is 1..20 (LhErr 224).
     */
    n = lh_stream_read(io, &first, 1);
    if (n != 1) {
        free(buf);
        return LH_ERR_TRUNCATED;
    }
    if (first == 0) {
        free(buf);
        return LH_ERR_TRUNCATED;
    }
    buf[0] = first;
    n = lh_stream_read(io, buf + 1, 20);
    if (n != 20) {
        free(buf);
        return LH_ERR_TRUNCATED;
    }
    header_level = (int)buf[LH_I_LEVEL];

    if (header_level == 2) {
        header_size = (int)buf[0] + (int)((unsigned int)buf[1] << 8);
        if (header_size < 26 || header_size >= LH_HDR_BUF - 4) {
            free(buf);
            return LH_ERR_BAD_HEADER;
        }
        if (header_size > 21) {
            n = lh_stream_read(io, buf + 21,
                (size_t)header_size - 21u);
            if (n != (size_t)header_size - 21u) {
                free(buf);
                return LH_ERR_TRUNCATED;
            }
        }
    } else {
        header_size = (int)first;
        if (header_size < 20 || header_size >= LH_HDR_BUF - 4) {
            free(buf);
            return LH_ERR_BAD_HEADER;
        }
        /*
         * Common prefix already consumed 21 bytes.  Level-0/1 headers shorter
         * than that are not produced by real LHA writers; reject rather than
         * invent a seek-back for the over-read.
         */
        if (header_size < 21) {
            free(buf);
            return LH_ERR_BAD_HEADER;
        }
        if (header_size > 21) {
            n = lh_stream_read(io, buf + 21,
                (size_t)header_size - 21u);
            if (n != (size_t)header_size - 21u) {
                free(buf);
                return LH_ERR_TRUNCATED;
            }
        }
        /* Prefetch between counted header body and packed data (lhex). */
        if (lh_stream_read(io, buf + header_size, 2) != 2) {
            free(buf);
            return LH_ERR_TRUNCATED;
        }
    }

    /*
     * Method IDs are five ASCII bytes: "-xxx-".  Amiga LhA uses -lhN- / -lhd-;
     * LArc uses -lzN-; PMarc uses -pmN-.  Do not require "-lh" only.
     */
    if (buf[LH_I_METHOD] != '-' || buf[LH_I_METHOD + 4] != '-') {
        free(buf);
        return LH_ERR_BAD_HEADER;
    }

    if (header_level == 2) {
        st = lh_hdr_read_level2(io, meta, buf);
        lh_hdr_debug("read", io, meta, st);
        free(buf);
        return st;
    }

    st = lh_hdr_read_lhex(io, meta, buf);
    lh_hdr_debug("read", io, meta, st);
    free(buf);
    return st;
}

/*
 * Append one extension block: LE16 length (3+payload), type, payload.
 * Returns bytes written into dest, or 0 on overflow.
 */
static size_t lh_hdr_put_ext(
    unsigned char *dest,
    size_t dest_cap,
    size_t pos,
    unsigned char type,
    const unsigned char *payload,
    size_t payload_len)
{
    size_t need;

    need = 3u + payload_len;
    if (pos + need > dest_cap) {
        return 0;
    }
    lh_write_le16(dest + pos, (lh_u16)need);
    dest[pos + 2] = type;
    if (payload_len > 0 && payload) {
        memcpy(dest + pos + 3, payload, payload_len);
    }
    return need;
}

/*
 * Split "LIBS/foo.library" into basename and LHA type-2 directory path
 * (components separated by 0xFF, trailing 0xFF).
 */
static void lh_hdr_split_path(
    const char *full,
    char *basename,
    size_t basename_cap,
    unsigned char *dirpath,
    size_t dirpath_cap,
    size_t *dirpath_len)
{
    const char *slash;
    const char *p;
    size_t dlen;
    size_t i;

    *dirpath_len = 0;
    basename[0] = '\0';
    if (!full || !full[0]) {
        return;
    }
    slash = strrchr(full, '/');
    if (!slash) {
        slash = strrchr(full, ':');
        if (slash) {
            /* Device-only prefix: keep whole name as basename. */
            slash = NULL;
        }
    }
    if (!slash) {
        strncpy(basename, full, basename_cap - 1);
        basename[basename_cap - 1] = '\0';
        return;
    }
    dlen = (size_t)(slash - full);
    if (dlen + 1u > dirpath_cap) {
        dlen = dirpath_cap - 1u;
    }
    for (i = 0; i < dlen; i++) {
        if (full[i] == '/') {
            dirpath[i] = (unsigned char)LH_PATH_SEP;
        } else {
            dirpath[i] = (unsigned char)full[i];
        }
    }
    /* Trailing path separator is conventional for type-2 directories. */
    if (dlen > 0 && dirpath[dlen - 1] != (unsigned char)LH_PATH_SEP
        && dlen + 1u < dirpath_cap) {
        dirpath[dlen++] = (unsigned char)LH_PATH_SEP;
    }
    *dirpath_len = dlen;
    p = slash + 1;
    strncpy(basename, p, basename_cap - 1);
    basename[basename_cap - 1] = '\0';
}

lh_status lh_hdr_write(lh_stream *io, const lh_hdr_meta *meta, unsigned char write_level)
{
    unsigned char *buf;
    char *basename;
    unsigned char *dirpath;
    size_t dirpath_len;
    size_t name_len;
    size_t comment_len;
    size_t hdr_body;
    size_t pos;
    size_t n;
    unsigned char checksum;
    size_t i;
    const char *write_name;
    lh_status st;
    size_t hcrc_pos;
    unsigned short hcrc;
    unsigned char os_id;
    unsigned char *u16;
    size_t u16_len;
    unsigned char *cpay;
    size_t cpay_len;

    buf = NULL;
    basename = NULL;
    dirpath = NULL;
    st = LH_OK;

    if (!io || !LH_STREAM_OPEN(io) || !meta || !meta->filename) {
        return LH_ERR_INVALID_ARG;
    }

    buf = (unsigned char *)malloc(LH_HDR_BUF);
    basename = (char *)malloc(256);
    dirpath = (unsigned char *)malloc(512);
    if (!buf || !basename || !dirpath) {
        st = LH_ERR_NO_MEMORY;
        goto done;
    }

    /*
     * Level 2: 16-bit header size, Unix timestamp, file CRC at 21, OS at 23,
     * then extensions.  Spec requires a type-0 common header carrying the
     * header CRC-16; without it classic/Unix LHA reports header CRC errors.
     * Offset 19 is reserved (0x20) on Unix; Amiga OS ID keeps protection bits
     * there so SetProtection round-trips.
     */
    if (write_level >= 2) {
        lh_hdr_split_path(meta->filename, basename, 256,
            dirpath, 512, &dirpath_len);
        if (!basename[0]) {
            st = LH_ERR_INVALID_ARG;
            goto done;
        }
        name_len = strlen(basename);
        comment_len = (meta->comment && meta->comment[0])
            ? strlen(meta->comment) : 0;
        os_id = meta->os_id ? meta->os_id : LH_OS_AMIGA;

        memset(buf, 0, LH_HDR_BUF);
        memcpy(buf + LH_I_METHOD, meta->method_sig, LH_SIG_LEN);
        lh_write_le32(buf + LH_I_PACKED, meta->packed_size);
        lh_write_le32(buf + LH_I_ORIGINAL, meta->original_size);
        lh_write_le32(buf + LH_I_STAMP, meta->timestamp);
        if (os_id == LH_OS_AMIGA) {
            buf[LH_I_ATTR] = meta->attribute;
        } else {
            buf[LH_I_ATTR] = 0x20;
        }
        buf[LH_I_LEVEL] = 2;
        lh_write_le16(buf + 21, meta->crc);
        buf[23] = os_id;
        pos = 24;

        /* Common header first: size 5, type 0, CRC placeholder. */
        n = lh_hdr_put_ext(buf, LH_HDR_BUF, pos, LH_EXT_COMMON, NULL, 2);
        if (n == 0) {
            st = LH_ERR_BAD_HEADER;
            goto done;
        }
        hcrc_pos = pos + 3;
        lh_write_le16(buf + hcrc_pos, 0);
        pos += n;

        n = lh_hdr_put_ext(buf, LH_HDR_BUF, pos, LH_EXT_FILENAME,
            (const unsigned char *)basename, name_len);
        if (n == 0) {
            st = LH_ERR_FILENAME_TOO_LONG;
            goto done;
        }
        pos += n;

        if (dirpath_len > 0) {
            n = lh_hdr_put_ext(buf, LH_HDR_BUF, pos, LH_EXT_PATH,
                dirpath, dirpath_len);
            if (n == 0) {
                st = LH_ERR_FILENAME_TOO_LONG;
                goto done;
            }
            pos += n;
        }

        if (comment_len > 0) {
            n = lh_hdr_put_ext(buf, LH_HDR_BUF, pos, LH_EXT_COMMENT,
                (const unsigned char *)meta->comment, comment_len);
            if (n == 0) {
                st = LH_ERR_BAD_HEADER;
                goto done;
            }
            pos += n;
        }

        /*
         * Unlha32-compatible Unicode mirrors so Windows tools see UTF-16LE
         * while legacy readers keep the Latin-1 0x01/0x02/0x3F fallback.
         */
        u16 = NULL;
        u16_len = 0;
        st = lh_latin1_to_utf16le(basename, &u16, &u16_len);
        if (st != LH_OK) {
            goto done;
        }
        n = lh_hdr_put_ext(buf, LH_HDR_BUF, pos, LH_EXT_UTF16_FILENAME,
            u16, u16_len);
        free(u16);
        if (n == 0) {
            st = LH_ERR_FILENAME_TOO_LONG;
            goto done;
        }
        pos += n;

        if (dirpath_len > 0) {
            u16 = NULL;
            u16_len = 0;
            st = lh_latin1_path_to_utf16le(dirpath, dirpath_len,
                &u16, &u16_len);
            if (st != LH_OK) {
                goto done;
            }
            n = lh_hdr_put_ext(buf, LH_HDR_BUF, pos, LH_EXT_UTF16_PATH,
                u16, u16_len);
            free(u16);
            if (n == 0) {
                st = LH_ERR_FILENAME_TOO_LONG;
                goto done;
            }
            pos += n;
        }

        if (comment_len > 0) {
            u16 = NULL;
            u16_len = 0;
            st = lh_latin1_to_utf16le(meta->comment, &u16, &u16_len);
            if (st != LH_OK) {
                goto done;
            }
            n = lh_hdr_put_ext(buf, LH_HDR_BUF, pos, LH_EXT_UTF16_COMMENT,
                u16, u16_len);
            free(u16);
            if (n == 0) {
                st = LH_ERR_BAD_HEADER;
                goto done;
            }
            pos += n;
        }

        if (meta->have_snapshot) {
            cpay = NULL;
            cpay_len = 0;
            st = lh_snapshot_pack(&meta->snapshot, &cpay, &cpay_len);
            if (st != LH_OK) {
                goto done;
            }
            n = lh_hdr_put_ext(buf, LH_HDR_BUF, pos, LH_EXT_SNAPSHOT,
                cpay, cpay_len);
            free(cpay);
            if (n == 0) {
                st = LH_ERR_BAD_HEADER;
                goto done;
            }
            pos += n;
        }

        /* Terminate extension chain. */
        if (pos + 2u > LH_HDR_BUF) {
            st = LH_ERR_BAD_HEADER;
            goto done;
        }
        lh_write_le16(buf + pos, 0);
        pos += 2;

        /*
         * Level-2 size low byte must not be 0 (looks like end-of-archive).
         * Pad one byte when needed, as classic LHA does.
         */
        if ((pos & 0xffu) == 0) {
            if (pos + 1u > LH_HDR_BUF) {
                st = LH_ERR_BAD_HEADER;
                goto done;
            }
            buf[pos++] = 0;
        }

        lh_write_le16(buf, (lh_u16)pos);
        hcrc = lh_crc16(buf, pos);
        lh_write_le16(buf + hcrc_pos, hcrc);

        if (lh_stream_write(io, buf, pos) != pos) {
            st = LH_ERR_IO;
            goto done;
        }
        st = LH_OK;
        goto done;
    }

    /* Level 0/1: classic lhex layout with inline name (no comment ext). */
    write_name = meta->filename;
    name_len = strlen(write_name);
    if (name_len > 200) {
        st = LH_ERR_FILENAME_TOO_LONG;
        goto done;
    }
    memset(buf, 0, LH_HDR_BUF);
    memcpy(buf + 2, meta->method_sig, LH_SIG_LEN);
    lh_write_le32(buf + 7, meta->packed_size);
    lh_write_le32(buf + 11, meta->original_size);
    lh_write_le32(buf + 15, meta->timestamp);
    buf[19] = meta->attribute;
    buf[20] = write_level;
    buf[21] = (unsigned char)name_len;
    memcpy(buf + 22, write_name, name_len);
    lh_write_le16(buf + 22 + name_len, meta->crc);
    buf[22 + name_len + 2] = meta->os_id ? meta->os_id : LH_OS_AMIGA;
    if (write_level >= 1) {
        lh_write_le16(buf + 22 + name_len + 3, 0);
        hdr_body = 22 + name_len + 5;
    } else {
        hdr_body = 22 + name_len + 2;
    }
    if (hdr_body > 255) {
        st = LH_ERR_BAD_HEADER;
        goto done;
    }
    buf[0] = (unsigned char)hdr_body;
    checksum = 0;
    for (i = 2; i < hdr_body; i++) {
        checksum = (unsigned char)(checksum + buf[i]);
    }
    buf[1] = checksum;
    /* lhex wire format: size, body[0..size-2], then 2 extension/prefetch bytes. */
    if (lh_stream_write(io, buf, 1) != 1) {
        st = LH_ERR_IO;
        goto done;
    }
    if (lh_stream_write(io, buf + 1, hdr_body - 1) != hdr_body - 1) {
        st = LH_ERR_IO;
        goto done;
    }
    if (lh_stream_write(io, buf + hdr_body, 2) != 2) {
        st = LH_ERR_IO;
        goto done;
    }
    st = LH_OK;

done:
    free(buf);
    free(basename);
    free(dirpath);
    return st;
}
