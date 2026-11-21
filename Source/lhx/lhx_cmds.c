/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lhx_cmds.c - LhX commands via lh.library DOS-style archive API.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <libraries/lh.h>
#include <proto/lh.h>

#include "lhx_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;

#define LHX_IOBUF 8192L

/* dos.doc: fib_DirEntryType positive for directories, negative for files. */
static int lhx_fib_is_file(struct FileInfoBlock *fib)
{
    if (!fib) {
        return 0;
    }
    if (fib->fib_DirEntryType > 0) {
        return 0;
    }
    return 1;
}

/*
 * Nested archive walk: LhExamine/LhExNext now yield immediate children.
 * Build full entry paths (test/test1/file) for list/extract/test/print.
 */
static LONG lhx_walk_entries(struct LhArchive *arc, STRPTR dirpath,
    struct LhxArgs *args,
    LONG (*fn)(struct LhxArgs *, struct LhArchive *, STRPTR,
        struct FileInfoBlock *, APTR),
    APTR ud)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG walkrc;
    LONG rc;
    STRPTR childpath;
    LONG dlen;
    LONG nlen;

    lock = LhLock(arc, dirpath ? dirpath : (STRPTR)"");
    if (!lock) {
        return RETURN_FAIL;
    }
    /*
     * Recursive walk: childpath must be heap so deep trees fit in 4K.
     */
    childpath = (STRPTR)AllocMem(LHX_PATH_LEN, MEMF_PUBLIC | MEMF_CLEAR);
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!childpath || !fib) {
        if (childpath) {
            FreeMem(childpath, LHX_PATH_LEN);
        }
        if (fib) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        }
        LhUnLock(lock);
        return RETURN_FAIL;
    }
    /* Examine = directory self; skip.  Children come from ExNext. */
    if (!LhExamine(lock, fib)) {
        FreeMem(childpath, LHX_PATH_LEN);
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhUnLock(lock);
        return RETURN_OK;
    }
    rc = RETURN_OK;
    ok = LhExNext(lock, fib);
    while (ok) {
        dlen = 0;
        if (dirpath != NULL && dirpath[0] != '\0') {
            while (dirpath[dlen] != '\0' && dlen < LHX_PATH_LEN - 12) {
                childpath[dlen] = dirpath[dlen];
                dlen++;
            }
            childpath[dlen++] = '/';
        }
        nlen = 0;
        while (fib->fib_FileName[nlen] != '\0'
                && dlen + nlen < LHX_PATH_LEN - 1) {
            childpath[dlen + nlen] = fib->fib_FileName[nlen];
            nlen++;
        }
        childpath[dlen + nlen] = '\0';

        if (!lhx_fib_is_file(fib)) {
            /* Recurse into nested virtual directory. */
            walkrc = lhx_walk_entries(arc, childpath, args, fn, ud);
            if (walkrc == RETURN_WARN) {
                rc = RETURN_WARN;
                break;
            }
            if (walkrc != RETURN_OK) {
                rc = RETURN_ERROR;
            }
        } else if (fn != NULL) {
            if (fn(args, arc, childpath, fib, ud) != RETURN_OK) {
                rc = RETURN_ERROR;
            }
        }
        if (lhx_check_break()) {
            Printf("LhX: *** Break\n", 0);
            Flush(Output());
            rc = RETURN_WARN;
            break;
        }
        ok = LhExNext(lock, fib);
    }
    FreeMem(childpath, LHX_PATH_LEN);
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhUnLock(lock);
    return rc;
}

struct lhx_list_ud {
    LONG count;
    LONG blocks;
};

static LONG lhx_list_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR fullname, struct FileInfoBlock *fib, APTR ud)
{
    struct lhx_list_ud *st;

    (void)arc;
    st = (struct lhx_list_ud *)ud;
    if (!lhx_any_selected(fullname, args->files)) {
        return RETURN_OK;
    }
    /* Show full nested path in the name column. */
    Strncpy(fib->fib_FileName, fullname, (LONG)sizeof(fib->fib_FileName));
    lhx_list_entry(args, fib);
    st->count++;
    st->blocks += fib->fib_NumBlocks;
    return RETURN_OK;
}

static struct LhArchive *lhx_open_archive(STRPTR path, LONG mode, STRPTR password)
{
    struct LhArchive *arc;

    arc = LhOpenArchive(path, mode);
    if (!arc) {
        return NULL;
    }
    if (password && password[0]) {
        LhSetPassword(arc, password);
    }
    return arc;
}

