/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * LhTest.c - Standalone lh.library smoke / regression harness.
 *
 * Exercises every public LVO from SDK/SFD/lh_lib.sfd (32 functions):
 *
 *   CreateBuffer, DeleteBuffer, LhEncode, LhDecode, LhCompress, LhDecompress,
 *   LhOpenArchive, LhCloseArchive, LhLock, LhUnLock, LhExamine, LhExNext,
 *   LhExAll, LhExAllEnd, LhInfo, LhOpenFromLock, LhOpen, LhRead, LhWrite,
 *   LhClose, LhSeek, LhNameFromLock, LhAddEntryA,
 *   LhDeleteFile, LhConcatArchive, LhSetPassword, LhReadData, LhExtractEntry,
 *   LhTestEntry, LhPrintEntry, LhErr, LhGetSnapshot.
 *   (LhAddEntry is the ==varargs alias of LhAddEntryA, same LVO.)
 *
 * Fixture entries are added with LH0 via LhAddEntryA so payload round-trips
 * stay deterministic.  Add coverage includes multi-KB in-memory LhAddEntryA
 * and an LhX-style on-disk file with SetProtection/SetComment, TagList add,
 * extract, and Examine of the restored protection bits and filenote.
 * Also covers LHADD_Snapshot / LhGetSnapshot and Latin-1 comment dual-emit
 * (legacy 0x3F + UTF-16 0x46) with nested level-2 paths.
 *
 * Regressions (CheckPoint / lh5 writer fixes):
 *   - LHADD_Method lh5 add must round-trip via real st1 encoder/decoder;
 *     library may still fall back to lh0 when compress expands
 *   - All-identical / Prefs-sized payloads must finish (lh5 EOF hang)
 *   - Larger patterned payloads must finish (no lh5 hash-chain hang)
 *   - Many sequential lh5+snapshot adds (CheckSys Prefs.lha shape)
 *   - LhDeleteFile while archive is open for write/append must not
 *     deadlock; subsequent adds and close must succeed
 *   - lh5 + LHADD_Snapshot together (CheckPoint SET path)
 *
 * Flow: classic buffer -> add round-trips -> snapshot/charset -> lh5/delete
 * regressions -> create fixture -> read APIs -> mutate -> verify.
 *
 *   LhTest >RAM:LhTest.out
 *   LhTest LhA-mos.lha
 *
 * Exit codes: 0 = all pass, 10 = test failure, 20 = library missing.
 */

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/exall.h>
#include <utility/tagitem.h>
#include <stdio.h>
#include <string.h>

#include <libraries/lh.h>
#include <proto/lh.h>
#include <proto/exec.h>
#include <proto/dos.h>

#define LT_RAMDIR       "RAM:LhTest"
#define LT_FIXTURE      "RAM:LhTest/LhTest.lha"
#define LT_FIXTURE2     "RAM:LhTest/LhTstCat.lha"
#define LT_EXTRACT_DIR  "RAM:LhTest/Out"
#define LT_ADD_ARC      "RAM:LhTest/LhAdd.lha"
#define LT_ADD_SRC      "RAM:LhTest/LtAddSrc.bin"
#define LT_ADD_ENTRY    "LtAddSrc.bin"
#define LT_ADD_OUT      "RAM:LhTest/Out/LtAddSrc.bin"
#define LT_ADD_SIZE     4096L
#define LT_ADD_COMMENT  "LhTest Amiga filenote"
#define LT_SNAP_ARC     "RAM:LhTest/LhSnap.lha"
#define LT_SNAP_ENTRY   "Libs/CheckPt.bin"
#define LT_CHAR_ARC     "RAM:LhTest/LhChar.lha"
#define LT_CHAR_ENTRY   "S/note.txt"
/* Latin-1 filenote: "caf" + e-acute (0xE9); dual-emitted as 0x3F + UTF-16 0x46. */
#define LT_CHAR_COMMENT "caf\xE9 filenote"
#define LT_REG_ARC      "RAM:LhTest/LhReg.lha"
#define LT_REG_LH5_ENT  "reg/lh5.bin"
#define LT_REG_STRESS   "reg/stress.bin"
#define LT_REG_SNAP     "Libs/RegSnap.bin"
#define LT_REG_DEL_A    "keep/a.txt"
#define LT_REG_DEL_B    "keep/b.txt"
#define LT_REG_DEL_C    "keep/c.txt"
/* Large enough to exercise lh5 ring/hash; heap, not stack. */
#define LT_REG_STRESS_SZ 8192L
/* CheckSys Prefs.lha: many small repetitive members in one APPEND session. */
#define LT_REG_MANY_N   48L
#define LT_REG_MANY_SZ  128L
/*
 * Distinctive Amiga protection for round-trip checks.  LHA stores one
 * attribute byte; script/pure/archive are high-active, rwed are low-active.
 * FIBF_EXECUTE set = execute protected (no 'e').
 */
#define LT_ADD_PROT     ((LONG)(FIBF_SCRIPT | FIBF_PURE | FIBF_ARCHIVE | FIBF_EXECUTE))
#define LT_METHOD_LH0   0L
#define LT_METHOD_LH5   5L
#define LT_ENTRY_A      "LtReadme.txt"
#define LT_ENTRY_B      "lhtest.bin"
#define LT_ENTRY_C      "LtExtra.txt"
#define LT_CONCAT_ENTRY "LtConcat.txt"

static const STRPTR lt_plain_a = (STRPTR)"LhTest fixture entry A.";
static const STRPTR lt_plain_b =
    (STRPTR)"LhTest fixture entry B (longer payload for lh.library).";
static const STRPTR lt_plain_c = (STRPTR)"LhTest appended entry C.";
static const STRPTR lt_plain_cat = (STRPTR)"LhTest concat donor entry.";

static ULONG lt_pass;
static ULONG lt_fail;
static BOOL lt_got_fixture;
static BOOL lt_mutate_ok;
static char lt_cur_name[80];
static char lt_cur_expect[96];

static const char lt_stack_cookie[] = "$STACK: 8192";

/* Print the test name and expected result before the call under test. */
static VOID
lt_begin(STRPTR name, STRPTR expect)
{
    strncpy(lt_cur_name, (const char *)name, sizeof(lt_cur_name) - 1);
    lt_cur_name[sizeof(lt_cur_name) - 1] = '\0';
    strncpy(lt_cur_expect, (const char *)expect, sizeof(lt_cur_expect) - 1);
    lt_cur_expect[sizeof(lt_cur_expect) - 1] = '\0';
    Printf("LhTest: RUN  %s\n", lt_cur_name);
    Printf("LhTest:   expect: %s\n", lt_cur_expect);
    Flush(Output());
}

/* Print PASS/FAIL and the actual result after the call under test. */
static VOID
lt_end(BOOL ok, STRPTR actual)
{
    if (ok) {
        lt_pass++;
        Printf("LhTest: PASS %s\n", lt_cur_name);
    } else {
        lt_fail++;
        Printf("LhTest: FAIL %s\n", lt_cur_name);
    }
    if (actual != NULL && actual[0] != '\0') {
        Printf("LhTest:   actual: %s\n", actual);
    } else {
        Printf("LhTest:   actual: %s\n", ok ? "ok" : "failed");
    }
    Flush(Output());
}

static VOID
lt_print_api_plan(VOID)
{
    Printf("LhTest: plan - all 32 public LVOs from SDK/SFD/lh_lib.sfd\n");
    Printf("LhTest:   (+ LhAddEntry varargs alias of LhAddEntryA)\n");
    Printf("LhTest:   (+ Snapshot / Latin-1 comment charset paths)\n");
    Printf("LhTest:   (+ lh5 EOF/identical, many-add, delete-while-write)\n");
    Flush(Output());
}

static VOID
lt_lherr_detail(char *buf, LONG bufsz)
{
    sprintf(buf, "LhErr %ld", (long)LhErr());
}

/*
 * Add with LH0 + attrs 0 via LhAddEntryA (explicit method tag).
 */
static BOOL
lt_add_store(struct LhArchive *arc, STRPTR name, APTR data, LONG len)
{
    struct TagItem tags[3];

    tags[0].ti_Tag = LHADD_Method;
    tags[0].ti_Data = (ULONG)LT_METHOD_LH0;
    tags[1].ti_Tag = LHADD_Attrs;
    tags[1].ti_Data = 0UL;
    tags[2].ti_Tag = TAG_DONE;
    return LhAddEntryA(arc, name, data, len, tags) ? TRUE : FALSE;
}

/*
 * Examine a disk file; fill prot/comment.  Returns FALSE if lock/examine fails.
 */
static BOOL
lt_disk_examine(STRPTR path, LONG *prot_out, STRPTR comment_out, LONG comment_max)
{
    BPTR lock;
    struct FileInfoBlock *fib;

    if (prot_out) {
        *prot_out = 0;
    }
    if (comment_out && comment_max > 0) {
        comment_out[0] = '\0';
    }
    lock = Lock(path, ACCESS_READ);
    if (lock == (BPTR)NULL) {
        return FALSE;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        UnLock(lock);
        return FALSE;
    }
    if (!Examine(lock, fib)) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        UnLock(lock);
        return FALSE;
    }
    if (prot_out) {
        *prot_out = fib->fib_Protection;
    }
    if (comment_out && comment_max > 0) {
        /* dos.library Examine(): NUL-terminated C string (RKRM 7.1). */
        strncpy((char *)comment_out, (char *)fib->fib_Comment,
            (size_t)comment_max - 1);
        comment_out[comment_max - 1] = '\0';
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    UnLock(lock);
    return TRUE;
}

static BOOL
lt_ensure_dir(STRPTR path)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    BOOL is_dir;

    lock = CreateDir(path);
    if (lock != (BPTR)NULL) {
        UnLock(lock);
        return TRUE;
    }
    if (IoErr() != ERROR_OBJECT_EXISTS) {
        return FALSE;
    }
    lock = Lock(path, ACCESS_READ);
    if (lock == (BPTR)NULL) {
        return FALSE;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        UnLock(lock);
        return FALSE;
    }
    is_dir = FALSE;
    if (Examine(lock, fib) && fib->fib_DirEntryType > 0) {
        is_dir = TRUE;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    UnLock(lock);
    if (is_dir) {
        return TRUE;
    }
    /* A plain file at this path blocks CreateDir for nested archives. */
    DeleteFile(path);
    lock = CreateDir(path);
    if (lock != (BPTR)NULL) {
        UnLock(lock);
        return TRUE;
    }
    return FALSE;
}

static BOOL
lt_ensure_parent(STRPTR filepath)
{
    char parent[256];
    STRPTR slash;

    strncpy(parent, (const char *)filepath, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    slash = (STRPTR)strrchr(parent, '/');
    if (slash == NULL) {
        return TRUE;
    }
    *slash = '\0';
    if (parent[0] == '\0') {
        return TRUE;
    }
    return lt_ensure_dir((STRPTR)parent);
}

static VOID
lt_remove_path(STRPTR path)
{
    BPTR fh;
    char aside[280];

    /* Remove any prior fixture; rename aside if DeleteFile is stubborn. */
    for (;;) {
        SetIoErr(0);
        fh = Open(path, MODE_OLDFILE);
        if (fh == (BPTR)NULL) {
            break;
        }
        Close(fh);
        if (!DeleteFile(path)) {
            strncpy(aside, (const char *)path, sizeof(aside) - 4);
            aside[sizeof(aside) - 4] = '\0';
            strcat(aside, ".$$");
            SetIoErr(0);
            if (Rename(path, (STRPTR)aside)) {
                DeleteFile((STRPTR)aside);
            }
            break;
        }
    }
    SetIoErr(0);
}

static BOOL
lt_test_open(VOID)
{
    lt_begin((STRPTR)"OpenLibrary(lh.library,2)",
        (STRPTR)"non-NULL LhBase");
    LhBase = OpenLibrary((STRPTR)LH_NAME, (LONG)LH_MIN_VERSION);
    if (LhBase == NULL) {
        lt_end(FALSE, (STRPTR)"NULL (not found)");
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)"non-NULL LhBase");
    if (LhBase->lib_IdString != NULL) {
        Printf("LhTest: library %s\n", LhBase->lib_IdString);
        Flush(Output());
    }
    return TRUE;
}

