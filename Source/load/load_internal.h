/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * load_internal.h - Load: install / ROLLBACK a Bin (system components MVP).
 *
 * LHA = Load / Handle / Archive.
 * Install: variant pick (CPU/FPU/MMU/WOS/PUP), OS:/Requires:, $VER gate,
 * copy-aside bak under SYS:T/Load, temp place + CRC16.
 * Absolute File: destinations (contain ':') ignore TO=.
 */

#ifndef LOAD_INTERNAL_H
#define LOAD_INTERNAL_H

#include <exec/types.h>
#include <dos/dos.h>

struct WBStartup;

#define LOAD_PATH_LEN 512
#define LOAD_MAX_FILES 128
#define LOAD_MAX_REQS 32
#define LOAD_KEY_LEN 64
#define LOAD_VAL_LEN 256
#define LOAD_TOKEN_LEN 16
#define LOAD_TDIR "SYS:T/Load"
#define LOAD_VER_LINE_MAX 128
/* Packed dest path tables for CLI/WB install summary. */
#define LOAD_SUMMARY_MAX 24
#define LOAD_SUMMARY_LEN 128

struct LoadArgs {
    STRPTR bin;
    STRPTR to;
    STRPTR dest;
    ULONG quiet;
    ULONG force;
    ULONG rollback;
    ULONG help;
    ULONG verbose;      /* CLI VERBOSE/S or WB ToolType VERBOSE */
    ULONG interactive;  /* Workbench: confirm + error requesters */
    char bin_path[LOAD_PATH_LEN];
    char to_path[LOAD_PATH_LEN];
    char dest_path[LOAD_PATH_LEN];
};

/* One Manifest File: line (variant of a logical install target). */
struct LoadFileVar {
    char dest[LOAD_VAL_LEN];
    char from[LOAD_VAL_LEN];
    char cpu[16];
    int fpu881;
    int fpu882;
    int mmu;
    int wos;
    int pup;
    int selected;
    /* From Handle File: Ver= (staged $VER at pack time). */
    int have_ver;
    int have_date;
    ULONG ver_major;
    ULONG ver_minor;
    ULONG ver_ymd;
};

struct LoadRequire {
    char name[LOAD_VAL_LEN];
    char minver[32];
    char url[LOAD_VAL_LEN];
};

struct LoadManifest {
    char name[LOAD_VAL_LEN];
    char version[LOAD_VAL_LEN];
    char install_root[LOAD_VAL_LEN];
    char os_min[32];
    struct LoadFileVar files[LOAD_MAX_FILES];
    LONG nfiles;
    struct LoadRequire reqs[LOAD_MAX_REQS];
    LONG nreqs;
};

/* UpdateSys-style $VER: major.minor and optional (dd.mm.yy) date. */
struct LoadVerInfo {
    int have_ver;
    int have_date;
    ULONG major;
    ULONG minor;
    ULONG ymd;
};

void load_print_usage(void);
void load_print_error(STRPTR msg, LONG code);
/* VERBOSE progress line (CLI Output or WB CON:). */
void load_log(STRPTR msg);

/*
 * Heap helpers for LARGE buffers (multi-path scratch, LoadManifest-sized
 * structs, recursive walk frames).  Small ephemeral locals (leaf[16],
 * verline[128], a single tok[256]) belong on the 4K stack.
 */
APTR load_alloc(ULONG size);
void load_free(APTR p, ULONG size);
STRPTR load_alloc_path(void);
void load_free_path(STRPTR p);

LONG load_path_is_absolute(STRPTR path);
LONG load_path_is_volume_root(STRPTR path);
LONG load_resolve_dest(STRPTR out, LONG outlen, STRPTR dest,
    STRPTR to_override, STRPTR install_root);

int load_host_cpu_rank(void);
int load_cpu_rank(STRPTR cpu);
LONG load_select_variants(struct LoadManifest *man);
LONG load_check_deps(struct LoadManifest *man);
/*
 * Before any bak/place: parents writable (no exclusive lock -- LIBS:/C:
 * stay shared-busy), overwrite targets not locked / not write-protected.
 * Returns 1 if all selected destinations look safe.
 */
LONG load_preflight(struct LoadManifest *man, STRPTR to_override);

