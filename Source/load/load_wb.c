/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * load_wb.c - Workbench entry and install confirmation requesters.
 *
 * Prefer requester.class (Reaction/ClassAct); EasyRequest fallback.
 * WB always SelectOutput to CON: (VERBOSE) or NIL: before any Printf.
 * Body text is %-escaped -- both EasyRequest and REQ_BodyText use RawDoFmt.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <intuition/classusr.h>
#include <intuition/intuitionbase.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/icon.h>
#include <libraries/lh.h>
#include <proto/lh.h>

#include "load_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;
extern struct Library *LhBase;

struct IntuitionBase *IntuitionBase = NULL;

/* Optional Reaction/ClassAct requester.class. */
struct Library *RequesterBase = NULL;

/*
 * Workbench has no console unless we open one.  VERBOSE -> CON: window;
 * otherwise NIL: so Printf/Flush never hit a NULL Output (that wedges).
 */
static BPTR load_wb_out = (BPTR)NULL;
static BPTR load_wb_oldout = (BPTR)NULL;
static ULONG load_wb_out_is_con = 0;

/* proto/icon.h stubs use this global. */
struct Library *IconBase = NULL;

/*
 * classes/requester.h tags.  REACTION_Dummy is TAG_USER+0x5000000
 * (not bare TAG_USER) -- wrong base makes NewObject ignore every REQ_*
 * tag and fall through to EasyRequest.
 */
#ifndef TAG_USER
#define TAG_USER (1L << 31)
#endif
#define LOAD_REACTION_Dummy  (TAG_USER + 0x5000000L)
#define LOAD_REQ_Dummy       (LOAD_REACTION_Dummy + 0x45000L)
#define LOAD_REQ_Type        (LOAD_REQ_Dummy + 1)
#define LOAD_REQ_TitleText   (LOAD_REQ_Dummy + 2)
#define LOAD_REQ_BodyText    (LOAD_REQ_Dummy + 3)
#define LOAD_REQ_GadgetText  (LOAD_REQ_Dummy + 4)
#define LOAD_RM_OPENREQ      (0x650001L)
#define LOAD_REQTYPE_INFO    0

struct LoadOrRequest {
    ULONG MethodID;
    struct TagItem *or_Attrs;
    struct Window *or_Window;
    struct Screen *or_Screen;
};

/*
 * REQUESTER_GetClass is LVO -30 (0x1e) on requester.class.
 * SAS/C pragma; other compilers use NewObject by public name.
 */
#ifdef __SASC
Class *REQUESTER_GetClass(void);
#pragma libcall RequesterBase REQUESTER_GetClass 1e 00
#endif

#define LOAD_WB_BODY_MAX 1200
#define LOAD_WB_LINE_MAX 100
#define LOAD_WB_MAX_LINES 14
#define LOAD_WB_DISPLAYME "Load.displayme"

static void load_wb_cat(STRPTR dst, LONG dstlen, STRPTR src)
{
    LONG L;
    LONG i;

    if (!dst || dstlen <= 0 || !src) {
        return;
    }
    L = 0;
    while (dst[L] != '\0' && L < dstlen - 1) {
        L++;
    }
    i = 0;
    while (src[i] != '\0' && L < dstlen - 1) {
        dst[L++] = src[i++];
    }
    dst[L] = '\0';
}

/*
 * Escape '%' -> '%%' for RawDoFmt.  EasyRequest always formats the body;
 * requester.class REQ_BodyText also runs RawDoFmt when '%' is present.
 */
