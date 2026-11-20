/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lh_lh5dec.c - LH4/LH5/LH6/LH7 decompression (static Huffman + sliding dict).
 *
 * Same block format; dictionary size / PBIT / NP differ:
 *   lh4: 4 KiB  (dicbit 12, pbit 4, np 13)
 *   lh5: 8 KiB  (dicbit 13, pbit 4, np 14)
 *   lh6: 32 KiB (dicbit 15, pbit 5, np 16)
 *   lh7: 64 KiB (dicbit 16, pbit 5, np 17)
 *   lhx: UNLHA32 (dicbit 20, pbit 5, np 31) — 1 MiB history window
 *
 * Decoder state is malloc'd — the Huffman tables alone are ~20 KiB and
 * must not live on a typical Amiga process stack.
 *
 * Hot-path tuning (portable C89, 680x0-friendly):
 *   - 32-bit LHarc bitbuf; fillbuf keeps working state in locals
 *   - peek tables store (bitlen<<16)|sym after make_table so decode
 *     needs one load instead of table + c_len / pt_len
 *   - decode straight into outBuf (history = prior output; reads before
 *     offset 0 yield 0x20).  No separate dictionary malloc or dual write.
 *   - LZ match copy with ULONG moves when distance >= 4
 */

#include "lh_internal.h"

#include <limits.h>
#include <string.h>

#define LH5_MAXMATCH      256
#define LH5_THRESHOLD     3
#define LH5_NC            (UCHAR_MAX + LH5_MAXMATCH + 2 - LH5_THRESHOLD)
#define LH5_CBIT          9
#define LH5_NT            (16 + 3)
#define LH5_TBIT          5
#define LH5_NPT           0x80

#define LH5_BITBUF_BITS 32
/* Mask so host LP64 unsigned long behaves like Amiga 32-bit ULONG. */
#define LH5_BITBUF_MASK 0xffffffffUL

#define LH5_PACK(len, sym) \
    ((((unsigned long)(len)) << 16) | (unsigned long)(unsigned short)(sym))
#define LH5_SYM(e)   ((unsigned short)(e))
#define LH5_LEN(e)   ((unsigned int)((e) >> 16))

struct lh5_dec {
    const unsigned char *in;
    unsigned long in_size;
    unsigned long in_pos;
    unsigned long compsize;
    unsigned long bitbuf;
    unsigned char subbitbuf;
    unsigned char bitcount;
    unsigned short blocksize;
    short np;
    short pbit;
    unsigned short left[2 * LH5_NC - 1];
    unsigned short right[2 * LH5_NC - 1];
    unsigned char c_len[LH5_NC];
    unsigned char pt_len[LH5_NPT];
    unsigned long c_table[4096];
    unsigned long pt_table[256];
};

static void lh5_fillbuf(struct lh5_dec *d, unsigned int n)
{
    unsigned long bb;
    unsigned int bc;
    unsigned char sb;

    if (n == 0) {
        return;
    }

    bb = d->bitbuf;
    bc = d->bitcount;
    sb = d->subbitbuf;

    while (n > bc) {
        n -= bc;
        bb = ((bb << bc) + ((unsigned long)sb >> (CHAR_BIT - bc)))
            & LH5_BITBUF_MASK;
        if (d->compsize != 0) {
            d->compsize--;
            if (d->in_pos < d->in_size) {
                sb = d->in[d->in_pos++];
            } else {
                sb = 0;
            }
        } else {
            sb = 0;
        }
        bc = CHAR_BIT;
    }
    bc -= n;
    bb = ((bb << n) + ((unsigned long)sb >> (CHAR_BIT - n)))
        & LH5_BITBUF_MASK;
    sb = (unsigned char)(sb << n);

    d->bitbuf = bb;
    d->bitcount = (unsigned char)bc;
    d->subbitbuf = sb;
}

static unsigned short lh5_getbits(struct lh5_dec *d, unsigned int n)
{
    unsigned short x;

    x = (unsigned short)(d->bitbuf >> (LH5_BITBUF_BITS - n));
    lh5_fillbuf(d, n);
    return x;
}

static void lh5_init_getbits(struct lh5_dec *d)
{
    d->bitbuf = 0;
    d->subbitbuf = 0;
    d->bitcount = 0;
    lh5_fillbuf(d, LH5_BITBUF_BITS);
}

/*
 * Classic LHa make_table.  Peek slots are unsigned long with the symbol
 * or tree-node index in the low 16 bits (high 16 clear).  lh5_pack_*
 * folds bit lengths into the high half for leaf entries afterwards.
 */
