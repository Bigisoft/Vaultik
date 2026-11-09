// vaultik-gui: the Qt 6 Widgets frontend. It manages saved jobs and their schedules, shows
// progress and history, and browses snapshots for restores. The backup work itself runs in the
// vaultik engine process.
//
//   vaultik-gui          opens the window (or brings an already running Vaultik to the front)
//   vaultik-gui --tray   starts behind the notification area icon (used at sign-in)

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QStringList>

#ifdef Q_OS_WIN
#include <windows.h>

// From uiautomationcoreapi.h (UIAutomationCore.dll), whose header needs the full COM headers.
extern "C" HRESULT WINAPI UiaDisconnectAllProviders();
#endif

#if defined(_MSC_VER) && defined(_DEBUG)

#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#endif

#include "main_window.h"
#include "theme.h"

namespace {

#if defined(_MSC_VER) && defined(_DEBUG)
// With VAULTIK_LEAK_CHECK=<file>, a debug build writes every heap block still allocated at exit
// (after static objects are destroyed) to that file.
void enable_leak_report() {
    wchar_t* path = nullptr;
    std::size_t len = 0;
    if (_wdupenv_s(&path, &len, L"VAULTIK_LEAK_CHECK") != 0 || path == nullptr) return;
    // Left open on purpose: the report is written at the very end of the process.
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::free(path);
    if (file == INVALID_HANDLE_VALUE) return;
    // Where the program is loaded, so addresses in the report can be matched to symbols.
    char base[64];
    const int n = std::snprintf(base, sizeof base, "image base %p\n", static_cast<void*>(GetModuleHandleW(nullptr)));
    DWORD written = 0;
    WriteFile(file, base, static_cast<DWORD>(n), &written, nullptr);
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, file);
}
#endif

#ifdef Q_OS_WIN
// One Vaultik per signed-in user: a second start asks the first to show its window, through a
// registered window message, and ends.
const wchar_t* kShowMessage = L"Vaultik.ShowWindow";

class ShowRequests : public QAbstractNativeEventFilter {
public:
    explicit ShowRequests(MainWindow& window) : window_(window), message_(RegisterWindowMessageW(kShowMessage)) {}

    bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override {
        if (type != "windows_generic_MSG") return false;
        const MSG* msg = static_cast<const MSG*>(message);
        if (message_ == 0 || msg->message != message_) return false;
        window_.bring_to_front();
        return true;
    }

private:
    MainWindow& window_;
    UINT message_;
};
#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    enable_leak_report();
#endif
    bool tray = false;
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--tray") tray = true;

#ifdef Q_OS_WIN
    // Released by Windows when this process ends.
    CreateMutexW(nullptr, FALSE, L"Local\\Vaultik.Gui");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: show it (unless this start came from sign-in, which needs nothing).
        if (!tray) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(kShowMessage), 0, 0);
        }
        return 0;
    }
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Vaultik"));
    QApplication::setOrganizationName(QStringLiteral("Vaultik"));
    QApplication::setApplicationVersion(QStringLiteral("0.4.0"));
    apply_theme(app);

    MainWindow window(tray);
#ifdef Q_OS_WIN
    ShowRequests requests(window);
    app.installNativeEventFilter(&requests);
#endif
    if (!tray) window.show();
    const int code = QApplication::exec();
#ifdef Q_OS_WIN
    app.removeNativeEventFilter(&requests);
    // Screen readers and other UI Automation clients hold references to the accessibility objects
    // of our controls; Microsoft asks providers to release them all before shutting down, so they
    // are freed with the window rather than left behind.
    UiaDisconnectAllProviders();
#endif
    return code;
}
