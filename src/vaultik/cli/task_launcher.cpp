// vaultik-task.exe: a windowless (GUI subsystem) launcher for scheduled runs. Task Scheduler
// starts it instead of vaultik.exe so no console window appears; it runs vaultik.exe from the
// same folder with the same arguments, hidden, and returns its exit code.

#include <windows.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring self(MAX_PATH, L'\0');
    DWORD n = 0;
    for (;;) {
        n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
        if (n == 0) return 1;
        if (n < self.size()) break;
        self.resize(self.size() * 2);
    }
    self.resize(n);
    const std::wstring dir = self.substr(0, self.find_last_of(L'\\') + 1);
    const std::wstring exe = dir + L"vaultik.exe";

    // Keep the arguments exactly as received: everything after our own program name.
    const wchar_t* full = GetCommandLineW();
    const wchar_t* args = full;
    if (*args == L'"') {
        ++args;
        while (*args != L'\0' && *args != L'"') ++args;
        if (*args == L'"') ++args;
    } else {
        while (*args != L'\0' && *args != L' ' && *args != L'\t') ++args;
    }
    while (*args == L' ' || *args == L'\t') ++args;

    std::wstring cmdline = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(),
                        &si, &pi))
        return 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
