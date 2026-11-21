/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lha_util.c - Filesystem helpers and archive rewrite for CLI.
 */

#include "lha_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lha_internal.h"

char *lha_strdup(const char *s)
{
    size_t n;
    char *d;

    if (!s) {
        return NULL;
    }
    n = strlen(s);
    d = (char *)malloc(n + 1);
    if (!d) {
        return NULL;
    }
    memcpy(d, s, n + 1);
    return d;
}

void lha_normalize_slashes(char *s)
{
    char *p;

    if (!s) {
        return;
    }
    for (p = s; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
}

char *lha_basename(const char *path)
{
    const char *p;
    const char *b;

    b = path;
    if (!path) {
        return lha_strdup("");
    }
    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            b = p + 1;
        }
    }
    return lha_strdup(b);
}

int lha_stricmp(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    if (!a && !b) {
        return 0;
    }
    if (!a) {
        return -1;
    }
    if (!b) {
        return 1;
    }
    while (*a || *b) {
        ca = (unsigned char)*a;
        cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        if (*a) {
            a++;
        }
        if (*b) {
            b++;
        }
    }
    return 0;
}

/*
 * True if pattern contains AmigaDOS / LhA wildcard tokens.
 * -Qw forces false so specs are treated literally.
 */
int lha_path_is_wild(const char *pattern)
{
    const char *p;

    if (g_opts.no_wildcards || !pattern) {
        return 0;
    }
    for (p = pattern; *p != '\0'; p++) {
        if (*p == '*' || *p == '?' || *p == '#' || *p == '['
            || *p == '(' || *p == '~' || *p == '|') {
            return 1;
        }
    }
    return 0;
}

void lha_free_paths(char **paths, int count)
{
    int i;

    if (!paths) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(paths[i]);
    }
    free(paths);
}

/*
 * Expand filesystem patterns.  Non-wild entries are kept as given (so ADD
 * can create a new archive name).  On non-Amiga hosts, wildcards are not
 * expanded and are kept literally.
 */
char **lha_expand_paths(char **patterns, int count, int *out_count)
{
    char **out;
    int i;

    if (out_count) {
        *out_count = 0;
    }
    if (!patterns || count <= 0) {
        return NULL;
    }
#if defined(LHA_AMIGA)
    if (!g_opts.no_wildcards) {
        return lha_expand_paths_amiga(patterns, count, out_count);
    }
#endif
    out = (char **)malloc((size_t)count * sizeof(char *));
    if (!out) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        out[i] = lha_strdup(patterns[i] ? patterns[i] : "");
        if (!out[i]) {
            lha_free_paths(out, i);
            return NULL;
        }
    }
    if (out_count) {
        *out_count = count;
    }
    return out;
}

/*
 * Strip a leading volume (text up to and including first ':').
 * Classic -x stores paths excluding the device specification.
 */
static const char *lha_strip_volume(const char *path)
{
    const char *p;

    if (!path) {
        return "";
    }
    for (p = path; *p != '\0'; p++) {
        if (*p == ':') {
            p++;
            if (*p == '/') {
                p++;
            }
            return p;
        }
    }
    return path;
}

char *lha_make_arcname(const char *path)
{
    char disk[512];
    const char *use;
    int ok;

    if (!path) {
        return lha_strdup("");
    }
    ok = 0;
#if defined(LHA_AMIGA)
    ok = lha_disk_case_path_amiga(path, disk, sizeof(disk));
#endif
    if (!ok) {
        strncpy(disk, path, sizeof(disk) - 1);
        disk[sizeof(disk) - 1] = '\0';
        lha_normalize_slashes(disk);
    }
    if (g_opts.preserve_paths > 0) {
        use = lha_strip_volume(disk);
        if (!use[0]) {
            return lha_basename(disk);
        }
        return lha_strdup(use);
    }
    return lha_basename(disk);
}

int lha_path_join(char *out, size_t outsz, const char *dir, const char *name)
{
    size_t dlen;
    size_t nlen;

    if (!out || outsz == 0 || !name) {
        return 0;
    }
    if (!dir || !dir[0] || strcmp(dir, ".") == 0 || strcmp(dir, "./") == 0) {
        strncpy(out, name, outsz - 1);
        out[outsz - 1] = '\0';
        return 1;
    }
    dlen = strlen(dir);
    nlen = strlen(name);
    if (dlen + nlen + 2 > outsz) {
        return 0;
    }
    strcpy(out, dir);
    if (out[dlen - 1] != '/' && out[dlen - 1] != '\\' && out[dlen - 1] != ':') {
        strcat(out, "/");
    }
    strcat(out, name);
    lha_normalize_slashes(out);
    return 1;
}

