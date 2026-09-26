#include "hotword_activity_dialog.h"

#include "dpi_util.h"
#include "hotword_context.h"
#include "hotword_selector.h"

#include <CommCtrl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace voicestick {

namespace {

HMENU ControlId(int id) {
    return reinterpret_cast<HMENU>(static_cast<std::intptr_t>(id));
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
    if (!text) {
        AppendDialogWord(buffer, 0);
        return;
    }
    while (*text) {
        AppendDialogWord(buffer, static_cast<WORD>(*text));
        ++text;
    }
    AppendDialogWord(buffer, 0);
}

void InsertColumn(HWND list_view, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = width;
    column.iSubItem = index;
    SendMessageW(list_view, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index),
                 reinterpret_cast<LPARAM>(&column));
}

int AppendListViewRow(HWND list_view, const wchar_t* text) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(SendMessageW(list_view, LVM_GETITEMCOUNT, 0, 0));
    item.pszText = const_cast<wchar_t*>(text);
    return static_cast<int>(SendMessageW(list_view, LVM_INSERTITEMW, 0,
                                         reinterpret_cast<LPARAM>(&item)));
}

void SetListViewText(HWND list_view, int item_index, int subitem_index, const wchar_t* text) {
    LVITEMW item{};
    item.iSubItem = subitem_index;
    item.pszText = const_cast<wchar_t*>(text);
    SendMessageW(list_view, LVM_SETITEMTEXTW, static_cast<WPARAM>(item_index),
                 reinterpret_cast<LPARAM>(&item));
}

// 活跃度数值（保留两位小数）。
std::wstring FormatScore(double score) {
    wchar_t buffer[32]{};
    swprintf(buffer, 32, L"%.2f", score);
    return buffer;
}

// 闲置/逐出天数：ts<=0（未知）显示 "-"，否则 (now-ts)/86400 向下取整。
std::wstring FormatDaysSince(std::int64_t now_s, std::int64_t ts) {
    if (ts <= 0) return L"-";
    const auto days = static_cast<long long>((now_s - ts) / 86400);
    if (days < 0) return L"0";
    wchar_t buffer[32]{};
    swprintf(buffer, 32, L"%lld", days);
    return buffer;
}

// 回收池行首 ✗ 图标：软件绘制 32bpp 预乘 alpha 位图（深红对角叉，1px
// 抗锯齿边）。不依赖 GDI 文本/主题，任意 DPI 下逐像素生成。
HBITMAP CreateXBitmap(int size) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return nullptr;
    if (!bits) {
        DeleteObject(bitmap);
        return nullptr;
    }
    auto* px = static_cast<unsigned __int32*>(bits);
    const double half = size / 2.0;
    const double band = std::max(1.5, size / 5.0);  // 笔画半宽
    const double edge = band + 1.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = x + 0.5 - half;
            const double dy = y + 0.5 - half;
            // 到两条对角线（dx-dy=0 / dx+dy=0）的距离。
            const double d1 = std::abs(dx - dy) * 0.7071067811865476;
            const double d2 = std::abs(dx + dy) * 0.7071067811865476;
            const double dist = std::min(d1, d2);
            if (dist >= edge) continue;
            const double alpha = dist <= band ? 1.0 : edge - dist;
            const unsigned a = static_cast<unsigned>(alpha * 255.0 + 0.5);
            const unsigned r = 200u * a / 255u, g = 50u * a / 255u, b = 50u * a / 255u;
            px[static_cast<std::size_t>(y) * size + x] =
                (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return bitmap;
}

// 回收池 state image list（两帧同图：未点击/点击瞬态，点击即删行）。
// 自定义 LVSIL_STATE 须在启用 LVS_EX_CHECKBOXES 之前挂上，commctrl 才会
// 采用它而非内置勾选框图（保留 checkbox 扩展风格只为复用"点击行首切换
// state 触发 LVN_ITEMCHANGED"的交互通路）。
HIMAGELIST CreateEvictedStateImages(UINT dpi) {
    const int size = (16 * static_cast<int>(dpi) + 96 / 2) / 96;
    HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR32, 2, 0);
    if (!images) return nullptr;
    HBITMAP x_bitmap = CreateXBitmap(size);
    if (!x_bitmap) {
        ImageList_Destroy(images);
        return nullptr;
    }
    ImageList_Add(images, x_bitmap, nullptr);
    ImageList_Add(images, x_bitmap, nullptr);
    DeleteObject(x_bitmap);
    return images;
}

} // namespace

