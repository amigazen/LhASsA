/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * load_run.c - Manifest parse, variant pick, deps, $VER gate, install, ROLLBACK.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <libraries/lh.h>
#include <proto/lh.h>

#include "load_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;
extern struct Library *LhBase;

/* Set for the duration of load_run so load_print_error can raise a requester. */
static ULONG load_run_interactive = 0;
static ULONG load_run_verbose = 0;

void load_log(STRPTR msg)
{
    if (!load_run_verbose || !msg) {
        return;
    }
    Printf("Load: %s\n", (LONG)msg);
    Flush(Output());
}

#ifndef AFF_68020
#define AFF_68020 (1L << 1)
#endif
#ifndef AFF_68030
#define AFF_68030 (1L << 2)
#endif
#ifndef AFF_68040
#define AFF_68040 (1L << 3)
#endif
#ifndef AFF_68881
#define AFF_68881 (1L << 4)
#endif
#ifndef AFF_68882
#define AFF_68882 (1L << 5)
#endif
#ifndef AFF_FPU40
#define AFF_FPU40 (1L << 6)
#endif
#ifndef AFF_68060
#define AFF_68060 (1L << 7)
#endif

void load_print_error(STRPTR msg, LONG code)
{
    if (!msg) {
        return;
    }
    if (!load_run_interactive) {
        if (code) {
            Printf("Load: %s (IoErr %ld)\n", (LONG)msg, code);
        } else {
            Printf("Load: %s\n", (LONG)msg);
        }
        Flush(Output());
    } else {
        load_wb_error(msg, code);
    }
}

void load_print_usage(void)
{
    /* Never dump a short synopsis; full text is Load HELP (.help file). */
    Printf("Load: type Load HELP for documentation.\n", 0);
    Flush(Output());
}

LONG load_path_is_absolute(STRPTR path)
{
    LONG i;

    if (!path) {
        return 0;
    }
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == ':') {
            return 1;
        }
    }
    return 0;
}

LONG load_path_is_volume_root(STRPTR path)
{
    LONG len;
    STRPTR fp;

    if (!path || !path[0]) {
        return 1;
    }
    len = 0;
    while (path[len] != '\0') {
        len++;
    }
    if (path[len - 1] == ':') {
        return 1;
    }
    fp = FilePart(path);
    if (!fp || !fp[0]) {
        return 1;
    }
    return 0;
}

LONG load_resolve_dest(STRPTR out, LONG outlen, STRPTR dest,
    STRPTR to_override, STRPTR install_root)
{
    STRPTR base;
    char seg[LOAD_VAL_LEN];
    LONG i;
    LONG j;

    if (!out || outlen <= 0 || !dest || !dest[0]) {
        return 0;
    }

    if (load_path_is_absolute(dest)) {
        Strncpy(out, dest, outlen);
        if (load_path_is_volume_root(out)) {
            return 0;
        }
        return 1;
    }

    base = NULL;
    if (to_override && to_override[0]) {
        base = to_override;
    } else if (install_root && install_root[0]) {
        base = install_root;
    } else {
        base = (STRPTR)"Work:";
    }

    Strncpy(out, base, outlen);

    i = 0;
    while (dest[i] != '\0') {
        while (dest[i] == '/') {
            i++;
        }
        if (dest[i] == '\0') {
            break;
        }
        j = 0;
        while (dest[i] != '\0' && dest[i] != '/' && j < LOAD_VAL_LEN - 1) {
            seg[j++] = dest[i++];
        }
        seg[j] = '\0';
        if (seg[0] == '\0') {
            continue;
        }
        if (!AddPart(out, (STRPTR)seg, (ULONG)outlen)) {
            return 0;
        }
    }
    if (load_path_is_volume_root(out)) {
        return 0;
    }
    return 1;
}

int load_host_cpu_rank(void)
{
    UWORD attn;

    attn = SysBase->AttnFlags;
    if (attn & AFF_68060) {
        return 60;
    }
    if (attn & AFF_68040) {
        return 40;
    }
    if (attn & AFF_68030) {
        return 30;
    }
    if (attn & AFF_68020) {
        return 20;
    }
    return 0;
}

static int load_host_has_fpu881(void)
{
    UWORD attn;

    attn = SysBase->AttnFlags;
    if (attn & (AFF_68881 | AFF_68882 | AFF_FPU40)) {
        return 1;
    }
    return 0;
}

static int load_host_has_fpu882(void)
{
    /* Strict 68882 bit; embedded 040/060 FPU is not tagged as .882. */
    if (SysBase->AttnFlags & AFF_68882) {
        return 1;
    }
    return 0;
}

static int load_host_has_mmu(void)
{
    UWORD attn;

    attn = SysBase->AttnFlags;
    if (attn & (AFF_68030 | AFF_68040 | AFF_68060)) {
        return 1;
    }
    return 0;
}

static int load_host_has_wos(void)
{
    if (FindName(&SysBase->LibList, (STRPTR)"ppc.library") != NULL) {
        return 1;
    }
    if (FindName(&SysBase->LibList, (STRPTR)"warp.library") != NULL) {
        return 1;
    }
    return 0;
}

static int load_host_has_pup(void)
{
    if (FindName(&SysBase->LibList, (STRPTR)"powerpc.library") != NULL) {
        return 1;
    }
    return 0;
}

int load_cpu_rank(STRPTR cpu)
{
    if (!cpu || !cpu[0]) {
        return 0;
    }
    if (Stricmp(cpu, (STRPTR)"any") == 0) {
        return 0;
    }
    if (Stricmp(cpu, (STRPTR)"wos") == 0 || Stricmp(cpu, (STRPTR)"pup") == 0) {
        return 0;
    }
    if (Stricmp(cpu, (STRPTR)"68000") == 0 || Stricmp(cpu, (STRPTR)"68010") == 0) {
        return 0;
    }
    if (Stricmp(cpu, (STRPTR)"68020") == 0) {
        return 20;
    }
    if (Stricmp(cpu, (STRPTR)"68030") == 0) {
        return 30;
    }
    if (Stricmp(cpu, (STRPTR)"68040") == 0) {
        return 40;
    }
    if (Stricmp(cpu, (STRPTR)"68060") == 0) {
        return 60;
    }
    return -1;
}

