#pragma once

#include <QDialog>

#include "vaultik/repo/s3_backend.hpp"

class QComboBox;
class QLabel;
class QLineEdit;

// A cloud storage destination as entered in CloudDialog.
struct CloudTarget {
    QString location;  // "s3:https://<endpoint>/<bucket>[/<folder>]"
    QString key_id;
    QString secret;
    QString region;  // empty: automatic

    vaultik::CloudCredentials credentials() const;
    void wipe();
};

// Asks for S3-compatible cloud storage: provider, endpoint, bucket, folder and access key, with a
// connection test.
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
    // The location from the fields, or empty with the problem in `error`.
    QString build_location(QString* error) const;

    QString stored_secret_;
    QComboBox* provider_ = nullptr;
    QLineEdit* endpoint_ = nullptr;
    QLineEdit* bucket_ = nullptr;
    QLineEdit* folder_ = nullptr;
    QLineEdit* key_id_ = nullptr;
    QLineEdit* secret_ = nullptr;
    QLineEdit* region_ = nullptr;
    QLabel* hint_ = nullptr;
};
