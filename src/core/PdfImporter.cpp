// Copyright (C) 2026  ScanTailor Advanced contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "PdfImporter.h"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <tiffio.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <poppler-document.h>
#include <poppler-image.h>
#include <poppler-page-renderer.h>
#include <poppler-page.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {
struct PageImage {
  QString file;
  int dpi = 0;
  bool bilevel = false;
};

int dotsPerMeter(const int dpi) {
  return static_cast<int>(dpi / 0.0254 + 0.5);
}

QStringList filterNames(const QPDFObjectHandle& dict) {
  QStringList names;
  QPDFObjectHandle f = dict.getKey("/Filter");
  if (f.isName()) {
    names << QString::fromStdString(f.getName());
  } else if (f.isArray()) {
    for (int i = 0; i < f.getArrayNItems(); ++i) {
      names << QString::fromStdString(f.getArrayItem(i).getName());
    }
  }
  return names;
}

QPDFObjectHandle decodeParmsFor(const QPDFObjectHandle& dict, const int filterIdx) {
  QPDFObjectHandle p = dict.getKey("/DecodeParms");
  if (p.isArray()) {
    return (filterIdx < p.getArrayNItems()) ? p.getArrayItem(filterIdx) : QPDFObjectHandle::newNull();
  }
  return p;
}

int intKey(const QPDFObjectHandle& dict, const char* key, const int def) {
  QPDFObjectHandle v = dict.getKey(key);
  return v.isInteger() ? static_cast<int>(v.getIntValue()) : def;
}

bool decodeInverted(const QPDFObjectHandle& dict) {
  QPDFObjectHandle d = dict.getKey("/Decode");
  return d.isArray() && d.getArrayNItems() >= 2 && d.getArrayItem(0).isNumber() && d.getArrayItem(0).getNumericValue() > 0.5;
}

// Bytes of a stream whose last filter is an image codec (DCT, CCITT...): the codec itself is left alone.
std::shared_ptr<Buffer> codecBytes(QPDFObjectHandle& stream, const QStringList& filters) {
  if (filters.size() == 1) {
    return stream.getRawStreamData();
  }
  return stream.getStreamData(qpdf_dl_generalized);  // Throws if other filters follow the codec.
}

bool writeFile(const QString& path, const QByteArray& data) {
  QFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

// Sets the JFIF density (dpi) without touching the compressed data.
QByteArray jpegWithDpi(const QByteArray& jpeg, const int dpi) {
  if (jpeg.size() < 4 || static_cast<uchar>(jpeg[0]) != 0xFF || static_cast<uchar>(jpeg[1]) != 0xD8) {
    return jpeg;
  }
  QByteArray out(jpeg);
  const auto put16 = [](QByteArray& a, int pos, int v) {
    a[pos] = static_cast<char>((v >> 8) & 0xFF);
    a[pos + 1] = static_cast<char>(v & 0xFF);
  };
  const bool hasJfif = out.size() > 18 && static_cast<uchar>(out[2]) == 0xFF && static_cast<uchar>(out[3]) == 0xE0
                       && out.mid(6, 5) == QByteArray("JFIF\0", 5);
  if (hasJfif) {
    out[13] = 1;  // units: dots per inch
    put16(out, 14, dpi);
    put16(out, 16, dpi);
    return out;
  }
  QByteArray app0;
  app0.append("\xFF\xE0\x00\x10JFIF\x00\x01\x01\x01\x00\x00\x00\x00\x00\x00", 18);
  put16(app0, 12, dpi);
  put16(app0, 14, dpi);
  out.insert(2, app0);
  return out;
}

bool writeCcittTiff(const QString& path,
                    const QByteArray& data,
                    const int width,
                    const int height,
                    const bool blackIsZero,
                    const int dpi) {
  TIFF* tif = TIFFOpen(QFile::encodeName(path).constData(), "w");
  if (!tif) {
    return false;
  }
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(width));
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(height));
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 1);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, blackIsZero ? PHOTOMETRIC_MINISBLACK : PHOTOMETRIC_MINISWHITE);
  TIFFSetField(tif, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, static_cast<uint32_t>(height));
  TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, static_cast<float>(dpi));
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, static_cast<float>(dpi));
  const bool ok = TIFFWriteRawStrip(tif, 0, const_cast<char*>(data.constData()), data.size()) == data.size();
  TIFFClose(tif);
  return ok;
}

