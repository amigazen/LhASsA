/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lha_internal.h - LhA CLI (ANSI C; Amiga hooks in lha_amiga.c).
 */

#ifndef LHA_INTERNAL_H
#define LHA_INTERNAL_H

#include "lha_platform.h"
#include "/include/lh.h"

#if !defined(HOST)
void lh_utility_shutdown(void);
#endif

typedef enum lha_cmd {
    LHA_CMD_NONE = 0,
    LHA_CMD_ADD,
    LHA_CMD_EXTRACT,
    LHA_CMD_EXTRACT_FLAT,
    LHA_CMD_LIST,
    LHA_CMD_VIEW,
    LHA_CMD_UPDATE,
    LHA_CMD_DELETE,
    LHA_CMD_MOVE,
    LHA_CMD_CONCAT,
    LHA_CMD_PRINT,
    LHA_CMD_TEST,
    LHA_CMD_FRESHEN,
    LHA_CMD_HUNT,
    LHA_CMD_REPLACE,
    LHA_CMD_COPY
} lha_cmd;

typedef struct lha_opts {
    lh_level level;
    int store_only;
    int header_level;
    int debug_verbose;
    int quiet;
    int quiet_level;
    int cmd_quiet;
    int view_full;
    int no_execute;
    int force;
    int text_mode;
    int delete_source;
    int ignore_paths;
    int preserve_paths;
    int generic_format;
    char workdir[512];
    /* LhA compatibility no-ops (cli.txt). */
    int noop_euc;
    int noop_no_progress;
    int ignore_filenotes;   /* -f */
    int noop_fast_progress;
    int noop_extract_new;
    int noop_extract_newer;
    int noop_arc_newest_date;
    int noop_alt_progress;
    int noop_filelist;
    int ignore_lhaopts;     /* -I */
    int noop_compress_archives;
    int no_homedirs;        /* -h */
    int noop_lower_names;
    int noop_upper_names;
    int noop_workdir_flag;
    int noop_exclude;
    int recurse;            /* -r collect files recursively */
    int recurse_archives;   /* -R collect archives recursively */
    int noop_add_no_arc;
    int noop_set_arc;
    int noop_no_autoshow;
    int noop_delete_autoshow;
    int empty_dirs;         /* -e with -r */
    int no_suffix;          /* -X */
    int always_suffix;      /* -y */
    /* Wired classic options (not no-ops). */
    int no_wildcards;   /* -Qw */
    int quick_add;      /* -Qq: skip duplicate-member scan on add */
} lha_opts;

/* One filesystem object to add (or an empty directory with -e). */
typedef struct lha_file_item {
    char *path;
    char *arcname;
    char *comment;
    int is_dir;
} lha_file_item;

typedef struct lha_args {
    lha_cmd cmd;
    char *archive;
    char **files;
    int file_count;
    char *destdir;
} lha_args;

extern lha_opts g_opts;

void lha_opts_init(lha_opts *o);
void lha_usage(const char *prog);
int lha_parse_args(int argc, char **argv, lha_args *out, char **unknown_opt);
void lha_args_free(lha_args *a);

char *lha_strdup(const char *s);
void lha_normalize_slashes(char *s);
char *lha_basename(const char *path);
int lha_stricmp(const char *a, const char *b);
int lha_path_is_wild(const char *pattern);
/* Expand filesystem patterns (MatchFirst on Amiga). Caller frees with lha_free_paths. */
char **lha_expand_paths(char **patterns, int count, int *out_count);
void lha_free_paths(char **paths, int count);
/*
 * Archive member name for add: basename by default; with -x, path minus
 * volume (VOL:foo/bar -> foo/bar).  Uses on-disk capitalisation when possible.
 */
char *lha_make_arcname(const char *path);
int lha_path_join(char *out, size_t outsz, const char *dir, const char *name);
int lha_mkpath(const char *path);
int lha_match_pattern(const char *pattern, const char *name);
int lha_name_matches_any(const char *name, char **patterns, int count);
char **lha_archive_member_names(const char *archive, int *out_count);
int lha_archive_has_member(const char *archive, const char *name);
int lha_read_file(const char *path, unsigned char **data, size_t *len, lh_datetime *dt);
int lha_write_file(const char *path, const unsigned char *data, size_t len);
#if defined(LHA_AMIGA)
int lha_write_file_amiga(const char *path, const unsigned char *data, size_t len);
int lha_mkpath_amiga(const char *path);
int lha_read_filenote(const char *path, char **out_comment);
int lha_set_filenote(const char *path, const char *comment);
int lha_disk_case_path_amiga(const char *path, char *out, size_t outsz);
int lha_match_pattern_amiga(const char *pattern, const char *name);
char **lha_expand_paths_amiga(char **patterns, int count, int *out_count);
#endif
void lha_apply_file_metadata(const char *path, const lh_datetime *dt, lh_attrs attrs);
void lha_apply_file_metadata_ex(const char *path, const lh_datetime *dt,
    lh_attrs attrs, const char *comment);

/* Append .lha/.lzh per -X/-y / compression mode. Caller frees. */
char *lha_resolve_archive_name(const char *name);
/* Collect ADD filespecs with home dirs / -r / -e. Caller frees via lha_free_file_items. */
int lha_collect_add_files(char **args, int argc, lha_file_item **out_items,
    int *out_count);
void lha_free_file_items(lha_file_item *items, int count);
/* Expand archive pattern with optional -R recursion. Caller frees paths. */
char **lha_expand_archives(const char *pattern, int *out_count);

void lha_add_freeze_begin(const char *name);
void lha_add_freeze_end(const char *name, int stored);
int lha_read_lhaopts(char *buf, size_t bufsz);

lh_status lha_rewrite_archive(
    const char *archive,
    int (*keep)(const lh_entry *entry, void *ctx),
    void *ctx
);

int lha_run_command(const lha_args *args);

const char *lha_archive_name_for_display(const char *path);
int lha_is_autoshow_name(const char *filename);
int lha_autoshow_enabled(void);
void lha_autoshow_display(const unsigned char *data, size_t len);
void lha_extract_banner(const char *archive);
void lha_test_banner(const char *archive);
void lha_extract_summary(unsigned long count, unsigned long bad);
void lha_test_summary(unsigned long count, unsigned long bad);
void lha_operation_success(void);
void lha_operation_failed(void);
void lha_add_message(const char *name);

void lha_progress_set_filter(const lha_args *args);
void lha_extract_progress_begin(void *ctx, const char *name, size_t size);
void lha_test_progress_begin(void *ctx, const char *name, size_t size);
void lha_progress_update(void *ctx, size_t done, size_t total);
void lha_progress_end(void *ctx, const char *name, int crc_ok,
    unsigned short wanted_crc, unsigned short computed_crc, size_t size);
void lha_test_progress_end(void *ctx, const char *name, int crc_ok,
    unsigned short wanted_crc, unsigned short computed_crc, size_t size);

#endif /* LHA_INTERNAL_H */
