// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "ui/components/AppIcon.h"

#include <objbase.h>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

namespace ui {

// SVG-path builder supporting M/L/H/V/C/S (+ relative) and Z, with repeated argument sets.
void BuildSvgPath(Gdiplus::GraphicsPath* path, const char* d) {
    const char* p = d;
    auto skipsep = [&]() {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') { ++p; }
    };
    auto readNum = [&](float& out) -> bool {
        skipsep();
        char ch = *p;
        if (!(ch == '-' || ch == '+' || ch == '.' || (ch >= '0' && ch <= '9'))) { return false; }
        char* end = nullptr;
        out = strtof(p, &end);
        if (end == p) { return false; }
        p = end;
        return true;
    };
    char cmd = 0;
    float cx = 0, cy = 0, sx = 0, sy = 0;  // current point, subpath start
    float c2x = 0, c2y = 0;                // previous cubic's 2nd control point (for S)
    bool prevCubic = false;
    while (*p) {
        skipsep();
        if (*p == 0) { break; }
        char ch = *p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            cmd = ch;
            ++p;
        }
        bool rel = (cmd >= 'a' && cmd <= 'z');
        char c = rel ? static_cast<char>(cmd - 'a' + 'A') : cmd;
        bool thisCubic = false;
        if (c == 'M') {
            float x, y;
            if (!readNum(x) || !readNum(y)) { break; }
            if (rel) { x += cx; y += cy; }
            cx = x; cy = y; sx = x; sy = y;
            path->StartFigure();
            cmd = rel ? 'l' : 'L';
        } else if (c == 'L') {
            float x, y;
            if (!readNum(x) || !readNum(y)) { break; }
            if (rel) { x += cx; y += cy; }
            path->AddLine(cx, cy, x, y);
            cx = x; cy = y;
        } else if (c == 'H') {
            float x;
            if (!readNum(x)) { break; }
            if (rel) { x += cx; }
            path->AddLine(cx, cy, x, cy);
            cx = x;
        } else if (c == 'V') {
            float y;
            if (!readNum(y)) { break; }
            if (rel) { y += cy; }
            path->AddLine(cx, cy, cx, y);
            cy = y;
        } else if (c == 'C') {
            float x1, y1, x2, y2, x, y;
            if (!readNum(x1) || !readNum(y1) || !readNum(x2) || !readNum(y2) || !readNum(x) || !readNum(y)) { break; }
            if (rel) { x1 += cx; y1 += cy; x2 += cx; y2 += cy; x += cx; y += cy; }
            path->AddBezier(cx, cy, x1, y1, x2, y2, x, y);
            c2x = x2; c2y = y2; cx = x; cy = y; thisCubic = true;
        } else if (c == 'S') {
            float x2, y2, x, y;
            if (!readNum(x2) || !readNum(y2) || !readNum(x) || !readNum(y)) { break; }
            if (rel) { x2 += cx; y2 += cy; x += cx; y += cy; }
            float x1 = prevCubic ? (2 * cx - c2x) : cx;
            float y1 = prevCubic ? (2 * cy - c2y) : cy;
            path->AddBezier(cx, cy, x1, y1, x2, y2, x, y);
            c2x = x2; c2y = y2; cx = x; cy = y; thisCubic = true;
        } else if (c == 'Z') {
            path->CloseFigure();
            cx = sx; cy = sy;
        } else {
            break;
        }
        prevCubic = thisCubic;
    }
}

