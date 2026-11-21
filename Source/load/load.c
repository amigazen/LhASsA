/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * load.c - Load: install or ROLLBACK a package Bin.
 *
 * LHA = Load / Handle / Archive.  $VER gates installs; bak under SYS:T/Load.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct Library *UtilityBase = NULL;

#include <proto/utility.h>
#include <libraries/lh.h>
#include <proto/lh.h>

#include "load_internal.h"
#include "/common/lhasa_cmdhelp.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

struct Library *LhBase = NULL;

static const char *verstag = "$VER: Load 1.4 (21.11.2026)\n";
/* 4K -- large/persistent structs and multi-path frames are heap-allocated. */
static const char *stack_cookie = "$STACK: 4096\n";
const long oslibversion = 37L;

#define LOAD_NARGS 8

static const char *TEMPLATE =
    "BIN,"
    "TO/K,"
    "DEST/K,"
    "FORCE/S,"
    "ROLLBACK/S,"
    "QUIET/S,"
    "VERBOSE/S,"
    "HELP/S";

static void load_set_rc(LONG code)
{
    char buf[16];
    LONG n;
    LONG v;
    LONG i;
    char tmp[16];

    v = code;
    if (v < 0) {
        v = -v;
    }
    n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0 && n < (LONG)sizeof(tmp) - 1) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    for (i = 0; i < n; i++) {
        buf[i] = tmp[n - 1 - i];
    }
    buf[n] = '\0';
    SetVar((STRPTR)"RC", (STRPTR)buf, -1, GVF_GLOBAL_ONLY);
}

static void load_args_from_rd(struct LoadArgs *out, LONG *rd)
{
    out->bin = (STRPTR)rd[0];
    out->to = (STRPTR)rd[1];
    out->dest = (STRPTR)rd[2];
    out->force = (ULONG)rd[3];
    out->rollback = (ULONG)rd[4];
    out->quiet = (ULONG)rd[5];
    out->verbose = (ULONG)rd[6];
    out->help = (ULONG)rd[7];
    out->interactive = 0;
    out->bin_path[0] = '\0';
    out->to_path[0] = '\0';
    out->dest_path[0] = '\0';
}

static void load_stabilize_args(struct LoadArgs *args)
{
    if (args->bin && args->bin[0]) {
        Strncpy((STRPTR)args->bin_path, args->bin, LOAD_PATH_LEN);
        args->bin = (STRPTR)args->bin_path;
    } else {
        args->bin = NULL;
    }
    if (args->to && args->to[0]) {
        Strncpy((STRPTR)args->to_path, args->to, LOAD_PATH_LEN);
        args->to = (STRPTR)args->to_path;
    } else {
        args->to = NULL;
    }
    if (args->dest && args->dest[0]) {
        Strncpy((STRPTR)args->dest_path, args->dest, LOAD_PATH_LEN);
        args->dest = (STRPTR)args->dest_path;
    } else {
        args->dest = NULL;
    }
}

