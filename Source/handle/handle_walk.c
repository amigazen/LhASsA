/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * handle_walk.c - Walk FROM= and LhAddEntryA into a Bin LHA.
 *
 * Entry names use '/' separators (LHA path style).  Members are stored
 * (LH0) by default until LH5 round-trips are reliable.  FIB protection,
 * DateStamp, and filenote are passed through LHADD_* tags.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/icon.h>
#include <libraries/lh.h>
#include <proto/lh.h>

#include "handle_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;
extern struct Library *LhBase;

/* proto/icon.h stubs use this global. */
struct Library *IconBase = NULL;

/* Classic Parts Project icon (planar); see make_parts_icon.py / Parts.info. */
extern const unsigned char handle_parts_info[];
extern const unsigned long handle_parts_info_len;

void handle_print_error(STRPTR msg, LONG code)
{
    if (!msg) {
        return;
    }
    if (code) {
        Printf("Handle: %s (IoErr %ld)\n", (LONG)msg, code);
    } else {
        Printf("Handle: %s\n", (LONG)msg);
    }
    Flush(Output());
}

void handle_print_usage(void)
{
    /* Never dump a short synopsis; full text is Handle HELP (.help file). */
    Printf("Handle: type Handle HELP for documentation.\n", 0);
    Flush(Output());
}

/*
 * Stage the embedded Parts.info under T: so GetDiskObject() can parse it.
 * Returns 1 if T:LhPartsIcon.info was written.
 */
static LONG handle_stage_parts_template(void)
{
    BPTR fh;
    LONG wrote;

    if (handle_parts_info_len == 0) {
        return 0;
    }
    DeleteFile((STRPTR)"T:LhPartsIcon.info");
    fh = Open((STRPTR)"T:LhPartsIcon.info", MODE_NEWFILE);
    if (fh == (BPTR)NULL) {
        return 0;
    }
    wrote = Write(fh, (APTR)handle_parts_info, (LONG)handle_parts_info_len);
    Close(fh);
    if (wrote != (LONG)handle_parts_info_len) {
        DeleteFile((STRPTR)"T:LhPartsIcon.info");
        return 0;
    }
    return 1;
}

/*
 * Companion Workbench icon for the Bin: Parts Project art, DefaultTool Load.
 *
 * icon.library contract (see autodocs / RKM):
 *   GetDiskObject / GetDefDiskObject -> PutDiskObject -> FreeDiskObject
 * If you temporarily replace do_DefaultTool, restore the original pointer
 * before FreeDiskObject so its free list stays consistent.
 */
static LONG handle_write_bin_icon(STRPTR bin_path, ULONG quiet)
{
    struct DiskObject *dobj;
    STRPTR old_tool;
    LONG ok;
    LONG opened_icon;

    if (!bin_path || !bin_path[0]) {
        return 0;
    }

    opened_icon = 0;
    if (!IconBase) {
        IconBase = OpenLibrary((STRPTR)"icon.library", 37L);
        if (!IconBase) {
            if (!quiet) {
                handle_print_error((STRPTR)"cannot open icon.library", 0);
            }
            return 0;
        }
        opened_icon = 1;
    }

    dobj = NULL;
    if (handle_stage_parts_template()) {
        dobj = GetDiskObject((STRPTR)"T:LhPartsIcon");
        /* Drop the staging file; DiskObject is fully in memory. */
        if (IconBase->lib_Version >= 37) {
            DeleteDiskObject((STRPTR)"T:LhPartsIcon");
        } else {
            DeleteFile((STRPTR)"T:LhPartsIcon.info");
        }
    }
    if (!dobj) {
        dobj = GetDefDiskObject(WBPROJECT);
    }
    if (!dobj) {
        if (!quiet) {
            handle_print_error((STRPTR)"cannot get Project icon", IoErr());
        }
        if (opened_icon) {
            CloseLibrary(IconBase);
            IconBase = NULL;
        }
        return 0;
    }

    /*
     * PutDiskObject writes name.info.  Swap DefaultTool only for the
     * write, then put the original pointer back before FreeDiskObject.
     */
    old_tool = dobj->do_DefaultTool;
    dobj->do_DefaultTool = (STRPTR)"Load";
    dobj->do_Type = WBPROJECT;

    ok = PutDiskObject(bin_path, dobj);

    dobj->do_DefaultTool = old_tool;
    FreeDiskObject(dobj);

    if (opened_icon) {
        CloseLibrary(IconBase);
        IconBase = NULL;
    }

    if (!ok) {
        if (!quiet) {
            handle_print_error((STRPTR)"cannot write Project icon", IoErr());
            Printf("Handle:   %s.info\n", (LONG)bin_path);
            Flush(Output());
        }
        return 0;
    }
    if (!quiet) {
        Printf("Handle: wrote %s.info\n", (LONG)bin_path);
        Flush(Output());
    }
    return 1;
}

