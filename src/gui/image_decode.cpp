#include "preview.h"

#include <QBuffer>
#include <QImageReader>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
// After <windows.h>.
#include <wincodec.h>
#endif

namespace {

#ifdef Q_OS_WIN

// Releases a COM interface when it goes out of scope.
template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p != nullptr) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// The EXIF orientation (1-8) of the first frame, 1 when unknown.
int exif_orientation(IWICBitmapFrameDecode* frame) {
    Com<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
    for (const wchar_t* query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"/xmp/tiff:Orientation"}) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
            int orientation = 1;
            if (value.vt == VT_UI2) orientation = value.uiVal;
            else if (value.vt == VT_UI4) orientation = static_cast<int>(value.ulVal);
            else if (value.vt == VT_LPWSTR && value.pwszVal != nullptr) orientation = _wtoi(value.pwszVal);
            PropVariantClear(&value);
            if (orientation >= 1 && orientation <= 8) return orientation;
        }
    }
    return 1;
}

WICBitmapTransformOptions transform_for(int orientation) {
    switch (orientation) {
        case 2: return WICBitmapTransformFlipHorizontal;
        case 3: return WICBitmapTransformRotate180;
        case 4: return WICBitmapTransformFlipVertical;
        case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
        case 6: return WICBitmapTransformRotate90;
        case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        case 8: return WICBitmapTransformRotate270;
        default: return WICBitmapTransformRotate0;
    }
}

QString container_name(IWICBitmapDecoder* decoder) {
    GUID guid{};
    if (FAILED(decoder->GetContainerFormat(&guid))) return {};
    if (guid == GUID_ContainerFormatJpeg) return QStringLiteral("JPEG");
    if (guid == GUID_ContainerFormatPng) return QStringLiteral("PNG");
    if (guid == GUID_ContainerFormatGif) return QStringLiteral("GIF");
    if (guid == GUID_ContainerFormatBmp) return QStringLiteral("BMP");
    if (guid == GUID_ContainerFormatTiff) return QStringLiteral("TIFF");
    if (guid == GUID_ContainerFormatIco) return QStringLiteral("ICO");
    if (guid == GUID_ContainerFormatHeif) return QStringLiteral("HEIF");
    if (guid == GUID_ContainerFormatWebp) return QStringLiteral("WebP");
    return QStringLiteral("image");
}

QImage decode_wic(const QByteArray& data, int max_side, QString* format) {
    // This may run on a worker thread; COM is set up for it here and released again.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);
    QImage out;
    {
        Com<IWICImagingFactory> factory;
        Com<IWICStream> stream;
        Com<IWICBitmapDecoder> decoder;
        Com<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(data.constData())),
                                                   static_cast<DWORD>(data.size()))) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame))) {
            if (format != nullptr) *format = container_name(decoder.p);
            IWICBitmapSource* source = frame.p;

            // Scale down large pictures first: decoding at full size could take hundreds of MB.
            UINT w = 0;
            UINT h = 0;
            source->GetSize(&w, &h);
            Com<IWICBitmapScaler> scaler;
            if (max_side > 0 && (w > static_cast<UINT>(max_side) || h > static_cast<UINT>(max_side)) &&
                SUCCEEDED(factory->CreateBitmapScaler(&scaler))) {
                const double s = static_cast<double>(max_side) / std::max(w, h);
                const UINT sw = std::max<UINT>(1, static_cast<UINT>(w * s));
                const UINT sh = std::max<UINT>(1, static_cast<UINT>(h * s));
                if (SUCCEEDED(scaler->Initialize(source, sw, sh, WICBitmapInterpolationModeFant))) source = scaler.p;
            }
            Com<IWICBitmapFlipRotator> rotator;
            const WICBitmapTransformOptions transform = transform_for(exif_orientation(frame.p));
            if (transform != WICBitmapTransformRotate0 && SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator)) &&
                SUCCEEDED(rotator->Initialize(source, transform)))
                source = rotator.p;
            Com<IWICFormatConverter> converter;
            if (SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                                WICBitmapPaletteTypeMedianCut))) {
                converter->GetSize(&w, &h);
                QImage image(static_cast<int>(w), static_cast<int>(h), QImage::Format_ARGB32_Premultiplied);
                if (!image.isNull() &&
                    SUCCEEDED(converter->CopyPixels(nullptr, static_cast<UINT>(image.bytesPerLine()),
                                                    static_cast<UINT>(image.sizeInBytes()), image.bits())))
                    out = std::move(image);
            }
        }
    }
    if (uninit) CoUninitialize();
    return out;
}

#endif

}  // namespace

QImage decode_image(const QByteArray& data, int max_side, QString* format) {
    // Qt's own readers first (PNG, BMP, SVG and the like), with the camera orientation applied.
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    if (reader.canRead()) {
        const QSize size = reader.size();
        if (max_side > 0 && size.isValid() && std::max(size.width(), size.height()) > max_side)
            reader.setScaledSize(size.scaled(max_side, max_side, Qt::KeepAspectRatio));
        QImage image = reader.read();
        if (!image.isNull()) {
            if (format != nullptr) *format = QString::fromLatin1(reader.format()).toUpper();
            return image;
        }
    }
#ifdef Q_OS_WIN
    return decode_wic(data, max_side, format);
#else
    return {};
#endif
}
