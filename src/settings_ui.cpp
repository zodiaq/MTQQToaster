#include "ui_text.h"
#include "settings_ui.h"
#include "resource.h"

#include <algorithm>
#include <cmath>
#include <commdlg.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <windowsx.h>
#include <limits>
#include <map>
#include <stdexcept>
#include <spdlog/spdlog.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

namespace {
using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Hosting;

constexpr UINT CloseSettingsMessage = WM_APP + 41;
constexpr DWORD SettingsStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
RECT WorkArea(HMONITOR monitor)
{
    MONITORINFO info{sizeof(info)};
    check_bool(GetMonitorInfoW(monitor, &info));
    return info.rcWork;
}
}

struct SettingsUi::Impl {
    explicit Impl(SettingsUiCallbacks value) : callbacks(std::move(value)) {}
    ~Impl()
    {
        CloseAll();
        for (auto& revoke : menuUnsubscribe) { try { revoke(); } catch (...) {} }
        menuUnsubscribe.clear();
        try { if (menuSource) { menuSource.Content(nullptr); menuSource.Close(); } } catch (...) {}
        menuSource = nullptr;
        if (menuWindow) DestroyWindow(menuWindow);
        ReleaseHost();
        if (manager) {
            try { manager.Close(); } catch (...) { OutputDebugStringW(L"MTQQToaster: XAML shutdown failed.\n"); }
            manager = nullptr;
            // WindowsXamlManager.Close releases resources asynchronously on this STA.
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

    void ReleaseHost() noexcept
    {
        for (auto& revoke : unsubscribe) { try { revoke(); } catch (...) {} }
        unsubscribe.clear();
        try { if (source) source.Content(nullptr); } catch (...) {}
        edits.clear();
        password = nullptr;
        topics = nullptr;
        logLevel = nullptr;
        logRow = nullptr;
        thumbnail = nullptr;
        content = nullptr;
        scroll = nullptr;
        root = nullptr;
        try { if (source) source.Close(); } catch (...) {}
        source = nullptr;
        island = nullptr;
        if (window) DestroyWindow(window);
    }

    SettingsUiCallbacks callbacks;
    HWND window = nullptr;
    HWND island = nullptr;
    WindowsXamlManager manager{nullptr};
    DesktopWindowXamlSource source{nullptr};
    DesktopWindowXamlSource menuSource{nullptr};
    HWND menuWindow = nullptr;
    HWND menuIsland = nullptr;
    std::vector<std::function<void()>> menuUnsubscribe;
    Grid root{nullptr};
    ScrollViewer scroll{nullptr};
    StackPanel content{nullptr};
    Grid logRow{nullptr};
    Image thumbnail{nullptr};
    ListView topics{nullptr};
    PasswordBox password{nullptr};
    ComboBox logLevel{nullptr};
    std::map<int, TextBox> edits;
    AppConfig editingConfig;
    std::vector<std::filesystem::path> selectedImages;
    bool placementReady = false;
    bool refreshing = false;
    bool closing = false;
    bool integratedFrame = false;
    bool showingTrayMenu = false;
    COLORREF headerColor = RGB(0, 0, 0);
    COLORREF headerTextColor = RGB(255, 255, 255);
    RECT defaultWorkArea{};
    std::vector<std::function<void()>> unsubscribe;

    int Selection() const { return topics.SelectedIndex(); }
    static LRESULT CALLBACK MenuWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            try {
                // Keep the standalone preview capturable when the UI test tool activates itself.
                if (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE && !self->callbacks.menuPreview) {
                    ShowWindow(hwnd, SW_HIDE); return 0;
                }
                if (message == WM_CLOSE) { ShowWindow(hwnd, SW_HIDE); return 0; }
                if (message == WM_COMMAND) {
                    ShowWindow(hwnd, SW_HIDE);
                    if (self->callbacks.menuCommand) self->callbacks.menuCommand(LOWORD(wParam));
                    return 0;
                }
                if (message == WM_SETFOCUS && self->menuIsland) { SetFocus(self->menuIsland); return 0; }
                if (message == WM_SIZE && self->menuIsland) {
                    SetWindowPos(self->menuIsland, nullptr, 0, 0, LOWORD(lParam), HIWORD(lParam), SWP_NOZORDER | SWP_SHOWWINDOW);
                    return 0;
                }
            }
            catch (...) { ShowWindow(hwnd, SW_HIDE); }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool ShowTrayMenu(POINT point)
    {
        // XAML initialization and UI Automation can pump nested window messages.
        // The shell may deliver another tray notification before the first call returns.
        if (showingTrayMenu) return true;
        showingTrayMenu = true;
        struct ShowingScope {
            bool& active;
            ~ShowingScope() { active = false; }
        } showingScope{showingTrayMenu};
        HIGHCONTRASTW contrast{sizeof(contrast)};
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
            (contrast.dwFlags & HCF_HIGHCONTRASTON)) return false;
        if (!manager) manager = WindowsXamlManager::InitializeForCurrentThread();
        if (!menuWindow) {
            WNDCLASSEXW cls{sizeof(cls)};
            cls.lpfnWndProc = MenuWindowProc;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            cls.lpszClassName = L"MTQQToaster.TrayMenu";
            if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw_last_error();
            menuWindow = CreateWindowExW((callbacks.menuPreview ? 0 : WS_EX_TOOLWINDOW) | WS_EX_TOPMOST, cls.lpszClassName,
                L"MTQQToaster", WS_POPUP | WS_BORDER, 0, 0, 320, 148, nullptr, nullptr, cls.hInstance, this);
            check_pointer(menuWindow);
            menuSource = DesktopWindowXamlSource();
            auto native = menuSource.as<IDesktopWindowXamlSourceNative>();
            check_hresult(native->AttachToWindow(menuWindow));
            check_hresult(native->get_WindowHandle(&menuIsland));
        }
        for (auto& revoke : menuUnsubscribe) revoke();
        menuUnsubscribe.clear();
        StackPanel panel;
        panel.Padding({4, 4, 4, 4});
        panel.TabFocusNavigation(Input::KeyboardNavigationMode::Cycle);
        DWORD light = 0, size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        panel.RequestedTheme(light ? ElementTheme::Light : ElementTheme::Dark);
        panel.Background(Media::SolidColorBrush(light ? Windows::UI::Color{255, 249, 249, 249} : Windows::UI::Color{255, 40, 40, 40}));
        const bool packaged = HasPackageIdentity();
        const int commands[] = {ID_SETTINGS, ID_STARTUP, ID_EXIT};
        const wchar_t* glyphs[] = {L"\xE713", L"\xE7B5", L"\xE8BB"};
        const UiString labels[] = {UiString::MenuSettings, packaged ? UiString::StartupSettings : UiString::Startup, UiString::Exit};
        std::vector<Button> buttons;
        for (int i = 0; i < 3; ++i) {
            if (i == 2) {
                Border separator;
                separator.Height(1);
                separator.Margin({8, 3, 8, 3});
                separator.Background(Media::SolidColorBrush(light ? Windows::UI::Color{255, 220, 220, 220} : Windows::UI::Color{255, 65, 65, 65}));
                panel.Children().Append(separator);
            }
            Button button;
            button.Height(44);
            button.HorizontalAlignment(HorizontalAlignment::Stretch);
            button.HorizontalContentAlignment(HorizontalAlignment::Left);
            button.CornerRadius({4, 4, 4, 4});
            button.Background(Media::SolidColorBrush(Windows::UI::Color{0, 0, 0, 0}));
            button.BorderThickness({0, 0, 0, 0});
            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.Spacing(12);
            FontIcon icon;
            icon.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
            icon.FontSize(18);
            icon.Glyph(i == 1 && !packaged && IsStartupRegistered() ? L"\xE73E" : glyphs[i]);
            row.Children().Append(icon);
            row.Children().Append(Label(UiText(labels[i]), 16));
            button.Content(row);
            Automation::AutomationProperties::SetName(button, UiText(labels[i]));
            auto token = button.Click([this, command = commands[i]](auto&&, auto&&) { PostMessageW(menuWindow, WM_COMMAND, command, 0); });
            menuUnsubscribe.push_back([button, token] { button.Click(token); });
            buttons.push_back(button);
            panel.Children().Append(button);
        }
        auto key = panel.KeyDown([this, buttons](auto&&, const Input::KeyRoutedEventArgs& args) {
            if (args.Key() == Windows::System::VirtualKey::Escape) {
                PostMessageW(menuWindow, WM_CLOSE, 0, 0);
                args.Handled(true);
            }
            else if (args.Key() == Windows::System::VirtualKey::Down || args.Key() == Windows::System::VirtualKey::Up) {
                size_t selected = 0;
                for (size_t i = 0; i < buttons.size(); ++i) if (buttons[i].FocusState() != FocusState::Unfocused) selected = i;
                selected = (selected + (args.Key() == Windows::System::VirtualKey::Down ? 1 : buttons.size() - 1)) % buttons.size();
                buttons[selected].Focus(FocusState::Keyboard);
                args.Handled(true);
            }
        });
        menuUnsubscribe.push_back([panel, key] { panel.KeyDown(key); });
        menuSource.Content(panel);
        const auto area = WorkArea(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST));
        SetWindowPos(menuWindow, nullptr, point.x, point.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        const auto dpi = GetDpiForWindow(menuWindow);
        const int width = MulDiv(320, dpi, 96), height = MulDiv(149, dpi, 96);
        const int x = (std::max)(area.left, (std::min)(point.x, area.right - width));
        const int y = (std::max)(area.top, (std::min)(point.y, area.bottom - height));
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUNDSMALL;
        DwmSetWindowAttribute(menuWindow, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        const COLORREF border = light ? RGB(220, 220, 220) : RGB(65, 65, 65);
        DwmSetWindowAttribute(menuWindow, DWMWA_BORDER_COLOR, &border, sizeof(border));
        check_bool(SetWindowPos(menuWindow, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW));
        SetForegroundWindow(menuWindow);
        panel.UpdateLayout();
        menuSource.NavigateFocus(XamlSourceFocusNavigationRequest(XamlSourceFocusNavigationReason::First));
        buttons.front().Focus(FocusState::Keyboard);
        return true;
    }
    std::wstring Text(int id) const
    {
        if (id == IDC_MQTT_PASS) return std::wstring(password.Password());
        if (id == IDC_LOG_LEVEL) return std::wstring(unbox_value<hstring>(logLevel.SelectedItem()));
        return std::wstring(edits.at(id).Text());
    }
    void SetText(int id, const std::wstring& value) { edits.at(id).Text(value); }
    void ShowError(const std::wstring& message) const
    {
        MessageBoxW(window, message.c_str(), UiText(UiString::WindowTitle), MB_OK | MB_ICONWARNING);
    }
    template<typename Action> void Guard(Action action) noexcept
    {
        try { if (!closing) action(); }
        catch (...) { ShowError(UiText(UiString::OperationFailed)); }
    }
    TextBlock Label(const wchar_t* text, double size = 14)
    {
        TextBlock label;
        label.Text(text);
        label.FontSize(size);
        label.VerticalAlignment(VerticalAlignment::Center);
        return label;
    }
    Button MakeButton(const wchar_t* text, int id)
    {
        Button button;
        button.Content(box_value(text));
        button.CornerRadius({4, 4, 4, 4});
        if (id == IDOK) button.AccessKey(L"S");
        button.MinWidth(88);
        button.Margin({0, 0, 8, 0});
        auto token = button.Click([this, id](auto&&, auto&&) {
            Guard([&] { Command(id, BN_CLICKED); });
        });
        unsubscribe.push_back([button, token] { button.Click(token); });
        return button;
    }
    TextBox Edit(int id, const wchar_t* name)
    {
        TextBox edit;
        edit.MinWidth(0);
        edit.CornerRadius({4, 4, 4, 4});
        edit.HorizontalAlignment(HorizontalAlignment::Stretch);
        Automation::AutomationProperties::SetName(edit, name);
        edits.emplace(id, edit);
        return edit;
    }
    StackPanel Card(const wchar_t* title)
    {
        StackPanel panel;
        panel.Spacing(8);
        panel.Children().Append(Label(title, 18));
        Border card;
        card.Padding({16, 8, 16, 10});
        card.CornerRadius({8, 8, 8, 8});
        // Theme resources preserve light/dark and high-contrast system colors.
        card.Background(Application::Current().Resources().Lookup(box_value(L"SystemControlBackgroundChromeMediumLowBrush")).as<Media::Brush>());
        card.Child(panel);
        content.Children().Append(card);
        return panel;
    }
    Grid Row(const wchar_t* label, UIElement control)
    {
        Grid row;
        ColumnDefinition left;
        left.Width({116, GridUnitType::Pixel});
        row.ColumnDefinitions().Append(left);
        ColumnDefinition right;
        right.Width({1, GridUnitType::Star});
        row.ColumnDefinitions().Append(right);
        row.Children().Append(Label(label));
        Grid::SetColumn(control.as<FrameworkElement>(), 1);
        row.Children().Append(control);
        return row;
    }
    void InitializeForm()
    {
        for (auto& revoke : unsubscribe) revoke();
        unsubscribe.clear();
        source.Content(nullptr);
        edits.clear();
        root = Grid();
        root.Width(700);
        root.Height(650);
        root.TabFocusNavigation(Input::KeyboardNavigationMode::Cycle);
        auto keyToken = root.KeyDown([this](auto&&, const Input::KeyRoutedEventArgs& args) {
            if (!closing && !args.Handled() && args.Key() == Windows::System::VirtualKey::Escape) {
                args.Handled(true);
                PostMessageW(window, CloseSettingsMessage, 0, 0);
            }
        });
        unsubscribe.push_back([element = root, keyToken] { element.KeyDown(keyToken); });
        root.Background(Application::Current().Resources().Lookup(box_value(L"SystemControlBackgroundAltHighBrush")).as<Media::Brush>());
        RowDefinition body;
        body.Height({1, GridUnitType::Star});
        root.RowDefinitions().Append(body);
        RowDefinition footer;
        footer.Height({1, GridUnitType::Auto});
        root.RowDefinitions().Append(footer);
        scroll = ScrollViewer();
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
        content = StackPanel();
        content.Spacing(8);
        content.Margin({16, 8, 16, 0});
        scroll.Content(content);
        root.Children().Append(scroll);
        auto connection = Card(UiText(UiString::Connection));
        connection.Children().Append(Row(UiText(UiString::Endpoint), Edit(IDC_MQTT_URL, L"MQTT URL")));
        edits.at(IDC_MQTT_URL).PlaceholderText(L"mqtts://host:8883");
        connection.Children().Append(Row(UiText(UiString::User), Edit(IDC_MQTT_USER, UiText(UiString::User))));
        password = PasswordBox();
        password.CornerRadius({4, 4, 4, 4});
        Automation::AutomationProperties::SetName(password, UiText(UiString::Password));
        connection.Children().Append(Row(UiText(UiString::Password), password));

        auto subscriptions = Card(UiText(UiString::Subscriptions));
        topics = ListView();
        topics.Height(80);
        ScrollViewer::SetVerticalScrollBarVisibility(topics, ScrollBarVisibility::Hidden);
        Automation::AutomationProperties::SetName(topics, UiText(UiString::Subscriptions));
        auto selectionToken = topics.SelectionChanged([this](auto&&, auto&&) {
            Guard([&] { if (!refreshing) LoadTopic(); });
        });
        unsubscribe.push_back([list = topics, selectionToken] { list.SelectionChanged(selectionToken); });
        subscriptions.Children().Append(topics);
        subscriptions.Children().Append(Row(UiText(UiString::Topic), Edit(IDC_TOPIC_NAME, UiText(UiString::Topic))));
        Grid imageRow;
        ColumnDefinition imageField;
        imageField.Width({1, GridUnitType::Star});
        imageRow.ColumnDefinitions().Append(imageField);
        ColumnDefinition imageButton;
        imageButton.Width({1, GridUnitType::Auto});
        imageRow.ColumnDefinitions().Append(imageButton);
        imageRow.Children().Append(Edit(IDC_TOPIC_IMAGE, UiText(UiString::Image)));
        auto browse = MakeButton(UiText(UiString::Browse), IDC_IMAGE_BROWSE);
        browse.Margin({8, 0, 0, 0});
        Grid::SetColumn(browse, 1);
        imageRow.Children().Append(browse);
        subscriptions.Children().Append(Row(UiText(UiString::Image), imageRow));
        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Children().Append(MakeButton(UiText(UiString::Update), IDC_TOPIC_EDIT));
        actions.Children().Append(MakeButton(UiText(UiString::Delete), IDC_TOPIC_DELETE));
        thumbnail = Image();
        thumbnail.Width(64);
        thumbnail.Height(48);
        thumbnail.Stretch(Media::Stretch::Uniform);
        Automation::AutomationProperties::SetName(thumbnail, UiText(UiString::Image));
        actions.Children().Append(thumbnail);
        subscriptions.Children().Append(actions);
        auto imageEdit = edits.at(IDC_TOPIC_IMAGE);
        auto imageToken = imageEdit.LostFocus([this](auto&&, auto&&) { Guard([&] { UpdateThumbnail(); }); });
        unsubscribe.push_back([imageEdit, imageToken] { imageEdit.LostFocus(imageToken); });

        auto logging = Card(UiText(UiString::Log));
        logRow = Grid();
        const wchar_t* names[] = {UiText(UiString::LogLevel), UiText(UiString::LogSize), UiText(UiString::LogFiles)};
        logLevel = ComboBox();
        logLevel.CornerRadius({4, 4, 4, 4});
        logLevel.HorizontalAlignment(HorizontalAlignment::Stretch);
        Automation::AutomationProperties::SetName(logLevel, names[0]);
        constexpr const wchar_t* levels[] = {L"TRACE", L"DEBUG", L"INFO", L"WARN", L"ERROR", L"NONE"};
        int selected = 2;
        for (int i = 0; i < 6; ++i) {
            logLevel.Items().Append(box_value(levels[i]));
            if (editingConfig.logLevel == to_string(levels[i])) selected = i;
        }
        logLevel.SelectedIndex(selected);
        UIElement logControls[] = {logLevel, Edit(IDC_LOG_SIZE, names[1]), Edit(IDC_LOG_FILES, names[2])};
        for (int i = 0; i < 3; ++i) {
            ColumnDefinition column;
            column.Width({1, GridUnitType::Star});
            logRow.ColumnDefinitions().Append(column);
            StackPanel field;
            field.Spacing(6);
            field.Margin({0, 0, i == 2 ? 0.0 : 12.0, 0});
            field.Children().Append(Label(names[i]));
            field.Children().Append(logControls[i]);
            Grid::SetColumn(field, i);
            logRow.Children().Append(field);
        }
        logging.Children().Append(logRow);
        auto hint = Label(UiText(UiString::RestartHint), 12);
        hint.TextWrapping(TextWrapping::Wrap);
        logging.Children().Append(hint);
        StackPanel buttons;
        buttons.Orientation(Orientation::Horizontal);
        buttons.HorizontalAlignment(HorizontalAlignment::Right);
        buttons.Margin({20, 12, 12, 12});
        buttons.Children().Append(MakeButton(UiText(UiString::Cancel), IDCANCEL));
        buttons.Children().Append(MakeButton(UiText(UiString::Save), IDOK));
        Grid::SetRow(buttons, 1);
        root.Children().Append(buttons);

        SetText(IDC_MQTT_URL, std::wstring(to_hstring(editingConfig.url)));
        SetText(IDC_MQTT_USER, std::wstring(to_hstring(editingConfig.user)));
        password.Password(to_hstring(editingConfig.pass));
        SetText(IDC_LOG_SIZE, std::to_wstring(editingConfig.maxLogSizeMb));
        SetText(IDC_LOG_FILES, std::to_wstring(editingConfig.maxLogFiles));
        RefreshTopics(editingConfig.topics.empty() ? -1 : 0);
        Viewbox fitting;
        fitting.Stretch(Media::Stretch::Uniform);
        fitting.StretchDirection(StretchDirection::DownOnly);
        fitting.VerticalAlignment(VerticalAlignment::Top);
        fitting.Child(root);
        source.Content(fitting);
    }

    void RefreshTopics(int selected)
    {
        refreshing = true;
        try {
            topics.Items().Clear();
            for (const auto& topic : editingConfig.topics) {
                std::wstring label(to_hstring(topic.topic));
                if (!topic.image.empty()) label += UiText(UiString::HasImage);
                topics.Items().Append(box_value(label));
            }
            topics.Items().Append(box_value(UiText(UiString::New)));
            topics.SelectedIndex(selected < 0 ? static_cast<int>(editingConfig.topics.size()) : selected);
        }
        catch (...) { refreshing = false; throw; }
        refreshing = false;
        LoadTopic();
    }
    void LoadTopic()
    {
        const int selected = Selection();
        if (selected < 0 || selected >= static_cast<int>(editingConfig.topics.size())) {
            SetText(IDC_TOPIC_NAME, L"");
            SetText(IDC_TOPIC_IMAGE, L"");
            UpdateThumbnail();
            return;
        }
        const auto index = static_cast<size_t>(selected);
        SetText(IDC_TOPIC_NAME, winrt::to_hstring(editingConfig.topics[index].topic).c_str());
        SetText(IDC_TOPIC_IMAGE, selectedImages[index].empty()
            ? std::wstring(winrt::to_hstring(editingConfig.topics[index].image)) : selectedImages[index].wstring());
        UpdateThumbnail();
    }

    void UpdateThumbnail() noexcept
    {
        try {
            thumbnail.Source(nullptr);
            std::filesystem::path path(Text(IDC_TOPIC_IMAGE));
            if (path.empty()) return;
            if (path.is_relative()) path = callbacks.imageDirectory / path;
            auto factory = create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
            com_ptr<IWICBitmapDecoder> decoder;
            check_hresult(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, decoder.put()));
            com_ptr<IWICBitmapFrameDecode> frame;
            check_hresult(decoder->GetFrame(0, frame.put()));
            UINT width = 0, height = 0;
            check_hresult(frame->GetSize(&width, &height));
            if (!width || !height) return;
            const double scale = (std::min)(128.0 / width, 96.0 / height);
            width = (std::max)(1u, static_cast<UINT>(width * scale));
            height = (std::max)(1u, static_cast<UINT>(height * scale));
            com_ptr<IWICBitmapScaler> scaler;
            check_hresult(factory->CreateBitmapScaler(scaler.put()));
            check_hresult(scaler->Initialize(frame.get(), width, height, WICBitmapInterpolationModeFant));
            com_ptr<IWICFormatConverter> converter;
            check_hresult(factory->CreateFormatConverter(converter.put()));
            check_hresult(converter->Initialize(scaler.get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
            Media::Imaging::WriteableBitmap bitmap(width, height);
            auto pixels = bitmap.PixelBuffer();
            check_hresult(converter->CopyPixels(nullptr, width * 4, pixels.Capacity(), pixels.data()));
            bitmap.Invalidate();
            thumbnail.Source(bitmap);
        }
        catch (...) { /* Missing or invalid files leave the preview empty. */ }
    }

    void UpdateTopic()
    {
        const auto name = Text(IDC_TOPIC_NAME);
        const auto first = name.find_first_not_of(L" \t");
        if (first == std::wstring::npos) { ShowError(UiText(UiString::EnterTopic)); return; }
        TopicConfig topic;
        topic.topic = winrt::to_string(std::wstring_view(name).substr(first, name.find_last_not_of(L" \t") - first + 1));
        const std::filesystem::path imageSource(Text(IDC_TOPIC_IMAGE));
        const int selected = Selection();
        if (selected >= 0 && selected < static_cast<int>(editingConfig.topics.size())) {
            const auto index = static_cast<size_t>(selected);
            const auto previous = winrt::to_hstring(editingConfig.topics[index].image);
            // An empty field explicitly removes the old image; restoring the old name cancels a pending copy.
            if (!imageSource.empty()) topic.image = editingConfig.topics[index].image;
            selectedImages[index] = imageSource == std::filesystem::path(previous.c_str()) ? std::filesystem::path{} : imageSource;
            editingConfig.topics[index] = std::move(topic);
            RefreshTopics(selected);
        }
        else {
            editingConfig.topics.push_back(std::move(topic));
            selectedImages.push_back(imageSource);
            RefreshTopics(static_cast<int>(editingConfig.topics.size()) - 1);
        }
    }

    void BrowseImage()
    {
        std::wstring path(32768, L'\0');
        OPENFILENAMEW options{sizeof(options)};
        options.hwndOwner = window;
        options.lpstrFilter = IsJapaneseUi()
            ? L"画像 (*.png;*.jpg;*.jpeg;*.gif)\0*.png;*.jpg;*.jpeg;*.gif\0すべてのファイル\0*.*\0"
            : L"Images (*.png;*.jpg;*.jpeg;*.gif)\0*.png;*.jpg;*.jpeg;*.gif\0All files\0*.*\0";
        options.lpstrFile = path.data();
        options.nMaxFile = static_cast<DWORD>(path.size());
        options.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&options)) { SetText(IDC_TOPIC_IMAGE, path.c_str()); UpdateThumbnail(); }
        else if (CommDlgExtendedError()) ShowError(UiText(UiString::BrowseFailed));
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
            ShowError(UiText(UiString::InvalidLog));
            return;
        }
        std::wstring error;
        if (!callbacks.saveConfig(candidate, selectedImages, error)) { ShowError(error); return; }
        PostMessageW(window, CloseSettingsMessage, 0, 0);
    }

