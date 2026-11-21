/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * load_atomic.c - SYS:T/Load bak, CRC16 verify, temp place, ROLLBACK.
 *
 * Protocol C: copy live to T:, stage full candidate, place via dest-volume
 * temp + Delete + Rename, verify CRC16, restore bak on failure.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>

#include "load_internal.h"

extern struct ExecBase *SysBase;
extern struct DosLibrary *DOSBase;
extern struct Library *UtilityBase;

#define LOAD_COPY_BUF 4096

/* Public port name -- FindPort sees this; only one Load at a time. */
#define LOAD_INSTANCE_PORT "LhASsA_Load"

static struct MsgPort *load_instance_port = NULL;

LONG load_claim_instance(void)
{
    struct MsgPort *p;

    Forbid();
    if (FindPort((STRPTR)LOAD_INSTANCE_PORT) != NULL) {
        Permit();
        return 0;
    }
    p = CreateMsgPort();
    if (p == NULL) {
        Permit();
        return 0;
    }
    p->mp_Node.ln_Name = (char *)LOAD_INSTANCE_PORT;
    p->mp_Node.ln_Pri = 0;
    AddPort(p);
    Permit();
    load_instance_port = p;
    return 1;
}

void load_release_instance(void)
{
    if (load_instance_port == NULL) {
        return;
    }
    Forbid();
    RemPort(load_instance_port);
    Permit();
    DeleteMsgPort(load_instance_port);
    load_instance_port = NULL;
}

APTR load_alloc(ULONG size)
{
    if (size == 0) {
        return NULL;
    }
    return AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
}

void load_free(APTR p, ULONG size)
{
    if (p && size > 0) {
        FreeMem(p, size);
    }
}

STRPTR load_alloc_path(void)
{
    return (STRPTR)load_alloc(LOAD_PATH_LEN);
}

void load_free_path(STRPTR p)
{
    load_free(p, LOAD_PATH_LEN);
}

static const UWORD load_crc16_table[256] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};

UWORD load_crc16_buf(APTR data, LONG len)
{
    UWORD crc;
    UBYTE *p;
    LONG i;

    crc = 0;
    if (!data || len <= 0) {
        return 0;
    }
    p = (UBYTE *)data;
    for (i = 0; i < len; i++) {
        crc = (UWORD)((crc >> 8)
            ^ load_crc16_table[(crc ^ p[i]) & 0xFF]);
    }
    return crc;
}

LONG load_crc16_file(STRPTR path, UWORD *crc_out)
{
    BPTR fh;
    UBYTE *buf;
    LONG n;
    LONG i;
    UWORD crc;

    if (!path || !crc_out) {
        return 0;
    }
    *crc_out = 0;
    fh = Open(path, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        return 0;
    }
    buf = (UBYTE *)AllocMem(LOAD_COPY_BUF, MEMF_PUBLIC);
    if (!buf) {
        Close(fh);
        return 0;
    }
    crc = 0;
    for (;;) {
        n = Read(fh, buf, LOAD_COPY_BUF);
        if (n < 0) {
            FreeMem(buf, LOAD_COPY_BUF);
            Close(fh);
            return 0;
        }
        if (n == 0) {
            break;
        }
        for (i = 0; i < n; i++) {
            crc = (UWORD)((crc >> 8)
                ^ load_crc16_table[(crc ^ buf[i]) & 0xFF]);
        }
    }
    FreeMem(buf, LOAD_COPY_BUF);
    Close(fh);
    *crc_out = crc;
    return 1;
}

#define LOAD_FP_CHUNK 256

/*
 * Fingerprint one file: size + CRC16(head) + CRC16(tail).
 * Returns 1 on success, 0 on I/O error.
 */
static LONG load_file_fingerprint(STRPTR path, LONG *size_out,
    UWORD *head_crc, UWORD *tail_crc)
{
    BPTR fh;
    UBYTE *buf;
    LONG size;
    LONG n;
    LONG chunk;

    if (!path || !size_out || !head_crc || !tail_crc) {
        return 0;
    }
    *size_out = 0;
    *head_crc = 0;
    *tail_crc = 0;
    fh = Open(path, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        return 0;
    }
    /*
     * Seek to end then back to start: Seek returns the previous position,
     * so the second Seek yields the file size.
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
    *size_out = size;
    if (size == 0) {
        Close(fh);
        return 1;
    }
    buf = (UBYTE *)AllocMem(LOAD_FP_CHUNK, MEMF_PUBLIC);
    if (!buf) {
        Close(fh);
        return 0;
    }
    chunk = size;
    if (chunk > LOAD_FP_CHUNK) {
        chunk = LOAD_FP_CHUNK;
    }
    n = Read(fh, buf, chunk);
    if (n != chunk) {
        FreeMem(buf, LOAD_FP_CHUNK);
        Close(fh);
        return 0;
    }
    *head_crc = load_crc16_buf(buf, n);

    if (size <= LOAD_FP_CHUNK) {
        /* Whole file is the head; tail matches. */
        *tail_crc = *head_crc;
    } else {
        if (Seek(fh, size - LOAD_FP_CHUNK, OFFSET_BEGINNING) < 0) {
            FreeMem(buf, LOAD_FP_CHUNK);
            Close(fh);
            return 0;
        }
        n = Read(fh, buf, LOAD_FP_CHUNK);
        if (n != LOAD_FP_CHUNK) {
            FreeMem(buf, LOAD_FP_CHUNK);
            Close(fh);
            return 0;
        }
        *tail_crc = load_crc16_buf(buf, n);
    }
    FreeMem(buf, LOAD_FP_CHUNK);
    Close(fh);
    return 1;
}

