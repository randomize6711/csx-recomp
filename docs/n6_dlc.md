# N6A: the official Counter-Strike Xbox downloadable content and title update

Source (read only, never modified): `~/Documents/Decomp/cs xbox/Counter-Strike DLC` (37 files, 27 MB).
Every file's SHA-256 is in tests/dlc_sha256sums.txt; to check the folder against it:

    cd "~/Documents/Decomp/cs xbox/Counter-Strike DLC" && sha256sum -c ~/Work/csx-recomp/tests/dlc_sha256sums.txt

(checked 2026-10-30: all 37 OK).

## What the folder is (from the files themselves)

- **An installer**, not game content: `default.xbe` ("Counter-Strike DLC", title 434C4458, 2003-12-19 / cert
  2004-11-17), `CDXU/default.xbe` ("Counter-Strike Update", same build), `CDXU/reboot.xbe`, their menu (`CDXMedia/`,
  `CDXU/CDXMedia/`: a skin, font, sounds), `ambient/ambient.wma` (its music), `default.tbn`, PNGs, `Readme.txt` (a
  third party's "Downloadable Content Installer v3" that copies and signs the content into E:\TDATA\4d530036).
- **Two content packages** (`content/c/4d53003600000001`, `...0002`): `contentmeta.xbx` (XCSF, names in several
  languages: **"Office Map"** and **"Inferno Map"**), `ContentImage.xbx` (a 6 KB icon) and a 4 KB text file of filler
  ("Blah Blah ..."; CS_Download.txt / DE_Download.txt). No map, texture, sound or model: the packages are unlock
  tokens. The maps themselves are on the game disc: `cs_office.SXWad` and `de_inferno.SXWad` are in the game folder.
- **The title update** (`content/uNTSC/`, `content/uPAL/`): `default.xbe` (Counter-Strike, title 4D530036, version
  257 NTSC / 258 PAL, built 2003-11-25, cert 2003-12-12), `update.xbe` and `downloader.xbe` (version 257: the disc's
  own are version 2, the same builds of 2003-08-05 re-certified). The disc's default.xbe (assets/default.xbe, the
  recompiled one, sha256 e7698b09...) is version 2, built 2003-10-22. NTSC and PAL update executables differ only in
  their certificates (every section identical).

## What the title update changes

