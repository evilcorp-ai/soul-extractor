/***********************************************************************
 * unimgc_gui.c  –  Evil Corp - Soul Extractor
 *
 * Native Win32 GUI for:
 *   Mode 1: IMGC decompression (HDD Raw Copy Tool format)
 *   Mode 2: IMG file system extraction (FAT/NTFS/ext/HFS+/exFAT)
 *
 * Build (MSVC x64):
 *   cl /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE
 *      unimgc_gui.c image.c lzo.c imgextract.c partition.c
 *      fs_detect.c fs_fat.c fs_ntfs.c fs_ext.c fs_hfsplus.c
 *      /Fe:soul_extractor.exe
 *      /link user32.lib gdi32.lib shell32.lib comdlg32.lib
 *            comctl32.lib ole32.lib /SUBSYSTEM:WINDOWS
 *
 * Copyright (c) 2019 shiz (decompression core); GUI wrapper WTFPL.
 ***********************************************************************/

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

/* _CRT_SECURE_NO_WARNINGS passed via compiler flag */
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <process.h>
#include <math.h>

#include "endian.h"
#include "image.h"
#include "imgextract.h"

/* ═══════════════════════ CYBERPUNK PALETTE ══════════════════════════ */
#define CLR_BG            RGB( 10,  10,  16)
#define CLR_BG2           RGB( 16,  16,  24)
#define CLR_PANEL         RGB( 18,  20,  30)
#define CLR_BORDER        RGB( 40,  45,  65)
#define CLR_BORDER_HOT    RGB( 80,  90, 130)
#define CLR_TEXT          RGB(220, 225, 240)
#define CLR_SUBTEXT       RGB(100, 110, 140)
#define CLR_NEON_MAGENTA  RGB(255,  20, 147)
#define CLR_NEON_CYAN     RGB(  0, 255, 255)
#define CLR_NEON_YELLOW   RGB(255, 240,   0)
#define CLR_NEON_GREEN    RGB(  0, 255, 100)
#define CLR_NEON_RED      RGB(255,  40,  60)
#define CLR_DROPZONE      RGB( 14,  16,  26)
#define CLR_DROPZONE_HL   RGB( 22,  24,  40)
#define CLR_PROGRESS_BG   RGB( 25,  28,  42)
#define CLR_BTN_BG        RGB( 28,  30,  48)
#define CLR_BTN_HOT       RGB( 38,  42,  68)
#define CLR_ACCENT        CLR_NEON_MAGENTA
#define CLR_ACCENT_HOT    RGB(255,  80, 180)
#define CLR_ACCENT2       CLR_NEON_CYAN
#define CLR_LOG_BG        RGB( 12,  14,  22)
#define CLR_LOG_TEXT      RGB(  0, 220, 200)
#define CLR_TAB_ACTIVE    RGB( 28,  30,  48)
#define CLR_TAB_INACTIVE  RGB( 14,  16,  24)

/* ══════════════════════ DIMENSIONS ══════════════════════════════════ */
#define WIN_W   680
#define WIN_H   620
#define MARGIN  20

/* ══════════════════════ CONTROL IDs ═════════════════════════════════ */
#define ID_LOG   1003

/* ══════════════════════ MODES ═══════════════════════════════════════ */
#define MODE_IMGC  0
#define MODE_IMG   1

/* ══════════════════════ APP STATE ═══════════════════════════════════ */
typedef struct {
    HWND  hwnd;
    HWND  hLog;
    HFONT hFontUI;
    HFONT hFontUIBold;
    HFONT hFontMono;
    HFONT hFontTitle;
    HFONT hFontIcon;
    HFONT hFontSmall;

    int   mode;                        /* MODE_IMGC or MODE_IMG */

    wchar_t inputPath[MAX_PATH];
    wchar_t outputFolder[MAX_PATH];

    int   dropHover;
    int   browseHover;
    int   folderHover;
    int   actionHover;
    int   tabImgcHover;
    int   tabImgHover;
    int   working;
    double progress;
    wchar_t statusText[256];

    RECT  rcTabImgc;
    RECT  rcTabImg;
    RECT  rcDrop;
    RECT  rcBrowse;
    RECT  rcFolder;
    RECT  rcAction;       /* Dump / Extract button */
    RECT  rcProgress;
    RECT  rcStatus;
    RECT  rcFolderLabel;
} AppState;

static AppState g;

/* ══════════════════ FORWARD DECLS ═══════════════════════════════════ */
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static unsigned __stdcall ImgcWorkerThread(void *);
static unsigned __stdcall ImgWorkerThread(void *);
static void Log(const wchar_t *fmt, ...);
static void SetStatus(const wchar_t *fmt, ...);

