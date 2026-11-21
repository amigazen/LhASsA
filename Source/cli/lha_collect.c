/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lha_collect.c - Home directories, -r recursion, -e empty dirs, -R archives.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lha_internal.h"

#if defined(LHA_AMIGA)
#define __USE_SYSBASE
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosasl.h>
#include <proto/exec.h>
#include <proto/dos.h>
#else
#include <sys/stat.h>
#include <dirent.h>
#endif

static int lha_is_homedir_spec(const char *s)
{
    size_t n;

    if (!s || !s[0] || g_opts.no_homedirs) {
        return 0;
    }
    n = strlen(s);
    if (n == 1 && s[0] == '/') {
        return 1;
    }
    if (s[n - 1] == '/' || s[n - 1] == ':') {
        return 1;
    }
    return 0;
}

static int lha_items_grow(lha_file_item **items, int *n, int *cap)
{
    lha_file_item *grown;
    int ncap;

    if (*n < *cap) {
        return 1;
    }
    ncap = (*cap == 0) ? 16 : (*cap * 2);
    grown = (lha_file_item *)realloc(*items, (size_t)ncap * sizeof(**items));
    if (!grown) {
        return 0;
    }
    *items = grown;
    *cap = ncap;
    return 1;
}

static int lha_items_add(lha_file_item **items, int *n, int *cap,
    const char *path, const char *arcname, const char *comment, int is_dir)
{
    lha_file_item *it;

    if (!lha_items_grow(items, n, cap)) {
        return 0;
    }
    it = &(*items)[*n];
    it->path = lha_strdup(path);
    it->arcname = lha_strdup(arcname);
    it->comment = (comment && comment[0]) ? lha_strdup(comment) : NULL;
    it->is_dir = is_dir;
    if (!it->path || !it->arcname) {
        free(it->path);
        free(it->arcname);
        free(it->comment);
        return 0;
    }
    (*n)++;
    return 1;
}

void lha_free_file_items(lha_file_item *items, int count)
{
    int i;

    if (!items) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(items[i].path);
        free(items[i].arcname);
        free(items[i].comment);
    }
    free(items);
}

/*
 * Archive member name: with -x, path relative to home (volume already
 * stripped from relative form); without -x, basename only.
 */
static char *lha_arcname_from_paths(const char *fs_path, const char *home)
{
    char disk[512];
    const char *use;
    size_t hlen;
    int ok;

    ok = 0;
#if defined(LHA_AMIGA)
    ok = lha_disk_case_path_amiga(fs_path, disk, sizeof(disk));
#endif
    if (!ok) {
        strncpy(disk, fs_path ? fs_path : "", sizeof(disk) - 1);
        disk[sizeof(disk) - 1] = '\0';
        lha_normalize_slashes(disk);
    }
    use = disk;
    if (home && home[0] && g_opts.preserve_paths > 0) {
        hlen = strlen(home);
        if (hlen > 0 && (home[hlen - 1] == '/' || home[hlen - 1] == ':')) {
            /* Compare ignoring case for AmigaFS. */
            if (strlen(disk) >= hlen) {
                size_t i;
                int match;

                match = 1;
                for (i = 0; i < hlen; i++) {
                    char a;
                    char b;

                    a = disk[i];
                    b = home[i];
                    if (a >= 'A' && a <= 'Z') {
                        a = (char)(a - 'A' + 'a');
                    }
                    if (b >= 'A' && b <= 'Z') {
                        b = (char)(b - 'A' + 'a');
                    }
                    if (a != b && !(b == ':' && a == '/')) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    use = disk + hlen;
                    while (*use == '/') {
                        use++;
                    }
                }
            }
        }
    }
    if (g_opts.preserve_paths > 0) {
        if (!use[0]) {
            return lha_basename(disk);
        }
        /* Still strip a leading volume if present. */
        {
            const char *p;

            for (p = use; *p; p++) {
                if (*p == ':') {
                    use = p + 1;
                    if (*use == '/') {
                        use++;
                    }
                    break;
                }
            }
        }
        return lha_strdup(use);
    }
    return lha_basename(disk);
}