/* Delete path.info beside an existing Bin (FORCE overwrite). */
static void handle_delete_bin_info(STRPTR bin_path)
{
    LONG opened_icon;

    if (!bin_path || !bin_path[0]) {
        return;
    }

    opened_icon = 0;
    if (!IconBase) {
        IconBase = OpenLibrary((STRPTR)"icon.library", 37L);
        if (IconBase) {
            opened_icon = 1;
        }
    }
    if (IconBase && IconBase->lib_Version >= 37) {
        DeleteDiskObject(bin_path);
    } else {
        char info_path[HANDLE_PATH_LEN];
        LONG L;

        Strncpy((STRPTR)info_path, bin_path, HANDLE_PATH_LEN);
        L = 0;
        while (info_path[L] != '\0' && L < HANDLE_PATH_LEN - 1) {
            L++;
        }
        if (L + 5 < HANDLE_PATH_LEN) {
            info_path[L++] = '.';
            info_path[L++] = 'i';
            info_path[L++] = 'n';
            info_path[L++] = 'f';
            info_path[L++] = 'o';
            info_path[L] = '\0';
            DeleteFile((STRPTR)info_path);
        }
    }
    if (opened_icon) {
        CloseLibrary(IconBase);
        IconBase = NULL;
    }
}

/*
 * Dot-prefixed names are hidden on Unix but often appear in stages copied
 * from cross-dev trees (.git, .svn, .cursor, ...).  Never pack them.
 */
LONG handle_is_hidden_name(STRPTR name)
{
    if (!name || !name[0]) {
        return 0;
    }
    if (name[0] == '.') {
        return 1;
    }
    return 0;
}

ULONG handle_detect_sys_mode(STRPTR from_path)
{
    STRPTR leaf;
    char buf[HANDLE_PATH_LEN];
    LONG n;

    if (!from_path || !from_path[0]) {
        return 0;
    }
    /*
     * Copy and strip a trailing '/' so FilePart("sys/") still yields "sys".
     * Amiga FilePart on a path ending in '/' can return an empty string.
     */
    Strncpy((STRPTR)buf, from_path, HANDLE_PATH_LEN);
    n = 0;
    while (buf[n] != '\0') {
        n++;
    }
    while (n > 1 && buf[n - 1] == '/') {
        n--;
        buf[n] = '\0';
    }
    leaf = FilePart((STRPTR)buf);
    if (leaf && leaf[0] && Stricmp(leaf, (STRPTR)"SYS") == 0) {
        return HANDLE_MODE_SYS;
    }
    return HANDLE_MODE_APP;
}

static void handle_str_cat(STRPTR dst, LONG dstlen, STRPTR src)
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

LONG handle_build_out_path(struct HandleArgs *args, STRPTR out, LONG outlen)
{
    char leaf[HANDLE_VAL_LEN];

    if (!args || !out || outlen <= 0 || !args->name || !args->name[0]) {
        return 0;
    }
    Strncpy((STRPTR)leaf, args->name, HANDLE_VAL_LEN);
    handle_str_cat((STRPTR)leaf, HANDLE_VAL_LEN, (STRPTR)".bin");

    if (args->out && args->out[0]) {
        Strncpy(out, args->out, outlen);
        if (!AddPart(out, (STRPTR)leaf, (ULONG)outlen)) {
            return 0;
        }
    } else {
        Strncpy(out, (STRPTR)leaf, outlen);
    }
    return 1;
}

LONG handle_path_join(STRPTR out, LONG outlen, STRPTR dir, STRPTR name)
{
    if (!out || outlen <= 0 || !name) {
        return 0;
    }
    if (!dir || !dir[0]) {
        Strncpy(out, name, outlen);
        return 1;
    }
    Strncpy(out, dir, outlen);
    if (!AddPart(out, name, (ULONG)outlen)) {
        return 0;
    }
    return 1;
}