/* ══════════════════ HELPERS ═════════════════════════════════════════ */
static HFONT MakeFont(int size, int weight, int italic, const wchar_t *face)
{
    return CreateFontW(-size, 0, 0, 0, weight, italic, 0, 0,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void FillRoundRect2(HDC hdc, RECT *r, int radius, COLORREF clr)
{
    HBRUSH hBr = CreateSolidBrush(clr);
    HPEN hPen  = CreatePen(PS_SOLID, 1, clr);
    HGDIOBJ oldBr  = SelectObject(hdc, hBr);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    RoundRect(hdc, r->left, r->top, r->right, r->bottom, radius, radius);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(hBr);
    DeleteObject(hPen);
}

static void DrawBorderRR(HDC hdc, RECT *r, int radius, COLORREF border,
                          COLORREF fill, int penStyle, int penWidth)
{
    HBRUSH hBr = CreateSolidBrush(fill);
    HPEN hPen  = CreatePen(penStyle, penWidth, border);
    HGDIOBJ oldBr  = SelectObject(hdc, hBr);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    RoundRect(hdc, r->left, r->top, r->right, r->bottom, radius, radius);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(hBr);
    DeleteObject(hPen);
}

static void DrawTextC(HDC hdc, RECT *r, const wchar_t *text, HFONT font,
                       COLORREF clr, UINT fmt)
{
    HGDIOBJ old = SelectObject(hdc, font);
    SetTextColor(hdc, clr);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text, -1, r, fmt | DT_NOPREFIX);
    SelectObject(hdc, old);
}

#define DTC_CENTER (DT_CENTER | DT_VCENTER | DT_SINGLELINE)
#define DTC_LEFT   (DT_LEFT   | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)

static void DrawNeonLine(HDC hdc, int x1, int y1, int x2, int y2, COLORREF clr)
{
    HPEN hPen = CreatePen(PS_SOLID, 1, clr);
    HGDIOBJ old = SelectObject(hdc, hPen);
    MoveToEx(hdc, x1, y1, NULL);
    LineTo(hdc, x2, y2);
    SelectObject(hdc, old);
    DeleteObject(hPen);
}

static int PtInRc(RECT *r, int x, int y)
{
    POINT pt = { x, y };
    return PtInRect(r, pt);
}

/* ══════════════ FOLDER PICKER ══════════════════════════════════════ */
static int PickFolder(HWND hwnd, wchar_t *folderPath, int maxLen)
{
    IFileDialog *pfd = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileOpenDialog, (void **)&pfd);
    if (SUCCEEDED(hr) && pfd) {
        DWORD opts = 0;
        pfd->lpVtbl->GetOptions(pfd, &opts);
        pfd->lpVtbl->SetOptions(pfd, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pfd->lpVtbl->SetTitle(pfd, L"Choose Output Folder");
        hr = pfd->lpVtbl->Show(pfd, hwnd);
        if (SUCCEEDED(hr)) {
            IShellItem *psi = NULL;
            hr = pfd->lpVtbl->GetResult(pfd, &psi);
            if (SUCCEEDED(hr) && psi) {
                wchar_t *path = NULL;
                psi->lpVtbl->GetDisplayName(psi, SIGDN_FILESYSPATH, &path);
                if (path) {
                    wcsncpy(folderPath, path, maxLen - 1);
                    folderPath[maxLen - 1] = 0;
                    CoTaskMemFree(path);
                    psi->lpVtbl->Release(psi);
                    pfd->lpVtbl->Release(pfd);
                    return 1;
                }
                psi->lpVtbl->Release(psi);
            }
        }
        pfd->lpVtbl->Release(pfd);
        return 0;
    }

    BROWSEINFOW bi = { 0 };
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"Choose Output Folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        SHGetPathFromIDListW(pidl, folderPath);
        CoTaskMemFree(pidl);
        return 1;
    }
    return 0;
}

/* ══════════════ FILE PICKER ═════════════════════════════════════════ */
static int ShowOpenDialog(HWND hwnd, wchar_t *path, int maxPath)
{
    const wchar_t *filter;
    const wchar_t *title;
    if (g.mode == MODE_IMGC) {
        filter = L"IMGC Files (*.imgc)\0*.imgc\0All Files (*.*)\0*.*\0";
        title  = L"Select .imgc file";
    } else {
        filter = L"Disk Images (*.img;*.raw;*.dd;*.iso)\0*.img;*.raw;*.dd;*.iso\0All Files (*.*)\0*.*\0";
        title  = L"Select disk image file";
    }

    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = maxPath;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle  = title;
    return GetOpenFileNameW(&ofn);
}

/* ══════════════ OUTPUT PATH HELPERS ═════════════════════════════════ */
static void MakeImgcOutputPath(const wchar_t *input, const wchar_t *folder,
                                wchar_t *output, int maxLen)
{
    const wchar_t *fname = wcsrchr(input, L'\\');
    fname = fname ? fname + 1 : input;
    wchar_t baseName[MAX_PATH];
    wcsncpy(baseName, fname, MAX_PATH - 1);
    baseName[MAX_PATH - 1] = 0;
    wchar_t *dot = wcsrchr(baseName, L'.');
    if (dot && !_wcsicmp(dot, L".imgc"))
        wcscpy(dot, L".img");
    else
        wcscat(baseName, L".img");
    _snwprintf(output, maxLen, L"%s\\%s", folder, baseName);
    output[maxLen - 1] = 0;
}

/* ══════════════ LOG ═════════════════════════════════════════════════ */
static void Log(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1024, fmt, ap);
    va_end(ap);
    int len = GetWindowTextLengthW(g.hLog);
    SendMessageW(g.hLog, EM_SETSEL, len, len);
    SendMessageW(g.hLog, EM_REPLACESEL, FALSE, (LPARAM)buf);
}

static void SetStatus(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(g.statusText, 256, fmt, ap);
    va_end(ap);
    InvalidateRect(g.hwnd, &g.rcStatus, FALSE);
}

/* ══════════════ SET INPUT ═══════════════════════════════════════════ */
static void SetInputFile(const wchar_t *path)
{
    wcsncpy(g.inputPath, path, MAX_PATH - 1);
    g.inputPath[MAX_PATH - 1] = 0;

    /* auto-detect mode by extension */
    const wchar_t *dot = wcsrchr(path, L'.');
    if (dot && _wcsicmp(dot, L".imgc") == 0)
        g.mode = MODE_IMGC;
    else
        g.mode = MODE_IMG;

    /* auto-set output folder */
    if (!g.outputFolder[0]) {
        wcsncpy(g.outputFolder, path, MAX_PATH - 1);
        g.outputFolder[MAX_PATH - 1] = 0;
        wchar_t *slash = wcsrchr(g.outputFolder, L'\\');
        if (slash) *slash = 0;
    }

    const wchar_t *fname = wcsrchr(path, L'\\');
    fname = fname ? fname + 1 : path;
    SetStatus(L"// READY: %s", fname);
    InvalidateRect(g.hwnd, NULL, FALSE);
}

