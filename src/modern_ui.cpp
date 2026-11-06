#include "modern_ui.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <commdlg.h>
#include <microsoft.ui.xaml.window.h>

#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>

using namespace winrt;
using namespace Microsoft::UI;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace {

HWND GetWindowHandle(const Window& window)
{
    HWND hwnd = nullptr;
    check_hresult(window.as<::IWindowNative>()->get_WindowHandle(&hwnd));
    return hwnd;
}

Button MakeButton(const wchar_t* text)
{
    Button button;
    button.Content(box_value(text));
    button.HorizontalAlignment(HorizontalAlignment::Stretch);
    return button;
}

TextBlock MakeLabel(const wchar_t* text)
{
    TextBlock label;
    label.Text(text);
    label.Margin({0, 8, 0, 4});
    return label;
}

TextBlock MakeSectionTitle(const wchar_t* text)
{
    TextBlock title;
    title.Text(text);
    title.FontSize(20);
    title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    title.Margin({0, 18, 0, 6});
    return title;
}

RECT WorkArea(HMONITOR monitor)
{
    MONITORINFO info{sizeof(info)};
    check_bool(GetMonitorInfoW(monitor, &info));
    return info.rcWork;
}

void MoveWindow(const Window& window, const SettingsWindowPlacement& placement)
{
    window.AppWindow().MoveAndResize({placement.x, placement.y, placement.width, placement.height});
}

}

struct ModernUi::Impl {
    explicit Impl(ModernUiCallbacks value) : callbacks(std::move(value)) {}

    ModernUiCallbacks callbacks;
    Window settingsWindow{nullptr};
    AppConfig editingConfig;
    std::vector<std::filesystem::path> selectedImages;

    TextBox mqttUrl{nullptr};
    TextBox mqttUser{nullptr};
    PasswordBox mqttPassword{nullptr};
    ComboBox logLevel{nullptr};
    NumberBox logSize{nullptr};
    NumberBox logFiles{nullptr};
    ListView topicList{nullptr};
    TextBox topicName{nullptr};
    TextBox topicImage{nullptr};
    InfoBar status{nullptr};
    StackPanel settingsForm{nullptr};
    StackPanel settingsActions{nullptr};
    ScrollViewer settingsScroll{nullptr};
    // Keep the event source alive and explicitly unsubscribe when the window closes.
    AppWindow observedWindow{nullptr};
    event_token placementChanged{};
    std::optional<SettingsWindowPlacement> lastPlacement;
    bool placementReady = false;
    RECT defaultWorkArea{};

    void CapturePlacement(const AppWindow& appWindow)
    {
        if (!placementReady) return;
        const auto presenter = appWindow.Presenter().try_as<OverlappedPresenter>();
        if (!presenter || presenter.State() == OverlappedPresenterState::Minimized) return;
        if (presenter.State() == OverlappedPresenterState::Maximized) {
            if (lastPlacement) lastPlacement->maximized = true;
            return;
        }
        const auto position = appWindow.Position();
        const auto size = appWindow.Size();
        if (size.Width > 0 && size.Height > 0) {
            lastPlacement = SettingsWindowPlacement{position.X, position.Y, size.Width, size.Height, false};
        }
    }