static VOID
lt_test_classic_buffer(VOID)
{
    struct LhBuffer *buf;
    struct LhBuffer *decbuf;
    static UBYTE comp[256];
    static UBYTE back[256];
    ULONG n;
    ULONG plain_len;
    char actual[48];

    plain_len = (ULONG)strlen((const char *)lt_plain_a);

    lt_begin((STRPTR)"CreateBuffer(FALSE)", (STRPTR)"non-NULL buffer");
    buf = CreateBuffer(FALSE);
    if (buf == NULL) {
        lt_end(FALSE, (STRPTR)"NULL");
        return;
    }
    lt_end(TRUE, (STRPTR)"non-NULL buffer");

    buf->lh_Src = (APTR)lt_plain_a;
    buf->lh_SrcSize = plain_len;
    buf->lh_Dst = (APTR)comp;
    buf->lh_DstSize = (ULONG)sizeof(comp);
    lt_begin((STRPTR)"LhEncode", (STRPTR)"compressed size > 0");
    n = LhEncode(buf);
    if (n == 0) {
        DeleteBuffer(buf);
        lt_end(FALSE, (STRPTR)"size=0");
        return;
    }
    sprintf(actual, "size=%lu", (unsigned long)n);
    lt_end(TRUE, (STRPTR)actual);

    buf->lh_Src = (APTR)comp;
    buf->lh_SrcSize = n;
    buf->lh_Dst = (APTR)back;
    buf->lh_DstSize = plain_len;
    memset(back, 0, sizeof(back));
    lt_begin((STRPTR)"LhDecode", (STRPTR)"original payload restored");
    n = LhDecode(buf);
    if (n != plain_len || memcmp(back, lt_plain_a, plain_len) != 0) {
        DeleteBuffer(buf);
        sprintf(actual, "size=%lu mismatch", (unsigned long)n);
        lt_end(FALSE, (STRPTR)actual);
        return;
    }
    sprintf(actual, "size=%lu match", (unsigned long)n);
    lt_end(TRUE, (STRPTR)actual);

    lt_begin((STRPTR)"DeleteBuffer", (STRPTR)"buffer freed");
    DeleteBuffer(buf);
    lt_end(TRUE, (STRPTR)"freed");

    lt_begin((STRPTR)"CreateBuffer(TRUE)", (STRPTR)"non-NULL decode-only buffer");
    decbuf = CreateBuffer(TRUE);
    if (decbuf == NULL) {
        lt_end(FALSE, (STRPTR)"NULL");
        return;
    }
    lt_end(TRUE, (STRPTR)"non-NULL decode-only buffer");

    lt_begin((STRPTR)"DeleteBuffer(decode-only)", (STRPTR)"buffer freed");
    DeleteBuffer(decbuf);
    lt_end(TRUE, (STRPTR)"freed");
}

static VOID
lt_test_codec_lh0(VOID)
{
    struct LhBuffer *buf;
    static UBYTE comp[512];
    static UBYTE back[256];
    ULONG n;
    ULONG plain_len;
    char actual[48];

    plain_len = (ULONG)strlen((const char *)lt_plain_b);

    lt_begin((STRPTR)"LhCompress setup", (STRPTR)"non-NULL buffer");
    buf = CreateBuffer(FALSE);
    if (buf == NULL) {
        lt_end(FALSE, (STRPTR)"NULL");
        return;
    }
    lt_end(TRUE, (STRPTR)"non-NULL buffer");

    buf->lh_Src = (APTR)lt_plain_b;
    buf->lh_SrcSize = plain_len;
    buf->lh_Dst = (APTR)comp;
    buf->lh_DstSize = (ULONG)sizeof(comp);
    lt_begin((STRPTR)"LhCompress", (STRPTR)"LH0 size > 0");
    n = LhCompress(LT_METHOD_LH0, buf);
    if (n == 0) {
        DeleteBuffer(buf);
        lt_end(FALSE, (STRPTR)"size=0");
        return;
    }
    sprintf(actual, "size=%lu", (unsigned long)n);
    lt_end(TRUE, (STRPTR)actual);

    buf->lh_Src = (APTR)comp;
    buf->lh_SrcSize = n;
    buf->lh_Dst = (APTR)back;
    buf->lh_DstSize = plain_len;
    memset(back, 0, sizeof(back));
    lt_begin((STRPTR)"LhDecompress", (STRPTR)"original payload restored");
    n = LhDecompress(LT_METHOD_LH0, buf);
    if (n != plain_len || memcmp(back, lt_plain_b, plain_len) != 0) {
        DeleteBuffer(buf);
        sprintf(actual, "size=%lu mismatch", (unsigned long)n);
        lt_end(FALSE, (STRPTR)actual);
        return;
    }
    sprintf(actual, "size=%lu match", (unsigned long)n);
    lt_end(TRUE, (STRPTR)actual);

    lt_begin((STRPTR)"DeleteBuffer(codec)", (STRPTR)"buffer freed");
    DeleteBuffer(buf);
    lt_end(TRUE, (STRPTR)"freed");
}

static BOOL
lt_write_archive(STRPTR path, STRPTR name_a, STRPTR data_a, ULONG len_a,
    STRPTR name_b, STRPTR data_b, ULONG len_b)
{
    struct LhArchive *arc;
    char detail[40];
    char expect[80];

    lt_begin((STRPTR)"fixture RAM dir", (STRPTR)"directory exists");
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)LT_RAMDIR);
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)LT_RAMDIR);

    lt_begin((STRPTR)"fixture parent dir", (STRPTR)"parent path ready");
    if (!lt_ensure_parent(path)) {
        lt_end(FALSE, path);
        return FALSE;
    }
    lt_end(TRUE, path);

    lt_remove_path(path);
    SetIoErr(0);

    lt_begin((STRPTR)"LhOpenArchive(write)", (STRPTR)"non-NULL archive");
    arc = LhOpenArchive(path, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, path);

    sprintf(expect, "DOSTRUE for %s", (const char *)name_a);
    lt_begin((STRPTR)"LhAddEntryA", (STRPTR)expect);
    if (!lt_add_store(arc, name_a, (APTR)data_a, (LONG)len_a)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, name_a);

    sprintf(expect, "DOSTRUE for %s", (const char *)name_b);
    lt_begin((STRPTR)"LhAddEntryA", (STRPTR)expect);
    if (!lt_add_store(arc, name_b, (APTR)data_b, (LONG)len_b)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, name_b);

    lt_begin((STRPTR)"LhCloseArchive", (STRPTR)"DOSTRUE");
    if (!LhCloseArchive(arc)) {
        lt_end(FALSE, (STRPTR)"DOSFALSE");
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)"DOSTRUE");
    return TRUE;
}

static BOOL
lt_count_entries(STRPTR path, LONG *count_out)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG count;

    count = 0;
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        return FALSE;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        LhCloseArchive(arc);
        return FALSE;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        return FALSE;
    }
    /*
     * Amiga dir walk: Examine fills the directory itself; ExNext yields
     * children.  Count children only (matches LhExAll / catalog size).
     */
    ok = LhExamine(lock, fib);
    while (ok) {
        ok = LhExNext(lock, fib);
        if (ok) {
            count++;
        }
    }
    LhUnLock(lock);
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
    *count_out = count;
    return TRUE;
}