LONG lhx_cmd_list(struct LhxArgs *args)
{
    struct LhArchive *arc;
    struct lhx_list_ud st;
    LONG walkrc;

    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    st.count = 0;
    st.blocks = 0;
    walkrc = lhx_walk_entries(arc, (STRPTR)"", args, lhx_list_one, (APTR)&st);
    LhCloseArchive(arc);
    if (walkrc == RETURN_WARN) {
        return RETURN_WARN;
    }
    if (!args->quiet) {
        if (st.count == 0 && args->files && args->files[0]) {
            Printf("No match found\n", 0);
        } else {
            lhx_list_total(st.count, st.blocks);
        }
    }
    return RETURN_OK;
}

static LONG lhx_extract_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR entry_name, LONG with_paths, LONG expected_size)
{
    STRPTR outpath;
    STRPTR namebuf;
    STRPTR pathbuf;
    LONG len;
    LONG rc;

    lhx_dbg_s((STRPTR)"extract_one", entry_name);
    lhx_dbg_l((STRPTR)"with_paths", with_paths);
    lhx_dbg_l((STRPTR)"expected_size", expected_size);
    if (!entry_name || !entry_name[0]) {
        lhx_dbg_s((STRPTR)"extract_one", (STRPTR)"skip empty name");
        return DOSTRUE;
    }
    /* namebuf + pathbuf = 1KB together -- heap, not stack. */
    namebuf = (STRPTR)AllocMem(LHX_PATH_LEN, MEMF_PUBLIC | MEMF_CLEAR);
    pathbuf = (STRPTR)AllocMem(LHX_PATH_LEN, MEMF_PUBLIC | MEMF_CLEAR);
    if (!namebuf || !pathbuf) {
        if (namebuf) {
            FreeMem(namebuf, LHX_PATH_LEN);
        }
        if (pathbuf) {
            FreeMem(pathbuf, LHX_PATH_LEN);
        }
        return DOSFALSE;
    }
    rc = DOSFALSE;
    if (with_paths) {
        Strncpy(namebuf, entry_name, LHX_PATH_LEN);
    } else {
        if (!lhx_basename(entry_name, namebuf, LHX_PATH_LEN)) {
            lhx_dbg_s((STRPTR)"extract_one", (STRPTR)"basename failed");
            goto out;
        }
    }
    lhx_dbg_s((STRPTR)"out name", namebuf);
    if (!lhx_path_join(pathbuf, LHX_PATH_LEN, args->destdir, namebuf)) {
        lhx_dbg_s((STRPTR)"extract_one", (STRPTR)"path_join failed");
        goto out;
    }
    outpath = pathbuf;
    lhx_dbg_s((STRPTR)"out path", outpath);
    if (args->force) {
        BPTR old;

        old = Lock(outpath, ACCESS_WRITE);
        if (old != (BPTR)NULL) {
            UnLock(old);
            DeleteFile(outpath);
        }
    }
    lhx_work_start(args, entry_name);
    if (!LhExtractEntry(arc, entry_name, outpath)) {
        lhx_dbg_l((STRPTR)"LhExtractEntry LhErr", LhErr());
        lhx_print_error((STRPTR)"cannot extract archive entry", LhErr());
        goto out;
    }
    len = expected_size;
    if (expected_size > 0) {
        BPTR outlock;
        struct FileInfoBlock *outfib;

        outlock = Lock(outpath, ACCESS_READ);
        if (outlock != (BPTR)NULL) {
            outfib = (struct FileInfoBlock *)AllocMem(
                (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
            if (outfib) {
                if (Examine(outlock, outfib)) {
                    len = outfib->fib_Size;
                }
                FreeMem(outfib, (ULONG)sizeof(struct FileInfoBlock));
            }
            UnLock(outlock);
        }
        lhx_dbg_l((STRPTR)"extracted size", len);
        if (len != expected_size) {
            lhx_dbg_ll((STRPTR)"size mismatch got want", len, expected_size);
            lhx_print_error((STRPTR)"archive entry size mismatch", 0);
            goto out;
        }
    }
    lhx_work_done(args, 0);
    lhx_dbg_s((STRPTR)"extract_one", (STRPTR)"ok");
    rc = DOSTRUE;
out:
    FreeMem(namebuf, LHX_PATH_LEN);
    FreeMem(pathbuf, LHX_PATH_LEN);
    return rc;
}

struct lhx_fail_ud {
    LONG fail;
    LONG with_paths;
};

static LONG lhx_extract_walk_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR fullname, struct FileInfoBlock *fib, APTR ud)
{
    struct lhx_fail_ud *st;

    st = (struct lhx_fail_ud *)ud;
    if (!lhx_any_selected(fullname, args->files)) {
        return RETURN_OK;
    }
    if (!lhx_extract_one(args, arc, fullname, st->with_paths, fib->fib_Size)) {
        st->fail++;
    }
    return RETURN_OK;
}

LONG lhx_cmd_extract(struct LhxArgs *args, LONG with_paths)
{
    struct LhArchive *arc;
    struct lhx_fail_ud st;
    LONG walkrc;

    lhx_dbg_s((STRPTR)"cmd", (STRPTR)"EXTRACT");
    lhx_dbg_l((STRPTR)"with_paths", with_paths);
    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_dbg_l((STRPTR)"LhOpenArchive LhErr", LhErr());
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    st.fail = 0;
    st.with_paths = with_paths;
    walkrc = lhx_walk_entries(arc, (STRPTR)"", args, lhx_extract_walk_one,
        (APTR)&st);
    LhCloseArchive(arc);
    if (walkrc == RETURN_WARN) {
        return RETURN_WARN;
    }
    return st.fail > 0 ? RETURN_WARN : RETURN_OK;
}

/*
 * Collect archive members whose basename matches want.  Used so NOPATHS
 * ADD replaces path-form members (SYS/C/LhX) when storing as LhX.
 */
#define LHX_PURGE_MAX 32

struct lhx_purge_ud {
    STRPTR want;
    char *list; /* LHX_PURGE_MAX * LHX_PATH_LEN */
    LONG n;
};

static LONG lhx_purge_collect_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR fullname, struct FileInfoBlock *fib, APTR ud)
{
    struct lhx_purge_ud *st;
    char leaf[108];

    (void)args;
    (void)arc;
    st = (struct lhx_purge_ud *)ud;
    if (!lhx_fib_is_file(fib) || !st || !st->want || !fullname) {
        return RETURN_OK;
    }
    if (!lhx_basename(fullname, (STRPTR)leaf, (LONG)sizeof(leaf))) {
        return RETURN_OK;
    }
    if (Stricmp((STRPTR)leaf, st->want) != 0) {
        return RETURN_OK;
    }
    if (st->n >= LHX_PURGE_MAX) {
        return RETURN_OK;
    }
    Strncpy(st->list + st->n * LHX_PATH_LEN, fullname, LHX_PATH_LEN);
    st->n++;
    return RETURN_OK;
}

