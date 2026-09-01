#include "battery_monitor_dialog.h"

#include "dpi_util.h"

#include <Windows.h>
#include <commdlg.h>

#include <gdiplus.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace voicestick {

namespace {

std::wstring Utf16(std::string_view text) {
    if (text.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), len);
    return wide;
}

// {0}/{1}/... 占位符替换（标题与状态文案含设备 ID、计数等参数）。
std::wstring FormatText(std::wstring text, std::initializer_list<std::wstring> values) {
    int index = 0;
    for (const auto& value : values) {
        const std::wstring placeholder = L"{" + std::to_wstring(index++) + L"}";
        std::size_t pos = 0;
        while ((pos = text.find(placeholder, pos)) != std::wstring::npos) {
            text.replace(pos, placeholder.size(), value);
            pos += value.size();
        }
    }
    return text;
}

void AlignDialogData(std::vector<BYTE>* buffer, std::size_t alignment) {
    while (buffer->size() % alignment != 0) buffer->push_back(0);
}

void AppendDialogData(std::vector<BYTE>* buffer, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const BYTE*>(data);
    buffer->insert(buffer->end(), bytes, bytes + size);
}

void AppendDialogWord(std::vector<BYTE>* buffer, WORD value) {
    AppendDialogData(buffer, &value, sizeof(value));
}

void AppendDialogWideString(std::vector<BYTE>* buffer, const wchar_t* text) {
    while (text && *text) {
        AppendDialogWord(buffer, static_cast<WORD>(*text));
        ++text;
    }
    AppendDialogWord(buffer, 0);
}

// 导出文件默认名：battery_VS-XXXX_YYYYMMDD_HHMMSS.<ext>。
std::wstring DefaultExportFileName(const std::string& device_id, const wchar_t* ext) {
    const std::time_t t = std::time(nullptr);
    std::tm tm_value{};
    localtime_s(&tm_value, &t);
    wchar_t stamp[24]{};
    swprintf_s(stamp, L"%04d%02d%02d_%02d%02d%02d", tm_value.tm_year + 1900,
               tm_value.tm_mon + 1, tm_value.tm_mday, tm_value.tm_hour,
               tm_value.tm_min, tm_value.tm_sec);
    return L"battery_VS-" + Utf16(device_id) + L"_" + stamp + ext;
}

// GDI+ PNG 编码器 CLSID（gdiplus 无 WIC 依赖，直接按 MIME 类型查）。
bool GetPngEncoderClsid(CLSID* clsid) {
    UINT count = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0) return false;
    std::vector<BYTE> buffer(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, size, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

} // namespace

BatteryMonitorDialog::BatteryMonitorDialog(HINSTANCE instance, HWND parent,
                                           UiLanguage language, std::string device_id)
    : instance_(instance), parent_(parent), language_(language),
      device_id_(std::move(device_id)) {}

BatteryMonitorDialog::~BatteryMonitorDialog() {
    // 析构路径（应用退出等）：置 closing_ 防止 WM_DESTROY 回调 on_closed
    // 再次释放本对象。
    closing_ = true;
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void BatteryMonitorDialog::Show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        return;
    }
    hwnd_ = CreateDialogIndirectParamW(instance_, BuildDialogTemplate(), parent_,
                                       BatteryMonitorDialog::DialogProc,
                                       reinterpret_cast<LPARAM>(this));
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

bool BatteryMonitorDialog::IsSameDevice(const std::string& device_id) const {
    return device_id_ == device_id;
}

// ---------------------------------------------------------------- 监测会话状态机

void BatteryMonitorDialog::StartMonitoring() {
    if (state_ == MonitorState::kAnchoring || state_ == MonitorState::kProbing ||
        state_ == MonitorState::kMonitoring) {
        return;
    }
    accumulator_ = PowerLogAccumulator();
    log_size_ = 0;
    cycle_ = 0;
    attempt_ = 0;
    failed_cycles_ = 0;
    dump_active_ = false;
    pending_blob_.clear();
    status_override_.clear();
    state_ = MonitorState::kAnchoring;
    anchor_step_ = 1;
    // 先清空设备端环形区：旧固件在环形区写满后对 offset==total 的增量 dump 不回包，
    // 清空可保证之后以 offset==log_size_ 的增量导出始终有响应（代价是丢弃历史功耗记录）。
    SendCommand(BleProtocol::PowerLogClearPayload());
    SetTimer(hwnd_, kTimerAnchorStep, kAnchorStepMs, nullptr);
    RefreshStatus();
    UpdateButtons();
}

