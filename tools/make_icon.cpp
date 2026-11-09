// Development tool: renders src/gui/app_icon.svg into a Windows icon with the usual sizes, each
// stored as PNG (Windows Vista and later read those). Run after changing the SVG:
//   vaultik-make-icon src/gui/app_icon.svg src/gui/vaultik.ico

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc != 3) {
        std::fprintf(stderr, "usage: vaultik-make-icon <in.svg> <out.ico>\n");
        return 2;
    }
    QSvgRenderer svg(QString::fromLocal8Bit(argv[1]));
    if (!svg.isValid()) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    std::vector<QByteArray> images;
    for (const int s : sizes) {
        QImage image(s, s, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        svg.render(&p, QRectF(0, 0, s, s));
        p.end();
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        images.push_back(png);
    }

    // ICONDIR, one ICONDIRENTRY per image, then the images.
    QByteArray ico;
    const auto u16 = [&ico](int v) {
        ico.append(static_cast<char>(v & 0xFF));
        ico.append(static_cast<char>((v >> 8) & 0xFF));
    };
    const auto u32 = [&](qint64 v) {
        u16(static_cast<int>(v & 0xFFFF));
        u16(static_cast<int>((v >> 16) & 0xFFFF));
    };
    u16(0);
    u16(1);  // icon
    u16(static_cast<int>(images.size()));
    qint64 offset = 6 + 16 * static_cast<qint64>(images.size());
    for (std::size_t i = 0; i < images.size(); ++i) {
        const int s = sizes[i];
        ico.append(static_cast<char>(s >= 256 ? 0 : s));  // 0 means 256
        ico.append(static_cast<char>(s >= 256 ? 0 : s));
        ico.append('\0');  // no palette
        ico.append('\0');
        u16(1);   // planes
        u16(32);  // bits per pixel
        u32(images[i].size());
        u32(offset);
        offset += images[i].size();
    }
    for (const QByteArray& png : images) ico.append(png);

    QFile out(QString::fromLocal8Bit(argv[2]));
    if (!out.open(QIODevice::WriteOnly) || out.write(ico) != ico.size()) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    return 0;
}