static int load_open_libs(void)
{
    if (!DOSBase) {
        DOSBase = (struct DosLibrary *)OpenLibrary((STRPTR)"dos.library", 37L);
        if (!DOSBase) {
            return 0;
        }
    }
    if (!UtilityBase) {
        UtilityBase = OpenLibrary((STRPTR)"utility.library", 37L);
        if (!UtilityBase) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char *argv[])
{
    struct RDArgs *rda;
    struct LoadArgs *args;
    LONG *rdargs;
    LONG rc;
    LONG i;

    (void)verstag;
    (void)stack_cookie;

    args = NULL;
    rdargs = NULL;
    rda = NULL;
    rc = RETURN_FAIL;

    if (!load_open_libs()) {
        if (DOSBase) {
            load_print_error((STRPTR)"cannot open utility.library", 0);
        }
        return RETURN_FAIL;
    }

    /*
     * Only one Load at a time (named public MsgPort).  Claim before WB
     * or CLI work so a second double-click cannot wedge requesters.
     */
    if (!load_claim_instance()) {
        if (argc == 0) {
            load_wb_error(
                (STRPTR)
                "Load is already running.\n"
                "Only one instance is allowed.",
                0);
            load_wb_close_libs();
        } else {
            load_print_error(
                (STRPTR)
                "Load is already running (only one instance allowed)",
                0);
        }
        load_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    /* Workbench: argc == 0 and argv is WBStartup *. */
    if (argc == 0) {
        rc = load_wb_startup((struct WBStartup *)argv);
        load_release_instance();
        load_set_rc(rc);
        return (int)rc;
    }

    args = (struct LoadArgs *)load_alloc((ULONG)sizeof(struct LoadArgs));
    rdargs = (LONG *)load_alloc((ULONG)sizeof(LONG) * (ULONG)LOAD_NARGS);
    if (!args || !rdargs) {
        load_print_error((STRPTR)"out of memory", 0);
        load_free(args, (ULONG)sizeof(struct LoadArgs));
        load_free(rdargs, (ULONG)sizeof(LONG) * (ULONG)LOAD_NARGS);
        load_release_instance();
        load_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    for (i = 0; i < LOAD_NARGS; i++) {
        rdargs[i] = 0;
    }

    rda = ReadArgs((STRPTR)TEMPLATE, rdargs, NULL);
    if (!rda) {
        LONG err;

        err = IoErr();
        if (err != 0) {
            PrintFault(err, (STRPTR)"Load");
        } else {
            load_print_usage();
        }
        load_free(args, (ULONG)sizeof(struct LoadArgs));
        load_free(rdargs, (ULONG)sizeof(LONG) * (ULONG)LOAD_NARGS);
        load_release_instance();
        load_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    load_args_from_rd(args, rdargs);

    if (args->help) {
        /* Full .help file only -- never a short usage synopsis. */
        if (lhasa_print_cmd_help((STRPTR)"Load")) {
            FreeArgs(rda);
            load_free(args, (ULONG)sizeof(struct LoadArgs));
            load_free(rdargs, (ULONG)sizeof(LONG) * (ULONG)LOAD_NARGS);
            load_release_instance();
            load_set_rc(RETURN_OK);
            return RETURN_OK;
        }
        FreeArgs(rda);
        load_free(args, (ULONG)sizeof(struct LoadArgs));
        load_free(rdargs, (ULONG)sizeof(LONG) * (ULONG)LOAD_NARGS);
        load_release_instance();
        load_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    load_stabilize_args(args);
    FreeArgs(rda);
    rda = NULL;
    load_free(rdargs, (ULONG)sizeof(LONG) * (ULONG)LOAD_NARGS);
    rdargs = NULL;

    if (args->rollback) {
        if (!args->dest || !args->dest[0]) {
            load_print_error((STRPTR)"ROLLBACK requires DEST", 0);
            Printf("Load: type Load HELP for documentation.\n", 0);
            Flush(Output());
            load_free(args, (ULONG)sizeof(struct LoadArgs));
            load_release_instance();
            load_set_rc(RETURN_FAIL);
            return RETURN_FAIL;
        }
        rc = load_run(args);
        load_free(args, (ULONG)sizeof(struct LoadArgs));
        load_release_instance();
        load_set_rc(rc);
        return (int)rc;
    }

    if (!args->bin || !args->bin[0]) {
        load_print_error((STRPTR)"BIN is required", 0);
        Printf("Load: type Load HELP for documentation.\n", 0);
        Flush(Output());
        load_free(args, (ULONG)sizeof(struct LoadArgs));
        load_release_instance();
        load_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    if (!load_open_lh_library()) {
        load_print_error(
            (STRPTR)"cannot open lh.library (check LIBS:lh.library)", 0);
        load_free(args, (ULONG)sizeof(struct LoadArgs));
        load_release_instance();
        load_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    rc = load_run(args);

    /* May already be NULL after an in-place lh.library upgrade path. */
    if (LhBase) {
        CloseLibrary(LhBase);
        LhBase = NULL;
    }
    load_free(args, (ULONG)sizeof(struct LoadArgs));
    load_release_instance();
    load_set_rc(rc);
    return (int)rc;
}