static VOID
lt_test_addentry_default(VOID)
{
    struct LhArchive *arc;
    struct FileInfoBlock *fib;
    struct TagItem tags[5];
    BPTR flock;
    BPTR fh;
    APTR disk_data;
    APTR round_data;
    LONG disk_len;
    LONG round_len;
    LONG wrote;
    LONG i;
    LONG n;
    LONG src_prot;
    LONG out_prot;
    ULONG mem_len;
    char *mem_payload;
    char src_comment[80];
    char out_comment[80];
    char detail[48];
    char expect[48];

    /*
     * In-memory LhAddEntryA with a multi-KB payload (not a 1-byte smoke).
     * Catches store/header issues that a single-byte add would miss.
     */
    mem_len = 2048UL;
    sprintf(expect, "round-trip %lu bytes", (unsigned long)mem_len);
    lt_begin((STRPTR)"LhAddEntryA", (STRPTR)expect);
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    mem_payload = (char *)AllocMem(mem_len, MEMF_PUBLIC | MEMF_CLEAR);
    if (mem_payload == NULL) {
        lt_end(FALSE, (STRPTR)"AllocMem failed");
        return;
    }
    for (i = 0; i < (LONG)mem_len; i++) {
        mem_payload[i] = (char)(i & 0xff);
    }
    lt_remove_path((STRPTR)LT_ADD_ARC);
    arc = LhOpenArchive((STRPTR)LT_ADD_ARC, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        FreeMem(mem_payload, mem_len);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhAddEntryA(arc, (STRPTR)"plain.bin", (APTR)mem_payload, (LONG)mem_len, NULL)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        FreeMem(mem_payload, mem_len);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhCloseArchive(arc)) {
        FreeMem(mem_payload, mem_len);
        lt_end(FALSE, (STRPTR)"close failed");
        return;
    }
    arc = LhOpenArchive((STRPTR)LT_ADD_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        FreeMem(mem_payload, mem_len);
        lt_end(FALSE, (STRPTR)"reopen failed");
        return;
    }
    round_data = NULL;
    round_len = LhReadData(arc, (STRPTR)"plain.bin", &round_data);
    if (round_len != (LONG)mem_len || round_data == NULL
            || memcmp(round_data, mem_payload, (size_t)mem_len) != 0) {
        sprintf(detail, "len=%ld mismatch", (long)round_len);
        if (round_data != NULL) {
            FreeMem(round_data, (ULONG)round_len > 0 ? (ULONG)round_len : 1UL);
        }
        LhCloseArchive(arc);
        FreeMem(mem_payload, mem_len);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(round_data, (ULONG)round_len);
    LhCloseArchive(arc);
    FreeMem(mem_payload, mem_len);
    lt_end(TRUE, (STRPTR)expect);

    /*
     * LhX-style path: real disk file with SetProtection/SetComment, FIB
     * TagList add, content round-trip, then extract and Examine metadata.
     */
    sprintf(expect, "disk %ld bytes + FIB tags", (long)LT_ADD_SIZE);
    lt_begin((STRPTR)"LhAddEntryA(disk)", (STRPTR)expect);
    lt_remove_path((STRPTR)LT_ADD_SRC);
    lt_remove_path((STRPTR)LT_ADD_ARC);
    disk_data = AllocMem((ULONG)LT_ADD_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (disk_data == NULL) {
        lt_end(FALSE, (STRPTR)"AllocMem failed");
        return;
    }
    for (i = 0; i < LT_ADD_SIZE; i++) {
        ((unsigned char *)disk_data)[i] = (unsigned char)((i * 17 + 3) & 0xff);
    }
    fh = Open((STRPTR)LT_ADD_SRC, MODE_NEWFILE);
    if (fh == (BPTR)NULL) {
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"cannot create src");
        return;
    }
    wrote = Write(fh, disk_data, LT_ADD_SIZE);
    Close(fh);
    if (wrote != LT_ADD_SIZE) {
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"Write src failed");
        return;
    }

    /* Amiga metadata on the input file before archiving (LhX reads via FIB). */
    if (!SetComment((STRPTR)LT_ADD_SRC, (STRPTR)LT_ADD_COMMENT)) {
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"SetComment src failed");
        return;
    }
    if (!SetProtection((STRPTR)LT_ADD_SRC, LT_ADD_PROT)) {
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"SetProtection src failed");
        return;
    }
    if (!lt_disk_examine((STRPTR)LT_ADD_SRC, &src_prot, (STRPTR)src_comment,
            (LONG)sizeof(src_comment))) {
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"Examine src failed");
        return;
    }
    if ((src_prot & 0xffL) != (LT_ADD_PROT & 0xffL)
            || strcmp(src_comment, LT_ADD_COMMENT) != 0) {
        sprintf(detail, "src prot=%ld note bad", (long)src_prot);
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }

    /* Size via Seek the Amiga way (same as Handle / fixed LhX). */
    fh = Open((STRPTR)LT_ADD_SRC, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"cannot reopen src");
        return;
    }
    if (Seek(fh, 0L, OFFSET_END) < 0) {
        Close(fh);
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"Seek END failed");
        return;
    }
    disk_len = Seek(fh, 0L, OFFSET_BEGINNING);
    Close(fh);
    if (disk_len != LT_ADD_SIZE) {
        sprintf(detail, "Seek size=%ld", (long)disk_len);
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }

    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"FIB AllocMem failed");
        return;
    }
    arc = LhOpenArchive((STRPTR)LT_ADD_ARC, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    n = 0;
    flock = Lock((STRPTR)LT_ADD_SRC, ACCESS_READ);
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
    if (!LhAddEntryA(arc, (STRPTR)LT_ADD_ENTRY, disk_data, disk_len, tags)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    if (!LhCloseArchive(arc)) {
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"close failed");
        return;
    }

    arc = LhOpenArchive((STRPTR)LT_ADD_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"reopen failed");
        return;
    }
    round_data = NULL;
    round_len = LhReadData(arc, (STRPTR)LT_ADD_ENTRY, &round_data);
    if (round_len != LT_ADD_SIZE || round_data == NULL
            || memcmp(round_data, disk_data, (size_t)LT_ADD_SIZE) != 0) {
        sprintf(detail, "len=%ld content bad", (long)round_len);
        if (round_data != NULL && round_len > 0) {
            FreeMem(round_data, (ULONG)round_len);
        }
        LhCloseArchive(arc);
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(round_data, (ULONG)round_len);

    /* Catalog must expose the same protection and filenote. */
    flock = LhLock(arc, (STRPTR)LT_ADD_ENTRY);
    if (flock == (BPTR)NULL) {
        LhCloseArchive(arc);
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"LhLock entry failed");
        return;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        LhUnLock(flock);
        LhCloseArchive(arc);
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)"FIB2 AllocMem failed");
        return;
    }
    if (!LhExamine(flock, fib)
            || (fib->fib_Protection & 0xffL) != (LT_ADD_PROT & 0xffL)
            || strcmp(fib->fib_Comment, LT_ADD_COMMENT) != 0) {
        sprintf(detail, "arc prot=%ld note bad", (long)fib->fib_Protection);
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhUnLock(flock);
        LhCloseArchive(arc);
        FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhUnLock(flock);
    LhCloseArchive(arc);
    FreeMem(disk_data, (ULONG)LT_ADD_SIZE);
    lt_end(TRUE, (STRPTR)expect);

    /*
     * Extract to disk and Examine the restored file — proves SetProtection
     * / SetComment on the unarchived object match the archived Amiga attrs.
     */
    lt_begin((STRPTR)"LhExtractEntry(meta)",
        (STRPTR)"prot+comment on extracted file");
    if (!lt_ensure_dir((STRPTR)LT_EXTRACT_DIR)) {
        lt_end(FALSE, (STRPTR)"mkdir Out failed");
        return;
    }
    SetProtection((STRPTR)LT_ADD_OUT, 0);
    DeleteFile((STRPTR)LT_ADD_OUT);
    arc = LhOpenArchive((STRPTR)LT_ADD_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open for extract failed");
        return;
    }
    if (!LhExtractEntry(arc, (STRPTR)LT_ADD_ENTRY, (STRPTR)LT_ADD_OUT)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    LhCloseArchive(arc);
    if (!lt_disk_examine((STRPTR)LT_ADD_OUT, &out_prot, (STRPTR)out_comment,
            (LONG)sizeof(out_comment))) {
        lt_end(FALSE, (STRPTR)"Examine extract failed");
        return;
    }
    if ((out_prot & 0xffL) != (LT_ADD_PROT & 0xffL)) {
        sprintf(detail, "prot got=%ld want=%ld", (long)out_prot,
            (long)LT_ADD_PROT);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (strcmp(out_comment, LT_ADD_COMMENT) != 0) {
        sprintf(detail, "comment='%s'", out_comment);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, (STRPTR)"prot+comment match");
}

/*
 * Snapshot ext 0x47: LHADD_Snapshot on write, LhGetSnapshot on read.
 */
static VOID
lt_test_snapshot(VOID)
{
    struct LhArchive *arc;
    struct LhSnapshot *sn;
    struct LhSnapshot *got;
    struct TagItem tags[4];
    static UBYTE payload[] = "snapshot-payload";
    LONG i;
    char detail[64];

    sn = NULL;
    got = NULL;
    arc = NULL;

    lt_begin((STRPTR)"LhGetSnapshot",
        (STRPTR)"round-trip LHADD_Snapshot fields");
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    lt_remove_path((STRPTR)LT_SNAP_ARC);

    /* LhSnapshot is ~250 bytes; keep off the Amiga stack. */
    sn = (struct LhSnapshot *)AllocMem((ULONG)sizeof(*sn),
        MEMF_PUBLIC | MEMF_CLEAR);
    got = (struct LhSnapshot *)AllocMem((ULONG)sizeof(*got),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (sn == NULL || got == NULL) {
        if (sn != NULL) {
            FreeMem(sn, (ULONG)sizeof(*sn));
        }
        if (got != NULL) {
            FreeMem(got, (ULONG)sizeof(*got));
        }
        lt_end(FALSE, (STRPTR)"AllocMem snapshot failed");
        return;
    }

    sn->lsn_Generation = 3UL;
    sn->lsn_Size = (ULONG)(sizeof(payload) - 1);
    sn->lsn_Flags = 0;
    sn->lsn_Format = LH_SNAP_FORMAT_V1;
    for (i = 0; i < LH_SNAP_ID_LEN; i++) {
        sn->lsn_SnapId[i] = (UBYTE)(0x10 + i);
    }
    for (i = 0; i < LH_SNAP_SHA256_LEN; i++) {
        sn->lsn_SHA256[i] = (UBYTE)(0xA0 + (i & 0x0f));
    }
    strcpy(sn->lsn_Ver, "$VER: LhTest 1.0");

    tags[0].ti_Tag = LHADD_Method;
    tags[0].ti_Data = (ULONG)LT_METHOD_LH0;
    tags[1].ti_Tag = LHADD_Snapshot;
    tags[1].ti_Data = (ULONG)sn;
    tags[2].ti_Tag = TAG_DONE;

    arc = LhOpenArchive((STRPTR)LT_SNAP_ARC, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhAddEntryA(arc, (STRPTR)LT_SNAP_ENTRY, payload,
            (LONG)(sizeof(payload) - 1), tags)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    /* Plain member without Snapshot — negative path below. */
    if (!lt_add_store(arc, (STRPTR)"plain.txt", (APTR)"x", 1L)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhCloseArchive(arc)) {
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)"close failed");
        return;
    }
    arc = NULL;

    arc = LhOpenArchive((STRPTR)LT_SNAP_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)"reopen failed");
        return;
    }
    memset(got, 0, sizeof(*got));
    if (!LhGetSnapshot(arc, (STRPTR)LT_SNAP_ENTRY, got)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (got->lsn_Generation != 3UL
            || got->lsn_Size != (ULONG)(sizeof(payload) - 1)
            || got->lsn_Format != LH_SNAP_FORMAT_V1
            || strcmp(got->lsn_Ver, "$VER: LhTest 1.0") != 0) {
        sprintf(detail, "gen=%lu size=%lu ver='%s'",
            (unsigned long)got->lsn_Generation,
            (unsigned long)got->lsn_Size, got->lsn_Ver);
        LhCloseArchive(arc);
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    for (i = 0; i < LH_SNAP_ID_LEN; i++) {
        if (got->lsn_SnapId[i] != (UBYTE)(0x10 + i)) {
            LhCloseArchive(arc);
            FreeMem(sn, (ULONG)sizeof(*sn));
            FreeMem(got, (ULONG)sizeof(*got));
            lt_end(FALSE, (STRPTR)"snap_id mismatch");
            return;
        }
    }
    for (i = 0; i < LH_SNAP_SHA256_LEN; i++) {
        if (got->lsn_SHA256[i] != (UBYTE)(0xA0 + (i & 0x0f))) {
            LhCloseArchive(arc);
            FreeMem(sn, (ULONG)sizeof(*sn));
            FreeMem(got, (ULONG)sizeof(*got));
            lt_end(FALSE, (STRPTR)"sha256 mismatch");
            return;
        }
    }
    /* Entry present but no 0x47 extension. */
    if (LhGetSnapshot(arc, (STRPTR)"plain.txt", got)) {
        LhCloseArchive(arc);
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)"plain.txt unexpectedly has snapshot");
        return;
    }
    /* Missing member. */
    if (LhGetSnapshot(arc, (STRPTR)"nosuch", got)) {
        LhCloseArchive(arc);
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)"nosuch unexpectedly ok");
        return;
    }
    LhCloseArchive(arc);
    FreeMem(sn, (ULONG)sizeof(*sn));
    FreeMem(got, (ULONG)sizeof(*got));
    lt_end(TRUE, (STRPTR)"snapshot fields + negatives");

    /*
     * Same member path twice: LhGetSnapshot / extract use highest
     * generation (CheckPoint multi-gen store).
     */
    {
        static UBYTE payload_g1[] = "gen-one";
        static UBYTE payload_g2[] = "gen-two-payload";
        UBYTE *rdata;
        LONG rlen;

        sn = NULL;
        got = NULL;
        arc = NULL;
        rdata = NULL;
        rlen = 0;

        lt_begin((STRPTR)"LhGetSnapshot multi-gen",
            (STRPTR)"same name keeps max generation");
        if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
            lt_end(FALSE, (STRPTR)"mkdir failed");
            return;
        }
        lt_remove_path((STRPTR)LT_SNAP_ARC);

        sn = (struct LhSnapshot *)AllocMem((ULONG)sizeof(*sn),
            MEMF_PUBLIC | MEMF_CLEAR);
        got = (struct LhSnapshot *)AllocMem((ULONG)sizeof(*got),
            MEMF_PUBLIC | MEMF_CLEAR);
        if (sn == NULL || got == NULL) {
            if (sn != NULL) {
                FreeMem(sn, (ULONG)sizeof(*sn));
            }
            if (got != NULL) {
                FreeMem(got, (ULONG)sizeof(*got));
            }
            lt_end(FALSE, (STRPTR)"AllocMem snapshot failed");
            return;
        }

        arc = LhOpenArchive((STRPTR)LT_SNAP_ARC, LHARC_MODE_WRITE);
        if (arc == NULL) {
            lt_lherr_detail(detail, (LONG)sizeof(detail));
            FreeMem(sn, (ULONG)sizeof(*sn));
            FreeMem(got, (ULONG)sizeof(*got));
            lt_end(FALSE, (STRPTR)detail);
            return;
        }

        memset(sn, 0, sizeof(*sn));
        sn->lsn_Generation = 1UL;
        sn->lsn_Size = (ULONG)(sizeof(payload_g1) - 1);
        sn->lsn_Format = LH_SNAP_FORMAT_V1;
        sn->lsn_SHA256[0] = 0x01;
        tags[0].ti_Tag = LHADD_Method;
        tags[0].ti_Data = (ULONG)LT_METHOD_LH0;
        tags[1].ti_Tag = LHADD_Snapshot;
        tags[1].ti_Data = (ULONG)sn;
        tags[2].ti_Tag = TAG_DONE;
        if (!LhAddEntryA(arc, (STRPTR)LT_SNAP_ENTRY, payload_g1,
                (LONG)(sizeof(payload_g1) - 1), tags)) {
            lt_lherr_detail(detail, (LONG)sizeof(detail));
            LhCloseArchive(arc);
            FreeMem(sn, (ULONG)sizeof(*sn));
            FreeMem(got, (ULONG)sizeof(*got));
            lt_end(FALSE, (STRPTR)detail);
            return;
        }

        sn->lsn_Generation = 2UL;
        sn->lsn_Size = (ULONG)(sizeof(payload_g2) - 1);
        sn->lsn_SHA256[0] = 0x02;
        if (!LhAddEntryA(arc, (STRPTR)LT_SNAP_ENTRY, payload_g2,
                (LONG)(sizeof(payload_g2) - 1), tags)) {
            lt_lherr_detail(detail, (LONG)sizeof(detail));
            LhCloseArchive(arc);
            FreeMem(sn, (ULONG)sizeof(*sn));
            FreeMem(got, (ULONG)sizeof(*got));
            lt_end(FALSE, (STRPTR)detail);
            return;
        }
        if (!LhCloseArchive(arc)) {
            FreeMem(sn, (ULONG)sizeof(*sn));
            FreeMem(got, (ULONG)sizeof(*got));
            lt_end(FALSE, (STRPTR)"close failed");
            return;
        }
        arc = NULL;

        arc = LhOpenArchive((STRPTR)LT_SNAP_ARC, LHARC_MODE_READ);
        if (arc == NULL) {
            FreeMem(sn, (ULONG)sizeof(*sn));
            FreeMem(got, (ULONG)sizeof(*got));
            lt_end(FALSE, (STRPTR)"reopen failed");
            return;
        }
        memset(got, 0, sizeof(*got));
        if (!LhGetSnapshot(arc, (STRPTR)LT_SNAP_ENTRY, got)
                || got->lsn_Generation != 2UL
                || got->lsn_SHA256[0] != 0x02) {
            sprintf(detail, "gen=%lu sha0=%lu",
                (unsigned long)got->lsn_Generation,
                (unsigned long)got->lsn_SHA256[0]);
            LhCloseArchive(arc);
            FreeMem(sn, (ULONG)sizeof(*sn));
            FreeMem(got, (ULONG)sizeof(*got));
            lt_end(FALSE, (STRPTR)detail);
            return;
        }
        rlen = LhReadData(arc, (STRPTR)LT_SNAP_ENTRY, (APTR *)&rdata);
        if (rlen != (LONG)(sizeof(payload_g2) - 1) || rdata == NULL
                || memcmp(rdata, payload_g2, (size_t)rlen) != 0) {
            if (rdata != NULL && rlen > 0) {
                FreeMem(rdata, (ULONG)rlen);
            }
            LhCloseArchive(arc);
            FreeMem(sn, (ULONG)sizeof(*sn));
            FreeMem(got, (ULONG)sizeof(*got));
            lt_end(FALSE, (STRPTR)"read not latest payload");
            return;
        }
        FreeMem(rdata, (ULONG)rlen);
        LhCloseArchive(arc);
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(TRUE, (STRPTR)"gen2 wins GetSnapshot+ReadData");
    }
}