HotwordActivityDialog::HotwordActivityDialog(HINSTANCE instance, HWND owner, UiLanguage language)
    : instance_(instance), owner_(owner), language_(language) {}

HotwordActivityDialog::~HotwordActivityDialog() {
    if (hwnd_) {
        EndDialog(hwnd_, IDCANCEL);
        hwnd_ = nullptr;
    }
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

void HotwordActivityDialog::Show() {
    DialogBoxIndirectParamW(instance_, BuildDialogTemplate(), owner_,
                            HotwordActivityDialog::DialogProc, reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK HotwordActivityDialog::DialogProc(HWND hwnd, UINT message, WPARAM w_param,
                                                    LPARAM l_param) {
    auto* dialog = reinterpret_cast<HotwordActivityDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<HotwordActivityDialog*>(l_param);
        SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(dialog));
        dialog->hwnd_ = hwnd;
        dialog->dpi_ = GetDpiForHwnd(hwnd);
        dialog->BuildContent();

        // 居中到所有者工作区（与 PairDeviceDialog 同模式）。
        RECT window_rect{};
        GetWindowRect(hwnd, &window_rect);
        const int window_width = window_rect.right - window_rect.left;
        const int window_height = window_rect.bottom - window_rect.top;
        RECT work_area = GetWorkAreaForWindow(hwnd);
        const int x = work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
        const int y = work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        dialog->Refresh();
        return TRUE;
    }
    return dialog ? dialog->HandleMessage(message, w_param, l_param) : FALSE;
}

INT_PTR HotwordActivityDialog::HandleMessage(UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(w_param) == kRefreshButtonId) {
            Refresh();
            return TRUE;
        }
        if (LOWORD(w_param) == kClearPoolButtonId) {
            HandleClearPool();
            return TRUE;
        }
        if (LOWORD(w_param) == IDCANCEL) {
            EndDialog(hwnd_, IDCANCEL);
            hwnd_ = nullptr;
            return TRUE;
        }
        return FALSE;
    case WM_NOTIFY:
        // 回收池行首 ✗ 图标：点击（checkbox 态切换到帧 2）即删除该词
        // （Refresh 里 InsertItem 触发的未选中通知不满足条件，不会误删/重入）。
        if (w_param == kEvictedListId && l_param != 0) {
            const auto* nmv = reinterpret_cast<const NMLISTVIEW*>(l_param);
            if (nmv->hdr.code == LVN_ITEMCHANGED &&
                (nmv->uChanged & LVIF_STATE) != 0 &&
                (nmv->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2)) {
                HandleEvictedCheck(nmv->iItem);
                return TRUE;
            }
        }
        return FALSE;
    default:
        return FALSE;
    }
}

void HotwordActivityDialog::HandleEvictedCheck(int item_index) {
    if (item_index < 0 || static_cast<std::size_t>(item_index) >= evicted_rows_.size()) return;
    if (remove_evicted_words) {
        remove_evicted_words({evicted_rows_[item_index]});
    }
    Refresh();
}

