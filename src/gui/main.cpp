// vaultik-gui: the Qt 6 Widgets frontend. It manages saved jobs and their schedules, shows
// progress and history, and browses snapshots for restores. The backup work itself runs in the
// vaultik engine process.

#include <QApplication>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <windows.h>

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

}  // namespace

int main(int argc, char** argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    enable_leak_report();
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Vaultik"));
    QApplication::setOrganizationName(QStringLiteral("Vaultik"));
    QApplication::setApplicationVersion(QStringLiteral("0.4.0"));
    apply_theme(app);

    MainWindow window;
    window.show();
    return QApplication::exec();
}