/*
 * Nested level-2 path + Latin-1 filenote (dual-emitted 0x3F / UTF-16 0x46).
 */
static VOID
lt_test_charset_comment(VOID)
{
    struct LhArchive *arc;
    struct TagItem tags[4];
    static UBYTE payload[] = "charset-body";
    char out_path[80];
    char out_comment[80];
    LONG out_prot;
    char detail[64];

    lt_begin((STRPTR)"charset comment",
        (STRPTR)"nested path + Latin-1 filenote round-trip");
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    if (!lt_ensure_dir((STRPTR)LT_EXTRACT_DIR)) {
        lt_end(FALSE, (STRPTR)"mkdir Out failed");
        return;
    }
    lt_remove_path((STRPTR)LT_CHAR_ARC);

    tags[0].ti_Tag = LHADD_Method;
    tags[0].ti_Data = (ULONG)LT_METHOD_LH0;
    tags[1].ti_Tag = LHADD_Comment;
    tags[1].ti_Data = (ULONG)LT_CHAR_COMMENT;
    tags[2].ti_Tag = TAG_DONE;

    arc = LhOpenArchive((STRPTR)LT_CHAR_ARC, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhAddEntryA(arc, (STRPTR)LT_CHAR_ENTRY, payload,
            (LONG)(sizeof(payload) - 1), tags)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhCloseArchive(arc)) {
        lt_end(FALSE, (STRPTR)"close failed");
        return;
    }

    arc = LhOpenArchive((STRPTR)LT_CHAR_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"reopen failed");
        return;
    }
    strcpy(out_path, LT_EXTRACT_DIR);
    if (out_path[strlen(out_path) - 1] != ':' && out_path[strlen(out_path) - 1] != '/') {
        strcat(out_path, "/");
    }
    strcat(out_path, "note.txt");
    lt_remove_path((STRPTR)out_path);
    if (!LhExtractEntry(arc, (STRPTR)LT_CHAR_ENTRY, (STRPTR)out_path)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    LhCloseArchive(arc);

    out_comment[0] = '\0';
    out_prot = 0;
    if (!lt_disk_examine((STRPTR)out_path, &out_prot, (STRPTR)out_comment,
            (LONG)sizeof(out_comment))) {
        lt_end(FALSE, (STRPTR)"Examine extract failed");
        return;
    }
    if (strcmp(out_comment, LT_CHAR_COMMENT) != 0) {
        sprintf(detail, "comment got len=%ld", (long)strlen(out_comment));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, (STRPTR)"Latin-1 filenote restored");
}

/*
 * Add with an explicit LHADD_Method (lh5 / store / etc).
 */
static BOOL
lt_add_method(struct LhArchive *arc, STRPTR name, APTR data, LONG len,
    LONG method)
{
    struct TagItem tags[3];

    tags[0].ti_Tag = LHADD_Method;
    tags[0].ti_Data = (ULONG)method;
    tags[1].ti_Tag = LHADD_Attrs;
    tags[1].ti_Data = 0UL;
    tags[2].ti_Tag = TAG_DONE;
    return LhAddEntryA(arc, name, data, len, tags) ? TRUE : FALSE;
}

/*
 * Fill buf with a simple LCG so lh5 often expands; library must fall back
 * to lh0 rather than failing with LhErr 223.
 */
static VOID
lt_fill_noise(UBYTE *buf, LONG len, ULONG seed)
{
    LONG i;
    ULONG x;

    x = seed;
    for (i = 0; i < len; i++) {
        x = x * 1103515245UL + 12345UL;
        buf[i] = (UBYTE)((x >> 16) & 0xffUL);
    }
}

/*
 * Repeating short motifs stress the lh5 hash chain / ring without needing
 * megabytes on Amiga RAM.
 */
static VOID
lt_fill_lh5_stress(UBYTE *buf, LONG len)
{
    LONG i;
    static const char motif[] = "AaBbCc0123456789Lh5Hash";

    for (i = 0; i < len; i++) {
        buf[i] = (UBYTE)motif[i % (LONG)(sizeof(motif) - 1)];
        if ((i & 31) == 0) {
            buf[i] ^= (UBYTE)(i & 0xff);
        }
    }
}

/*
 * LHADD_Method=5 must succeed for compressible and expanding payloads.
 * Expanding data may be stored as -lh0-; LhErr 223 is a regression.
 */