void HotwordActivityDialog::HandleClearPool() {
    if (evicted_rows_.empty() || !remove_evicted_words) return;
    const std::wstring body =
        TrW(StringId::kHotwordActivityClearConfirmBodyPrefix, language_) +
        std::to_wstring(evicted_rows_.size()) +
        TrW(StringId::kHotwordActivityClearConfirmBodySuffix, language_);
    if (MessageBoxW(hwnd_, body.c_str(),
                    TrW(StringId::kHotwordActivityClearConfirmTitle, language_).c_str(),
                    MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
        return;
    }
    remove_evicted_words(evicted_rows_);
    Refresh();
}

LPCDLGTEMPLATE HotwordActivityDialog::BuildDialogTemplate() {
    dialog_template_.clear();

    DLGTEMPLATE dialog_template{};
    dialog_template.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dialog_template.dwExtendedStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    dialog_template.cdit = 0;
    dialog_template.x = 0;
    dialog_template.y = 0;
    dialog_template.cx = 260;
    dialog_template.cy = 190;

    AppendDialogData(&dialog_template_, &dialog_template, sizeof(dialog_template));
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWord(&dialog_template_, 0);
    AppendDialogWideString(&dialog_template_, TrW(StringId::kHotwordActivityTitle, language_).c_str());
    AppendDialogWord(&dialog_template_, 9);
    AppendDialogWideString(&dialog_template_, L"Segoe UI");
    return reinterpret_cast<LPCDLGTEMPLATE>(dialog_template_.data());
}

void HotwordActivityDialog::BuildContent() {
    DestroyControls();

    INITCOMMONCONTROLSEX common_controls{};
    common_controls.dwSize = sizeof(common_controls);
    common_controls.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&common_controls);

    ui_font_ = CreateUiFont(dpi_);
    const HFONT font = ui_font_;
    const int margin = Dp(16);
    const int client_width = Dp(460);
    const int status_y = Dp(14);
    const int word_list_y = Dp(38);
    const int word_list_h = Dp(186);
    const int evicted_label_y = word_list_y + word_list_h + Dp(10);
    const int evicted_list_y = evicted_label_y + Dp(22);
    const int evicted_list_h = Dp(104);
    const int button_y = evicted_list_y + evicted_list_h + Dp(14);
    const int button_w = Dp(86);
    const int button_h = Dp(30);
    const int list_w = client_width - 2 * margin;

    const auto status_text = TrW(StringId::kHotwordActivityActiveCategoryLabel, language_);
    status_label_ = CreateWindowExW(0, L"STATIC", status_text.c_str(),
                                    WS_CHILD | WS_VISIBLE,
                                    margin, status_y, list_w, Dp(22), hwnd_, nullptr,
                                    instance_, nullptr);

    word_list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                     LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                     LVS_NOSORTHEADER,
                                 margin, word_list_y, list_w, word_list_h, hwnd_,
                                 ControlId(kWordListId), instance_, nullptr);
    SendMessageW(word_list_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    const int scrollbar_width = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_);
    const int col_width = list_w - scrollbar_width - Dp(6);
    const int col_word = Dp(130);
    const int col_category = Dp(84);
    const int col_count = Dp(64);
    const int col_score = Dp(66);
    InsertColumn(word_list_, 0, TrW(StringId::kHotwordActivityColWord, language_).c_str(), col_word);
    InsertColumn(word_list_, 1, TrW(StringId::kHotwordActivityColCategory, language_).c_str(), col_category);
    InsertColumn(word_list_, 2, TrW(StringId::kHotwordActivityColCount, language_).c_str(), col_count);
    InsertColumn(word_list_, 3, TrW(StringId::kHotwordActivityColScore, language_).c_str(), col_score);
    InsertColumn(word_list_, 4, TrW(StringId::kHotwordActivityColDaysSinceUse, language_).c_str(),
                 col_width - col_word - col_category - col_count - col_score);

    const auto evicted_text = TrW(StringId::kHotwordActivityEvictedGroup, language_);
    evicted_label_ = CreateWindowExW(0, L"STATIC", evicted_text.c_str(),
                                     WS_CHILD | WS_VISIBLE,
                                     margin, evicted_label_y, list_w, Dp(20), hwnd_, nullptr,
                                     instance_, nullptr);

    evicted_list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                        LVS_NOSORTHEADER,
                                    margin, evicted_list_y, list_w, evicted_list_h, hwnd_,
                                    ControlId(kEvictedListId), instance_, nullptr);
    evicted_state_images_ = CreateEvictedStateImages(dpi_);
    if (evicted_state_images_) {
        ListView_SetImageList(evicted_list_, evicted_state_images_, LVSIL_STATE);
    }
    SendMessageW(evicted_list_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES |
                     LVS_EX_CHECKBOXES);
    InsertColumn(evicted_list_, 0, TrW(StringId::kHotwordActivityColWord, language_).c_str(), col_word);
    InsertColumn(evicted_list_, 1, TrW(StringId::kHotwordActivityColOrigCategory, language_).c_str(), col_category);
    InsertColumn(evicted_list_, 2, TrW(StringId::kHotwordActivityColCount, language_).c_str(), col_count);
    InsertColumn(evicted_list_, 3, TrW(StringId::kHotwordActivityColScore, language_).c_str(), col_score);
    InsertColumn(evicted_list_, 4, TrW(StringId::kHotwordActivityColEvictedDays, language_).c_str(),
                 col_width - col_word - col_category - col_count - col_score);

    const int close_x = client_width - margin - button_w;
    const int refresh_x = close_x - Dp(10) - button_w;
    const int clear_x = refresh_x - Dp(10) - button_w;
    clear_button_ = CreateWindowExW(0, L"BUTTON",
                                    TrW(StringId::kHotwordActivityClearPoolButton, language_).c_str(),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    clear_x, button_y, button_w, button_h, hwnd_,
                                    ControlId(kClearPoolButtonId), instance_, nullptr);
    refresh_button_ = CreateWindowExW(0, L"BUTTON",
                                      TrW(StringId::kHotwordActivityRefreshButton, language_).c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      refresh_x, button_y, button_w, button_h, hwnd_,
                                      ControlId(kRefreshButtonId), instance_, nullptr);
    close_button_ = CreateWindowExW(0, L"BUTTON",
                                    TrW(StringId::kClose, language_).c_str(),
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                    close_x, button_y, button_w, button_h, hwnd_,
                                    ControlId(IDCANCEL), instance_, nullptr);
    for (HWND control : {status_label_, word_list_, evicted_label_, evicted_list_,
                         clear_button_, refresh_button_, close_button_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    // 客户区高度按内容推算，宽度固定 460（Dp 换算）。
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, client_width, button_y + button_h + margin};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left,
                 desired.bottom - desired.top, SWP_NOMOVE | SWP_NOZORDER);
}

