#pragma once

#include <QDialog>

#include <string>

#include "vaultik/jobs/report.hpp"

class QComboBox;
class QTextBrowser;

// The monthly report (see jobs/report.hpp) of a month chosen from the last year, shown as a page,
// with a way to save it as a web page or plain text.
class ReportDialog : public QDialog {
    Q_OBJECT

public:
    // `month` "YYYY-MM"; empty: the month before this one.
    ReportDialog(const vaultik::jobs::JobStore& store, const std::string& month, QWidget* parent = nullptr);

private:
    void show_month();
    void save_as();

    const vaultik::jobs::JobStore& store_;
    QComboBox* month_ = nullptr;
    QTextBrowser* view_ = nullptr;
    vaultik::jobs::MonthlyReport report_;
};