static VOID
lt_test_lh5_add_roundtrip(VOID)
{
    struct LhArchive *arc;
    APTR round;
    UBYTE *comp;
    UBYTE *noise;
    LONG round_len;
    LONG n;
    char detail[48];

    comp = NULL;
    noise = NULL;
    round = NULL;
    n = 512L;

    lt_begin((STRPTR)"LhAddEntryA(lh5)",
        (STRPTR)"lh5 method round-trip + expand fallback");
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    lt_remove_path((STRPTR)LT_REG_ARC);

    comp = (UBYTE *)AllocMem((ULONG)n, MEMF_PUBLIC | MEMF_CLEAR);
    noise = (UBYTE *)AllocMem((ULONG)n, MEMF_PUBLIC | MEMF_CLEAR);
    if (comp == NULL || noise == NULL) {
        if (comp != NULL) {
            FreeMem(comp, (ULONG)n);
        }
        if (noise != NULL) {
            FreeMem(noise, (ULONG)n);
        }
        lt_end(FALSE, (STRPTR)"AllocMem failed");
        return;
    }
    memset(comp, 0x55, (size_t)n);
    lt_fill_noise(noise, n, 0xC0FFEEuL);

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        FreeMem(comp, (ULONG)n);
        FreeMem(noise, (ULONG)n);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!lt_add_method(arc, (STRPTR)LT_REG_LH5_ENT, (APTR)comp, n,
            LT_METHOD_LH5)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        FreeMem(comp, (ULONG)n);
        FreeMem(noise, (ULONG)n);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!lt_add_method(arc, (STRPTR)"reg/noise.bin", (APTR)noise, n,
            LT_METHOD_LH5)) {
        /*
         * Pre-fix: compress expand/fail mapped to ERROR_WRITE_PROTECTED
         * (223).  Must not happen after lh0 fallback.
         */
        sprintf(detail, "noise LhErr %ld", (long)LhErr());
        LhCloseArchive(arc);
        FreeMem(comp, (ULONG)n);
        FreeMem(noise, (ULONG)n);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhCloseArchive(arc)) {
        FreeMem(comp, (ULONG)n);
        FreeMem(noise, (ULONG)n);
        lt_end(FALSE, (STRPTR)"close failed");
        return;
    }

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        FreeMem(comp, (ULONG)n);
        FreeMem(noise, (ULONG)n);
        lt_end(FALSE, (STRPTR)"reopen failed");
        return;
    }
    round_len = LhReadData(arc, (STRPTR)LT_REG_LH5_ENT, &round);
    if (round_len != n || round == NULL
            || memcmp(round, comp, (size_t)n) != 0) {
        sprintf(detail, "comp len=%ld", (long)round_len);
        if (round != NULL) {
            FreeMem(round, (ULONG)round_len > 0 ? (ULONG)round_len : 1UL);
        }
        LhCloseArchive(arc);
        FreeMem(comp, (ULONG)n);
        FreeMem(noise, (ULONG)n);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(round, (ULONG)round_len);
    round = NULL;
    round_len = LhReadData(arc, (STRPTR)"reg/noise.bin", &round);
    if (round_len != n || round == NULL
            || memcmp(round, noise, (size_t)n) != 0) {
        sprintf(detail, "noise len=%ld", (long)round_len);
        if (round != NULL) {
            FreeMem(round, (ULONG)round_len > 0 ? (ULONG)round_len : 1UL);
        }
        LhCloseArchive(arc);
        FreeMem(comp, (ULONG)n);
        FreeMem(noise, (ULONG)n);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(round, (ULONG)round_len);
    LhCloseArchive(arc);
    FreeMem(comp, (ULONG)n);
    FreeMem(noise, (ULONG)n);
    lt_end(TRUE, (STRPTR)"lh5 + expand-fallback ok");
}

/*
 * ~8KB patterned payload: must finish (no hash-chain hang) and round-trip.
 */
static VOID
lt_test_lh5_hash_stress(VOID)
{
    struct LhArchive *arc;
    APTR round;
    UBYTE *buf;
    LONG round_len;
    char detail[48];

    buf = NULL;
    round = NULL;

    lt_begin((STRPTR)"LhAddEntryA(lh5 stress)",
        (STRPTR)"8KB patterned lh5 finishes + round-trip");
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    lt_remove_path((STRPTR)LT_REG_ARC);

    buf = (UBYTE *)AllocMem((ULONG)LT_REG_STRESS_SZ, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL) {
        lt_end(FALSE, (STRPTR)"AllocMem failed");
        return;
    }
    lt_fill_lh5_stress(buf, LT_REG_STRESS_SZ);

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        FreeMem(buf, (ULONG)LT_REG_STRESS_SZ);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!lt_add_method(arc, (STRPTR)LT_REG_STRESS, (APTR)buf,
            LT_REG_STRESS_SZ, LT_METHOD_LH5)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        FreeMem(buf, (ULONG)LT_REG_STRESS_SZ);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhCloseArchive(arc)) {
        FreeMem(buf, (ULONG)LT_REG_STRESS_SZ);
        lt_end(FALSE, (STRPTR)"close failed");
        return;
    }

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        FreeMem(buf, (ULONG)LT_REG_STRESS_SZ);
        lt_end(FALSE, (STRPTR)"reopen failed");
        return;
    }
    round_len = LhReadData(arc, (STRPTR)LT_REG_STRESS, &round);
    if (round_len != LT_REG_STRESS_SZ || round == NULL
            || memcmp(round, buf, (size_t)LT_REG_STRESS_SZ) != 0) {
        if (round_len < 0) {
            sprintf(detail, "read LhErr %ld", (long)LhErr());
        } else {
            sprintf(detail, "len=%ld mismatch", (long)round_len);
        }
        if (round != NULL) {
            FreeMem(round,
                (ULONG)round_len > 0 ? (ULONG)round_len : 1UL);
        }
        LhCloseArchive(arc);
        FreeMem(buf, (ULONG)LT_REG_STRESS_SZ);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(round, (ULONG)round_len);
    LhCloseArchive(arc);
    FreeMem(buf, (ULONG)LT_REG_STRESS_SZ);
    lt_end(TRUE, (STRPTR)"stress round-trip");
}

/*
 * All-identical bytes at Prefs/.info sizes.  Pre-fix lh5 second pass
 * spun forever once a match was chosen near EOF (CheckSys SET lockup).
 */
static VOID
lt_test_lh5_identical(VOID)
{
    struct LhArchive *arc;
    APTR round;
    UBYTE *buf;
    LONG round_len;
    LONG n;
    LONG s;
    LONG i;
    static const LONG sizes[] = {
        16L, 64L, 100L, 200L, 256L, 257L, 512L, 1024L, 4096L
    };
    char name[32];
    char detail[48];

    buf = NULL;
    round = NULL;
    n = (LONG)(sizeof(sizes) / sizeof(sizes[0]));

    lt_begin((STRPTR)"LhAddEntryA(lh5 identical)",
        (STRPTR)"all-0x55 sizes finish (EOF hang regression)");
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    lt_remove_path((STRPTR)LT_REG_ARC);

    buf = (UBYTE *)AllocMem(4096UL, MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL) {
        lt_end(FALSE, (STRPTR)"AllocMem failed");
        return;
    }
    memset(buf, 0x55, 4096);

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        FreeMem(buf, 4096UL);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    for (i = 0; i < n; i++) {
        s = sizes[i];
        sprintf(name, "id/%ld.bin", (long)s);
        if (!lt_add_method(arc, (STRPTR)name, (APTR)buf, s, LT_METHOD_LH5)) {
            sprintf(detail, "add %ld LhErr %ld", (long)s, (long)LhErr());
            LhCloseArchive(arc);
            FreeMem(buf, 4096UL);
            lt_end(FALSE, (STRPTR)detail);
            return;
        }
    }
    if (!LhCloseArchive(arc)) {
        FreeMem(buf, 4096UL);
        lt_end(FALSE, (STRPTR)"close failed");
        return;
    }

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        FreeMem(buf, 4096UL);
        lt_end(FALSE, (STRPTR)"reopen failed");
        return;
    }
    for (i = 0; i < n; i++) {
        s = sizes[i];
        sprintf(name, "id/%ld.bin", (long)s);
        round = NULL;
        round_len = LhReadData(arc, (STRPTR)name, &round);
        if (round_len != s || round == NULL
                || memcmp(round, buf, (size_t)s) != 0) {
            sprintf(detail, "sz=%ld len=%ld", (long)s, (long)round_len);
            if (round != NULL) {
                FreeMem(round,
                    (ULONG)round_len > 0 ? (ULONG)round_len : 1UL);
            }
            LhCloseArchive(arc);
            FreeMem(buf, 4096UL);
            lt_end(FALSE, (STRPTR)detail);
            return;
        }
        FreeMem(round, (ULONG)round_len);
    }
    LhCloseArchive(arc);
    FreeMem(buf, 4096UL);
    lt_end(TRUE, (STRPTR)"identical sizes ok");
}

/*
 * Many lh5+snapshot adds without close between (CheckSys Prefs.lha SET).
 */
static VOID
lt_test_lh5_many_snap(VOID)
{
    struct LhArchive *arc;
    struct LhSnapshot *sn;
    struct TagItem tags[4];
    UBYTE *buf;
    APTR round;
    LONG round_len;
    LONG i;
    LONG j;
    char name[40];
    char detail[48];

    buf = NULL;
    sn = NULL;
    round = NULL;

    lt_begin((STRPTR)"LhAddEntryA(lh5 many)",
        (STRPTR)"48x lh5+snapshot APPEND-style adds");
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    lt_remove_path((STRPTR)LT_REG_ARC);

    buf = (UBYTE *)AllocMem((ULONG)LT_REG_MANY_SZ, MEMF_PUBLIC | MEMF_CLEAR);
    sn = (struct LhSnapshot *)AllocMem((ULONG)sizeof(*sn),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (buf == NULL || sn == NULL) {
        if (buf != NULL) {
            FreeMem(buf, (ULONG)LT_REG_MANY_SZ);
        }
        if (sn != NULL) {
            FreeMem(sn, (ULONG)sizeof(*sn));
        }
        lt_end(FALSE, (STRPTR)"AllocMem failed");
        return;
    }
    /* Repetitive prefs-like body with a short distinct header. */
    memset(buf, 0xAA, (size_t)LT_REG_MANY_SZ);
    buf[0] = 'F';
    buf[1] = 'O';
    buf[2] = 'R';
    buf[3] = 'M';

    sn->lsn_Generation = 1UL;
    sn->lsn_Size = (ULONG)LT_REG_MANY_SZ;
    sn->lsn_Flags = 0;
    sn->lsn_Format = LH_SNAP_FORMAT_V1;
    for (j = 0; j < LH_SNAP_ID_LEN; j++) {
        sn->lsn_SnapId[j] = (UBYTE)(0x30 + j);
    }
    for (j = 0; j < LH_SNAP_SHA256_LEN; j++) {
        sn->lsn_SHA256[j] = (UBYTE)(0xC0 + (j & 0x0f));
    }
    strcpy(sn->lsn_Ver, "$VER: LhTest many 1.0");

    tags[0].ti_Tag = LHADD_Method;
    tags[0].ti_Data = (ULONG)LT_METHOD_LH5;
    tags[1].ti_Tag = LHADD_Snapshot;
    tags[1].ti_Data = (ULONG)sn;
    tags[2].ti_Tag = TAG_DONE;

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        FreeMem(buf, (ULONG)LT_REG_MANY_SZ);
        FreeMem(sn, (ULONG)sizeof(*sn));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    for (i = 0; i < LT_REG_MANY_N; i++) {
        sprintf(name, "Prefs/f%ld.bin", (long)i);
        sn->lsn_Generation = (ULONG)(i + 1);
        buf[4] = (UBYTE)(i & 0xff);
        if (!LhAddEntryA(arc, (STRPTR)name, (APTR)buf, LT_REG_MANY_SZ,
                tags)) {
            sprintf(detail, "i=%ld LhErr %ld", (long)i, (long)LhErr());
            LhCloseArchive(arc);
            FreeMem(buf, (ULONG)LT_REG_MANY_SZ);
            FreeMem(sn, (ULONG)sizeof(*sn));
            lt_end(FALSE, (STRPTR)detail);
            return;
        }
    }
    if (!LhCloseArchive(arc)) {
        FreeMem(buf, (ULONG)LT_REG_MANY_SZ);
        FreeMem(sn, (ULONG)sizeof(*sn));
        lt_end(FALSE, (STRPTR)"close failed");
        return;
    }

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        FreeMem(buf, (ULONG)LT_REG_MANY_SZ);
        FreeMem(sn, (ULONG)sizeof(*sn));
        lt_end(FALSE, (STRPTR)"reopen failed");
        return;
    }
    /* Spot-check first, middle, last. */
    {
        static const LONG check[] = { 0L, 24L, 47L };

        for (i = 0; i < 3; i++) {
            sprintf(name, "Prefs/f%ld.bin", (long)check[i]);
            buf[4] = (UBYTE)(check[i] & 0xff);
            round = NULL;
            round_len = LhReadData(arc, (STRPTR)name, &round);
            if (round_len != LT_REG_MANY_SZ || round == NULL
                    || memcmp(round, buf, (size_t)LT_REG_MANY_SZ) != 0) {
                sprintf(detail, "f%ld len=%ld", (long)check[i],
                    (long)round_len);
                if (round != NULL) {
                    FreeMem(round,
                        (ULONG)round_len > 0 ? (ULONG)round_len : 1UL);
                }
                LhCloseArchive(arc);
                FreeMem(buf, (ULONG)LT_REG_MANY_SZ);
                FreeMem(sn, (ULONG)sizeof(*sn));
                lt_end(FALSE, (STRPTR)detail);
                return;
            }
            FreeMem(round, (ULONG)round_len);
        }
    }
    LhCloseArchive(arc);
    FreeMem(buf, (ULONG)LT_REG_MANY_SZ);
    FreeMem(sn, (ULONG)sizeof(*sn));
    lt_end(TRUE, (STRPTR)"48 adds ok");
}

/*
 * Delete while a writer is open (CheckPoint drop-slot path).  Must not
 * deadlock; later adds and close must succeed; deleted member stays gone.
 */