int lha_mkpath(const char *path)
{
#ifdef LHA_AMIGA
    return lha_mkpath_amiga(path);
#else
    char buf[512];
    char *p;
    size_t len;

    if (!path || !path[0]) {
        return 0;
    }
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    lha_normalize_slashes(buf);
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '/') {
        buf[len - 1] = '\0';
    }
    for (p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    return mkdir(buf, 0755) == 0 || (buf[0] != '\0');
#endif
}

int lha_write_file(const char *path, const unsigned char *data, size_t len)
{
#ifdef LHA_AMIGA
    return lha_write_file_amiga(path, data, len);
#else
    FILE *fp;
    char dirbuf[512];
    char *slash;

    if (!path) {
        return 0;
    }
    strncpy(dirbuf, path, sizeof(dirbuf) - 1);
    dirbuf[sizeof(dirbuf) - 1] = '\0';
    slash = strrchr(dirbuf, '/');
    if (slash) {
        *slash = '\0';
        lha_mkpath(dirbuf);
    }
    fp = fopen(path, "wb");
    if (!fp) {
        return 0;
    }
    if (len > 0 && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return 1;
#endif
}

/*
 * Portable subset when AmigaDOS ParsePattern is unavailable: *, #?, ?,
 * and case-insensitive exact match.  Also tries the leaf name.
 */
static int lha_match_pattern_simple(const char *pattern, const char *name)
{
    const char *pp;
    const char *np;
    const char *leaf;

    if (!pattern || !pattern[0]
        || strcmp(pattern, "*") == 0
        || (pattern[0] == '#' && pattern[1] == '?' && pattern[2] == '\0')) {
        return 1;
    }
    if (!name) {
        return 0;
    }
    if (lha_stricmp(pattern, name) == 0) {
        return 1;
    }
    leaf = name;
    for (np = name; *np; np++) {
        if (*np == '/' || *np == '\\' || *np == ':') {
            leaf = np + 1;
        }
    }
    if (leaf != name && lha_stricmp(pattern, leaf) == 0) {
        return 1;
    }

    /* Simple * / ? glob against name and leaf. */
    pp = pattern;
    np = name;
    for (;;) {
        if (*pp == '\0') {
            return *np == '\0';
        }
        if (*pp == '*') {
            pp++;
            if (*pp == '\0') {
                return 1;
            }
            while (*np) {
                if (lha_match_pattern_simple(pp, np)) {
                    return 1;
                }
                np++;
            }
            return 0;
        }
        if (*np == '\0') {
            return 0;
        }
        if (*pp == '?') {
            pp++;
            np++;
            continue;
        }
        {
            char ca;
            char cb;

            ca = *pp;
            cb = *np;
            if (ca >= 'A' && ca <= 'Z') {
                ca = (char)(ca - 'A' + 'a');
            }
            if (cb >= 'A' && cb <= 'Z') {
                cb = (char)(cb - 'A' + 'a');
            }
            if (ca != cb) {
                return 0;
            }
        }
        pp++;
        np++;
    }
}

int lha_match_pattern(const char *pattern, const char *name)
{
    const char *p;
    const char *leaf;

    if (g_opts.no_wildcards) {
        if (!pattern || !pattern[0]) {
            return 1;
        }
        if (!name) {
            return 0;
        }
        if (lha_stricmp(pattern, name) == 0) {
            return 1;
        }
        leaf = name;
        for (p = name; *p != '\0'; p++) {
            if (*p == '/' || *p == '\\' || *p == ':') {
                leaf = p + 1;
            }
        }
        return lha_stricmp(pattern, leaf) == 0;
    }
#if defined(LHA_AMIGA)
    return lha_match_pattern_amiga(pattern, name);
#else
    if (lha_match_pattern_simple(pattern, name)) {
        return 1;
    }
    if (!name) {
        return 0;
    }
    leaf = name;
    for (p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            leaf = p + 1;
        }
    }
    if (leaf != name) {
        return lha_match_pattern_simple(pattern, leaf);
    }
    return 0;
#endif
}

int lha_name_matches_any(const char *name, char **patterns, int count)
{
    int i;

    if (!patterns || count <= 0) {
        return 1;
    }
    if (!name) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (lha_match_pattern(patterns[i], name)) {
            return 1;
        }
    }
    return 0;
}

/*
 * Load every member filename from archive (header-only).  Returns a
 * malloc'd list; empty archives / missing files yield count 0 and NULL.
 * Used by classic `a` duplicate skip before the writer is opened.
 */
