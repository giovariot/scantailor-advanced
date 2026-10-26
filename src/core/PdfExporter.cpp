// Copyright (C) 2026  ScanTailor Advanced contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "PdfExporter.h"

#include <tiffio.h>
#include <zlib.h>

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QTemporaryFile>
#include <vector>

namespace {
struct Encoded {
  QByteArray data;
  QByteArray dict;  // Image dictionary entries after /Width and /Height.
};

QByteArray num(const double v) {
  return QByteArray::number(v, 'f', 4);
}

bool isBlack(const QRgb c) {
  return qGray(c) < 128;
}

// Bilevel images are Format_Mono, or Indexed8 with at most two colors.
bool toMono(QImage& img) {
  if (img.format() == QImage::Format_Mono || img.format() == QImage::Format_MonoLSB) {
    img = img.convertToFormat(QImage::Format_Mono);
    return true;
  }
  if (img.format() == QImage::Format_Indexed8 && img.colorCount() <= 2) {
    img = img.convertToFormat(QImage::Format_Mono);
    return true;
  }
  return false;
}

bool isGrayImage(const QImage& img) {
  if (img.format() == QImage::Format_Grayscale8 || img.format() == QImage::Format_Grayscale16) {
    return true;
  }
  if (img.format() == QImage::Format_Indexed8) {
    for (const QRgb c : img.colorTable()) {
      if (qRed(c) != qGreen(c) || qGreen(c) != qBlue(c)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

bool encodeG4(const QImage& mono, QByteArray& out) {
  const int w = mono.width();
  const int h = mono.height();
  const bool zeroIsBlack = mono.colorCount() >= 1 && isBlack(mono.color(0));
  const bool oneIsBlack = mono.colorCount() >= 2 && isBlack(mono.color(1));

  QTemporaryFile tmp;
  if (!tmp.open()) {
    return false;
  }
  const QByteArray path = QFile::encodeName(tmp.fileName());
  tmp.close();

  TIFF* tif = TIFFOpen(path.constData(), "w");
  if (!tif) {
    return false;
  }
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(w));
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(h));
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 1);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISWHITE);  // 1 bits are black.
  TIFFSetField(tif, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, static_cast<uint32_t>(h));

  // Pixel index 1 is black if oneIsBlack; otherwise index 0 is black and bits must be flipped.
  const bool flip = zeroIsBlack && !oneIsBlack;
  std::vector<uint8_t> row((w + 7) / 8);
  bool ok = true;
  for (int y = 0; y < h && ok; ++y) {
    const uchar* line = mono.constScanLine(y);
    for (size_t i = 0; i < row.size(); ++i) {
      row[i] = flip ? static_cast<uint8_t>(~line[i]) : line[i];
    }
    ok = TIFFWriteScanline(tif, row.data(), static_cast<uint32_t>(y), 0) == 1;
  }
  TIFFClose(tif);
  if (!ok) {
    return false;
  }

  tif = TIFFOpen(path.constData(), "r");
  if (!tif) {
    return false;
  }
  const tmsize_t size = TIFFRawStripSize(tif, 0);
  out.resize(static_cast<int>(size));
  const tmsize_t got = TIFFReadRawStrip(tif, 0, out.data(), size);
  TIFFClose(tif);
  out.resize(static_cast<int>(got));
  return got > 0;
}

bool encodePage(const QImage& source, const PdfExporter::Options& opt, Encoded& enc) {
  QImage img = source;
  if (toMono(img)) {
    if (!encodeG4(img, enc.data)) {
      return false;
    }
    enc.dict = "/ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /CCITTFaxDecode "
               "/DecodeParms << /K -1 /Columns " + QByteArray::number(img.width()) + " /Rows "
               + QByteArray::number(img.height()) + " >>";
    return true;
  }

  const bool gray = isGrayImage(img);
  img = img.convertToFormat(gray ? QImage::Format_Grayscale8 : QImage::Format_RGB888);
  const QByteArray cs = gray ? "/DeviceGray" : "/DeviceRGB";

  if (opt.jpegQuality > 0) {
    QBuffer buf(&enc.data);
    buf.open(QIODevice::WriteOnly);
    QImageWriter writer(&buf, "jpeg");
    writer.setQuality(opt.jpegQuality);
    if (!writer.write(img)) {
      return false;
    }
    enc.dict = "/ColorSpace " + cs + " /BitsPerComponent 8 /Filter /DCTDecode";
    return true;
  }

  const int rowBytes = img.width() * (gray ? 1 : 3);
  QByteArray raw;
  raw.reserve(rowBytes * img.height());
  for (int y = 0; y < img.height(); ++y) {
    raw.append(reinterpret_cast<const char*>(img.constScanLine(y)), rowBytes);
  }
  uLongf destLen = compressBound(static_cast<uLong>(raw.size()));
  enc.data.resize(static_cast<int>(destLen));
  if (compress2(reinterpret_cast<Bytef*>(enc.data.data()), &destLen, reinterpret_cast<const Bytef*>(raw.constData()),
                static_cast<uLong>(raw.size()), Z_BEST_COMPRESSION)
      != Z_OK) {
    return false;
  }
  enc.data.resize(static_cast<int>(destLen));
  enc.dict = "/ColorSpace " + cs + " /BitsPerComponent 8 /Filter /FlateDecode";
  return true;
}
}  // namespace