static VOID
lt_test_delete_while_writing(VOID)
{
    struct LhArchive *arc;
    APTR round;
    LONG round_len;
    static UBYTE pa[] = "del-a";
    static UBYTE pb[] = "del-b-payload";
    static UBYTE pc[] = "del-c-after";
    char detail[48];

    round = NULL;

    lt_begin((STRPTR)"LhDeleteFile(while write)",
        (STRPTR)"delete mid-write then add + close");
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    lt_remove_path((STRPTR)LT_REG_ARC);

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!lt_add_store(arc, (STRPTR)LT_REG_DEL_A, (APTR)pa,
            (LONG)(sizeof(pa) - 1))) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!lt_add_store(arc, (STRPTR)LT_REG_DEL_B, (APTR)pb,
            (LONG)(sizeof(pb) - 1))) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    /* Writer still open — this used to deadlock / corrupt. */
    if (!LhDeleteFile(arc, (STRPTR)LT_REG_DEL_A)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!lt_add_store(arc, (STRPTR)LT_REG_DEL_C, (APTR)pc,
            (LONG)(sizeof(pc) - 1))) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhCloseArchive(arc)) {
        lt_end(FALSE, (STRPTR)"close after delete failed");
        return;
    }

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"reopen failed");
        return;
    }
    round_len = LhReadData(arc, (STRPTR)LT_REG_DEL_A, &round);
    if (round_len >= 0 && round != NULL) {
        FreeMem(round, (ULONG)round_len > 0 ? (ULONG)round_len : 1UL);
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"A still present");
        return;
    }
    round = NULL;
    round_len = LhReadData(arc, (STRPTR)LT_REG_DEL_B, &round);
    if (round_len != (LONG)(sizeof(pb) - 1) || round == NULL
            || memcmp(round, pb, (size_t)(sizeof(pb) - 1)) != 0) {
        sprintf(detail, "B len=%ld", (long)round_len);
        if (round != NULL) {
            FreeMem(round, (ULONG)round_len > 0 ? (ULONG)round_len : 1UL);
        }
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(round, (ULONG)round_len);
    round = NULL;
    round_len = LhReadData(arc, (STRPTR)LT_REG_DEL_C, &round);
    if (round_len != (LONG)(sizeof(pc) - 1) || round == NULL
            || memcmp(round, pc, (size_t)(sizeof(pc) - 1)) != 0) {
        sprintf(detail, "C len=%ld", (long)round_len);
        if (round != NULL) {
            FreeMem(round, (ULONG)round_len > 0 ? (ULONG)round_len : 1UL);
        }
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(round, (ULONG)round_len);
    LhCloseArchive(arc);
    lt_end(TRUE, (STRPTR)"A gone; B+C ok");
}

/*
 * CheckPoint SET path: LHADD_Method lh5 + LHADD_Snapshot on one member.
 */
static VOID
lt_test_lh5_snapshot(VOID)
{
    struct LhArchive *arc;
    struct LhSnapshot *sn;
    struct LhSnapshot *got;
    struct TagItem tags[4];
    static UBYTE payload[] = "lh5-snap-body";
    LONG i;
    char detail[64];

    sn = NULL;
    got = NULL;

    lt_begin((STRPTR)"LhAddEntryA(lh5+snap)",
        (STRPTR)"lh5 method + Snapshot round-trip");
    if (!lt_ensure_dir((STRPTR)LT_RAMDIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    lt_remove_path((STRPTR)LT_REG_ARC);

    sn = (struct LhSnapshot *)AllocMem((ULONG)sizeof(*sn),
        MEMF_PUBLIC | MEMF_CLEAR);
    got = (struct LhSnapshot *)AllocMem((ULONG)sizeof(*got),
        MEMF_PUBLIC | MEMF_CLEAR);
    if (sn == NULL || got == NULL) {
        if (sn != NULL) {
            FreeMem(sn, (ULONG)sizeof(*sn));
        }
        if (got != NULL) {
            FreeMem(got, (ULONG)sizeof(*got));
        }
        lt_end(FALSE, (STRPTR)"AllocMem snapshot failed");
        return;
    }

    sn->lsn_Generation = 7UL;
    sn->lsn_Size = (ULONG)(sizeof(payload) - 1);
    sn->lsn_Flags = 0;
    sn->lsn_Format = LH_SNAP_FORMAT_V1;
    for (i = 0; i < LH_SNAP_ID_LEN; i++) {
        sn->lsn_SnapId[i] = (UBYTE)(0x20 + i);
    }
    for (i = 0; i < LH_SNAP_SHA256_LEN; i++) {
        sn->lsn_SHA256[i] = (UBYTE)(0xB0 + (i & 0x0f));
    }
    strcpy(sn->lsn_Ver, "$VER: LhTest lh5 1.0");

    tags[0].ti_Tag = LHADD_Method;
    tags[0].ti_Data = (ULONG)LT_METHOD_LH5;
    tags[1].ti_Tag = LHADD_Snapshot;
    tags[1].ti_Data = (ULONG)sn;
    tags[2].ti_Tag = TAG_DONE;

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_WRITE);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhAddEntryA(arc, (STRPTR)LT_REG_SNAP, payload,
            (LONG)(sizeof(payload) - 1), tags)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (!LhCloseArchive(arc)) {
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)"close failed");
        return;
    }

    arc = LhOpenArchive((STRPTR)LT_REG_ARC, LHARC_MODE_READ);
    if (arc == NULL) {
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)"reopen failed");
        return;
    }
    memset(got, 0, sizeof(*got));
    if (!LhGetSnapshot(arc, (STRPTR)LT_REG_SNAP, got)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if (got->lsn_Generation != sn->lsn_Generation
            || got->lsn_Size != sn->lsn_Size
            || got->lsn_Format != sn->lsn_Format
            || memcmp(got->lsn_SnapId, sn->lsn_SnapId,
                (size_t)LH_SNAP_ID_LEN) != 0
            || memcmp(got->lsn_SHA256, sn->lsn_SHA256,
                (size_t)LH_SNAP_SHA256_LEN) != 0
            || strcmp(got->lsn_Ver, sn->lsn_Ver) != 0) {
        LhCloseArchive(arc);
        FreeMem(sn, (ULONG)sizeof(*sn));
        FreeMem(got, (ULONG)sizeof(*got));
        lt_end(FALSE, (STRPTR)"snapshot mismatch");
        return;
    }
    LhCloseArchive(arc);
    FreeMem(sn, (ULONG)sizeof(*sn));
    FreeMem(got, (ULONG)sizeof(*got));
    lt_end(TRUE, (STRPTR)"lh5+snapshot fields");
}

static VOID
lt_test_fixture_create(VOID)
{
    ULONG len_a;
    ULONG len_b;

    len_a = (ULONG)strlen((const char *)lt_plain_a);
    len_b = (ULONG)strlen((const char *)lt_plain_b);
    Printf("LhTest: RUN  fixture create\n");
    Printf("LhTest:   expect: archive written\n");
    Flush(Output());
    lt_got_fixture = lt_write_archive((STRPTR)LT_FIXTURE,
        (STRPTR)LT_ENTRY_A, lt_plain_a, len_a,
        (STRPTR)LT_ENTRY_B, lt_plain_b, len_b);
    if (lt_got_fixture) {
        lt_pass++;
        Printf("LhTest: PASS fixture create\n");
        Printf("LhTest:   actual: %s\n", LT_FIXTURE);
    } else {
        lt_fail++;
        Printf("LhTest: FAIL fixture create\n");
        Printf("LhTest:   actual: write failed\n");
    }
    Flush(Output());
}

static VOID
lt_test_openarchive_read(STRPTR path)
{
    struct LhArchive *arc;
    char detail[40];

    lt_begin((STRPTR)"LhOpenArchive(read)", (STRPTR)"non-NULL archive");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, path);

    lt_begin((STRPTR)"LhCloseArchive(read)", (STRPTR)"DOSTRUE");
    LhCloseArchive(arc);
    lt_end(TRUE, (STRPTR)"DOSTRUE");
}

static VOID
lt_test_lock_examine(STRPTR path, LONG expect_count)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG count;
    char detail[48];
    char expect[48];

    lt_begin((STRPTR)"LhLock", (STRPTR)"non-NULL root lock");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"AllocMem failed");
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, (STRPTR)"root lock");

    lt_begin((STRPTR)"LhExamine", (STRPTR)"first entry FIB filled");
    count = 0;
    ok = LhExamine(lock, fib);
    if (!ok) {
        LhUnLock(lock);
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"DOSFALSE");
        return;
    }
    lt_end(TRUE, fib->fib_FileName);

    /*
     * Root Examine returns the directory FIB (empty name); children come
     * from ExNext only - same as dos.library on a real directory lock.
     */
    lt_begin((STRPTR)"LhExNext", (STRPTR)"walk remaining entries");
    while (LhExNext(lock, fib)) {
        count++;
    }
    sprintf(detail, "walked to %ld", (long)count);
    lt_end(TRUE, (STRPTR)detail);

    lt_begin((STRPTR)"LhUnLock", (STRPTR)"DOSTRUE");
    LhUnLock(lock);
    lt_end(TRUE, (STRPTR)"DOSTRUE");
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);

    sprintf(expect, "%ld entries", (long)expect_count);
    lt_begin((STRPTR)"LhExamine/LhExNext count", (STRPTR)expect);
    sprintf(detail, "%ld entries", (long)count);
    if (count == expect_count) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_exall(STRPTR path, LONG expect_count)
{
    struct LhArchive *arc;
    BPTR lock;
    struct ExAllControl ctl;
    UBYTE buf[4096];
    LONG total;
    char detail[48];
    char expect[48];

    sprintf(expect, "%ld entries (ED_NAME)", (long)expect_count);
    lt_begin((STRPTR)"LhExAll", (STRPTR)expect);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"lock failed");
        return;
    }
    memset(&ctl, 0, sizeof(ctl));
    ctl.eac_LastKey = 0;
    total = 0;
    for (;;) {
        if (!LhExAll(lock, (STRPTR)buf, (LONG)sizeof(buf), ED_NAME, &ctl)) {
            total += (LONG)ctl.eac_Entries;
            break;
        }
        total += (LONG)ctl.eac_Entries;
    }
    sprintf(detail, "%ld entries", (long)total);
    if (total == expect_count) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }

    lt_begin((STRPTR)"LhExAllEnd", (STRPTR)"DOSTRUE");
    LhExAllEnd(lock);
    lt_end(TRUE, (STRPTR)"DOSTRUE");
    LhUnLock(lock);
    LhCloseArchive(arc);
}

static VOID
lt_test_lock_entry(STRPTR path, STRPTR name)
{
    struct LhArchive *arc;
    BPTR lock;
    char label[48];
    char expect[64];

    sprintf(label, "LhLock %s", (const char *)name);
    sprintf(expect, "non-NULL lock for %s", (const char *)name);
    lt_begin((STRPTR)label, (STRPTR)expect);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    lock = LhLock(arc, name);
    if (lock == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"NULL lock");
        return;
    }
    lt_end(TRUE, (STRPTR)"non-NULL lock");
    LhUnLock(lock);
    LhCloseArchive(arc);
}