namespace {

Gdiplus::GraphicsPath* g_iconP1 = nullptr;  // #b457db (light purple)
Gdiplus::GraphicsPath* g_iconP2 = nullptr;  // #7d3c98 (dark purple)
Gdiplus::GraphicsPath* g_iconP3 = nullptr;  // #afff33 (lime check)
const float kIconVbW = 51.9f, kIconVbH = 59.0f;

void EnsureIconPaths() {
    if (g_iconP1 != nullptr) {
        return;
    }
    static const char* kP1 = "M25.95,5.29L4.73,14.67v9.14s.19,2.97.19,2.97c.44,5.68,1.91,11.36,4.98,16.15,3.66,5.7,9.31,9.73,15.96,11.08.03,0,.06-.01.09-.02v5.01c-1.48-.18-2.78-.55-4.2-1.02-5.81-1.94-10.85-5.63-14.5-10.57-2.63-3.55-4.41-7.55-5.57-11.81C.62,31.67.18,27.73.03,23.65l-.03-12.15L25.95.01v5.28Z";
    static const char* kP2 = "M25.97,5.28h-.02S25.95.01,25.95.01h.02s17.17,7.6,17.17,7.6l-3.77,3.62-13.4-5.95ZM47.16,19.23l-.07,6.08-.08,1.19c-.31,4.92-1.51,9.91-3.72,14.22-2.64,5.14-6.83,9.25-12.1,11.63-1.71.78-3.4,1.34-5.24,1.64v5.01h.01c1.56-.2,2.98-.59,4.47-1.1,7.09-2.44,12.87-7.43,16.5-13.99,3.39-6.11,4.69-12.97,4.96-19.92v-12.13s-4.73,7.37-4.73,7.37Z";
    static const char* kP3 = "M24.86,28.82c.25.13.64.34.82.17l2.35-2.29,2.17-2.03,4.23-4.01,2.87-2.77,2.39-2.26,2.75-2.65,1.03-.91c.39-.34.69-.7,1.07-1.07l2.41-2.31,1.92-1.81c.39-.37.93-.37,1.27-.06.39.36.4.8.07,1.31l-20.63,32.08c-.42.65-.7,1.2-1.29,1.71-.83.72-1.9.97-3.07.73-.93-.19-1.63-.93-2.27-1.8l-4.25-5.78-.65-.85-8.08-10.88c-.29-.39-.48-.73-.32-1.17.11-.3.59-.74,1-.54l5.05,2.53,9.15,4.67Z";
    g_iconP1 = new Gdiplus::GraphicsPath(Gdiplus::FillModeWinding);
    g_iconP2 = new Gdiplus::GraphicsPath(Gdiplus::FillModeWinding);
    g_iconP3 = new Gdiplus::GraphicsPath(Gdiplus::FillModeWinding);
    BuildSvgPath(g_iconP1, kP1);
    BuildSvgPath(g_iconP2, kP2);
    BuildSvgPath(g_iconP3, kP3);
}

// Fill the icon into a GDI+ surface, fit (preserving aspect) and centered in (lx,ly,lw,lh).
void PaintIconInto(Gdiplus::Graphics& g, float lx, float ly, float lw, float lh) {
    EnsureIconPaths();
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float s = (lw / kIconVbW < lh / kIconVbH) ? (lw / kIconVbW) : (lh / kIconVbH);
    float drawnW = kIconVbW * s, drawnH = kIconVbH * s;
    Gdiplus::Matrix mtx;
    mtx.Translate(lx + (lw - drawnW) / 2.0f, ly + (lh - drawnH) / 2.0f);
    mtx.Scale(s, s);
    g.SetTransform(&mtx);
    Gdiplus::SolidBrush b1(Gdiplus::Color(255, 0xB4, 0x57, 0xDB));
    Gdiplus::SolidBrush b2(Gdiplus::Color(255, 0x7D, 0x3C, 0x98));
    Gdiplus::SolidBrush b3(Gdiplus::Color(255, 0xAF, 0xFF, 0x33));
    if (g_iconP1) { g.FillPath(&b1, g_iconP1); }
    if (g_iconP2) { g.FillPath(&b2, g_iconP2); }
    if (g_iconP3) { g.FillPath(&b3, g_iconP3); }
    g.ResetTransform();
}

// Find the CLSID of a GDI+ image encoder by MIME type (e.g. L"image/png").
int GetEncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) { return -1; }
    auto* info = static_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
    if (!info) { return -1; }
    Gdiplus::GetImageEncoders(num, size, info);
    int found = -1;
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(info[j].MimeType, mime) == 0) {
            *clsid = info[j].Clsid;
            found = static_cast<int>(j);
            break;
        }
    }
    free(info);
    return found;
}

