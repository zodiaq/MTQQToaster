#include "settings_ui.h"
#include "resource.h"

#include <algorithm>
#include <commdlg.h>
#include <limits>
#include <winrt/base.h>

namespace {

std::wstring ReadText(HWND control)
{
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    value.resize(static_cast<size_t>(GetWindowTextW(control, value.data(), static_cast<int>(value.size()))));
    return value;
}

RECT WorkArea(HMONITOR monitor)
{
    MONITORINFO info{sizeof(info)};
    winrt::check_bool(GetMonitorInfoW(monitor, &info));
    return info.rcWork;
}



}

struct SettingsUi::Impl {
    explicit Impl(SettingsUiCallbacks value) : callbacks(std::move(value)) {}
    ~Impl() { CloseAll(); }

    SettingsUiCallbacks callbacks;
    HWND window = nullptr;
    HWND form = nullptr;
    AppConfig editingConfig;
    std::vector<std::filesystem::path> selectedImages;
    struct ControlLayout { HWND control; RECT units; };
    std::vector<ControlLayout> controls;
    int scrollPosition = 0;
    int wheelRemainder = 0;
    HWND lastFocus = nullptr;
    bool placementReady = false;
    RECT defaultWorkArea{};

    HWND Control(int id) const { return GetDlgItem(form, id); }
    std::wstring Text(int id) const { return ReadText(Control(id)); }
    void SetText(int id, const std::wstring& value) { winrt::check_bool(SetWindowTextW(Control(id), value.c_str())); }
    int Selection() const { return static_cast<int>(SendMessageW(Control(IDC_TOPIC_LIST), LB_GETCURSEL, 0, 0)); }

    RECT Pixels(HWND dialog, RECT units) const
    {
        winrt::check_bool(MapDialogRect(dialog, &units));
        return units;
    }

    void ShowError(const std::wstring& message) const
    {
        MessageBoxW(window, message.c_str(), L"MTQQToaster 設定", MB_OK | MB_ICONWARNING);
    }

    void LayoutForm()
    {
        if (!form || controls.empty()) return;
        RECT client{};
        winrt::check_bool(GetClientRect(form, &client));
        // Include only the controls and a small bottom margin in the scroll range.
        const auto content = Pixels(form, {0, 0, 336, 258});
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMax = content.bottom - 1;
        info.nPage = static_cast<UINT>(client.bottom);
        info.nPos = scrollPosition;
        SetScrollInfo(form, SB_VERT, &info, TRUE);
        scrollPosition = GetScrollPos(form, SB_VERT);
        winrt::check_bool(GetClientRect(form, &client));
        const int extraWidth = client.right - content.right;
        const int extraHeight = (std::max)(0L, client.bottom - content.bottom);
        RECT comboBounds{};
        winrt::check_bool(GetWindowRect(Control(IDC_LOG_LEVEL), &comboBounds));
        const int inputHeight = comboBounds.bottom - comboBounds.top;
        for (const auto& item : controls) {
            RECT rect = Pixels(form, item.units);
            const int id = GetDlgCtrlID(item.control);
            if (item.units.top >= 158) OffsetRect(&rect, 0, extraHeight);
            if (id == IDC_TOPIC_LIST) rect.bottom += extraHeight;
            if (id == IDC_MQTT_URL || id == IDC_MQTT_USER || id == IDC_MQTT_PASS ||
                id == IDC_TOPIC_LIST || id == IDC_TOPIC_NAME || id == IDC_TOPIC_IMAGE) rect.right += extraWidth;
            if (id == IDC_IMAGE_BROWSE) OffsetRect(&rect, extraWidth, 0);
            // Single-line native edits do not vertically center text in oversized
            // rectangles. Use the native combo's font/DPI-dependent field height.
            if (id == IDC_MQTT_URL || id == IDC_MQTT_USER || id == IDC_MQTT_PASS ||
                id == IDC_TOPIC_NAME || id == IDC_TOPIC_IMAGE ||
                id == IDC_LOG_SIZE || id == IDC_LOG_FILES || id == IDC_IMAGE_BROWSE) {
                rect.bottom = rect.top + inputHeight;
            }
            OffsetRect(&rect, 0, -scrollPosition);
            winrt::check_bool(SetWindowPos(item.control, nullptr, rect.left, rect.top,
                rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE));
        }
        InvalidateRect(form, nullptr, TRUE);
    }

