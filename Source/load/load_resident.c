/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * load_resident.c - Soft-flush resident system resources before place.
 *
 * Libraries / BOOPSI class libs: FindName(LibList) + RemLibrary (RemLib-style).
 * Devices: FindName(DeviceList) + RemDevice.
 * L: handlers / DOSDrivers: no remount dance -- advise reboot after install.
 *
 * Never force-unload.  Install still writes the new file; caller warns and
 * may return RETURN_WARN (5) when a reboot is needed to activate.
 *
 * CRITICAL: RemLibrary / RemDevice already Forbid internally and run Expunge
 * while forbidden.  Nesting our own Forbid() around them can unbalance
 * Exec's TD nest count if Expunge Permits -- that wedges the OS so no new
 * libraries can be loaded.  Find under Forbid, Permit, then Rem*.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <libraries/lh.h>

#include "load_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;
extern struct Library *LhBase;

#define LOAD_RES_NONE     0
#define LOAD_RES_LIBRARY  1
#define LOAD_RES_DEVICE   2
#define LOAD_RES_HANDLER  3

static LONG load_str_ends_with(STRPTR s, STRPTR suf)
{
    LONG sl;
    LONG su;
    LONG i;

    if (!s || !suf) {
        return 0;
    }
    sl = 0;
    while (s[sl] != '\0') {
        sl++;
    }
    su = 0;
    while (suf[su] != '\0') {
        su++;
    }
    if (su == 0 || sl < su) {
        return 0;
    }
    for (i = 0; i < su; i++) {
        if (ToUpper(s[sl - su + i]) != ToUpper(suf[i])) {
            return 0;
        }
    }
    return 1;
}

static LONG load_path_has_prefix(STRPTR path, STRPTR prefix)
{
    LONG i;

    if (!path || !prefix) {
        return 0;
    }
    for (i = 0; prefix[i] != '\0'; i++) {
        if (path[i] == '\0') {
            return 0;
        }
        if (ToUpper(path[i]) != ToUpper(prefix[i])) {
            return 0;
        }
    }
    return 1;
}

/*
 * Libraries we must never RemLibrary from Load -- Expunge would tear down
 * the session.  Advise reboot to activate instead.
 */
static LONG load_lib_is_critical(STRPTR leaf)
{
    if (!leaf || !leaf[0]) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"exec.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"dos.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"utility.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"intuition.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"graphics.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"layers.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"expansion.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"keymap.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"diskfont.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"icon.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"workbench.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"gadtools.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"asl.library") == 0) {
        return 1;
    }
    if (Stricmp(leaf, (STRPTR)"locale.library") == 0) {
        return 1;
    }
    return 0;
}

/*
 * Classify a destination for resident activation handling.
 */
static LONG load_resident_kind(STRPTR dest)
{
    STRPTR leaf;

    if (!dest || !dest[0]) {
        return LOAD_RES_NONE;
    }

    /* Handlers / mount files: reboot is the safe activation advice. */
    if (load_path_has_prefix(dest, (STRPTR)"L:")
        || load_path_has_prefix(dest, (STRPTR)"Devs:DOSDrivers")) {
        return LOAD_RES_HANDLER;
    }

    leaf = FilePart(dest);
    if (!leaf || !leaf[0]) {
        return LOAD_RES_NONE;
    }

    if (load_str_ends_with(leaf, (STRPTR)".device")) {
        return LOAD_RES_DEVICE;
    }

    /*
     * Shared libraries and BOOPSI class modules live on LibList under the
     * leaf name (foo.library, foo.gadget, foo.datatype, ...).
     */
    if (load_path_has_prefix(dest, (STRPTR)"LIBS:")
        || load_path_has_prefix(dest, (STRPTR)"SYS:Classes/")
        || load_path_has_prefix(dest, (STRPTR)"CLASSES:")
        || load_str_ends_with(leaf, (STRPTR)".library")
        || load_str_ends_with(leaf, (STRPTR)".gadget")
        || load_str_ends_with(leaf, (STRPTR)".image")
        || load_str_ends_with(leaf, (STRPTR)".class")
        || load_str_ends_with(leaf, (STRPTR)".datatype")) {
        return LOAD_RES_LIBRARY;
    }

    return LOAD_RES_NONE;
}

LONG load_dest_is_lh_library(STRPTR dest)
{
    STRPTR leaf;

    if (!dest) {
        return 0;
    }
    leaf = FilePart(dest);
    if (!leaf) {
        return 0;
    }
    return (Stricmp(leaf, (STRPTR)LH_NAME) == 0) ? 1 : 0;
}

/*
 * Soft-remove a library from Exec's LibList (RemLib pattern).
 * Returns 1 if reboot still needed (was present and still present).
 *
 * Find under Forbid, then Permit BEFORE RemLibrary -- RemLibrary Forbids
 * itself for Expunge.
 */