static void lh5_make_table(
    struct lh5_dec *d,
    short nchar,
    unsigned char *bitlen,
    short tablebits,
    unsigned long *table)
{
    unsigned short count[17];
    unsigned short weight[17];
    unsigned short start[17];
    unsigned short total;
    unsigned int i;
    int j;
    int k;
    int l;
    int m;
    int n;
    int avail;
    unsigned long *tp;
    unsigned short *p;
    int on_table;

    avail = nchar;
    for (i = 1; i <= 16; i++) {
        count[i] = 0;
        weight[i] = (unsigned short)(1 << (16 - i));
    }
    for (i = 0; i < (unsigned int)nchar; i++) {
        if (bitlen[i] > 16) {
            return;
        }
        count[bitlen[i]]++;
    }
    total = 0;
    for (i = 1; i <= 16; i++) {
        start[i] = total;
        total = (unsigned short)(total + weight[i] * count[i]);
    }
    if ((total & 0xffffu) != 0) {
        return;
    }
    m = 16 - tablebits;
    for (i = 1; i <= (unsigned int)tablebits; i++) {
        start[i] = (unsigned short)(start[i] >> m);
        weight[i] = (unsigned short)(weight[i] >> m);
    }
    j = (int)(start[tablebits + 1] >> m);
    k = 1 << tablebits;
    if (j != 0) {
        for (i = (unsigned int)j; i < (unsigned int)k; i++) {
            table[i] = 0;
        }
    }
    for (j = 0; j < nchar; j++) {
        k = bitlen[j];
        if (k == 0) {
            continue;
        }
        l = (int)start[k] + (int)weight[k];
        if (k <= tablebits) {
            for (i = start[k]; i < (unsigned int)l; i++) {
                table[i] = (unsigned long)(unsigned short)j;
            }
        } else {
            tp = &table[(i = start[k]) >> m];
            i <<= tablebits;
            n = k - tablebits;
            on_table = 1;
            p = 0;
            while (--n >= 0) {
                if (on_table) {
                    if (*tp == 0) {
                        if (avail >= (2 * LH5_NC - 1)) {
                            return;
                        }
                        d->right[avail] = 0;
                        d->left[avail] = 0;
                        *tp = (unsigned long)avail++;
                    }
                    if (LH5_SYM(*tp) >= (unsigned short)(2 * LH5_NC - 1)) {
                        return;
                    }
                    if (i & 0x8000u) {
                        p = &d->right[LH5_SYM(*tp)];
                    } else {
                        p = &d->left[LH5_SYM(*tp)];
                    }
                    on_table = 0;
                } else {
                    if (*p == 0) {
                        if (avail >= (2 * LH5_NC - 1)) {
                            return;
                        }
                        d->right[avail] = 0;
                        d->left[avail] = 0;
                        *p = (unsigned short)avail++;
                    }
                    if (*p >= (unsigned short)(2 * LH5_NC - 1)) {
                        return;
                    }
                    if (i & 0x8000u) {
                        p = &d->right[*p];
                    } else {
                        p = &d->left[*p];
                    }
                }
                i <<= 1;
            }
            *p = (unsigned short)j;
        }
        start[k] = (unsigned short)l;
    }
}

static void lh5_pack_c_table(struct lh5_dec *d)
{
    unsigned int i;
    unsigned short j;

    for (i = 0; i < 4096; i++) {
        j = LH5_SYM(d->c_table[i]);
        if (j < LH5_NC) {
            d->c_table[i] = LH5_PACK(d->c_len[j], j);
        }
    }
}

static void lh5_pack_pt_table(struct lh5_dec *d, short nchar)
{
    unsigned int i;
    unsigned short j;

    for (i = 0; i < 256; i++) {
        j = LH5_SYM(d->pt_table[i]);
        if (j < (unsigned short)nchar) {
            d->pt_table[i] = LH5_PACK(d->pt_len[j], j);
        }
    }
}

static void lh5_read_pt_len(
    struct lh5_dec *d,
    short nn,
    short nbit,
    short i_special)
{
    short i;
    short c;
    short n;
    unsigned long mask;

    n = (short)lh5_getbits(d, (unsigned int)nbit);
    if (n == 0) {
        c = (short)lh5_getbits(d, (unsigned int)nbit);
        for (i = 0; i < nn; i++) {
            d->pt_len[i] = 0;
        }
        for (i = 0; i < 256; i++) {
            d->pt_table[i] = LH5_PACK(0, c);
        }
    } else {
        i = 0;
        while (i < n) {
            c = (short)(d->bitbuf >> (LH5_BITBUF_BITS - 3));
            if (c == 7) {
                mask = 1UL << (LH5_BITBUF_BITS - 4);
                while (mask & d->bitbuf) {
                    mask >>= 1;
                    c++;
                }
            }
            lh5_fillbuf(d, (unsigned int)((c < 7) ? 3 : c - 3));
            d->pt_len[i++] = (unsigned char)c;
            if (i == i_special) {
                c = (short)lh5_getbits(d, 2);
                while (--c >= 0) {
                    d->pt_len[i++] = 0;
                }
            }
        }
        while (i < nn) {
            d->pt_len[i++] = 0;
        }
        lh5_make_table(d, nn, d->pt_len, 8, d->pt_table);
        lh5_pack_pt_table(d, nn);
    }
}