    void ApplyDefaultPlacement()
    {
        auto window = settingsWindow;
        placementReady = false;
        if (const auto presenter = window.AppWindow().Presenter().try_as<OverlappedPresenter>()) presenter.Restore();
        POINT pointer{};
        GetCursorPos(&pointer);
        defaultWorkArea = WorkArea(MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST));
        // Move onto the destination monitor before measuring with its DPI.
        window.AppWindow().Move({defaultWorkArea.left, defaultWorkArea.top});
        const HWND hwnd = GetWindowHandle(window);
        const double scale = static_cast<double>(GetDpiForWindow(hwnd)) / USER_DEFAULT_SCREEN_DPI;
        RECT nonClient{};
        check_bool(AdjustWindowRectExForDpi(&nonClient,
            static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd)));
        const int borderWidth = nonClient.right - nonClient.left;
        const int borderHeight = nonClient.bottom - nonClient.top;
        const float clientWidth = static_cast<float>((std::min)(660.0,
            (defaultWorkArea.right - defaultWorkArea.left - borderWidth) / scale));
        const float formWidth = (std::max)(1.0f, clientWidth - 56.0f);
        settingsForm.Measure({formWidth, std::numeric_limits<float>::infinity()});
        settingsActions.Measure({formWidth, std::numeric_limits<float>::infinity()});
        // Include root padding, the action-row margin and a rounding allowance.
        const double clientHeight = settingsForm.DesiredSize().Height + settingsActions.DesiredSize().Height + 40;
        const int width = static_cast<int>(std::ceil(clientWidth * scale)) + borderWidth;
        const int height = static_cast<int>(std::ceil(clientHeight * scale)) + borderHeight;
        auto placement = FitSettingsWindowToWorkArea({0, 0, width, height}, defaultWorkArea);
        placement.x = defaultWorkArea.left + (defaultWorkArea.right - defaultWorkArea.left - placement.width) / 2;
        placement.y = defaultWorkArea.top + (defaultWorkArea.bottom - defaultWorkArea.top - placement.height) / 2;
        MoveWindow(window, placement);
        lastPlacement = placement;
        placementReady = true;
    }

    void RestorePlacement(const SettingsWindowPlacement& saved)
    {
        const RECT bounds{saved.x, saved.y, saved.x + saved.width, saved.y + saved.height};
        const auto workArea = WorkArea(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST));
        lastPlacement = FitSettingsWindowToWorkArea(saved, workArea);
        MoveWindow(settingsWindow, *lastPlacement);
        if (saved.maximized) {
            if (const auto presenter = settingsWindow.AppWindow().Presenter().try_as<OverlappedPresenter>()) presenter.Maximize();
        }
        placementReady = true;
    }

    void ShowStatus(const std::wstring& message, InfoBarSeverity severity)
    {
        status.Message(message);
        status.Severity(severity);
        status.IsOpen(true);
    }

    void RefreshTopicList(int selected = -1)
    {
        topicList.Items().Clear();
        for (const auto& topic : editingConfig.topics) {
            std::wstring label = to_hstring(topic.topic).c_str();
            if (!topic.image.empty()) label += L"  ·  画像あり";
            topicList.Items().Append(box_value(label));
        }
        if (selected >= 0 && selected < static_cast<int>(editingConfig.topics.size())) {
            topicList.SelectedIndex(selected);
        }
    }

    void LoadSelectedTopic()
    {
        const int selected = topicList.SelectedIndex();
        if (selected < 0 || selected >= static_cast<int>(editingConfig.topics.size())) {
            topicName.Text(L"");
            topicImage.Text(L"");
            return;
        }
        topicName.Text(to_hstring(editingConfig.topics[static_cast<size_t>(selected)].topic));
        const auto& source = selectedImages[static_cast<size_t>(selected)];
        topicImage.Text(source.empty()
                            ? to_hstring(editingConfig.topics[static_cast<size_t>(selected)].image)
                            : hstring(source.wstring()));
    }

    void BrowseForImage()
    {
        std::wstring fileName(32768, L'\0');
        OPENFILENAMEW options{};
        options.lStructSize = sizeof(options);
        options.hwndOwner = GetWindowHandle(settingsWindow);
        options.lpstrFilter = L"画像ファイル (*.png;*.jpg;*.jpeg;*.gif)\0*.png;*.jpg;*.jpeg;*.gif\0すべてのファイル\0*.*\0";
        options.lpstrFile = fileName.data();
        options.nMaxFile = static_cast<DWORD>(fileName.size());
        options.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&options)) return;

        fileName.resize(wcslen(fileName.c_str()));
        topicImage.Text(fileName);
    }

    void UpdateTopic()
    {
        std::wstring value = topicName.Text().c_str();
        const auto first = value.find_first_not_of(L" \t");
        const auto last = value.find_last_not_of(L" \t");
        if (first == std::wstring::npos) {
            ShowStatus(L"topicを入力してください。", InfoBarSeverity::Warning);
            return;
        }

        TopicConfig topic;
        topic.topic = to_string(hstring(value.substr(first, last - first + 1)));
        const int selected = topicList.SelectedIndex();
        if (selected >= 0 && selected < static_cast<int>(editingConfig.topics.size())) {
            const size_t index = static_cast<size_t>(selected);
            topic.image = editingConfig.topics[index].image;
            editingConfig.topics[index] = std::move(topic);
            const std::filesystem::path imagePath(topicImage.Text().c_str());
            if (!imagePath.empty() && imagePath != std::filesystem::path(to_hstring(editingConfig.topics[index].image).c_str())) {
                selectedImages[index] = imagePath;
            }
            RefreshTopicList(selected);
        }
        else {
            editingConfig.topics.push_back(std::move(topic));
            selectedImages.emplace_back(topicImage.Text().c_str());
            RefreshTopicList(static_cast<int>(editingConfig.topics.size()) - 1);
        }
        status.IsOpen(false);
    }

    void DeleteTopic()
    {
        const int selected = topicList.SelectedIndex();
        if (selected < 0 || selected >= static_cast<int>(editingConfig.topics.size())) return;
        editingConfig.topics.erase(editingConfig.topics.begin() + selected);
        selectedImages.erase(selectedImages.begin() + selected);
        RefreshTopicList((std::min)(selected, static_cast<int>(editingConfig.topics.size()) - 1));
        LoadSelectedTopic();
    }

    void SaveSettings()
    {
        editingConfig.url = to_string(mqttUrl.Text());
        editingConfig.user = to_string(mqttUser.Text());
        editingConfig.pass = to_string(mqttPassword.Password());
        if (const auto selected = logLevel.SelectedItem()) {
            editingConfig.logLevel = to_string(unbox_value<hstring>(selected));
        }

        const double sizeValue = logSize.Value();
        const double filesValue = logFiles.Value();
        if (!std::isfinite(sizeValue) || !std::isfinite(filesValue) || sizeValue < 1 || filesValue < 1) {
            ShowStatus(L"ログサイズと保持数には1以上の整数を入力してください。", InfoBarSeverity::Warning);
            return;
        }
        editingConfig.maxLogSizeMb = static_cast<size_t>(sizeValue);
        editingConfig.maxLogFiles = static_cast<size_t>(filesValue);

        std::wstring error;
        if (!callbacks.saveConfig(editingConfig, selectedImages, error)) {
            ShowStatus(error, InfoBarSeverity::Error);
            return;
        }
        settingsWindow.Close();
    }

    void ShowSettings(bool resetPlacement)
    {
        if (settingsWindow) {
            if (resetPlacement) ApplyDefaultPlacement();
            settingsWindow.Activate();
            return;
        }

        placementReady = false;
        lastPlacement.reset();
        const auto saved = !resetPlacement && callbacks.loadPlacement ? callbacks.loadPlacement() : std::nullopt;

        editingConfig = callbacks.loadConfig();
        selectedImages.assign(editingConfig.topics.size(), {});

        settingsWindow = Window();
        settingsWindow.Title(L"MTQQToaster 設定");
        settingsWindow.SystemBackdrop(Media::MicaBackdrop());

        auto root = Grid();
        root.Padding({28, 18, 28, 18});
        root.RowDefinitions().Append(RowDefinition());
        root.RowDefinitions().Append(RowDefinition());
        root.RowDefinitions().GetAt(0).Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        root.RowDefinitions().GetAt(1).Height(GridLengthHelper::Auto());

        auto scroll = ScrollViewer();
        settingsScroll = scroll;
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        auto form = StackPanel();
        settingsForm = form;
        form.Spacing(2);

        status = InfoBar();
        status.IsOpen(false);
        status.IsClosable(true);
        form.Children().Append(status);

        auto heading = TextBlock();
        heading.Text(L"設定");
        heading.FontSize(28);
        heading.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        form.Children().Append(heading);

        form.Children().Append(MakeSectionTitle(L"MQTT接続"));
        form.Children().Append(MakeLabel(L"MQTT URL"));
        mqttUrl = TextBox();
        mqttUrl.Text(to_hstring(editingConfig.url));
        mqttUrl.PlaceholderText(L"mqtt://127.0.0.1:1883");
        form.Children().Append(mqttUrl);

        form.Children().Append(MakeLabel(L"ユーザー名"));
        mqttUser = TextBox();
        mqttUser.Text(to_hstring(editingConfig.user));
        form.Children().Append(mqttUser);

        form.Children().Append(MakeLabel(L"パスワード"));
        mqttPassword = PasswordBox();
        mqttPassword.Password(to_hstring(editingConfig.pass));
        form.Children().Append(mqttPassword);

        form.Children().Append(MakeSectionTitle(L"購読topic"));
        topicList = ListView();
        topicList.MaxHeight(180);
        topicList.SelectionMode(ListViewSelectionMode::Single);
        topicList.SelectionChanged([this](auto&&, auto&&) { LoadSelectedTopic(); });
        form.Children().Append(topicList);

        form.Children().Append(MakeLabel(L"topic"));
        topicName = TextBox();
        topicName.PlaceholderText(L"home/notifications/#");
        form.Children().Append(topicName);

        form.Children().Append(MakeLabel(L"既定画像"));
        auto imageRow = Grid();
        imageRow.ColumnDefinitions().Append(ColumnDefinition());
        imageRow.ColumnDefinitions().Append(ColumnDefinition());
        imageRow.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        imageRow.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::Auto());
        imageRow.ColumnSpacing(8);
        topicImage = TextBox();
        topicImage.IsReadOnly(true);
        topicImage.PlaceholderText(L"画像なし");
        imageRow.Children().Append(topicImage);
        auto browse = MakeButton(L"参照…");
        Grid::SetColumn(browse, 1);
        browse.Click([this](auto&&, auto&&) { BrowseForImage(); });
        imageRow.Children().Append(browse);
        form.Children().Append(imageRow);

        auto topicButtons = StackPanel();
        topicButtons.Orientation(Orientation::Horizontal);
        topicButtons.Spacing(8);
        topicButtons.Margin({0, 8, 0, 0});
        auto update = MakeButton(L"追加 / 更新");
        update.Click([this](auto&&, auto&&) { UpdateTopic(); });
        auto clear = MakeButton(L"新規");
        clear.Click([this](auto&&, auto&&) {
            topicList.SelectedIndex(-1);
            topicName.Text(L"");
            topicImage.Text(L"");
        });
        auto remove = MakeButton(L"削除");
        remove.Click([this](auto&&, auto&&) { DeleteTopic(); });
        topicButtons.Children().Append(update);
        topicButtons.Children().Append(clear);
        topicButtons.Children().Append(remove);
        form.Children().Append(topicButtons);

        form.Children().Append(MakeSectionTitle(L"ログ"));
        auto levelPanel = StackPanel();
        levelPanel.Children().Append(MakeLabel(L"ログレベル"));
        logLevel = ComboBox();
        constexpr const wchar_t* levels[] = {L"TRACE", L"DEBUG", L"INFO", L"WARN", L"ERROR", L"NONE"};
        int selectedLevel = 2;
        for (int i = 0; i < static_cast<int>(std::size(levels)); ++i) {
            logLevel.Items().Append(box_value(levels[i]));
            if (editingConfig.logLevel == to_string(hstring(levels[i]))) selectedLevel = i;
        }
        logLevel.SelectedIndex(selectedLevel);
        logLevel.HorizontalAlignment(HorizontalAlignment::Stretch);
        levelPanel.Children().Append(logLevel);

        auto logRow = Grid();
        logRow.ColumnDefinitions().Append(ColumnDefinition());
        logRow.ColumnDefinitions().Append(ColumnDefinition());
        logRow.ColumnDefinitions().Append(ColumnDefinition());
        logRow.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        logRow.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        logRow.ColumnDefinitions().GetAt(2).Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        logRow.ColumnSpacing(12);
        auto sizePanel = StackPanel();
        sizePanel.Children().Append(MakeLabel(L"最大サイズ (MB)"));
        logSize = NumberBox();
        logSize.Minimum(1);
        logSize.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
        logSize.Value(static_cast<double>(editingConfig.maxLogSizeMb));
        sizePanel.Children().Append(logSize);
        auto filesPanel = StackPanel();
        filesPanel.Children().Append(MakeLabel(L"保持数"));
        logFiles = NumberBox();
        logFiles.Minimum(1);
        logFiles.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
        logFiles.Value(static_cast<double>(editingConfig.maxLogFiles));
        filesPanel.Children().Append(logFiles);
        Grid::SetColumn(sizePanel, 1);
        Grid::SetColumn(filesPanel, 2);
        logRow.Children().Append(levelPanel);
        logRow.Children().Append(sizePanel);
        logRow.Children().Append(filesPanel);
        form.Children().Append(logRow);

        scroll.Content(form);
        root.Children().Append(scroll);

        auto actions = StackPanel();
        settingsActions = actions;
        actions.Orientation(Orientation::Horizontal);
        actions.HorizontalAlignment(HorizontalAlignment::Right);
        actions.Spacing(8);
        actions.Margin({0, 16, 0, 0});
        auto cancel = MakeButton(L"キャンセル");
        cancel.Click([this](auto&&, auto&&) { settingsWindow.Close(); });
        auto save = MakeButton(L"保存");
        save.Click([this](auto&&, auto&&) { SaveSettings(); });
        actions.Children().Append(cancel);
        actions.Children().Append(save);
        Grid::SetRow(actions, 1);
        root.Children().Append(actions);

        auto window = settingsWindow;
        window.Content(root);
        window.Closed([this](auto&&, auto&&) {
            if (observedWindow) {
                observedWindow.Changed(placementChanged);
                observedWindow = nullptr;
                placementChanged = {};
            }
            placementReady = false;
            try {
                if (lastPlacement && callbacks.savePlacement) callbacks.savePlacement(*lastPlacement);
            }
            catch (const std::exception& ex) {
                spdlog::warn("Failed to save window placement: {}", ex.what());
            }
            catch (...) { spdlog::warn("Failed to save window placement."); }
            settingsWindow = nullptr;
        });
        const auto appWindow = window.AppWindow();
        observedWindow = appWindow;
        placementChanged = appWindow.Changed([this](const AppWindow& sender, auto&&) {
            try { CapturePlacement(sender); }
            catch (...) { spdlog::warn("Failed to capture window placement."); }
        });
        root.Loaded([this, saved](auto&&, auto&&) {
            if (placementReady) return;
            try {
                if (saved) RestorePlacement(*saved);
                else ApplyDefaultPlacement();
            }
            catch (const hresult_error& ex) {
                spdlog::error("Window placement error: 0x{:08x}", static_cast<unsigned int>(ex.code()));
            }
        });
        // Seed a useful location before Loaded can measure the control templates.
        POINT pointer{};
        GetCursorPos(&pointer);
        const auto workArea = WorkArea(MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST));
        window.AppWindow().MoveAndResize({workArea.left + (workArea.right - workArea.left - 660) / 2,
            workArea.top + (workArea.bottom - workArea.top - 760) / 2, 660, 760});
        RefreshTopicList(editingConfig.topics.empty() ? -1 : 0);
        window.Activate();
    }

    void CloseAll()
    {
        if (settingsWindow) settingsWindow.Close();
    }

    bool IsSettingsReady() const
    {
        if (!settingsWindow) return false;
        const auto root = settingsWindow.Content().try_as<FrameworkElement>();
        const bool selectionReady = editingConfig.topics.empty() ||
            (topicList.SelectedIndex() == 0 && topicName.Text() == to_hstring(editingConfig.topics[0].topic));
        return root && root.IsLoaded() && root.ActualWidth() > 0 && root.ActualHeight() > 0 &&
               mqttUrl.IsLoaded() && mqttUrl.Template() && mqttPassword.Template() &&
               topicList.Template() && logLevel.Template() && logSize.Template() &&
               logFiles.Template() && Media::VisualTreeHelper::GetChildrenCount(mqttUrl) > 0 &&
               selectionReady;
    }

    bool IsDefaultLayoutReady() const
    {
        if (!IsSettingsReady() || !placementReady) return false;
        const auto window = settingsWindow.AppWindow();
        const auto position = window.Position();
        const auto size = window.Size();
        const auto levelPoint = logLevel.TransformToVisual(settingsForm).TransformPoint({0, 0});
        const auto sizePoint = logSize.TransformToVisual(settingsForm).TransformPoint({0, 0});
        const auto filesPoint = logFiles.TransformToVisual(settingsForm).TransformPoint({0, 0});
        spdlog::info("UI layout: {}x{}, scrollable height {:.1f}", size.Width, size.Height, settingsScroll.ScrollableHeight());
        return std::abs(2 * position.X + size.Width - defaultWorkArea.left - defaultWorkArea.right) <= 2 &&
               std::abs(2 * position.Y + size.Height - defaultWorkArea.top - defaultWorkArea.bottom) <= 2 &&
               (settingsScroll.ScrollableHeight() <= 1 || size.Height == defaultWorkArea.bottom - defaultWorkArea.top) &&
               std::abs(levelPoint.Y - sizePoint.Y) <= 1 && std::abs(sizePoint.Y - filesPoint.Y) <= 1 &&
               levelPoint.X < sizePoint.X && sizePoint.X < filesPoint.X;
    }
};

ModernUi::ModernUi(ModernUiCallbacks callbacks) : impl_(std::make_unique<Impl>(std::move(callbacks))) {}
ModernUi::~ModernUi() = default;

void ModernUi::ShowSettings(bool resetPlacement) { impl_->ShowSettings(resetPlacement); }
bool ModernUi::IsSettingsReady() const { return impl_->IsSettingsReady(); }
bool ModernUi::IsDefaultLayoutReady() const { return impl_->IsDefaultLayoutReady(); }
HWND ModernUi::SettingsHandle() const { return GetWindowHandle(impl_->settingsWindow); }
void ModernUi::CloseAll() { impl_->CloseAll(); }
