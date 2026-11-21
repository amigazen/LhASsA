/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * handle.c - Handle: create a Bin-profile LHA from a CPU-rooted stage.
 *
 * LHA = Load / Handle / Archive.  Generates Manifest from $VER: + layout.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct Library *UtilityBase = NULL;

#include <proto/utility.h>
#include <libraries/lh.h>
#include <proto/lh.h>

#include "handle_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;

struct Library *LhBase = NULL;

static const char *verstag = "$VER: Handle 1.0 (15.07.2026)\n";
static const char *stack_cookie = "$STACK: 16384\n";
const long oslibversion = 37L;

#define HANDLE_NARGS 5

static const char *TEMPLATE =
    "FROM/A,"
    "TO/A,"
    "FORCE/S,"
    "QUIET/S,"
    "HELP/S";

static void handle_set_rc(LONG code)
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

static void handle_args_from_rd(struct HandleArgs *out, LONG *rd)
{
    out->from = (STRPTR)rd[0];
    out->to = (STRPTR)rd[1];
    out->force = (ULONG)rd[2];
    out->quiet = (ULONG)rd[3];
    out->help = (ULONG)rd[4];
    out->from_path[0] = '\0';
    out->to_path[0] = '\0';
}

static void handle_stabilize_args(struct HandleArgs *args)
{
    if (args->from && args->from[0]) {
        Strncpy((STRPTR)args->from_path, args->from, HANDLE_PATH_LEN);
        args->from = (STRPTR)args->from_path;
    } else {
        args->from = NULL;
    }
    if (args->to && args->to[0]) {
        Strncpy((STRPTR)args->to_path, args->to, HANDLE_PATH_LEN);
        args->to = (STRPTR)args->to_path;
    } else {
        args->to = NULL;
    }
}

static int handle_open_libs(void)
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
    struct HandleArgs args;
    LONG rdargs[HANDLE_NARGS];
    LONG rc;
    LONG i;

    (void)argc;
    (void)argv;
    (void)verstag;
    (void)stack_cookie;

    if (!handle_open_libs()) {
        if (DOSBase) {
            handle_print_error((STRPTR)"cannot open utility.library", 0);
        }
        return RETURN_FAIL;
    }

    for (i = 0; i < HANDLE_NARGS; i++) {
        rdargs[i] = 0;
    }

    rda = ReadArgs((STRPTR)TEMPLATE, rdargs, NULL);
    if (!rda) {
        LONG err;

        err = IoErr();
        if (err != 0) {
            PrintFault(err, (STRPTR)"Handle");
        } else {
            handle_print_usage();
        }
        handle_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    handle_args_from_rd(&args, rdargs);

    if (args.help) {
        handle_print_usage();
        FreeArgs(rda);
        handle_set_rc(RETURN_OK);
        return RETURN_OK;
    }

    if (!args.from || !args.from[0] || !args.to || !args.to[0]) {
        handle_print_error((STRPTR)"FROM and TO are required", 0);
        handle_print_usage();
        FreeArgs(rda);
        handle_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    handle_stabilize_args(&args);
    FreeArgs(rda);
    rda = NULL;

    LhBase = OpenLibrary((STRPTR)LH_NAME, (LONG)LH_MIN_VERSION);
    if (!LhBase) {
        handle_print_error((STRPTR)"cannot open lh.library", 0);
        handle_set_rc(RETURN_FAIL);
        return RETURN_FAIL;
    }

    rc = handle_run(&args);

    CloseLibrary(LhBase);
    LhBase = NULL;
    handle_set_rc(rc);
    return (int)rc;
}