    void Layout()
    {
        if (!form) return;
        RECT client{};
        winrt::check_bool(GetClientRect(window, &client));
        const auto spacing = Pixels(window, {0, 0, 10, 8});
        const auto button = Pixels(window, {0, 0, 68, 16});
        const int bottom = client.bottom - spacing.bottom;
        winrt::check_bool(SetWindowPos(GetDlgItem(window, IDOK), nullptr,
            client.right - spacing.right - button.right, bottom - button.bottom,
            button.right, button.bottom, SWP_NOZORDER | SWP_NOACTIVATE));
        winrt::check_bool(SetWindowPos(GetDlgItem(window, IDCANCEL), nullptr,
            client.right - 2 * spacing.right - 2 * button.right, bottom - button.bottom,
            button.right, button.bottom, SWP_NOZORDER | SWP_NOACTIVATE));
        winrt::check_bool(SetWindowPos(form, nullptr, spacing.right, spacing.bottom,
            (std::max)(1L, client.right - 2 * spacing.right),
            (std::max)(1L, bottom - button.bottom - 3 * spacing.bottom), SWP_NOZORDER | SWP_NOACTIVATE));
        LayoutForm();
    }

    void ScrollTo(int position)
    {
        scrollPosition = position;
        LayoutForm();
    }

    void EnsureVisible(HWND control)
    {
        if (!control || GetParent(control) != form) return;
        RECT rect{}, client{};
        winrt::check_bool(GetWindowRect(control, &rect));
        MapWindowPoints(nullptr, form, reinterpret_cast<POINT*>(&rect), 2);
        winrt::check_bool(GetClientRect(form, &client));
        if (rect.top < 0) ScrollTo(scrollPosition + rect.top);
        else if (rect.bottom > client.bottom) ScrollTo(scrollPosition + rect.bottom - client.bottom);
    }