/*
 * 1 = host can run this variant; 0 = no.
 */
static int load_variant_runnable(const struct LoadFileVar *fv, int host_cpu)
{
    int rank;

    if (!fv) {
        return 0;
    }
    if (fv->wos) {
        return load_host_has_wos();
    }
    if (fv->pup) {
        return load_host_has_pup();
    }
    rank = load_cpu_rank(fv->cpu);
    if (rank < 0 || rank > host_cpu) {
        return 0;
    }
    if (fv->fpu882 && !load_host_has_fpu882()) {
        return 0;
    }
    if (fv->fpu881 && !fv->fpu882 && !load_host_has_fpu881()) {
        return 0;
    }
    if (fv->mmu && !load_host_has_mmu()) {
        return 0;
    }
    return 1;
}

/*
 * Higher is better among runnable candidates.
 * Prefer higher CPU; then FPU when host has FPU; then MMU when host has MMU.
 */
static int load_variant_score(const struct LoadFileVar *fv, int host_cpu)
{
    int score;
    int rank;

    rank = load_cpu_rank(fv->cpu);
    if (rank < 0) {
        rank = 0;
    }
    if (fv->wos || fv->pup) {
        score = 1000;
    } else {
        score = rank * 100;
    }
    if (fv->fpu882) {
        score += 30;
    } else if (fv->fpu881) {
        score += 20;
    } else if (load_host_has_fpu881()) {
        /* Prefer a no-FPU universal build less when FPU builds exist — score
         * without FPU stays lower so FPU variants win when runnable. */
        score += 0;
    }
    if (fv->mmu) {
        score += 5;
    }
    (void)host_cpu;
    return score;
}

LONG load_select_variants(struct LoadManifest *man)
{
    LONG i;
    LONG j;
    int host;
    int best_score;
    LONG best_idx;
    int score;
    int seen;

    if (!man) {
        return 0;
    }

    host = load_host_cpu_rank();

    for (i = 0; i < man->nfiles; i++) {
        man->files[i].selected = 0;
    }

    for (i = 0; i < man->nfiles; i++) {
        seen = 0;
        for (j = 0; j < i; j++) {
            if (Stricmp(man->files[j].dest, man->files[i].dest) == 0) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }

        best_score = -1;
        best_idx = -1;
        for (j = i; j < man->nfiles; j++) {
            if (Stricmp(man->files[j].dest, man->files[i].dest) != 0) {
                continue;
            }
            if (!load_variant_runnable(&man->files[j], host)) {
                continue;
            }
            score = load_variant_score(&man->files[j], host);
            if (score > best_score) {
                best_score = score;
                best_idx = j;
            }
        }
        if (best_idx < 0) {
            load_print_error((STRPTR)"no runnable variant for destination", 0);
            Printf("Load:   %s\n", (LONG)man->files[i].dest);
            Flush(Output());
            return 0;
        }
        man->files[best_idx].selected = 1;
    }
    return 1;
}

LONG load_check_deps(struct LoadManifest *man)
{
    ULONG need_maj;
    ULONG need_min;
    LONG i;
    struct Library *lib;
    ULONG got_maj;
    ULONG got_min;

    if (!man) {
        return 0;
    }

    /* OS: major.minor — compare against exec.library (SysBase). */
    if (man->os_min[0]
        && man->os_min[0] >= '0' && man->os_min[0] <= '9') {
        if (!load_ver_parse_dotted(man->os_min, &need_maj, &need_min)) {
            load_print_error((STRPTR)"bad OS: version in Manifest", 0);
            return 0;
        }
        got_maj = (ULONG)SysBase->LibNode.lib_Version;
        got_min = (ULONG)SysBase->LibNode.lib_Revision;
        if (got_maj < need_maj
            || (got_maj == need_maj && got_min < need_min)) {
            load_print_error((STRPTR)"OS version too old for package", 0);
            Printf("Load:   need exec %s, have %ld.%ld\n",
                (LONG)man->os_min, got_maj, got_min);
            Flush(Output());
            return 0;
        }
    }

    for (i = 0; i < man->nreqs; i++) {
        need_maj = 0;
        need_min = 0;
        if (man->reqs[i].minver[0]) {
            if (!load_ver_parse_dotted(man->reqs[i].minver, &need_maj,
                    &need_min)) {
                load_print_error((STRPTR)"bad Requires: Min= version", 0);
                Printf("Load:   %s\n", (LONG)man->reqs[i].name);
                Flush(Output());
                return 0;
            }
        }
        lib = OpenLibrary(man->reqs[i].name, need_maj);
        if (!lib) {
            load_print_error((STRPTR)"missing required library", 0);
            Printf("Load:   %s", (LONG)man->reqs[i].name);
            if (man->reqs[i].minver[0]) {
                Printf(" Min=%s", (LONG)man->reqs[i].minver);
            }
            Printf("\n", 0);
            if (man->reqs[i].url[0]) {
                Printf("Load:   obtain from %s\n", (LONG)man->reqs[i].url);
            }
            Flush(Output());
            return 0;
        }
        got_maj = (ULONG)lib->lib_Version;
        got_min = (ULONG)lib->lib_Revision;
        CloseLibrary(lib);
        if (got_maj < need_maj
            || (got_maj == need_maj && got_min < need_min)) {
            load_print_error((STRPTR)"required library too old", 0);
            Printf("Load:   %s need %s, have %ld.%ld\n",
                (LONG)man->reqs[i].name,
                (LONG)man->reqs[i].minver,
                got_maj, got_min);
            if (man->reqs[i].url[0]) {
                Printf("Load:   obtain from %s\n", (LONG)man->reqs[i].url);
            }
            Flush(Output());
            return 0;
        }
    }
    return 1;
}

