// vaultik-gui: the Qt 6 Widgets frontend. It manages saved jobs and their schedules, shows
// progress and history, and browses snapshots for restores. The backup work itself runs in the
// vaultik engine process.
//
//   vaultik-gui          opens the window (or brings an already running Vaultik to the front)
//   vaultik-gui --tray   starts behind the notification area icon (used at sign-in)

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QSettings>
#include <QStringList>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW

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
#include "translator.h"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/error.hpp"
#include "vaultik/version.hpp"

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
// One Vaultik per signed-in user and settings folder: a second start asks the first to show its
// window, through a registered window message, and ends. A copy with other settings (a portable
// one, or VAULTIK_CONFIG_DIR) is a separate instance with its own names.
std::wstring instance_name(const wchar_t* base) {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a of the settings folder, any case
    for (const char c : vaultik::os::fold_case(vaultik::jobs::config_dir())) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    wchar_t suffix[24];
    swprintf(suffix, 24, L".%016llx", static_cast<unsigned long long>(h));
    return std::wstring(base) + suffix;
}

// The window of the running instance (marked with its property), or null.
HWND instance_window(const wchar_t* prop_base) {
    struct Find {
        std::wstring prop;
        HWND found = nullptr;
    } find{instance_name(prop_base)};
    EnumWindows(
        [](HWND hwnd, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Find*>(p);
            if (GetPropW(hwnd, f->prop.c_str()) == nullptr) return TRUE;
            f->found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&find));
    return find.found;
}

// Whether "Start Vaultik as administrator" is on (Settings). Never for a portable copy: its
// program folder is not protected, so the task could not exist anyway.
bool wants_elevation() {
    if (vaultik::jobs::is_portable()) return false;
    const QSettings s(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("Vaultik"), QStringLiteral("Vaultik"));
    return s.value(QStringLiteral("startup/admin"), false).toBool();
}

// Starts the elevated copy through its task (no approval prompt) and, unless this start is the
// hidden one from sign-in, shows its window once it is there. False when the task could not be
// started, so this copy carries on itself.
bool start_elevated(bool tray) {
    std::string ignored;
    try {
        if (vaultik::os::run_command({"schtasks.exe", "/Run", "/TN", vaultik::jobs::elevated_gui_task()}, &ignored) != 0)
            return false;
    } catch (const vaultik::Error&) {
        return false;
    }
    if (tray) return true;
    const UINT show = RegisterWindowMessageW(instance_name(L"Vaultik.ShowWindow").c_str());
    for (int i = 0; i < 100; ++i) {  // up to 20 seconds
        if (const HWND w = instance_window(L"Vaultik.Window")) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(w, show, 0, 0);
            break;
        }
        Sleep(200);
    }
    return true;
}

// With administrator rights, settings from the environment (which programs without those rights
// can change, through the account's environment variables) must not load code: Qt finds its
// plugins only next to the program.
void clean_environment() {
    for (const wchar_t* name : {L"QT_PLUGIN_PATH", L"QT_QPA_PLATFORM_PLUGIN_PATH", L"QT_QPA_PLATFORM",
                                L"QT_QPA_PLATFORMTHEME", L"QT_QPA_GENERIC_PLUGINS", L"QT_STYLE_OVERRIDE",
                                L"QT_OPENGL", L"QT_OPENGL_DLL", L"QT_ANGLE_PLATFORM", L"QSG_RHI_BACKEND",
                                L"QT_LOGGING_CONF", L"QT_DEBUG_PLUGINS", L"QT_ACCESSIBILITY", L"QT_IM_MODULE",
                                L"QML_IMPORT_PATH", L"QML2_IMPORT_PATH"}) {
        SetEnvironmentVariableW(name, nullptr);
        _wputenv_s(name, L"");
    }
}

// The path after "--history", read from the Unicode command line (argv holds the ANSI code page,
// which cannot spell every file name).
QString history_argument() {
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args == nullptr) return {};
    QString path;
    for (int i = 1; i + 1 < count; ++i)
        if (std::wstring_view(args[i]) == L"--history") path = QString::fromWCharArray(args[i + 1]);
    LocalFree(args);
    return path;
}

