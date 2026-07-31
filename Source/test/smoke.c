/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * smoke.c - Host smoke tests for liblh.a
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lh.h"

static int fail_count;

static void expect_ok(const char *label, lh_status st)
{
    if (st != LH_OK) {
        fprintf(stderr, "FAIL %s: %s\n", label, lh_status_string(st));
        fail_count++;
    }
}

static void test_codec_roundtrip(void)
{
    unsigned char *plain;
    unsigned char out[4096];
    unsigned char back[2048];
    size_t out_len;
    size_t plain_len;
    lh_status st;
    size_t i;

    /* Real LH5 needs enough input to beat Huffman table overhead. */
    plain_len = 2048;
    plain = (unsigned char *)malloc(plain_len);
    if (plain == NULL) {
        fprintf(stderr, "FAIL lh5 AllocMem\n");
        fail_count++;
        return;
    }
    for (i = 0; i < plain_len; i++) {
        plain[i] = (unsigned char)(0x55);
    }

    out_len = sizeof(out);
    st = lh_compress(LH_METHOD_LH5, plain, plain_len, out, &out_len);
    expect_ok("lh5 compress", st);
    memset(back, 0, sizeof(back));
    st = lh_decompress(LH_METHOD_LH5, out, out_len, plain_len, back,
        plain_len);
    expect_ok("lh5 decompress", st);
    if (st == LH_OK && memcmp(plain, back, plain_len) != 0) {
        fprintf(stderr, "FAIL lh5 data mismatch\n");
        fail_count++;
    } else if (st == LH_OK) {
        puts("lh5: roundtrip ok");
    }
    free(plain);

    {
        const char *short_plain = "LhASsA smoke test payload for lh0.";

        out_len = sizeof(out);
        st = lh_compress(LH_METHOD_LH0,
            (const unsigned char *)short_plain, strlen(short_plain),
            out, &out_len);
        expect_ok("lh0 compress", st);
        memset(back, 0, sizeof(back));
        st = lh_decompress(LH_METHOD_LH0, out, out_len, strlen(short_plain),
            back, sizeof(back));
        expect_ok("lh0 decompress", st);
    }
}

static void test_archive_cycle(void)
{
    const char *arc = "smoke_test.lzh";
    lh_writer *w;
    lh_reader *r;
    lh_entry entry;
    lh_datetime dt;
    lh_status st;
    lh_status err;
    const unsigned char data[] = "archive body";

    lh_datetime_from_time_t(&dt, 946684800L); /* 2000-01-01 */
    w = lh_writer_open(arc, 1, LH_LEVEL_LH5, &err);
    if (!w) {
        fprintf(stderr, "FAIL writer open: %s\n", lh_status_string(err));
        fail_count++;
        return;
    }
    st = lh_writer_add(w, "hello.txt", NULL, LH_ATTR_DEFAULT, &dt,
        LH_LEVEL_STORE, 1, data, sizeof(data) - 1, NULL);
    expect_ok("writer add", st);
    lh_writer_close(&w);

    r = lh_reader_open(arc, &err);
    if (!r) {
        fprintf(stderr, "FAIL reader open: %s\n", lh_status_string(err));
        fail_count++;
        remove(arc);
        return;
    }
    memset(&entry, 0, sizeof(entry));
    st = lh_reader_next(r, &entry);
    expect_ok("reader next", st);
    if (!entry.filename || strcmp(entry.filename, "hello.txt") != 0) {
        fprintf(stderr, "FAIL entry name\n");
        fail_count++;
    }
    if (entry.data_len != sizeof(data) - 1) {
        fprintf(stderr, "FAIL entry size\n");
        fail_count++;
    }
    lh_entry_clear(&entry);
    lh_reader_close(&r);
    remove(arc);
}

static void test_level1_multi_list(void)
{
    const char *arc = "smoke_lvl1.lzh";
    lh_writer *w;
    lh_reader *r;
    lh_entry entry;
    lh_datetime dt;
    lh_status st;
    lh_status err;
    const unsigned char a[] = "first";
    const unsigned char b[] = "second";
    int count;

    lh_datetime_from_time_t(&dt, 946684800L);
    w = lh_writer_open(arc, 1, LH_LEVEL_STORE, &err);
    if (!w) {
        fprintf(stderr, "FAIL level1 writer open: %s\n", lh_status_string(err));
        fail_count++;
        return;
    }
    st = lh_writer_add(w, "one.txt", NULL, LH_ATTR_DEFAULT, &dt,
        LH_LEVEL_STORE, 1, a, sizeof(a) - 1, NULL);
    expect_ok("level1 writer add one", st);
    st = lh_writer_add(w, "two.txt", NULL, LH_ATTR_DEFAULT, &dt,
        LH_LEVEL_STORE, 1, b, sizeof(b) - 1, NULL);
    expect_ok("level1 writer add two", st);
    lh_writer_close(&w);

    r = lh_reader_open(arc, &err);
    if (!r) {
        fprintf(stderr, "FAIL level1 reader open: %s\n", lh_status_string(err));
        fail_count++;
        remove(arc);
        return;
    }
    lh_reader_set_header_only(r, 1);
    count = 0;
    memset(&entry, 0, sizeof(entry));
    for (;;) {
        st = lh_reader_next(r, &entry);
        expect_ok("level1 reader next", st);
        if (!entry.filename) {
            break;
        }
        count++;
        lh_entry_clear(&entry);
    }
    if (count != 2) {
        fprintf(stderr, "FAIL level1 list count (%d)\n", count);
        fail_count++;
    }
    lh_reader_close(&r);
    remove(arc);
}