void BatteryMonitorDialog::StopMonitoring() {
    KillSessionTimers();
    dump_active_ = false;
    pending_blob_.clear();
    status_override_.clear();
    state_ = MonitorState::kIdle;
    RefreshStatus();
    UpdateButtons();
}

void BatteryMonitorDialog::AbortWithError(StringId reason) {
    KillSessionTimers();
    dump_active_ = false;
    pending_blob_.clear();
    status_override_.clear();
    error_reason_ = reason;
    state_ = MonitorState::kError;
    RefreshStatus();
    UpdateButtons();
}

void BatteryMonitorDialog::FinishMonitoring() {
    KillSessionTimers();
    dump_active_ = false;
    pending_blob_.clear();
    state_ = MonitorState::kFinished;
    RefreshStatus();
    UpdateButtons();
}

void BatteryMonitorDialog::BeginProbing() {
    state_ = MonitorState::kProbing;
    // 越界 offset 探测：设备把 offset 钳到 total 并立即回 eof 空片，据此拿到日志
    // 当前大小作为增量导出基线，避免全量导出历史日志（最大 256KB，走 BLE 约 50s）。
    SendCommand(BleProtocol::PowerLogDumpPayload(kProbeOffset, 16));
    SetTimer(hwnd_, kTimerProbeTimeout, kProbeTimeoutMs, nullptr);
    RefreshStatus();
    UpdateButtons();
}

void BatteryMonitorDialog::ScheduleNextCycle() {
    next_cycle_tick_ = GetTickCount64() + kCycleIntervalMs;
    SetTimer(hwnd_, kTimerCycle, kCycleIntervalMs, nullptr);
}

void BatteryMonitorDialog::StartDumpAttempt() {
    ++attempt_;
    pending_blob_.clear();
    expected_offset_ = log_size_;
    dump_active_ = true;
    SendCommand(BleProtocol::PowerLogDumpPayload(log_size_, kDumpChunkMax));
    SetTimer(hwnd_, kTimerDumpTimeout, kDumpTimeoutMs, nullptr);
    RefreshStatus();
}

void BatteryMonitorDialog::HandleDumpFailure() {
    KillTimer(hwnd_, kTimerDumpTimeout);
    dump_active_ = false;
    pending_blob_.clear();
    if (attempt_ < kMaxAttemptsPerCycle) {
        StartDumpAttempt();  // 周期内立即重试
        return;
    }
    ++failed_cycles_;
    if (failed_cycles_ >= kMaxFailedCycles) {
        AbortWithError(StringId::kBatteryMonitorErrDumpTimeout);
        return;
    }
    ScheduleNextCycle();  // 放弃本周期，等下一周期
    RefreshStatus();
}

void BatteryMonitorDialog::FinishCycle(std::uint32_t total) {
    if (total < log_size_) {
        // total 回退：设备端日志被清空或覆盖。
        AbortWithError(StringId::kBatteryMonitorErrCleared);
        return;
    }
    std::vector<PowerLogSample> new_samples;
    if (!accumulator_.ConsumeIncrementalBlob(pending_blob_.data(), pending_blob_.size(),
                                             &new_samples)) {
        // uptime 回退：设备重启，时间连续性被破坏。
        AbortWithError(StringId::kBatteryMonitorErrRestart);
        return;
    }
    log_size_ = total;
    failed_cycles_ = 0;
    pending_blob_.clear();
    if (cycle_ >= kTotalCycles) {
        FinishMonitoring();
        return;
    }
    ScheduleNextCycle();
    RefreshStatus();
    UpdateButtons();  // 采样数变化后刷新导出按钮可用态
}

void BatteryMonitorDialog::KillSessionTimers() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kTimerAnchorStep);
    KillTimer(hwnd_, kTimerProbeTimeout);
    KillTimer(hwnd_, kTimerCycle);
    KillTimer(hwnd_, kTimerDumpTimeout);
    KillTimer(hwnd_, kTimerStatusTick);
}

void BatteryMonitorDialog::SendCommand(ByteVector payload) {
    if (on_send_command) on_send_command(device_id_, std::move(payload));
}

// ---------------------------------------------------------------- 设备回调（UI 线程）