void HotwordActivityDialog::Refresh() {
    if (!word_list_ || !evicted_list_) return;
    SendMessageW(word_list_, LVM_DELETEALLITEMS, 0, 0);
    SendMessageW(evicted_list_, LVM_DELETEALLITEMS, 0, 0);
    evicted_rows_.clear();
    if (!take_snapshot) {
        SetWindowTextW(status_label_, L"");
        if (clear_button_) EnableWindow(clear_button_, FALSE);
        return;
    }
    const HotwordActivitySnapshot snapshot = take_snapshot();
    const std::int64_t now_s = std::time(nullptr);
    const std::string& active_name = snapshot.config.active_hotword_category;

    // 全分类全部词：活跃分类排最前，分类内按活跃度降序（同分字典序）。
    struct Row {
        const std::string* word;
        const std::string* category;
        double score;
        const HotwordUsage* usage;
    };
    std::vector<Row> rows;
    for (const auto& category : snapshot.config.hotword_categories) {
        for (const auto& word : category.words) {
            Row row{&word, &category.name, 0.0, nullptr};
            if (const auto it = snapshot.usage.find(word); it != snapshot.usage.end()) {
                row.usage = &it->second;
                row.score = HotwordScore(it->second, now_s);
            }
            rows.push_back(row);
        }
    }
    std::sort(rows.begin(), rows.end(), [&active_name](const Row& a, const Row& b) {
        const bool a_active = *a.category == active_name;
        const bool b_active = *b.category == active_name;
        if (a_active != b_active) return a_active;
        if (*a.category != *b.category) return *a.category < *b.category;
        if (a.score != b.score) return a.score > b.score;
        return *a.word < *b.word;
    });
    for (const auto& row : rows) {
        const int index = AppendListViewRow(word_list_, Utf16(*row.word).c_str());
        SetListViewText(word_list_, index, 1, Utf16(*row.category).c_str());
        wchar_t count[32]{};
        swprintf(count, 32, L"%d", row.usage ? row.usage->count : 0);
        SetListViewText(word_list_, index, 2, count);
        const std::wstring score = row.usage ? FormatScore(row.score) : L"-";
        SetListViewText(word_list_, index, 3, score.c_str());
        const std::wstring idle =
            FormatDaysSince(now_s, row.usage ? row.usage->last_used_ts : 0);
        SetListViewText(word_list_, index, 4, idle.c_str());
    }

    // 回收池：按逐出时间新→旧。
    auto evicted = snapshot.evicted;
    std::sort(evicted.begin(), evicted.end(), [](const EvictedWord& a, const EvictedWord& b) {
        return a.evicted_ts > b.evicted_ts;
    });
    for (const auto& entry : evicted) {
        const int index = AppendListViewRow(evicted_list_, Utf16(entry.word).c_str());
        SetListViewText(evicted_list_, index, 1, Utf16(entry.category).c_str());
        wchar_t count[32]{};
        swprintf(count, 32, L"%d", entry.usage.count);
        SetListViewText(evicted_list_, index, 2, count);
        const std::wstring score = FormatScore(HotwordScore(entry.usage, now_s));
        SetListViewText(evicted_list_, index, 3, score.c_str());
        const std::wstring evicted_days = FormatDaysSince(now_s, entry.evicted_ts);
        SetListViewText(evicted_list_, index, 4, evicted_days.c_str());
        evicted_rows_.push_back(entry.word);
    }
    // 清空按钮：池空或删除入口未接线时禁用。
    if (clear_button_) {
        EnableWindow(clear_button_,
                     !evicted_rows_.empty() && remove_evicted_words ? TRUE : FALSE);
    }

    // 状态行：活跃分类 · n/25 · 监控中/正常（词数超过软限 5 进入监控）。
    std::wstring status;
    if (const auto* active = FindHotwordCategory(snapshot.config, active_name)) {
        const bool monitoring =
            static_cast<int>(active->words.size()) > kHotwordCtxSoftLimit;
        status = TrW(StringId::kHotwordActivityActiveCategoryLabel, language_) +
                 Utf16(active->name.empty() ? std::string("-") : active->name) + L" · " +
                 std::to_wstring(active->words.size()) + L"/" +
                 std::to_wstring(HotwordCategory::kMaxWords) + L" · " +
                 TrW(monitoring ? StringId::kHotwordActivityStatusMonitoring
                                : StringId::kHotwordActivityStatusNormal,
                     language_);
    }
    SetWindowTextW(status_label_, status.c_str());
}