static void test_buffer_api(void)
{
    LhBuffer *buf;
    const char *plain = "buffer api";
    unsigned long n;

    buf = lh_create_buffer(0);
    if (!buf) {
        fprintf(stderr, "FAIL create buffer\n");
        fail_count++;
        return;
    }
    buf->lh_Src = (void *)plain;
    buf->lh_SrcSize = (unsigned long)strlen(plain);
    buf->lh_Dst = malloc(strlen(plain) + 64);
    buf->lh_DstSize = (unsigned long)(strlen(plain) + 64);
    n = lh_encode(buf);
    if (n == 0) {
        fprintf(stderr, "FAIL lh encode\n");
        fail_count++;
    }
    lh_delete_buffer(buf);
}

static void test_snapshot_cycle(void)
{
    const char *arc = "smoke_cp.lzh";
    lh_writer *w;
    lh_reader *r;
    lh_entry entry;
    lh_datetime dt;
    lh_status st;
    lh_status err;
    lh_snapshot cp;
    const unsigned char data[] = "snapshot body";
    unsigned char snap[LH_SNAP_ID_LEN];
    unsigned char sha[LH_SNAP_SHA256_LEN];
    int i;

    memset(&cp, 0, sizeof(cp));
    memset(snap, 0xAB, sizeof(snap));
    memset(sha, 0xCD, sizeof(sha));
    cp.generation = 3UL;
    cp.size = (unsigned long)(sizeof(data) - 1);
    cp.flags = 0;
    cp.format = (unsigned char)LH_SNAP_FORMAT_V1;
    memcpy(cp.snap_id, snap, LH_SNAP_ID_LEN);
    memcpy(cp.sha256, sha, LH_SNAP_SHA256_LEN);
    strcpy(cp.ver, "$VER: Smoke 1.0");

    lh_datetime_from_time_t(&dt, 946684800L);
    w = lh_writer_open(arc, 2, LH_LEVEL_STORE, &err);
    if (!w) {
        fprintf(stderr, "FAIL cp writer open: %s\n", lh_status_string(err));
        fail_count++;
        return;
    }
    st = lh_writer_add(w, "cp.txt", NULL, LH_ATTR_DEFAULT, &dt,
        LH_LEVEL_STORE, 1, data, sizeof(data) - 1, &cp);
    expect_ok("cp writer add", st);
    lh_writer_close(&w);

    r = lh_reader_open(arc, &err);
    if (!r) {
        fprintf(stderr, "FAIL cp reader open: %s\n", lh_status_string(err));
        fail_count++;
        remove(arc);
        return;
    }
    memset(&entry, 0, sizeof(entry));
    st = lh_reader_next(r, &entry);
    expect_ok("cp reader next", st);
    if (!entry.have_snapshot) {
        fprintf(stderr, "FAIL cp missing on read\n");
        fail_count++;
    } else if (entry.snapshot.generation != 3UL
        || entry.snapshot.size != (unsigned long)(sizeof(data) - 1)
        || strcmp(entry.snapshot.ver, "$VER: Smoke 1.0") != 0) {
        fprintf(stderr, "FAIL cp fields mismatch\n");
        fail_count++;
    } else {
        for (i = 0; i < LH_SNAP_ID_LEN; i++) {
            if (entry.snapshot.snap_id[i] != 0xAB) {
                fprintf(stderr, "FAIL cp snap_id\n");
                fail_count++;
                break;
            }
        }
        for (i = 0; i < LH_SNAP_SHA256_LEN; i++) {
            if (entry.snapshot.sha256[i] != 0xCD) {
                fprintf(stderr, "FAIL cp sha256\n");
                fail_count++;
                break;
            }
        }
    }
    lh_entry_clear(&entry);
    lh_reader_close(&r);
    remove(arc);
}

int main(void)
{
    fail_count = 0;
    test_codec_roundtrip();
    test_archive_cycle();
    test_level1_multi_list();
    test_snapshot_cycle();
    test_buffer_api();
    if (fail_count) {
        fprintf(stderr, "%d test(s) failed\n", fail_count);
        return 1;
    }
    puts("smoke: ok");
    return 0;
}