    void RefreshTopics(int selected)
    {
        SendMessageW(Control(IDC_TOPIC_LIST), LB_RESETCONTENT, 0, 0);
        for (const auto& topic : editingConfig.topics) {
            std::wstring label = winrt::to_hstring(topic.topic).c_str();
            if (!topic.image.empty()) label += L"  (画像あり)";
            const auto result = SendMessageW(Control(IDC_TOPIC_LIST), LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (result == LB_ERR || result == LB_ERRSPACE) throw std::runtime_error("Could not populate topic list");
        }
        SendMessageW(Control(IDC_TOPIC_LIST), LB_SETCURSEL, selected, 0);
        LoadTopic();
    }

    void LoadTopic()
    {
        const int selected = Selection();
        if (selected < 0 || selected >= static_cast<int>(editingConfig.topics.size())) {
            SetText(IDC_TOPIC_NAME, L"");
            SetText(IDC_TOPIC_IMAGE, L"");
            return;
        }
        const auto index = static_cast<size_t>(selected);
        SetText(IDC_TOPIC_NAME, winrt::to_hstring(editingConfig.topics[index].topic).c_str());
        SetText(IDC_TOPIC_IMAGE, selectedImages[index].empty()
            ? std::wstring(winrt::to_hstring(editingConfig.topics[index].image)) : selectedImages[index].wstring());
    }

    void UpdateTopic()
    {
        const auto name = Text(IDC_TOPIC_NAME);
        const auto first = name.find_first_not_of(L" \t");
        if (first == std::wstring::npos) { ShowError(L"topicを入力してください。"); return; }
        TopicConfig topic;
        topic.topic = winrt::to_string(std::wstring_view(name).substr(first, name.find_last_not_of(L" \t") - first + 1));
        const std::filesystem::path source(Text(IDC_TOPIC_IMAGE));
        const int selected = Selection();
        if (selected >= 0 && selected < static_cast<int>(editingConfig.topics.size())) {
            const auto index = static_cast<size_t>(selected);
            const auto previous = winrt::to_hstring(editingConfig.topics[index].image);
            // An empty field explicitly removes the old image; restoring the old name cancels a pending copy.
            if (!source.empty()) topic.image = editingConfig.topics[index].image;
            selectedImages[index] = source == std::filesystem::path(previous.c_str()) ? std::filesystem::path{} : source;
            editingConfig.topics[index] = std::move(topic);
            RefreshTopics(selected);
        }
        else {
            editingConfig.topics.push_back(std::move(topic));
            selectedImages.push_back(source);
            RefreshTopics(static_cast<int>(editingConfig.topics.size()) - 1);
        }
    }

    void BrowseImage()
    {
        std::wstring path(32768, L'\0');
        OPENFILENAMEW options{sizeof(options)};
        options.hwndOwner = window;
        options.lpstrFilter = L"画像 (*.png;*.jpg;*.jpeg;*.gif)\0*.png;*.jpg;*.jpeg;*.gif\0すべてのファイル\0*.*\0";
        options.lpstrFile = path.data();
        options.nMaxFile = static_cast<DWORD>(path.size());
        options.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&options)) SetText(IDC_TOPIC_IMAGE, path.c_str());
        else if (CommDlgExtendedError()) ShowError(L"画像選択ダイアログを開けませんでした。");
    }

    void Save()
    {
        auto candidate = editingConfig;
        candidate.url = winrt::to_string(Text(IDC_MQTT_URL));
        candidate.user = winrt::to_string(Text(IDC_MQTT_USER));
        candidate.pass = winrt::to_string(Text(IDC_MQTT_PASS));
        candidate.logLevel = winrt::to_string(Text(IDC_LOG_LEVEL));
        if (!ParsePositiveSetting(Text(IDC_LOG_SIZE), candidate.maxLogSizeMb) ||
            candidate.maxLogSizeMb > (std::numeric_limits<size_t>::max)() / (1024 * 1024) ||
            !ParsePositiveSetting(Text(IDC_LOG_FILES), candidate.maxLogFiles)) {
            ShowError(L"ログサイズと保持数には有効な1以上の整数を入力してください。");
            return;
        }
        std::wstring error;
        if (!callbacks.saveConfig(candidate, selectedImages, error)) { ShowError(error); return; }
        CloseAll();
    }

    void Command(int id, int notification)
    {
        if (id == IDC_TOPIC_LIST && notification == LBN_SELCHANGE) LoadTopic();
        else if (notification == BN_CLICKED) {
            switch (id) {
            case IDOK: Save(); break;
            case IDCANCEL: CloseAll(); break;
            case IDC_IMAGE_BROWSE: BrowseImage(); break;
            case IDC_TOPIC_EDIT: UpdateTopic(); break;
            case IDC_TOPIC_ADD: SendMessageW(Control(IDC_TOPIC_LIST), LB_SETCURSEL, static_cast<WPARAM>(-1), 0); LoadTopic(); break;
            case IDC_TOPIC_DELETE: {
                const int selected = Selection();
                if (selected < 0) break;
                editingConfig.topics.erase(editingConfig.topics.begin() + selected);
                selectedImages.erase(selectedImages.begin() + selected);
                RefreshTopics((std::min)(selected, static_cast<int>(editingConfig.topics.size()) - 1));
                break;
            }
            }
        }
    }

    void InitializeForm(HWND hwnd)
    {
        form = hwnd;
        // Capture template geometry in dialog units so it can be reapplied after a DPI change.
        const auto base = Pixels(form, {0, 0, 4, 8});
        for (HWND child = GetWindow(form, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            RECT rect{};
            winrt::check_bool(GetWindowRect(child, &rect));
            MapWindowPoints(nullptr, form, reinterpret_cast<POINT*>(&rect), 2);
            rect = {MulDiv(rect.left, 4, base.right), MulDiv(rect.top, 8, base.bottom),
                    MulDiv(rect.right, 4, base.right), MulDiv(rect.bottom, 8, base.bottom)};
            // GetWindowRect returns only the collapsed height of a combo box.
            if (GetDlgCtrlID(child) == IDC_LOG_LEVEL) rect.bottom = rect.top + 100;
            controls.push_back({child, rect});
        }
        SetText(IDC_MQTT_URL, winrt::to_hstring(editingConfig.url).c_str());
        SetText(IDC_MQTT_USER, winrt::to_hstring(editingConfig.user).c_str());
        SetText(IDC_MQTT_PASS, winrt::to_hstring(editingConfig.pass).c_str());
        constexpr const wchar_t* levels[] = {L"TRACE", L"DEBUG", L"INFO", L"WARN", L"ERROR", L"NONE"};
        int selection = 2;
        for (int i = 0; i < static_cast<int>(std::size(levels)); ++i) {
            const auto result = SendMessageW(Control(IDC_LOG_LEVEL), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(levels[i]));
            if (result == CB_ERR || result == CB_ERRSPACE) throw std::runtime_error("Could not populate log levels");
            if (editingConfig.logLevel == winrt::to_string(levels[i])) selection = i;
        }
        SendMessageW(Control(IDC_LOG_LEVEL), CB_SETCURSEL, selection, 0);
        SetText(IDC_LOG_SIZE, std::to_wstring(editingConfig.maxLogSizeMb));
        SetText(IDC_LOG_FILES, std::to_wstring(editingConfig.maxLogFiles));
        RefreshTopics(editingConfig.topics.empty() ? -1 : 0);
    }

    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, DWLP_USER));
        try {
            if (message == WM_INITDIALOG) {
                self = reinterpret_cast<Impl*>(lParam);
                SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
                if (!self->window) self->window = hwnd;
                else self->InitializeForm(hwnd);
                return TRUE;
            }
            if (!self) return FALSE;
            switch (message) {
            case WM_COMMAND: self->Command(LOWORD(wParam), HIWORD(wParam)); return TRUE;
            case WM_CLOSE: self->CloseAll(); return TRUE;
            case WM_SIZE:
                if (hwnd == self->window && wParam != SIZE_MINIMIZED) self->Layout();
                return TRUE;
            case WM_DPICHANGED:
                // Per-monitor-v2 dialog manager scales the font and suggested bounds.
                PostMessageW(self->window, WM_APP + 1, 0, 0);
                return FALSE;
            case WM_APP + 1: self->Layout(); return TRUE;
            case WM_GETMINMAXINFO: {
                auto info = reinterpret_cast<MINMAXINFO*>(lParam);
                RECT minimum = self->Pixels(hwnd, {0, 0, 370, 140});
                winrt::check_bool(AdjustWindowRectExForDpi(&minimum, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                    FALSE, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd)));
                info->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top};
                return TRUE;
            }
            case WM_VSCROLL: {
                SCROLLINFO info{sizeof(info), SIF_ALL};
                GetScrollInfo(self->form, SB_VERT, &info);
                int position = self->scrollPosition;
                switch (LOWORD(wParam)) {
                case SB_LINEUP: position -= 24; break;
                case SB_LINEDOWN: position += 24; break;
                case SB_PAGEUP: position -= info.nPage; break;
                case SB_PAGEDOWN: position += info.nPage; break;
                case SB_THUMBTRACK: position = info.nTrackPos; break;
                case SB_TOP: position = 0; break;
                case SB_BOTTOM: position = info.nMax; break;
                }
                self->ScrollTo(position);
                return TRUE;
            }
            case WM_MOUSEWHEEL:
                self->wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
                self->ScrollTo(self->scrollPosition - (self->wheelRemainder / WHEEL_DELTA) * 48);
                self->wheelRemainder %= WHEEL_DELTA;
                return TRUE;
            case WM_NCDESTROY:
                SetWindowLongPtrW(hwnd, DWLP_USER, 0);
                if (hwnd == self->form) { self->form = nullptr; self->controls.clear(); }
                if (hwnd == self->window) self->window = nullptr;
                return FALSE;
            }
        }
        catch (...) {
            // Never propagate C++ exceptions through a Win32 callback.
            if (message == WM_INITDIALOG) { DestroyWindow(hwnd); return FALSE; }
            MessageBoxW(hwnd, L"設定画面の操作に失敗しました。", L"MTQQToaster", MB_OK | MB_ICONERROR);
        }
        return FALSE;
    }

    void ApplyDefaultPlacement()
    {
        placementReady = false;
        ShowWindow(window, SW_RESTORE);
        POINT pointer{};
        winrt::check_bool(GetCursorPos(&pointer));
        defaultWorkArea = WorkArea(MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST));
        winrt::check_bool(SetWindowPos(window, nullptr, defaultWorkArea.left, defaultWorkArea.top, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
        // Two extra dialog units absorb independent pixel rounding in Layout().
        RECT rect = Pixels(window, {0, 0, 370, 308});
        winrt::check_bool(AdjustWindowRectExForDpi(&rect, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
            FALSE, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window)));
        auto placement = FitSettingsWindowToWorkArea({0, 0, rect.right - rect.left, rect.bottom - rect.top}, defaultWorkArea);
        placement.x = defaultWorkArea.left + (defaultWorkArea.right - defaultWorkArea.left - placement.width) / 2;
        placement.y = defaultWorkArea.top + (defaultWorkArea.bottom - defaultWorkArea.top - placement.height) / 2;
        winrt::check_bool(SetWindowPos(window, nullptr, placement.x, placement.y, placement.width, placement.height,
            SWP_NOZORDER | SWP_NOACTIVATE));
        scrollPosition = 0;
        Layout();
        placementReady = true;
    }

    void ShowSettings(bool resetPlacement)
    {
        if (window) {
            if (resetPlacement) ApplyDefaultPlacement();
            if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            return;
        }
        editingConfig = callbacks.loadConfig();
        selectedImages.assign(editingConfig.topics.size(), {});
        scrollPosition = 0;
        placementReady = false;
        const auto saved = !resetPlacement && callbacks.loadPlacement ? callbacks.loadPlacement() : std::nullopt;
        try {
            winrt::check_pointer(CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
                DialogProc, reinterpret_cast<LPARAM>(this)));
            winrt::check_pointer(CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS_FORM), window,
                DialogProc, reinterpret_cast<LPARAM>(this)));
            SetWindowLongPtrW(form, GWLP_ID, IDC_SETTINGS_FORM);
            Layout();
            if (saved) {
                const RECT bounds{saved->x, saved->y, saved->x + saved->width, saved->y + saved->height};
                const auto placement = FitSettingsWindowToWorkArea(*saved, WorkArea(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST)));
                winrt::check_bool(SetWindowPos(window, nullptr, placement.x, placement.y, placement.width, placement.height,
                    SWP_NOZORDER | SWP_NOACTIVATE));
                ShowWindow(window, saved->maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
            }
            else { ApplyDefaultPlacement(); ShowWindow(window, SW_SHOW); }
            placementReady = true;
            SetForegroundWindow(window);
            SetFocus(Control(IDC_MQTT_URL));
        }
        catch (...) { CloseAll(); throw; }
    }

    void CloseAll() noexcept
    {
        if (!window) return;
        try {
            if (placementReady && callbacks.savePlacement) {
                WINDOWPLACEMENT placement{sizeof(placement)};
                winrt::check_bool(GetWindowPlacement(window, &placement));
                // WINDOWPLACEMENT normal bounds are workspace-relative for a normal top-level window.
                MONITORINFO monitor{sizeof(monitor)};
                winrt::check_bool(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor));
                auto rect = placement.rcNormalPosition;
                OffsetRect(&rect, monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top);
                callbacks.savePlacement({rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                    placement.showCmd == SW_SHOWMAXIMIZED || (placement.flags & WPF_RESTORETOMAXIMIZED) != 0});
            }
        }
        catch (...) { OutputDebugStringW(L"MTQQToaster: could not save settings window placement.\n"); }
        placementReady = false;
        DestroyWindow(window);
    }

    bool IsSettingsReady() const
    {
        return window && form && IsWindowVisible(window) && Control(IDC_MQTT_URL) && Control(IDC_LOG_FILES) &&
            (editingConfig.topics.empty() || (Selection() == 0 &&
                Text(IDC_TOPIC_NAME) == winrt::to_hstring(editingConfig.topics[0].topic)));
    }

    bool IsDefaultLayoutReady() const
    {
        if (!IsSettingsReady() || !placementReady) return false;
        RECT rect{};
        winrt::check_bool(GetWindowRect(window, &rect));
        RECT level{}, size{}, files{}, viewport{};
        winrt::check_bool(GetWindowRect(Control(IDC_LOG_LEVEL), &level));
        winrt::check_bool(GetWindowRect(Control(IDC_LOG_SIZE), &size));
        winrt::check_bool(GetWindowRect(Control(IDC_LOG_FILES), &files));
        winrt::check_bool(GetWindowRect(form, &viewport));
        const bool workAreaLimited = rect.bottom - rect.top == defaultWorkArea.bottom - defaultWorkArea.top;
        if (!workAreaLimited && (GetWindowLongPtrW(form, GWL_STYLE) & WS_VSCROLL)) return false;
        return std::abs(rect.left + rect.right - defaultWorkArea.left - defaultWorkArea.right) <= 2 &&
            std::abs(rect.top + rect.bottom - defaultWorkArea.top - defaultWorkArea.bottom) <= 2 && scrollPosition == 0 &&
            std::abs(level.top - size.top) <= 1 && std::abs(size.top - files.top) <= 1 &&
            level.bottom == size.bottom && size.bottom == files.bottom &&
            level.right < size.left && size.right < files.left &&
            (files.bottom <= viewport.bottom || rect.bottom - rect.top == defaultWorkArea.bottom - defaultWorkArea.top);
    }
};

SettingsUi::SettingsUi(SettingsUiCallbacks callbacks) : impl_(std::make_unique<Impl>(std::move(callbacks))) {}
SettingsUi::~SettingsUi() = default;
void SettingsUi::ShowSettings(bool resetPlacement) { impl_->ShowSettings(resetPlacement); }
bool SettingsUi::IsSettingsReady() const { return impl_->IsSettingsReady(); }
bool SettingsUi::IsDefaultLayoutReady() const { return impl_->IsDefaultLayoutReady(); }
HWND SettingsUi::SettingsHandle() const { return impl_->window; }
void SettingsUi::CloseAll() { impl_->CloseAll(); }
bool SettingsUi::ProcessMessage(MSG& message)
{
    if (!impl_->window) return false;
    const bool handled = IsDialogMessageW(impl_->window, &message) != FALSE;
    const HWND focused = GetFocus();
    if (focused != impl_->lastFocus) {
        impl_->EnsureVisible(focused);
        impl_->lastFocus = focused;
    }
    return handled;
}