/*
 * Archive-relative path: join with '/' (not Amiga AddPart).
 * rel empty => just name; else rel + '/' + name.
 */
LONG handle_arc_join(STRPTR out, LONG outlen, STRPTR rel, STRPTR name)
{
    LONG rlen;
    LONG nlen;
    LONG i;

    if (!out || outlen <= 0 || !name) {
        return 0;
    }
    nlen = 0;
    while (name[nlen] != '\0') {
        nlen++;
    }
    if (!rel || !rel[0]) {
        if (nlen >= outlen) {
            return 0;
        }
        for (i = 0; i < nlen; i++) {
            out[i] = name[i];
        }
        out[nlen] = '\0';
        return 1;
    }
    rlen = 0;
    while (rel[rlen] != '\0') {
        rlen++;
    }
    if (rlen + 1 + nlen >= outlen) {
        return 0;
    }
    for (i = 0; i < rlen; i++) {
        out[i] = rel[i];
    }
    out[rlen] = '/';
    for (i = 0; i < nlen; i++) {
        out[rlen + 1 + i] = name[i];
    }
    out[rlen + 1 + nlen] = '\0';
    return 1;
}

LONG handle_read_file(STRPTR path, APTR *data, LONG *len)
{
    BPTR fh;
    LONG size;
    APTR buf;
    LONG got;

    *data = NULL;
    *len = 0;
    if (!path) {
        return 0;
    }
    fh = Open(path, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        return 0;
    }
    /*
     * Seek returns the previous position.  Move to end, then Seek back to
     * start; the value returned is the file size.
     */
    if (Seek(fh, 0L, OFFSET_END) < 0) {
        Close(fh);
        return 0;
    }
    size = Seek(fh, 0L, OFFSET_BEGINNING);
    if (size < 0) {
        Close(fh);
        return 0;
    }
    if (size == 0) {
        Close(fh);
        return 1;
    }
    buf = AllocMem((ULONG)size, MEMF_PUBLIC);
    if (!buf) {
        Close(fh);
        return 0;
    }
    got = Read(fh, buf, size);
    Close(fh);
    if (got != size) {
        FreeMem(buf, (ULONG)size);
        return 0;
    }
    *data = buf;
    *len = size;
    return 1;
}

static LONG handle_add_one(
    struct HandleArgs *args,
    struct LhArchive *arc,
    STRPTR disk_path,
    STRPTR arc_name,
    struct FileInfoBlock *fib)
{
    APTR data;
    LONG len;
    struct TagItem tags[5];
    ULONG n;

    data = NULL;
    len = 0;
    if (!handle_read_file(disk_path, &data, &len)) {
        handle_print_error((STRPTR)"cannot read file", IoErr());
        Printf("  %s\n", (LONG)disk_path);
        Flush(Output());
        return 0;
    }
    if (!args->quiet) {
        Printf("ADD %s\n", (LONG)arc_name);
        Flush(Output());
    }