static void lha_join_home_spec(char *out, size_t outsz, const char *home,
    const char *spec)
{
    size_t hlen;

    if (!out || outsz == 0) {
        return;
    }
    out[0] = '\0';
    if (!home || !home[0]) {
        strncpy(out, spec ? spec : "", outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    strncpy(out, home, outsz - 1);
    out[outsz - 1] = '\0';
    if (!spec || !spec[0]) {
        return;
    }
    hlen = strlen(out);
    if (hlen > 0 && out[hlen - 1] != '/' && out[hlen - 1] != ':') {
        if (hlen + 1 < outsz) {
            out[hlen] = '/';
            out[hlen + 1] = '\0';
        }
    }
    strncat(out, spec, outsz - strlen(out) - 1);
}

#if defined(LHA_AMIGA)

static int lha_amiga_is_dir(const char *path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    int isdir;

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
    isdir = 0;
    if (Examine(lock, fib) && fib->fib_DirEntryType > 0) {
        isdir = 1;
    }
    FreeMem(fib, (ULONG)sizeof(*fib));
    UnLock(lock);
    return isdir;
}

static int lha_collect_one_path(const char *path, const char *home,
    lha_file_item **items, int *n, int *cap)
{
    char *arcname;
    char *note;
    int ok;

    note = NULL;
    if (!g_opts.ignore_filenotes) {
        (void)lha_read_filenote(path, &note);
    }
    arcname = lha_arcname_from_paths(path, home);
    if (!arcname) {
        free(note);
        return 0;
    }
    ok = lha_items_add(items, n, cap, path, arcname, note, 0);
    free(arcname);
    free(note);
    return ok;
}

static int lha_walk_dir(const char *dir, const char *pattern, const char *home,
    const char *arc_prefix, lha_file_item **items, int *n, int *cap)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    int files_here;
    int dirs_here;
    char child[512];
    char child_arc[512];

    /* Empty dir path means the current directory (Amiga Lock("")). */
    lock = Lock((STRPTR)(dir && dir[0] ? dir : ""), ACCESS_READ);
    if (lock == (BPTR)NULL) {
        return 1;
    }
    fib = (struct FileInfoBlock *)AllocMem((ULONG)sizeof(*fib),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        UnLock(lock);
        return 0;
    }
    if (!Examine(lock, fib)) {
        FreeMem(fib, (ULONG)sizeof(*fib));
        UnLock(lock);
        return 1;
    }
    files_here = 0;
    dirs_here = 0;
    ok = ExNext(lock, fib);
    while (ok) {
        if (fib->fib_FileName[0] == '.'
            && (fib->fib_FileName[1] == '\0'
                || (fib->fib_FileName[1] == '.' && fib->fib_FileName[2] == '\0'))) {
            ok = ExNext(lock, fib);
            continue;
        }
        if (!lha_path_join(child, sizeof(child), dir,
                (const char *)fib->fib_FileName)) {
            ok = ExNext(lock, fib);
            continue;
        }
        if (fib->fib_DirEntryType > 0) {
            dirs_here++;
            if (arc_prefix && arc_prefix[0]) {
                if (!lha_path_join(child_arc, sizeof(child_arc), arc_prefix,
                        (const char *)fib->fib_FileName)) {
                    ok = ExNext(lock, fib);
                    continue;
                }
            } else {
                strncpy(child_arc, (const char *)fib->fib_FileName,
                    sizeof(child_arc) - 1);
                child_arc[sizeof(child_arc) - 1] = '\0';
            }
            if (!lha_walk_dir(child, pattern, home, child_arc, items, n, cap)) {
                FreeMem(fib, (ULONG)sizeof(*fib));
                UnLock(lock);
                return 0;
            }
        } else {
            files_here++;
            if (lha_match_pattern(pattern, (const char *)fib->fib_FileName)) {
                if (!lha_collect_one_path(child, home, items, n, cap)) {
                    FreeMem(fib, (ULONG)sizeof(*fib));
                    UnLock(lock);
                    return 0;
                }
            }
        }
        ok = ExNext(lock, fib);
    }
    FreeMem(fib, (ULONG)sizeof(*fib));
    UnLock(lock);

    if (g_opts.empty_dirs && files_here == 0 && dirs_here == 0
        && arc_prefix && arc_prefix[0] && g_opts.preserve_paths > 0) {
        if (!lha_items_add(items, n, cap, dir, arc_prefix, NULL, 1)) {
            return 0;
        }
    }
    return 1;
}

static int lha_collect_flat(const char *home, const char *spec,
    lha_file_item **items, int *n, int *cap)
{
    char full[512];
    char *pats[1];
    char **exp;
    int nexp;
    int i;

    lha_join_home_spec(full, sizeof(full), home, spec);
    pats[0] = full;
    exp = lha_expand_paths(pats, 1, &nexp);
    if (!exp || nexp <= 0) {
        lha_free_paths(exp, nexp);
        return 1;
    }
    for (i = 0; i < nexp; i++) {
        if (!lha_collect_one_path(exp[i], home, items, n, cap)) {
            lha_free_paths(exp, nexp);
            return 0;
        }
    }
    lha_free_paths(exp, nexp);
    return 1;
}

static int lha_collect_recursive(const char *home, const char *spec,
    lha_file_item **items, int *n, int *cap)
{
    char full[512];
    char base[512];
    char *slash;
    const char *pattern;
    int wild;

    lha_join_home_spec(full, sizeof(full), home, spec);
    wild = lha_path_is_wild(spec);

    if (!wild && !lha_amiga_is_dir(full)) {
        /* Explicit file: only in the home/current directory. */
        return lha_collect_flat(home, spec, items, n, cap);
    }
    if (!wild && lha_amiga_is_dir(full)) {
        /* Explicit directory: as if /* was appended. */
        return lha_walk_dir(full, "#?", home,
            g_opts.preserve_paths > 0 ? "" : "", items, n, cap);
    }

    /* Pattern: last node is the match pattern; walk its parent tree. */
    strncpy(base, full, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    slash = strrchr(base, '/');
    if (!slash) {
        slash = strrchr(base, ':');
    }
    if (slash && *(slash + 1)) {
        pattern = slash + 1;
        if (*slash == '/') {
            *slash = '\0';
        } else {
            /* volume:pattern -> walk volume root */
            slash[1] = '\0';
            pattern = spec;
            {
                const char *p;

                p = spec;
                while (*p && *p != '/' && *p != ':') {
                    p++;
                }
                if (*p == ':') {
                    pattern = p + 1;
                }
            }
        }
        if (base[0] == '\0') {
            strncpy(base, home && home[0] ? home : "", sizeof(base) - 1);
        }
    } else {
        pattern = spec;
        if (home && home[0]) {
            strncpy(base, home, sizeof(base) - 1);
        } else {
            strncpy(base, "", sizeof(base) - 1);
        }
        base[sizeof(base) - 1] = '\0';
        if (base[0] == '\0') {
            /* Current directory. */
            strncpy(base, "", sizeof(base) - 1);
        }
    }
    if (base[0] == '\0') {
        return lha_walk_dir("", pattern, home, "", items, n, cap);
    }
    return lha_walk_dir(base, pattern, home, "", items, n, cap);
}

#else /* !LHA_AMIGA */

static int lha_host_is_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

static int lha_collect_one_path(const char *path, const char *home,
    lha_file_item **items, int *n, int *cap)
{
    char *arcname;

    arcname = lha_arcname_from_paths(path, home);
    if (!arcname) {
        return 0;
    }
    if (!lha_items_add(items, n, cap, path, arcname, NULL, 0)) {
        free(arcname);
        return 0;
    }
    free(arcname);
    return 1;
}

static int lha_walk_dir(const char *dir, const char *pattern, const char *home,
    const char *arc_prefix, lha_file_item **items, int *n, int *cap)
{
    DIR *d;
    struct dirent *de;
    char child[512];
    char child_arc[512];
    int files_here;
    int dirs_here;

    d = opendir(dir[0] ? dir : ".");
    if (!d) {
        return 1;
    }
    files_here = 0;
    dirs_here = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.'
            && (de->d_name[1] == '\0'
                || (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
            continue;
        }
        if (!lha_path_join(child, sizeof(child), dir[0] ? dir : ".",
                de->d_name)) {
            continue;
        }
        if (lha_host_is_dir(child)) {
            dirs_here++;
            if (arc_prefix && arc_prefix[0]) {
                if (!lha_path_join(child_arc, sizeof(child_arc), arc_prefix,
                        de->d_name)) {
                    continue;
                }
            } else {
                strncpy(child_arc, de->d_name, sizeof(child_arc) - 1);
                child_arc[sizeof(child_arc) - 1] = '\0';
            }
            if (!lha_walk_dir(child, pattern, home, child_arc, items, n, cap)) {
                closedir(d);
                return 0;
            }
        } else {
            files_here++;
            if (lha_match_pattern(pattern, de->d_name)) {
                if (!lha_collect_one_path(child, home, items, n, cap)) {
                    closedir(d);
                    return 0;
                }
            }
        }
    }
    closedir(d);
    if (g_opts.empty_dirs && files_here == 0 && dirs_here == 0
        && arc_prefix && arc_prefix[0] && g_opts.preserve_paths > 0) {
        if (!lha_items_add(items, n, cap, dir, arc_prefix, NULL, 1)) {
            return 0;
        }
    }
    return 1;
}

static int lha_collect_flat(const char *home, const char *spec,
    lha_file_item **items, int *n, int *cap)
{
    char full[512];

    lha_join_home_spec(full, sizeof(full), home, spec);
    return lha_collect_one_path(full, home, items, n, cap);
}

static int lha_collect_recursive(const char *home, const char *spec,
    lha_file_item **items, int *n, int *cap)
{
    char full[512];
    char base[512];
    char *slash;
    const char *pattern;
    int wild;

    lha_join_home_spec(full, sizeof(full), home, spec);
    wild = lha_path_is_wild(spec);
    if (!wild && !lha_host_is_dir(full)) {
        return lha_collect_flat(home, spec, items, n, cap);
    }
    if (!wild && lha_host_is_dir(full)) {
        return lha_walk_dir(full, "*", home, "", items, n, cap);
    }
    strncpy(base, full, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    slash = strrchr(base, '/');
    if (slash && *(slash + 1)) {
        pattern = slash + 1;
        *slash = '\0';
        if (base[0] == '\0') {
            strncpy(base, ".", sizeof(base) - 1);
        }
    } else {
        pattern = spec;
        strncpy(base, home && home[0] ? home : ".", sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    }
    return lha_walk_dir(base, pattern, home, "", items, n, cap);
}

#endif /* LHA_AMIGA */

int lha_collect_add_files(char **args, int argc, lha_file_item **out_items,
    int *out_count)
{
    lha_file_item *items;
    int n;
    int cap;
    int i;
    char home[512];
    char *allpat;
    int ok;

    if (out_count) {
        *out_count = 0;
    }
    if (out_items) {
        *out_items = NULL;
    }
    items = NULL;
    n = 0;
    cap = 0;
    home[0] = '\0';

    if (argc <= 0) {
        allpat = (char *)"#?";
        ok = g_opts.recurse
            ? lha_collect_recursive(home, allpat, &items, &n, &cap)
            : lha_collect_flat(home, allpat, &items, &n, &cap);
        if (!ok) {
            lha_free_file_items(items, n);
            return 0;
        }
        if (out_items) {
            *out_items = items;
        }
        if (out_count) {
            *out_count = n;
        }
        return 1;
    }

    for (i = 0; i < argc; i++) {
        if (lha_is_homedir_spec(args[i])) {
            if (args[i][0] == '/' && args[i][1] == '\0') {
                home[0] = '\0';
            } else {
                strncpy(home, args[i], sizeof(home) - 1);
                home[sizeof(home) - 1] = '\0';
            }
            continue;
        }
        ok = g_opts.recurse
            ? lha_collect_recursive(home, args[i], &items, &n, &cap)
            : lha_collect_flat(home, args[i], &items, &n, &cap);
        if (!ok) {
            lha_free_file_items(items, n);
            return 0;
        }
    }
    if (out_items) {
        *out_items = items;
    }
    if (out_count) {
        *out_count = n;
    }
    return 1;
}

/*
 * Resolve archive name suffix (.lha for LHA methods, .lzh for -0/LhArc).
 */
char *lha_resolve_archive_name(const char *name)
{
    char buf[512];
    const char *suf;
    const char *leaf;
    const char *p;
    int has_ext;

    if (!name) {
        return NULL;
    }
    if (g_opts.no_suffix) {
        return lha_strdup(name);
    }
    suf = ".lha";
    if (g_opts.level == LH_LEVEL_LH1 || g_opts.header_level == 0) {
        /* LhArc 1.x style often uses .lzh; keep .lzh when -0 selected. */
        if (g_opts.level == LH_LEVEL_LH1 || g_opts.level == LH_LEVEL_STORE) {
            if (g_opts.header_level == 0) {
                suf = ".lzh";
            }
        }
    }
    leaf = name;
    for (p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            leaf = p + 1;
        }
    }
    has_ext = 0;
    for (p = leaf; *p; p++) {
        if (*p == '.') {
            has_ext = 1;
            break;
        }
    }
    if (!g_opts.always_suffix && has_ext) {
        return lha_strdup(name);
    }
    /*
     * With -R and a bare name (myarc), leave unsuffixed so expansion can
     * match both myarc.lha and myarc.lzh.
     */
    if (g_opts.recurse_archives && !g_opts.always_suffix && !has_ext) {
        return lha_strdup(name);
    }
    if (strlen(name) + strlen(suf) >= sizeof(buf)) {
        return lha_strdup(name);
    }
    strcpy(buf, name);
    strcat(buf, suf);
    return lha_strdup(buf);
}

/*
 * Expand an archive pattern.  With -R, walk directories.  Without, use
 * flat MatchFirst / literal (plus default suffix already applied).
 */
char **lha_expand_archives(const char *pattern, int *out_count)
{
    char *pats[1];
    char **exp;
    int n;

    if (out_count) {
        *out_count = 0;
    }
    if (!pattern) {
        return NULL;
    }
    if (!g_opts.recurse_archives) {
        pats[0] = (char *)pattern;
        return lha_expand_paths(pats, 1, out_count);
    }
    /*
     * Recursive archive collect: walk from directory of pattern matching
     * the leaf against .lha/.lzh files (or the leaf pattern).
     */
#if defined(LHA_AMIGA)
    {
        char base[512];
        char leafpat[256];
        char *slash;
        lha_file_item *items;
        int ni;
        int cap;
        int i;
        char **out;

        strncpy(base, pattern, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        slash = strrchr(base, '/');
        if (!slash) {
            slash = strrchr(base, ':');
        }
        if (slash && *(slash + 1)) {
            strncpy(leafpat, slash + 1, sizeof(leafpat) - 1);
            leafpat[sizeof(leafpat) - 1] = '\0';
            if (*slash == '/') {
                *slash = '\0';
            } else {
                slash[1] = '\0';
            }
        } else {
            strncpy(leafpat, pattern, sizeof(leafpat) - 1);
            leafpat[sizeof(leafpat) - 1] = '\0';
            base[0] = '\0';
        }
        /* Bare name without wildcards: match name.lha / name.lzh. */
        if (!lha_path_is_wild(leafpat) && strchr(leafpat, '.') == NULL) {
            char alt[256];

            items = NULL;
            ni = 0;
            cap = 0;
            sprintf(alt, "%s.lha", leafpat);
            if (!lha_walk_dir(base[0] ? base : "", alt, "", "", &items, &ni,
                    &cap)) {
                lha_free_file_items(items, ni);
                return NULL;
            }
            sprintf(alt, "%s.lzh", leafpat);
            if (!lha_walk_dir(base[0] ? base : "", alt, "", "", &items, &ni,
                    &cap)) {
                lha_free_file_items(items, ni);
                return NULL;
            }
        } else {
            items = NULL;
            ni = 0;
            cap = 0;
            if (!lha_walk_dir(base[0] ? base : "", leafpat, "", "", &items, &ni,
                    &cap)) {
                lha_free_file_items(items, ni);
                return NULL;
            }
        }
        if (ni <= 0) {
            lha_free_file_items(items, ni);
            return NULL;
        }
        /* Prefer archive-looking names when the pattern is a broad wildcard. */
        out = (char **)malloc((size_t)ni * sizeof(char *));
        if (!out) {
            lha_free_file_items(items, ni);
            return NULL;
        }
        {
            int no;
            size_t plen;
            int want_arc;

            no = 0;
            want_arc = 1;
            plen = strlen(leafpat);
            if (plen >= 4) {
                if (lha_stricmp(leafpat + plen - 4, ".lha") == 0
                    || lha_stricmp(leafpat + plen - 4, ".lzh") == 0) {
                    want_arc = 0;
                }
            }
            for (i = 0; i < ni; i++) {
                const char *lp;
                size_t ln;

                lp = items[i].path;
                ln = strlen(lp);
                if (want_arc) {
                    if (ln < 4
                        || (lha_stricmp(lp + ln - 4, ".lha") != 0
                            && lha_stricmp(lp + ln - 4, ".lzh") != 0)) {
                        continue;
                    }
                }
                out[no++] = items[i].path;
                items[i].path = NULL;
            }
            lha_free_file_items(items, ni);
            if (no <= 0) {
                free(out);
                return NULL;
            }
            if (out_count) {
                *out_count = no;
            }
            return out;
        }
    }
#else
    pats[0] = (char *)pattern;
    exp = lha_expand_paths(pats, 1, &n);
    if (out_count) {
        *out_count = n;
    }
    return exp;
#endif
}