static void load_wb_escape_pct(STRPTR dst, LONG dstlen, STRPTR src)
{
    LONG L;
    LONG i;

    if (!dst || dstlen <= 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    L = 0;
    i = 0;
    while (src[i] != '\0' && L < dstlen - 1) {
        if (src[i] == '%' && L < dstlen - 2) {
            dst[L++] = '%';
        }
        dst[L++] = src[i++];
    }
    dst[L] = '\0';
}

static void load_wb_cat_long(STRPTR dst, LONG dstlen, LONG v)
{
    char tmp[16];
    char num[16];
    LONG n;
    LONG x;
    LONG i;

    x = v;
    if (x < 0) {
        load_wb_cat(dst, dstlen, (STRPTR)"-");
        x = -x;
    }
    n = 0;
    if (x == 0) {
        num[n++] = '0';
    } else {
        while (x > 0 && n < (LONG)sizeof(num) - 1) {
            num[n++] = (char)('0' + (x % 10));
            x /= 10;
        }
    }
    for (i = 0; i < n; i++) {
        tmp[i] = num[n - 1 - i];
    }
    tmp[n] = '\0';
    load_wb_cat(dst, dstlen, (STRPTR)tmp);
}

static void load_wb_pad2(STRPTR dst, LONG dstlen, LONG v)
{
    if (v < 10) {
        load_wb_cat(dst, dstlen, (STRPTR)"0");
    }
    load_wb_cat_long(dst, dstlen, v);
}

/*
 * Format "major.minor" and optional " (dd.mm.yy)" from $VER info.
 */
static LONG load_wb_fmt_ver(const struct LoadVerInfo *ver, STRPTR out,
    LONG outlen)
{
    LONG day;
    LONG month;
    LONG year;

    if (!out || outlen < 4) {
        return 0;
    }
    out[0] = '\0';
    if (!ver || !ver->have_ver) {
        return 0;
    }
    load_wb_cat_long(out, outlen, (LONG)ver->major);
    load_wb_cat(out, outlen, (STRPTR)".");
    load_wb_cat_long(out, outlen, (LONG)ver->minor);
    if (ver->have_date) {
        day = (LONG)(ver->ymd % 100UL);
        month = (LONG)((ver->ymd / 100UL) % 100UL);
        year = (LONG)(ver->ymd / 10000UL);
        load_wb_cat(out, outlen, (STRPTR)" (");
        load_wb_pad2(out, outlen, day);
        load_wb_cat(out, outlen, (STRPTR)".");
        load_wb_pad2(out, outlen, month);
        load_wb_cat(out, outlen, (STRPTR)".");
        load_wb_pad2(out, outlen, year);
        load_wb_cat(out, outlen, (STRPTR)")");
    }
    return 1;
}

static LONG load_wb_ensure_libs(void)
{
    if (!IntuitionBase) {
        IntuitionBase = (struct IntuitionBase *)OpenLibrary(
            (STRPTR)"intuition.library", 37L);
        if (!IntuitionBase) {
            return 0;
        }
    }
    if (!RequesterBase) {
        /* Version 0: ClassAct and OS3.2 both register the public class. */
        RequesterBase = OpenLibrary((STRPTR)"requester.class", 0L);
    }
    return 1;
}

/*
 * Show a REQTYPE_INFO requester.  Returns gadget number: 1..N-1 then 0 for
 * the rightmost button (a single-button INFO requester always returns 0).
 *
 * Match Avalanche OpenRequester(obj, NULL): RM_OPENREQ with or_Window and
 * or_Screen both NULL (default pub screen).  Never LockPubScreen here --
 * holding that lock across RM_OPENREQ left the nest wrong after dismiss and
 * wedged Intuition even though the requester closed cleanly.
 *
 * Body is %-escaped before either path (RawDoFmt).
 */
static LONG load_wb_open_req(STRPTR title, STRPTR body, STRPTR gadgets)
{
    Object *obj;
    Class *cl;
    struct LoadOrRequest msg;
    struct EasyStruct es;
    STRPTR fmtbody;
    ULONG fmt_sz;
    LONG blen;
    LONG rc;

    if (!title) {
        title = (STRPTR)"Load";
    }
    if (!body) {
        body = (STRPTR)"";
    }
    if (!gadgets) {
        /* Prefer a real word; never label a gadget "OK" (use "Ok" if needed). */
        gadgets = (STRPTR)"_Continue";
    }

    if (!load_wb_ensure_libs()) {
        return 0;
    }

    blen = 0;
    while (body[blen] != '\0') {
        blen++;
    }
    fmt_sz = (ULONG)blen * 2UL + 1UL;
    if (fmt_sz < 64UL) {
        fmt_sz = 64UL;
    }
    fmtbody = (STRPTR)AllocMem(fmt_sz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!fmtbody) {
        return 0;
    }
    load_wb_escape_pct(fmtbody, (LONG)fmt_sz, body);

    rc = 0;
    obj = NULL;
    cl = NULL;

    if (RequesterBase) {
#ifdef __SASC
        cl = REQUESTER_GetClass();
#endif
        if (cl) {
            obj = NewObject(cl, NULL,
                LOAD_REQ_Type, LOAD_REQTYPE_INFO,
                LOAD_REQ_TitleText, title,
                LOAD_REQ_BodyText, fmtbody,
                LOAD_REQ_GadgetText, gadgets,
                TAG_DONE);
        }
        if (!obj) {
            obj = NewObject(NULL, (STRPTR)"requester.class",
                LOAD_REQ_Type, LOAD_REQTYPE_INFO,
                LOAD_REQ_TitleText, title,
                LOAD_REQ_BodyText, fmtbody,
                LOAD_REQ_GadgetText, gadgets,
                TAG_DONE);
        }
        if (obj) {
            /* OpenRequester(obj, NULL) -- see classes/requester.h */
            msg.MethodID = LOAD_RM_OPENREQ;
            msg.or_Attrs = NULL;
            msg.or_Window = NULL;
            msg.or_Screen = NULL;
            rc = (LONG)DoMethodA(obj, (Msg)&msg);
            DisposeObject(obj);
            FreeMem(fmtbody, fmt_sz);
            return rc;
        }
    }

    /* EasyRequest fallback (no LockPubScreen). */
    es.es_StructSize = sizeof(struct EasyStruct);
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)title;
    es.es_TextFormat = (UBYTE *)fmtbody;
    es.es_GadgetFormat = (UBYTE *)gadgets;
    rc = EasyRequest(NULL, &es, NULL);
    FreeMem(fmtbody, fmt_sz);
    return rc;
}