void BatteryMonitorDialog::OnPowerLogFragment(const std::string& device_id,
                                              const PowerLogFragment& fragment) {
    if (!IsSameDevice(device_id) || !hwnd_) return;
    switch (state_) {
    case MonitorState::kProbing:
        // 探测响应为单片 eof 空包；非 eof 分片（异常固件）等超时兜底。
        if (!fragment.eof) return;
        KillTimer(hwnd_, kTimerProbeTimeout);
        log_size_ = fragment.total;
        state_ = MonitorState::kMonitoring;
        cycle_ = 0;
        ScheduleNextCycle();  // 首个采集周期 60s 后
        SetTimer(hwnd_, kTimerStatusTick, 1000, nullptr);  // 1s 倒计时刷新
        RefreshStatus();
        UpdateButtons();
        return;
    case MonitorState::kMonitoring:
        if (!dump_active_) return;  // 周期外到达的分片（前次会话残留等）忽略
        if (fragment.offset != expected_offset_) {
            // 丢片/乱序：本次 dump 作废，按超时同路径重试。
            HandleDumpFailure();
            return;
        }
        pending_blob_.insert(pending_blob_.end(), fragment.data.begin(), fragment.data.end());
        expected_offset_ += static_cast<std::uint32_t>(fragment.data.size());
        if (fragment.eof) {
            dump_active_ = false;
            KillTimer(hwnd_, kTimerDumpTimeout);
            FinishCycle(fragment.total);
        }
        return;
    default:
        return;
    }
}

