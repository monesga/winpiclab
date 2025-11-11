// piclab.cpp
// Win32 single-file tool to add a bottom label to a PNG via "Open with" in Explorer.
// Features:
//  - Prompts for label text.
//  - Asks to Overwrite (Yes) or Save a Copy (No -> "<name>_labeled.png").
//  - Translucent black scrim + white text.
//  - Strong diagnostics and Explorer refresh.
//
// Build:
//   cl /EHsc /W4 piclab.cpp gdiplus.lib user32.lib gdi32.lib comdlg32.lib shlwapi.lib shell32.lib

#define NOMINMAX
#include <algorithm>
#include <windows.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <shellapi.h> // CommandLineToArgvW
#include <shlobj.h>   // SHChangeNotify
#include <string>
#include <vector>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

using namespace Gdiplus;

// ----------------------------- Helpers -----------------------------

static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static void MsgBox(HWND parent, const std::wstring& text, UINT type = MB_OK | MB_ICONINFORMATION) {
    MessageBoxW(parent, text.c_str(), L"PNG Labeler", type);
}

static std::wstring LastErrorText(DWORD err = GetLastError()) {
    LPWSTR buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    FormatMessageW(flags, nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if (buf) LocalFree(buf);
    return s;
}

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> mem(size);
    ImageCodecInfo* enc = reinterpret_cast<ImageCodecInfo*>(mem.data());
    if (GetImageEncoders(num, size, enc) != Ok) return -1;
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(enc[j].MimeType, format) == 0) {
            *pClsid = enc[j].Clsid;
            return (int)j;
        }
    }
    return -1;
}

static std::wstring PathWithSuffixBeforeExt(const std::wstring& path, const std::wstring& suffix) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");

    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return path + suffix; // no extension
    }
    std::wstring out = path;
    out.insert(dot, suffix);
    return out;
}

static std::wstring GetTempSiblingPath(const std::wstring& original) {
    WCHAR drive[_MAX_DRIVE]{}, dir[_MAX_DIR]{}, fname[_MAX_FNAME]{}, ext[_MAX_EXT]{};
    _wsplitpath_s(original.c_str(), drive, _MAX_DRIVE, dir, _MAX_DIR, fname, _MAX_FNAME, ext, _MAX_EXT);
    WCHAR folder[MAX_PATH]{};
    _wmakepath_s(folder, drive, dir, L"", L"");
    WCHAR tmp[MAX_PATH];
    swprintf_s(tmp, L"%s%s_label_tmp_%u.png", folder, fname, GetTickCount());
    return std::wstring(tmp);
}

static void RefreshShellFor(const std::wstring& path) {
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, path.c_str(), nullptr);
}

// ----------------------------- Label Prompt -----------------------------

struct InputState {
    HWND hWnd{};
    HWND hEdit{};
    std::wstring result;
    bool accepted{false};
};

