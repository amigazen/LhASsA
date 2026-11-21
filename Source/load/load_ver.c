/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * load_ver.c - Scan and compare Amiga $VER: tags.
 *
 * Canonical form (see ////version.txt � AmigaMail 2.0):
 *   $VER: name major.minor (dd.mm.yy)
 * Date separators may be '.' or '/' (e.g. (20.3.91) or (20/3/91)).
 *
 * Return from load_ver_scan_file:
 *   1  = parsed $VER: successfully
 *   0  = I/O error
 *  -1  = file OK but no usable $VER: (missing, or only prose hits)
 *
 * Scan is capped -- real Amiga $VER: lives near the start of a binary.
 * Reading whole libraries for a tag freezes the machine.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "load_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;

#define LOAD_VER_BUF 4096
/* Enough for hunk header + $VER; never walk multi-megabyte files. */
#define LOAD_VER_SCAN_MAX 32768L

void load_ver_clear(struct LoadVerInfo *vi)
{
    if (!vi) {
        return;
    }
    vi->have_ver = 0;
    vi->have_date = 0;
    vi->major = 0;
    vi->minor = 0;
    vi->ymd = 0;
}

static STRPTR load_ver_skip_ws(STRPTR p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static STRPTR load_ver_next_tok(STRPTR p)
{
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '(') {
        p++;
    }
    return p;
}

static LONG load_ver_parse_token(STRPTR tok, ULONG *maj, ULONG *min)
{
    LONG n;
    LONG a;
    LONG b;
    STRPTR p;

    p = tok;
    n = StrToLong(p, &a);
    if (n <= 0 || a < 0) {
        return 0;
    }
    p += n;
    if (*p != '.') {
        return 0;
    }
    p++;
    n = StrToLong(p, &b);
    if (n <= 0 || b < 0) {
        return 0;
    }
    p += n;
    if (*p != '\0' && *p != ' ' && *p != '\t') {
        /* Trailing .patch ignored if present. */
        if (*p != '.') {
            return 0;
        }
    }
    *maj = (ULONG)a;
    *min = (ULONG)b;
    return 1;
}

static LONG load_ver_parse_date(STRPTR s, ULONG *ymd_out)
{
    STRPTR p;
    LONG day;
    LONG month;
    LONG year;
    LONG n;
    char sep;

    p = s;
    while (*p != '\0' && *p != '(') {
        p++;
    }
    if (*p != '(') {
        return 0;
    }
    p++;
    n = StrToLong(p, &day);
    if (n <= 0 || day < 1 || day > 31) {
        return 0;
    }
    p += n;
    sep = *p;
    if (sep != '.' && sep != '/') {
        return 0;
    }
    p++;
    n = StrToLong(p, &month);
    if (n <= 0 || month < 1 || month > 12) {
        return 0;
    }
    p += n;
    /* Second separator may be '.' or '/' (need not match the first). */
    if (*p != '.' && *p != '/') {
        return 0;
    }
    p++;
    n = StrToLong(p, &year);
    if (n <= 0 || year < 0) {
        return 0;
    }
    if (year < 100) {
        *ymd_out = (ULONG)(year * 10000L + month * 100L + day);
    } else {
        *ymd_out = (ULONG)((year % 100) * 10000L + month * 100L + day);
    }
    return 1;
}

LONG load_ver_parse_line(STRPTR line, struct LoadVerInfo *vi)
{
    char buf[LOAD_VER_LINE_MAX];
    STRPTR p;
    STRPTR tok;
    STRPTR end;
    char save;
    int saw_paren;

    load_ver_clear(vi);
    if (!line || !vi) {
        return 0;
    }
    Strncpy((STRPTR)buf, line, LOAD_VER_LINE_MAX);
    p = buf;
    if (p[0] != '$' || p[1] != 'V' || p[2] != 'E' || p[3] != 'R'
        || p[4] != ':') {
        return 0;
    }
    p += 5;
    p = load_ver_skip_ws(p);
    saw_paren = 0;
    while (*p != '\0') {
        if (*p == '(') {
            saw_paren = 1;
            break;
        }
        tok = p;
        end = load_ver_next_tok(p);
        save = *end;
        *end = '\0';
        if (!vi->have_ver && load_ver_parse_token(tok, &vi->major, &vi->minor)) {
            vi->have_ver = 1;
        }
        *end = save;
        if (save == '\0') {
            p = end;
        } else {
            p = end + 1;
        }
        p = load_ver_skip_ws(p);
    }
    if (!vi->have_ver) {
        return 0;
    }
    /*
     * Date is optional, but a '(' after the version must parse as
     * (dd.mm.yy) or (dd/mm/yy) per version.txt.
     */
    if (saw_paren) {
        if (!load_ver_parse_date(p, &vi->ymd)) {
            load_ver_clear(vi);
            return 0;
        }
        vi->have_date = 1;
    }
    return 1;
}

LONG load_ver_parse_dotted(STRPTR s, ULONG *major, ULONG *minor)
{
    if (!s || !major || !minor) {
        return 0;
    }
    return load_ver_parse_token(s, major, minor);
}

/*
 * Parse Manifest Ver= value: major.minor or major.minor(dd.mm.yy).
 * Used by Load for File: lines written by Handle (no $VER: extract needed).
 */
LONG load_ver_parse_short(STRPTR s, struct LoadVerInfo *vi)
{
    char num[32];
    LONG i;
    STRPTR p;

    load_ver_clear(vi);
    if (!s || !vi || !s[0]) {
        return 0;
    }
    i = 0;
    p = s;
    while (*p != '\0' && *p != '(' && i < (LONG)sizeof(num) - 1) {
        num[i++] = *p++;
    }
    num[i] = '\0';
    while (i > 0 && (num[i - 1] == ' ' || num[i - 1] == '\t')) {
        i--;
        num[i] = '\0';
    }
    if (!load_ver_parse_token((STRPTR)num, &vi->major, &vi->minor)) {
        load_ver_clear(vi);
        return 0;
    }
    vi->have_ver = 1;
    if (*p == '(') {
        if (!load_ver_parse_date(p, &vi->ymd)) {
            /* Version alone is still usable; drop a bad date. */
            vi->have_date = 0;
            vi->ymd = 0;
        } else {
            vi->have_date = 1;
        }
    }
    return 1;
}

LONG load_ver_scan_mem(APTR data, LONG len, struct LoadVerInfo *vi)
{
    UBYTE *buf;
    LONG i;
    LONG start;
    char verline[LOAD_VER_LINE_MAX];
    LONG vlen;

    load_ver_clear(vi);
    if (!data || len <= 0 || !vi) {
        return -1;
    }
    buf = (UBYTE *)data;
    for (i = 0; i + 5 <= len; i++) {
        if (buf[i] == '$' && buf[i + 1] == 'V' && buf[i + 2] == 'E'
            && buf[i + 3] == 'R' && buf[i + 4] == ':') {
            vlen = 0;
            start = i;
            while (start < len && vlen < LOAD_VER_LINE_MAX - 1) {
                if (buf[start] == '\0' || buf[start] == '\n'
                    || buf[start] == '\r') {
                    break;
                }
                verline[vlen++] = (char)buf[start++];
            }
            verline[vlen] = '\0';
            return load_ver_parse_line((STRPTR)verline, vi) ? 1 : 0;
        }
    }
    return -1;
}

LONG load_ver_scan_file(STRPTR path, struct LoadVerInfo *vi)
{
    BPTR fh;
    UBYTE *buf;
    char verline[LOAD_VER_LINE_MAX];
    LONG r;
    LONG keep;
    LONG i;
    LONG start;
    LONG vlen;
    LONG found;
    LONG total;
    LONG want;

    load_ver_clear(vi);
    if (!path || !vi) {
        return 0;
    }
    fh = Open(path, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        return 0;
    }
    buf = (UBYTE *)AllocMem(LOAD_VER_BUF + 8, MEMF_PUBLIC);
    if (!buf) {
        Close(fh);
        return 0;
    }
    keep = 0;
    found = 0;
    total = 0;
    for (;;) {
        if (total >= LOAD_VER_SCAN_MAX) {
            break;
        }
        want = LOAD_VER_BUF;
        if (total + want > LOAD_VER_SCAN_MAX) {
            want = LOAD_VER_SCAN_MAX - total;
        }
        r = Read(fh, buf + keep, want);
        if (r < 0) {
            FreeMem(buf, LOAD_VER_BUF + 8);
            Close(fh);
            return 0;
        }
        /*
         * EOF with nothing left to scan.  IMPORTANT: do not loop forever
         * when keep>0 at EOF (old code did r+=keep then never saw r==0).
         */
        if (r == 0 && keep == 0) {
            break;
        }
        total += r;
        r = r + keep;
        for (i = 0; i + 5 <= r; i++) {
            if (buf[i] == '$' && buf[i + 1] == 'V' && buf[i + 2] == 'E'
                && buf[i + 3] == 'R' && buf[i + 4] == ':') {
                vlen = 0;
                start = i;
                while (start < r && vlen < LOAD_VER_LINE_MAX - 1) {
                    if (buf[start] == '\0' || buf[start] == '\n'
                        || buf[start] == '\r') {
                        break;
                    }
                    verline[vlen++] = (char)buf[start++];
                }
                verline[vlen] = '\0';
                /*
                 * Prose can mention "$VER:" (.help files).  Skip bad hits
                 * and keep looking -- do not abort the whole scan.
                 */
                if (load_ver_parse_line((STRPTR)verline, vi)) {
                    found = 1;
                    break;
                }
                load_ver_clear(vi);
            }
        }
        if (found) {
            break;
        }
        if (r == keep) {
            break;
        }
        keep = 4;
        if (keep > r) {
            keep = r;
        }
        if (keep > 0) {
            CopyMem(buf + r - keep, buf, (ULONG)keep);
        }
    }
    FreeMem(buf, LOAD_VER_BUF + 8);
    Close(fh);
    if (!found) {
        return -1;
    }
    return 1;
}

/*
 * 1 = candidate newer, 0 = same/skip, -1 = candidate older.
 */
int load_ver_compare(const struct LoadVerInfo *cand,
    const struct LoadVerInfo *inst)
{
    if (!cand || !inst) {
        return 0;
    }
    if (cand->have_ver && inst->have_ver) {
        if (cand->major > inst->major) {
            return 1;
        }
        if (cand->major < inst->major) {
            return -1;
        }
        if (cand->minor > inst->minor) {
            return 1;
        }
        if (cand->minor < inst->minor) {
            return -1;
        }
        if (cand->have_date && inst->have_date) {
            if (cand->ymd > inst->ymd) {
                return 1;
            }
            if (cand->ymd < inst->ymd) {
                return -1;
            }
        }
        return 0;
    }
    if (cand->have_ver && !inst->have_ver) {
        return 1;
    }
    if (!cand->have_ver && inst->have_ver) {
        return -1;
    }
    if (cand->have_date && inst->have_date) {
        if (cand->ymd > inst->ymd) {
            return 1;
        }
        if (cand->ymd < inst->ymd) {
            return -1;
        }
    }
    return 0;
}