static VOID
lt_test_exall_size(STRPTR path, LONG expect_count)
{
    struct LhArchive *arc;
    BPTR lock;
    struct ExAllControl ctl;
    UBYTE buf[4096];
    LONG total;
    char detail[48];
    char expect[48];

    sprintf(expect, "%ld entries (ED_SIZE)", (long)expect_count);
    lt_begin((STRPTR)"LhExAll(ED_SIZE)", (STRPTR)expect);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"lock failed");
        return;
    }
    memset(&ctl, 0, sizeof(ctl));
    ctl.eac_LastKey = 0;
    total = 0;
    for (;;) {
        if (!LhExAll(lock, (STRPTR)buf, (LONG)sizeof(buf), ED_SIZE, &ctl)) {
            total += (LONG)ctl.eac_Entries;
            break;
        }
        total += (LONG)ctl.eac_Entries;
    }
    LhExAllEnd(lock);
    LhUnLock(lock);
    LhCloseArchive(arc);
    sprintf(detail, "%ld entries", (long)total);
    if (total == expect_count) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_seek_end(STRPTR path, STRPTR name, ULONG expect_len)
{
    struct LhArchive *arc;
    BPTR fh;
    LONG esz;
    char detail[48];
    char expect[48];

    sprintf(expect, "pos=%lu", (unsigned long)expect_len);
    lt_begin((STRPTR)"LhSeek(OFFSET_END)", (STRPTR)expect);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    fh = LhOpen(arc, name, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"LhOpen failed");
        return;
    }
    esz = LhSeek(fh, 0L, OFFSET_END);
    LhClose(fh);
    LhCloseArchive(arc);
    sprintf(detail, "pos=%ld", (long)esz);
    if (esz == (LONG)expect_len) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_info(STRPTR path)
{
    struct LhArchive *arc;
    BPTR lock;
    struct InfoData info;
    char detail[48];

    lt_begin((STRPTR)"LhInfo", (STRPTR)"DOSTRUE, NumBlocksUsed > 0");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"lock failed");
        return;
    }
    memset(&info, 0, sizeof(info));
    if (LhInfo(lock, &info) && info.id_NumBlocksUsed > 0) {
        sprintf(detail, "NumBlocksUsed=%ld", (long)info.id_NumBlocksUsed);
        lt_end(TRUE, (STRPTR)detail);
    } else {
        sprintf(detail, "NumBlocksUsed=%ld", (long)info.id_NumBlocksUsed);
        lt_end(FALSE, (STRPTR)detail);
    }
    LhUnLock(lock);
    LhCloseArchive(arc);
}

static VOID
lt_test_openfromlock(STRPTR path, STRPTR name, ULONG expect_len)
{
    struct LhArchive *arc;
    BPTR lock;
    BPTR fh;
    char namebuf[256];
    UBYTE buf[256];
    LONG n;
    LONG total;
    LONG wrote;
    char detail[64];
    char expect[64];

    lt_begin((STRPTR)"LhNameFromLock", name);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    lock = LhLock(arc, name);
    if (lock == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"lock failed");
        return;
    }
    namebuf[0] = '\0';
    if (LhNameFromLock(lock, (STRPTR)namebuf, (LONG)sizeof(namebuf)) <= 0) {
        LhUnLock(lock);
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"empty name");
        return;
    }
    if (strcmp((const char *)namebuf, (const char *)name) != 0) {
        LhUnLock(lock);
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)namebuf);
        return;
    }
    lt_end(TRUE, (STRPTR)namebuf);

    lt_begin((STRPTR)"LhOpenFromLock", (STRPTR)"non-NULL file handle");
    fh = LhOpenFromLock(lock, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        LhUnLock(lock);
        LhCloseArchive(arc);
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, name);

    sprintf(expect, "read %lu bytes", (unsigned long)expect_len);
    lt_begin((STRPTR)"LhRead", (STRPTR)expect);
    total = 0;
    for (;;) {
        n = LhRead(fh, buf, (LONG)sizeof(buf));
        if (n < 0) {
            LhClose(fh);
            LhUnLock(lock);
            LhCloseArchive(arc);
            lt_end(FALSE, (STRPTR)"read error");
            return;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    sprintf(detail, "read=%ld", (long)total);
    if (total == (LONG)expect_len) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }

    lt_begin((STRPTR)"LhSeek", (STRPTR)"pos=0");
    if (LhSeek(fh, 0L, OFFSET_BEGINNING) != 0) {
        LhClose(fh);
        LhUnLock(lock);
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"seek failed");
        return;
    }
    lt_end(TRUE, (STRPTR)"pos=0");

    lt_begin((STRPTR)"LhWrite(read-only)", (STRPTR)"rejected (< 0)");
    wrote = LhWrite(fh, buf, (LONG)sizeof(buf));
    if (wrote >= 0) {
        LhClose(fh);
        LhUnLock(lock);
        LhCloseArchive(arc);
        sprintf(detail, "wrote=%ld", (long)wrote);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    sprintf(detail, "rc=%ld", (long)wrote);
    lt_end(TRUE, (STRPTR)detail);

    lt_begin((STRPTR)"LhClose", (STRPTR)"DOSTRUE");
    LhClose(fh);
    lt_end(TRUE, (STRPTR)"DOSTRUE");
    LhUnLock(lock);
    LhCloseArchive(arc);

    sprintf(expect, "read=%lu", (unsigned long)expect_len);
    lt_begin((STRPTR)"LhOpenFromLock roundtrip", (STRPTR)expect);
    sprintf(detail, "read=%ld", (long)total);
    if (total == (LONG)expect_len) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_open_handle(STRPTR path, STRPTR name, ULONG expect_len)
{
    struct LhArchive *arc;
    BPTR fh;
    UBYTE buf[256];
    LONG n;
    LONG total;
    LONG esz;
    char detail[64];
    char expect[64];

    lt_begin((STRPTR)"LhOpen", (STRPTR)"non-NULL file handle");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open archive failed");
        return;
    }
    fh = LhOpen(arc, name, MODE_OLDFILE);
    if (fh == (BPTR)NULL) {
        LhCloseArchive(arc);
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, name);

    sprintf(expect, "read=%lu seek=%lu", (unsigned long)expect_len,
        (unsigned long)expect_len);
    lt_begin((STRPTR)"LhOpen/LhSeek/LhRead/LhClose", (STRPTR)expect);
    esz = LhSeek(fh, 0L, OFFSET_END);
    LhSeek(fh, 0L, OFFSET_BEGINNING);
    total = 0;
    for (;;) {
        n = LhRead(fh, buf, (LONG)sizeof(buf));
        if (n < 0) {
            LhClose(fh);
            LhCloseArchive(arc);
            lt_end(FALSE, (STRPTR)"read error");
            return;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    LhClose(fh);
    LhCloseArchive(arc);
    sprintf(detail, "read=%ld seek=%ld", (long)total, (long)esz);
    if (total == (LONG)expect_len && esz == (LONG)expect_len) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_setpassword(STRPTR path)
{
    struct LhArchive *arc;

    lt_begin((STRPTR)"LhSetPassword", (STRPTR)"DOSTRUE");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    if (LhSetPassword(arc, (STRPTR)"LhTestSecret")) {
        lt_end(TRUE, (STRPTR)"DOSTRUE");
    } else {
        lt_end(FALSE, (STRPTR)"DOSFALSE");
    }
    LhSetPassword(arc, (STRPTR)"");
    LhCloseArchive(arc);
}

static VOID
lt_test_read_data(STRPTR path, STRPTR name, STRPTR expect, ULONG expect_len)
{
    struct LhArchive *arc;
    APTR data;
    LONG len;
    char label[64];
    char detail[40];
    char want[48];

    sprintf(label, "LhReadData %s", (const char *)name);
    sprintf(want, "len=%lu content match", (unsigned long)expect_len);
    lt_begin((STRPTR)label, (STRPTR)want);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    data = NULL;
    len = LhReadData(arc, name, &data);
    if (len < 0) {
        LhCloseArchive(arc);
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    if ((ULONG)len != expect_len || data == NULL
        || memcmp(data, expect, expect_len) != 0) {
        if (data != NULL) {
            FreeMem(data, (ULONG)len);
        }
        LhCloseArchive(arc);
        sprintf(detail, "len=%ld content mismatch", (long)len);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    FreeMem(data, (ULONG)len);
    LhCloseArchive(arc);
    sprintf(detail, "len=%ld match", (long)len);
    lt_end(TRUE, (STRPTR)detail);
}

static VOID
lt_test_testentry(STRPTR path, STRPTR name, ULONG expect_len)
{
    struct LhArchive *arc;
    LONG len;
    char label[64];
    char detail[48];
    char want[48];

    sprintf(label, "LhTestEntry %s", (const char *)name);
    sprintf(want, "len=%lu", (unsigned long)expect_len);
    lt_begin((STRPTR)label, (STRPTR)want);
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    len = LhTestEntry(arc, name);
    LhCloseArchive(arc);
    sprintf(detail, "len=%ld", (long)len);
    if (len == (LONG)expect_len) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_printentry(STRPTR path, STRPTR name)
{
    struct LhArchive *arc;
    char label[64];

    sprintf(label, "LhPrintEntry %s", (const char *)name);
    lt_begin((STRPTR)label, (STRPTR)"DOSTRUE (payload to Output)");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    if (LhPrintEntry(arc, name)) {
        /*
         * LhPrintEntry writes the raw payload to Output with no trailing
         * newline; emit one so the PASS line is not glued to the text.
         */
        Printf("\n");
        Flush(Output());
        lt_end(TRUE, (STRPTR)"DOSTRUE");
    } else {
        lt_end(FALSE, (STRPTR)"DOSFALSE");
    }
    LhCloseArchive(arc);
}

static BOOL
lt_file_size(STRPTR path, LONG *size_out)
{
    BPTR lock;
    struct FileInfoBlock *fib;
    BOOL ok;

    ok = FALSE;
    lock = Lock(path, ACCESS_READ);
    if (lock == (BPTR)NULL) {
        return FALSE;
    }
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        UnLock(lock);
        return FALSE;
    }
    if (Examine(lock, fib)) {
        *size_out = fib->fib_Size;
        ok = TRUE;
    }
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    UnLock(lock);
    return ok;
}

static VOID
lt_test_extract(STRPTR arc_path, STRPTR entry, ULONG expect_len)
{
    struct LhArchive *arc;
    char outpath[256];
    LONG size;
    char label[64];
    char detail[48];

    sprintf(label, "LhExtractEntry %s", (const char *)entry);
    sprintf(detail, "file size=%lu", (unsigned long)expect_len);
    lt_begin((STRPTR)label, (STRPTR)detail);
    if (!lt_ensure_dir((STRPTR)LT_EXTRACT_DIR)) {
        lt_end(FALSE, (STRPTR)"mkdir failed");
        return;
    }
    sprintf(outpath, "%s/%s", LT_EXTRACT_DIR, (const char *)entry);
    /* Prior runs may have left write/delete-protected extracts (bad attrs). */
    SetProtection((STRPTR)outpath, 0);
    DeleteFile((STRPTR)outpath);

    arc = LhOpenArchive(arc_path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    if (!LhExtractEntry(arc, entry, (STRPTR)outpath)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    LhCloseArchive(arc);

    size = 0;
    if (lt_file_size((STRPTR)outpath, &size) && size == (LONG)expect_len) {
        sprintf(detail, "size=%ld", (long)size);
        lt_end(TRUE, (STRPTR)detail);
    } else {
        sprintf(detail, "size=%ld", (long)size);
        lt_end(FALSE, (STRPTR)detail);
    }
}

static BOOL
lt_build_single_entry(STRPTR path, STRPTR name, STRPTR data, ULONG len)
{
    struct LhArchive *arc;

    if (!lt_ensure_parent(path)) {
        return FALSE;
    }
    lt_remove_path(path);
    arc = LhOpenArchive(path, LHARC_MODE_WRITE);
    if (arc == NULL) {
        return FALSE;
    }
    if (!lt_add_store(arc, name, (APTR)data, (LONG)len)) {
        LhCloseArchive(arc);
        return FALSE;
    }
    LhCloseArchive(arc);
    return TRUE;
}

static BOOL
lt_test_mutate(STRPTR path)
{
    struct LhArchive *arc;
    ULONG len_c;
    ULONG len_cat;
    LONG count;
    char detail[48];

    lt_mutate_ok = FALSE;
    len_c = (ULONG)strlen((const char *)lt_plain_c);
    len_cat = (ULONG)strlen((const char *)lt_plain_cat);

    lt_begin((STRPTR)"concat donor create", (STRPTR)"archive written");
    if (!lt_build_single_entry((STRPTR)LT_FIXTURE2,
            (STRPTR)LT_CONCAT_ENTRY, lt_plain_cat, len_cat)) {
        lt_end(FALSE, (STRPTR)LT_FIXTURE2);
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)LT_FIXTURE2);

    lt_begin((STRPTR)"LhOpenArchive(append)", (STRPTR)"non-NULL archive");
    arc = LhOpenArchive(path, LHARC_MODE_APPEND);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, path);

    lt_begin((STRPTR)"LhAddEntryA(append)", (STRPTR)"DOSTRUE for LtExtra.txt");
    if (!lt_add_store(arc, (STRPTR)LT_ENTRY_C, (APTR)lt_plain_c, (LONG)len_c)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)LT_ENTRY_C);

    lt_begin((STRPTR)"LhConcatArchive", (STRPTR)"DOSTRUE");
    if (!LhConcatArchive(arc, (STRPTR)LT_FIXTURE2)) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)detail);
        return FALSE;
    }
    lt_end(TRUE, (STRPTR)LT_FIXTURE2);
    LhCloseArchive(arc);

    lt_begin((STRPTR)"post-append count", (STRPTR)"4 entries");
    count = 0;
    if (lt_count_entries(path, &count) && count == 4) {
        sprintf(detail, "%ld entries", (long)count);
        lt_end(TRUE, (STRPTR)detail);
    } else {
        sprintf(detail, "%ld entries", (long)count);
        lt_end(FALSE, (STRPTR)detail);
    }

    lt_begin((STRPTR)"LhDeleteFile", (STRPTR)"DOSTRUE for lhtest.bin");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return FALSE;
    }
    if (LhDeleteFile(arc, (STRPTR)LT_ENTRY_B)) {
        lt_end(TRUE, (STRPTR)LT_ENTRY_B);
    } else {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
    }
    LhCloseArchive(arc);

    lt_begin((STRPTR)"post-delete count", (STRPTR)"3 entries");
    count = 0;
    if (lt_count_entries(path, &count) && count == 3) {
        sprintf(detail, "%ld entries", (long)count);
        lt_end(TRUE, (STRPTR)detail);
    } else {
        sprintf(detail, "%ld entries", (long)count);
        lt_end(FALSE, (STRPTR)detail);
    }
    lt_mutate_ok = TRUE;
    return TRUE;
}