static void load_skip_ws(STRPTR *pp)
{
    while (**pp == ' ' || **pp == '\t') {
        (*pp)++;
    }
}

static LONG load_copy_token(STRPTR dst, LONG dstlen, STRPTR *pp)
{
    LONG n;

    n = 0;
    while (**pp != '\0' && **pp != ' ' && **pp != '\t' && **pp != '\r'
        && **pp != '\n' && n < dstlen - 1) {
        dst[n++] = *(*pp)++;
    }
    dst[n] = '\0';
    return n > 0 ? 1 : 0;
}

static LONG load_parse_file_line(STRPTR line, struct LoadFileVar *out)
{
    STRPTR p;
    char tok[LOAD_VAL_LEN];
    struct LoadVerInfo vi;

    out->dest[0] = '\0';
    out->from[0] = '\0';
    out->cpu[0] = '\0';
    out->fpu881 = 0;
    out->fpu882 = 0;
    out->mmu = 0;
    out->wos = 0;
    out->pup = 0;
    out->selected = 0;
    out->have_ver = 0;
    out->have_date = 0;
    out->ver_major = 0;
    out->ver_minor = 0;
    out->ver_ymd = 0;

    p = line;
    load_skip_ws(&p);
    if (Strnicmp(p, (STRPTR)"File:", 5) != 0) {
        return 0;
    }
    p += 5;
    load_skip_ws(&p);
    if (!load_copy_token(out->dest, LOAD_VAL_LEN, &p)) {
        return 0;
    }

    for (;;) {
        load_skip_ws(&p);
        if (*p == '\0' || *p == '#' || *p == '\r' || *p == '\n') {
            break;
        }
        if (!load_copy_token((STRPTR)tok, LOAD_VAL_LEN, &p)) {
            break;
        }
        if (Strnicmp((STRPTR)tok, (STRPTR)"From=", 5) == 0) {
            Strncpy(out->from, (STRPTR)(tok + 5), LOAD_VAL_LEN);
        } else if (Strnicmp((STRPTR)tok, (STRPTR)"CPU=", 4) == 0) {
            Strncpy(out->cpu, (STRPTR)(tok + 4), (LONG)sizeof(out->cpu));
            if (Stricmp(out->cpu, (STRPTR)"wos") == 0) {
                out->wos = 1;
            } else if (Stricmp(out->cpu, (STRPTR)"pup") == 0) {
                out->pup = 1;
            }
        } else if (Strnicmp((STRPTR)tok, (STRPTR)"FPU=", 4) == 0) {
            if (Stricmp((STRPTR)(tok + 4), (STRPTR)"882") == 0) {
                out->fpu882 = 1;
                out->fpu881 = 1;
            } else if (Stricmp((STRPTR)(tok + 4), (STRPTR)"881") == 0) {
                out->fpu881 = 1;
            }
        } else if (Strnicmp((STRPTR)tok, (STRPTR)"MMU=", 4) == 0) {
            if (tok[4] == '1' || Stricmp((STRPTR)(tok + 4), (STRPTR)"yes") == 0) {
                out->mmu = 1;
            }
        } else if (Strnicmp((STRPTR)tok, (STRPTR)"Ver=", 4) == 0) {
            if (load_ver_parse_short((STRPTR)(tok + 4), &vi) && vi.have_ver) {
                out->have_ver = 1;
                out->have_date = vi.have_date;
                out->ver_major = vi.major;
                out->ver_minor = vi.minor;
                out->ver_ymd = vi.ymd;
            }
        }
    }

    if (!out->from[0]) {
        return 0;
    }
    if (!out->cpu[0]) {
        Strncpy(out->cpu, (STRPTR)"68000", (LONG)sizeof(out->cpu));
    }
    return 1;
}

static LONG load_parse_requires_line(STRPTR line, struct LoadRequire *out)
{
    STRPTR p;
    char tok[LOAD_VAL_LEN];

    out->name[0] = '\0';
    out->minver[0] = '\0';
    out->url[0] = '\0';

    p = line;
    load_skip_ws(&p);
    if (Strnicmp(p, (STRPTR)"Requires:", 9) != 0) {
        return 0;
    }
    p += 9;
    load_skip_ws(&p);
    if (!load_copy_token(out->name, LOAD_VAL_LEN, &p)) {
        return 0;
    }
    for (;;) {
        load_skip_ws(&p);
        if (*p == '\0' || *p == '#' || *p == '\r' || *p == '\n') {
            break;
        }
        if (!load_copy_token((STRPTR)tok, LOAD_VAL_LEN, &p)) {
            break;
        }
        if (Strnicmp((STRPTR)tok, (STRPTR)"Min=", 4) == 0) {
            Strncpy(out->minver, (STRPTR)(tok + 4), (LONG)sizeof(out->minver));
        } else if (Strnicmp((STRPTR)tok, (STRPTR)"URL=", 4) == 0) {
            Strncpy(out->url, (STRPTR)(tok + 4), LOAD_VAL_LEN);
        }
    }
    return 1;
}

static LONG load_parse_kv(STRPTR line, STRPTR key, LONG keylen,
    STRPTR val, LONG vallen)
{
    STRPTR p;
    LONG n;

    p = line;
    load_skip_ws(&p);
    if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n') {
        return 0;
    }
    n = 0;
    while (*p != '\0' && *p != ':' && n < keylen - 1) {
        key[n++] = *p++;
    }
    key[n] = '\0';
    if (*p != ':') {
        return 0;
    }
    p++;
    load_skip_ws(&p);
    n = 0;
    while (*p != '\0' && *p != '\r' && *p != '\n' && n < vallen - 1) {
        if (*p == '#' && (n == 0 || val[n - 1] == ' ')) {
            break;
        }
        val[n++] = *p++;
    }
    while (n > 0 && (val[n - 1] == ' ' || val[n - 1] == '\t')) {
        n--;
    }
    val[n] = '\0';
    return 1;
}