static LRESULT CALLBACK InputWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    InputState* st = reinterpret_cast<InputState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        st = reinterpret_cast<InputState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        CreateWindowW(L"STATIC", L"Enter label text:",
            WS_CHILD | WS_VISIBLE, 12, 12, 360, 18, hWnd, nullptr, cs->hInstance, nullptr);

        st->hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            12, 34, 360, 24, hWnd, (HMENU)1001, cs->hInstance, nullptr);

        HWND hOk = CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            216, 70, 72, 26, hWnd, (HMENU)IDOK, cs->hInstance, nullptr);

        HWND hCancel = CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE, 300, 70, 72, 26, hWnd, (HMENU)IDCANCEL, cs->hInstance, nullptr);

        SendMessageW(st->hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hOk,       WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCancel,   WM_SETFONT, (WPARAM)hFont, TRUE);

        SetFocus(st->hEdit);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            WCHAR buf[1024];
            GetWindowTextW(st->hEdit, buf, 1024);
            st->result = Trim(buf);
            st->accepted = !st->result.empty();
            DestroyWindow(hWnd);
            return 0;
        }
        case IDCANCEL:
            st->accepted = false;
            DestroyWindow(hWnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        st->accepted = false;
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static bool PromptForText(HINSTANCE hInst, std::wstring& out) {
    const wchar_t* kClass = L"PNGLabelerInputWndClass";
    WNDCLASSW wc{};
    wc.lpfnWndProc = InputWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    InputState st{};
    HWND hWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"Add Label",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 140,
        nullptr, nullptr, hInst, &st);
    if (!hWnd) return false;

    // Center
    RECT rc; GetWindowRect(hWnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    SetWindowPos(hWnd, nullptr, sx, sy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (st.accepted) out = st.result;
    return st.accepted;
}

// ----------------------------- Image Processing -----------------------------

static bool ProcessAndSave(const std::wstring& srcPath,
                           const std::wstring& label,
                           bool overwrite,
                           std::wstring& outSavedPath,
                           std::wstring& outError) {
    GdiplusStartupInput gsi;
    ULONG_PTR token;
    if (GdiplusStartup(&token, &gsi, nullptr) != Ok) {
        outError = L"GDI+ startup failed.";
        return false;
    }

    bool ok = false;
    Bitmap* bmp = Bitmap::FromFile(srcPath.c_str(), FALSE);
    if (!bmp || bmp->GetLastStatus() != Ok) {
        outError = L"Failed to load image. Is it a valid PNG?";
        delete bmp;
        GdiplusShutdown(token);
        return false;
    }

    UINT w = bmp->GetWidth(), h = bmp->GetHeight();
    Graphics g(bmp);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    REAL pad    = (REAL)std::max<double>(8.0,  (double)h * 0.012);
    REAL fontPt = (REAL)std::max<double>(10.0, (double)h * 0.042); // ~4.2% of height

    const WCHAR* family = L"Segoe UI";
    {
        FontFamily test(L"Segoe UI");
        if (!test.IsAvailable()) family = L"Arial";
    }
    Font font(family, fontPt, FontStyleBold, UnitPoint);

    StringFormat sf(StringFormatFlagsNoClip);
    sf.SetAlignment(StringAlignmentNear);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);

    RectF layoutRect(0, 0, (REAL)w - 2 * pad, 1000);
    RectF bounds{};
    g.MeasureString(label.c_str(), (INT)label.size(), &font, layoutRect, &sf, &bounds);

    REAL scrimH = bounds.Height + 2 * pad;
    scrimH = (REAL)std::max<double>(scrimH, std::max<double>(h * 0.05, 18.0)); // min ~5% or 18px
    scrimH = (REAL)std::min<double>(scrimH, h * 0.15); 
   SolidBrush scrim(Color(120, 0, 0, 0)); // ~47% black
    RectF scrimRect(0, (REAL)h - scrimH, (REAL)w, scrimH);
    g.FillRectangle(&scrim, scrimRect);

    SolidBrush white(Color(255, 255, 255, 255));
    SolidBrush shadow(Color(160, 0, 0, 0));

    RectF textRect(pad, (REAL)h - scrimH + pad, (REAL)w - 2 * pad, scrimH - 2 * pad);
    RectF shadowRect = textRect; shadowRect.X += 1; shadowRect.Y += 1;
    g.DrawString(label.c_str(), (INT)label.size(), &font, shadowRect, &sf, &shadow);
    g.DrawString(label.c_str(), (INT)label.size(), &font, textRect,   &sf, &white);

    CLSID pngClsid{};
    if (GetEncoderClsid(L"image/png", &pngClsid) == -1) {
        outError = L"PNG encoder not found (GDI+).";
        delete bmp;
        GdiplusShutdown(token);
        return false;
    }

    if (overwrite) {
        // Save to temp then atomically replace original
        std::wstring tmp = GetTempSiblingPath(srcPath);
        Status s = bmp->Save(tmp.c_str(), &pngClsid, nullptr);
        if (s != Ok) {
            outError = L"Save to temp failed (status " + std::to_wstring(s) + L").";
        } else {
            if (MoveFileExW(tmp.c_str(), srcPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                ok = true;
                outSavedPath = srcPath;
            } else {
                DWORD e = GetLastError();
                DeleteFileW(tmp.c_str());
                outError = L"Replace original failed. Win32 error " + std::to_wstring(e) + L": " + LastErrorText(e);
            }
        }
    } else {
        // Save as a side-by-side copy
        std::wstring dst = PathWithSuffixBeforeExt(srcPath, L"_labeled");
        Status s = bmp->Save(dst.c_str(), &pngClsid, nullptr);
        if (s == Ok) {
            ok = true;
            outSavedPath = dst;
        } else {
            outError = L"Save copy failed (status " + std::to_wstring(s) + L").";
        }
    }

    delete bmp;
    GdiplusShutdown(token);
    return ok;
}

// ----------------------------- Entry -----------------------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2) {
        MsgBox(nullptr, L"Usage:\n  piclab.exe <image.png>");
        return 1;
    }

    std::wstring path = argv[1];
    LocalFree(argv);

    if (!PathFileExistsW(path.c_str())) {
        MsgBox(nullptr, L"File not found:\n" + path, MB_OK | MB_ICONERROR);
        return 2;
    }

    std::wstring label;
    if (!PromptForText(hInst, label)) {
        // User cancelled or empty label
        return 0;
    }

    // Ask whether to overwrite or save a copy
    int choice = MessageBoxW(nullptr,
        (L"Add label:\n\n  \"" + label + L"\"\n\n"
         L"Yes = Overwrite original\n"
         L"No  = Save a copy (\"*_labeled.png\")\n"
         L"Cancel = Abort").c_str(),
        L"PNG Labeler",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);

    if (choice == IDCANCEL) return 0;
    bool overwrite = (choice == IDYES);

    std::wstring savedPath, err;
    bool ok = ProcessAndSave(path, label, overwrite, savedPath, err);
    if (!ok) {
        MsgBox(nullptr, L"Failed to write image:\n" + err, MB_OK | MB_ICONERROR);
        return 3;
    }

    RefreshShellFor(savedPath);
    MsgBox(nullptr, L"Saved:\n" + savedPath, MB_OK | MB_ICONINFORMATION);
    return 0;
}