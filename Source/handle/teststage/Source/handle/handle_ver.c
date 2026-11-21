/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * handle_ver.c - Scan Amiga $VER: tags for Manifest Version generation.
 *
 * Canonical form (see ////version.txt — AmigaMail 2.0):
 *   $VER: name major.minor (dd.mm.yy)
 * Date separators may be '.' or '/' (e.g. (20.3.91) or (20/3/91)).
 * Missing or unparseable $VER: is a hard failure (scan returns 0).
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "handle_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;

#define HANDLE_VER_BUF 4096

void handle_ver_clear(struct HandleVerInfo *vi)
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

static STRPTR handle_ver_skip_ws(STRPTR p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static STRPTR handle_ver_next_tok(STRPTR p)
{
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '(') {
        p++;
    }
    return p;
}

static LONG handle_ver_parse_token(STRPTR tok, ULONG *maj, ULONG *min)
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
        /* Allow trailing .patch but ignore it. */
        if (*p != '.') {
            return 0;
        }
    }
    *maj = (ULONG)a;
    *min = (ULONG)b;
    return 1;
}

static LONG handle_ver_parse_date(STRPTR s, ULONG *ymd_out)
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

LONG handle_ver_parse_line(STRPTR line, struct HandleVerInfo *vi)
{
    char buf[HANDLE_VER_LINE_MAX];
    STRPTR p;
    STRPTR tok;
    STRPTR end;
    char save;
    int saw_paren;

    handle_ver_clear(vi);
    if (!line || !vi) {
        return 0;
    }
    Strncpy((STRPTR)buf, line, HANDLE_VER_LINE_MAX);
    p = buf;
    if (p[0] != '$' || p[1] != 'V' || p[2] != 'E' || p[3] != 'R'
        || p[4] != ':') {
        return 0;
    }
    p += 5;
    p = handle_ver_skip_ws(p);
    saw_paren = 0;
    while (*p != '\0') {
        if (*p == '(') {
            saw_paren = 1;
            break;
        }
        tok = p;
        end = handle_ver_next_tok(p);
        save = *end;
        *end = '\0';
        if (!vi->have_ver && handle_ver_parse_token(tok, &vi->major, &vi->minor)) {
            vi->have_ver = 1;
        }
        *end = save;
        if (save == '\0') {
            p = end;
        } else {
            p = end + 1;
        }
        p = handle_ver_skip_ws(p);
    }
    if (!vi->have_ver) {
        return 0;
    }
    /*
     * Date is optional, but a '(' after the version must parse as
     * (dd.mm.yy) or (dd/mm/yy) per version.txt.
     */
    if (saw_paren) {
        if (!handle_ver_parse_date(p, &vi->ymd)) {
            handle_ver_clear(vi);
            return 0;
        }
        vi->have_date = 1;
    }
    return 1;
}

LONG handle_ver_parse_dotted(STRPTR s, ULONG *major, ULONG *minor)
{
    if (!s || !major || !minor) {
        return 0;
    }
    return handle_ver_parse_token(s, major, minor);
}

LONG handle_ver_scan_file(STRPTR path, struct HandleVerInfo *vi)
{
    BPTR fh;
    UBYTE *buf;
    LONG r;
    LONG keep;
    LONG i;
    LONG start;
    char verline[HANDLE_VER_LINE_MAX];
    LONG vlen;
    LONG found;

    handle_ver_clear(vi);
    if (!path || !vi) {
        return 0;
    }
    fh = Open(path, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        return 0;
    }
    buf = (UBYTE *)AllocMem(HANDLE_VER_BUF + 8, MEMF_PUBLIC);
    if (!buf) {
        Close(fh);
        return 0;
    }
    keep = 0;
    found = 0;
    for (;;) {
        r = Read(fh, buf + keep, HANDLE_VER_BUF);
        if (r < 0) {
            FreeMem(buf, HANDLE_VER_BUF + 8);
            Close(fh);
            return 0;
        }
        if (r == 0 && keep == 0) {
            break;
        }
        r += keep;
        for (i = 0; i + 5 <= r; i++) {
            if (buf[i] == '$' && buf[i + 1] == 'V' && buf[i + 2] == 'E'
                && buf[i + 3] == 'R' && buf[i + 4] == ':') {
                vlen = 0;
                start = i;
                while (start < r && vlen < HANDLE_VER_LINE_MAX - 1) {
                    if (buf[start] == '\0' || buf[start] == '\n'
                        || buf[start] == '\r') {
                        break;
                    }
                    verline[vlen++] = (char)buf[start++];
                }
                verline[vlen] = '\0';
                if (!handle_ver_parse_line((STRPTR)verline, vi)) {
                    FreeMem(buf, HANDLE_VER_BUF + 8);
                    Close(fh);
                    return 0;
                }
                found = 1;
                break;
            }
        }
        if (found) {
            break;
        }
        if (r == 0) {
            break;
        }
        keep = 4;
        if (keep > r) {
            keep = r;
        }
        CopyMem(buf + r - keep, buf, (ULONG)keep);
    }
    FreeMem(buf, HANDLE_VER_BUF + 8);
    Close(fh);
    /* Missing $VER: or unparseable tag => failure for Handle. */
    return (found && vi->have_ver) ? 1 : 0;
}

/*
 * 1 = candidate newer, 0 = same/skip, -1 = candidate older.
 */
int handle_ver_compare(const struct HandleVerInfo *cand,
    const struct HandleVerInfo *inst)
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