// Copies PDF rows (padded to a byte) into a QImage (padded to 4 bytes).
void copyRows(QImage& img, const uchar* src, const int rowBytes, const int height) {
  for (int y = 0; y < height; ++y) {
    std::copy(src + static_cast<size_t>(y) * rowBytes, src + static_cast<size_t>(y + 1) * rowBytes, img.scanLine(y));
  }
}

// Builds a QImage from raw (already Flate-decoded) PDF image data. Null image if unsupported.
QImage rawToImage(const QPDFObjectHandle& dict, const Buffer& buf) {
  const int w = intKey(dict, "/Width", 0);
  const int h = intKey(dict, "/Height", 0);
  const int bpc = intKey(dict, "/BitsPerComponent", 8);
  if (w <= 0 || h <= 0) {
    return QImage();
  }
  const bool invert = decodeInverted(dict);

  QPDFObjectHandle cs = dict.getKey("/ColorSpace");
  std::string base;
  QPDFObjectHandle indexed;
  if (cs.isArray() && cs.getArrayNItems() > 0) {
    const std::string kind = cs.getArrayItem(0).isName() ? cs.getArrayItem(0).getName() : "";
    if (kind == "/ICCBased" && cs.getArrayNItems() > 1) {
      const int n = intKey(cs.getArrayItem(1).getDict(), "/N", 0);
      base = (n == 1) ? "/DeviceGray" : (n == 3) ? "/DeviceRGB" : "";
    } else if (kind == "/Indexed" && cs.getArrayNItems() >= 4) {
      indexed = cs;
      const QPDFObjectHandle b = cs.getArrayItem(1);
      base = b.isName() ? b.getName() : "";
      if (base == "/CalRGB") {
        base = "/DeviceRGB";
      }
      if (base != "/DeviceRGB" && base != "/DeviceGray") {
        return QImage();
      }
    } else if (kind == "/CalGray") {
      base = "/DeviceGray";
    } else if (kind == "/CalRGB") {
      base = "/DeviceRGB";
    }
  } else if (cs.isName()) {
    base = cs.getName();
    if (base == "/CalGray" || base == "/G") {
      base = "/DeviceGray";
    }
    if (base == "/CalRGB" || base == "/RGB") {
      base = "/DeviceRGB";
    }
  }

  const uchar* data = buf.getBuffer();
  const size_t size = buf.getSize();

  if (indexed.isArray()) {
    if (bpc != 8 && bpc != 1) {
      return QImage();
    }
    const int rowBytes = (w * bpc + 7) / 8;
    if (size < static_cast<size_t>(rowBytes) * h) {
      return QImage();
    }
    QPDFObjectHandle lookup = indexed.getArrayItem(3);
    std::string table = lookup.isString() ? lookup.getStringValue()
                                          : (lookup.isStream() ? std::string(reinterpret_cast<const char*>(lookup.getStreamData()->getBuffer()),
                                                                             lookup.getStreamData()->getSize())
                                                               : std::string());
    const int comps = (base == "/DeviceRGB") ? 3 : 1;
    const int count = std::min<int>(static_cast<int>(table.size() / comps), 1 << bpc);
    QVector<QRgb> colors;
    for (int i = 0; i < count; ++i) {
      const auto t = [&](int k) { return static_cast<uchar>(table[static_cast<size_t>(i) * comps + k]); };
      colors.push_back(comps == 3 ? qRgb(t(0), t(1), t(2)) : qRgb(t(0), t(0), t(0)));
    }
    QImage img(w, h, bpc == 8 ? QImage::Format_Indexed8 : QImage::Format_Mono);
    img.setColorTable(colors);
    copyRows(img, data, rowBytes, h);
    return img;
  }

  if (base == "/DeviceGray") {
    if (bpc == 1) {
      const int rowBytes = (w + 7) / 8;
      if (size < static_cast<size_t>(rowBytes) * h) {
        return QImage();
      }
      QImage img(w, h, QImage::Format_Mono);
      img.setColorTable({invert ? 0xffffffffu : 0xff000000u, invert ? 0xff000000u : 0xffffffffu});
      copyRows(img, data, rowBytes, h);
      return img;
    }
    if (bpc == 8) {
      if (size < static_cast<size_t>(w) * h) {
        return QImage();
      }
      QImage img(w, h, QImage::Format_Grayscale8);
      copyRows(img, data, w, h);
      if (invert) {
        img.invertPixels();
      }
      return img;
    }
  } else if (base == "/DeviceRGB" && bpc == 8) {
    if (size < static_cast<size_t>(w) * h * 3) {
      return QImage();
    }
    QImage img(w, h, QImage::Format_RGB888);
    copyRows(img, data, w * 3, h);
    return img;
  }
  return QImage();
}