/* ══════════ WinMain ═════════════════════════════════════════════════ */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    (void)hPrev; (void)cmdLine;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = { 0 };
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"EvilCorpSoulExtractor";
    wc.hbrBackground = NULL;
    wc.hIcon         = LoadIcon(hInst, MAKEINTRESOURCE(1));
    RegisterClassExW(&wc);

    g.hFontUI     = MakeFont(14, FW_NORMAL, 0, L"Segoe UI");
    g.hFontUIBold = MakeFont(14, FW_BOLD,   0, L"Segoe UI");
    g.hFontMono   = MakeFont(12, FW_NORMAL, 0, L"Consolas");
    g.hFontTitle  = MakeFont(24, FW_BOLD,   0, L"Segoe UI");
    g.hFontIcon   = MakeFont(44, FW_THIN,   0, L"Segoe UI Symbol");
    g.hFontSmall  = MakeFont(11, FW_NORMAL, 0, L"Segoe UI");

    g.mode = MODE_IMGC;
    wcscpy(g.statusText, L"// AWAITING INPUT ...");

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);

    g.hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        L"EvilCorpSoulExtractor",
        L"EVIL CORP \x2013 SOUL EXTRACTOR",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sx - WIN_W) / 2, (sy - WIN_H) / 2, WIN_W, WIN_H,
        NULL, NULL, hInst, NULL);

    ShowWindow(g.hwnd, nShow);
    UpdateWindow(g.hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(g.hFontUI);
    DeleteObject(g.hFontUIBold);
    DeleteObject(g.hFontMono);
    DeleteObject(g.hFontTitle);
    DeleteObject(g.hFontIcon);
    DeleteObject(g.hFontSmall);
    CoUninitialize();
    return (int)msg.wParam;
}

/* ══════════════ LAYOUT ══════════════════════════════════════════════ */
static void CalcLayout(RECT *rc)
{
    int x = rc->left + MARGIN;
    int y = rc->top  + MARGIN;
    int w = (rc->right - rc->left) - 2 * MARGIN;

    /* Title: ~50px */
    y += 52;

    /* Tab buttons */
    int tabW = w / 2 - 4;
    g.rcTabImgc.left = x;        g.rcTabImgc.top = y;
    g.rcTabImgc.right = x + tabW; g.rcTabImgc.bottom = y + 32;
    g.rcTabImg.left = x + tabW + 8; g.rcTabImg.top = y;
    g.rcTabImg.right = x + w;      g.rcTabImg.bottom = y + 32;
    y += 32 + 8;

    /* Drop zone */
    g.rcDrop.left = x;       g.rcDrop.top = y;
    g.rcDrop.right = x + w;  g.rcDrop.bottom = y + 120;
    y += 120 + 8;

    /* Three buttons */
    int gap = 8;
    int btnW = (w - 2 * gap) / 3;

    g.rcBrowse.left = x;           g.rcBrowse.top = y;
    g.rcBrowse.right = x + btnW;   g.rcBrowse.bottom = y + 36;

    g.rcFolder.left = x + btnW + gap;           g.rcFolder.top = y;
    g.rcFolder.right = x + 2 * btnW + gap;      g.rcFolder.bottom = y + 36;

    g.rcAction.left = x + 2 * (btnW + gap);     g.rcAction.top = y;
    g.rcAction.right = x + w;                    g.rcAction.bottom = y + 36;
    y += 36 + 5;

    /* Folder label */
    g.rcFolderLabel.left = x;   g.rcFolderLabel.top = y;
    g.rcFolderLabel.right = x + w; g.rcFolderLabel.bottom = y + 16;
    y += 16 + 5;

    /* Progress bar */
    g.rcProgress.left = x;   g.rcProgress.top = y;
    g.rcProgress.right = x + w; g.rcProgress.bottom = y + 8;
    y += 8 + 4;

    /* Status text */
    g.rcStatus.left = x;   g.rcStatus.top = y;
    g.rcStatus.right = x + w; g.rcStatus.bottom = y + 18;
    y += 18 + 6;

    /* Log */
    if (g.hLog)
        MoveWindow(g.hLog, x + 1, y + 1, w - 2, rc->bottom - MARGIN - y - 2, TRUE);
}