    n = 0;
    if (fib) {
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
    tags[n].ti_Tag = TAG_DONE;

    if (!LhAddEntryA(arc, arc_name, data, len, tags)) {
        handle_print_error((STRPTR)"LhAddEntryA failed", LhErr());
        Printf("  %s\n", (LONG)arc_name);
        Flush(Output());
        if (data) {
            FreeMem(data, (ULONG)len);
        }
        return 0;
    }
    if (data) {
        FreeMem(data, (ULONG)len);
    }
    return 1;
}

/*
 * Recursively add files under dir_lock.
 * disk_dir = Amiga path to this directory (for Open/Lock).
 * arc_rel  = archive-relative prefix ('' at stage root).
 */
static LONG handle_walk_dir(
    struct HandleArgs *args,
    struct LhArchive *arc,
    BPTR dir_lock,
    STRPTR disk_dir,
    STRPTR arc_rel)
{
    struct FileInfoBlock *fib;
    LONG ok;
    STRPTR child_disk;
    STRPTR child_arc;
    BPTR child_lock;

    /* Recursive -- heap path buffers so deep trees fit in a 4K stack. */
    child_disk = (STRPTR)AllocMem(HANDLE_PATH_LEN, MEMF_PUBLIC | MEMF_CLEAR);
    child_arc = (STRPTR)AllocMem(HANDLE_PATH_LEN, MEMF_PUBLIC | MEMF_CLEAR);
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!child_disk || !child_arc || !fib) {
        if (child_disk) {
            FreeMem(child_disk, HANDLE_PATH_LEN);
        }
        if (child_arc) {
            FreeMem(child_arc, HANDLE_PATH_LEN);
        }
        if (fib) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        }
        handle_print_error((STRPTR)"out of memory", 0);
        return 0;
    }

    ok = Examine(dir_lock, fib);
    if (!ok || fib->fib_DirEntryType <= 0) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        FreeMem(child_arc, HANDLE_PATH_LEN);
        FreeMem(child_disk, HANDLE_PATH_LEN);
        handle_print_error((STRPTR)"not a directory", IoErr());
        return 0;
    }

    while (ExNext(dir_lock, fib)) {
        if (fib->fib_FileName[0] == '\0') {
            continue;
        }
        if (handle_is_hidden_name(fib->fib_FileName)) {
            continue;
        }

        if (!handle_path_join(child_disk, HANDLE_PATH_LEN, disk_dir,
                fib->fib_FileName)) {
            handle_print_error((STRPTR)"path too long", 0);
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            FreeMem(child_arc, HANDLE_PATH_LEN);
            FreeMem(child_disk, HANDLE_PATH_LEN);
            return 0;
        }
        if (!handle_arc_join(child_arc, HANDLE_PATH_LEN, arc_rel,
                fib->fib_FileName)) {
            handle_print_error((STRPTR)"archive path too long", 0);
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            FreeMem(child_arc, HANDLE_PATH_LEN);
            FreeMem(child_disk, HANDLE_PATH_LEN);
            return 0;
        }

        /* Handle-generated Bin profile members. */
        if (Stricmp(fib->fib_FileName, (STRPTR)"Manifest") == 0
            || Stricmp(fib->fib_FileName, (STRPTR)HANDLE_BIN_PACKAGE_VER) == 0
            || Stricmp(fib->fib_FileName, (STRPTR)HANDLE_BIN_AUTOSHOW) == 0) {
            continue;
        }

        if (fib->fib_DirEntryType > 0) {
            child_lock = Lock(child_disk, ACCESS_READ);
            if (child_lock == (BPTR)NULL) {
                handle_print_error((STRPTR)"cannot lock subdirectory", IoErr());
                Printf("  %s\n", (LONG)child_disk);
                Flush(Output());
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                FreeMem(child_arc, HANDLE_PATH_LEN);
                FreeMem(child_disk, HANDLE_PATH_LEN);
                return 0;
            }
            if (!handle_walk_dir(args, arc, child_lock, child_disk,
                    child_arc)) {
                UnLock(child_lock);
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                FreeMem(child_arc, HANDLE_PATH_LEN);
                FreeMem(child_disk, HANDLE_PATH_LEN);
                return 0;
            }
            UnLock(child_lock);
        } else {
            if (!handle_add_one(args, arc, child_disk, child_arc, fib)) {
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                FreeMem(child_arc, HANDLE_PATH_LEN);
                FreeMem(child_disk, HANDLE_PATH_LEN);
                return 0;
            }
        }
    }

    /* ExNext ends with ERROR_NO_MORE_ENTRIES on success. */
    if (IoErr() != ERROR_NO_MORE_ENTRIES) {
        handle_print_error((STRPTR)"directory scan failed", IoErr());
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        FreeMem(child_arc, HANDLE_PATH_LEN);
        FreeMem(child_disk, HANDLE_PATH_LEN);
        return 0;
    }

    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    FreeMem(child_arc, HANDLE_PATH_LEN);
    FreeMem(child_disk, HANDLE_PATH_LEN);
    return 1;
}