QString pagePath(const QString& dir, const QString& prefix, const int n, const char* ext) {
  return QDir(dir).filePath(QString("%1%2.%3").arg(prefix).arg(n, 4, 10, QChar('0')).arg(ext));
}

int medianOf(std::vector<int> v) {
  if (v.empty()) {
    return 0;
  }
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}
}  // namespace

bool PdfImporter::isPdf(const QString& path) {
  return QFileInfo(path).suffix().compare("pdf", Qt::CaseInsensitive) == 0;
}

PdfImporter::Result PdfImporter::import(const QString& pdfPath,
                                        const QString& outDir,
                                        const QString& prefix,
                                        const ProgressCallback& progress) {
  Result result;
  if (!QDir().mkpath(outDir)) {
    result.error = QObject::tr("Unable to create directory %1").arg(outDir);
    return result;
  }

  std::shared_ptr<QPDF> pdf;
  std::vector<QPDFPageObjectHelper> pages;
  try {
    pdf = QPDF::create();
    pdf->processFile(QFile::encodeName(pdfPath).constData());
    pages = QPDFPageDocumentHelper(*pdf).getAllPages();
  } catch (const std::exception& e) {
    result.error = QString::fromUtf8(e.what());
    return result;
  }
  if (pages.empty()) {
    result.error = QObject::tr("The PDF has no pages.");
    return result;
  }

  std::unique_ptr<poppler::document> renderer;  // Opened lazily, only for pages that cannot be extracted.

  std::vector<PageImage> imported;
  const int total = static_cast<int>(pages.size());
  for (int n = 0; n < total; ++n) {
    if (progress && !progress(n, total)) {
      result.error = QObject::tr("Canceled.");
      return result;
    }

    PageImage out;
    QPDFPageObjectHelper& page = pages[static_cast<size_t>(n)];

    double pageW = 0, pageH = 0;
    int bestW = 0, bestH = 0;
    QPDFObjectHandle best;
    try {
      const auto box = page.getCropBox(true).getArrayAsRectangle();
      pageW = std::abs(box.urx - box.llx);
      pageH = std::abs(box.ury - box.lly);
      QPDFObjectHandle rot = page.getObjectHandle().getKey("/Rotate");
      const int rotate = rot.isInteger() ? static_cast<int>(rot.getIntValue()) : 0;
      if (rotate % 180 != 0) {
        std::swap(pageW, pageH);
      }
      for (auto& entry : page.getImages()) {
        QPDFObjectHandle im = entry.second;
        const int iw = intKey(im.getDict(), "/Width", 0);
        const int ih = intKey(im.getDict(), "/Height", 0);
        if (static_cast<long long>(iw) * ih > static_cast<long long>(bestW) * bestH) {
          best = im;
          bestW = iw;
          bestH = ih;
        }
      }
    } catch (const std::exception&) {
      best = QPDFObjectHandle();
    }

    // The image counts as "the scan" only if it fills the page (same aspect ratio).
    int dpi = 0;
    bool coversPage = false;
    if (best.isStream() && pageW > 0 && pageH > 0) {
      const double dpiX = bestW / (pageW / 72.0);
      const double dpiY = bestH / (pageH / 72.0);
      coversPage = std::abs(dpiX - dpiY) / std::max(dpiX, dpiY) < 0.03;
      dpi = static_cast<int>(std::lround((dpiX + dpiY) / 2));
    }

    bool done = false;
    if (coversPage) {
      try {
        const QPDFObjectHandle dict = best.getDict();
        const QStringList filters = filterNames(dict);
        const int pageNo = n + 1;
        if (filters.contains("/DCTDecode")) {
          const auto buf = codecBytes(best, filters);
          out.file = pagePath(outDir, prefix, pageNo, "jpg");
          done = writeFile(out.file, jpegWithDpi(QByteArray(reinterpret_cast<const char*>(buf->getBuffer()),
                                                            static_cast<int>(buf->getSize())),
                                                 dpi));
        } else if (filters.contains("/CCITTFaxDecode")) {
          const QPDFObjectHandle parms = decodeParmsFor(dict, static_cast<int>(filters.indexOf("/CCITTFaxDecode")));
          const bool g4 = parms.isDictionary() && intKey(parms, "/K", 0) < 0;
          const bool aligned = parms.isDictionary() && parms.getKey("/EncodedByteAlign").isBool()
                               && parms.getKey("/EncodedByteAlign").getBoolValue();
          const bool blackIs1 = parms.isDictionary() && parms.getKey("/BlackIs1").isBool()
                                && parms.getKey("/BlackIs1").getBoolValue();
          const int cols = parms.isDictionary() ? intKey(parms, "/Columns", 1728) : 1728;
          if (g4 && !aligned && cols == bestW) {
            const auto buf = codecBytes(best, filters);
            out.file = pagePath(outDir, prefix, pageNo, "tif");
            // Black runs are black pixels; 1 bits are white only if BlackIs1 is true (or /Decode inverts).
            done = writeCcittTiff(out.file,
                                  QByteArray(reinterpret_cast<const char*>(buf->getBuffer()),
                                             static_cast<int>(buf->getSize())),
                                  bestW, bestH, blackIs1 != decodeInverted(dict), dpi);
            out.bilevel = done;
          }
        } else if (!filters.contains("/JBIG2Decode") && !filters.contains("/JPXDecode")
                   && !filters.contains("/CCITTFaxDecode") && !dict.hasKey("/Mask") && !dict.hasKey("/SMask")
                   && !(dict.getKey("/ImageMask").isBool() && dict.getKey("/ImageMask").getBoolValue())) {
          const auto buf = best.getStreamData(qpdf_dl_generalized);
          QImage img = rawToImage(dict, *buf);
          if (!img.isNull()) {
            img.setDotsPerMeterX(dotsPerMeter(dpi));
            img.setDotsPerMeterY(dotsPerMeter(dpi));
            out.file = pagePath(outDir, prefix, pageNo, "png");
            done = img.save(out.file, "PNG");
            out.bilevel = done && img.format() == QImage::Format_Mono;
          }
        }
      } catch (const std::exception& e) {
        qWarning("PDF page %d: %s", n + 1, e.what());
        done = false;
      }
    }

    if (!done) {
      // Fallback: render the page. Same pixel size as the embedded scan when we know it.
      if (!renderer) {
        renderer.reset(poppler::document::load_from_file(QFile::encodeName(pdfPath).constData()));
      }
      std::unique_ptr<poppler::page> rpage(renderer ? renderer->create_page(n) : nullptr);
      if (rpage) {
        const int renderDpi = (dpi >= 72 && dpi <= 1200) ? dpi : 300;
        poppler::page_renderer pr;
        pr.set_image_format(poppler::image::format_rgb24);
        pr.set_render_hint(poppler::page_renderer::antialiasing, true);
        pr.set_render_hint(poppler::page_renderer::text_antialiasing, true);
        const poppler::image rendered = pr.render_page(rpage.get(), renderDpi, renderDpi);
        if (rendered.is_valid()) {
          QImage img(reinterpret_cast<const uchar*>(rendered.const_data()), rendered.width(), rendered.height(),
                     rendered.bytes_per_row(), QImage::Format_RGB888);
          img = img.copy();  // Detach from poppler's buffer.
          const bool bilevelSource = best.isStream() && intKey(best.getDict(), "/BitsPerComponent", 8) == 1;
          if (bilevelSource) {
            img = img.convertToFormat(QImage::Format_Grayscale8).convertToFormat(QImage::Format_Mono, Qt::ThresholdDither);
          }
          img.setDotsPerMeterX(dotsPerMeter(renderDpi));
          img.setDotsPerMeterY(dotsPerMeter(renderDpi));
          out.file = pagePath(outDir, prefix, n + 1, "png");
          out.bilevel = bilevelSource;
          done = img.save(out.file, "PNG");
          dpi = renderDpi;
        }
      }
    }

    if (!done) {
      result.error = QObject::tr("Unable to import page %1.").arg(n + 1);
      return result;
    }
    out.dpi = dpi;
    imported.push_back(out);
  }
  if (progress) {
    progress(total, total);
  }

  std::vector<int> dpis;
  bool allBilevel = true;
  for (const PageImage& p : imported) {
    result.files << p.file;
    if (p.dpi > 0) {
      dpis.push_back(p.dpi);
    }
    allBilevel = allBilevel && p.bilevel;
  }
  result.dpi = medianOf(dpis);
  result.allBilevel = allBilevel;
  return result;
}