/* ══════════════ PAINTING ════════════════════════════════════════════ */
static void PaintAll(HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    /* background */
    HBRUSH hBg = CreateSolidBrush(CLR_BG);
    FillRect(hdc, &rc, hBg);
    DeleteObject(hBg);

    /* grid */
    for (int gx = 0; gx < W; gx += 40)
        DrawNeonLine(hdc, gx, 0, gx, H, RGB(18, 20, 28));
    for (int gy = 0; gy < H; gy += 40)
        DrawNeonLine(hdc, 0, gy, W, gy, RGB(18, 20, 28));

    /* top neon */
    DrawNeonLine(hdc, 0, 0, W, 0, CLR_NEON_MAGENTA);
    DrawNeonLine(hdc, 0, 1, W, 1, RGB(180, 15, 100));

    /* title */
    {
        RECT r = { MARGIN, MARGIN + 2, rc.right - MARGIN, MARGIN + 28 };
        DrawTextC(hdc, &r, L"EVIL CORP", g.hFontTitle, CLR_NEON_MAGENTA, DTC_CENTER);
        RECT r2 = { MARGIN, MARGIN + 28, rc.right - MARGIN, MARGIN + 46 };
        DrawTextC(hdc, &r2, L"S O U L   E X T R A C T O R", g.hFontUIBold, CLR_NEON_CYAN, DTC_CENTER);
    }

    /* title rule */
    {
        int ruleY = MARGIN + 50;
        DrawNeonLine(hdc, MARGIN, ruleY, rc.right - MARGIN, ruleY, CLR_BORDER);
        DrawNeonLine(hdc, MARGIN, ruleY - 2, MARGIN + 60, ruleY - 2, CLR_NEON_MAGENTA);
        DrawNeonLine(hdc, rc.right - MARGIN - 60, ruleY - 2, rc.right - MARGIN, ruleY - 2, CLR_NEON_CYAN);
    }

    /* ─── Tab buttons ─── */
    {
        /* IMGC tab */
        COLORREF imgcBg  = (g.mode == MODE_IMGC) ? CLR_TAB_ACTIVE : CLR_TAB_INACTIVE;
        COLORREF imgcBdr = (g.mode == MODE_IMGC) ? CLR_NEON_MAGENTA :
                           (g.tabImgcHover ? CLR_BORDER_HOT : CLR_BORDER);
        COLORREF imgcTxt = (g.mode == MODE_IMGC) ? CLR_NEON_MAGENTA : CLR_SUBTEXT;
        DrawBorderRR(hdc, &g.rcTabImgc, 4, imgcBdr, imgcBg, PS_SOLID, 1);
        DrawTextC(hdc, &g.rcTabImgc, L"\x25C8  IMGC DUMP", g.hFontUIBold, imgcTxt, DTC_CENTER);
        if (g.mode == MODE_IMGC)
            DrawNeonLine(hdc, g.rcTabImgc.left + 4, g.rcTabImgc.bottom - 1,
                              g.rcTabImgc.right - 4, g.rcTabImgc.bottom - 1, CLR_NEON_MAGENTA);

        /* IMG tab */
        COLORREF imgBg  = (g.mode == MODE_IMG) ? CLR_TAB_ACTIVE : CLR_TAB_INACTIVE;
        COLORREF imgBdr = (g.mode == MODE_IMG) ? CLR_NEON_CYAN :
                          (g.tabImgHover ? CLR_BORDER_HOT : CLR_BORDER);
        COLORREF imgTxt = (g.mode == MODE_IMG) ? CLR_NEON_CYAN : CLR_SUBTEXT;
        DrawBorderRR(hdc, &g.rcTabImg, 4, imgBdr, imgBg, PS_SOLID, 1);
        DrawTextC(hdc, &g.rcTabImg, L"\x25A3  IMG EXTRACT", g.hFontUIBold, imgTxt, DTC_CENTER);
        if (g.mode == MODE_IMG)
            DrawNeonLine(hdc, g.rcTabImg.left + 4, g.rcTabImg.bottom - 1,
                              g.rcTabImg.right - 4, g.rcTabImg.bottom - 1, CLR_NEON_CYAN);
    }

    /* ─── Drop zone ─── */
    {
        COLORREF dzBg     = g.dropHover ? CLR_DROPZONE_HL : CLR_DROPZONE;
        COLORREF dzBorder = g.dropHover ? CLR_NEON_CYAN : CLR_BORDER;
        DrawBorderRR(hdc, &g.rcDrop, 6, dzBorder, dzBg,
                     g.dropHover ? PS_SOLID : PS_DOT, 2);

        /* corner accents */
        int cLen = 14;
        COLORREF cClr = g.dropHover ? CLR_NEON_CYAN : CLR_NEON_MAGENTA;
        DrawNeonLine(hdc, g.rcDrop.left+4, g.rcDrop.top+4, g.rcDrop.left+4+cLen, g.rcDrop.top+4, cClr);
        DrawNeonLine(hdc, g.rcDrop.left+4, g.rcDrop.top+4, g.rcDrop.left+4, g.rcDrop.top+4+cLen, cClr);
        DrawNeonLine(hdc, g.rcDrop.right-5, g.rcDrop.top+4, g.rcDrop.right-5-cLen, g.rcDrop.top+4, cClr);
        DrawNeonLine(hdc, g.rcDrop.right-5, g.rcDrop.top+4, g.rcDrop.right-5, g.rcDrop.top+4+cLen, cClr);
        DrawNeonLine(hdc, g.rcDrop.left+4, g.rcDrop.bottom-5, g.rcDrop.left+4+cLen, g.rcDrop.bottom-5, cClr);
        DrawNeonLine(hdc, g.rcDrop.left+4, g.rcDrop.bottom-5, g.rcDrop.left+4, g.rcDrop.bottom-5-cLen, cClr);
        DrawNeonLine(hdc, g.rcDrop.right-5, g.rcDrop.bottom-5, g.rcDrop.right-5-cLen, g.rcDrop.bottom-5, cClr);
        DrawNeonLine(hdc, g.rcDrop.right-5, g.rcDrop.bottom-5, g.rcDrop.right-5, g.rcDrop.bottom-5-cLen, cClr);

        if (g.inputPath[0]) {
            const wchar_t *fname = wcsrchr(g.inputPath, L'\\');
            fname = fname ? fname + 1 : g.inputPath;
            RECT r1 = g.rcDrop;
            r1.bottom = r1.top + (r1.bottom - r1.top) / 2 + 8;
            r1.top += 10;
            DrawTextC(hdc, &r1, L"\x25C8", g.hFontIcon, CLR_NEON_CYAN, DTC_CENTER);
            RECT r2 = g.rcDrop;
            r2.top = r1.bottom - 8;
            r2.bottom -= 5;
            DrawTextC(hdc, &r2, fname, g.hFontUIBold, CLR_TEXT, DTC_CENTER);
        } else {
            const wchar_t *hint = (g.mode == MODE_IMGC)
                ? L"DROP .IMGC FILE HERE" : L"DROP .IMG FILE HERE";
            RECT r1 = g.rcDrop;
            r1.bottom = r1.top + (r1.bottom - r1.top) / 2;
            r1.top += 5;
            DrawTextC(hdc, &r1, L"\x25BC", g.hFontIcon, CLR_SUBTEXT, DTC_CENTER);
            RECT r2 = g.rcDrop;
            r2.top = r1.bottom - 12;
            DrawTextC(hdc, &r2, hint, g.hFontUI, CLR_SUBTEXT, DTC_CENTER);
        }
    }

    /* ─── Browse button ─── */
    {
        COLORREF bg  = g.browseHover ? CLR_BTN_HOT : CLR_BTN_BG;
        COLORREF bdr = g.browseHover ? CLR_NEON_CYAN : CLR_BORDER;
        DrawBorderRR(hdc, &g.rcBrowse, 4, bdr, bg, PS_SOLID, 1);
        DrawTextC(hdc, &g.rcBrowse, L"\x25A1  BROWSE", g.hFontUIBold, CLR_TEXT, DTC_CENTER);
    }

    /* ─── Folder button ─── */
    {
        COLORREF bg  = g.folderHover ? CLR_BTN_HOT : CLR_BTN_BG;
        COLORREF bdr = g.folderHover ? CLR_NEON_YELLOW : CLR_BORDER;
        DrawBorderRR(hdc, &g.rcFolder, 4, bdr, bg, PS_SOLID, 1);
        DrawTextC(hdc, &g.rcFolder, L"\x25A3  OUTPUT DIR", g.hFontUIBold, CLR_NEON_YELLOW, DTC_CENTER);
    }

    /* ─── Action button (Dump / Extract) ─── */
    {
        COLORREF bg, bdr, txt;
        int ready = g.inputPath[0] && g.outputFolder[0];
        if (g.working) {
            bg = CLR_PANEL; bdr = CLR_BORDER; txt = CLR_SUBTEXT;
        } else if (ready) {
            bg  = g.actionHover ? RGB(60, 8, 35) : RGB(45, 5, 28);
            bdr = g.actionHover ? CLR_ACCENT_HOT : CLR_NEON_MAGENTA;
            txt = CLR_NEON_MAGENTA;
        } else {
            bg = CLR_PANEL; bdr = CLR_BORDER; txt = CLR_SUBTEXT;
        }
        DrawBorderRR(hdc, &g.rcAction, 4, bdr, bg, PS_SOLID, 1);

        const wchar_t *label;
        if (g.working) {
            label = (g.mode == MODE_IMGC) ? L"\x25CC  DUMPING..." : L"\x25CC  EXTRACTING...";
        } else {
            label = (g.mode == MODE_IMGC) ? L"\x25B6  DUMP" : L"\x25B6  EXTRACT";
        }
        DrawTextC(hdc, &g.rcAction, label, g.hFontUIBold, txt, DTC_CENTER);
    }

    /* ─── Folder label ─── */
    {
        wchar_t label[MAX_PATH + 20];
        if (g.outputFolder[0])
            _snwprintf(label, MAX_PATH + 20, L"OUT \x2192  %s", g.outputFolder);
        else
            wcscpy(label, L"OUT \x2192  (click OUTPUT DIR)");
        DrawTextC(hdc, &g.rcFolderLabel, label, g.hFontSmall,
                 g.outputFolder[0] ? CLR_NEON_YELLOW : CLR_SUBTEXT, DTC_LEFT);
    }

    /* ─── Progress ─── */
    FillRoundRect2(hdc, &g.rcProgress, 4, CLR_PROGRESS_BG);
    if (g.progress > 0.0) {
        RECT rcF = g.rcProgress;
        int fw = (int)((rcF.right - rcF.left) * g.progress);
        rcF.right = rcF.left + (fw > 4 ? fw : 4);
        COLORREF pClr = (g.progress >= 1.0) ? CLR_NEON_GREEN : CLR_NEON_MAGENTA;
        FillRoundRect2(hdc, &rcF, 4, pClr);
    }

    /* ─── Status ─── */
    {
        COLORREF clr = CLR_SUBTEXT;
        if (wcsstr(g.statusText, L"Error") || wcsstr(g.statusText, L"FAIL"))
            clr = CLR_NEON_RED;
        else if (wcsstr(g.statusText, L"COMPLETE") || wcsstr(g.statusText, L"Done"))
            clr = CLR_NEON_GREEN;
        else if (wcsstr(g.statusText, L"DUMPING") || wcsstr(g.statusText, L"EXTRACTING"))
            clr = CLR_NEON_MAGENTA;
        else if (wcsstr(g.statusText, L"READY"))
            clr = CLR_NEON_CYAN;
        DrawTextC(hdc, &g.rcStatus, g.statusText, g.hFontMono, clr, DTC_LEFT);
    }

    /* log border */
    if (g.hLog) {
        RECT rlb;
        GetWindowRect(g.hLog, &rlb);
        MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&rlb, 2);
        rlb.left--; rlb.top--; rlb.right++; rlb.bottom++;
        DrawBorderRR(hdc, &rlb, 2, CLR_BORDER, CLR_LOG_BG, PS_SOLID, 1);
    }

    /* bottom neon */
    DrawNeonLine(hdc, 0, H - 1, W, H - 1, CLR_NEON_CYAN);
    DrawNeonLine(hdc, 0, H - 2, W, H - 2, RGB(0, 140, 140));
}

