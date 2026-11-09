#include "hello.h"

#include <QCoreApplication>
#include <QThread>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <UserConsentVerifierInterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.UI.h>
#endif

namespace hello {

#ifdef Q_OS_WIN

namespace {

using winrt::Windows::Foundation::AsyncStatus;
using namespace winrt::Windows::Security::Credentials::UI;

// The GUI thread is a single-threaded apartment, which must not block on an async operation:
// keep the application running until it completes.
template <class Op>
void wait(const Op& op) {
    while (op.Status() == AsyncStatus::Started) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        QThread::msleep(10);
    }
}

}  // namespace

bool available() {
    try {
        const auto op = UserConsentVerifier::CheckAvailabilityAsync();
        wait(op);
        return op.Status() == AsyncStatus::Completed && op.GetResults() == UserConsentVerifierAvailability::Available;
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

Result verify(QWidget* window, const QString& message) {
    try {
        // A desktop app asks for its own window, so the prompt appears in front of it.
        const auto interop = winrt::get_activation_factory<UserConsentVerifier, IUserConsentVerifierInterop>();
        winrt::Windows::Foundation::IAsyncOperation<UserConsentVerificationResult> op;
        const winrt::hstring text(message.toStdWString());
        winrt::check_hresult(interop->RequestVerificationForWindowAsync(
            reinterpret_cast<HWND>(window->window()->winId()), static_cast<HSTRING>(winrt::get_abi(text)),
            winrt::guid_of<decltype(op)>(), winrt::put_abi(op)));
        wait(op);
        if (op.Status() != AsyncStatus::Completed) return Result::Failed;
        switch (op.GetResults()) {
            case UserConsentVerificationResult::Verified: return Result::Verified;
            case UserConsentVerificationResult::Canceled: return Result::Cancelled;
            case UserConsentVerificationResult::DeviceNotPresent:
            case UserConsentVerificationResult::NotConfiguredForUser:
            case UserConsentVerificationResult::DisabledByPolicy: return Result::Unavailable;
            default: return Result::Failed;
        }
    } catch (const winrt::hresult_error&) {
        return Result::Failed;
    }
}

#else

bool available() {
    return false;
}

Result verify(QWidget*, const QString&) {
    return Result::Unavailable;
}

#endif

}  // namespace hello
