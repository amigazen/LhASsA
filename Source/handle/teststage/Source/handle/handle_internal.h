/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * handle_internal.h - Handle: build a Bin-profile LHA via lh.library.
 *
 * LHA = Load / Handle / Archive.  Handle packs a CPU-rooted stage and
 * generates Manifest from layout + $VER: scans (shared lh_ver.o).
 *
 * Stage layout:
 *   FROM/68020/LIBS/foo.library  ->  File: LIBS:foo.library From=... CPU=68020
 */

#ifndef HANDLE_INTERNAL_H
#define HANDLE_INTERNAL_H

#include <exec/types.h>
#include <dos/dos.h>

#include "lh_ver.h"

#define HANDLE_PATH_LEN 512
#define HANDLE_MAX_FILES 128
#define HANDLE_VAL_LEN 256
#define HANDLE_MANIFEST_MAX 8192

struct HandleArgs {
    STRPTR from;
    STRPTR to;
    ULONG quiet;
    ULONG force;
    ULONG help;
    char from_path[HANDLE_PATH_LEN];
    char to_path[HANDLE_PATH_LEN];
};

struct HandleFileVar {
    char dest[HANDLE_VAL_LEN];
    char from[HANDLE_VAL_LEN];
    char cpu[16];
};

struct HandleScan {
    char name[HANDLE_VAL_LEN];
    char version[HANDLE_VAL_LEN];
    struct HandleFileVar files[HANDLE_MAX_FILES];
    LONG nfiles;
};

void handle_print_usage(void);
void handle_print_error(STRPTR msg, LONG code);

LONG handle_read_file(STRPTR path, APTR *data, LONG *len);
LONG handle_path_join(STRPTR out, LONG outlen, STRPTR dir, STRPTR name);
LONG handle_arc_join(STRPTR out, LONG outlen, STRPTR rel, STRPTR name);

/* handle_manifest.c */
LONG handle_scan_stage(STRPTR from_root, struct HandleScan *scan);
LONG handle_build_manifest(struct HandleScan *scan, STRPTR out, LONG outlen);

LONG handle_run(struct HandleArgs *args);

#endif /* HANDLE_INTERNAL_H */