/* ═══════════ IMGC WORKER ════════════════════════════════════════════ */
#define WM_WORKER_PROGRESS  (WM_USER + 1)
#define WM_WORKER_DONE      (WM_USER + 2)
#define WM_WORKER_LOG       (WM_USER + 3)

typedef struct {
    wchar_t inPath[MAX_PATH];
    wchar_t outPath[MAX_PATH];
    HWND    hwnd;
} WorkerArgs;

static unsigned __stdcall ImgcWorkerThread(void *param)
{
    WorkerArgs *args = (WorkerArgs *)param;
    char inPathA[MAX_PATH * 2], outPathA[MAX_PATH * 2];
    WideCharToMultiByte(CP_ACP, 0, args->inPath, -1, inPathA, sizeof(inPathA), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, args->outPath, -1, outPathA, sizeof(outPathA), NULL, NULL);

    FILE *fin = fopen(inPathA, "rb");
    if (!fin) {
        PostMessageW(args->hwnd, WM_WORKER_DONE, 0, (LPARAM)L"Error: FAIL - Could not open input");
        free(args); return 1;
    }
    FILE *fout = fopen(outPathA, "wb");
    if (!fout) {
        fclose(fin);
        PostMessageW(args->hwnd, WM_WORKER_DONE, 0, (LPARAM)L"Error: FAIL - Could not open output");
        free(args); return 1;
    }

    uint8_t hbuf[IMGC_HEADER_SIZE];
    if (fread(hbuf, 1, sizeof(hbuf), fin) != sizeof(hbuf)) {
        fclose(fin); fclose(fout);
        PostMessageW(args->hwnd, WM_WORKER_DONE, 0, (LPARAM)L"Error: FAIL - Too small for header");
        free(args); return 1;
    }

    struct imgc_header hdr;
    if (imgc_parse(hbuf, sizeof(hbuf), &hdr) < 0) {
        fclose(fin); fclose(fout);
        PostMessageW(args->hwnd, WM_WORKER_DONE, 0, (LPARAM)L"Error: FAIL - Invalid IMGC header");
        free(args); return 1;
    }

    {
        wchar_t wm[256], wr[256], ws[256], wn[256], wv[256];
        MultiByteToWideChar(CP_ACP,0,pascal_to_cstr(&hdr.volume.model),-1,wm,256);
        MultiByteToWideChar(CP_ACP,0,pascal_to_cstr(&hdr.volume.revision),-1,wr,256);
        MultiByteToWideChar(CP_ACP,0,pascal_to_cstr(&hdr.volume.serial),-1,ws,256);
        MultiByteToWideChar(CP_ACP,0,pascal_to_cstr(&hdr.software.name),-1,wn,256);
        MultiByteToWideChar(CP_ACP,0,pascal_to_cstr(&hdr.software.version),-1,wv,256);
        wchar_t info[600];
        _snwprintf(info, 600,
            L"[HDR] Vol: %s  Rev: %s  S/N: %s\r\n"
            L"[HDR] SW: %s v%s\r\n"
            L"[HDR] Sectors: %" PRIu64 L" x %" PRIu64 L"\r\n"
            L"--------------------------------------\r\n",
            wm,wr,ws,wn,wv,hdr.image.sector_count,hdr.image.sector_size);
        PostMessageW(args->hwnd, WM_WORKER_LOG, 0, (LPARAM)_wcsdup(info));
    }

    uint64_t total_size = hdr.image.sector_count * hdr.image.sector_size;
    uint8_t *bbuf = NULL, *dbuf = NULL;
    size_t bsize = 0, dsize = 0;
    char zeros[4096] = {0};
    int err = 0;
    int blocks = 0;

    for (;;) {
        if (total_size > 0) {
            int64_t pos = _ftelli64(fout);
            int ipct = (int)((double)pos / (double)total_size * 10000);
            PostMessageW(args->hwnd, WM_WORKER_PROGRESS, ipct, 0);
        }
        uint8_t bh[IMGC_BLOCK_HEADER_SIZE];
        size_t nr = fread(bh, 1, sizeof(bh), fin);
        if (nr != sizeof(bh)) {
            if (feof(fin) && nr == 0) break;
            PostMessageW(args->hwnd, WM_WORKER_DONE, 0,
                (LPARAM)L"Error: FAIL - Truncated input");
            err = 1; break;
        }
        struct imgc_block_header bhdr;
        if (imgc_parse_block(bh, sizeof(bh), &bhdr) < 0) {
            PostMessageW(args->hwnd, WM_WORKER_DONE, 0,
                (LPARAM)L"Error: FAIL - Bad block header");
            err = 1; break;
        }
        blocks++;
        size_t sz = bhdr.size - IMGC_BLOCK_HEADER_SIZE;
        if (sz > bsize) {
            uint8_t *t = (uint8_t *)realloc(bbuf, sz);
            if (!t) { err = 1; break; }
            bbuf = t; bsize = sz;
        }
        if (fread(bbuf, 1, sz, fin) != sz) { err = 1; break; }
        switch (bhdr.type) {
        case IMGC_BLOCK_COMPRESSED: {
            size_t dsz = imgc_decompress_block(bbuf, sz, NULL, 0);
            if (dsz > dsize) {
                uint8_t *t = (uint8_t *)realloc(dbuf, dsz);
                if (!t) { err = 1; break; }
                dbuf = t; dsize = dsz;
            }
            if (err) break;
            if (!imgc_decompress_block(bbuf, sz, dbuf, dsz)) { err = 1; break; }
            fwrite(dbuf, 1, dsz, fout);
            break; }
        case IMGC_BLOCK_ZERO: {
            size_t dsz = (size_t)le64toh(*(uint64_t *)bbuf);
            while (dsz > sizeof(zeros))
                dsz -= fwrite(zeros, 1, sizeof(zeros), fout);
            fwrite(zeros, 1, dsz, fout);
            break; }
        default: err = 1; break;
        }
        if (err) break;
    }

    free(bbuf); free(dbuf);
    if (!err) {
        int64_t outSz = _ftelli64(fout);
        wchar_t s[256];
        _snwprintf(s, 256, L"[DONE] %d blocks, %" PRId64 L" bytes\r\n", blocks, outSz);
        PostMessageW(args->hwnd, WM_WORKER_LOG, 0, (LPARAM)_wcsdup(s));
    }
    fclose(fin); fclose(fout);
    if (!err) {
        PostMessageW(args->hwnd, WM_WORKER_PROGRESS, 10000, 0);
        PostMessageW(args->hwnd, WM_WORKER_DONE, 1, (LPARAM)L"// DUMP COMPLETE - Done!");
    } else if (!err) {
        PostMessageW(args->hwnd, WM_WORKER_DONE, 0, (LPARAM)L"Error: FAIL");
    }
    free(args);
    return 0;
}