Same size (3973120 bytes), the same 20 sections; `.text` is 64 bytes longer (so everything after the first change moves
and a byte diff is meaningless), `.rdata` 40 bytes shorter. The only changed readable strings are the build stamps
("Exe build: 14:46:16 Oct 21 2003" -> "16:13:12 Nov 24 2003", the build path's "Final" -> "final"): **no new text,
no new feature, no new content**; a rebuild with code fixes. The disassembly of both `.text` sections with addresses
masked differs in 0.7 % of instructions (about 4800 of 726000, many of them data misread as code by a linear sweep).
Which functions changed and what they fix: see "Function-level comparison" below (N6A work).

## The DLC maps in the PC port before N6A

- The title's Custom Game map list (SuperLauncher_SinglePlayerMatchMapList) has 18 maps and Random: not Inferno and
  Office, which the console unlocked with these packages. Its Server Filters' map list (system link) names both.
- N4.8's party map list adds every de_ / cs_ map of the game folder the title leaves out: Inferno and Office are
  there (20 maps), without previews.

## Files

| File | Bytes | Type | SHA-256 (first 16) |
|---|---|---|---|
| ./ambient/ambient.wma | 2858144 | Microsoft ASF | 2f1b9f0bb5724523 |
| ./CDXMedia/arialuni.ttf | 367112 | TrueType Font data, digitally signed, 23 | 413c78f91bd39e13 |
| ./CDXMedia/CDX.inx | 2204 | Unicode text, UTF-16, little-endian text | 26a7e39cf86f2f5e |
| ./CDXMedia/cdx.xpr | 1710080 | Microsoft Xbox XPR0 texture, format: DXT | ed1ba61c0ddd68ee |
| ./CDXMedia/cdx.xsb | 604 | data | 0d21f8f7cef940db |
| ./CDXMedia/cdx.xwb | 687896 | data | b3578be79a65d790 |
| ./CDXU/CDXMedia/arialuni.ttf | 367112 | TrueType Font data, digitally signed, 23 | 413c78f91bd39e13 |
| ./CDXU/CDXMedia/CDX.inx | 2472 | Unicode text, UTF-16, little-endian text | 019e4407731d314d |
| ./CDXU/CDXMedia/cdx.xpr | 1710080 | Microsoft Xbox XPR0 texture, format: DXT | ed1ba61c0ddd68ee |
| ./CDXU/CDXMedia/cdx.xsb | 604 | data | 0d21f8f7cef940db |
| ./CDXU/CDXMedia/cdx.xwb | 687896 | data | b3578be79a65d790 |
| ./CDXU/default.xbe | 1388544 | Microsoft Xbox executable: "Counter-Stri | 8d95f730c6eb8aab |
| ./CDXU/loading.png | 374290 | PNG image data, 720 x 480, 8-bit/color R | 7d8b7d1577827978 |
| ./CDXU/reboot.png | 45176 | PNG image data, 320 x 240, 8-bit/color R | bdff403e6f845016 |
| ./CDXU/reboot.xbe | 53248 | Microsoft Xbox executable: "Reboot Xbox" | fd2a90b2b476d92a |
| ./CDXU/thumb.png | 164898 | PNG image data, 320 x 240, 8-bit/color R | 367e8b115df37fe4 |
| ./content/4d53003600000001.png | 134606 | PNG image data, 320 x 240, 8-bit/color R | ec9b329465c739ff |
| ./content/4d53003600000002.png | 179236 | PNG image data, 320 x 240, 8-bit/color R | 7e6be488febc670b |
| ./content/c/4d53003600000001/ContentImage.xbx | 6144 | Microsoft Xbox XPR0 texture, format: DXT | 641069c8ea7059e3 |
| ./content/c/4d53003600000001/contentmeta.xbx | 612 | data | ad5d84585132b508 |
| ./content/c/4d53003600000001/CS_Download.txt | 4235 | ASCII text, with CRLF line terminators | 956e2cce69a3b697 |
| ./content/c/4d53003600000002/ContentImage.xbx | 6144 | Microsoft Xbox XPR0 texture, format: DXT | dc112e1e69ecdc10 |
| ./content/c/4d53003600000002/contentmeta.xbx | 612 | data | f3dc7c835be47371 |
| ./content/c/4d53003600000002/DE_Download.txt | 4235 | ASCII text, with CRLF line terminators | 956e2cce69a3b697 |
| ./content/uNTSC/contentmeta.xbx | 368 | data | b4ca6d0f5b314b72 |
| ./content/uNTSC/default.xbe | 3973120 | Microsoft Xbox executable: "Counter-Stri | c7584a61912d2174 |
| ./content/uNTSC/downloader.xbe | 1138688 | Microsoft Xbox executable: "Downloader"  | b2ea7ed950c0ed74 |
| ./content/uNTSC.png | 156239 | PNG image data, 320 x 240, 8-bit/color R | accc4d7d3ecd2922 |
| ./content/uNTSC/update.xbe | 2387968 | Microsoft Xbox executable: "Downloader"  | 1a950e1cffce5d8d |
| ./content/uPAL/contentmeta.xbx | 368 | data | 61981f571ed2a8e2 |
| ./content/uPAL/default.xbe | 3973120 | Microsoft Xbox executable: "Counter-Stri | 1e3189180ca105e8 |
| ./content/uPAL/downloader.xbe | 1138688 | Microsoft Xbox executable: "Downloader"  | 2561559089e6f2b2 |
| ./content/uPAL.png | 194088 | PNG image data, 320 x 240, 8-bit/color R | a2036ef22372a0a2 |
| ./content/uPAL/update.xbe | 2387968 | Microsoft Xbox executable: "Downloader"  | 8b0a47d7f81a7f69 |
| ./default.tbn | 233312 | PNG image data, 300 x 300, 8-bit/color R | f42adc3bea228a70 |
| ./default.xbe | 1388544 | Microsoft Xbox executable: "Counter-Stri | 33f13eb60be6a552 |
| ./Readme.txt | 1728 | ASCII text, with very long lines (543),  | 66283c600c95f412 |
