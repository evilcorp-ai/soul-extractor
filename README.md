# Evil Corp – Soul Extractor

> IMGC Decompressor & Raw Disk Image File System Extractor

A standalone Windows GUI tool with two modes:
- **IMGC DUMP** — Decompress HDD Raw Copy Tool's proprietary `.imgc` format to raw `.img`
- **IMG EXTRACT** — Open raw disk images and extract entire file systems to disk

## Supported File Systems

| File System | Partition Tables |
|-------------|-----------------|
| FAT12 / FAT16 / FAT32 | MBR, GPT, bare |
| exFAT | MBR, GPT, bare |
| NTFS | MBR, GPT, bare |
| ext2 / ext3 / ext4 | MBR, GPT, bare |
| HFS+ | MBR, GPT, bare |

## Features

- 🔥 **Cyberpunk UI** — neon magenta/cyan dark theme with grid overlay
- 📂 **Drag & Drop** — drop files directly onto the window
- 📁 **Folder Picker** — choose output directory for extraction
- ⚡ **Threaded** — background processing keeps the UI responsive
- 📊 **Progress** — real-time progress bar and terminal-style log
- 🎯 **Auto-detect** — automatically selects mode by file extension
- 🏗️ **Single EXE** — zero dependencies, ~200KB standalone portable executable

## Build

Requires Visual Studio Build Tools (MSVC). From a Developer Command Prompt:

```
cl /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
   unimgc_gui.c image.c lzo.c imgextract.c partition.c ^
   fs_detect.c fs_fat.c fs_ntfs.c fs_ext.c fs_hfsplus.c ^
   app.res /Fe:soul_extractor.exe ^
   /link user32.lib gdi32.lib shell32.lib comdlg32.lib ^
         comctl32.lib ole32.lib /SUBSYSTEM:WINDOWS
```

Compile the resource file first: `rc app.rc`

## Usage

1. Run `soul_extractor.exe`
2. Select mode: **IMGC DUMP** or **IMG EXTRACT** (tabs at top)
3. Drop a file or click **BROWSE**
4. Click **OUTPUT DIR** to choose destination
5. Click **DUMP** or **EXTRACT**

## License

WTFPL (decompression core by [shiz](https://github.com/shizmob/unimgc))