/* ═══════════ IMG EXTRACT WORKER ═════════════════════════════════════ */
typedef struct {
    wchar_t imgPath[MAX_PATH];
    wchar_t outDir[MAX_PATH];
    HWND    hwnd;
} ImgWorkerArgs;

static void img_worker_progress(double pct, const wchar_t *file, void *ctx)
{
    HWND hwnd = (HWND)ctx;
    int ipct = (int)(pct * 10000);
    PostMessageW(hwnd, WM_WORKER_PROGRESS, ipct, 0);
}

static void img_worker_log(const wchar_t *msg, void *ctx)
{
    HWND hwnd = (HWND)ctx;
    size_t len = wcslen(msg);
    wchar_t *copy = (wchar_t *)malloc((len + 3) * sizeof(wchar_t));
    if (copy) {
        wcscpy(copy, msg);
        wcscat(copy, L"\r\n");
        PostMessageW(hwnd, WM_WORKER_LOG, 0, (LPARAM)copy);
    }
}

static unsigned __stdcall ImgWorkerThread(void *param)
{
    ImgWorkerArgs *args = (ImgWorkerArgs *)param;

    ExtractCallbacks cb;
    cb.on_progress = img_worker_progress;
    cb.on_log      = img_worker_log;
    cb.ctx         = (void *)args->hwnd;

    int result = img_extract(args->imgPath, args->outDir, &cb);

    if (result == 0) {
        PostMessageW(args->hwnd, WM_WORKER_PROGRESS, 10000, 0);
        PostMessageW(args->hwnd, WM_WORKER_DONE, 1,
            (LPARAM)L"// EXTRACTION COMPLETE - Done!");
    } else {
        PostMessageW(args->hwnd, WM_WORKER_DONE, 0,
            (LPARAM)L"Error: FAIL - Extraction failed");
    }

    free(args);
    return 0;
}

