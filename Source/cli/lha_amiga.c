/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lha_amiga.c - Amiga dos.library hooks for LhA extract (dates, paths, I/O).
 * ANSI builds compile empty stubs; no stdio here.
 */

#include "lha_platform.h"
#include "/include/lh.h"

#ifdef LHA_AMIGA

#define __USE_SYSBASE
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lha_internal.h"

#define LHA_PAT_BUF 512
#define LHA_EXPAND_MAX 256
#define LHA_EXPAND_PATH 160

/*
 * lh_archive.c consults LhBase->lhb_PendingMem when built into lh.library.
 * Standalone LhA has no library base; keep the symbol so the shared core
 * links, with PendingMem handoff permanently inactive.
 */
struct LHBase;
struct LHBase *LhBase = NULL;

static void lha_dt_to_datestamp(const lh_datetime *dt, struct DateStamp *ds)
{
    unsigned long unix_secs;
    unsigned long amiga_secs;

    if (!dt || !ds) {
        return;
    }
    unix_secs = (unsigned long)dt->second
        + (unsigned long)dt->minute * 60UL
        + (unsigned long)dt->hour * 3600UL
        + (unsigned long)(dt->day - 1) * 86400UL
        + (unsigned long)(dt->month - 1) * 30UL * 86400UL
        + (unsigned long)(dt->year - 1970) * 365UL * 86400UL;
    amiga_secs = unix_secs - ((8UL * 365UL + 2UL) * 86400UL);
    ds->ds_Days = (LONG)(amiga_secs / 86400UL);
    amiga_secs %= 86400UL;
    ds->ds_Minute = (LONG)(amiga_secs / 60UL);
    ds->ds_Tick = (LONG)((amiga_secs % 60UL) * 50UL);
}

static int lha_mkdir_chain(STRPTR dirpath)
{
    char buf[512];
    char *p;
    BPTR lock;

    if (!dirpath || !dirpath[0]) {
        return 1;
    }
    strncpy(buf, (const char *)dirpath, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            lock = CreateDir((STRPTR)buf);
            if (lock == (BPTR)NULL && IoErr() != ERROR_OBJECT_EXISTS) {
                return 0;
            }
            if (lock != (BPTR)NULL) {
                UnLock(lock);
            }
            *p = '/';
        }
    }
    lock = CreateDir((STRPTR)buf);
    if (lock == (BPTR)NULL && IoErr() != ERROR_OBJECT_EXISTS) {
        return 0;
    }
    if (lock != (BPTR)NULL) {
        UnLock(lock);
    }
    return 1;
}

int lha_mkpath_amiga(const char *path)
{
    char buf[512];
    STRPTR pp;
    STRPTR p;

    if (!path || !path[0]) {
        return 0;
    }
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    pp = PathPart((STRPTR)buf);
    if (!pp || pp == (STRPTR)buf) {
        return 1;
    }
    *pp = '\0';
    if (buf[0] == '\0') {
        return 1;
    }
    p = (STRPTR)buf;
    while (*p) {
        p++;
    }
    if (p > (STRPTR)buf && *(p - 1) == ':') {
        return 1;
    }
    return lha_mkdir_chain((STRPTR)buf);
}

int lha_write_file_amiga(const char *path, const unsigned char *data, size_t len)
{
    BPTR fh;
    LONG wrote;
    char parent[512];
    STRPTR pp;

    if (!path || !path[0]) {
        return 0;
    }
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    pp = PathPart((STRPTR)parent);
    if (pp && pp != (STRPTR)parent) {
        *pp = '\0';
        if (!lha_mkpath_amiga(parent)) {
            return 0;
        }
    }
    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (fh == (BPTR)NULL) {
        return 0;
    }
    if (len == 0) {
        Close(fh);
        return 1;
    }
    if (!data) {
        Close(fh);
        DeleteFile((STRPTR)path);
        return 0;
    }
    wrote = Write(fh, (APTR)data, (LONG)len);
    Close(fh);
    if (wrote != (LONG)len) {
        DeleteFile((STRPTR)path);
        return 0;
    }
    return 1;
}

void lha_apply_file_metadata(const char *path, const lh_datetime *dt, lh_attrs attrs)
{
    lha_apply_file_metadata_ex(path, dt, attrs, NULL);
}

void lha_apply_file_metadata_ex(const char *path, const lh_datetime *dt,
    lh_attrs attrs, const char *comment)
{
    struct DateStamp ds;

    if (!path || !path[0]) {
        return;
    }
    if (dt) {
        lha_dt_to_datestamp(dt, &ds);
        SetFileDate((STRPTR)path, &ds);
    }
    SetProtection((STRPTR)path, (LONG)attrs);
    if (!g_opts.ignore_filenotes && comment && comment[0]) {
        SetComment((STRPTR)path, (STRPTR)comment);
    }
}

