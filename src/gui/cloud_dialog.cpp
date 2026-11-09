#include "cloud_dialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <atomic>

#include <nlohmann/json.hpp>

#include "common.h"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/backends.hpp"
#include "vaultik/repo/gdrive_backend.hpp"
#include "vaultik/repo/mega_backend.hpp"
#include "vaultik/repo/onedrive_backend.hpp"
#include "vaultik/repo/pcloud_backend.hpp"

using namespace vaultik;

namespace {

// Index order of the provider combo box.
enum Provider { kR2, kB2, kAws, kOther, kPCloud, kOneDrive, kGDrive, kMega };

const char* kEndpointHints[] = {
    "https://<account id>.r2.cloudflarestorage.com",
    "https://s3.<region>.backblazeb2.com",
    "https://s3.<region>.amazonaws.com",
    "https://host:port",
};

Provider provider_of(const QString& endpoint) {
    const QString e = endpoint.toLower();
    if (e.contains(QStringLiteral(".r2.cloudflarestorage.com"))) return kR2;
    if (e.contains(QStringLiteral(".backblazeb2.com"))) return kB2;
    if (e.contains(QStringLiteral(".amazonaws.com"))) return kAws;
    return kOther;
}

Provider provider_of(CloudKind kind) {
    switch (kind) {
        case CloudKind::PCloud: return kPCloud;
        case CloudKind::OneDrive: return kOneDrive;
        case CloudKind::GoogleDrive: return kGDrive;
        case CloudKind::Mega: return kMega;
        default: return kR2;
    }
}

const char* app_key(CloudKind kind) {
    switch (kind) {
        case CloudKind::PCloud: return "pcloud";
        case CloudKind::OneDrive: return "onedrive";
        case CloudKind::GoogleDrive: return "gdrive";
        default: return "";
    }
}

// The app registrations entered for the drives, kept for the next sign-in (sealed like passwords).
std::string apps_path() {
    return os::join(jobs::config_dir(), "cloud-apps.dat");
}

nlohmann::json load_apps() {
    try {
        Bytes plain = os::unprotect_secret(os::read_file(apps_path(), 64 * 1024));
        nlohmann::json j = nlohmann::json::parse(plain.begin(), plain.end());
        secure_wipe(plain.data(), plain.size());
        if (j.is_object()) return j;
    } catch (const std::exception&) {
    }
    return nlohmann::json::object();
}

void store_apps(const nlohmann::json& j) {
    const std::string text = j.dump();
    Bytes sealed = os::protect_secret(ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    os::write_file_atomic(apps_path(), sealed, 0600);
}

// Runs `work` on a worker thread behind a progress dialog with Cancel. Returns the error text
// (empty on success); `code_needed` is set when MEGA asked for a two-factor code.
QString run_signin_busy(QWidget* parent, const QString& text,
                 const std::function<void(const std::function<bool()>&)>& work, bool* code_needed = nullptr) {
    std::atomic<bool> cancel{false};
    QString error;
    bool needs_code = false;
    QThread* thread = QThread::create([&] {
        try {
            work([&] { return cancel.load(); });
        } catch (const MegaCodeNeeded&) {
            needs_code = true;
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
    });
    QProgressDialog dialog(text, QObject::tr("Cancel"), 0, 0, parent);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(0);
    QEventLoop loop;
    QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    QObject::connect(&dialog, &QProgressDialog::canceled, &dialog, [&] { cancel = true; });
    thread->start();
    dialog.show();
    loop.exec();
    thread->wait();
    delete thread;
    if (code_needed != nullptr) *code_needed = needs_code;
    if (cancel && error.isEmpty() && !needs_code) error = QObject::tr("Cancelled.");
    return error;
}

}  // namespace

CloudCredentials CloudTarget::credentials() const {
    return {u8(key_id), u8(secret), u8(region)};
}

void CloudTarget::wipe() {
    secret.fill(QChar(0));
    secret.clear();
}

CloudDialog::CloudDialog(const CloudTarget& initial, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Cloud storage"));
    resize(600, 0);

    provider_ = new QComboBox;
    provider_->addItems({tr("Cloudflare R2"), tr("Backblaze B2"), tr("Amazon S3"),
                         tr("Other S3-compatible storage (MinIO, Wasabi, IDrive e2, ...)"), tr("pCloud"),
                         tr("OneDrive"), tr("Google Drive"), tr("MEGA")});
    endpoint_ = new QLineEdit;
    bucket_ = new QLineEdit;
    bucket_->setPlaceholderText(tr("an existing bucket"));
    folder_ = new QLineEdit;
    folder_->setPlaceholderText(tr("optional, e.g. my-pc"));
    key_id_ = new QLineEdit;
    secret_ = new QLineEdit;
    secret_->setEchoMode(QLineEdit::Password);
    region_ = new QLineEdit;
    region_->setPlaceholderText(tr("automatic"));

    drive_folder_ = new QLineEdit;
    drive_folder_->setPlaceholderText(tr("e.g. Backups/my-pc"));
    app_id_ = new QLineEdit;
    app_secret_ = new QLineEdit;
    app_secret_->setEchoMode(QLineEdit::Password);
    sign_in_ = new QPushButton(tr("Sign in..."));
    account_ = new QLabel;
    auto* sign_row = new QWidget;
    auto* sl = new QHBoxLayout(sign_row);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->addWidget(sign_in_);
    sl->addWidget(account_, 1);
    connect(sign_in_, &QPushButton::clicked, this, &CloudDialog::sign_in);

    hint_ = new QLabel;
    hint_->setWordWrap(true);
    hint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hint_->setForegroundRole(QPalette::PlaceholderText);

    form_ = new QFormLayout;
    form_->addRow(tr("Provider:"), provider_);
    form_->addRow(tr("Endpoint:"), endpoint_);
    form_->addRow(tr("Bucket:"), bucket_);
    form_->addRow(tr("Folder in the bucket:"), folder_);
    form_->addRow(tr("Access key ID:"), key_id_);
    form_->addRow(tr("Secret access key:"), secret_);
    form_->addRow(tr("Region:"), region_);
    form_->addRow(tr("Folder:"), drive_folder_);
    form_->addRow(tr("App client ID:"), app_id_);
    form_->addRow(tr("App client secret:"), app_secret_);
    form_->addRow(tr("Account:"), sign_row);
    form_->addRow(hint_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* test = buttons->addButton(tr("Test connection"), QDialogButtonBox::ActionRole);
    connect(test, &QPushButton::clicked, this, &CloudDialog::test_connection);
    connect(buttons, &QDialogButtonBox::accepted, this, &CloudDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form_);
    layout->addWidget(buttons);

    const CloudKind kind = cloud_kind(u8(initial.location));
    if (kind == CloudKind::S3) {
        stored_secret_ = initial.secret;
        key_id_->setText(initial.key_id);
        region_->setText(initial.region);
        try {
            const S3Location l = parse_s3_location(u8(initial.location));
            endpoint_->setText(qs(l.endpoint));
            bucket_->setText(qs(l.bucket));
            folder_->setText(qs(l.folder));
            provider_->setCurrentIndex(provider_of(qs(l.endpoint)));
        } catch (const Error&) {
        }
        if (!stored_secret_.isEmpty()) secret_->setPlaceholderText(tr("stored (leave empty to keep it)"));
    } else if (kind != CloudKind::None) {
        provider_->setCurrentIndex(provider_of(kind));
        try {
            drive_folder_->setText(qs(cloud_folder(u8(initial.location))));
        } catch (const Error&) {
        }
        drive_account_ = initial.key_id;
        drive_secret_ = initial.secret;
        if (!drive_secret_.isEmpty()) signed_kind_ = kind;
    }
    connect(provider_, &QComboBox::currentIndexChanged, this, &CloudDialog::provider_changed);
    provider_changed(provider_->currentIndex());
}

CloudDialog::~CloudDialog() {
    stored_secret_.fill(QChar(0));
    drive_secret_.fill(QChar(0));
    secret_->clear();
    app_secret_->clear();
}

bool CloudDialog::is_drive() const {
    return provider_->currentIndex() >= kPCloud;
}

CloudKind CloudDialog::drive_kind() const {
    switch (provider_->currentIndex()) {
        case kPCloud: return CloudKind::PCloud;
        case kOneDrive: return CloudKind::OneDrive;
        case kGDrive: return CloudKind::GoogleDrive;
        case kMega: return CloudKind::Mega;
        default: return CloudKind::None;
    }
}

void CloudDialog::load_app() {
    const char* key = app_key(drive_kind());
    app_id_->clear();
    app_secret_->clear();
    if (*key == '\0') return;
    const nlohmann::json apps = load_apps();
    if (!apps.contains(key)) return;
    app_id_->setText(qs(apps[key].value("client_id", std::string())));
    app_secret_->setText(qs(apps[key].value("client_secret", std::string())));
}

void CloudDialog::save_app() {
    const char* key = app_key(drive_kind());
    if (*key == '\0') return;
    try {
        nlohmann::json apps = load_apps();
        apps[key] = {{"client_id", u8(app_id_->text().trimmed())}, {"client_secret", u8(app_secret_->text().trimmed())}};
        store_apps(apps);
    } catch (const std::exception&) {
        // only a convenience for the next sign-in
    }
}

void CloudDialog::show_account() {
    if (signed_kind_ == drive_kind() && !drive_secret_.isEmpty()) {
        account_->setText(tr("Signed in as %1").arg(drive_account_));
        sign_in_->setText(tr("Sign in again..."));
    } else {
        account_->setText(tr("Not signed in"));
        sign_in_->setText(tr("Sign in..."));
    }
}

void CloudDialog::provider_changed(int index) {
    const bool drive = index >= kPCloud;
    for (QWidget* w : {static_cast<QWidget*>(endpoint_), static_cast<QWidget*>(bucket_), static_cast<QWidget*>(folder_),
                       static_cast<QWidget*>(key_id_), static_cast<QWidget*>(secret_), static_cast<QWidget*>(region_)})
        form_->setRowVisible(w, !drive);
    form_->setRowVisible(drive_folder_, drive);
    form_->setRowVisible(sign_in_->parentWidget(), drive);
    const bool oauth = index == kPCloud || index == kOneDrive || index == kGDrive;
    form_->setRowVisible(app_id_, oauth);
    form_->setRowVisible(app_secret_, index == kPCloud || index == kGDrive);
    if (!drive) endpoint_->setPlaceholderText(QString::fromLatin1(kEndpointHints[index]));
    if (drive) {
        load_app();
        show_account();
    }

    switch (index) {
        case kR2:
            hint_->setText(tr("In the Cloudflare dashboard: R2, then \"Manage API tokens\" for a token with "
                              "Object Read & Write on the bucket. Its S3 endpoint, access key ID and secret are "
                              "shown once when it is created."));
            break;
        case kB2:
            hint_->setText(tr("In Backblaze: Application Keys, then \"Add a New Application Key\" with read and "
                              "write access to the bucket. The endpoint is shown on the bucket's page."));
            break;
        case kAws:
            hint_->setText(tr("An IAM user's access key with s3:ListBucket on the bucket and s3:GetObject, "
                              "s3:PutObject and s3:DeleteObject on its objects."));
            break;
        case kOther:
            hint_->setText(tr("Any storage that speaks the S3 API with path-style addresses and Signature "
                              "Version 4. Use http:// only on your own network."));
            break;
        case kPCloud:
            hint_->setText(tr("pCloud needs a free app registration once: at docs.pcloud.com, \"My applications\", "
                              "create an app and add the redirect URI http://localhost:53682/ . Enter its client "
                              "ID and client secret here, then sign in. The folder is from the top of your pCloud."));
            break;
        case kOneDrive:
            hint_->setText(tr("OneDrive needs a free app registration once: in portal.azure.com, \"App "
                              "registrations\", New registration, accounts \"in any organizational directory and "
                              "personal Microsoft accounts\", platform \"Mobile and desktop applications\" with the "
                              "redirect URI http://localhost, and \"Allow public client flows\" on. Enter its "
                              "Application (client) ID. Vaultik can only use its own folder, OneDrive\\Apps\\<app "
                              "name>; the folder here is inside it."));
            break;
        case kGDrive:
            hint_->setText(tr("Google Drive needs a free app registration once: in console.cloud.google.com, a "
                              "project with the Google Drive API enabled, the OAuth consent screen set to External "
                              "and published (otherwise the sign-in lapses after 7 days), and an OAuth client ID of "
                              "type \"Desktop app\". Enter its client ID and client secret. Vaultik only sees the "
                              "folders it creates; the folder is from the top of My Drive."));
            break;
        case kMega:
            hint_->setText(tr("Sign in with your MEGA email and password (and the two-factor code if you use "
                              "one). Vaultik keeps only a MEGA session, not the password; ending all sessions in "
                              "MEGA's settings signs it out. The folder is from the top of your Cloud Drive."));
            break;
        default:
            hint_->clear();
    }
}

void CloudDialog::sign_in() {
    const CloudKind kind = drive_kind();
    cloud::SignIn result;
    QString error;
    if (kind == CloudKind::Mega) {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Sign in to MEGA"));
        auto* email = new QLineEdit(drive_account_);
        auto* password = new QLineEdit;
        password->setEchoMode(QLineEdit::Password);
        auto* code = new QLineEdit;
        code->setPlaceholderText(tr("six digits from your authenticator app"));
        auto* form = new QFormLayout;
        form->addRow(tr("Email:"), email);
        form->addRow(tr("Password:"), password);
        form->addRow(tr("Two-factor code:"), code);
        form->setRowVisible(code, false);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        auto* l = new QVBoxLayout(&dialog);
        l->addLayout(form);
        l->addWidget(buttons);
        for (;;) {
            if (dialog.exec() != QDialog::Accepted) {
                password->clear();
                return;
            }
            const std::string e = u8(email->text().trimmed());
            std::string p = u8(password->text());
            const std::string c = u8(code->text().trimmed());
            bool needs_code = false;
            error = run_signin_busy(
                this, tr("Signing in to MEGA..."),
                [&](const std::function<bool()>&) { result = MegaBackend::sign_in(e, p, c); }, &needs_code);
            secure_wipe(p.data(), p.size());
            if (needs_code) {
                form->setRowVisible(code, true);
                code->setFocus();
                QMessageBox::information(this, windowTitle(), tr("Enter the two-factor code for this MEGA account."));
                continue;
            }
            break;
        }
    } else {
        cloud::OAuthApp app{u8(app_id_->text().trimmed()), u8(app_secret_->text().trimmed())};
        const QString text = tr("Finish signing in in your web browser, then come back here.");
        error = run_signin_busy(this, text, [&](const std::function<bool()>& cancelled) {
            if (kind == CloudKind::PCloud) result = PCloudBackend::sign_in(app, cancelled);
            else if (kind == CloudKind::OneDrive) result = OneDriveBackend::sign_in(app, cancelled);
            else result = GoogleDriveBackend::sign_in(app, cancelled);
        });
        secure_wipe(app.client_secret.data(), app.client_secret.size());
    }
    if (!error.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    save_app();
    drive_secret_.fill(QChar(0));
    drive_account_ = qs(result.account);
    drive_secret_ = qs(result.secret);
    secure_wipe(result.secret.data(), result.secret.size());
    signed_kind_ = kind;
    show_account();
}

QString CloudDialog::build_location(QString* error) const {
    if (is_drive()) {
        const QString location = qs(cloud_prefix(drive_kind())) + drive_folder_->text().trimmed();
        try {
            cloud_folder(u8(location));
        } catch (const Error& e) {
            *error = QString::fromUtf8(e.what());
            return {};
        }
        return location;
    }
    QString endpoint = endpoint_->text().trimmed();
    while (endpoint.endsWith('/')) endpoint.chop(1);
    if (endpoint.isEmpty()) {
        *error = tr("Enter the endpoint.");
        return {};
    }
    if (!endpoint.contains(QStringLiteral("://"))) endpoint.prepend(QStringLiteral("https://"));
    const QString bucket = bucket_->text().trimmed();
    QString folder = folder_->text().trimmed().replace('\\', '/');
    while (folder.startsWith('/')) folder.remove(0, 1);
    while (folder.endsWith('/')) folder.chop(1);
    if (bucket.isEmpty()) {
        *error = tr("Enter the bucket.");
        return {};
    }
    if (bucket.contains('/')) {
        *error = tr("The bucket is a name without \"/\"; put the rest in \"Folder in the bucket\".");
        return {};
    }
    const QString location = QStringLiteral("s3:") + endpoint + '/' + bucket + (folder.isEmpty() ? QString() : '/' + folder);
    try {
        parse_s3_location(u8(location));
    } catch (const Error& e) {
        *error = QString::fromUtf8(e.what());
        return {};
    }
    return location;
}

CloudTarget CloudDialog::target() const {
    CloudTarget t;
    QString ignored;
    t.location = build_location(&ignored);
    if (is_drive()) {
        if (signed_kind_ == drive_kind()) {
            t.key_id = drive_account_;
            t.secret = drive_secret_;
        }
        return t;
    }
    t.key_id = key_id_->text().trimmed();
    t.secret = secret_->text().isEmpty() ? stored_secret_ : secret_->text();
    t.region = region_->text().trimmed();
    return t;
}

void CloudDialog::accept() {
    QString error;
    if (build_location(&error).isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    if (is_drive()) {
        if (signed_kind_ != drive_kind() || drive_secret_.isEmpty()) {
            QMessageBox::warning(this, windowTitle(), tr("Sign in first."));
            return;
        }
    } else if (key_id_->text().trimmed().isEmpty() || (secret_->text().isEmpty() && stored_secret_.isEmpty())) {
        QMessageBox::warning(this, windowTitle(), tr("Enter the access key ID and the secret access key."));
        return;
    }
    QDialog::accept();
}

void CloudDialog::test_connection() {
    QString error;
    CloudTarget t = target();
    if (t.location.isEmpty()) {
        build_location(&error);
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    if (t.secret.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), is_drive() ? tr("Sign in first.") : tr("Enter the secret access key."));
        return;
    }
    const std::string location = u8(t.location);
    CloudCredentials keys = t.credentials();
    t.wipe();
    // OneDrive hands out a new sign-in token on use: keep the newest.
    std::string renewed;
    keys.on_secret_changed = [&renewed](const std::string& s) { renewed = s; };
    bool has_repo = false;
    error = run_signin_busy(this, tr("Connecting..."), [&](const std::function<bool()>&) {
        const std::unique_ptr<Backend> backend = open_backend(location, &keys);
        has_repo = backend->exists("config");
        backend->list("locks");  // needs listing rights too
    });
    if (!renewed.empty() && is_drive()) {
        drive_secret_ = qs(renewed);
        secure_wipe(renewed.data(), renewed.size());
    }
    if (!error.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    QMessageBox::information(this, windowTitle(),
                             has_repo ? tr("Connected. This location holds a Vaultik backup; enter its password in the job.")
                                      : tr("Connected. The location is empty: saving the job creates a new backup there."));
}