/* ═══════════ START OPERATIONS ═══════════════════════════════════════ */
static void StartImgcDump(HWND hwnd)
{
    if (g.working || !g.inputPath[0] || !g.outputFolder[0]) return;

    wchar_t outputPath[MAX_PATH];
    MakeImgcOutputPath(g.inputPath, g.outputFolder, outputPath, MAX_PATH);

    if (GetFileAttributesW(outputPath) != INVALID_FILE_ATTRIBUTES) {
        wchar_t msg[MAX_PATH + 64];
        const wchar_t *fn = wcsrchr(outputPath, L'\\');
        fn = fn ? fn + 1 : outputPath;
        _snwprintf(msg, MAX_PATH + 64, L"\"%s\" exists. Overwrite?", fn);
        if (MessageBoxW(hwnd, msg, L"EVIL CORP - SOUL EXTRACTOR",
                        MB_ICONQUESTION | MB_YESNO) != IDYES)
            return;
    }

    g.working = 1; g.progress = 0.0;
    SetStatus(L"// DUMPING ...");
    SetWindowTextW(g.hLog, L"");
    Log(L"[INPUT]  %s\r\n[OUTPUT] %s\r\n\r\n", g.inputPath, outputPath);
    InvalidateRect(hwnd, NULL, FALSE);

    WorkerArgs *args = (WorkerArgs *)malloc(sizeof(WorkerArgs));
    wcscpy(args->inPath, g.inputPath);
    wcscpy(args->outPath, outputPath);
    args->hwnd = hwnd;
    CloseHandle((HANDLE)_beginthreadex(NULL, 0, ImgcWorkerThread, args, 0, NULL));
}

static void StartImgExtract(HWND hwnd)
{
    if (g.working || !g.inputPath[0] || !g.outputFolder[0]) return;

    g.working = 1; g.progress = 0.0;
    SetStatus(L"// EXTRACTING ...");
    SetWindowTextW(g.hLog, L"");
    Log(L"[INPUT]  %s\r\n[OUTPUT] %s\r\n\r\n", g.inputPath, g.outputFolder);
    InvalidateRect(hwnd, NULL, FALSE);

    ImgWorkerArgs *args = (ImgWorkerArgs *)malloc(sizeof(ImgWorkerArgs));
    wcscpy(args->imgPath, g.inputPath);
    wcscpy(args->outDir, g.outputFolder);
    args->hwnd = hwnd;
    CloseHandle((HANDLE)_beginthreadex(NULL, 0, ImgWorkerThread, args, 0, NULL));
}