LONG load_parse_manifest(STRPTR text, LONG len, struct LoadManifest *man)
{
    LONG i;
    LONG start;
    STRPTR line;
    STRPTR key;
    STRPTR val;
    struct LoadFileVar *fv;
    struct LoadRequire *req;
    LONG llen;
    LONG ok;

    if (!text || !man || len < 0) {
        return 0;
    }

    /*
     * Parse workspace (line + key + val + fv + req) is ~1.9KB together --
     * too large for the 4K stack; keep on the heap.
     */
    line = (STRPTR)load_alloc(512);
    key  = (STRPTR)load_alloc(LOAD_KEY_LEN);
    val  = (STRPTR)load_alloc(LOAD_VAL_LEN);
    fv   = (struct LoadFileVar *)load_alloc((ULONG)sizeof(struct LoadFileVar));
    req  = (struct LoadRequire *)load_alloc((ULONG)sizeof(struct LoadRequire));
    if (!line || !key || !val || !fv || !req) {
        load_free(line, 512);
        load_free(key, LOAD_KEY_LEN);
        load_free(val, LOAD_VAL_LEN);
        load_free(fv, (ULONG)sizeof(struct LoadFileVar));
        load_free(req, (ULONG)sizeof(struct LoadRequire));
        return 0;
    }

    man->name[0] = '\0';
    man->version[0] = '\0';
    man->os_min[0] = '\0';
    Strncpy(man->install_root, (STRPTR)"Work:", LOAD_VAL_LEN);
    man->nfiles = 0;
    man->nreqs = 0;
    ok = 1;

    i = 0;
    while (i < len) {
        start = i;
        while (i < len && text[i] != '\n' && text[i] != '\r') {
            i++;
        }
        llen = i - start;
        if (llen >= 512) {
            llen = 511;
        }
        if (llen > 0) {
            CopyMem(text + start, line, (ULONG)llen);
        }
        line[llen] = '\0';
        while (i < len && (text[i] == '\n' || text[i] == '\r')) {
            i++;
        }

        {
            STRPTR lp;

            lp = line;
            load_skip_ws(&lp);
            if (*lp == '#' || *lp == '\0') {
                continue;
            }
            if (Strnicmp(lp, (STRPTR)"File:", 5) == 0) {
                if (man->nfiles >= LOAD_MAX_FILES) {
                    load_print_error((STRPTR)"too many File: lines", 0);
                    ok = 0;
                    break;
                }
                if (!load_parse_file_line(lp, fv)) {
                    load_print_error((STRPTR)"bad File: line", 0);
                    ok = 0;
                    break;
                }
                man->files[man->nfiles] = *fv;
                man->nfiles++;
                continue;
            }
            if (Strnicmp(lp, (STRPTR)"Requires:", 9) == 0) {
                if (man->nreqs >= LOAD_MAX_REQS) {
                    load_print_error((STRPTR)"too many Requires: lines", 0);
                    ok = 0;
                    break;
                }
                if (!load_parse_requires_line(lp, req)) {
                    load_print_error((STRPTR)"bad Requires: line", 0);
                    ok = 0;
                    break;
                }
                man->reqs[man->nreqs] = *req;
                man->nreqs++;
                continue;
            }
            if (load_parse_kv(lp, key, LOAD_KEY_LEN, val, LOAD_VAL_LEN)) {
                if (Stricmp(key, (STRPTR)"Name") == 0) {
                    Strncpy(man->name, val, LOAD_VAL_LEN);
                } else if (Stricmp(key, (STRPTR)"Version") == 0) {
                    Strncpy(man->version, val, LOAD_VAL_LEN);
                } else if (Stricmp(key, (STRPTR)"InstallRoot") == 0) {
                    Strncpy(man->install_root, val, LOAD_VAL_LEN);
                } else if (Stricmp(key, (STRPTR)"OS") == 0) {
                    Strncpy(man->os_min, val, (LONG)sizeof(man->os_min));
                }
            }
        }
    }

    load_free(line, 512);
    load_free(key, LOAD_KEY_LEN);
    load_free(val, LOAD_VAL_LEN);
    load_free(fv, (ULONG)sizeof(struct LoadFileVar));
    load_free(req, (ULONG)sizeof(struct LoadRequire));
    return ok;
}

