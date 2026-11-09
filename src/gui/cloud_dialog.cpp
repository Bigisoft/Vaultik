#include "cloud_dialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "common.h"
#include "vaultik/repo/s3_backend.hpp"

using namespace vaultik;

namespace {

// Index order of the provider combo box.
enum Provider { kR2, kB2, kAws, kOther };

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

}  // namespace

CloudCredentials CloudTarget::credentials() const {
    return {u8(key_id), u8(secret), u8(region)};
}

void CloudTarget::wipe() {
    secret.fill(QChar(0));
    secret.clear();
}

CloudDialog::CloudDialog(const CloudTarget& initial, QWidget* parent)
    : QDialog(parent), stored_secret_(initial.secret) {
    setWindowTitle(tr("Cloud storage"));
    resize(560, 0);

    provider_ = new QComboBox;
    provider_->addItems({tr("Cloudflare R2"), tr("Backblaze B2"), tr("Amazon S3"),
                         tr("Other S3-compatible storage (MinIO, Wasabi, ...)")});
    endpoint_ = new QLineEdit;
    bucket_ = new QLineEdit;
    bucket_->setPlaceholderText(tr("an existing bucket"));
    folder_ = new QLineEdit;
    folder_->setPlaceholderText(tr("optional, e.g. my-pc"));
    key_id_ = new QLineEdit;
    secret_ = new QLineEdit;
    secret_->setEchoMode(QLineEdit::Password);
    if (!stored_secret_.isEmpty()) secret_->setPlaceholderText(tr("stored (leave empty to keep it)"));
    region_ = new QLineEdit;
    region_->setPlaceholderText(tr("automatic"));
    hint_ = new QLabel;
    hint_->setWordWrap(true);
    hint_->setForegroundRole(QPalette::PlaceholderText);

    auto* form = new QFormLayout;
    form->addRow(tr("Provider:"), provider_);
    form->addRow(tr("Endpoint:"), endpoint_);
    form->addRow(tr("Bucket:"), bucket_);
    form->addRow(tr("Folder in the bucket:"), folder_);
    form->addRow(tr("Access key ID:"), key_id_);
    form->addRow(tr("Secret access key:"), secret_);
    form->addRow(tr("Region:"), region_);
    form->addRow(hint_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* test = buttons->addButton(tr("Test connection"), QDialogButtonBox::ActionRole);
    connect(test, &QPushButton::clicked, this, &CloudDialog::test_connection);
    connect(buttons, &QDialogButtonBox::accepted, this, &CloudDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(provider_, &QComboBox::currentIndexChanged, this, &CloudDialog::provider_changed);

    key_id_->setText(initial.key_id);
    region_->setText(initial.region);
    if (!initial.location.isEmpty()) {
        try {
            const S3Location l = parse_s3_location(u8(initial.location));
            endpoint_->setText(qs(l.endpoint));
            bucket_->setText(qs(l.bucket));
            folder_->setText(qs(l.folder));
            provider_->setCurrentIndex(provider_of(qs(l.endpoint)));
        } catch (const Error&) {
        }
    }
    provider_changed(provider_->currentIndex());
}

CloudDialog::~CloudDialog() {
    stored_secret_.fill(QChar(0));
    secret_->clear();
}

void CloudDialog::provider_changed(int index) {
    endpoint_->setPlaceholderText(QString::fromLatin1(kEndpointHints[index]));
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
        default:
            hint_->setText(tr("Any storage that speaks the S3 API with path-style addresses and Signature "
                              "Version 4. Use http:// only on your own network."));
    }
}

QString CloudDialog::build_location(QString* error) const {
    QString endpoint = endpoint_->text().trimmed();
    while (endpoint.endsWith('/')) endpoint.chop(1);
    if (endpoint.isEmpty()) {
        *error = tr("Enter the endpoint.");
        return {};
    }
    if (!endpoint.contains(QStringLiteral("://"))) endpoint.prepend(QStringLiteral("https://"));
    QString bucket = bucket_->text().trimmed();
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
    if (key_id_->text().trimmed().isEmpty() || (secret_->text().isEmpty() && stored_secret_.isEmpty())) {
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
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString text;
    bool ok = false;
    try {
        S3Backend backend(u8(t.location), t.credentials());
        const bool has_repo = backend.exists("config");
        backend.list("locks");  // needs list rights on the bucket too
        ok = true;
        text = has_repo ? tr("Connected. This location holds a Vaultik backup; enter its password in the job.")
                        : tr("Connected. The location is empty: saving the job creates a new backup there.");
    } catch (const std::exception& e) {
        text = QString::fromUtf8(e.what());
    }
    t.wipe();
    QApplication::restoreOverrideCursor();
    if (ok) {
        QMessageBox::information(this, windowTitle(), text);
    } else {
        QMessageBox::warning(this, windowTitle(), text);
    }
}
