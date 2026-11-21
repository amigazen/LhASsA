/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lhasa_cmdhelp.c - Print LhASsA command .help file text for HELP/S.
 *
 * Always dumps the file contents (not a short usage line).  Looks up:
 *   HELP:<lang>/LhASsA/<cmd>.help
 *   HELP:english/LhASsA/<cmd>.help
 * Language comes from locale.library when available.  HELP: is the
 * system assign (typically LOCALE:Help); we do not probe other roots.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <libraries/locale.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct Library *LocaleBase;

#include <proto/locale.h>

#include "lhasa_cmdhelp.h"

#define LHASA_HELP_BUF 512
#define LHASA_LANG_MAX 64
#define LHASA_PATH_MAX 160

extern struct DosLibrary *DOSBase;

static void lhasa_help_copy(STRPTR dst, LONG dstlen, STRPTR src)
{
    LONG i;

    if (!dst || dstlen <= 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    i = 0;
    while (src[i] != '\0' && i < dstlen - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void lhasa_help_cat(STRPTR dst, LONG dstlen, STRPTR src)
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
 * Fill lang with a HELP drawer name (e.g. "english").  Returns 1 if set.
 */
static LONG lhasa_help_locale_lang(STRPTR lang, LONG langlen)
{
    struct Locale *loc;
    STRPTR name;
    LONG i;
    LONG n;

    if (!lang || langlen <= 0) {
        return 0;
    }
    lang[0] = '\0';

    LocaleBase = OpenLibrary((STRPTR)"locale.library", 38L);
    if (!LocaleBase) {
        return 0;
    }

    loc = OpenLocale(NULL);
    if (!loc) {
        CloseLibrary(LocaleBase);
        LocaleBase = NULL;
        return 0;
    }

    name = NULL;
    if (loc->loc_LanguageName && loc->loc_LanguageName[0]) {
        name = loc->loc_LanguageName;
    } else if (loc->loc_LocaleName && loc->loc_LocaleName[0]) {
        name = loc->loc_LocaleName;
    }

    if (name) {
        n = 0;
        while (name[n] != '\0' && name[n] != '.' && n < langlen - 1) {
            lang[n] = name[n];
            n++;
        }
        lang[n] = '\0';
        for (i = 0; lang[i] != '\0'; i++) {
            if (lang[i] >= 'A' && lang[i] <= 'Z') {
                lang[i] = (char)(lang[i] - 'A' + 'a');
            }
        }
    }

    CloseLocale(loc);
    CloseLibrary(LocaleBase);
    LocaleBase = NULL;

    return (lang[0] != '\0') ? 1 : 0;
}

static LONG lhasa_help_build_path(STRPTR out, LONG outlen, STRPTR lang,
    STRPTR cmd_name)
{
    if (!out || outlen < 32 || !lang || !lang[0]
        || !cmd_name || !cmd_name[0]) {
        return 0;
    }
    out[0] = '\0';
    lhasa_help_cat(out, outlen, (STRPTR)"HELP:");
    lhasa_help_cat(out, outlen, lang);
    lhasa_help_cat(out, outlen, (STRPTR)"/LhASsA/");
    lhasa_help_cat(out, outlen, cmd_name);
    lhasa_help_cat(out, outlen, (STRPTR)".help");
    return 1;
}

static LONG lhasa_help_print_file(STRPTR path)
{
    BPTR fh;
    UBYTE *buf;
    LONG n;

    if (!path || !path[0]) {
        return 0;
    }
    fh = Open(path, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        return 0;
    }
    buf = (UBYTE *)AllocMem(LHASA_HELP_BUF, MEMF_PUBLIC);
    if (!buf) {
        Close(fh);
        return 0;
    }
    for (;;) {
        n = Read(fh, buf, LHASA_HELP_BUF);
        if (n < 0) {
            FreeMem(buf, LHASA_HELP_BUF);
            Close(fh);
            return 0;
        }
        if (n == 0) {
            break;
        }
        if (Write(Output(), buf, n) != n) {
            FreeMem(buf, LHASA_HELP_BUF);
            Close(fh);
            return 0;
        }
    }
    FreeMem(buf, LHASA_HELP_BUF);
    Close(fh);
    Flush(Output());
    return 1;
}

static LONG lhasa_help_try(STRPTR lang, STRPTR cmd_name, STRPTR path,
    LONG pathlen)
{
    if (!lhasa_help_build_path(path, pathlen, lang, cmd_name)) {
        return 0;
    }
    return lhasa_help_print_file(path);
}

LONG lhasa_print_cmd_help(STRPTR cmd_name)
{
    char lang[LHASA_LANG_MAX];
    char path[LHASA_PATH_MAX];
    char eng[16];
    LONG have_lang;

    if (!cmd_name || !cmd_name[0]) {
        return 0;
    }

    lhasa_help_copy((STRPTR)eng, (LONG)sizeof(eng), (STRPTR)"english");
    lang[0] = '\0';
    have_lang = lhasa_help_locale_lang((STRPTR)lang, LHASA_LANG_MAX);

    if (have_lang) {
        if (lhasa_help_try((STRPTR)lang, cmd_name, (STRPTR)path,
                LHASA_PATH_MAX)) {
            return 1;
        }
    }

    if (lhasa_help_try((STRPTR)eng, cmd_name, (STRPTR)path,
            LHASA_PATH_MAX)) {
        return 1;
    }

    Printf("%s: cannot open help file\n", (LONG)cmd_name);
    Printf("  expected HELP:english/LhASsA/%s.help\n", (LONG)cmd_name);
    Flush(Output());
    return 0;
}