class ShowRequests : public QAbstractNativeEventFilter {
public:
    explicit ShowRequests(MainWindow& window) : window_(window), message_(RegisterWindowMessageW(instance_name(L"Vaultik.ShowWindow").c_str())) {}

    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override {
        if (type != "windows_generic_MSG") return false;
        const MSG* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_COPYDATA) {
            // A path from a second start ("--history"), sent to the window marked as this instance's.
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(msg->lParam);
            if (data == nullptr || data->dwData != kHistoryData || GetPropW(msg->hwnd, instance_name(kWindowProp).c_str()) == nullptr)
                return false;
            const QString path = QString::fromWCharArray(static_cast<const wchar_t*>(data->lpData),
                                                         static_cast<qsizetype>(data->cbData / sizeof(wchar_t)));
            QMetaObject::invokeMethod(&window_, [w = &window_, path] { w->show_history_for(path); }, Qt::QueuedConnection);
            if (result != nullptr) *result = TRUE;
            return true;
        }
        if (message_ == 0 || msg->message != message_) return false;
        window_.bring_to_front();
        return true;
    }

    static constexpr ULONG_PTR kHistoryData = 0x56484953;  // "VHIS"
    static constexpr const wchar_t* kWindowProp = L"Vaultik.Window";

private:
    MainWindow& window_;
    UINT message_;
};
#else
QString history_argument() {
    const QStringList args = QCoreApplication::arguments();
    const qsizetype at = args.indexOf(QStringLiteral("--history"));
    return at >= 0 && at + 1 < args.size() ? args[at + 1] : QString();
}
#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    enable_leak_report();
#endif
    bool tray = false;
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--tray") tray = true;
    const QString history = history_argument();  // from File Explorer's right-click menu

#ifdef Q_OS_WIN
    const bool elevated = vaultik::os::process_elevated();
    if (elevated) clean_environment();
    // Released by Windows when this process ends.
    HANDLE instance = CreateMutexW(nullptr, FALSE, instance_name(L"Local\\Vaultik.Gui").c_str());
    if (GetLastError() != ERROR_ALREADY_EXISTS && !elevated && history.isEmpty() && wants_elevation()) {
        // "Start Vaultik as administrator": the elevated copy takes over; this one ends.
        CloseHandle(instance);
        if (start_elevated(tray)) return 0;
        instance = CreateMutexW(nullptr, FALSE, instance_name(L"Local\\Vaultik.Gui").c_str());
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        AllowSetForegroundWindow(ASFW_ANY);
        if (!history.isEmpty()) {
            // Hand the path to the running window, marked with this instance's property.
            struct Find {
                std::wstring prop;
                HWND found = nullptr;
            } find{instance_name(ShowRequests::kWindowProp)};
            EnumWindows(
                [](HWND hwnd, LPARAM p) -> BOOL {
                    auto* f = reinterpret_cast<Find*>(p);
                    if (GetPropW(hwnd, f->prop.c_str()) == nullptr) return TRUE;
                    f->found = hwnd;
                    return FALSE;
                },
                reinterpret_cast<LPARAM>(&find));
            if (find.found != nullptr) {
                const std::wstring text = history.toStdWString();
                COPYDATASTRUCT data{ShowRequests::kHistoryData, static_cast<DWORD>(text.size() * sizeof(wchar_t)),
                                    const_cast<wchar_t*>(text.data())};
                DWORD_PTR ignored = 0;
                SendMessageTimeoutW(find.found, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000,
                                    &ignored);
            }
        } else if (!tray) {
            // Already running: show it (unless this start came from sign-in, which needs nothing).
            PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(instance_name(L"Vaultik.ShowWindow").c_str()), 0, 0);
        }
        return 0;
    }
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Vaultik"));
    QApplication::setOrganizationName(QStringLiteral("Vaultik"));
    QApplication::setApplicationVersion(QStringLiteral(VAULTIK_VERSION));
    if (vaultik::jobs::is_portable()) {
        // A portable copy leaves nothing on the computer: its own settings go next to the jobs.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QString::fromUtf8(vaultik::jobs::config_dir()));
    }
    install_translation(app);
    apply_theme(app);

    MainWindow window(tray && history.isEmpty());
#ifdef Q_OS_WIN
    ShowRequests requests(window);
    app.installNativeEventFilter(&requests);
    // Marks this instance's window, so a second start can hand it a path.
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    SetPropW(hwnd, instance_name(ShowRequests::kWindowProp).c_str(), reinterpret_cast<HANDLE>(1));
    if (elevated) {
        // Windows drops messages from programs without administrator rights to this window; a
        // second start (from the Start menu or File Explorer) must still reach it.
        ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(hwnd, RegisterWindowMessageW(instance_name(L"Vaultik.ShowWindow").c_str()),
                                    MSGFLT_ALLOW, nullptr);
    }
#endif
    if (!tray || !history.isEmpty()) window.show();
    if (!history.isEmpty())
        QMetaObject::invokeMethod(&window, [&window, history] { window.show_history_for(history); }, Qt::QueuedConnection);
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
