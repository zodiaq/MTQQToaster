#include <windows.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <cstdio>

int main()
{
    using namespace winrt;
    using namespace Windows::UI::Xaml;
    init_apartment(apartment_type::single_threaded);
    int result = 0;
    const char* stage = "initialize";
    Hosting::WindowsXamlManager manager{nullptr};
    Hosting::DesktopWindowXamlSource source{nullptr};
    HWND parent = nullptr;
    try {
        manager = Hosting::WindowsXamlManager::InitializeForCurrentThread();
        stage = "create host";
        parent = CreateWindowExW(0, L"STATIC", L"XAML probe", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check_pointer(parent);
        for (int i = 0; i < 3; ++i) {
            source = Hosting::DesktopWindowXamlSource();
            check_hresult(source.as<IDesktopWindowXamlSourceNative>()->AttachToWindow(parent));
            stage = "create controls";
            Controls::StackPanel panel;
            Controls::TextBox text;
            text.Text(L"日本語 / MQTT");
            panel.Children().Append(text);
            panel.Children().Append(Controls::PasswordBox());
            panel.Children().Append(Controls::ComboBox());
            panel.Children().Append(Controls::ListView());
            panel.Children().Append(Controls::Button());
            source.Content(panel);
            panel.UpdateLayout();
            check_bool(text.Text() == L"日本語 / MQTT");
            source.Content(nullptr);
            source.Close();
            source = nullptr;
        }
        std::puts("PASS: OS XAML controls created and closed three times.");
    }
    catch (const hresult_error& error) {
        std::printf("FAIL at %s: 0x%08X %s\n", stage, static_cast<unsigned>(error.code()),
            to_string(error.message()).c_str());
        result = 1;
    }
    if (source) source.Close();
    if (parent) DestroyWindow(parent);
    if (manager) manager.Close();
    source = nullptr;
    manager = nullptr;
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    uninit_apartment();
    return result;
}