int lha_read_filenote(const char *path, char **out_comment)
{
    BPTR lock;
    struct FileInfoBlock *fib;

    if (out_comment) {
        *out_comment = NULL;
    }
    if (!path || !out_comment) {
        return 0;
    }
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)NULL) {
        return 0;
    }
    fib = (struct FileInfoBlock *)AllocMem((ULONG)sizeof(*fib),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        UnLock(lock);
        return 0;
    }
    if (!Examine(lock, fib) || fib->fib_Comment[0] == '\0') {
        FreeMem(fib, (ULONG)sizeof(*fib));
        UnLock(lock);
        return 0;
    }
    *out_comment = lha_strdup((const char *)fib->fib_Comment);
    FreeMem(fib, (ULONG)sizeof(*fib));
    UnLock(lock);
    return (*out_comment != NULL) ? 1 : 0;
}

int lha_set_filenote(const char *path, const char *comment)
{
    if (!path || !path[0]) {
        return 0;
    }
    if (!comment) {
        comment = "";
    }
    return SetComment((STRPTR)path, (STRPTR)comment) ? 1 : 0;
}

int lha_read_lhaopts(char *buf, size_t bufsz)
{
    LONG len;

    if (!buf || bufsz == 0) {
        return 0;
    }
    buf[0] = '\0';
    len = GetVar((STRPTR)"LHAOPTS", (STRPTR)buf, (LONG)bufsz - 1,
        GVF_GLOBAL_ONLY);
    if (len < 0) {
        buf[0] = '\0';
        return 0;
    }
    buf[bufsz - 1] = '\0';
    return 1;
}

/*
 * AmigaDOS pattern match for archive member filters (#?, *, ?...).
 * Also accepts a match against the leaf name.
 */
int lha_match_pattern_amiga(const char *pattern, const char *name)
{
    UBYTE parsed[LHA_PAT_BUF];
    LONG wild;
    const char *leaf;
    const char *p;

    if (!pattern || !pattern[0]
        || (pattern[0] == '*' && pattern[1] == '\0')
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
    for (p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == ':') {
            leaf = p + 1;
        }
    }
    if (leaf != name && lha_stricmp(pattern, leaf) == 0) {
        return 1;
    }
    wild = ParsePatternNoCase((STRPTR)pattern, parsed, (LONG)sizeof(parsed));
    if (wild == -1) {
        return 0;
    }
    if (MatchPatternNoCase(parsed, (STRPTR)name)) {
        return 1;
    }
    if (leaf != name && MatchPatternNoCase(parsed, (STRPTR)leaf)) {
        return 1;
    }
    return 0;
}

/*
 * Resolve on-disk capitalisation via NameFromLock.  Relative inputs keep
 * a relative shape (same trailing component count).
 */
int lha_disk_case_path_amiga(const char *path, char *out, size_t outsz)
{
    BPTR lock;
    char full[512];
    const char *use;
    const char *p;
    LONG ncomp;
    LONG count;
    LONG len;
    int has_vol;
    int in_comp;

    if (!path || !out || outsz == 0) {
        return 0;
    }
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (lock == (BPTR)NULL) {
        return 0;
    }
    if (!NameFromLock(lock, (STRPTR)full, (LONG)sizeof(full))) {
        UnLock(lock);
        return 0;
    }
    UnLock(lock);

    has_vol = 0;
    for (p = path; *p != '\0'; p++) {
        if (*p == ':') {
            has_vol = 1;
            break;
        }
    }
    if (has_vol) {
        use = full;
    } else {
        ncomp = 0;
        in_comp = 0;
        for (p = path; *p != '\0'; p++) {
            if (*p == '/' || *p == ':') {
                if (in_comp) {
                    ncomp++;
                    in_comp = 0;
                }
            } else {
                in_comp = 1;
            }
        }
        if (in_comp) {
            ncomp++;
        }
        len = 0;
        while (full[len] != '\0') {
            len++;
        }
        count = 0;
        use = full;
        p = full + len;
        while (p > full) {
            p--;
            if (*p == '/' || *p == ':') {
                count++;
                if (count == ncomp) {
                    use = p + 1;
                    break;
                }
            }
        }
    }
    strncpy(out, use, outsz - 1);
    out[outsz - 1] = '\0';
    lha_normalize_slashes(out);
    return 1;
}

