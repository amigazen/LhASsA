# LhASsA

**Lh Archive System services for Amiga**

This is **LhASsA** - an open source implementation of LhA as a system service for Amiga computers. 

## [amigazen project](http://www.amigazen.com)

*A web, suddenly*

*Forty years meditation*

*Minds awaken, free*

**amigazen project** uses modern software development tools and methods to update and rerelease classic Amiga open source software. Projects include a new AWeb, Amiga Python 2, and the ToolKit project - a universal SDK for Amiga.

Key to the amigazen project approach is ensuring every project can be built with the same common set of development tools and configurations, so the ToolKit project was created to provide a standard configuration for Amiga development. All *amigazen project* releases are guaranteed to build against the ToolKit standard so that anyone can download and begin contributing straightaway without having to tailor the toolchain for their own setup.

Stefan Boberg, Jim Cooper, David Tritscher, and Sven Ottemann (authors of the classic Amiga LhA releases) are not affiliated with amigazen project. LhA was published as freeware/shareware for AmigaOS from 1991 onward; this tree is an independent reimplementation written using archive format documentation, not a derivative of the original LhA sources or any other implementation.

The amigazen project philosophy is based on openness:

*Open* to anyone and everyone  -  *Open* source and free for all  -  *Open* your mind and create!

PRs for all projects are gratefully received at [GitHub](https://github.com/amigazen/). While the focus now is on classic 68k software, it is intended that all amigazen project releases can be ported to other Amiga-like systems including AROS and MorphOS where feasible.

LhASsA is distributed under the [BSD 2-Clause License](LICENSE.md).

## About LhASsA

**LhA** (`.lzh` / `.lha`) is the de facto standard archive format on Amiga platforms from the late 1980s through the 1990s until today. Archives use level 0-2 headers, per-file CRC16 checks, and a family of **lh*** compression methods (lh0 store, lh1-lh7, lzs, lz4, lz5, and others).

**LhASsA** is a brand new implementation of LhA for Amiga, providing not only a command line archiving tool, but a suite of services that integrate LhA archives as a first class component of the Amiga operating system. LhASsA includes a full set of system services, as well as an SDK, consisting of:

- **lh.library** - the core of LhASsA is lh.library providing archive management functions that mirror AmigaDOS file management functions, and a full set of highly optimised compression and decompression functions supporting all the major Lh algorithm variants.
- **LhX** - a fast AmigaDOS ReadArgs front end for packing and unpacking LhA archives; also acts as a Workbench DefaultTool for file extraction
- **lh-handler** - an overlay filesystem that allows for accessing LhA files as if they are directories __and__ within Workbench as virtual drawers
- **LhA** - a command line option compatible version of the original LhA command, built standalone with lh.lib and written in cross platform C to allow compiling for Linux, Mac, Windows and any other platform
- **lh.library legacy** - the new lh.library is backwards compatible to the original functions of 1991's LhLib.lha by Holger P Krekel and Olaf 'Olsen' Barthel from 1991
- And a ToolKit SDK including:
-- **lh.lib** - the static link library core for embedding in software without requiring the lh.library dependency
-- **Autodocs & Includes** - a full set of developer documentation for LhASsA 

## Features

- **LhA 2.15-compatible** - LhASsA's LhA command targets classic 2.15 commands, options, wildcards, home directories, and console style
- **All lh* variants** - Compress lh0 (store only), lh1, lh5 (default), lh6, lh7;  Decompress lh0-lh7, lzs, lz4, lz5
- **Archive operations** - add, delete, concatenate, list (`l` / `v` / `vv`), test, print, extract (`x` / `e`)
- **AmigaDOS wildcards** - `#?`, `*`, and Kickstart 2.x+ patterns on filespecs and archive names; `-Qw` disables them
- **Compatible** - Unlike the Amiga original, LhASsA can process LhA files with Unix and Microsoft Windows extensions and international character file names

## Frequently Asked Questions

### Does LhASsA's LhA command implement all classic LhA features?

LhASsA targets **LHA format compatibility** and a large subset of LhA 2.15 behaviour. Several commands and options are still missing or only partially honoured.

**Commands not yet implemented**

| Command | Original purpose |
|---------|------------------|
| `u` | Update archive |
| `f` | Freshen archive |
| `m` | Move files into archive |
| `r` | Replace files |
| `h` | Hunt (compare archive vs filesystem) |
| `y` | Copy archive with new options |

**Implemented classic behaviour (LhA CLI)**

- **Add (`a`)** - creates/appends; skips existing members with a warning (use `u`/`f`/`r` when those land for replace). `-Qq` skips the duplicate scan
- **Paths (`-x`)** - basename by default; `-x` stores paths with the volume stripped; `-r` enables `-x` automatically (`-x0` clears it)
- **Home directories** - trailing `/` or `:` prefixes for path stripping; `/` resets; `-h` disables
- **`-r`** - recursive file collect; `-e` archives empty directories as `-lhd-`
- **`-R`** - recursive archive collect for list/extract/test/etc.
- **Wildcards** - AmigaDOS patterns on filespecs and archive names (`m` still requires a concrete archive); `-Qw` disables
- **Filenotes** - stored and restored as comments; `-f` ignores them
- **Suffixes** - append `.lha`/`.lzh` when needed; `-X` never appends; `-y` always appends
- **`ENV:LHAOPTS`** - applied at startup; `-I` ignores it
- **Add progress** - `Freezing:` / `Frozen:` lines (packed %% not yet reported mid-compress)

**LhX (AmigaDOS front end)** differs from classic `a` on purpose: ADD stores full command-line paths unless `NOPATHS`, replaces same-named members, and may expand a wild `ARCHIVE` for LIST/EXTRACT/TEST.

**Still missing / library-limited**

- **lh2-lh4, lh8+** - recognised in headers; decompression support varies (lh5-lh7, lzs, lz4, lz5 are the main tested paths)
- **Password-protected entries** - not supported for write; reader hooks exist but are incomplete
- **Self-extracting archives** - not supported
- **Multivolume** (`-V` / `-Qv`) - not supported
- **Preserve / clear / set archive bit** (`-a` / `-C` / `-S` / `-s`) and several date/confirm filters - parsed as no-ops for script compatibility

**No-op command-line options**

These switches are parsed and ignored (or only partially honoured) so LhA 2.15 command lines do not fail:

| Option | Original purpose |
|--------|------------------|
| `-a` | Preserve file attributes on extract |
| `-A` | Set archive attributes when adding |
| `-b` | I/O buffer size (KB) |
| `-B` | Keep backup of archive |
| `-c` | Confirm overwrites interactively |
| `-C` | Clear archive (A) bit on extracted files |
| `-d` | Archive date = newest file |
| `-D` | Alternate progress display |
| `-E` | Touch extracted files with current time |
| `-F` | Fast progress display mode |
| `-G` | Only extract newer files |
| `-i` / `-L` | Read / create file list |
| `-k` | Keep partial extractions after errors |
| `-K` | Kill empty directories (move) |
| `-l` / `-u` | Lowercase / uppercase filenames |
| `-m` | No messages for query |
| `-M` | No autoshow files (partial: `-M` / autoshow / `-Qd` exist) |
| `-o` / `-O` | Add/extract by date |
| `-p` | Pause after loading |
| `-P` | Set Exec task priority |
| `-s` | Add only files without the A bit set |
| `-S` | Set A bit on added files |
| `-t` / `-T` | Only extract new / new & newer files |
| `-V` / `-Qv` | Multivolume archive size / devices |
| `-W` | Exclude filenames |
| `-Y` | Store big files below ratio threshold |
| `-Z` | Compress archives inside archive |

**Partially implemented**

| Option / feature | LhASsA behaviour |
|------------------|-----------------|
| `-q` / `lq` / `vq` | Quiet console output |
| `-n` / `-N` | Suppress byte / all progress indicators |
| `-F` | Accepted; fast progress path not fully differentiated |
| `-x` | Preserve path names on add and extract |
| `-h` | Disable home directories |
| `-f` | Ignore filenotes on add and extract |
| `-e` | Store empty directories when used with `-r` |
| `-r` / `-R` | Recursive file / archive collect |
| `-X` / `-y` | Archive suffix control |
| `-I` / `ENV:LHAOPTS` | Ignore / apply default options |
| `-Qq` / `-Qw` | Quick add (skip dup scan) / disable wildcards |
| `-z` | Store uncompressed (lh0) |
| `-0` / `-1` / `-2` | Header levels 0, 1, 2 when writing |
| `-2` / `-3` | lh5 / lh6 compression when adding |
| `o5` / `o6` / `o7` | Select lh5 / lh6 / lh7 method |
| `w=<dir>` | Extract work directory |
| File comments | Filenotes on add/extract; preserved on rewrite |
| List totals | Archive date on totals line (Amiga `Lock`/`Examine` where available) |

**LhASsA-only extensions**

| Option | Purpose |
|--------|---------|
| `-v` (as an option flag, not the `v` command) | Verbose header/debug output on stderr |

### Can I contribute?

Yes. Code, testing, documentation, and archive fixtures are welcome. See the repository and [amigazen project](https://github.com/amigazen/) for pull requests.

## Contact

- **GitHub:** https://github.com/amigazen/lhassa/
- **Web:** http://www.amigazen.com/toolkit/ (Amiga browser compatible)
- **amigazen:** toolkit@amigazen.com

## Acknowledgements

*Amiga* is a trademark of **Amiga Inc**.

**LhA** was developed by **Stefan Boberg**, **Jim Cooper**, **David Tritscher**, **Sven Ottemann**, and the wider LhA for UNIX community (Yooichi Tagawa, Nobutaka Watazaki, and others).