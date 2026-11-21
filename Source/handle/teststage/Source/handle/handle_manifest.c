/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * handle_manifest.c - Scan CPU-rooted stage and generate Manifest text.
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

static int handle_is_cpu_dir(STRPTR name)
{
    if (!name) {
        return 0;
    }
    if (Stricmp(name, (STRPTR)"any") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"68000") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"68010") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"68020") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"68030") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"68040") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"68060") == 0) {
        return 1;
    }
    return 0;
}

static int handle_is_assign_stem(STRPTR name)
{
    if (!name) {
        return 0;
    }
    if (Stricmp(name, (STRPTR)"LIBS") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"C") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"Devs") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"L") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"Prefs") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"Fonts") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"Expansion") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"Locale") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"System") == 0) {
        return 1;
    }
    if (Stricmp(name, (STRPTR)"S") == 0) {
        return 1;
    }
    return 0;
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

static void handle_append_ulong(STRPTR dst, LONG dstlen, ULONG v)
{
    char tmp[12];
    LONG n;
    LONG i;
    LONG L;

    n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < 11) {
            tmp[n++] = (char)('0' + (v % 10UL));
            v /= 10UL;
        }
    }
    L = 0;
    while (dst[L] != '\0' && L < dstlen - 1) {
        L++;
    }
    for (i = n - 1; i >= 0 && L < dstlen - 1; i--) {
        dst[L++] = tmp[i];
    }
    dst[L] = '\0';
}

static LONG handle_add_file(struct HandleScan *scan, STRPTR dest,
    STRPTR from, STRPTR cpu)
{
    struct HandleFileVar *fv;

    if (scan->nfiles >= HANDLE_MAX_FILES) {
        handle_print_error((STRPTR)"too many staged files", 0);
        return 0;
    }
    fv = &scan->files[scan->nfiles];
    Strncpy(fv->dest, dest, HANDLE_VAL_LEN);
    Strncpy(fv->from, from, HANDLE_VAL_LEN);
    Strncpy(fv->cpu, cpu, (LONG)sizeof(fv->cpu));
    scan->nfiles++;
    return 1;
}

/*
 * Walk assign subtree: FROM/cpu/ASSIGN/rest...
 * arc_rel is "cpu/ASSIGN/..." for From=.
 * dest is "ASSIGN:rest..."
 */