static LONG load_flush_library(STRPTR leaf)
{
    struct Library *found;
    LONG need_reboot;

    need_reboot = 0;
    if (!leaf || !leaf[0]) {
        return 0;
    }
    if (load_lib_is_critical(leaf)) {
        /* Still resident after place -- reboot to pick up the new file. */
        Forbid();
        found = (struct Library *)FindName(&SysBase->LibList, leaf);
        Permit();
        return found ? 1 : 0;
    }

    Forbid();
    found = (struct Library *)FindName(&SysBase->LibList, leaf);
    Permit();
    if (!found) {
        return 0;
    }

    RemLibrary(found);

    Forbid();
    found = (struct Library *)FindName(&SysBase->LibList, leaf);
    Permit();
    if (found) {
        need_reboot = 1;
    }
    return need_reboot;
}

static LONG load_flush_device(STRPTR leaf)
{
    struct Device *found;
    LONG need_reboot;

    need_reboot = 0;
    if (!leaf || !leaf[0]) {
        return 0;
    }

    /* Never RemDevice timer/input/keyboard/gameport/console mid-session. */
    if (Stricmp(leaf, (STRPTR)"timer.device") == 0
        || Stricmp(leaf, (STRPTR)"input.device") == 0
        || Stricmp(leaf, (STRPTR)"keyboard.device") == 0
        || Stricmp(leaf, (STRPTR)"gameport.device") == 0
        || Stricmp(leaf, (STRPTR)"console.device") == 0
        || Stricmp(leaf, (STRPTR)"ramdrive.device") == 0) {
        Forbid();
        found = (struct Device *)FindName(&SysBase->DeviceList, leaf);
        Permit();
        return found ? 1 : 0;
    }

    Forbid();
    found = (struct Device *)FindName(&SysBase->DeviceList, leaf);
    Permit();
    if (!found) {
        return 0;
    }

    RemDevice(found);

    Forbid();
    found = (struct Device *)FindName(&SysBase->DeviceList, leaf);
    Permit();
    if (found) {
        need_reboot = 1;
    }
    return need_reboot;
}

/*
 * Attempt to flush a resident resource so the newly placed file is used
 * on next open.  Returns 1 if a reboot is still recommended.
 */
LONG load_try_flush_resource(STRPTR dest)
{
    LONG kind;
    STRPTR leaf;

    kind = load_resident_kind(dest);
    if (kind == LOAD_RES_NONE) {
        return 0;
    }
    if (kind == LOAD_RES_HANDLER) {
        /* Mounted handlers: remount is fragile; advise reboot. */
        return 1;
    }

    leaf = FilePart(dest);
    if (!leaf || !leaf[0]) {
        return 0;
    }

    if (kind == LOAD_RES_DEVICE) {
        return load_flush_device(leaf);
    }
    return load_flush_library(leaf);
}

/*
 * Post-place flush for LIBS:lh.library: disk already has the new file.
 * Close our open, RemLibrary, caller reopens.  Returns 1 if the old
 * resident copy could not be cleared (reboot to activate).
 */
LONG load_release_lh_library(void)
{
    struct Library *found;
    LONG need_reboot;

    need_reboot = 0;
    if (LhBase) {
        CloseLibrary(LhBase);
        LhBase = NULL;
    }

    /*
     * Same rule as load_flush_library: never RemLibrary inside our Forbid.
     * Open count should be zero after CloseLibrary above so Expunge can run.
     */
    Forbid();
    found = (struct Library *)FindName(&SysBase->LibList, (STRPTR)LH_NAME);
    Permit();
    if (found) {
        RemLibrary(found);
    }

    Forbid();
    found = (struct Library *)FindName(&SysBase->LibList, (STRPTR)LH_NAME);
    Permit();
    if (found) {
        need_reboot = 1;
    }
    return need_reboot;
}

LONG load_reopen_lh_library(void)
{
    return load_open_lh_library();
}

/*
 * Open lh.library for Load.  Prefer LH_MIN_VERSION, then any version.
 * If OpenLibrary still fails, try restoring LIBS:lh.library from the
 * newest SYS:T/Load bak (aborted self-upgrade recovery) and open again.
 */
LONG load_open_lh_library(void)
{
    if (LhBase) {
        return 1;
    }

    LhBase = OpenLibrary((STRPTR)LH_NAME, (LONG)LH_MIN_VERSION);
    if (LhBase) {
        return 1;
    }

    /* Older or unversioned install may still be usable. */
    LhBase = OpenLibrary((STRPTR)LH_NAME, 0L);
    if (LhBase) {
        return 1;
    }

    /*
     * Disk file may be missing/broken after a crashed lh.library place.
     * Restore from T: bak if present, then retry open.
     */
    if (load_try_restore_lh_bak()) {
        LhBase = OpenLibrary((STRPTR)LH_NAME, (LONG)LH_MIN_VERSION);
        if (LhBase) {
            return 1;
        }
        LhBase = OpenLibrary((STRPTR)LH_NAME, 0L);
        if (LhBase) {
            return 1;
        }
    }
    return 0;
}

void load_print_reboot_advice(ULONG quiet)
{
    if (quiet) {
        return;
    }
    Printf("\n", 0);
    Printf("Load: Some system software was updated but is still in use.\n", 0);
    Printf("Load: Please reboot to activate the new version.\n", 0);
    Flush(Output());
}
