#include "kit_print.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QMarginsF>
#include <QMessageBox>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTextDocument>
#include <QUrl>

#include "common.h"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/recovery_kit.hpp"

using namespace vaultik;

namespace {

// The kit's fixed phrases through the frontend's translation.
std::string translated(const std::string& phrase) {
    return u8(QCoreApplication::translate("RecoveryKit", phrase.c_str()));
}

// The QR code as a sharp image, `scale` pixels per module, with its light border.
QImage qr_image(const qr::Code& code, int scale) {
    const int side = (code.size + 8) * scale;
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    for (int y = 0; y < code.size; ++y)
        for (int x = 0; x < code.size; ++x)
            if (code.dark(x, y)) p.fillRect((x + 4) * scale, (y + 4) * scale, scale, scale, Qt::black);
    return image;
}

// One A4 page from the same page the web version is, with the fonts embedded (so every language
// prints) and the QR code as an image.
void write_pdf(QIODevice& device, const jobs::RecoveryKit& kit) {
    QPdfWriter writer(&device);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(16, 14, 16, 14), QPageLayout::Millimeter);
    writer.setTitle(QCoreApplication::translate("RecoveryKit", "Vaultik recovery kit"));
    writer.setCreator(QStringLiteral("Vaultik"));
    QTextDocument doc;
    doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("qr")), qr_image(jobs::kit_qr(kit), 12));
    doc.setHtml(qs(jobs::render_kit_html(kit, translated, "<img src=\"qr\" width=\"180\" height=\"180\">")));
    doc.print(&writer);
}

}  // namespace

void print_recovery_kit(QWidget* parent, const QString& key, const QString& location) {
    const QString title = QObject::tr("Print recovery kit");
    const QString pdf = QObject::tr("PDF document (*.pdf)");
    const QString web = QObject::tr("Web page (*.html)");
    QString filter = pdf;
    const QString path = QFileDialog::getSaveFileName(
        parent, title, QDir::homePath() + "/" + QObject::tr("Vaultik recovery kit") + ".pdf", pdf + ";;" + web, &filter);
    if (path.isEmpty()) return;
    const bool as_web = path.endsWith(QStringLiteral(".html"), Qt::CaseInsensitive) ||
                        path.endsWith(QStringLiteral(".htm"), Qt::CaseInsensitive) ||
                        (filter == web && !path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive));
    jobs::RecoveryKit kit;
    try {
        kit = jobs::make_recovery_kit(u8(key), u8(location));
    } catch (const std::exception& e) {
        plain_warning(parent, title, QString::fromUtf8(e.what()));
        return;
    }
    const bool ok = save_private_file(path, [&](QIODevice& f) {
        if (as_web) {
            const QByteArray html = QByteArray::fromStdString(jobs::render_kit_html(kit, translated));
            return f.write(html) == html.size();
        }
        write_pdf(f, kit);
        return f.size() > 0;
    });
    secure_wipe(kit.key.data(), kit.key.size());
    if (!ok) {
        plain_warning(parent, title, QObject::tr("Cannot write %1.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    QMessageBox box(QMessageBox::Question, title, QString(), QMessageBox::Yes | QMessageBox::No, parent);
    box.setTextFormat(Qt::PlainText);
    box.setText(QObject::tr("The recovery kit is saved in %1.\n\nOpen it now to print it? Once it is printed, delete "
                            "the file: it holds the recovery key.")
                    .arg(QDir::toNativeSeparators(path)));
    if (box.exec() == QMessageBox::Yes) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