static void lhx_delete_basename_matches(struct LhArchive *arc,
    struct LhxArgs *args, STRPTR basename)
{
    struct lhx_purge_ud st;
    LONG i;
    ULONG list_sz;

    if (!arc || !basename || !basename[0]) {
        return;
    }
    list_sz = (ULONG)LHX_PURGE_MAX * (ULONG)LHX_PATH_LEN;
    st.want = basename;
    st.n = 0;
    st.list = (char *)AllocMem(list_sz, MEMF_PUBLIC | MEMF_CLEAR);
    if (!st.list) {
        return;
    }
    (void)lhx_walk_entries(arc, (STRPTR)"", args, lhx_purge_collect_one,
        (APTR)&st);
    for (i = 0; i < st.n; i++) {
        if (!LhDeleteFile(arc, (STRPTR)(st.list + i * LHX_PATH_LEN))) {
            lhx_print_error((STRPTR)"replace delete failed", LhErr());
        }
    }
    FreeMem(st.list, list_sz);
}

LONG lhx_cmd_add(struct LhxArgs *args)
{
    struct LhArchive *arc;
    struct LhArchive *probe;
    LONG i;
    LONG j;
    LONG nfiles;
    APTR data;
    LONG len;
    LONG mode;
    BPTR testlock;
    BPTR flock;
    BPTR elock;
    struct FileInfoBlock *fib;
    struct TagItem tags[5];
    ULONG n;
    STRPTR *files;
    LONG xerr;
    char *nameheap;
    UBYTE *replace;
    STRPTR arcname;

    if (!args->files || !args->files[0]) {
        lhx_print_error((STRPTR)"ADD requires FILES", 0);
        return RETURN_FAIL;
    }
    files = lhx_expand_file_args(args->files, &xerr);
    if (files == NULL) {
        return (xerr == ERROR_BREAK) ? RETURN_WARN : RETURN_FAIL;
    }
    args->files = files;

    nfiles = 0;
    while (args->files[nfiles] != NULL) {
        nfiles++;
    }
    if (nfiles <= 0) {
        lhx_print_error((STRPTR)"ADD requires FILES", 0);
        return RETURN_FAIL;
    }

    nameheap = (char *)AllocMem((ULONG)nfiles * (ULONG)LHX_PATH_LEN,
        MEMF_PUBLIC | MEMF_CLEAR);
    replace = (UBYTE *)AllocMem((ULONG)nfiles, MEMF_PUBLIC | MEMF_CLEAR);
    if (!nameheap || !replace) {
        if (nameheap) {
            FreeMem(nameheap, (ULONG)nfiles * (ULONG)LHX_PATH_LEN);
        }
        if (replace) {
            FreeMem(replace, (ULONG)nfiles);
        }
        return RETURN_FAIL;
    }

    for (i = 0; i < nfiles; i++) {
        /*
         * Default: store the path as given/expanded (SYS/C/LhX).  NOPATHS
         * strips to basename only.  Volume ':' becomes '/' in the member;
         * capitalisation follows the filesystem.
         */
        if (!lhx_make_arcname(args->files[i],
                (STRPTR)(nameheap + i * LHX_PATH_LEN), LHX_PATH_LEN,
                (LONG)args->nopaths)) {
            nameheap[i * LHX_PATH_LEN] = '\0';
        }
    }

    /* Same member name twice in one ADD: later file replaces earlier. */
    for (i = 0; i < nfiles; i++) {
        if (nameheap[i * LHX_PATH_LEN] == '\0') {
            continue;
        }
        for (j = 0; j < i; j++) {
            if (nameheap[j * LHX_PATH_LEN] == '\0') {
                continue;
            }
            if (Stricmp((STRPTR)(nameheap + i * LHX_PATH_LEN),
                    (STRPTR)(nameheap + j * LHX_PATH_LEN)) == 0) {
                replace[i] = 1;
                break;
            }
        }
    }

    testlock = Lock(args->archive, ACCESS_READ);
    if (testlock != (BPTR)NULL) {
        UnLock(testlock);
        mode = LHARC_MODE_APPEND;
    } else {
        mode = LHARC_MODE_WRITE;
    }

    /*
     * APPEND has no catalog for LhLock.  Probe READ first so we only
     * LhDeleteFile when a same-named member is already present.
     */
    if (mode == LHARC_MODE_APPEND) {
        probe = lhx_open_archive(args->archive, LHARC_MODE_READ,
            args->password);
        if (probe) {
            fib = (struct FileInfoBlock *)AllocMem(
                (ULONG)sizeof(struct FileInfoBlock),
                MEMF_PUBLIC | MEMF_CLEAR);
            if (fib) {
                for (i = 0; i < nfiles; i++) {
                    if (nameheap[i * LHX_PATH_LEN] == '\0') {
                        continue;
                    }
                    elock = LhLock(probe,
                        (STRPTR)(nameheap + i * LHX_PATH_LEN));
                    if (elock != (BPTR)NULL) {
                        if (LhExamine(elock, fib) && lhx_fib_is_file(fib)) {
                            replace[i] = 1;
                        }
                        LhUnLock(elock);
                    }
                }
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            }
            LhCloseArchive(probe);
        }
    }

    arc = lhx_open_archive(args->archive, mode, args->password);
    if (!arc) {
        lhx_print_error((STRPTR)"cannot open archive for output", LhErr());
        FreeMem(nameheap, (ULONG)nfiles * (ULONG)LHX_PATH_LEN);
        FreeMem(replace, (ULONG)nfiles);
        return RETURN_FAIL;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        LhCloseArchive(arc);
        FreeMem(nameheap, (ULONG)nfiles * (ULONG)LHX_PATH_LEN);
        FreeMem(replace, (ULONG)nfiles);
        return RETURN_FAIL;
    }
    for (i = 0; i < nfiles; i++) {
        if (lhx_check_break()) {
            Printf("LhX: *** Break\n", 0);
            Flush(Output());
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            LhCloseArchive(arc);
            FreeMem(nameheap, (ULONG)nfiles * (ULONG)LHX_PATH_LEN);
            FreeMem(replace, (ULONG)nfiles);
            return RETURN_WARN;
        }
        arcname = (STRPTR)(nameheap + i * LHX_PATH_LEN);
        if (arcname[0] == '\0') {
            continue;
        }
        data = NULL;
        len = 0;
        if (!lhx_read_file(args->files[i], &data, &len)) {
            lhx_print_error((STRPTR)"cannot read file", IoErr());
            continue;
        }
        lhx_add_line(args, arcname);

        /*
         * LhAddEntry always appends; remove prior members first so ADD
         * replaces rather than duplicating.  NOPATHS also drops any
         * path-form member with the same basename (e.g. SYS/C/LhX when
         * storing LhX) -- exact-name delete alone left those behind.
         */
        if (args->nopaths) {
            lhx_delete_basename_matches(arc, args, arcname);
        } else if (replace[i]) {
            if (!LhDeleteFile(arc, arcname)) {
                lhx_print_error((STRPTR)"replace delete failed", LhErr());
                if (data) {
                    FreeMem(data, (ULONG)len);
                }
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                LhCloseArchive(arc);
                FreeMem(nameheap, (ULONG)nfiles * (ULONG)LHX_PATH_LEN);
                FreeMem(replace, (ULONG)nfiles);
                return RETURN_FAIL;
            }
        }

        n = 0;
        flock = Lock(args->files[i], ACCESS_READ);
        if (flock != (BPTR)NULL && Examine(flock, fib)) {
            tags[n].ti_Tag = LHADD_Attrs;
            tags[n].ti_Data = (ULONG)fib->fib_Protection;
            n++;
            tags[n].ti_Tag = LHADD_DateStamp;
            tags[n].ti_Data = (ULONG)&fib->fib_Date;
            n++;
            if (fib->fib_Comment[0] != '\0') {
                tags[n].ti_Tag = LHADD_Comment;
                tags[n].ti_Data = (ULONG)fib->fib_Comment;
                n++;
            }
        }
        if (flock != (BPTR)NULL) {
            UnLock(flock);
        }
        tags[n].ti_Tag = TAG_DONE;

        if (!LhAddEntryA(arc, arcname, data, len, tags)) {
            lhx_print_error((STRPTR)"add failed", LhErr());
            if (data) {
                FreeMem(data, (ULONG)len);
            }
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            LhCloseArchive(arc);
            FreeMem(nameheap, (ULONG)nfiles * (ULONG)LHX_PATH_LEN);
            FreeMem(replace, (ULONG)nfiles);
            return RETURN_FAIL;
        }
        if (data) {
            FreeMem(data, (ULONG)len);
        }
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
    FreeMem(nameheap, (ULONG)nfiles * (ULONG)LHX_PATH_LEN);
    FreeMem(replace, (ULONG)nfiles);
    return RETURN_OK;
}

struct lhx_test_ud {
    LONG fail;
};

static LONG lhx_test_walk_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR fullname, struct FileInfoBlock *fib, APTR ud)
{
    struct lhx_test_ud *st;
    LONG len;

    st = (struct lhx_test_ud *)ud;
    if (!lhx_any_selected(fullname, args->files)) {
        return RETURN_OK;
    }
    len = LhTestEntry(arc, fullname);
    if (len < 0 || len != fib->fib_Size) {
        st->fail++;
        return RETURN_ERROR;
    }
    lhx_work_start(args, fullname);
    lhx_work_done(args, 1);
    return RETURN_OK;
}