void load_wb_close_libs(void)
{
    if (RequesterBase) {
        CloseLibrary(RequesterBase);
        RequesterBase = NULL;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}

void load_wb_error(STRPTR msg, LONG code)
{
    STRPTR body;

    body = (STRPTR)AllocMem(LOAD_WB_BODY_MAX, MEMF_PUBLIC | MEMF_CLEAR);
    if (!body) {
        return;
    }
    if (msg && msg[0]) {
        load_wb_cat(body, LOAD_WB_BODY_MAX, msg);
    } else {
        load_wb_cat(body, LOAD_WB_BODY_MAX, (STRPTR)"Load failed.");
    }
    if (code) {
        load_wb_cat(body, LOAD_WB_BODY_MAX, (STRPTR)"\nCode ");
        load_wb_cat_long(body, LOAD_WB_BODY_MAX, code);
    }
    load_wb_open_req((STRPTR)"Load", body, (STRPTR)"_Continue");
    FreeMem(body, LOAD_WB_BODY_MAX);
}

void load_wb_warn(STRPTR msg)
{
    if (!msg) {
        msg = (STRPTR)"Warning.";
    }
    load_wb_open_req((STRPTR)"Load", msg, (STRPTR)"_Continue");
}

/*
 * Append up to max_lines paths from a packed LOAD_SUMMARY_LEN table.
 * Returns how many were printed; if more remain, appends "...and N more".
 */
static LONG load_wb_cat_summary_list(STRPTR out, LONG outlen,
    STRPTR table, LONG n, LONG max_lines)
{
    LONG i;
    LONG shown;
    STRPTR row;

    if (!out || outlen <= 0 || !table || n <= 0) {
        return 0;
    }
    shown = 0;
    for (i = 0; i < n; i++) {
        if (shown >= max_lines) {
            break;
        }
        row = table + i * LOAD_SUMMARY_LEN;
        if (!row[0]) {
            continue;
        }
        load_wb_cat(out, outlen, (STRPTR)"  ");
        load_wb_cat(out, outlen, row);
        load_wb_cat(out, outlen, (STRPTR)"\n");
        shown++;
    }
    if (n > shown) {
        load_wb_cat(out, outlen, (STRPTR)"  ...and ");
        load_wb_cat_long(out, outlen, n - shown);
        load_wb_cat(out, outlen, (STRPTR)" more\n");
    }
    return shown;
}

/*
 * Post-install WB dialog: exact Added / Updated dest lists (have_bak =>
 * Updated, else Added).  Optional reboot note when Rem* left a module
 * resident.
 */
void load_wb_install_done(STRPTR added, LONG nadded,
    STRPTR updated, LONG nupdated, LONG need_reboot)
{
    STRPTR body;
    ULONG body_sz;
    LONG per_sec;

    /*
     * Size for header + two sections of paths.  Cap listed lines so the
     * requester stays readable on a low WB screen.
     */
    per_sec = LOAD_WB_MAX_LINES;
    body_sz = 256UL
        + (ULONG)(per_sec + 2) * (ULONG)(LOAD_SUMMARY_LEN + 4) * 2UL;
    if (body_sz < (ULONG)LOAD_WB_BODY_MAX) {
        body_sz = (ULONG)LOAD_WB_BODY_MAX;
    }
    body = (STRPTR)AllocMem(body_sz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!body) {
        load_wb_warn(need_reboot
            ? (STRPTR)
              "Install finished, but a reboot is recommended\n"
              "so resident modules pick up the new files."
            : (STRPTR)"Install finished.");
        return;
    }

    load_wb_cat(body, (LONG)body_sz, (STRPTR)"Install complete.\n");

    if (nadded <= 0 && nupdated <= 0) {
        load_wb_cat(body, (LONG)body_sz,
            (STRPTR)"\nNothing new to install (all up to date).\n");
    } else {
        if (nadded > 0) {
            load_wb_cat(body, (LONG)body_sz, (STRPTR)"\nAdded:\n");
            load_wb_cat_summary_list(body, (LONG)body_sz, added, nadded,
                per_sec);
        }
        if (nupdated > 0) {
            load_wb_cat(body, (LONG)body_sz, (STRPTR)"\nUpdated:\n");
            load_wb_cat_summary_list(body, (LONG)body_sz, updated, nupdated,
                per_sec);
        }
    }

    if (need_reboot) {
        load_wb_cat(body, (LONG)body_sz,
            (STRPTR)
            "\nA reboot is recommended so resident modules\n"
            "pick up the new files.\n");
    }

    load_wb_open_req((STRPTR)"Load", body, (STRPTR)"_Done");
    FreeMem(body, body_sz);
}

#define LOAD_DM_VAL_LEN 64
#define LOAD_DM_TITLE_LEN 80
#define LOAD_DM_GADGET_LEN 48

/*
 * Aminet/Parts field block at the top of Load.displayme.  Load uses the
 * values as requester title / button text; the raw "Key: value" lines are
 * never shown in the WB UI (LhA autoshow still shows the full member).
 */
struct LoadDisplayMeFields {
    char short_s[LOAD_DM_VAL_LEN];
    char author[LOAD_DM_VAL_LEN];
    char version[LOAD_DM_VAL_LEN];
    char parts_name[LOAD_DM_VAL_LEN];
    char parts_installer[LOAD_DM_VAL_LEN];
    STRPTR body; /* points into the NUL-terminated displayme text */
};

static void load_dm_copy_val(STRPTR dst, LONG dstlen, STRPTR src)
{
    LONG i;

    if (!dst || dstlen <= 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    while (*src == ' ' || *src == '\t') {
        src++;
    }
    i = 0;
    while (src[i] != '\0' && src[i] != '\r' && src[i] != '\n'
            && i < dstlen - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\t')) {
        i--;
        dst[i] = '\0';
    }
}

/*
 * Split Aminet-style header (Key: value lines) from the blank-line-separated
 * prose body.  Returns 1 always; body may be empty if the member is fields only.
 */
static LONG load_dm_parse(STRPTR text, struct LoadDisplayMeFields *f)
{
    STRPTR p;
    STRPTR line;
    STRPTR colon;
    char key[32];
    LONG ki;
    LONG in_fields;

    if (!f) {
        return 0;
    }
    f->short_s[0] = '\0';
    f->author[0] = '\0';
    f->version[0] = '\0';
    f->parts_name[0] = '\0';
    f->parts_installer[0] = '\0';
    f->body = (STRPTR)"";
    if (!text) {
        return 0;
    }

    p = text;
    in_fields = 1;
    while (in_fields && *p != '\0') {
        line = p;
        while (*p != '\0' && *p != '\n' && *p != '\r') {
            p++;
        }
        if (*p == '\r') {
            *p++ = '\0';
        }
        if (*p == '\n') {
            *p++ = '\0';
        }

        /* Blank line ends the field block. */
        if (line[0] == '\0') {
            in_fields = 0;
            f->body = p;
            break;
        }

        colon = line;
        while (*colon != '\0' && *colon != ':') {
            colon++;
        }
        if (*colon != ':') {
            /* Non-field line before a blank -- treat rest as body. */
            f->body = line;
            break;
        }

        ki = 0;
        while (line < colon && ki < (LONG)sizeof(key) - 1) {
            key[ki++] = *line++;
        }
        key[ki] = '\0';
        while (ki > 0 && (key[ki - 1] == ' ' || key[ki - 1] == '\t')) {
            ki--;
            key[ki] = '\0';
        }
        colon++;

        if (Stricmp((STRPTR)key, (STRPTR)"Short") == 0) {
            load_dm_copy_val(f->short_s, LOAD_DM_VAL_LEN, colon);
        } else if (Stricmp((STRPTR)key, (STRPTR)"Author") == 0) {
            load_dm_copy_val(f->author, LOAD_DM_VAL_LEN, colon);
        } else if (Stricmp((STRPTR)key, (STRPTR)"Version") == 0) {
            load_dm_copy_val(f->version, LOAD_DM_VAL_LEN, colon);
        } else if (Stricmp((STRPTR)key, (STRPTR)"Parts-Name") == 0) {
            load_dm_copy_val(f->parts_name, LOAD_DM_VAL_LEN, colon);
        } else if (Stricmp((STRPTR)key, (STRPTR)"Parts-Installer") == 0) {
            load_dm_copy_val(f->parts_installer, LOAD_DM_VAL_LEN, colon);
        }
        /* Other Aminet/Parts keys are consumed, not shown. */
    }
    if (in_fields) {
        f->body = (STRPTR)"";
    }
    /* Skip leading blank lines in the prose body. */
    while (f->body[0] == '\n' || f->body[0] == '\r') {
        f->body++;
    }
    return 1;
}

/*
 * Build requester title from Parts-Name / Short / Version.  Never dumps
 * the raw field block into the body.
 */
static void load_dm_make_title(struct LoadDisplayMeFields *f, STRPTR out,
    LONG outlen)
{
    if (!out || outlen <= 0) {
        return;
    }
    out[0] = '\0';
    if (f && f->parts_name[0]) {
        load_wb_cat(out, outlen, f->parts_name);
    } else if (f && f->short_s[0]) {
        load_wb_cat(out, outlen, f->short_s);
    } else {
        load_wb_cat(out, outlen, (STRPTR)"Load");
    }
    if (f && f->version[0]) {
        load_wb_cat(out, outlen, (STRPTR)" ");
        load_wb_cat(out, outlen, f->version);
    }
}

/*
 * Displayme dismiss is always Next.  Parts-Installer names the confirm
 * button on the following requester (_Load), not this one -- using Load
 * here made the first dialog look like the install confirm and confused
 * the WB flow (and could stack two Load-labelled requesters).
 */
static void load_dm_make_gadget(struct LoadDisplayMeFields *f, STRPTR out,
    LONG outlen)
{
    (void)f;
    if (!out || outlen < 6) {
        return;
    }
    Strncpy(out, (STRPTR)"_Next", outlen);
}

/*
 * Show Load.displayme from the Bin before confirm.  Field values become
 * the title / button; only the prose after the blank line is the body.
 */
static void load_wb_show_displayme(struct LhArchive *arc)
{
    APTR data;
    LONG len;
    STRPTR text;
    STRPTR body;
    LONG i;
    UBYTE *p;
    ULONG text_sz;
    ULONG body_sz;
    struct LoadDisplayMeFields *fields;
    STRPTR title;
    STRPTR gadgets;
    LONG blen;

    if (!arc) {
        return;
    }
    data = NULL;
    text = NULL;
    body = NULL;
    fields = NULL;
    title = NULL;
    gadgets = NULL;
    text_sz = 0;
    body_sz = 0;

    len = LhReadData(arc, (STRPTR)LOAD_WB_DISPLAYME, &data);
    if (len <= 0 || !data) {
        load_log((STRPTR)"no Load.displayme member");
        return;
    }
    text_sz = (ULONG)len + 1UL;
    text = (STRPTR)AllocMem(text_sz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!text) {
        FreeMem(data, (ULONG)len);
        return;
    }
    p = (UBYTE *)data;
    for (i = 0; i < len; i++) {
        text[i] = (char)p[i];
    }
    text[len] = '\0';
    FreeMem(data, (ULONG)len);
    data = NULL;

    /*
     * fields/title/gadgets on heap -- ClassAct RM_OPENREQ runs on this
     * task's 4K stack and load_run already keeps dest[512] live.
     */
    fields = (struct LoadDisplayMeFields *)AllocMem(
        (ULONG)sizeof(struct LoadDisplayMeFields), MEMF_PUBLIC | MEMF_CLEAR);
    title = (STRPTR)AllocMem(LOAD_DM_TITLE_LEN, MEMF_PUBLIC | MEMF_CLEAR);
    gadgets = (STRPTR)AllocMem(LOAD_DM_GADGET_LEN, MEMF_PUBLIC | MEMF_CLEAR);
    if (!fields || !title || !gadgets) {
        goto out;
    }

    load_dm_parse(text, fields);
    load_dm_make_title(fields, title, LOAD_DM_TITLE_LEN);
    load_dm_make_gadget(fields, gadgets, LOAD_DM_GADGET_LEN);

    /*
     * Body: optional Short lead-in (value only, not "Short:"), then prose.
     * Author is kept for a quiet one-liner when present.
     */
    blen = 0;
    while (fields->body[blen] != '\0') {
        blen++;
    }
    body_sz = (ULONG)blen + (ULONG)LOAD_DM_VAL_LEN * 2UL + 64UL;
    body = (STRPTR)AllocMem(body_sz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!body) {
        goto out;
    }
    if (fields->short_s[0] && fields->parts_name[0]
        && Stricmp(fields->short_s, fields->parts_name) != 0) {
        load_wb_cat(body, (LONG)body_sz, fields->short_s);
        load_wb_cat(body, (LONG)body_sz, (STRPTR)"\n\n");
    }
    if (fields->author[0]) {
        load_wb_cat(body, (LONG)body_sz, (STRPTR)"by ");
        load_wb_cat(body, (LONG)body_sz, fields->author);
        load_wb_cat(body, (LONG)body_sz, (STRPTR)"\n\n");
    }
    if (fields->body[0]) {
        load_wb_cat(body, (LONG)body_sz, fields->body);
    } else if (body[0] == '\0') {
        load_wb_cat(body, (LONG)body_sz,
            (STRPTR)"Parts Bin package. Press to continue.");
    }

    load_log((STRPTR)"displayme requester");
    load_wb_open_req(title, body, gadgets);

out:
    if (body) {
        FreeMem(body, body_sz);
    }
    if (gadgets) {
        FreeMem(gadgets, LOAD_DM_GADGET_LEN);
    }
    if (title) {
        FreeMem(title, LOAD_DM_TITLE_LEN);
    }
    if (fields) {
        FreeMem(fields, (ULONG)sizeof(struct LoadDisplayMeFields));
    }
    if (text) {
        FreeMem(text, text_sz);
    }
}

/*
 * Build confirm body from Manifest only (File: Ver= + dest exists?).
 *
 * CRITICAL: do not Open/scan installed binaries here.  After Next the
 * previous code called load_ver_scan_file on every dest (including
 * LIBS:lh.library), reading whole files -- that froze the machine.
 * Installed $VER is checked later during stage, not in the WB UI.
 */
static LONG load_wb_build_summary(struct LoadManifest *man,
    STRPTR to_override, STRPTR out, LONG outlen)
{
    LONG i;
    LONG nshown;
    LONG nsel;
    STRPTR dest;
    char line[LOAD_WB_LINE_MAX];
    char verbuf[40];
    struct LoadVerInfo pkg;
    BPTR lock;
    LONG existed;
    LONG ok;

    if (!man || !out || outlen <= 0) {
        return 0;
    }

    /*
     * dest is LOAD_PATH_LEN (512) -- heap it; load_run already has another
     * dest[512] on the 4K stack for the whole install.
     */
    dest = (STRPTR)AllocMem(LOAD_PATH_LEN, MEMF_PUBLIC | MEMF_CLEAR);
    if (!dest) {
        return 0;
    }

    out[0] = '\0';
    ok = 1;

    load_wb_cat(out, outlen, (STRPTR)"Load ");
    if (man->name[0]) {
        load_wb_cat(out, outlen, (STRPTR)"\"");
        load_wb_cat(out, outlen, man->name);
        load_wb_cat(out, outlen, (STRPTR)"\"");
    } else {
        load_wb_cat(out, outlen, (STRPTR)"package");
    }
    if (man->version[0]) {
        load_wb_cat(out, outlen, (STRPTR)" ");
        load_wb_cat(out, outlen, man->version);
    }
    load_wb_cat(out, outlen, (STRPTR)"\n\n");

    nsel = 0;
    nshown = 0;
    for (i = 0; i < man->nfiles; i++) {
        if (!man->files[i].selected) {
            continue;
        }
        nsel++;
        if (nshown >= LOAD_WB_MAX_LINES) {
            continue;
        }
        if (!load_resolve_dest(dest, LOAD_PATH_LEN, man->files[i].dest,
                to_override, man->install_root)) {
            continue;
        }

        load_ver_clear(&pkg);
        if (man->files[i].have_ver) {
            pkg.have_ver = 1;
            pkg.have_date = man->files[i].have_date;
            pkg.major = man->files[i].ver_major;
            pkg.minor = man->files[i].ver_minor;
            pkg.ymd = man->files[i].ver_ymd;
        }
        /* Existence only -- never Open/Read the dest for $VER here. */
        existed = 0;
        lock = Lock(dest, ACCESS_READ);
        if (lock) {
            existed = 1;
            UnLock(lock);
        }

        line[0] = '\0';
        load_wb_cat((STRPTR)line, LOAD_WB_LINE_MAX, dest);
        load_wb_cat((STRPTR)line, LOAD_WB_LINE_MAX, (STRPTR)"\n  ");
        verbuf[0] = '\0';
        if (load_wb_fmt_ver(&pkg, (STRPTR)verbuf, (LONG)sizeof(verbuf))) {
            load_wb_cat((STRPTR)line, LOAD_WB_LINE_MAX, (STRPTR)verbuf);
        } else {
            load_wb_cat((STRPTR)line, LOAD_WB_LINE_MAX, (STRPTR)"(no Ver=)");
        }
        if (existed) {
            load_wb_cat((STRPTR)line, LOAD_WB_LINE_MAX, (STRPTR)"  (exists)");
        } else {
            load_wb_cat((STRPTR)line, LOAD_WB_LINE_MAX, (STRPTR)"  (new)");
        }
        load_wb_cat((STRPTR)line, LOAD_WB_LINE_MAX, (STRPTR)"\n");
        load_wb_cat(out, outlen, (STRPTR)line);
        nshown++;
    }

    if (nsel > nshown) {
        load_wb_cat(out, outlen, (STRPTR)"...and ");
        load_wb_cat_long(out, outlen, nsel - nshown);
        load_wb_cat(out, outlen, (STRPTR)" more\n");
    }

    /* TO= only when the user relocated relative destinations. */
    if (to_override && to_override[0]) {
        load_wb_cat(out, outlen, (STRPTR)"\nTO=");
        load_wb_cat(out, outlen, to_override);
        load_wb_cat(out, outlen, (STRPTR)"\n");
    }

    load_wb_cat(out, outlen, (STRPTR)"\nLoad this package?");
    FreeMem(dest, LOAD_PATH_LEN);
    return ok;
}

LONG load_wb_confirm_install(struct LoadManifest *man, STRPTR to_override,
    struct LhArchive *arc)
{
    STRPTR body;
    LONG rc;

    /* Autoshow text first, then the install plan. */
    load_wb_show_displayme(arc);

    load_log((STRPTR)"building confirm summary");
    body = (STRPTR)AllocMem(LOAD_WB_BODY_MAX, MEMF_PUBLIC | MEMF_CLEAR);
    if (!body) {
        return 0;
    }
    if (!load_wb_build_summary(man, to_override, body, LOAD_WB_BODY_MAX)) {
        FreeMem(body, LOAD_WB_BODY_MAX);
        return 0;
    }
    load_log((STRPTR)"confirm requester (Load|Cancel)");
    /* "_Load|_Cancel": Load=1, Cancel=0. */
    rc = load_wb_open_req((STRPTR)"Load", body, (STRPTR)"_Load|_Cancel");
    FreeMem(body, LOAD_WB_BODY_MAX);
    return (rc == 1) ? 1 : 0;
}

static LONG load_wbarg_to_path(struct WBArg *arg, STRPTR out, LONG outlen)
{
    if (!arg || !out || outlen <= 1) {
        return 0;
    }
    out[0] = '\0';
    if (arg->wa_Lock == (BPTR)NULL) {
        if (!arg->wa_Name || !arg->wa_Name[0]) {
            return 0;
        }
        Strncpy(out, arg->wa_Name, outlen);
        return out[0] != '\0';
    }
    if (!NameFromLock(arg->wa_Lock, out, outlen)) {
        return 0;
    }
    if (!arg->wa_Name || !arg->wa_Name[0]) {
        return 1;
    }
    if (!AddPart(out, arg->wa_Name, (ULONG)outlen)) {
        return 0;
    }
    return 1;
}

/*
 * True if the named icon (path without .info) has a VERBOSE ToolType.
 */
static LONG load_wb_tooltype_verbose(STRPTR path)
{
    struct DiskObject *dobj;
    STRPTR val;
    LONG yes;
    LONG opened;

    yes = 0;
    if (!path || !path[0]) {
        return 0;
    }
    opened = 0;
    if (!IconBase) {
        IconBase = OpenLibrary((STRPTR)"icon.library", 37L);
        if (!IconBase) {
            return 0;
        }
        opened = 1;
    }
    dobj = GetDiskObject(path);
    if (dobj) {
        if (dobj->do_ToolTypes) {
            val = FindToolType(dobj->do_ToolTypes, (STRPTR)"VERBOSE");
            if (val) {
                yes = 1;
            }
        }
        FreeDiskObject(dobj);
    }
    if (opened) {
        CloseLibrary(IconBase);
        IconBase = NULL;
    }
    return yes;
}

/*
 * Workbench must SelectOutput to a real handle before any Printf/Flush.
 * VERBOSE -> visible CON:; otherwise NIL: (discard).
 */
LONG load_wb_open_verbose_con(void)
{
    if (load_wb_out) {
        return load_wb_out_is_con ? 1 : 0;
    }
    /*
     * AUTO/CLOSE/WAIT: window appears for VERBOSE progress and stays until
     * the user closes it after Load exits (so they can read the log).
     */
    load_wb_out = Open(
        (STRPTR)"CON:0/40/640/200/Load VERBOSE/AUTO/CLOSE/WAIT",
        MODE_NEWFILE);
    if (!load_wb_out) {
        return 0;
    }
    load_wb_out_is_con = 1;
    load_wb_oldout = SelectOutput(load_wb_out);
    return 1;
}

static LONG load_wb_open_nil_out(void)
{
    if (load_wb_out) {
        return 1;
    }
    load_wb_out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    if (!load_wb_out) {
        return 0;
    }
    load_wb_out_is_con = 0;
    load_wb_oldout = SelectOutput(load_wb_out);
    return 1;
}

void load_wb_close_verbose_con(void)
{
    if (!load_wb_out) {
        return;
    }
    Flush(Output());
    if (load_wb_oldout) {
        SelectOutput(load_wb_oldout);
        load_wb_oldout = (BPTR)NULL;
    }
    Close(load_wb_out);
    load_wb_out = (BPTR)NULL;
    load_wb_out_is_con = 0;
}

LONG load_wb_startup(struct WBStartup *wb)
{
    struct LoadArgs *args;
    struct WBArg *arg;
    char prog_path[LOAD_PATH_LEN];
    LONG rc;
    LONG n;

    if (!wb || !wb->sm_ArgList || wb->sm_NumArgs < 1) {
        return RETURN_FAIL;
    }

    /* LoadArgs embeds ~1.5KB of path arrays -- heap, not stack. */
    args = (struct LoadArgs *)load_alloc((ULONG)sizeof(struct LoadArgs));
    if (!args) {
        return RETURN_FAIL;
    }

    args->bin = NULL;
    args->to = NULL;
    args->dest = NULL;
    args->quiet = 0;
    args->force = 0;
    args->rollback = 0;
    args->verbose = 0;
    args->help = 0;
    args->interactive = 1;
    args->bin_path[0] = '\0';
    args->to_path[0] = '\0';
    args->dest_path[0] = '\0';
    prog_path[0] = '\0';

    n = wb->sm_NumArgs;
    arg = wb->sm_ArgList;
    if (n < 2) {
        load_wb_error((STRPTR)"Drop a Parts .bin on Load, or double-click it.",
            0);
        load_wb_close_libs();
        load_free(args, (ULONG)sizeof(struct LoadArgs));
        return RETURN_FAIL;
    }

    if (!load_wbarg_to_path(&arg[1], args->bin_path, LOAD_PATH_LEN)) {
        load_wb_error((STRPTR)"Cannot resolve Bin path from Workbench.", 0);
        load_wb_close_libs();
        load_free(args, (ULONG)sizeof(struct LoadArgs));
        return RETURN_FAIL;
    }
    args->bin = (STRPTR)args->bin_path;

    /* VERBOSE ToolType on Load itself or on the Parts Bin Project icon. */
    if (load_wbarg_to_path(&arg[0], (STRPTR)prog_path, LOAD_PATH_LEN)) {
        if (load_wb_tooltype_verbose((STRPTR)prog_path)) {
            args->verbose = 1;
        }
    }
    if (!args->verbose && load_wb_tooltype_verbose(args->bin_path)) {
        args->verbose = 1;
    }
    /*
     * Workbench: always SelectOutput to a real handle before any dos
     * print.  VERBOSE gets a CON:; otherwise NIL: (CLI already has a
     * console -- this path is WB-only).
     */
    if (args->verbose) {
        if (!load_wb_open_verbose_con()) {
            /* Still run; fall back to NIL: so prints are safe. */
            load_wb_open_nil_out();
        }
        load_log((STRPTR)"Workbench start (VERBOSE)");
        load_log((STRPTR)args->bin_path);
    } else {
        load_wb_open_nil_out();
        /* No console -- keep install prints off the (NIL) stream. */
        args->quiet = 1;
    }

    if (!load_open_lh_library()) {
        load_wb_error(
            (STRPTR)
            "Cannot open lh.library.\n"
            "Check LIBS:lh.library, or run:\n"
            "  Load ROLLBACK DEST=LIBS:lh.library",
            0);
        load_wb_close_verbose_con();
        load_wb_close_libs();
        load_free(args, (ULONG)sizeof(struct LoadArgs));
        return RETURN_FAIL;
    }

    rc = load_run(args);

    if (LhBase) {
        CloseLibrary(LhBase);
        LhBase = NULL;
    }
    load_wb_close_verbose_con();
    load_wb_close_libs();
    load_free(args, (ULONG)sizeof(struct LoadArgs));
    return rc;
}