static VOID
lt_run_post_mutate_verify(VOID)
{
    struct LhArchive *arc;
    LONG len;
    ULONG len_a;
    ULONG len_c;
    ULONG len_cat;

    len_a = (ULONG)strlen((const char *)lt_plain_a);
    len_c = (ULONG)strlen((const char *)lt_plain_c);
    len_cat = (ULONG)strlen((const char *)lt_plain_cat);

    lt_test_lock_examine((STRPTR)LT_FIXTURE, 3);
    lt_test_read_data((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, lt_plain_a, len_a);
    lt_test_read_data((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_C, lt_plain_c, len_c);
    lt_test_read_data((STRPTR)LT_FIXTURE, (STRPTR)LT_CONCAT_ENTRY,
        lt_plain_cat, len_cat);
    lt_test_extract((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_C, len_c);
    lt_test_printentry((STRPTR)LT_FIXTURE, (STRPTR)LT_CONCAT_ENTRY);

    lt_begin((STRPTR)"post-delete LhTestEntry B", (STRPTR)"absent (len < 0)");
    arc = LhOpenArchive((STRPTR)LT_FIXTURE, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    len = LhTestEntry(arc, (STRPTR)LT_ENTRY_B);
    LhCloseArchive(arc);
    if (len < 0) {
        lt_end(TRUE, (STRPTR)"absent");
    } else {
        lt_end(FALSE, (STRPTR)"still present");
    }
}

static VOID
lt_test_lherr(VOID)
{
    struct LhArchive *arc;
    char detail[40];

    lt_begin((STRPTR)"LhErr", (STRPTR)"non-zero after failed open");
    arc = LhOpenArchive((STRPTR)"RAM:NoSuchArchive_999.lha", LHARC_MODE_READ);
    if (arc == NULL && LhErr() != 0) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(TRUE, (STRPTR)detail);
    } else {
        if (arc != NULL) {
            LhCloseArchive(arc);
        }
        sprintf(detail, "LhErr %ld", (long)LhErr());
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_test_negative(VOID)
{
    struct LhArchive *arc;
    LONG len;
    char detail[32];

    if (!lt_got_fixture) {
        Printf("LhTest: SKIP neg/missing entry (no fixture)\n");
        Flush(Output());
        return;
    }
    lt_begin((STRPTR)"neg/missing entry", (STRPTR)"LhTestEntry returns < 0");
    arc = LhOpenArchive((STRPTR)LT_FIXTURE, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_end(FALSE, (STRPTR)"open failed");
        return;
    }
    len = LhTestEntry(arc, (STRPTR)"NoSuchEntry.dat");
    LhCloseArchive(arc);
    sprintf(detail, "len=%ld", (long)len);
    if (len < 0) {
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)detail);
    }
}

static VOID
lt_run_read_apis(VOID)
{
    ULONG len_a;
    ULONG len_b;

    len_a = (ULONG)strlen((const char *)lt_plain_a);
    len_b = (ULONG)strlen((const char *)lt_plain_b);

    lt_test_openarchive_read((STRPTR)LT_FIXTURE);
    lt_test_lock_examine((STRPTR)LT_FIXTURE, 2);
    lt_test_lock_entry((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_B);
    lt_test_exall((STRPTR)LT_FIXTURE, 2);
    lt_test_exall_size((STRPTR)LT_FIXTURE, 2);
    lt_test_info((STRPTR)LT_FIXTURE);
    lt_test_openfromlock((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, len_a);
    lt_test_seek_end((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, len_a);
    lt_test_open_handle((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, len_a);
    lt_test_setpassword((STRPTR)LT_FIXTURE);
    lt_test_read_data((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, lt_plain_a, len_a);
    lt_test_read_data((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_B, lt_plain_b, len_b);
    lt_test_testentry((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, len_a);
    lt_test_testentry((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_B, len_b);
    lt_test_printentry((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A);
    lt_test_printentry((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_B);
    lt_test_extract((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_A, len_a);
    lt_test_extract((STRPTR)LT_FIXTURE, (STRPTR)LT_ENTRY_B, len_b);
}

static VOID
lt_test_external(STRPTR path)
{
    struct LhArchive *arc;
    BPTR lock;
    struct FileInfoBlock *fib;
    LONG ok;
    LONG count;
    char detail[80];

    Printf("LhTest: external archive %s\n", path);
    Flush(Output());

    lt_begin((STRPTR)"external open", (STRPTR)"non-NULL archive");
    arc = LhOpenArchive(path, LHARC_MODE_READ);
    if (arc == NULL) {
        lt_lherr_detail(detail, (LONG)sizeof(detail));
        lt_end(FALSE, (STRPTR)detail);
        return;
    }
    lt_end(TRUE, path);

    lt_begin((STRPTR)"external lock", (STRPTR)"non-NULL root lock");
    fib = (struct FileInfoBlock *)AllocMem(
        (ULONG)sizeof(struct FileInfoBlock), MEMF_PUBLIC | MEMF_CLEAR);
    if (fib == NULL) {
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"AllocMem failed");
        return;
    }
    lock = LhLock(arc, (STRPTR)"");
    if (lock == (BPTR)NULL) {
        FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
        LhCloseArchive(arc);
        lt_end(FALSE, (STRPTR)"NULL lock");
        return;
    }
    lt_end(TRUE, (STRPTR)"root lock");

    lt_begin((STRPTR)"external LhTestEntry",
        (STRPTR)"each entry size matches FIB");
    count = 0;
    ok = LhExamine(lock, fib);
    while (ok) {
        LONG tlen;

        tlen = LhTestEntry(arc, fib->fib_FileName);
        if (tlen != fib->fib_Size) {
            sprintf(detail, "%s len=%ld size=%ld",
                fib->fib_FileName, (long)tlen, (long)fib->fib_Size);
            lt_end(FALSE, (STRPTR)detail);
            LhUnLock(lock);
            FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
            LhCloseArchive(arc);
            return;
        }
        count++;
        ok = LhExNext(lock, fib);
    }
    LhUnLock(lock);
    FreeMem(fib, (ULONG)sizeof(struct FileInfoBlock));
    LhCloseArchive(arc);
    if (count > 0) {
        sprintf(detail, "%ld entries", (long)count);
        lt_end(TRUE, (STRPTR)detail);
    } else {
        lt_end(FALSE, (STRPTR)"empty");
    }
}

static VOID
lt_print_archive_skip(VOID)
{
    Printf("LhTest: SKIP archive LVOs (fixture create failed):\n");
    Printf("  LhOpenArchive LhCloseArchive LhLock LhUnLock LhExamine LhExNext\n");
    Printf("  LhExAll LhExAllEnd LhInfo LhOpenFromLock LhOpen LhRead LhWrite\n");
    Printf("  LhClose LhSeek LhNameFromLock LhAddEntryA\n");
    Printf("  LhDeleteFile LhConcatArchive LhSetPassword LhReadData\n");
    Printf("  LhExtractEntry LhTestEntry LhPrintEntry LhErr LhGetSnapshot\n");
    Flush(Output());
}

int
main(int argc, char **argv)
{
    lt_pass = 0;
    lt_fail = 0;
    lt_got_fixture = FALSE;
    lt_mutate_ok = FALSE;

    Printf("LhTest: lh.library API harness\n");
    lt_print_api_plan();

    if (!lt_test_open()) {
        Printf("LhTest: %lu passed, %lu failed (library missing)\n",
            lt_pass, lt_fail);
        Flush(Output());
        return 20;
    }

    lt_test_classic_buffer();
    lt_test_codec_lh0();
    lt_test_addentry_default();
    lt_test_snapshot();
    lt_test_charset_comment();
    lt_test_lh5_add_roundtrip();
    lt_test_lh5_hash_stress();
    lt_test_lh5_identical();
    lt_test_lh5_many_snap();
    lt_test_delete_while_writing();
    lt_test_lh5_snapshot();
    lt_test_fixture_create();

    if (lt_got_fixture) {
        lt_run_read_apis();
        if (lt_test_mutate((STRPTR)LT_FIXTURE) && lt_mutate_ok) {
            lt_run_post_mutate_verify();
        }
    } else {
        lt_print_archive_skip();
    }

    lt_test_lherr();
    lt_test_negative();

    if (argc > 1 && argv[1] != NULL) {
        lt_test_external((STRPTR)argv[1]);
    }

    CloseLibrary(LhBase);
    LhBase = NULL;

    Printf("LhTest: %lu passed, %lu failed\n", lt_pass, lt_fail);
    Flush(Output());

    if (lt_fail > 0) {
        return 10;
    }
    return 0;
}