LONG handle_run(struct HandleArgs *args)
{
    BPTR from_lock;
    BPTR test_lock;
    struct FileInfoBlock *fib;
    struct LhArchive *arc;
    char arc_prefix[HANDLE_PATH_LEN];
    LONG count_ok;
    struct HandleScan *scan;
    STRPTR manbuf;
    char verbuf[HANDLE_VER_LINE_MAX];
    LONG manlen;
    LONG verlen;
    struct TagItem tags[2];
    struct TagItem store_tags[2];
    LONG rc;

    /*
     * HandleScan + Manifest text are far larger than the default stack
     * ($STACK was 16K; scan alone is ~80K).  Allocate from the heap.
     */
    scan = (struct HandleScan *)AllocMem(
        (ULONG)sizeof(struct HandleScan), MEMF_PUBLIC | MEMF_CLEAR);
    if (!scan) {
        handle_print_error((STRPTR)"out of memory (scan)", 0);
        return RETURN_FAIL;
    }
    manbuf = (STRPTR)AllocMem(HANDLE_MANIFEST_MAX, MEMF_PUBLIC | MEMF_CLEAR);
    if (!manbuf) {
        FreeMem(scan, (ULONG)sizeof(struct HandleScan));
        handle_print_error((STRPTR)"out of memory (manifest)", 0);
        return RETURN_FAIL;
    }

    rc = RETURN_FAIL;
    arc_prefix[0] = '\0';
    from_lock = Lock(args->from_path, ACCESS_READ);
    if (from_lock == (BPTR)NULL) {
        handle_print_error((STRPTR)"cannot lock FROM directory", IoErr());
        goto out_free;
    }

    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        UnLock(from_lock);
        handle_print_error((STRPTR)"out of memory", 0);
        goto out_free;
    }
    if (!Examine(from_lock, fib) || fib->fib_DirEntryType <= 0) {
        handle_print_error((STRPTR)"FROM must be a directory", IoErr());
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        UnLock(from_lock);
        goto out_free;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    fib = NULL;

    if (!handle_scan_stage(args, scan)) {
        UnLock(from_lock);
        goto out_free;
    }
    if (!handle_build_manifest(scan, manbuf, HANDLE_MANIFEST_MAX)) {
        UnLock(from_lock);
        handle_print_error((STRPTR)"cannot build Manifest", 0);
        goto out_free;
    }
    if (!handle_build_out_path(args, args->out_path, HANDLE_PATH_LEN)) {
        UnLock(from_lock);
        handle_print_error((STRPTR)"cannot build output path", 0);
        goto out_free;
    }
    if (!handle_build_package_ver(scan, (STRPTR)verbuf, HANDLE_VER_LINE_MAX)) {
        UnLock(from_lock);
        handle_print_error((STRPTR)"cannot build package $VER", 0);
        goto out_free;
    }
    manlen = 0;
    while (manbuf[manlen] != '\0') {
        manlen++;
    }
    verlen = 0;
    while (verbuf[verlen] != '\0') {
        verlen++;
    }
    if (!args->quiet) {
        Printf("Handle: %s %s (%ld File: lines",
            (LONG)scan->name, (LONG)scan->version, scan->nfiles);
        if (args->sys_mode) {
            Printf(", system", 0);
        } else {
            Printf(", application", 0);
        }
        Printf(")\n", 0);
        Flush(Output());
    }

    test_lock = Lock(args->out_path, ACCESS_READ);
    if (test_lock != (BPTR)NULL) {
        UnLock(test_lock);
        if (!args->force) {
            UnLock(from_lock);
            handle_print_error((STRPTR)"output already exists (use FORCE)", 0);
            Printf("Handle:   %s\n", (LONG)args->out_path);
            Flush(Output());
            goto out_free;
        }
        if (!DeleteFile(args->out_path)) {
            UnLock(from_lock);
            handle_print_error((STRPTR)"cannot delete existing output", IoErr());
            goto out_free;
        }
        /* Drop stale companion icon before rewriting the Bin. */
        handle_delete_bin_info(args->out_path);
    }

    {
        LONG tlen;
        STRPTR tfp;

        tlen = 0;
        while (args->out_path[tlen] != '\0') {
            tlen++;
        }
        if (tlen > 0 && args->out_path[tlen - 1] == ':') {
            UnLock(from_lock);
            handle_print_error((STRPTR)"output must be a file, not a volume root",
                0);
            goto out_free;
        }
        tfp = FilePart(args->out_path);
        if (!tfp || !tfp[0]) {
            UnLock(from_lock);
            handle_print_error((STRPTR)"output must be a file, not a volume root",
                0);
            goto out_free;
        }
    }

    arc = LhOpenArchive(args->out_path, LHARC_MODE_WRITE);
    if (!arc) {
        UnLock(from_lock);
        handle_print_error((STRPTR)"cannot create archive", LhErr());
        goto out_free;
    }

    store_tags[0].ti_Tag = LHADD_Method;
    store_tags[0].ti_Data = 0;  /* LH0 — stored, not compressed */
    store_tags[1].ti_Tag = TAG_DONE;

    if (!LhAddEntryA(arc, (STRPTR)HANDLE_BIN_PACKAGE_VER, (STRPTR)verbuf,
            verlen, store_tags)) {
        LhCloseArchive(arc);
        UnLock(from_lock);
        DeleteFile(args->out_path);
        handle_print_error((STRPTR)"cannot add -package.ver", LhErr());
        goto out_free;
    }
    if (!args->quiet) {
        Printf("ADD %s\n", (LONG)HANDLE_BIN_PACKAGE_VER);
        Flush(Output());
    }

    {
        STRPTR dmbuf;
        LONG dmlen;

        dmbuf = (STRPTR)AllocMem(HANDLE_MANIFEST_MAX, MEMF_PUBLIC | MEMF_CLEAR);
        if (!dmbuf) {
            LhCloseArchive(arc);
            UnLock(from_lock);
            DeleteFile(args->out_path);
            handle_print_error((STRPTR)"out of memory (displayme)", 0);
            goto out_free;
        }
        if (!handle_build_displayme(scan, dmbuf, HANDLE_MANIFEST_MAX)) {
            FreeMem(dmbuf, HANDLE_MANIFEST_MAX);
            LhCloseArchive(arc);
            UnLock(from_lock);
            DeleteFile(args->out_path);
            handle_print_error((STRPTR)"cannot build Load.displayme", 0);
            goto out_free;
        }
        dmlen = 0;
        while (dmbuf[dmlen] != '\0') {
            dmlen++;
        }
        if (!LhAddEntryA(arc, (STRPTR)HANDLE_BIN_AUTOSHOW, dmbuf, dmlen,
                store_tags)) {
            FreeMem(dmbuf, HANDLE_MANIFEST_MAX);
            LhCloseArchive(arc);
            UnLock(from_lock);
            DeleteFile(args->out_path);
            handle_print_error((STRPTR)"cannot add Load.displayme", LhErr());
            goto out_free;
        }
        FreeMem(dmbuf, HANDLE_MANIFEST_MAX);
    }
    if (!args->quiet) {
        Printf("ADD %s\n", (LONG)HANDLE_BIN_AUTOSHOW);
        Flush(Output());
    }

    tags[0].ti_Tag = TAG_DONE;
    if (!LhAddEntryA(arc, (STRPTR)"Manifest", manbuf, manlen, tags)) {
        LhCloseArchive(arc);
        UnLock(from_lock);
        DeleteFile(args->out_path);
        handle_print_error((STRPTR)"cannot add Manifest", LhErr());
        goto out_free;
    }
    if (!args->quiet) {
        Printf("ADD Manifest\n", 0);
        Flush(Output());
    }

    if (args->sys_mode) {
        Strncpy((STRPTR)arc_prefix, (STRPTR)"SYS", HANDLE_PATH_LEN);
    }
    count_ok = handle_walk_dir(args, arc, from_lock, args->from_path,
        (STRPTR)arc_prefix);

    /* Optional NAME.readme from cwd, FROM parent, or under FROM. */
    if (count_ok) {
        char readme_disk[HANDLE_PATH_LEN];
        char readme_arc[HANDLE_VAL_LEN];

        if (handle_find_readme(args, (STRPTR)readme_disk, HANDLE_PATH_LEN,
                (STRPTR)readme_arc, HANDLE_VAL_LEN)) {
            if (!handle_add_one(args, arc, (STRPTR)readme_disk,
                    (STRPTR)readme_arc, NULL)) {
                count_ok = 0;
            }
        }
    }

    LhCloseArchive(arc);
    UnLock(from_lock);

    if (!count_ok) {
        DeleteFile(args->out_path);
        goto out_free;
    }

    if (!args->quiet) {
        Printf("Handle: wrote %s\n", (LONG)args->out_path);
        Flush(Output());
    }

    /*
     * Companion Project icon (DefaultTool Load).  Bin is already good if
     * this fails; warn but still return OK.
     */
    if (!handle_write_bin_icon(args->out_path, args->quiet)) {
        if (!args->quiet) {
            handle_print_error(
                (STRPTR)"Bin written; Project icon was not created", 0);
        }
    }

    rc = RETURN_OK;

out_free:
    FreeMem(manbuf, HANDLE_MANIFEST_MAX);
    FreeMem(scan, (ULONG)sizeof(struct HandleScan));
    return rc;
}