/*
 * Expand filesystem wildcards with MatchFirst/MatchNext (files only).
 */
char **lha_expand_paths_amiga(char **patterns, int count, int *out_count)
{
    char **out;
    int n;
    int i;
    int cap;
    LONG wild;
    LONG merr;
    UBYTE parsed[LHA_PAT_BUF];
    struct AnchorPath *ap;
    ULONG apsize;
    char *copy;

    if (out_count) {
        *out_count = 0;
    }
    if (!patterns || count <= 0) {
        return NULL;
    }
    cap = 16;
    n = 0;
    out = (char **)malloc((size_t)cap * sizeof(char *));
    if (!out) {
        return NULL;
    }
    apsize = (ULONG)sizeof(struct AnchorPath) + (ULONG)LHA_EXPAND_PATH;
    ap = (struct AnchorPath *)AllocVec(apsize, MEMF_CLEAR);
    if (ap == NULL) {
        free(out);
        return NULL;
    }
    ap->ap_BreakBits = SIGBREAKF_CTRL_C;
    ap->ap_Strlen = (WORD)LHA_EXPAND_PATH;

    for (i = 0; i < count; i++) {
        if (!patterns[i]) {
            continue;
        }
        wild = ParsePatternNoCase((STRPTR)patterns[i], parsed,
            (LONG)sizeof(parsed));
        if (wild <= 0) {
            if (n >= cap) {
                char **grown;
                int ncap;

                ncap = cap * 2;
                grown = (char **)realloc(out, (size_t)ncap * sizeof(char *));
                if (!grown) {
                    MatchEnd(ap);
                    FreeVec(ap);
                    lha_free_paths(out, n);
                    return NULL;
                }
                out = grown;
                cap = ncap;
            }
            copy = lha_strdup(patterns[i]);
            if (!copy) {
                MatchEnd(ap);
                FreeVec(ap);
                lha_free_paths(out, n);
                return NULL;
            }
            out[n++] = copy;
            continue;
        }
        merr = MatchFirst((STRPTR)patterns[i], ap);
        while (merr == 0) {
            if (ap->ap_Info.fib_DirEntryType < 0) {
                const char *src;

                if (n >= LHA_EXPAND_MAX) {
                    MatchEnd(ap);
                    FreeVec(ap);
                    lha_free_paths(out, n);
                    return NULL;
                }
                if (n >= cap) {
                    char **grown;
                    int ncap;

                    ncap = cap * 2;
                    grown = (char **)realloc(out,
                        (size_t)ncap * sizeof(char *));
                    if (!grown) {
                        MatchEnd(ap);
                        FreeVec(ap);
                        lha_free_paths(out, n);
                        return NULL;
                    }
                    out = grown;
                    cap = ncap;
                }
                if (ap->ap_Buf[0] != '\0') {
                    src = (const char *)ap->ap_Buf;
                } else {
                    src = (const char *)ap->ap_Info.fib_FileName;
                }
                copy = lha_strdup(src);
                if (!copy) {
                    MatchEnd(ap);
                    FreeVec(ap);
                    lha_free_paths(out, n);
                    return NULL;
                }
                out[n++] = copy;
            }
            merr = MatchNext(ap);
        }
        MatchEnd(ap);
        memset(ap, 0, (size_t)apsize);
        ap->ap_BreakBits = SIGBREAKF_CTRL_C;
        ap->ap_Strlen = (WORD)LHA_EXPAND_PATH;
    }
    FreeVec(ap);
    if (n == 0) {
        free(out);
        return NULL;
    }
    if (out_count) {
        *out_count = n;
    }
    return out;
}

#else /* !LHA_AMIGA */

#include <stdlib.h>
#include <string.h>

struct LHBase;
struct LHBase *LhBase = NULL;

void lha_apply_file_metadata(const char *path, const lh_datetime *dt, lh_attrs attrs)
{
    (void)path;
    (void)dt;
    (void)attrs;
}

void lha_apply_file_metadata_ex(const char *path, const lh_datetime *dt,
    lh_attrs attrs, const char *comment)
{
    (void)path;
    (void)dt;
    (void)attrs;
    (void)comment;
}

int lha_read_lhaopts(char *buf, size_t bufsz)
{
    const char *env;

    if (!buf || bufsz == 0) {
        return 0;
    }
    env = getenv("LHAOPTS");
    if (!env) {
        buf[0] = '\0';
        return 0;
    }
    strncpy(buf, env, bufsz - 1);
    buf[bufsz - 1] = '\0';
    return 1;
}

#endif
