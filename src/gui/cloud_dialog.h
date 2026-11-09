#pragma once

#include <QDialog>

#include "vaultik/repo/cloud.hpp"

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

// A cloud storage destination as entered in CloudDialog.
struct CloudTarget {
    QString location;  // "s3:https://<endpoint>/<bucket>[/<folder>]", "pcloud:<folder>", ...
    QString key_id;    // S3 access key ID, or the signed-in account of a drive
    QString secret;    // S3 secret key, or a drive's sign-in token (JSON)
    QString region;    // S3 only; empty: automatic
    int lock_days = 0;  // S3 only: Object Lock on uploads for this many days; 0 for none
    bool lock_compliance = false;

    vaultik::CloudCredentials credentials() const;
    void wipe();
};

// Asks for cloud storage: S3-compatible storage (provider, endpoint, bucket, folder, access key),
// or a drive (pCloud, OneDrive, Google Drive, MEGA: folder and a sign-in). Has a connection test.
class CloudDialog : public QDialog {
    Q_OBJECT

public:
    // A secret in `initial` is not shown: the field stays empty, and empty keeps that secret.
    explicit CloudDialog(const CloudTarget& initial, QWidget* parent = nullptr);
    ~CloudDialog() override;

    // The destination entered, with the stored secret when the field was left empty.
    CloudTarget target() const;
    void accept() override;

private:
    void provider_changed(int index);
    void test_connection();
    void sign_in();
    bool is_drive() const;
    vaultik::CloudKind drive_kind() const;
    // The location from the fields, or empty with the problem in `error`.
    QString build_location(QString* error) const;
    void load_app();
    void save_app();
    void show_account();

    QString stored_secret_;
    QComboBox* provider_ = nullptr;
    QFormLayout* form_ = nullptr;
    // S3
    QLineEdit* endpoint_ = nullptr;
    QLineEdit* bucket_ = nullptr;
    QLineEdit* folder_ = nullptr;
    QLineEdit* key_id_ = nullptr;
    QLineEdit* secret_ = nullptr;
    QLineEdit* region_ = nullptr;
    QSpinBox* lock_days_ = nullptr;
    QComboBox* lock_mode_ = nullptr;
    // Drives
    QLineEdit* drive_folder_ = nullptr;
    QLineEdit* app_id_ = nullptr;
    QLineEdit* app_secret_ = nullptr;
    QPushButton* sign_in_ = nullptr;
    QLabel* account_ = nullptr;
    QString drive_account_;
    QString drive_secret_;
    vaultik::CloudKind signed_kind_ = vaultik::CloudKind::None;

    QLabel* hint_ = nullptr;
};