static LONG load_dest_exists(STRPTR path)
{
    BPTR lock;

    lock = Lock(path, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    return 0;
}

/* One member extracted to SYS:T/Load, ready to place after archive close. */
struct LoadStaged {
    char dest[LOAD_PATH_LEN];
    char from[LOAD_VAL_LEN];
    char cand[LOAD_PATH_LEN];
    char token[LOAD_TOKEN_LEN];
    char bak[LOAD_PATH_LEN];
    UWORD crc;
    LONG have_bak;
    LONG skip;
    LONG is_lh_lib;
    LONG committed; /* 1 after successful place; used by abort */
};

/*
 * Drop SYS:T/Load artefacts for one staged entry.  Dest is not touched
 * (caller restores committed dests first when aborting a transaction).
 */
static void load_discard_staged(struct LoadStaged *st)
{
    char side[LOAD_PATH_LEN];

    if (!st) {
        return;
    }
    if (st->cand[0]) {
        DeleteFile(st->cand);
        st->cand[0] = '\0';
    }
    if (st->have_bak && st->bak[0]) {
        DeleteFile(st->bak);
        st->bak[0] = '\0';
        if (st->token[0]) {
            side[0] = '\0';
            if (load_sidecar_path((STRPTR)side, LOAD_PATH_LEN, st->token)) {
                DeleteFile((STRPTR)side);
            }
        }
        st->have_bak = 0;
    }
}

/*
 * Undo a successful place: restore from bak when we had a prior file,
 * otherwise delete the newly installed dest.
 */
static LONG load_uncommit_staged(struct LoadStaged *st)
{
    if (!st || st->skip || !st->committed) {
        return 1;
    }
    if (st->have_bak && st->bak[0]) {
        if (load_dest_exists(st->dest)) {
            if (!load_clear_write_protect(st->dest)) {
                return 0;
            }
            if (!DeleteFile(st->dest)) {
                return 0;
            }
        }
        if (!load_copy_file(st->bak, st->dest)) {
            return 0;
        }
    } else if (st->dest[0] && load_dest_exists(st->dest)) {
        if (!load_clear_write_protect(st->dest)) {
            return 0;
        }
        if (!DeleteFile(st->dest)) {
            return 0;
        }
    }
    st->committed = 0;
    return 1;
}

/*
 * Abort an install transaction using per-entry committed flags (lh.library
 * may have been placed out of order).  Reverse committed places latest-first,
 * then discard uncommitted T: candidates.  On restore failure, or for
 * lh.library, keep bak+sidecar so ROLLBACK / open-recovery can finish.
 */
static void load_abort_install(struct LoadStaged *staged, LONG nstaged)
{
    LONG i;
    LONG ok;

    if (!staged || nstaged <= 0) {
        return;
    }
    load_log((STRPTR)"abort: rolling back transaction");

    /* Undo successful places from the end of the table backwards. */
    for (i = nstaged - 1; i >= 0; i--) {
        if (!staged[i].committed) {
            continue;
        }
        ok = load_uncommit_staged(&staged[i]);
        if (!ok) {
            load_print_error(
                (STRPTR)"abort: could not restore; bak kept for ROLLBACK",
                0);
            if (staged[i].dest[0]) {
                Printf("Load:   DEST=%s\n", (LONG)staged[i].dest);
                Flush(Output());
            }
            if (staged[i].cand[0]) {
                DeleteFile(staged[i].cand);
                staged[i].cand[0] = '\0';
            }
            continue;
        }
        if (staged[i].is_lh_lib && staged[i].have_bak) {
            if (staged[i].cand[0]) {
                DeleteFile(staged[i].cand);
                staged[i].cand[0] = '\0';
            }
            continue;
        }
        load_discard_staged(&staged[i]);
    }

    /* Drop uncommitted candidates; keep bak when dest missing or lh. */
    for (i = 0; i < nstaged; i++) {
        if (staged[i].committed) {
            continue;
        }
        if (staged[i].cand[0]) {
            DeleteFile(staged[i].cand);
            staged[i].cand[0] = '\0';
        }
        if (staged[i].is_lh_lib && staged[i].have_bak && staged[i].bak[0]) {
            load_log((STRPTR)"abort: keeping lh.library bak");
            continue;
        }
        if (staged[i].have_bak && staged[i].bak[0]
            && staged[i].dest[0] && !load_dest_exists(staged[i].dest)) {
            load_print_error(
                (STRPTR)"abort: dest missing; bak kept for ROLLBACK", 0);
            Printf("Load:   DEST=%s\n", (LONG)staged[i].dest);
            Flush(Output());
            continue;
        }
        load_discard_staged(&staged[i]);
    }
}

/*
 * Extract one member to T: and apply skip rules.  Does not place yet
 * so lh.library can be closed/flushed after all archive I/O is done.
 * Prefer Manifest File: Ver= (from Handle) for the candidate version;
 * after extract, optionally double-check the real $VER: matches.
 * Returns: 1 staged, 0 fail (no T: leftovers), 2 skipped (cand deleted).
 */
static LONG load_stage_one(struct LhArchive *arc, struct LoadFileVar *fv,
    STRPTR dest, struct LoadArgs *args, struct LoadStaged *out)
{
    struct LoadVerInfo cand_ver;
    struct LoadVerInfo inst_ver;
    struct LoadVerInfo got_ver;
    int cmp;
    LONG vrc;
    LONG has_ver;
    char leaf[16];
    LONG i;
    UWORD crc;
    STRPTR from;

    if (!out || !fv) {
        return 0;
    }
    from = fv->from;
    out->dest[0] = '\0';
    out->from[0] = '\0';
    out->cand[0] = '\0';
    out->token[0] = '\0';
    out->bak[0] = '\0';
    out->crc = 0;
    out->have_bak = 0;
    out->skip = 0;
    out->is_lh_lib = 0;
    out->committed = 0;

    if (load_path_is_volume_root(dest)) {
        load_print_error((STRPTR)"refusing to extract onto a volume root", 0);
        return 0;
    }

    Strncpy(out->dest, dest, LOAD_PATH_LEN);
    Strncpy(out->from, from, LOAD_VAL_LEN);
    out->is_lh_lib = load_dest_is_lh_library(dest);

    /*
     * Extract straight to T: -- never LhReadData whole libraries into RAM
     * (that exhausted memory early on and left the OS unable to LoadSeg
     * any further libraries).
     */
    if (!load_ensure_tdir()) {
        load_print_error((STRPTR)"cannot create SYS:T/Load", IoErr());
        return 0;
    }
    if (!load_alloc_token(out->token, LOAD_TOKEN_LEN)) {
        load_print_error((STRPTR)"cannot allocate SYS:T/Load token", 0);
        return 0;
    }
    leaf[0] = 'L';
    leaf[1] = 'H';
    leaf[2] = 'c';
    for (i = 0; out->token[i] != '\0' && i < 8; i++) {
        leaf[3 + i] = out->token[i];
    }
    leaf[3 + i] = '\0';
    Strncpy(out->cand, (STRPTR)LOAD_TDIR, LOAD_PATH_LEN);
    if (!AddPart(out->cand, (STRPTR)leaf, LOAD_PATH_LEN)) {
        out->cand[0] = '\0';
        return 0;
    }
    DeleteFile(out->cand);
    if (!LhExtractEntry(arc, from, out->cand)) {
        out->cand[0] = '\0';
        load_print_error((STRPTR)"cannot extract archive member", LhErr());
        Printf("Load:   %s\n", (LONG)from);
        Flush(Output());
        return 0;
    }
    if (!load_crc16_file(out->cand, &crc)) {
        load_discard_staged(out);
        load_print_error((STRPTR)"cannot CRC candidate", IoErr());
        return 0;
    }
    out->crc = crc;

    /*
     * Per-file upgrade gate against the installed copy only.
     * Prefer Manifest File: Ver= written by Handle (staged $VER).
     * After extract, double-check the candidate $VER: matches Ver= when
     * both are present -- mismatch means a corrupt/wrong Bin member.
     *
     * No Ver= (data/.help) -> size + CRC16(first/last 256 bytes).
     */
    load_ver_clear(&cand_ver);
    has_ver = 0;
    if (fv->have_ver) {
        cand_ver.have_ver = 1;
        cand_ver.have_date = fv->have_date;
        cand_ver.major = fv->ver_major;
        cand_ver.minor = fv->ver_minor;
        cand_ver.ymd = fv->ver_ymd;
        has_ver = 1;
        /* Double-check extracted member against Manifest Ver=. */
        vrc = load_ver_scan_file(out->cand, &got_ver);
        if (vrc == 1 && got_ver.have_ver) {
            if (got_ver.major != fv->ver_major
                || got_ver.minor != fv->ver_minor) {
                load_discard_staged(out);
                load_print_error(
                    (STRPTR)"member $VER: does not match Manifest Ver=",
                    0);
                Printf("Load:   %s\n", (LONG)from);
                Flush(Output());
                return 0;
            }
            if (!cand_ver.have_date && got_ver.have_date) {
                cand_ver.have_date = 1;
                cand_ver.ymd = got_ver.ymd;
            }
        }
    } else {
        /*
         * Older Bins without File: Ver= -- fall back to scanning the
         * extracted candidate (already on T:, no extra extract).
         */
        vrc = load_ver_scan_file(out->cand, &cand_ver);
        if (vrc == 1 && cand_ver.have_ver) {
            has_ver = 1;
        } else {
            load_ver_clear(&cand_ver);
        }
    }

    if (load_dest_exists(dest)) {
        if (has_ver) {
            vrc = load_ver_scan_file(dest, &inst_ver);
            if (vrc != 1 || !inst_ver.have_ver) {
                has_ver = 0;
                load_ver_clear(&inst_ver);
            } else {
                cmp = load_ver_compare(&cand_ver, &inst_ver);
                if (cmp < 0 && !args->force) {
                    load_discard_staged(out);
                    if (!args->quiet) {
                        Printf("Load: skip older %s\n", (LONG)dest);
                        Flush(Output());
                    }
                    out->skip = 1;
                    return 2;
                }
                if (cmp == 0 && !args->force) {
                    load_discard_staged(out);
                    if (!args->quiet) {
                        Printf("Load: skip same %s\n", (LONG)dest);
                        Flush(Output());
                    }
                    out->skip = 1;
                    return 2;
                }
            }
        }
        if (!has_ver) {
            if (!args->force
                && load_files_same_content(out->cand, dest)) {
                load_discard_staged(out);
                if (!args->quiet) {
                    Printf("Load: skip same %s\n", (LONG)dest);
                    Flush(Output());
                }
                out->skip = 1;
                return 2;
            }
        }
        /* Reuse out->token so LHc / LHb / LHk share one id. */
        if (!load_backup_to_t(dest, out->token, LOAD_TOKEN_LEN,
                out->bak, LOAD_PATH_LEN)) {
            load_discard_staged(out);
            load_print_error(
                (STRPTR)"cannot backup existing file to SYS:T/Load",
                IoErr());
            return 0;
        }
        out->have_bak = 1;
        if (!args->quiet) {
            Printf("Load: bak %s\n", (LONG)out->bak);
            Flush(Output());
        }
    }
    return 1;
}

/*
 * Place staged candidate FIRST, then soft-flush the resident copy.
 * RemLibrary/RemDevice only run after the new file is on disk so a failed
 * rem never leaves the system without a file.  Sets *reboot_out when the
 * module is still resident after the flush attempt.
 * Returns: 1 ok, 0 fail (this dest restored by load_place_file when possible).
 */
static LONG load_commit_one(struct LoadStaged *st, struct LoadArgs *args,
    LONG *reboot_out)
{
    LONG need_reboot;

    if (!st || st->skip) {
        return 1;
    }
    need_reboot = 0;
    st->committed = 0;

    if (!args->quiet) {
        Printf("Load: %s -> %s\n", (LONG)st->from, (LONG)st->dest);
        Flush(Output());
    }

    if (!load_place_file(st->cand, st->dest, st->crc,
            st->have_bak ? st->bak : NULL)) {
        /* Place failed; dest restored from bak when one existed. */
        DeleteFile(st->cand);
        st->cand[0] = '\0';
        return 0;
    }
    DeleteFile(st->cand);
    st->cand[0] = '\0';
    st->committed = 1;
    if (st->have_bak && !args->quiet) {
        Printf("Load: rollback copy kept at %s\n", (LONG)st->bak);
        Flush(Output());
    }

    /*
     * Disk has the new file.  Now try to drop the old resident copy so the
     * next open picks it up.  If it stays resident, caller advises reboot.
     */
    if (st->is_lh_lib) {
        load_log((STRPTR)"flushing lh.library after place");
        if (load_release_lh_library()) {
            need_reboot = 1;
        }
        if (!load_open_lh_library()) {
            need_reboot = 1;
        }
    } else {
        need_reboot = load_try_flush_resource(st->dest);
    }

    if (reboot_out && need_reboot) {
        *reboot_out = 1;
    }
    return 1;
}

static void load_print_readme(struct LhArchive *arc, STRPTR pkg_name)
{
    char member[LOAD_VAL_LEN];
    APTR data;
    LONG len;
    LONG L;

    if (!arc || !pkg_name || !pkg_name[0]) {
        return;
    }
    Strncpy((STRPTR)member, pkg_name, LOAD_VAL_LEN);
    L = 0;
    while (member[L] != '\0' && L < LOAD_VAL_LEN - 1) {
        L++;
    }
    if (L + 8 >= LOAD_VAL_LEN) {
        return;
    }
    member[L++] = '.';
    member[L++] = 'r';
    member[L++] = 'e';
    member[L++] = 'a';
    member[L++] = 'd';
    member[L++] = 'm';
    member[L++] = 'e';
    member[L] = '\0';

    data = NULL;
    len = LhReadData(arc, (STRPTR)member, &data);
    if (len <= 0 || !data) {
        return;
    }
    Printf("\n", 0);
    Flush(Output());
    Write(Output(), data, len);
    if (len > 0 && ((UBYTE *)data)[len - 1] != '\n') {
        Printf("\n", 0);
    }
    Printf("\n", 0);
    Flush(Output());
    FreeMem(data, (ULONG)len);
}

LONG load_run(struct LoadArgs *args)
{
    struct LhArchive *arc;
    APTR man_data;
    LONG man_len;
    struct LoadManifest *man;
    LONG i;
    char dest[LOAD_PATH_LEN];
    LONG rc;
    LONG one;
    char *added;
    char *updated;
    char *skipped;
    LONG nadded;
    LONG nupdated;
    LONG nskip;
    struct LoadStaged *staged;
    LONG nstaged;
    LONG ncommitted;
    LONG need_reboot;
    LONG reboot_flag;
    LONG did_commit;
    ULONG man_sz;
    ULONG sum_sz;

    nadded = 0;
    nupdated = 0;
    nskip = 0;
    nstaged = 0;
    ncommitted = 0;
    need_reboot = 0;
    did_commit = 0;
    staged = NULL;
    man = NULL;
    added = NULL;
    updated = NULL;
    skipped = NULL;
    arc = NULL;
    man_data = NULL;
    man_len = 0;
    rc = RETURN_FAIL;
    /*
     * LoadManifest is ~90KB -- must NOT live on the stack ($STACK is 4K).
     * Stack overflow here was crashing right after "preflight destinations".
     */
    man_sz = (ULONG)sizeof(struct LoadManifest);
    sum_sz = (ULONG)LOAD_SUMMARY_MAX * (ULONG)LOAD_SUMMARY_LEN;
    load_run_interactive = args->interactive ? 1UL : 0UL;
    load_run_verbose = args->verbose ? 1UL : 0UL;
    if (load_run_verbose && args->quiet) {
        /* VERBOSE wins over QUIET for progress lines. */
        args->quiet = 0;
    }

    if (args->rollback) {
        load_log((STRPTR)"ROLLBACK path");
        if (!args->dest || !args->dest[0]) {
            load_print_error((STRPTR)"ROLLBACK requires DEST", 0);
            load_run_interactive = 0;
            load_run_verbose = 0;
            return RETURN_FAIL;
        }
        rc = load_rollback_dest(args->dest, args->quiet);
        load_run_interactive = 0;
        load_run_verbose = 0;
        return rc;
    }

    load_log((STRPTR)"allocating Manifest / staging");
    man = (struct LoadManifest *)AllocMem(man_sz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!man) {
        load_print_error((STRPTR)"out of memory for Manifest", 0);
        goto fail;
    }
    added = (char *)AllocMem(sum_sz, MEMF_PUBLIC | MEMF_CLEAR);
    updated = (char *)AllocMem(sum_sz, MEMF_PUBLIC | MEMF_CLEAR);
    skipped = (char *)AllocMem(sum_sz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!added || !updated || !skipped) {
        load_print_error((STRPTR)"out of memory for summary", 0);
        goto fail;
    }
    staged = (struct LoadStaged *)AllocMem(
        (ULONG)sizeof(struct LoadStaged) * (ULONG)LOAD_MAX_FILES,
        MEMF_PUBLIC | MEMF_CLEAR);
    if (!staged) {
        load_print_error((STRPTR)"out of memory for install staging", 0);
        goto fail;
    }

    load_log((STRPTR)"opening Bin");
    if (args->bin && args->bin[0]) {
        load_log(args->bin);
    }
    arc = LhOpenArchive(args->bin, LHARC_MODE_READ);
    if (!arc) {
        load_print_error((STRPTR)"cannot open Bin", LhErr());
        goto fail;
    }

    load_log((STRPTR)"reading Manifest");
    man_data = NULL;
    man_len = LhReadData(arc, (STRPTR)"Manifest", &man_data);
    if (man_len < 0 || !man_data) {
        load_print_error((STRPTR)"cannot read Manifest", LhErr());
        goto fail;
    }

    load_log((STRPTR)"parsing Manifest");
    if (!load_parse_manifest((STRPTR)man_data, man_len, man)) {
        goto fail;
    }
    FreeMem(man_data, (ULONG)man_len);
    man_data = NULL;
    man_len = 0;

    if (!args->quiet) {
        if (man->name[0]) {
            Printf("Load: package %s", (LONG)man->name);
            if (man->version[0]) {
                Printf(" %s", (LONG)man->version);
            }
            Printf("\n", 0);
            Flush(Output());
        }
    }

    if (man->nfiles == 0) {
        load_print_error((STRPTR)"Manifest has no File: map", 0);
        goto fail;
    }

    if (!man->version[0]) {
        load_print_error((STRPTR)"Manifest missing Version", 0);
        goto fail;
    }

    load_log((STRPTR)"checking Requires / OS");
    if (!load_check_deps(man)) {
        goto fail;
    }

    load_log((STRPTR)"selecting CPU/FPU variants");
    if (!load_select_variants(man)) {
        goto fail;
    }

    load_log((STRPTR)"preflight destinations");
    if (!load_preflight(man, args->to)) {
        goto fail;
    }

    /*
     * Workbench: show what will be installed (and previous $VER) and wait
     * for confirmation before any bak or place.
     */
    if (args->interactive) {
        load_log((STRPTR)"showing displayme / confirm requesters");
        if (!load_wb_confirm_install(man, args->to, arc)) {
            load_log((STRPTR)"user cancelled install");
            rc = RETURN_OK;
            goto cleanup;
        }
        load_log((STRPTR)"user confirmed install");
    }

    /*
     * Phase 1: extract every selected member to SYS:T/Load while lh.library
     * is still open.  Skip/bak decisions happen here.  Destinations are
     * untouched until phase 2.
     */
    load_log((STRPTR)"phase 1: stage selected members to SYS:T/Load");
    for (i = 0; i < man->nfiles; i++) {
        if (!man->files[i].selected) {
            continue;
        }
        if (!load_resolve_dest(dest, LOAD_PATH_LEN, man->files[i].dest,
            args->to, man->install_root)) {
            load_print_error((STRPTR)"cannot resolve destination", 0);
            goto fail_tx;
        }
        if (nstaged >= LOAD_MAX_FILES) {
            load_print_error((STRPTR)"too many files to stage", 0);
            goto fail_tx;
        }
        load_log((STRPTR)"staging");
        load_log(dest);
        one = load_stage_one(arc, &man->files[i], dest, args,
            &staged[nstaged]);
        if (one == 0) {
            goto fail_tx;
        }
        if (one == 2) {
            load_log((STRPTR)"skipped (up-to-date or FORCE off)");
            if (nskip < LOAD_SUMMARY_MAX) {
                Strncpy(skipped + nskip * LOAD_SUMMARY_LEN, dest,
                    LOAD_SUMMARY_LEN);
                nskip++;
            }
            continue;
        }
        nstaged++;
    }

    if (!args->quiet) {
        load_print_readme(arc, man->name);
    }

    load_log((STRPTR)"closing Bin before commit");
    LhCloseArchive(arc);
    arc = NULL;

    /*
     * Phase 2: place every staged file, then RemLibrary/RemDevice.
     * Disk is updated first; reboot is advised only when a flush left the
     * old resident module in memory.
     */
    load_log((STRPTR)"phase 2: commit staged files");
    for (i = 0; i < nstaged; i++) {
        reboot_flag = 0;
        load_log((STRPTR)"committing");
        load_log(staged[i].dest);
        if (!load_commit_one(&staged[i], args, &reboot_flag)) {
            /*
             * This dest was restored by load_place_file when possible.
             * Reverse earlier successful places so the set stays atomic.
             */
            goto fail_tx;
        }
        ncommitted++;
        if (reboot_flag) {
            need_reboot = 1;
        }
        /*
         * have_bak means dest existed before place => Updated; else Added.
         */
        if (staged[i].have_bak) {
            if (nupdated < LOAD_SUMMARY_MAX) {
                Strncpy(updated + nupdated * LOAD_SUMMARY_LEN,
                    staged[i].dest, LOAD_SUMMARY_LEN);
                nupdated++;
            }
        } else if (nadded < LOAD_SUMMARY_MAX) {
            Strncpy(added + nadded * LOAD_SUMMARY_LEN, staged[i].dest,
                LOAD_SUMMARY_LEN);
            nadded++;
        }
    }

    if (!args->quiet) {
        Printf("Load: summary\n", 0);
        if (nadded == 0 && nupdated == 0 && nskip == 0) {
            Printf("  (nothing selected)\n", 0);
        }
        for (i = 0; i < nadded; i++) {
            Printf("  added      %s\n",
                (LONG)(added + i * LOAD_SUMMARY_LEN));
        }
        for (i = 0; i < nupdated; i++) {
            Printf("  updated    %s\n",
                (LONG)(updated + i * LOAD_SUMMARY_LEN));
        }
        for (i = 0; i < nskip; i++) {
            Printf("  skipped    %s\n",
                (LONG)(skipped + i * LOAD_SUMMARY_LEN));
        }
        Printf("Load: done\n", 0);
        Flush(Output());
    }

    if (args->interactive) {
        load_wb_install_done(added, nadded, updated, nupdated, need_reboot);
    }
    if (need_reboot) {
        load_log((STRPTR)"reboot recommended");
        load_print_reboot_advice(args->quiet);
        rc = RETURN_WARN;
    } else {
        load_log((STRPTR)"done");
        rc = RETURN_OK;
    }
    goto cleanup;

fail_tx:
    /*
     * Transaction failed after some staging and/or commits.  Roll back
     * committed files and discard uncommitted T: candidates/baks.
     */
    did_commit = ncommitted;
    load_abort_install(staged, nstaged);
    ncommitted = 0;
    nstaged = 0;
    /*
     * After abort, drop any in-memory lh handle (may be a post-place open
     * of a file we just restored over) and reopen from disk / T: bak.
     */
    if (LhBase) {
        CloseLibrary(LhBase);
        LhBase = NULL;
    }
    if (!load_open_lh_library()) {
        load_print_error(
            (STRPTR)
            "lh.library unavailable after abort; try "
            "Load ROLLBACK DEST=LIBS:lh.library",
            0);
    }
    /* Extra requester only when places were undone (stage errors already
     * reported via load_print_error). */
    if (args->interactive && did_commit > 0) {
        load_wb_error(
            (STRPTR)
            "Install aborted; previous files restored where possible.",
            0);
    }
    rc = RETURN_FAIL;
    goto cleanup;

fail:
    rc = RETURN_FAIL;
    /* fall through */

cleanup:
    if (man_data && man_len > 0) {
        FreeMem(man_data, (ULONG)man_len);
        man_data = NULL;
    }
    if (arc) {
        LhCloseArchive(arc);
        arc = NULL;
    }
    if (staged) {
        /*
         * Success path: leave bak/sidecar for ROLLBACK.  Failure paths
         * already discarded via load_abort_install.
         */
        FreeMem(staged,
            (ULONG)sizeof(struct LoadStaged) * (ULONG)LOAD_MAX_FILES);
        staged = NULL;
    }
    if (added) {
        FreeMem(added, sum_sz);
        added = NULL;
    }
    if (updated) {
        FreeMem(updated, sum_sz);
        updated = NULL;
    }
    if (skipped) {
        FreeMem(skipped, sum_sz);
        skipped = NULL;
    }
    if (man) {
        FreeMem(man, man_sz);
        man = NULL;
    }
    load_run_interactive = 0;
    load_run_verbose = 0;
    return rc;
}