/* ═══════════ WNDPROC ════════════════════════════════════════════════ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int logY = MARGIN + 52 + 32 + 8 + 120 + 8 + 36 + 5 + 16 + 5 + 8 + 4 + 18 + 6;
        g.hLog = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            MARGIN + 1, logY + 1, rc.right - 2*MARGIN - 2, rc.bottom - MARGIN - logY - 2,
            hwnd, (HMENU)ID_LOG, GetModuleHandle(NULL), NULL);
        SendMessageW(g.hLog, WM_SETFONT, (WPARAM)g.hFontMono, TRUE);
        CalcLayout(&rc);
        Log(L"// EVIL CORP - SOUL EXTRACTOR v2.0\r\n");
        Log(L"// Modes: IMGC DUMP | IMG EXTRACT\r\n");
        Log(L"// Supported FS: FAT12/16/32, exFAT, NTFS, ext2/3/4, HFS+\r\n\r\n");
        return 0;
    }
    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        CalcLayout(&rc);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC m = CreateCompatibleDC(hdc);
        HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(m, bm);
        PaintAll(hwnd, m);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, m, 0, 0, SRCCOPY);
        SelectObject(m, ob); DeleteObject(bm); DeleteDC(m);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC h = (HDC)wParam;
        SetTextColor(h, CLR_LOG_TEXT);
        SetBkColor(h, CLR_LOG_BG);
        static HBRUSH hBrE = NULL;
        if (!hBrE) hBrE = CreateSolidBrush(CLR_LOG_BG);
        return (LRESULT)hBrE;
    }
    case WM_DROPFILES: {
        if (g.working) break;
        HDROP hDrop = (HDROP)wParam;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(hDrop, 0, path, MAX_PATH))
            SetInputFile(path);
        DragFinish(hDrop);
        g.dropHover = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        int ob = g.browseHover, of = g.folderHover, oa = g.actionHover;
        int oti = g.tabImgcHover, ote = g.tabImgHover;
        g.browseHover   = PtInRc(&g.rcBrowse, x, y);
        g.folderHover   = PtInRc(&g.rcFolder, x, y);
        g.actionHover   = PtInRc(&g.rcAction, x, y);
        g.tabImgcHover  = PtInRc(&g.rcTabImgc, x, y);
        g.tabImgHover   = PtInRc(&g.rcTabImg, x, y);
        if (ob != g.browseHover || of != g.folderHover || oa != g.actionHover ||
            oti != g.tabImgcHover || ote != g.tabImgHover)
            InvalidateRect(hwnd, NULL, FALSE);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g.browseHover = g.folderHover = g.actionHover = 0;
        g.tabImgcHover = g.tabImgHover = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);

        if (PtInRc(&g.rcTabImgc, x, y) && !g.working) {
            g.mode = MODE_IMGC;
            g.inputPath[0] = 0;
            SetStatus(L"// AWAITING INPUT ...");
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (PtInRc(&g.rcTabImg, x, y) && !g.working) {
            g.mode = MODE_IMG;
            g.inputPath[0] = 0;
            SetStatus(L"// AWAITING INPUT ...");
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (PtInRc(&g.rcBrowse, x, y) && !g.working) {
            wchar_t path[MAX_PATH] = {0};
            if (ShowOpenDialog(hwnd, path, MAX_PATH))
                SetInputFile(path);
        }
        else if (PtInRc(&g.rcFolder, x, y) && !g.working) {
            wchar_t folder[MAX_PATH] = {0};
            if (PickFolder(hwnd, folder, MAX_PATH)) {
                wcsncpy(g.outputFolder, folder, MAX_PATH - 1);
                InvalidateRect(hwnd, NULL, FALSE);
                Log(L"[FOLDER] %s\r\n", g.outputFolder);
            }
        }
        else if (PtInRc(&g.rcAction, x, y) && !g.working && g.inputPath[0]) {
            if (!g.outputFolder[0]) {
                MessageBoxW(hwnd, L"Select an output folder first.",
                            L"EVIL CORP", MB_ICONWARNING);
            } else if (g.mode == MODE_IMGC) {
                StartImgcDump(hwnd);
            } else {
                StartImgExtract(hwnd);
            }
        }
        else if (PtInRc(&g.rcDrop, x, y) && !g.working) {
            wchar_t path[MAX_PATH] = {0};
            if (ShowOpenDialog(hwnd, path, MAX_PATH))
                SetInputFile(path);
        }
        return 0;
    }
    case WM_WORKER_PROGRESS: {
        g.progress = (double)(int)wParam / 10000.0;
        wchar_t buf[64];
        const wchar_t *verb = (g.mode == MODE_IMGC) ? L"DUMPING" : L"EXTRACTING";
        _snwprintf(buf, 64, L"// %s ... %.1f%%", verb, g.progress * 100.0);
        wcscpy(g.statusText, buf);
        InvalidateRect(hwnd, &g.rcProgress, FALSE);
        InvalidateRect(hwnd, &g.rcStatus, FALSE);
        return 0;
    }
    case WM_WORKER_DONE: {
        g.working = 0;
        wchar_t *m = (wchar_t *)lParam;
        if (m) { SetStatus(L"%s", m); Log(L"\r\n%s\r\n", m); }
        if (wParam) g.progress = 1.0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_WORKER_LOG: {
        wchar_t *t = (wchar_t *)lParam;
        if (t) { Log(L"%s", t); free(t); }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
            if (PtInRc(&g.rcBrowse, pt.x, pt.y) || PtInRc(&g.rcFolder, pt.x, pt.y) ||
                PtInRc(&g.rcAction, pt.x, pt.y) || PtInRc(&g.rcDrop, pt.x, pt.y) ||
                PtInRc(&g.rcTabImgc, pt.x, pt.y) || PtInRc(&g.rcTabImg, pt.x, pt.y)) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
