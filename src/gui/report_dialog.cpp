#include "report_dialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "common.h"
#include "vaultik/platform/os.hpp"

using namespace vaultik;

namespace {

// The report's fixed phrases through the frontend's translation.
std::string translated(const std::string& phrase) {
    return u8(QCoreApplication::translate("ReportDialog", phrase.c_str()));
}

}  // namespace

ReportDialog::ReportDialog(const jobs::JobStore& store, const std::string& month, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(tr("Monthly report"));
    month_ = new QComboBox;
    const std::string current = jobs::month_of(os::now());
    std::string m = current;
    for (int i = 0; i < 13; ++i) {
        month_->addItem(i == 0 ? tr("%1 (so far)").arg(qs(m)) : qs(m), qs(m));
        m = jobs::previous_month(m);
    }
    const std::string wanted = month.empty() ? jobs::previous_month(current) : month;
    const int index = month_->findData(qs(wanted));
    month_->setCurrentIndex(index >= 0 ? index : 1);

    view_ = new QTextBrowser;
    view_->setOpenLinks(false);
    view_->setOpenExternalLinks(false);
    // The page has its own light colours; keep them readable in a dark theme too.
    view_->setStyleSheet(QStringLiteral("QTextBrowser { background: #ffffff; color: #1b1b1b; }"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QPushButton* save = buttons->addButton(tr("Save as..."), QDialogButtonBox::ActionRole);
    connect(save, &QPushButton::clicked, this, &ReportDialog::save_as);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* form = new QFormLayout;
    form->addRow(tr("Month:"), month_);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(view_, 1);
    layout->addWidget(buttons);
    connect(month_, &QComboBox::currentIndexChanged, this, &ReportDialog::show_month);
    resize(720, 640);
    show_month();
}

void ReportDialog::show_month() {
    const std::string month = u8(month_->currentData().toString());
    try {
        report_ = jobs::report_for(store_, month);
        view_->setHtml(qs(jobs::render_html(report_, translated)));
    } catch (const std::exception& e) {
        view_->setPlainText(qs(e.what()));
    }
}

void ReportDialog::save_as() {
    const QString web = tr("Web page (*.html)");
    const QString text = tr("Text file (*.txt)");
    QString filter = web;
    const QString name = tr("Vaultik report %1").arg(qs(report_.month));
    const QString path = QFileDialog::getSaveFileName(this, tr("Save the report"), QDir::homePath() + "/" + name + ".html",
                                                      web + ";;" + text, &filter);
    if (path.isEmpty()) return;
    const bool as_text = filter == text || path.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive);
    const std::string content =
        as_text ? jobs::render_text(report_, translated) : jobs::render_html(report_, translated);
    try {
        os::write_file_atomic(os::absolute_path(u8(QDir::toNativeSeparators(path))),
                              ByteSpan(reinterpret_cast<const std::uint8_t*>(content.data()), content.size()));
    } catch (const std::exception& e) {
        plain_warning(this, windowTitle(), qs(e.what()));
    }
}