// Render the app icon at the given pixel size to a PNG byte blob.
bool RenderIconPng(int size, const CLSID& pngClsid, std::vector<BYTE>& out) {
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    PaintIconInto(g, 0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size));

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) { return false; }
    bool ok = (bmp.Save(stream, &pngClsid, nullptr) == Gdiplus::Ok);
    if (ok) {
        HGLOBAL hg = nullptr;
        if (SUCCEEDED(GetHGlobalFromStream(stream, &hg)) && hg) {
            SIZE_T n = GlobalSize(hg);
            STATSTG stat;
            ZeroMemory(&stat, sizeof(stat));
            if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))) {
                n = static_cast<SIZE_T>(stat.cbSize.QuadPart);
            }
            out.resize(n);
            void* p = GlobalLock(hg);
            if (p) {
                memcpy(out.data(), p, n);
                GlobalUnlock(hg);
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
    }
    stream->Release();
    return ok;
}

}  // namespace

// Draw the app icon onto an HDC, centered in the box.
void DrawAppIcon(HDC dc, int lx, int ly, int lw, int lh) {
    Gdiplus::Graphics g(dc);
    PaintIconInto(g, static_cast<float>(lx), static_cast<float>(ly),
                  static_cast<float>(lw), static_cast<float>(lh));
}

// Create an HICON of the app icon at the given pixel size (transparent background).
HICON CreateAppIcon(int size) {
    if (size < 8) { size = 8; }
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    PaintIconInto(g, 0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size));
    HICON hic = nullptr;
    bmp.GetHICON(&hic);
    return hic;
}

// Export a multi-size .ico (PNG-compressed entries, Vista+) of the app icon to path.
bool ExportIco(const wchar_t* path) {
    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0) { return false; }

    const int sizes[] = { 256, 64, 48, 32, 24, 16 };
    const int count = static_cast<int>(sizeof(sizes) / sizeof(sizes[0]));
    std::vector<std::vector<BYTE>> blobs(count);
    for (int i = 0; i < count; ++i) {
        if (!RenderIconPng(sizes[i], pngClsid, blobs[i])) { return false; }
    }

    std::vector<BYTE> file;
    auto putW = [&](WORD v) {
        file.push_back(static_cast<BYTE>(v & 0xFF));
        file.push_back(static_cast<BYTE>((v >> 8) & 0xFF));
    };
    auto putD = [&](DWORD v) {
        for (int k = 0; k < 4; ++k) { file.push_back(static_cast<BYTE>((v >> (8 * k)) & 0xFF)); }
    };

    // ICONDIR
    putW(0);                       // reserved
    putW(1);                       // type = icon
    putW(static_cast<WORD>(count));
    // ICONDIRENTRYs (16 bytes each); image data follows all entries.
    DWORD offset = 6 + static_cast<DWORD>(count) * 16;
    for (int i = 0; i < count; ++i) {
        int s = sizes[i];
        file.push_back(static_cast<BYTE>(s >= 256 ? 0 : s));  // width  (0 => 256)
        file.push_back(static_cast<BYTE>(s >= 256 ? 0 : s));  // height (0 => 256)
        file.push_back(0);                                    // color count
        file.push_back(0);                                    // reserved
        putW(1);                                              // planes
        putW(32);                                             // bit count
        putD(static_cast<DWORD>(blobs[i].size()));            // bytes in resource
        putD(offset);                                         // offset to image
        offset += static_cast<DWORD>(blobs[i].size());
    }
    for (int i = 0; i < count; ++i) {
        file.insert(file.end(), blobs[i].begin(), blobs[i].end());
    }

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return false; }
    DWORD written = 0;
    BOOL ok = WriteFile(h, file.data(), static_cast<DWORD>(file.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == file.size();
}

}  // namespace ui