static void lh5_read_c_len(struct lh5_dec *d)
{
    short i;
    short c;
    short n;
    unsigned long mask;
    unsigned long e;

    n = (short)lh5_getbits(d, LH5_CBIT);
    if (n == 0) {
        c = (short)lh5_getbits(d, LH5_CBIT);
        for (i = 0; i < LH5_NC; i++) {
            d->c_len[i] = 0;
        }
        for (i = 0; i < 4096; i++) {
            d->c_table[i] = LH5_PACK(0, c);
        }
    } else {
        i = 0;
        while (i < n) {
            e = d->pt_table[d->bitbuf >> (LH5_BITBUF_BITS - 8)];
            c = (short)LH5_SYM(e);
            if (c >= LH5_NT) {
                /* Peeked 8 bits landed on a node; walk without consuming yet. */
                mask = 1UL << (LH5_BITBUF_BITS - 9);
                do {
                    if (d->bitbuf & mask) {
                        c = (short)d->right[c];
                    } else {
                        c = (short)d->left[c];
                    }
                    mask >>= 1;
                } while (c >= LH5_NT);
                lh5_fillbuf(d, d->pt_len[c]);
            } else {
                lh5_fillbuf(d, LH5_LEN(e));
            }
            if (c <= 2) {
                if (c == 0) {
                    c = 1;
                } else if (c == 1) {
                    c = (short)(lh5_getbits(d, 4) + 3);
                } else {
                    c = (short)(lh5_getbits(d, LH5_CBIT) + 20);
                }
                while (--c >= 0) {
                    d->c_len[i++] = 0;
                }
            } else {
                d->c_len[i++] = (unsigned char)(c - 2);
            }
        }
        while (i < LH5_NC) {
            d->c_len[i++] = 0;
        }
        lh5_make_table(d, LH5_NC, d->c_len, 12, d->c_table);
        lh5_pack_c_table(d);
    }
}

static unsigned short lh5_decode_c(struct lh5_dec *d)
{
    unsigned long e;
    unsigned short j;
    unsigned long mask;

    if (d->blocksize == 0) {
        d->blocksize = lh5_getbits(d, 16);
        lh5_read_pt_len(d, LH5_NT, LH5_TBIT, 3);
        lh5_read_c_len(d);
        lh5_read_pt_len(d, d->np, d->pbit, -1);
    }
    d->blocksize--;
    e = d->c_table[d->bitbuf >> (LH5_BITBUF_BITS - 12)];
    j = LH5_SYM(e);
    if (j < LH5_NC) {
        lh5_fillbuf(d, LH5_LEN(e));
    } else {
        lh5_fillbuf(d, 12);
        mask = 1UL << (LH5_BITBUF_BITS - 1);
        do {
            if (d->bitbuf & mask) {
                j = d->right[j];
            } else {
                j = d->left[j];
            }
            mask >>= 1;
        } while (j >= LH5_NC);
        lh5_fillbuf(d, d->c_len[j]);
    }
    return j;
}

static unsigned short lh5_decode_p(struct lh5_dec *d)
{
    unsigned long e;
    unsigned short j;
    unsigned long mask;
    unsigned int nbits;

    e = d->pt_table[d->bitbuf >> (LH5_BITBUF_BITS - 8)];
    j = LH5_SYM(e);
    if (j < (unsigned short)d->np) {
        lh5_fillbuf(d, LH5_LEN(e));
    } else {
        lh5_fillbuf(d, 8);
        mask = 1UL << (LH5_BITBUF_BITS - 1);
        do {
            if (d->bitbuf & mask) {
                j = d->right[j];
            } else {
                j = d->left[j];
            }
            mask >>= 1;
        } while (j >= (unsigned short)d->np);
        lh5_fillbuf(d, d->pt_len[j]);
    }
    if (j != 0) {
        nbits = (unsigned int)(j - 1);
        j = (unsigned short)((1U << nbits) + lh5_getbits(d, nbits));
    }
    return j;
}

/*
 * Expand one LZ match into out[].  History is prior output; bytes before
 * offset 0 are the classic space (0x20) dictionary fill.  dist is the
 * LHA position code (0 = previous byte).
 */