void BatteryMonitorDialog::OnPowerMgmtState(const std::string& device_id, bool usb_auto_off) {
    if (!IsSameDevice(device_id)) return;
    usb_auto_off_state_ = usb_auto_off;
    if (usb_auto_off_check_) {
        SendMessageW(usb_auto_off_check_, BM_SETCHECK,
                     usb_auto_off ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    UpdateUsbWarning();
}

void BatteryMonitorDialog::NotifyAllDisconnected() {
    // 监测中设备断连立即中止；非监测态无需处理。
    if (state_ == MonitorState::kAnchoring || state_ == MonitorState::kProbing ||
        state_ == MonitorState::kMonitoring) {
        AbortWithError(StringId::kBatteryMonitorErrDisconnected);
    }
}

// ---------------------------------------------------------------- 状态显示

void BatteryMonitorDialog::RefreshStatus() {
    if (!status_label_) return;
    const auto language = EffectiveUiLanguage(language_);
    std::wstring text;
    if (!status_override_.empty() && state_ != MonitorState::kMonitoring) {
        text = status_override_;  // 保存结果等瞬态文案（监测中让位于周期倒计时）
    } else {
        switch (state_) {
        case MonitorState::kIdle:
            text = TrW(StringId::kBatteryMonitorStatusIdle, language);
            break;
        case MonitorState::kAnchoring:
            text = TrW(StringId::kBatteryMonitorStatusAnchoring, language);
            break;
        case MonitorState::kProbing:
            text = TrW(StringId::kBatteryMonitorStatusProbing, language);
            break;
        case MonitorState::kMonitoring:
            text = FormatText(TrW(StringId::kBatteryMonitorStatusMonitoring, language),
                              {std::to_wstring(cycle_),
                               std::to_wstring(accumulator_.samples().size()),
                               FormatCountdownText()});
            break;
        case MonitorState::kFinished:
            text = FormatText(TrW(StringId::kBatteryMonitorStatusFinished, language),
                              {std::to_wstring(accumulator_.samples().size())});
            break;
        case MonitorState::kError:
            text = FormatText(TrW(StringId::kBatteryMonitorStatusError, language),
                              {TrW(error_reason_, language)});
            break;
        }
    }
    SetText(status_label_, text);
    UpdateUsbWarning();
}

void BatteryMonitorDialog::UpdateButtons() {
    const bool running = state_ == MonitorState::kAnchoring ||
                         state_ == MonitorState::kProbing ||
                         state_ == MonitorState::kMonitoring;
    if (start_button_) EnableWindow(start_button_, !running);
    if (stop_button_) EnableWindow(stop_button_, running);
    const bool has_samples = !accumulator_.samples().empty();
    if (export_csv_button_) EnableWindow(export_csv_button_, has_samples);
    if (export_png_button_) EnableWindow(export_png_button_, has_samples);
}

void BatteryMonitorDialog::UpdateUsbWarning() {
    if (!warn_label_) return;
    // USB 供电时提示保持供电（电池供电下设备空闲约 10 分钟会自动关机）；
    // 供电态自动关机开关打开时同样提示（USB 下也会 10 分钟关机）。
    const auto& samples = accumulator_.samples();
    const bool usb_powered = !samples.empty() && samples.back().usb_powered;
    const bool active = state_ == MonitorState::kAnchoring ||
                        state_ == MonitorState::kProbing ||
                        state_ == MonitorState::kMonitoring;
    ShowWindow(warn_label_,
               (active && (usb_powered || usb_auto_off_state_)) ? SW_SHOW : SW_HIDE);
}

std::wstring BatteryMonitorDialog::FormatCountdownText() const {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG remain_ms = next_cycle_tick_ > now ? next_cycle_tick_ - now : 0;
    const unsigned long secs = static_cast<unsigned long>((remain_ms + 999) / 1000);
    return EffectiveUiLanguage(language_) == UiLanguage::kSimplifiedChinese
               ? std::to_wstring(secs) + L" 秒"
               : std::to_wstring(secs) + L"s";
}

// ---------------------------------------------------------------- 导出

void BatteryMonitorDialog::ExportCsv() {
    if (accumulator_.samples().empty()) return;
    const auto language = EffectiveUiLanguage(language_);
    const std::wstring default_name = DefaultExportFileName(device_id_, L".csv");
    wchar_t path[MAX_PATH]{};
    wcscpy_s(path, default_name.c_str());
    const std::wstring title = TrW(StringId::kBatteryMonitorExportCsv, language);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"CSV (*.csv)\0*.csv\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = title.c_str();
    if (!GetSaveFileNameW(&ofn)) return;  // 用户取消

    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        ShowSaveResult(false, L"open failed");
        return;
    }
    out << accumulator_.FormatCsv();
    out.close();
    if (out.fail()) {
        ShowSaveResult(false, L"write failed");
        return;
    }
    ShowSaveResult(true, path);
}

void BatteryMonitorDialog::ExportPng() {
    const auto language = EffectiveUiLanguage(language_);
    const std::wstring default_name = DefaultExportFileName(device_id_, L".png");
    wchar_t path[MAX_PATH]{};
    wcscpy_s(path, default_name.c_str());
    const std::wstring title = TrW(StringId::kBatteryMonitorExportPng, language);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"PNG (*.png)\0*.png\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = title.c_str();
    if (!GetSaveFileNameW(&ofn)) return;  // 用户取消

    std::wstring error;
    if (SavePng(path, &error)) {
        ShowSaveResult(true, path);
    } else {
        ShowSaveResult(false, error);
    }
}

void BatteryMonitorDialog::ShowSaveResult(bool success, const std::wstring& detail) {
    const auto language = EffectiveUiLanguage(language_);
    status_override_ = success
        ? TrW(StringId::kBatteryMonitorSavedTo, language) + detail
        : FormatText(TrW(StringId::kBatteryMonitorErrSaveFailed, language), {detail});
    RefreshStatus();
}

bool BatteryMonitorDialog::SavePng(const std::wstring& path, std::wstring* error) {
    // 有效点：读数有效（vbat>0）且已对齐墙钟；不足 2 个无法画曲线。
    std::vector<const PowerLogSample*> valid;
    for (const auto& sample : accumulator_.samples()) {
        if (sample.vbat_mv > 0 && sample.epoch_s >= 0) valid.push_back(&sample);
    }
    if (valid.size() < 2) {
        *error = L"not enough valid samples";
        return false;
    }

    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        *error = L"GDI+ init failed";
        return false;
    }

    bool ok = false;
    {
        constexpr int kImageWidth = 1000;
        constexpr int kImageHeight = 500;
        // PixelFormat32bppARGB 在 gdipluspixelformats.h 中是宏（非枚举），不能加 Gdiplus:: 限定。
        Gdiplus::Bitmap bitmap(kImageWidth, kImageHeight, PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(&bitmap);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        graphics.Clear(Gdiplus::Color(255, 255, 255, 255));

        const auto language = EffectiveUiLanguage(language_);
        // GDI+ 不做字体回退链接，中文界面显式用雅黑避免方框。
        Gdiplus::FontFamily font_family(
            language == UiLanguage::kSimplifiedChinese ? L"Microsoft YaHei" : L"Segoe UI");
        Gdiplus::Font title_font(&font_family, 15, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font label_font(&font_family, 12, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush text_brush(Gdiplus::Color(255, 30, 30, 30));
        Gdiplus::Pen axis_pen(Gdiplus::Color(255, 60, 60, 60), 1.0f);
        Gdiplus::Pen grid_pen(Gdiplus::Color(255, 220, 220, 220), 1.0f);
        Gdiplus::Pen line_pen(Gdiplus::Color(255, 31, 119, 180), 2.0f);
        Gdiplus::SolidBrush point_brush(Gdiplus::Color(255, 31, 119, 180));

        // 标题：VS-{0} 电池电压监测（{1} 点）。
        const std::wstring title = FormatText(
            TrW(StringId::kBatteryMonitorChartTitle, language),
            {Utf16(device_id_), std::to_wstring(valid.size())});
        Gdiplus::StringFormat center_format;
        center_format.SetAlignment(Gdiplus::StringAlignmentCenter);
        graphics.DrawString(title.c_str(), -1, &title_font,
                            Gdiplus::RectF(0, 12, kImageWidth, 24), &center_format,
                            &text_brush);

        // 数据范围：X = 相对首个有效点的分钟，Y = vbat 毫伏（上下各留 10% 边距）。
        float min_v = static_cast<float>(valid.front()->vbat_mv);
        float max_v = min_v;
        for (const auto* sample : valid) {
            const float v = static_cast<float>(sample->vbat_mv);
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
        }
        const float v_pad = std::max((max_v - min_v) * 0.1f, 10.0f);
        min_v -= v_pad;
        max_v += v_pad;
        const std::int64_t t0 = valid.front()->epoch_s;
        const float x_max = std::max(
            static_cast<float>(valid.back()->epoch_s - t0) / 60.0f, 1.0f);

        const float plot_left = 80.0f;
        const float plot_right = kImageWidth - 30.0f;
        const float plot_top = 56.0f;
        const float plot_bottom = kImageHeight - 70.0f;
        const auto map_x = [&](float x) {
            return plot_left + x / x_max * (plot_right - plot_left);
        };
        const auto map_y = [&](float v) {
            return plot_bottom - (v - min_v) / (max_v - min_v) * (plot_bottom - plot_top);
        };

        // 网格 + Y 轴刻度（4 等分）。
        Gdiplus::StringFormat right_format;
        right_format.SetAlignment(Gdiplus::StringAlignmentFar);
        for (int i = 0; i <= 4; ++i) {
            const float v = min_v + (max_v - min_v) * static_cast<float>(i) / 4.0f;
            const float y = map_y(v);
            graphics.DrawLine(&grid_pen, plot_left, y, plot_right, y);
            const std::wstring label = std::to_wstring(static_cast<int>(v + 0.5f));
            graphics.DrawString(label.c_str(), -1, &label_font,
                                Gdiplus::RectF(0, y - 8, plot_left - 8, 16), &right_format,
                                &text_brush);
        }
        // X 轴刻度（6 等分，分钟整数）。
        for (int i = 0; i <= 5; ++i) {
            const float x = x_max * static_cast<float>(i) / 5.0f;
            const float px = map_x(x);
            graphics.DrawLine(&grid_pen, px, plot_top, px, plot_bottom);
            const std::wstring label = std::to_wstring(static_cast<int>(x + 0.5f));
            graphics.DrawString(label.c_str(), -1, &label_font,
                                Gdiplus::RectF(px - 20, plot_bottom + 6, 40, 16),
                                &center_format, &text_brush);
        }

        // 坐标轴与轴文案。
        graphics.DrawLine(&axis_pen, plot_left, plot_top, plot_left, plot_bottom);
        graphics.DrawLine(&axis_pen, plot_left, plot_bottom, plot_right, plot_bottom);
        graphics.DrawString(TrW(StringId::kBatteryMonitorAxisTime, language).c_str(), -1,
                            &label_font,
                            Gdiplus::RectF(plot_left, plot_bottom + 28,
                                           plot_right - plot_left, 20),
                            &center_format, &text_brush);
        const Gdiplus::GraphicsState saved = graphics.Save();
        graphics.TranslateTransform(20.0f, (plot_top + plot_bottom) / 2.0f);
        graphics.RotateTransform(-90.0f);
        graphics.DrawString(TrW(StringId::kBatteryMonitorAxisVoltage, language).c_str(), -1,
                            &label_font,
                            Gdiplus::RectF(-(plot_bottom - plot_top) / 2.0f, -10.0f,
                                           plot_bottom - plot_top, 20.0f),
                            &center_format, &text_brush);
        graphics.Restore(saved);

        // 电压折线 + 数据点。
        std::vector<Gdiplus::PointF> points;
        points.reserve(valid.size());
        for (const auto* sample : valid) {
            const float x = static_cast<float>(sample->epoch_s - t0) / 60.0f;
            points.emplace_back(map_x(x), map_y(static_cast<float>(sample->vbat_mv)));
        }
        graphics.DrawLines(&line_pen, points.data(), static_cast<INT>(points.size()));
        for (const auto& point : points) {
            graphics.FillEllipse(&point_brush, point.X - 3.0f, point.Y - 3.0f, 6.0f, 6.0f);
        }

        CLSID png_clsid{};
        if (GetPngEncoderClsid(&png_clsid)) {
            ok = bitmap.Save(path.c_str(), &png_clsid, nullptr) == Gdiplus::Ok;
        }
    }
    Gdiplus::GdiplusShutdown(gdiplus_token);
    if (!ok && error->empty()) *error = L"PNG encode/save failed";
    return ok;
}

// ---------------------------------------------------------------- 窗口过程

INT_PTR CALLBACK BatteryMonitorDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param,
                                                  LPARAM l_param) {
    auto* dialog = reinterpret_cast<BatteryMonitorDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<BatteryMonitorDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        dialog->BuildUi();
        dialog->CenterWindow();
        dialog->RefreshStatus();
        dialog->UpdateButtons();
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR BatteryMonitorDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kIdStart:
            StartMonitoring();
            return TRUE;
        case kIdStop:
            StopMonitoring();
            return TRUE;
        case kIdExportCsv:
            ExportCsv();
            return TRUE;
        case kIdExportPng:
            ExportPng();
            return TRUE;
        case kIdClose:
            DestroyWindow(hwnd_);
            return TRUE;
        case kIdUsbAutoOff:
            if (HIWORD(w_param) == BN_CLICKED) {
                // 下发开关命令；固件回推 power_mgmt 事件后由 OnPowerMgmtState 落定显示。
                const bool checked =
                    SendMessageW(usb_auto_off_check_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SendCommand(BleProtocol::UsbAutoOffPayload(checked));
            }
            return TRUE;
        }
        break;
    case WM_TIMER:
        switch (w_param) {
        case kTimerAnchorStep:
            if (state_ == MonitorState::kAnchoring) {
                if (anchor_step_ == 1) {
                    // 写入墙钟锚点（桌面当前 epoch），设备记录 time_anchor 条目。
                    anchor_step_ = 2;
                    const auto now = std::chrono::system_clock::now();
                    const auto epoch = static_cast<std::uint32_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            now.time_since_epoch()).count());
                    SendCommand(BleProtocol::PowerLogTimeAnchorPayload(epoch));
                    SetTimer(hwnd_, kTimerAnchorStep, kAnchorStepMs, nullptr);
                } else {
                    BeginProbing();
                }
            }
            return TRUE;
        case kTimerProbeTimeout:
            if (state_ == MonitorState::kProbing) {
                AbortWithError(StringId::kBatteryMonitorErrProbeTimeout);
            }
            return TRUE;
        case kTimerCycle:
            if (state_ == MonitorState::kMonitoring && !dump_active_) {
                ++cycle_;
                attempt_ = 0;
                StartDumpAttempt();
            }
            return TRUE;
        case kTimerDumpTimeout:
            if (state_ == MonitorState::kMonitoring && dump_active_) {
                HandleDumpFailure();
            }
            return TRUE;
        case kTimerStatusTick:
            RefreshStatus();  // 倒计时秒数刷新
            return TRUE;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return TRUE;
    case WM_DPICHANGED: {
        UINT new_dpi = HIWORD(w_param);
        if (new_dpi != 0 && new_dpi != dpi_) {
            dpi_ = new_dpi;
            auto* rect = reinterpret_cast<const RECT*>(l_param);
            SetWindowPos(hwnd_, nullptr, rect->left, rect->top,
                         rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            BuildUi();
            RefreshStatus();
            UpdateButtons();
        }
        return TRUE;
    }
    case WM_DESTROY: {
        KillSessionTimers();
        hwnd_ = nullptr;
        DestroyControls();
        // 窗口销毁回调：win32_app 在此释放本对象，回调返回后不得再访问 this。
        std::function<void()> closed = on_closed;
        if (!closing_ && closed) closed();
        return TRUE;
    }
    default:
        break;
    }
    return FALSE;
}

LPCDLGTEMPLATE BatteryMonitorDialog::BuildDialogTemplate() {
    dialog_template_.clear();
    AlignDialogData(&dialog_template_, 4);

    DLGTEMPLATE dialog{};
    dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog.cdit = 0;
    dialog.x = 0;
    dialog.y = 0;
    dialog.cx = 300;
    dialog.cy = 120;
    AppendDialogData(&dialog_template_, &dialog, sizeof(dialog));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_,
                           FormatText(TrW(StringId::kBatteryMonitorTitle,
                                          EffectiveUiLanguage(language_)),
                                      {Utf16(device_id_)})
                               .c_str());
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void BatteryMonitorDialog::BuildUi() {
    DestroyControls();

    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(kClientWidth), Dp(kClientHeight)};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);

    ui_font_ = CreateUiFont(dpi_);
    const auto language = EffectiveUiLanguage(language_);
    auto remember = [&](HWND control) {
        if (control) {
            all_controls_.push_back(control);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
        }
        return control;
    };

    // 状态行（多行：状态文案 + 保存结果）。
    status_label_ = remember(CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
        Dp(16), Dp(14), Dp(340), Dp(44), hwnd_, nullptr, instance_, nullptr));
    // 右上角：供电态（USB）10min 自动关机开关。
    usb_auto_off_check_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kBatteryMonitorUsbAutoOff, language).c_str(),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        Dp(366), Dp(16), Dp(218), Dp(22), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdUsbAutoOff)), instance_, nullptr));
    SendMessageW(usb_auto_off_check_, BM_SETCHECK,
                 usb_auto_off_state_ ? BST_CHECKED : BST_UNCHECKED, 0);
    // USB 供电提示（初始隐藏，UpdateUsbWarning 控制显隐）。
    warn_label_ = remember(CreateWindowExW(
        0, L"STATIC", TrW(StringId::kBatteryMonitorWarnUsbPower, language).c_str(),
        WS_CHILD | SS_LEFT,
        Dp(16), Dp(62), Dp(568), Dp(34), hwnd_, nullptr, instance_, nullptr));

    const int button_y = Dp(kClientHeight - 46);
    start_button_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kBatteryMonitorStart, language).c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Dp(16), button_y, Dp(90), Dp(30), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStart)), instance_, nullptr));
    stop_button_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kBatteryMonitorStop, language).c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Dp(116), button_y, Dp(90), Dp(30), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStop)), instance_, nullptr));
    export_csv_button_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kBatteryMonitorExportCsv, language).c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Dp(216), button_y, Dp(110), Dp(30), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdExportCsv)), instance_, nullptr));
    export_png_button_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kBatteryMonitorExportPng, language).c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Dp(336), button_y, Dp(110), Dp(30), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdExportPng)), instance_, nullptr));
    close_button_ = remember(CreateWindowExW(
        0, L"BUTTON", TrW(StringId::kBatteryMonitorClose, language).c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Dp(kClientWidth - 106), button_y, Dp(90), Dp(30), hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdClose)), instance_, nullptr));
}

void BatteryMonitorDialog::DestroyControls() {
    for (HWND control : all_controls_) {
        if (control && IsWindow(control)) DestroyWindow(control);
    }
    all_controls_.clear();
    status_label_ = nullptr;
    warn_label_ = nullptr;
    usb_auto_off_check_ = nullptr;
    start_button_ = nullptr;
    stop_button_ = nullptr;
    export_csv_button_ = nullptr;
    export_png_button_ = nullptr;
    close_button_ = nullptr;
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void BatteryMonitorDialog::CenterWindow() {
    RECT window_rect{};
    GetWindowRect(hwnd_, &window_rect);
    const int window_width = window_rect.right - window_rect.left;
    const int window_height = window_rect.bottom - window_rect.top;
    RECT work_area = GetWorkAreaForWindow(hwnd_);
    const int x = work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
    const int y = work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void BatteryMonitorDialog::SetText(HWND control, const std::wstring& text) {
    if (control) SetWindowTextW(control, text.c_str());
}

int BatteryMonitorDialog::Dp(int px) const {
    return ScalePx(px, dpi_);
}

} // namespace voicestick