static LONG handle_walk_assign(struct HandleScan *scan, BPTR dir_lock,
    STRPTR disk_dir, STRPTR arc_rel, STRPTR dest_prefix)
{
    struct FileInfoBlock *fib;
    char child_disk[HANDLE_PATH_LEN];
    char child_arc[HANDLE_PATH_LEN];
    char child_dest[HANDLE_VAL_LEN];
    BPTR child_lock;

    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        return 0;
    }
    if (!Examine(dir_lock, fib) || fib->fib_DirEntryType <= 0) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        return 0;
    }
    while (ExNext(dir_lock, fib)) {
        if (fib->fib_FileName[0] == '.' &&
            (fib->fib_FileName[1] == '\0' ||
             (fib->fib_FileName[1] == '.' && fib->fib_FileName[2] == '\0'))) {
            continue;
        }
        if (!handle_path_join((STRPTR)child_disk, HANDLE_PATH_LEN, disk_dir,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        if (!handle_arc_join((STRPTR)child_arc, HANDLE_PATH_LEN, arc_rel,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        /* dest_prefix is like LIBS: or LIBS:subdir/ */
        Strncpy((STRPTR)child_dest, dest_prefix, HANDLE_VAL_LEN);
        handle_str_cat((STRPTR)child_dest, HANDLE_VAL_LEN, fib->fib_FileName);

        if (fib->fib_DirEntryType > 0) {
            handle_str_cat((STRPTR)child_dest, HANDLE_VAL_LEN, (STRPTR)"/");
            child_lock = Lock((STRPTR)child_disk, ACCESS_READ);
            if (!child_lock) {
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
            if (!handle_walk_assign(scan, child_lock, (STRPTR)child_disk,
                    (STRPTR)child_arc, (STRPTR)child_dest)) {
                UnLock(child_lock);
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
            UnLock(child_lock);
        } else {
            /* CPU is first component of arc_rel (cpu/...). */
            char cpu[16];
            LONG i;

            cpu[0] = '\0';
            for (i = 0; arc_rel[i] != '\0' && arc_rel[i] != '/' && i < 15; i++) {
                cpu[i] = arc_rel[i];
            }
            cpu[i] = '\0';
            if (!handle_add_file(scan, (STRPTR)child_dest, (STRPTR)child_arc,
                    (STRPTR)cpu)) {
                FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
                return 0;
            }
        }
    }
    if (IoErr() != ERROR_NO_MORE_ENTRIES) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        return 0;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    return 1;
}

static LONG handle_walk_cpu(struct HandleScan *scan, BPTR cpu_lock,
    STRPTR disk_cpu, STRPTR cpu_name)
{
    struct FileInfoBlock *fib;
    char child_disk[HANDLE_PATH_LEN];
    char arc_rel[HANDLE_PATH_LEN];
    char dest_prefix[HANDLE_VAL_LEN];
    BPTR child_lock;

    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        return 0;
    }
    if (!Examine(cpu_lock, fib) || fib->fib_DirEntryType <= 0) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        return 0;
    }
    while (ExNext(cpu_lock, fib)) {
        if (fib->fib_DirEntryType <= 0) {
            continue;
        }
        if (!handle_is_assign_stem(fib->fib_FileName)) {
            continue;
        }
        if (!handle_path_join((STRPTR)child_disk, HANDLE_PATH_LEN, disk_cpu,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        if (!handle_arc_join((STRPTR)arc_rel, HANDLE_PATH_LEN, cpu_name,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        Strncpy((STRPTR)dest_prefix, fib->fib_FileName, HANDLE_VAL_LEN);
        handle_str_cat((STRPTR)dest_prefix, HANDLE_VAL_LEN, (STRPTR)":");

        child_lock = Lock((STRPTR)child_disk, ACCESS_READ);
        if (!child_lock) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        if (!handle_walk_assign(scan, child_lock, (STRPTR)child_disk,
                (STRPTR)arc_rel, (STRPTR)dest_prefix)) {
            UnLock(child_lock);
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            return 0;
        }
        UnLock(child_lock);
    }
    if (IoErr() != ERROR_NO_MORE_ENTRIES) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        return 0;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    return 1;
}

/*
 * Join FROM root with archive-style relative path (slash-separated).
 */
static LONG handle_disk_from_rel(STRPTR out, LONG outlen, STRPTR root,
    STRPTR rel)
{
    char seg[HANDLE_VAL_LEN];
    LONG i;
    LONG j;

    if (!out || outlen <= 0 || !root || !rel) {
        return 0;
    }
    Strncpy(out, root, outlen);
    i = 0;
    while (rel[i] != '\0') {
        while (rel[i] == '/') {
            i++;
        }
        if (rel[i] == '\0') {
            break;
        }
        j = 0;
        while (rel[i] != '\0' && rel[i] != '/' && j < (LONG)sizeof(seg) - 1) {
            seg[j++] = rel[i++];
        }
        seg[j] = '\0';
        if (!AddPart(out, (STRPTR)seg, (ULONG)outlen)) {
            return 0;
        }
    }
    return 1;
}

LONG handle_scan_stage(STRPTR from_root, struct HandleScan *scan)
{
    BPTR root;
    struct FileInfoBlock *fib;
    char child_disk[HANDLE_PATH_LEN];
    BPTR child_lock;
    LONG i;
    struct HandleVerInfo vi;
    char disk_path[HANDLE_PATH_LEN];
    STRPTR fp;

    if (!from_root || !scan) {
        return 0;
    }
    scan->name[0] = '\0';
    scan->version[0] = '\0';
    scan->nfiles = 0;

    root = Lock(from_root, ACCESS_READ);
    if (!root) {
        handle_print_error((STRPTR)"cannot lock FROM", IoErr());
        return 0;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        UnLock(root);
        return 0;
    }
    if (!Examine(root, fib) || fib->fib_DirEntryType <= 0) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        UnLock(root);
        handle_print_error((STRPTR)"FROM must be a directory", 0);
        return 0;
    }

    while (ExNext(root, fib)) {
        if (fib->fib_DirEntryType <= 0) {
            continue;
        }
        if (!handle_is_cpu_dir(fib->fib_FileName)) {
            continue;
        }
        if (!handle_path_join((STRPTR)child_disk, HANDLE_PATH_LEN, from_root,
                fib->fib_FileName)) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            UnLock(root);
            return 0;
        }
        child_lock = Lock((STRPTR)child_disk, ACCESS_READ);
        if (!child_lock) {
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            UnLock(root);
            return 0;
        }
        if (!handle_walk_cpu(scan, child_lock, (STRPTR)child_disk,
                fib->fib_FileName)) {
            UnLock(child_lock);
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            UnLock(root);
            return 0;
        }
        UnLock(child_lock);
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    UnLock(root);

    if (scan->nfiles == 0) {
        handle_print_error((STRPTR)"no CPU/ASSIGN files under FROM", 0);
        Printf("Handle: expected FROM/680xx/LIBS/... layout\n", 0);
        Flush(Output());
        return 0;
    }

    /* Name from first dest basename. */
    fp = FilePart(scan->files[0].dest);
    if (fp && fp[0]) {
        Strncpy(scan->name, fp, HANDLE_VAL_LEN);
    } else {
        Strncpy(scan->name, (STRPTR)"package", HANDLE_VAL_LEN);
    }

    /* All staged binaries must share one $VER major.minor. */
    {
        ULONG maj;
        ULONG min;

        maj = 0;
        min = 0;
        for (i = 0; i < scan->nfiles; i++) {
            if (!handle_disk_from_rel((STRPTR)disk_path, HANDLE_PATH_LEN,
                    from_root, scan->files[i].from)) {
                return 0;
            }
            if (!handle_ver_scan_file((STRPTR)disk_path, &vi)) {
                handle_print_error(
                    (STRPTR)"staged file missing or unparseable $VER:", 0);
                Printf("Handle:   %s\n", (LONG)disk_path);
                Printf("Handle:   need $VER: name maj.min [(dd.mm.yy|dd/mm/yy)]\n",
                    0);
                Flush(Output());
                return 0;
            }
            if (i == 0) {
                maj = vi.major;
                min = vi.minor;
            } else if (vi.major != maj || vi.minor != min) {
                handle_print_error((STRPTR)"package $VER not uniform across files", 0);
                return 0;
            }
        }
        scan->version[0] = '\0';
        handle_append_ulong(scan->version, HANDLE_VAL_LEN, maj);
        handle_str_cat(scan->version, HANDLE_VAL_LEN, (STRPTR)".");
        handle_append_ulong(scan->version, HANDLE_VAL_LEN, min);
    }
    return 1;
}

LONG handle_build_manifest(struct HandleScan *scan, STRPTR out, LONG outlen)
{
    LONG i;

    if (!scan || !out || outlen < 64) {
        return 0;
    }
    out[0] = '\0';
    handle_str_cat(out, outlen, (STRPTR)"Format: 1\n");
    handle_str_cat(out, outlen, (STRPTR)"Name: ");
    handle_str_cat(out, outlen, scan->name);
    handle_str_cat(out, outlen, (STRPTR)"\nVersion: ");
    handle_str_cat(out, outlen, scan->version);
    handle_str_cat(out, outlen, (STRPTR)"\nArch: m68k\nOS: os3\n");
    handle_str_cat(out, outlen, (STRPTR)"InstallRoot: Work:\n");
    handle_str_cat(out, outlen, (STRPTR)"Summary: generated by Handle\n");
    handle_str_cat(out, outlen, (STRPTR)"\n");
    for (i = 0; i < scan->nfiles; i++) {
        handle_str_cat(out, outlen, (STRPTR)"File: ");
        handle_str_cat(out, outlen, scan->files[i].dest);
        handle_str_cat(out, outlen, (STRPTR)" From=");
        handle_str_cat(out, outlen, scan->files[i].from);
        handle_str_cat(out, outlen, (STRPTR)" CPU=");
        handle_str_cat(out, outlen, scan->files[i].cpu);
        handle_str_cat(out, outlen, (STRPTR)"\n");
    }
    return 1;
}