LONG load_files_same_content(STRPTR a, STRPTR b)
{
    LONG sa;
    LONG sb;
    UWORD ah;
    UWORD at;
    UWORD bh;
    UWORD bt;

    if (!a || !b) {
        return 0;
    }
    if (!load_file_fingerprint(a, &sa, &ah, &at)) {
        return 0;
    }
    if (!load_file_fingerprint(b, &sb, &bh, &bt)) {
        return 0;
    }
    if (sa != sb) {
        return 0;
    }
    if (ah != bh || at != bt) {
        return 0;
    }
    return 1;
}

static LONG load_path_exists(STRPTR path)
{
    BPTR lock;

    lock = Lock(path, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    return 0;
}

static void load_u32_to_digits(ULONG v, char *dig8)
{
    LONG i;

    for (i = 7; i >= 0; i--) {
        dig8[i] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    dig8[8] = '\0';
}

LONG load_ensure_tdir(void)
{
    BPTR lock;
    BPTR parent;

    lock = Lock((STRPTR)LOAD_TDIR, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    parent = Lock((STRPTR)"SYS:T", ACCESS_READ);
    if (!parent) {
        parent = CreateDir((STRPTR)"SYS:T");
        if (!parent) {
            return 0;
        }
    }
    UnLock(parent);
    lock = CreateDir((STRPTR)LOAD_TDIR);
    if (!lock) {
        lock = Lock((STRPTR)LOAD_TDIR, ACCESS_READ);
        if (!lock) {
            return 0;
        }
    }
    UnLock(lock);
    return 1;
}

LONG load_ensure_parent(STRPTR path)
{
    char buf[LOAD_PATH_LEN];
    STRPTR fp;
    LONG plen;
    BPTR lock;

    if (!path || !path[0]) {
        return 0;
    }
    Strncpy((STRPTR)buf, path, LOAD_PATH_LEN);
    fp = FilePart((STRPTR)buf);
    if (!fp || fp == (STRPTR)buf) {
        return 1;
    }
    if (fp > (STRPTR)buf && fp[-1] == '/') {
        fp[-1] = '\0';
    } else if (fp > (STRPTR)buf && fp[-1] == ':') {
        return 1;
    } else {
        *fp = '\0';
    }
    plen = 0;
    while (buf[plen] != '\0') {
        plen++;
    }
    if (plen == 0 || buf[plen - 1] == ':') {
        return 1;
    }
    lock = Lock((STRPTR)buf, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    lock = CreateDir((STRPTR)buf);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    lock = Lock((STRPTR)buf, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    return 0;
}

LONG load_copy_file(STRPTR src, STRPTR dst)
{
    BPTR in;
    BPTR out;
    UBYTE *buf;
    LONG n;
    LONG w;

    if (!src || !dst) {
        return 0;
    }
    in = Open(src, MODE_OLDFILE);
    if (in == (BPTR)NULL) {
        return 0;
    }
    out = Open(dst, MODE_NEWFILE);
    if (out == (BPTR)NULL) {
        Close(in);
        return 0;
    }
    buf = (UBYTE *)AllocMem(LOAD_COPY_BUF, MEMF_PUBLIC);
    if (!buf) {
        Close(out);
        Close(in);
        DeleteFile(dst);
        return 0;
    }
    for (;;) {
        n = Read(in, buf, LOAD_COPY_BUF);
        if (n < 0) {
            FreeMem(buf, LOAD_COPY_BUF);
            Close(out);
            Close(in);
            DeleteFile(dst);
            return 0;
        }
        if (n == 0) {
            break;
        }
        w = Write(out, buf, n);
        if (w != n) {
            FreeMem(buf, LOAD_COPY_BUF);
            Close(out);
            Close(in);
            DeleteFile(dst);
            return 0;
        }
    }
    FreeMem(buf, LOAD_COPY_BUF);
    Close(out);
    Close(in);
    return 1;
}

LONG load_write_file(STRPTR path, APTR data, LONG len)
{
    BPTR fh;
    LONG w;

    if (!path) {
        return 0;
    }
    fh = Open(path, MODE_NEWFILE);
    if (fh == (BPTR)NULL) {
        return 0;
    }
    if (len > 0 && data) {
        w = Write(fh, data, len);
        if (w != len) {
            Close(fh);
            DeleteFile(path);
            return 0;
        }
    }
    Close(fh);
    return 1;
}

LONG load_clear_write_protect(STRPTR path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG prot;

    lock = Lock(path, ACCESS_READ);
    if (!lock) {
        return 1;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        UnLock(lock);
        return 0;
    }
    if (!Examine(lock, fib)) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        UnLock(lock);
        return 0;
    }
    prot = fib->fib_Protection;
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    UnLock(lock);
    if ((prot & FIBF_WRITE) == 0) {
        return 1;
    }
    return SetProtection(path, prot & ~FIBF_WRITE) ? 1 : 0;
}

/*
 * Return 1 if path has the AmigaDOS write-protect bit set (FIBF_WRITE).
 * Missing path => 0 (not protected).
 */
static LONG load_is_write_protected(STRPTR path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG prot;
    LONG wp;

    lock = Lock(path, ACCESS_READ);
    if (!lock) {
        return 0;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        UnLock(lock);
        return 1;
    }
    wp = 1;
    if (Examine(lock, fib)) {
        prot = fib->fib_Protection;
        wp = ((prot & FIBF_WRITE) != 0) ? 1 : 0;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    UnLock(lock);
    return wp;
}

/*
 * Exclusive ACCESS_WRITE lock probe.  Fails when another task has the
 * object open/locked (typical for in-use .library / handlers).
 */
static LONG load_can_exclusive(STRPTR path)
{
    BPTR lock;

    lock = Lock(path, ACCESS_WRITE);
    if (!lock) {
        return 0;
    }
    UnLock(lock);
    return 1;
}

/*
 * Fill out with the deepest existing ancestor of path (drawer or volume).
 */
static LONG load_existing_ancestor(STRPTR path, STRPTR out, LONG outlen)
{
    STRPTR fp;
    LONG plen;

    if (!path || !out || outlen <= 0) {
        return 0;
    }
    Strncpy(out, path, outlen);
    for (;;) {
        if (load_path_exists(out)) {
            return 1;
        }
        plen = 0;
        while (out[plen] != '\0') {
            plen++;
        }
        if (plen == 0) {
            return 0;
        }
        if (out[plen - 1] == ':') {
            /* Volume/assign root must exist to install into. */
            return 0;
        }
        fp = FilePart(out);
        if (!fp || fp == out) {
            return 0;
        }
        if (fp > out && fp[-1] == '/') {
            fp[-1] = '\0';
        } else if (fp > out && fp[-1] == ':') {
            *fp = '\0';
        } else {
            *fp = '\0';
        }
        plen = 0;
        while (out[plen] != '\0') {
            plen++;
        }
        if (plen == 0) {
            return 0;
        }
    }
}

/*
 * Log one preflight detail line.  Never opens a WB requester here -- a
 * modal requester per file after Lock probes on LIBS:/C: was a good way
 * to wedge Intuition.  One summary requester is raised at the end.
 */
static void load_preflight_note(STRPTR msg, STRPTR path)
{
    if (msg) {
        Printf("Load: %s\n", (LONG)msg);
    }
    if (path && path[0]) {
        Printf("Load:   %s\n", (LONG)path);
    }
    Flush(Output());
}

/*
 * Preflight selected Manifest destinations before any bak/place.
 * Refuses write-protected or locked overwrite targets, and unwritable
 * parent drawers / missing assigns.
 */
LONG load_preflight(struct LoadManifest *man, STRPTR to_override)
{
    LONG i;
    STRPTR dest;
    STRPTR ancestor;
    LONG errors;

    if (!man) {
        return 0;
    }

    dest = load_alloc_path();
    ancestor = load_alloc_path();
    if (!dest || !ancestor) {
        load_free_path(dest);
        load_free_path(ancestor);
        load_print_error((STRPTR)"preflight: out of memory", 0);
        return 0;
    }

    if (!load_ensure_tdir()) {
        load_print_error((STRPTR)"preflight: cannot create SYS:T/Load", IoErr());
        load_free_path(dest);
        load_free_path(ancestor);
        return 0;
    }
    if (load_is_write_protected((STRPTR)LOAD_TDIR)) {
        load_print_error((STRPTR)"preflight: SYS:T/Load is write-protected", 0);
        load_free_path(dest);
        load_free_path(ancestor);
        return 0;
    }
    if (!load_can_exclusive((STRPTR)LOAD_TDIR)) {
        load_print_error((STRPTR)"preflight: SYS:T/Load is locked/in use", 0);
        load_free_path(dest);
        load_free_path(ancestor);
        return 0;
    }

    errors = 0;
    for (i = 0; i < man->nfiles; i++) {
        if (!man->files[i].selected) {
            continue;
        }
        if (!load_resolve_dest(dest, LOAD_PATH_LEN, man->files[i].dest,
                to_override, man->install_root)) {
            load_preflight_note(
                (STRPTR)"preflight: cannot resolve destination",
                man->files[i].dest);
            errors++;
            continue;
        }
        if (load_path_is_volume_root(dest)) {
            load_preflight_note(
                (STRPTR)"preflight: refusing volume root", dest);
            errors++;
            continue;
        }

        if (load_path_exists(dest)) {
            if (!load_can_exclusive(dest)) {
                load_preflight_note(
                    (STRPTR)"preflight: destination is locked or in use",
                    dest);
                Printf(
                    "Load:   close the program using it, or reboot, then retry\n",
                    0);
                Flush(Output());
                errors++;
                continue;
            }
            /* Ensure overwrite target is writable before bak/place. */
            if (load_is_write_protected(dest)) {
                if (!load_clear_write_protect(dest)) {
                    load_preflight_note(
                        (STRPTR)"preflight: cannot clear write-protect",
                        dest);
                    errors++;
                }
            }
        } else {
            if (!load_existing_ancestor(dest, ancestor, LOAD_PATH_LEN)) {
                load_preflight_note(
                    (STRPTR)"preflight: destination assign/path missing",
                    dest);
                errors++;
                continue;
            }
            /*
             * Do NOT require ACCESS_WRITE on the parent drawer.  LIBS:, C:,
             * LOCALE:, etc. almost always have shared locks (OpenLibrary,
             * shells, WB) so Lock(..., ACCESS_WRITE) fails even when a new
             * file can still be created there -- and on some handlers that
             * probe can stall.  Exclusive lock stays on the overwrite
             * target above; here only writability matters.
             */
            if (load_is_write_protected(ancestor)) {
                if (!load_clear_write_protect(ancestor)) {
                    load_preflight_note(
                        (STRPTR)"preflight: parent drawer write-protected",
                        ancestor);
                    Printf("Load:   (for %s)\n", (LONG)dest);
                    Flush(Output());
                    errors++;
                }
            }
        }
    }

    load_free_path(dest);
    load_free_path(ancestor);

    if (errors > 0) {
        Printf("Load: preflight failed (%ld problem(s)); nothing installed\n",
            errors);
        Flush(Output());
        /* One WB requester for the whole preflight, not one per file. */
        load_print_error(
            (STRPTR)
            "Preflight failed; nothing installed.\n"
            "See the log for which paths are locked or missing.",
            0);
        return 0;
    }
    return 1;
}

LONG load_bak_path(STRPTR out, LONG outlen, STRPTR token)
{
    char leaf[16];
    LONG i;

    if (!out || outlen < 20 || !token) {
        return 0;
    }
    leaf[0] = 'L';
    leaf[1] = 'H';
    leaf[2] = 'b';
    for (i = 0; token[i] != '\0' && i < 8; i++) {
        leaf[3 + i] = token[i];
    }
    leaf[3 + i] = '\0';
    Strncpy(out, (STRPTR)LOAD_TDIR, outlen);
    if (!AddPart(out, (STRPTR)leaf, (ULONG)outlen)) {
        return 0;
    }
    return 1;
}

LONG load_sidecar_path(STRPTR out, LONG outlen, STRPTR token)
{
    char leaf[16];
    LONG i;

    if (!out || outlen < 20 || !token) {
        return 0;
    }
    leaf[0] = 'L';
    leaf[1] = 'H';
    leaf[2] = 'k';
    for (i = 0; token[i] != '\0' && i < 8; i++) {
        leaf[3 + i] = token[i];
    }
    leaf[3 + i] = '\0';
    Strncpy(out, (STRPTR)LOAD_TDIR, outlen);
    if (!AddPart(out, (STRPTR)leaf, (ULONG)outlen)) {
        return 0;
    }
    return 1;
}

LONG load_alloc_token(STRPTR token, LONG tokenlen)
{
    struct DateStamp ds;
    ULONG base;
    ULONG seq;
    STRPTR bak;
    STRPTR side;
    STRPTR cand;
    char dig[9];
    char leaf[16];
    LONG i;
    LONG ok;

    if (!token || tokenlen < 12) {
        return 0;
    }
    if (!load_ensure_tdir()) {
        return 0;
    }
    bak = load_alloc_path();
    side = load_alloc_path();
    cand = load_alloc_path();
    if (!bak || !side || !cand) {
        load_free_path(bak);
        load_free_path(side);
        load_free_path(cand);
        return 0;
    }
    ok = 0;
    DateStamp(&ds);
    base = (ULONG)((ds.ds_Days % 10000) * 100L + (ds.ds_Minute % 100));
    for (seq = 0; seq < 100; seq++) {
        load_u32_to_digits(base * 100UL + seq, dig);
        Strncpy(token, (STRPTR)dig, tokenlen);
        if (!load_bak_path(bak, LOAD_PATH_LEN, token)) {
            break;
        }
        if (!load_sidecar_path(side, LOAD_PATH_LEN, token)) {
            break;
        }
        /* Also require free LHc######## so cand/bak/side can share token. */
        leaf[0] = 'L';
        leaf[1] = 'H';
        leaf[2] = 'c';
        for (i = 0; dig[i] != '\0' && i < 8; i++) {
            leaf[3 + i] = dig[i];
        }
        leaf[3 + i] = '\0';
        Strncpy(cand, (STRPTR)LOAD_TDIR, LOAD_PATH_LEN);
        if (!AddPart(cand, (STRPTR)leaf, LOAD_PATH_LEN)) {
            break;
        }
        if (!load_path_exists(bak)
            && !load_path_exists(side)
            && !load_path_exists(cand)) {
            ok = 1;
            break;
        }
    }
    load_free_path(bak);
    load_free_path(side);
    load_free_path(cand);
    return ok;
}


static void load_str_cat(STRPTR dst, LONG dstlen, STRPTR src)
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

static void load_append_ulong(STRPTR dst, LONG dstlen, ULONG v)
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

LONG load_write_sidecar(STRPTR token, STRPTR dest, STRPTR bak,
    const struct LoadVerInfo *ver, UWORD crc)
{
    char side[LOAD_PATH_LEN];
    char text[512];
    LONG L;
    LONG ok;
    char hex[5];
    static const char *h = "0123456789ABCDEF";

    if (!load_sidecar_path((STRPTR)side, LOAD_PATH_LEN, token)) {
        return 0;
    }
    text[0] = '\0';
    Strncpy((STRPTR)text, (STRPTR)"Dest: ", 512);
    load_str_cat((STRPTR)text, 512, dest);
    load_str_cat((STRPTR)text, 512, (STRPTR)"\nBak: ");
    load_str_cat((STRPTR)text, 512, bak);
    load_str_cat((STRPTR)text, 512, (STRPTR)"\nToken: ");
    load_str_cat((STRPTR)text, 512, token);
    load_str_cat((STRPTR)text, 512, (STRPTR)"\nVer: ");
    if (ver && ver->have_ver) {
        load_append_ulong((STRPTR)text, 512, ver->major);
        load_str_cat((STRPTR)text, 512, (STRPTR)".");
        load_append_ulong((STRPTR)text, 512, ver->minor);
    }
    load_str_cat((STRPTR)text, 512, (STRPTR)"\nCRC: ");
    hex[0] = h[(crc >> 12) & 0xF];
    hex[1] = h[(crc >> 8) & 0xF];
    hex[2] = h[(crc >> 4) & 0xF];
    hex[3] = h[crc & 0xF];
    hex[4] = '\0';
    load_str_cat((STRPTR)text, 512, (STRPTR)hex);
    load_str_cat((STRPTR)text, 512, (STRPTR)"\n");
    L = 0;
    while (text[L] != '\0') {
        L++;
    }
    ok = load_write_file((STRPTR)side, (STRPTR)text, L);
    return ok;
}

/*
 * Copy live dest to SYS:T/Load as LHb######## + LHk######## sidecar.
 * If token_out already holds a token (non-empty), reuse it so the
 * candidate (LHc) / bak / sidecar share one id.  Otherwise allocate.
 * On any failure, removes partial bak/sidecar and leaves dest untouched.
 */
LONG load_backup_to_t(STRPTR dest, STRPTR token_out, LONG tokenlen,
    STRPTR bak_out, LONG baklen)
{
    char token[LOAD_TOKEN_LEN];
    char side[LOAD_PATH_LEN];
    struct LoadVerInfo vi;
    UWORD crc;
    LONG ok;

    if (!dest || !token_out || !bak_out) {
        return 0;
    }
    ok = 0;
    token[0] = '\0';
    if (token_out[0] != '\0') {
        Strncpy((STRPTR)token, token_out, LOAD_TOKEN_LEN);
    } else {
        if (!load_alloc_token((STRPTR)token, LOAD_TOKEN_LEN)) {
            goto out;
        }
    }
    if (!load_bak_path(bak_out, baklen, (STRPTR)token)) {
        goto out;
    }
    if (!load_sidecar_path((STRPTR)side, LOAD_PATH_LEN, (STRPTR)token)) {
        goto out;
    }
    if (load_path_exists(bak_out) || load_path_exists((STRPTR)side)) {
        goto out;
    }
    if (!load_clear_write_protect(dest)) {
        goto out;
    }
    if (!load_copy_file(dest, bak_out)) {
        DeleteFile(bak_out);
        goto out;
    }
    if (!load_crc16_file(bak_out, &crc)) {
        DeleteFile(bak_out);
        goto out;
    }
    load_ver_scan_file(dest, &vi);
    if (!load_write_sidecar((STRPTR)token, dest, bak_out, &vi, crc)) {
        DeleteFile(bak_out);
        DeleteFile((STRPTR)side);
        goto out;
    }
    Strncpy(token_out, (STRPTR)token, tokenlen);
    ok = 1;
out:
    return ok;
}

/*
 * Place candidate at dest.  bak_path may be NULL if no prior file.
 * On failure after dest deleted, restore from bak_path when present.
 */
/*
 * Restore dest from bak_path.  Returns 1 on success.  Dest may be missing.
 */
static LONG load_restore_from_bak(STRPTR dest, STRPTR bak_path)
{
    if (!dest || !dest[0] || !bak_path || !bak_path[0]) {
        return 0;
    }
    if (!load_path_exists(bak_path)) {
        return 0;
    }
    if (load_path_exists(dest)) {
        if (!load_clear_write_protect(dest)) {
            return 0;
        }
        if (!DeleteFile(dest)) {
            return 0;
        }
    }
    if (!load_copy_file(bak_path, dest)) {
        return 0;
    }
    return 1;
}

LONG load_place_file(STRPTR cand, STRPTR dest, UWORD expect_crc,
    STRPTR bak_path)
{
    char temp[LOAD_PATH_LEN];
    char leaf[16];
    STRPTR fp;
    UWORD crc;
    LONG had_dest;
    ULONG v;
    LONG j;

    if (!cand || !dest) {
        return 0;
    }
    if (!load_ensure_parent(dest)) {
        load_print_error((STRPTR)"cannot create destination directory", IoErr());
        return 0;
    }
    if (!load_crc16_file(cand, &crc) || crc != expect_crc) {
        load_print_error((STRPTR)"candidate CRC mismatch", 0);
        return 0;
    }

    /* Temp sibling on dest volume: same dir, leaf LHt + 6 digits of crc. */
    Strncpy((STRPTR)temp, dest, LOAD_PATH_LEN);
    fp = FilePart((STRPTR)temp);
    if (!fp) {
        return 0;
    }
    leaf[0] = 'L';
    leaf[1] = 'H';
    leaf[2] = 't';
    v = (ULONG)expect_crc;
    for (j = 5; j >= 0; j--) {
        leaf[3 + j] = (char)('0' + (v % 10UL));
        v /= 10UL;
    }
    leaf[9] = '\0';
    Strncpy(fp, (STRPTR)leaf, 16);

    if (load_path_exists((STRPTR)temp)) {
        DeleteFile((STRPTR)temp);
    }
    if (!load_copy_file(cand, (STRPTR)temp)) {
        load_print_error((STRPTR)"cannot stage temp on destination volume", IoErr());
        return 0;
    }
    if (!load_crc16_file((STRPTR)temp, &crc) || crc != expect_crc) {
        DeleteFile((STRPTR)temp);
        load_print_error((STRPTR)"temp CRC mismatch", 0);
        return 0;
    }

    had_dest = load_path_exists(dest);
    if (had_dest) {
        if (!load_clear_write_protect(dest)) {
            DeleteFile((STRPTR)temp);
            return 0;
        }
        if (!DeleteFile(dest)) {
            DeleteFile((STRPTR)temp);
            load_print_error((STRPTR)"cannot remove old destination", IoErr());
            return 0;
        }
    }

    if (!Rename((STRPTR)temp, dest)) {
        load_print_error((STRPTR)"cannot rename temp into place", IoErr());
        DeleteFile((STRPTR)temp);
        if (had_dest) {
            if (!load_restore_from_bak(dest, bak_path)) {
                load_print_error(
                    (STRPTR)"CRITICAL: dest removed; bak restore failed",
                    0);
            }
        }
        return 0;
    }

    if (!load_crc16_file(dest, &crc) || crc != expect_crc) {
        load_print_error((STRPTR)"installed CRC mismatch; restoring bak", 0);
        if (had_dest) {
            if (!load_restore_from_bak(dest, bak_path)) {
                load_print_error(
                    (STRPTR)"CRITICAL: bad place; bak restore failed", 0);
            }
        } else {
            load_clear_write_protect(dest);
            DeleteFile(dest);
        }
        return 0;
    }
    return 1;
}

static LONG load_sidecar_get_dest(STRPTR text, STRPTR dest_out, LONG destlen,
    STRPTR bak_out, LONG baklen)
{
    STRPTR p;
    STRPTR line;
    char buf[512];
    LONG ok;

    dest_out[0] = '\0';
    bak_out[0] = '\0';
    if (!text) {
        return 0;
    }
    Strncpy((STRPTR)buf, text, 512);
    p = buf;
    while (*p) {
        line = p;
        while (*p && *p != '\n' && *p != '\r') {
            p++;
        }
        if (*p) {
            *p++ = '\0';
            while (*p == '\n' || *p == '\r') {
                p++;
            }
        }
        if (Strnicmp(line, (STRPTR)"Dest: ", 6) == 0) {
            Strncpy(dest_out, line + 6, destlen);
        } else if (Strnicmp(line, (STRPTR)"Bak: ", 5) == 0) {
            Strncpy(bak_out, line + 5, baklen);
        }
    }
    ok = (dest_out[0] && bak_out[0]) ? 1 : 0;
    return ok;
}

/*
 * Quietly restore LIBS:lh.library from the newest matching SYS:T/Load bak.
 * Returns 1 if a bak was found and copied; 0 if none / copy failed.
 * Does not print -- used when OpenLibrary fails at Load startup.
 */
LONG load_try_restore_lh_bak(void)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    STRPTR path;
    STRPTR best_bak;
    STRPTR dest_got;
    STRPTR bak_got;
    APTR data;
    LONG len;
    BPTR fh;
    LONG got;
    LONG found;
    LONG rc;
    char best_token[LOAD_TOKEN_LEN];
    char leaf[16];
    static char lhdest[] = "LIBS:lh.library";

    /*
     * path/best_bak/dest_got/bak_got are LOAD_PATH_LEN (512) each -- must
     * not live on the stack.  best_token (16) and leaf (16) are fine there.
     */
    path     = load_alloc_path();
    best_bak = load_alloc_path();
    dest_got = load_alloc_path();
    bak_got  = load_alloc_path();
    if (!path || !best_bak || !dest_got || !bak_got) {
        load_free_path(path);
        load_free_path(best_bak);
        load_free_path(dest_got);
        load_free_path(bak_got);
        return 0;
    }

    best_bak[0]   = '\0';
    best_token[0] = '\0';
    found         = 0;
    rc            = 0;
    lock          = (BPTR)NULL;
    fib           = NULL;

    if (!load_ensure_tdir()) {
        goto out;
    }
    lock = Lock((STRPTR)LOAD_TDIR, ACCESS_READ);
    if (!lock) {
        goto out;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        goto out;
    }
    if (!Examine(lock, fib)) {
        goto out;
    }
    while (ExNext(lock, fib)) {
        if (fib->fib_DirEntryType > 0) {
            continue;
        }
        if (!(fib->fib_FileName[0] == 'L' && fib->fib_FileName[1] == 'H'
            && fib->fib_FileName[2] == 'k')) {
            continue;
        }
        Strncpy(path, (STRPTR)LOAD_TDIR, LOAD_PATH_LEN);
        if (!AddPart(path, fib->fib_FileName, LOAD_PATH_LEN)) {
            continue;
        }
        fh = Open(path, MODE_OLDFILE);
        if (fh == (BPTR)NULL) {
            continue;
        }
        if (Seek(fh, 0L, OFFSET_END) < 0) {
            Close(fh);
            continue;
        }
        len = Seek(fh, 0L, OFFSET_BEGINNING);
        if (len <= 0 || len > 4096) {
            Close(fh);
            continue;
        }
        data = AllocMem((ULONG)len + 1, MEMF_PUBLIC | MEMF_CLEAR);
        if (!data) {
            Close(fh);
            continue;
        }
        got = Read(fh, data, len);
        Close(fh);
        if (got != len) {
            FreeMem(data, (ULONG)len + 1);
            continue;
        }
        ((char *)data)[len] = '\0';
        if (!load_sidecar_get_dest((STRPTR)data, dest_got,
                LOAD_PATH_LEN, bak_got, LOAD_PATH_LEN)) {
            FreeMem(data, (ULONG)len + 1);
            continue;
        }
        FreeMem(data, (ULONG)len + 1);
        if (Stricmp(dest_got, (STRPTR)lhdest) != 0) {
            continue;
        }
        Strncpy((STRPTR)leaf, fib->fib_FileName + 3, 16);
        if (!found || Stricmp((STRPTR)leaf, (STRPTR)best_token) > 0) {
            Strncpy((STRPTR)best_token, (STRPTR)leaf, LOAD_TOKEN_LEN);
            Strncpy(best_bak, bak_got, LOAD_PATH_LEN);
            found = 1;
        }
    }

    if (!found || !best_bak[0] || !load_path_exists(best_bak)) {
        goto out;
    }
    if (!load_ensure_parent((STRPTR)lhdest)) {
        goto out;
    }
    if (load_path_exists((STRPTR)lhdest)) {
        load_clear_write_protect((STRPTR)lhdest);
        DeleteFile((STRPTR)lhdest);
    }
    if (!load_copy_file(best_bak, (STRPTR)lhdest)) {
        goto out;
    }
    rc = 1;

out:
    if (fib) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    }
    if (lock) {
        UnLock(lock);
    }
    load_free_path(path);
    load_free_path(best_bak);
    load_free_path(dest_got);
    load_free_path(bak_got);
    return rc;
}

LONG load_rollback_dest(STRPTR dest, ULONG quiet)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    STRPTR path;
    STRPTR best_side;
    STRPTR best_bak;
    STRPTR dest_got;
    STRPTR bak_got;
    APTR data;
    LONG len;
    BPTR fh;
    LONG got;
    LONG found;
    LONG rc;
    char best_token[LOAD_TOKEN_LEN];
    char leaf[16];

    /*
     * path/best_side/best_bak/dest_got/bak_got are LOAD_PATH_LEN (512) each
     * and must not live on the stack.  best_token (16) and leaf (16) are fine.
     */
    path      = load_alloc_path();
    best_side = load_alloc_path();
    best_bak  = load_alloc_path();
    dest_got  = load_alloc_path();
    bak_got   = load_alloc_path();
    if (!path || !best_side || !best_bak || !dest_got || !bak_got) {
        load_free_path(path);
        load_free_path(best_side);
        load_free_path(best_bak);
        load_free_path(dest_got);
        load_free_path(bak_got);
        return RETURN_FAIL;
    }

    best_side[0]  = '\0';
    best_bak[0]   = '\0';
    best_token[0] = '\0';
    found         = 0;
    rc            = RETURN_FAIL;
    lock          = (BPTR)NULL;
    fib           = NULL;

    if (!dest || !dest[0]) {
        goto out;
    }
    if (!load_ensure_tdir()) {
        load_print_error((STRPTR)"cannot access SYS:T/Load", IoErr());
        goto out;
    }

    lock = Lock((STRPTR)LOAD_TDIR, ACCESS_READ);
    if (!lock) {
        load_print_error((STRPTR)"no rollback directory", IoErr());
        goto out;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (!fib) {
        goto out;
    }
    if (!Examine(lock, fib)) {
        goto out;
    }
    while (ExNext(lock, fib)) {
        if (fib->fib_DirEntryType > 0) {
            continue;
        }
        /* Sidecar leaves: LHk######## */
        if (!(fib->fib_FileName[0] == 'L' && fib->fib_FileName[1] == 'H'
            && fib->fib_FileName[2] == 'k')) {
            continue;
        }
        Strncpy(path, (STRPTR)LOAD_TDIR, LOAD_PATH_LEN);
        if (!AddPart(path, fib->fib_FileName, LOAD_PATH_LEN)) {
            continue;
        }
        fh = Open(path, MODE_OLDFILE);
        if (fh == (BPTR)NULL) {
            continue;
        }
        if (Seek(fh, 0L, OFFSET_END) < 0) {
            Close(fh);
            continue;
        }
        len = Seek(fh, 0L, OFFSET_BEGINNING);
        if (len <= 0 || len > 4096) {
            Close(fh);
            continue;
        }
        data = AllocMem((ULONG)len + 1, MEMF_PUBLIC | MEMF_CLEAR);
        if (!data) {
            Close(fh);
            continue;
        }
        got = Read(fh, data, len);
        Close(fh);
        if (got != len) {
            FreeMem(data, (ULONG)len + 1);
            continue;
        }
        ((char *)data)[len] = '\0';
        if (!load_sidecar_get_dest((STRPTR)data, dest_got,
                LOAD_PATH_LEN, bak_got, LOAD_PATH_LEN)) {
            FreeMem(data, (ULONG)len + 1);
            continue;
        }
        FreeMem(data, (ULONG)len + 1);
        if (Stricmp(dest_got, dest) != 0) {
            continue;
        }
        /* Prefer lexicographically greatest token (latest stamp). */
        Strncpy((STRPTR)leaf, fib->fib_FileName + 3, 16);
        if (!found || Stricmp((STRPTR)leaf, (STRPTR)best_token) > 0) {
            Strncpy((STRPTR)best_token, (STRPTR)leaf, LOAD_TOKEN_LEN);
            Strncpy(best_side, path, LOAD_PATH_LEN);
            Strncpy(best_bak, bak_got, LOAD_PATH_LEN);
            found = 1;
        }
    }

    if (!found) {
        load_print_error((STRPTR)"no rollback bak for destination", 0);
        Printf("Load:   %s\n", (LONG)dest);
        Flush(Output());
        goto out;
    }
    if (!load_path_exists(best_bak)) {
        load_print_error((STRPTR)"rollback bak file missing", 0);
        goto out;
    }
    if (!load_ensure_parent(dest)) {
        goto out;
    }
    if (load_path_exists(dest)) {
        load_clear_write_protect(dest);
        DeleteFile(dest);
    }
    if (!load_copy_file(best_bak, dest)) {
        load_print_error((STRPTR)"rollback copy failed", IoErr());
        goto out;
    }
    if (!quiet) {
        Printf("Load: ROLLBACK %s <- %s\n", (LONG)dest, (LONG)best_bak);
        Flush(Output());
    }
    rc = RETURN_OK;

out:
    if (fib) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    }
    if (lock) {
        UnLock(lock);
    }
    load_free_path(path);
    load_free_path(best_side);
    load_free_path(best_bak);
    load_free_path(dest_got);
    load_free_path(bak_got);
    return rc;
}
