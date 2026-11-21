/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 amigazen project
 *
 * lhasa_cmdhelp.h - Print AmigaDOS command help from HELP: drawers.
 */

#ifndef LHASA_CMDHELP_H
#define LHASA_CMDHELP_H

#include <exec/types.h>

/*
 * Print the full text of <cmd>.help to the current output (not short usage).
 * Tries HELP:<lang>/LhASsA/ then HELP:english/LhASsA/, then SYS:Help/...
 * Returns 1 if the file was printed, 0 if not found (error already printed).
 */
LONG lhasa_print_cmd_help(STRPTR cmd_name);

#endif /* LHASA_CMDHELP_H */