LONG load_parse_manifest(STRPTR text, LONG len, struct LoadManifest *man);

/* load_ver.c */
void load_ver_clear(struct LoadVerInfo *vi);
LONG load_ver_parse_line(STRPTR line, struct LoadVerInfo *vi);
/*
 * 1 = parsed $VER:, 0 = I/O error, -1 = no usable tag (missing or only
 * prose "$VER:" hits).  Scan is capped near the start of the file.
 */
LONG load_ver_scan_file(STRPTR path, struct LoadVerInfo *vi);
/*
 * Scan a memory buffer for $VER: (same returns as load_ver_scan_file).
 */
LONG load_ver_scan_mem(APTR data, LONG len, struct LoadVerInfo *vi);
LONG load_ver_parse_dotted(STRPTR s, ULONG *major, ULONG *minor);
/*
 * Manifest File: Ver= token: "major.minor" or "major.minor(dd.mm.yy)".
 */
LONG load_ver_parse_short(STRPTR s, struct LoadVerInfo *vi);
int load_ver_compare(const struct LoadVerInfo *cand,
    const struct LoadVerInfo *inst);

/* load_atomic.c — SYS:T/Load bak + CRC16 place */
UWORD load_crc16_buf(APTR data, LONG len);
LONG load_crc16_file(STRPTR path, UWORD *crc_out);
/*
 * Same size and CRC16 of first/last LOAD_FP_CHUNK bytes (or whole file if
 * smaller).  Used when a member has no usable $VER: (data files, or text
 * that merely mentions "$VER:" in prose).
 */
LONG load_files_same_content(STRPTR a, STRPTR b);

/* Single-instance: named public MsgPort "LhASsA_Load". */
LONG load_claim_instance(void);
void load_release_instance(void);
LONG load_ensure_tdir(void);
LONG load_ensure_parent(STRPTR path);
LONG load_copy_file(STRPTR src, STRPTR dst);
LONG load_write_file(STRPTR path, APTR data, LONG len);
LONG load_clear_write_protect(STRPTR path);
LONG load_alloc_token(STRPTR token, LONG tokenlen);
LONG load_bak_path(STRPTR out, LONG outlen, STRPTR token);
LONG load_sidecar_path(STRPTR out, LONG outlen, STRPTR token);
LONG load_write_sidecar(STRPTR token, STRPTR dest, STRPTR bak,
    const struct LoadVerInfo *ver, UWORD crc);
LONG load_backup_to_t(STRPTR dest, STRPTR token_out, LONG tokenlen,
    STRPTR bak_out, LONG baklen);
LONG load_place_file(STRPTR cand, STRPTR dest, UWORD expect_crc,
    STRPTR bak_path);
LONG load_rollback_dest(STRPTR dest, ULONG quiet);

/* load_resident.c -- flush resident libs/devices; reboot advice */
LONG load_dest_is_lh_library(STRPTR dest);
LONG load_try_flush_resource(STRPTR dest);
LONG load_release_lh_library(void);
LONG load_reopen_lh_library(void);
/*
 * Open lh.library (min version, then any).  If that fails, quietly try
 * SYS:T/Load ROLLBACK of LIBS:lh.library then open again -- recovers from
 * an aborted self-upgrade that left the file missing/broken.
 */
LONG load_open_lh_library(void);
void load_print_reboot_advice(ULONG quiet);
/* Silent bak restore used by load_open_lh_library (no error spam). */
LONG load_try_restore_lh_bak(void);

LONG load_run(struct LoadArgs *args);

/* load_wb.c -- Workbench / requester.class (EasyRequest fallback) */
struct LhArchive;
LONG load_wb_startup(struct WBStartup *wb);
LONG load_wb_confirm_install(struct LoadManifest *man, STRPTR to_override,
    struct LhArchive *arc);
void load_wb_error(STRPTR msg, LONG code);
void load_wb_warn(STRPTR msg);
/* WB done dialog: list dest paths packed as LOAD_SUMMARY_LEN rows. */
void load_wb_install_done(STRPTR added, LONG nadded,
    STRPTR updated, LONG nupdated, LONG need_reboot);
void load_wb_close_libs(void);
LONG load_wb_open_verbose_con(void);
void load_wb_close_verbose_con(void);

#endif /* LOAD_INTERNAL_H */