QString PdfExporter::write(const QStringList& imageFiles,
                           const QString& pdfPath,
                           const Options& options,
                           const ProgressCallback& progress) {
  if (imageFiles.isEmpty()) {
    return QObject::tr("There are no pages to export.");
  }
  QFile file(pdfPath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return QObject::tr("Unable to write %1: %2").arg(pdfPath, file.errorString());
  }

  std::vector<qint64> offsets;  // offsets[i] is the position of object i + 1.
  const auto beginObject = [&](const int id) {
    if (static_cast<int>(offsets.size()) < id) {
      offsets.resize(static_cast<size_t>(id));
    }
    offsets[static_cast<size_t>(id - 1)] = file.pos();
    file.write(QByteArray::number(id) + " 0 obj\n");
  };

  const int total = imageFiles.size();
  file.write("%PDF-1.5\n%\xE2\xE3\xCF\xD3\n");

  QByteArray kids;
  for (int i = 0; i < total; ++i) {
    kids += QByteArray::number(3 + 3 * i) + " 0 R ";
  }
  beginObject(1);
  file.write("<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
  beginObject(2);
  file.write("<< /Type /Pages /Count " + QByteArray::number(total) + " /Kids [ " + kids + "] >>\nendobj\n");

  for (int i = 0; i < total; ++i) {
    if (progress && !progress(i, total)) {
      file.close();
      QFile::remove(pdfPath);
      return QObject::tr("Canceled.");
    }

    QImageReader reader(imageFiles[i]);
    reader.setAutoTransform(false);
    const QImage img = reader.read();
    if (img.isNull()) {
      file.close();
      QFile::remove(pdfPath);
      return QObject::tr("Unable to read %1: %2").arg(imageFiles[i], reader.errorString());
    }
    const double dpiX = img.dotsPerMeterX() > 0 ? img.dotsPerMeterX() * 0.0254 : options.fallbackDpi;
    const double dpiY = img.dotsPerMeterY() > 0 ? img.dotsPerMeterY() * 0.0254 : options.fallbackDpi;
    const double ptW = img.width() * 72.0 / dpiX;
    const double ptH = img.height() * 72.0 / dpiY;

    Encoded enc;
    if (!encodePage(img, options, enc)) {
      file.close();
      QFile::remove(pdfPath);
      return QObject::tr("Unable to encode %1.").arg(imageFiles[i]);
    }

    const int pageId = 3 + 3 * i;
    const QByteArray content = "q " + num(ptW) + " 0 0 " + num(ptH) + " 0 0 cm /Im0 Do Q\n";

    beginObject(pageId);
    file.write("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + num(ptW) + " " + num(ptH) + "] /Resources << /XObject << /Im0 "
               + QByteArray::number(pageId + 2) + " 0 R >> >> /Contents " + QByteArray::number(pageId + 1)
               + " 0 R >>\nendobj\n");
    beginObject(pageId + 1);
    file.write("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream\nendobj\n");
    beginObject(pageId + 2);
    file.write("<< /Type /XObject /Subtype /Image /Width " + QByteArray::number(img.width()) + " /Height "
               + QByteArray::number(img.height()) + " " + enc.dict + " /Length " + QByteArray::number(enc.data.size())
               + " >>\nstream\n");
    file.write(enc.data);
    file.write("\nendstream\nendobj\n");
  }

  const qint64 xrefPos = file.pos();
  file.write("xref\n0 " + QByteArray::number(static_cast<int>(offsets.size()) + 1) + "\n0000000000 65535 f \n");
  for (const qint64 off : offsets) {
    file.write(QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n");
  }
  file.write("trailer\n<< /Size " + QByteArray::number(static_cast<int>(offsets.size()) + 1) + " /Root 1 0 R >>\nstartxref\n"
             + QByteArray::number(xrefPos) + "\n%%EOF\n");
  if (progress) {
    progress(total, total);
  }
  if (!file.flush()) {
    return QObject::tr("Unable to write %1: %2").arg(pdfPath, file.errorString());
  }
  return QString();
}