    void Command(int id, int notification)
    {
        if (id == IDC_TOPIC_LIST && notification == LBN_SELCHANGE) LoadTopic();
        else if (notification == BN_CLICKED) {
            switch (id) {
            case IDOK: Save(); break;
            case IDCANCEL: PostMessageW(window, CloseSettingsMessage, 0, 0); break;
            case IDC_IMAGE_BROWSE: BrowseImage(); break;
            case IDC_TOPIC_EDIT: UpdateTopic(); break;
            case IDC_TOPIC_ADD: topics.SelectedIndex(static_cast<int>(editingConfig.topics.size())); LoadTopic(); break;
            case IDC_TOPIC_DELETE: {
                const int selected = Selection();
                if (selected < 0 || selected >= static_cast<int>(editingConfig.topics.size())) break;
                editingConfig.topics.erase(editingConfig.topics.begin() + selected);
                selectedImages.erase(selectedImages.begin() + selected);
                RefreshTopics((std::min)(selected, static_cast<int>(editingConfig.topics.size()) - 1));
                break;
            }
            }
        }
    }

    int HeaderHeight() const { return integratedFrame ? MulDiv(44, GetDpiForWindow(window), 96) : 0; }

    void UpdateFrame()
    {
        HIGHCONTRASTW contrast{sizeof(contrast)};
        const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
            (contrast.dwFlags & HCF_HIGHCONTRASTON);
        if (root) {
            const auto color = root.Background().as<Media::SolidColorBrush>().Color();
            headerColor = RGB(color.R, color.G, color.B);
            headerTextColor = root.ActualTheme() == ElementTheme::Dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
        }
        const BOOL dark = root && root.ActualTheme() == ElementTheme::Dark;
        const auto caption = highContrast ? DWMWA_COLOR_DEFAULT : headerColor;
        const auto foreground = highContrast ? DWMWA_COLOR_DEFAULT : headerTextColor;
        // These attributes are supported on Windows 11. Keep the native frame on older OSes.
        const HRESULT support = DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        const bool custom = SUCCEEDED(support) && !highContrast;
        if (FAILED(DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &foreground, sizeof(foreground))) && custom)
            OutputDebugStringW(L"MTQQToaster: title text color unavailable.\n");
        if (FAILED(DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))) && custom)
            OutputDebugStringW(L"MTQQToaster: dark frame unavailable.\n");
        if (custom != integratedFrame) {
            integratedFrame = custom;
            check_bool(SetWindowPos(window, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED));
        }
        const MARGINS margins{0, 0, custom ? HeaderHeight() : 0, 0};
        if (FAILED(DwmExtendFrameIntoClientArea(window, &margins)) && custom)
            OutputDebugStringW(L"MTQQToaster: frame extension unavailable.\n");
        InvalidateRect(window, nullptr, TRUE);
        Layout();
    }

    void PaintHeader()
    {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        if (dc) {
            RECT bounds{};
            GetClientRect(window, &bounds);
            bounds.bottom = HeaderHeight();
            const auto brush = CreateSolidBrush(headerColor);
            if (brush) { FillRect(dc, &bounds, brush); DeleteObject(brush); }
            const auto dpi = GetDpiForWindow(window);
            const auto font = CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Segoe UI");
            const auto old = font ? SelectObject(dc, font) : nullptr;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, headerTextColor);
            bounds.left = MulDiv(20, dpi, 96);
            bounds.right -= MulDiv(150, dpi, 96);
            DrawTextW(dc, UiText(UiString::WindowTitle), -1, &bounds,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (font) { SelectObject(dc, old); DeleteObject(font); }
        }
        EndPaint(window, &paint);
    }

    void Layout()
    {
        if (!island) return;
        RECT client{};
        check_bool(GetClientRect(window, &client));
        const int top = HeaderHeight();
        check_bool(SetWindowPos(island, nullptr, 0, top, client.right, (std::max)(1L, client.bottom - top),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW));
    }
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
        try {
            LRESULT dwmResult = 0;
            if (self->integratedFrame && DwmDefWindowProc(hwnd, message, wParam, lParam, &dwmResult)) return dwmResult;
            switch (message) {
            case WM_NCCALCSIZE:
                if (self->integratedFrame && wParam) {
                    auto params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                    const LONG top = params->rgrc[0].top;
                    DefWindowProcW(hwnd, message, wParam, lParam);
                    const auto dpi = GetDpiForWindow(hwnd);
                    params->rgrc[0].top = top + (IsZoomed(hwnd)
                        ? GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi) : 0);
                    return 0;
                }
                break;
            case WM_NCHITTEST:
                if (self->integratedFrame) {
                    const auto hit = DefWindowProcW(hwnd, message, wParam, lParam);
                    if (hit != HTCLIENT) return hit;
                    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    ScreenToClient(hwnd, &point);
                    if (point.y < self->HeaderHeight()) return HTCAPTION;
                    return HTCLIENT;
                }
                break;
            case WM_PAINT:
                if (self->integratedFrame) { self->PaintHeader(); return 0; }
                break;
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED: self->UpdateFrame(); return 0;
            case WM_CLOSE:
            case CloseSettingsMessage: self->CloseAll(); return 0;
            case WM_SIZE: if (wParam != SIZE_MINIMIZED) self->Layout(); return 0;
            case WM_DPICHANGED: {
                const auto bounds = reinterpret_cast<RECT*>(lParam);
                check_bool(SetWindowPos(hwnd, nullptr, bounds->left, bounds->top,
                    bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE));
                self->Layout();
                self->UpdateFrame();
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto info = reinterpret_cast<MINMAXINFO*>(lParam);
                const auto dpi = GetDpiForWindow(hwnd);
                info->ptMinTrackSize = {MulDiv(540, dpi, 96), MulDiv(360, dpi, 96)};
                return 0;
            }
            case WM_SETFOCUS:
                if (self->island) SetFocus(self->island);
                return 0;
            case WM_NCDESTROY:
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                self->window = nullptr;
                break;
            }
        }
        catch (...) { self->ShowError(UiText(UiString::OperationFailed)); }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    void ApplyDefaultPlacement()
    {
        placementReady = false;
        ShowWindow(window, SW_RESTORE);
        POINT pointer{};
        check_bool(GetCursorPos(&pointer));
        defaultWorkArea = WorkArea(MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST));
        check_bool(SetWindowPos(window, nullptr, defaultWorkArea.left, defaultWorkArea.top, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
        const auto dpi = GetDpiForWindow(window);
        RECT rect{0, 0, MulDiv(700, dpi, 96), MulDiv(680, dpi, 96)};
        check_bool(AdjustWindowRectExForDpi(&rect, SettingsStyle, FALSE, 0, dpi));
        auto placement = FitSettingsWindowToWorkArea({0, 0, rect.right - rect.left, rect.bottom - rect.top}, defaultWorkArea);
        placement.x = defaultWorkArea.left + (defaultWorkArea.right - defaultWorkArea.left - placement.width) / 2;
        placement.y = defaultWorkArea.top + (defaultWorkArea.bottom - defaultWorkArea.top - placement.height) / 2;
        check_bool(SetWindowPos(window, nullptr, placement.x, placement.y, placement.width, placement.height,
            SWP_NOZORDER | SWP_NOACTIVATE));
        if (scroll) scroll.ChangeView(nullptr, box_value(0.0).as<Windows::Foundation::IReference<double>>(), nullptr, true);
        Layout();
        placementReady = true;
    }
    void ShowSettings(bool resetPlacement)
    {
        if (window && IsWindowVisible(window)) {
            if (resetPlacement) ApplyDefaultPlacement();
            if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
            return;
        }
        closing = false;
        editingConfig = callbacks.loadConfig();
        selectedImages.assign(editingConfig.topics.size(), {});
        const auto saved = !resetPlacement && callbacks.loadPlacement ? callbacks.loadPlacement() : std::nullopt;
        try {
            if (!manager) manager = WindowsXamlManager::InitializeForCurrentThread();
            WNDCLASSEXW cls{sizeof(cls)};
            cls.lpfnWndProc = WindowProc;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            cls.lpszClassName = L"MTQQToaster.Settings";
            if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw_last_error();
            if (!window) {
                check_pointer(CreateWindowExW(0, cls.lpszClassName, UiText(UiString::WindowTitle), SettingsStyle,
                    CW_USEDEFAULT, CW_USEDEFAULT, 700, 780, nullptr, nullptr, cls.hInstance, this));
                source = DesktopWindowXamlSource();
                auto native = source.as<IDesktopWindowXamlSourceNative>();
                check_hresult(native->AttachToWindow(window));
                check_hresult(native->get_WindowHandle(&island));
            }
            InitializeForm();
            UpdateFrame();
            Layout();
            ApplyDefaultPlacement();
            if (saved) {
                RECT current{};
                check_bool(GetWindowRect(window, &current));
                const RECT bounds{saved->x, saved->y, saved->x + saved->width, saved->y + saved->height};
                const auto placement = FitSettingsWindowToWorkArea({saved->x, saved->y,
                    current.right - current.left, current.bottom - current.top}, WorkArea(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST)));
                check_bool(SetWindowPos(window, nullptr, placement.x, placement.y, placement.width, placement.height,
                    SWP_NOZORDER | SWP_NOACTIVATE));
                ShowWindow(window, SW_SHOWNORMAL);
            }
            else { ApplyDefaultPlacement(); ShowWindow(window, SW_SHOW); }
            placementReady = true;
            SetForegroundWindow(window);
            source.NavigateFocus(XamlSourceFocusNavigationRequest(XamlSourceFocusNavigationReason::First));
        }
        catch (...) { CloseAll(); ReleaseHost(); throw; }
    }
    void CloseAll() noexcept
    {
        closing = true;
        try {
            if (window && placementReady && callbacks.savePlacement) {
                WINDOWPLACEMENT placement{sizeof(placement)};
                check_bool(GetWindowPlacement(window, &placement));
                MONITORINFO monitor{sizeof(monitor)};
                check_bool(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor));
                auto rect = placement.rcNormalPosition;
                OffsetRect(&rect, monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top);
                callbacks.savePlacement({rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                    placement.showCmd == SW_SHOWMAXIMIZED || (placement.flags & WPF_RESTORETOMAXIMIZED) != 0});
            }
        }
        catch (...) { OutputDebugStringW(L"MTQQToaster: could not save settings window placement.\n"); }
        placementReady = false;
        // Retain the island until application shutdown. Destroying the last island
        // between openings can interfere with other WinRT callbacks on this STA.
        if (window) ShowWindow(window, SW_HIDE);
    }
    bool IsSettingsReady() const
    {
        return window && source && root && root.ActualWidth() > 0 && IsWindowVisible(window) && edits.size() == 6 &&
            (editingConfig.topics.empty() || (Selection() == 0 &&
                Text(IDC_TOPIC_NAME) == to_hstring(editingConfig.topics[0].topic)));
    }
    bool IsDefaultLayoutReady() const
    {
        if (!IsSettingsReady() || !placementReady) {
            spdlog::warn("Settings layout not ready: ready={}, placement={}", IsSettingsReady(), placementReady);
            return false;
        }
        RECT rect{};
        check_bool(GetWindowRect(window, &rect));
        root.UpdateLayout();
        const bool centered = std::abs(rect.left + rect.right - defaultWorkArea.left - defaultWorkArea.right) <= 2 &&
            std::abs(rect.top + rect.bottom - defaultWorkArea.top - defaultWorkArea.bottom) <= 2;
        const auto a = logLevel.TransformToVisual(logRow).TransformPoint({0, 0});
        const auto b = edits.at(IDC_LOG_SIZE).TransformToVisual(logRow).TransformPoint({0, 0});
        const auto c = edits.at(IDC_LOG_FILES).TransformToVisual(logRow).TransformPoint({0, 0});
        const bool valid = centered && content.ActualHeight() <= scroll.ActualHeight() &&
            scroll.ScrollableHeight() == 0 && scroll.VerticalOffset() == 0 && root.ActualWidth() > 0 &&
            std::abs(a.Y - b.Y) < 1 && std::abs(b.Y - c.Y) < 1 &&
            a.X + logLevel.ActualWidth() < b.X && b.X + edits.at(IDC_LOG_SIZE).ActualWidth() < c.X;
        if (!valid) spdlog::warn("Settings layout: centered={}, offset={}, width={}, logY={}/{}/{}, logX={}/{}/{}, logWidths={}/{}",
            centered, scroll.VerticalOffset(), root.ActualWidth(), a.Y, b.Y, c.Y, a.X, b.X, c.X,
            logLevel.ActualWidth(), edits.at(IDC_LOG_SIZE).ActualWidth());
        return valid;
    }
    bool ProcessMessage(MSG& message)
    {
        if (menuSource && IsWindowVisible(menuWindow)) {
            BOOL handled = FALSE;
            check_hresult(menuSource.as<IDesktopWindowXamlSourceNative2>()->PreTranslateMessage(&message, &handled));
            if (handled) return true;
        }
        if (!source) return false;
        BOOL handled = FALSE;
        check_hresult(source.as<IDesktopWindowXamlSourceNative2>()->PreTranslateMessage(&message, &handled));
        return handled != FALSE;
    }
};