LONG lhx_cmd_test(struct LhxArgs *args)
{
    struct LhArchive *arc;
    struct lhx_test_ud st;
    LONG walkrc;

    lhx_dbg_s((STRPTR)"cmd", (STRPTR)"TEST");
    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_dbg_l((STRPTR)"LhOpenArchive LhErr", LhErr());
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    st.fail = 0;
    walkrc = lhx_walk_entries(arc, (STRPTR)"", args, lhx_test_walk_one,
        (APTR)&st);
    LhCloseArchive(arc);
    if (walkrc == RETURN_WARN) {
        return RETURN_WARN;
    }
    if (walkrc != RETURN_OK || st.fail > 0) {
        return RETURN_FAIL;
    }
    return RETURN_OK;
}

static LONG lhx_print_walk_one(struct LhxArgs *args, struct LhArchive *arc,
    STRPTR fullname, struct FileInfoBlock *fib, APTR ud)
{
    (void)fib;
    (void)ud;
    if (!lhx_any_selected(fullname, args->files)) {
        return RETURN_OK;
    }
    if (!LhPrintEntry(arc, fullname)) {
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

LONG lhx_cmd_print(struct LhxArgs *args)
{
    struct LhArchive *arc;
    LONG walkrc;

    arc = lhx_open_archive(args->archive, LHARC_MODE_READ, args->password);
    if (!arc) {
        lhx_print_error((STRPTR)"cannot open archive", LhErr());
        return RETURN_FAIL;
    }
    walkrc = lhx_walk_entries(arc, (STRPTR)"", args, lhx_print_walk_one, NULL);
    LhCloseArchive(arc);
    if (walkrc == RETURN_WARN) {
        return RETURN_WARN;
    }
    if (walkrc != RETURN_OK) {
        return RETURN_FAIL;
    }
    Flush(Output());
    return RETURN_OK;
}