static void lh5_copy_match(
    unsigned char *out,
    unsigned long *count_io,
    unsigned long outSize,
    unsigned long dist,
    unsigned long len)
{
    unsigned long count;
    unsigned long rem;
    unsigned long chunk;
    unsigned long d;
    long src;
    unsigned char *dst;
    unsigned char *sp;
    unsigned char b;
    unsigned long v;

    count = *count_io;
    d = dist + 1;

    while (len != 0 && count < outSize) {
        rem = outSize - count;
        chunk = len;
        if (chunk > rem) {
            chunk = rem;
        }

        src = (long)count - (long)d;
        dst = out + count;

        if (src < 0) {
            while (chunk != 0 && src < 0) {
                *dst++ = 0x20;
                src++;
                chunk--;
                len--;
                count++;
            }
            if (chunk == 0) {
                continue;
            }
        }

        sp = out + (unsigned long)src;

        if (d >= 4 && chunk >= 4) {
            while (chunk != 0
                && (((unsigned long)sp | (unsigned long)dst) & 3UL) != 0)
            {
                b = *sp++;
                *dst++ = b;
                chunk--;
                len--;
                count++;
            }
            while (chunk >= 4) {
                v = *(unsigned long *)sp;
                *(unsigned long *)dst = v;
                sp += 4;
                dst += 4;
                chunk -= 4;
                len -= 4;
                count += 4;
            }
        }
        while (chunk != 0) {
            b = *sp++;
            *dst++ = b;
            chunk--;
            len--;
            count++;
        }
    }

    *count_io = count;
}

/*
 * Shared LH4..LH7 / LHX decoder.  dicbit selects window size (2^dicbit);
 * pbit is the bit width used when reading the position-tree length count;
 * np is the number of position codes (usually dicbit+1; LHX uses 31).
 *
 * Match distance is still bounded by the method window; output goes
 * straight to outBuf (no side ring).
 */
static long lh_decompress_lh_new(
    void *inBuf,
    unsigned long inSize,
    void *outBuf,
    unsigned long outSize,
    unsigned int dicbit,
    unsigned int pbit,
    unsigned int np)
{
    struct lh5_dec *dec;
    unsigned char *out;
    unsigned long dicsiz;
    unsigned long count;
    unsigned long c;
    unsigned long j;
    unsigned long dist;
    int offset;

    if (!inBuf || !outBuf || inSize == 0 || outSize == 0) {
        return LH_ERR_INVALID_ARG;
    }
    if (dicbit < 12 || dicbit > 20 || pbit < 4 || pbit > 5) {
        return LH_ERR_INVALID_ARG;
    }
    if (np == 0 || np > LH5_NPT) {
        return LH_ERR_INVALID_ARG;
    }

    dicsiz = 1UL << dicbit;

    dec = (struct lh5_dec *)malloc(sizeof(*dec));
    if (!dec) {
        return LH_ERR_NO_MEMORY;
    }

    memset(dec, 0, sizeof(*dec));
    dec->in = (const unsigned char *)inBuf;
    dec->in_size = inSize;
    dec->in_pos = 0;
    dec->compsize = inSize;
    dec->blocksize = 0;
    dec->np = (short)np;
    dec->pbit = (short)pbit;
    lh5_init_getbits(dec);

    offset = 0x100 - LH5_THRESHOLD;
    count = 0;
    out = (unsigned char *)outBuf;

    while (count < outSize) {
        c = lh5_decode_c(dec);
        if (c <= UCHAR_MAX) {
            out[count++] = (unsigned char)c;
        } else {
            j = c - (unsigned long)offset;
            dist = lh5_decode_p(dec);
            if (dist >= dicsiz) {
                free(dec);
                return LH_ERR_INVALID_ARCHIVE;
            }
            lh5_copy_match(out, &count, outSize, dist, j);
        }
    }

    free(dec);
    return LH_OK;
}

long lh_decompress_lh4(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    return lh_decompress_lh_new(inBuf, inSize, outBuf, outSize, 12, 4, 13);
}

long lh_decompress_lh5(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    return lh_decompress_lh_new(inBuf, inSize, outBuf, outSize, 13, 4, 14);
}

long lh_decompress_lh6(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    return lh_decompress_lh_new(inBuf, inSize, outBuf, outSize, 15, 5, 16);
}

long lh_decompress_lh7(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    return lh_decompress_lh_new(inBuf, inSize, outBuf, outSize, 16, 5, 17);
}

long lh_decompress_lhx(void *inBuf, unsigned long inSize, void *outBuf, unsigned long outSize)
{
    /* UNLHA32 -lhx-: 1 MiB window, OFFSET_BITS=5 → up to 31 position codes. */
    return lh_decompress_lh_new(inBuf, inSize, outBuf, outSize, 20, 5, 31);
}