char **lha_archive_member_names(const char *archive, int *out_count)
{
    lh_reader *r;
    lh_entry entry;
    lh_status st;
    lh_status err;
    char **names;
    int n;
    int cap;
    char *copy;

    if (out_count) {
        *out_count = 0;
    }
    if (!archive) {
        return NULL;
    }
    r = lh_reader_open(archive, &err);
    if (!r) {
        return NULL;
    }
    names = NULL;
    n = 0;
    cap = 0;
    lh_reader_set_header_only(r, 1);
    for (;;) {
        memset(&entry, 0, sizeof(entry));
        st = lh_reader_next(r, &entry);
        if (st == LH_OK && !entry.filename) {
            break;
        }
        if (st != LH_OK) {
            lh_entry_clear(&entry);
            break;
        }
        if (entry.filename && entry.filename[0]) {
            if (n >= cap) {
                char **grown;
                int ncap;

                ncap = (cap == 0) ? 16 : cap * 2;
                grown = (char **)realloc(names,
                    (size_t)ncap * sizeof(char *));
                if (!grown) {
                    lh_entry_clear(&entry);
                    lh_reader_close(&r);
                    lha_free_paths(names, n);
                    return NULL;
                }
                names = grown;
                cap = ncap;
            }
            copy = lha_strdup(entry.filename);
            if (!copy) {
                lh_entry_clear(&entry);
                lh_reader_close(&r);
                lha_free_paths(names, n);
                return NULL;
            }
            names[n++] = copy;
        }
        lh_entry_clear(&entry);
    }
    lh_reader_close(&r);
    if (out_count) {
        *out_count = n;
    }
    return names;
}

int lha_archive_has_member(const char *archive, const char *name)
{
    char **names;
    int n;
    int i;
    int found;

    names = lha_archive_member_names(archive, &n);
    if (!names) {
        return 0;
    }
    found = 0;
    for (i = 0; i < n; i++) {
        if (lha_stricmp(names[i], name) == 0) {
            found = 1;
            break;
        }
    }
    lha_free_paths(names, n);
    return found;
}

int lha_read_file(const char *path, unsigned char **data, size_t *len, lh_datetime *dt)
{
    FILE *fp;
    unsigned char *buf;
    long sz;
    struct stat st;

    if (!path || !data || !len) {
        return 0;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }
    buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return 0;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    *data = buf;
    *len = (size_t)sz;
    if (dt && stat(path, &st) == 0) {
        lh_datetime_from_time_t(dt, (long)st.st_mtime);
    }
    return 1;
}

lh_status lha_rewrite_archive(
    const char *archive,
    int (*keep)(const lh_entry *entry, void *ctx),
    void *ctx
)
{
    return lh_archive_rewrite(archive, keep, ctx,
        (unsigned char)g_opts.header_level, g_opts.level, g_opts.store_only);
}

const char *lha_archive_name_for_display(const char *path)
{
    return path ? path : "";
}