SettingsUi::SettingsUi(SettingsUiCallbacks callbacks) : impl_(std::make_unique<Impl>(std::move(callbacks))) {}
SettingsUi::~SettingsUi() = default;
void SettingsUi::ShowSettings(bool resetPlacement) { impl_->ShowSettings(resetPlacement); }
bool SettingsUi::ShowTrayMenu(POINT point)
{
    try { return impl_->ShowTrayMenu(point); }
    catch (...) {
        if (impl_->menuWindow) ShowWindow(impl_->menuWindow, SW_HIDE);
        throw;
    }
}
bool SettingsUi::IsTrayMenuVisible() const { return impl_->menuWindow && IsWindowVisible(impl_->menuWindow); }
void SettingsUi::ExerciseTrayMenuForSmokeTest()
{
    POINT point{};
    check_bool(GetCursorPos(&point));
    for (int i = 0; i < 2; ++i) {
        if (!ShowTrayMenu(point)) return; // High contrast deliberately uses the native menu.
        if (!IsTrayMenuVisible()) throw std::runtime_error("Tray menu was not shown");
        SendMessageW(impl_->menuWindow, WM_CLOSE, 0, 0);
        if (IsTrayMenuVisible()) throw std::runtime_error("Tray menu was not dismissed");
    }
}
bool SettingsUi::IsSettingsReady() const { return impl_->IsSettingsReady(); }
bool SettingsUi::IsDefaultLayoutReady() const { return impl_->IsDefaultLayoutReady(); }
HWND SettingsUi::SettingsHandle() const { return impl_->window && IsWindowVisible(impl_->window) ? impl_->window : nullptr; }
void SettingsUi::CloseAll() { impl_->CloseAll(); }
bool SettingsUi::ProcessMessage(MSG& message) { return impl_->ProcessMessage(message); }
void SettingsUi::ExerciseEditsForSmokeTest()
{
    const auto style = GetWindowLongPtrW(impl_->window, GWL_STYLE);
    if (style & (WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) throw std::runtime_error("Settings window is not fixed size");
    impl_->SetText(IDC_TOPIC_IMAGE, L"");
    impl_->Command(IDC_TOPIC_EDIT, BN_CLICKED);
    impl_->Command(IDC_TOPIC_ADD, BN_CLICKED);
    const auto count = impl_->editingConfig.topics.size();
    if (impl_->Selection() != static_cast<int>(count) ||
        unbox_value<hstring>(impl_->topics.SelectedItem()) != UiText(UiString::New)) throw std::runtime_error("New topic item missing");
    impl_->Command(IDC_TOPIC_DELETE, BN_CLICKED);
    if (impl_->editingConfig.topics.size() != count) throw std::runtime_error("New item deleted a topic");
    impl_->SetText(IDC_TOPIC_NAME, L"追加/日本語/#");
    impl_->Command(IDC_TOPIC_EDIT, BN_CLICKED);
    if (impl_->editingConfig.topics.back().topic != "追加/日本語/#") throw std::runtime_error("Topic edit failed");
    impl_->Command(IDC_TOPIC_DELETE, BN_CLICKED);
    impl_->SetText(IDC_MQTT_USER, L"テスト");
    impl_->SetText(IDC_LOG_SIZE, L"12");
    impl_->Command(IDOK, BN_CLICKED);
    // Production defers teardown until Click returns; the test has no active event.
    MSG pending{};
    while (PeekMessageW(&pending, impl_->window, CloseSettingsMessage, CloseSettingsMessage, PM_REMOVE)) {}
    SendMessageW(impl_->window, CloseSettingsMessage, 0, 0);
}
void SettingsUi::ExerciseCancelForSmokeTest()
{
    impl_->SetText(IDC_MQTT_USER, L"cancelled");
    impl_->Command(IDCANCEL, BN_CLICKED);
    MSG pending{};
    while (PeekMessageW(&pending, impl_->window, CloseSettingsMessage, CloseSettingsMessage, PM_REMOVE)) {}
    SendMessageW(impl_->window, CloseSettingsMessage, 0, 0);
}