void HotwordActivityDialog::DestroyControls() {
    if (status_label_ && IsWindow(status_label_)) DestroyWindow(status_label_);
    if (word_list_ && IsWindow(word_list_)) DestroyWindow(word_list_);
    if (evicted_label_ && IsWindow(evicted_label_)) DestroyWindow(evicted_label_);
    if (evicted_list_ && IsWindow(evicted_list_)) DestroyWindow(evicted_list_);
    if (clear_button_ && IsWindow(clear_button_)) DestroyWindow(clear_button_);
    if (refresh_button_ && IsWindow(refresh_button_)) DestroyWindow(refresh_button_);
    if (close_button_ && IsWindow(close_button_)) DestroyWindow(close_button_);
    status_label_ = nullptr;
    word_list_ = nullptr;
    evicted_label_ = nullptr;
    evicted_list_ = nullptr;
    clear_button_ = nullptr;
    refresh_button_ = nullptr;
    close_button_ = nullptr;
    if (evicted_state_images_) {
        ImageList_Destroy(evicted_state_images_);
        evicted_state_images_ = nullptr;
    }
    evicted_rows_.clear();
    if (ui_font_) {
        DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
}

std::wstring HotwordActivityDialog::Utf16(const std::string& text) const {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            result.data(), size);
    }
    return result;
}

int HotwordActivityDialog::Dp(int px) const {
    return (px * static_cast<int>(dpi_) + 96 / 2) / 96;
}

} // namespace voicestick