int lha_is_autoshow_name(const char *filename)
{
    size_t n;
    size_t slen;
    size_t i;
    static const char suf[] = ".displayme";
    const char *p;
    char a;

    if (!filename) {
        return 0;
    }
    n = strlen(filename);
    slen = sizeof(suf) - 1;
    if (n < slen) {
        return 0;
    }
    p = filename + n - slen;
    for (i = 0; i < slen; i++) {
        a = p[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (a != suf[i]) {
            return 0;
        }
    }
    return 1;
}

int lha_autoshow_enabled(void)
{
    if (g_opts.noop_no_autoshow) {
        return 0;
    }
    if (g_opts.quiet) {
        return 0;
    }
    if (g_opts.noop_no_progress) {
        return 0;
    }
    return 1;
}

void lha_autoshow_display(const unsigned char *data, size_t len)
{
    if (!data || len == 0) {
        return;
    }
    fwrite(data, 1, len, stdout);
    if (data[len - 1] != '\n') {
        fputc('\n', stdout);
    }
    fputc('\n', stdout);
}

static int lha_show_progress(void)
{
    if (g_opts.quiet || g_opts.noop_no_progress) {
        return 0;
    }
    return 1;
}

static int lha_show_messages(void)
{
    if (g_opts.quiet) {
        return 0;
    }
    return 1;
}

void lha_extract_banner(const char *archive)
{
    if (!lha_show_messages()) {
        return;
    }
    fprintf(stdout, "\033[0 pExtracting from archive '%s':\n\n",
        lha_archive_name_for_display(archive));
    fflush(stdout);
}

void lha_test_banner(const char *archive)
{
    if (!lha_show_messages()) {
        return;
    }
    fprintf(stdout, "\033[0 pTesting archive '%s':\n\n",
        lha_archive_name_for_display(archive));
    fflush(stdout);
}

void lha_extract_summary(unsigned long count, unsigned long bad)
{
    if (!lha_show_messages()) {
        return;
    }
    if (count == 1) {
        fputs("\n1 file extracted", stdout);
    } else {
        fprintf(stdout, "\n%lu files extracted", count);
    }
    if (bad > 0) {
        if (bad == 1) {
            fputs(", [ 1 file BAD ]", stdout);
        } else {
            fprintf(stdout, ", [ %lu files BAD ]", bad);
        }
    } else {
        fputs(", all files OK", stdout);
    }
    fputc('.', stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void lha_test_summary(unsigned long count, unsigned long bad)
{
    if (!lha_show_messages()) {
        return;
    }
    if (count == 1) {
        fputs("\n1 file tested", stdout);
    } else {
        fprintf(stdout, "\n%lu files tested", count);
    }
    if (bad > 0) {
        if (bad == 1) {
            fputs(", [ 1 file BAD ]", stdout);
        } else {
            fprintf(stdout, ", [ %lu files BAD ]", bad);
        }
    } else {
        fputs(", all files OK", stdout);
    }
    fputc('.', stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void lha_operation_success(void)
{
    if (!lha_show_messages()) {
        return;
    }
    fputs("\n\033 pOperation successful.\n", stdout);
    fflush(stdout);
}

void lha_operation_failed(void)
{
    if (!lha_show_messages()) {
        return;
    }
    fputs("\n\033 pOperation failed.\n", stdout);
    fflush(stdout);
}

void lha_add_message(const char *name)
{
    lha_add_freeze_begin(name);
}

void lha_add_freeze_begin(const char *name)
{
    if (!lha_show_messages() || !name) {
        return;
    }
    fprintf(stdout, " Freezing: %s\n", name);
    fflush(stdout);
}

void lha_add_freeze_end(const char *name, int stored)
{
    if (!lha_show_messages() || !name) {
        return;
    }
    if (stored) {
        fprintf(stdout, " Frozen (store): %s\n", name);
    } else {
        fprintf(stdout, " Frozen: %s\n", name);
    }
    fflush(stdout);
}

static const char *lha_progress_name;
static size_t lha_progress_total;
static int lha_progress_is_test;
static const lha_args *lha_progress_filter_args;
static int lha_progress_skip;

void lha_progress_set_filter(const lha_args *args)
{
    lha_progress_filter_args = args;
}

static void lha_progress_finish_line(
    const char *name,
    int crc_ok,
    unsigned short wanted_crc,
    unsigned short computed_crc)
{
    if (crc_ok) {
        fputc('\n', stdout);
    } else {
        fprintf(stdout, "\n ** BAD CRC (wanted %04X got %04X): %s\n",
            (unsigned int)wanted_crc, (unsigned int)computed_crc, name);
    }
    fflush(stdout);
}

void lha_progress_begin(void *ctx, const char *name, size_t size)
{
    const char *label;

    (void)ctx;
    if (!lha_show_progress()) {
        return;
    }
    lha_progress_name = name;
    lha_progress_total = size;
    label = lha_progress_is_test ? "Testing" : "Extracting";
    fprintf(stdout, " %s: (       0/%8lu)  %s", label,
        (unsigned long)size, name ? name : "");
    fflush(stdout);
}

void lha_progress_update(void *ctx, size_t done, size_t total)
{
    const char *label;

    (void)ctx;
    (void)total;
    if (lha_progress_skip || !lha_show_progress()) {
        return;
    }
    if (g_opts.noop_fast_progress) {
        return;
    }
    label = lha_progress_is_test ? "Testing" : "Extracting";
    fprintf(stdout, "\r %s: (%8lu/%8lu)  %s", label,
        (unsigned long)done,
        (unsigned long)lha_progress_total,
        lha_progress_name ? lha_progress_name : "");
    fflush(stdout);
}

void lha_progress_end(void *ctx, const char *name, int crc_ok,
    unsigned short wanted_crc, unsigned short computed_crc, size_t size)
{
    (void)ctx;
    (void)size;
    if (lha_progress_skip || !lha_show_progress()) {
        return;
    }
    lha_progress_finish_line(name, crc_ok, wanted_crc, computed_crc);
}

void lha_test_progress_end(void *ctx, const char *name, int crc_ok,
    unsigned short wanted_crc, unsigned short computed_crc, size_t size)
{
    int saved;

    saved = lha_progress_is_test;
    lha_progress_is_test = 1;
    lha_progress_end(ctx, name, crc_ok, wanted_crc, computed_crc, size);
    lha_progress_is_test = saved;
}

void lha_extract_progress_begin(void *ctx, const char *name, size_t size)
{
    lha_progress_skip = 0;
    if (lha_progress_filter_args) {
        if (!lha_name_matches_any(name, lha_progress_filter_args->files,
                lha_progress_filter_args->file_count)) {
            lha_progress_skip = 1;
            return;
        }
    }
    lha_progress_is_test = 0;
    lha_progress_begin(ctx, name, size);
}

void lha_test_progress_begin(void *ctx, const char *name, size_t size)
{
    lha_progress_is_test = 1;
    lha_progress_begin(ctx, name, size);
}
