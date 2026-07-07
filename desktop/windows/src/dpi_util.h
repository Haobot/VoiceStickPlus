#pragma once

#include <Windows.h>
#include <ShellScalingApi.h>

#pragma comment(lib, "Shcore.lib")

namespace voicestick {

inline UINT GetFallbackDpi() {
    HDC dc = GetDC(nullptr);
    UINT dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return dpi != 0 ? dpi : 96;
}

inline UINT GetDpiForHwnd(HWND hwnd) {
    if (hwnd) {
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi != 0) return dpi;
    }
    return GetFallbackDpi();
}

inline UINT GetDpiForMonitorHandle(HMONITOR monitor) {
    UINT dpi_x = 0;
    UINT dpi_y = 0;
    if (monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x != 0) {
        return dpi_x;
    }
    return GetFallbackDpi();
}

inline int ScalePx(int px, UINT dpi) {
    return MulDiv(px, static_cast<int>(dpi), 96);
}

inline float ScaleF(int px, UINT dpi) {
    return static_cast<float>(ScalePx(px, dpi));
}

inline RECT GetWorkAreaForMonitor(HMONITOR monitor) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
        return mi.rcWork;
    }
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

inline HMONITOR GetCursorMonitor() {
    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        return MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    }
    return MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
}

inline RECT GetWorkAreaForWindow(HWND hwnd) {
    return GetWorkAreaForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY));
}

inline RECT GetWorkAreaForCursor() {
    return GetWorkAreaForMonitor(GetCursorMonitor());
}

inline UINT GetDpiForCursorMonitor() {
    return GetDpiForMonitorHandle(GetCursorMonitor());
}

inline HFONT CreateUiFont(UINT dpi) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    return reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

// 与 CreateUiFont 同源，仅把字重置为 FW_BOLD，用于分组标题等强调文本。
inline HFONT CreateUiFontBold(UINT dpi) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        LOGFONTW lf = metrics.lfMessageFont;
        lf.lfWeight = FW_BOLD;
        return CreateFontIndirectW(&lf);
    }
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        LOGFONTW lf = metrics.lfMessageFont;
        lf.lfWeight = FW_BOLD;
        return CreateFontIndirectW(&lf);
    }
    return reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

} // namespace voicestick
