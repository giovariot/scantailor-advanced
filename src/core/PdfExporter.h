// Copyright (C) 2026  ScanTailor Advanced contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_PDFEXPORTER_H_
#define SCANTAILOR_CORE_PDFEXPORTER_H_

#include <QString>
#include <QStringList>
#include <functional>

/**
 * Writes the processed pages as one multi-page PDF.  Each page is an image placed at its
 * own resolution, so the physical page size is pixels / dpi.
 *
 *  - 1-bit pages are stored as CCITT Group 4 (lossless, very small);
 *  - gray and color pages are stored as JPEG (quality > 0) or Flate (quality == 0, lossless).
 */
class PdfExporter {
 public:
  struct Options {
    int jpegQuality = 85;  // 1..100, or 0 for lossless Flate.
    int fallbackDpi = 300;  // Used for images without resolution information.
  };

  /** Returns true to go on, false to cancel. Arguments: current page, total pages. */
  using ProgressCallback = std::function<bool(int, int)>;

  /** Returns an empty string on success, or an error message. */
  static QString write(const QStringList& imageFiles,
                       const QString& pdfPath,
                       const Options& options,
                       const ProgressCallback& progress = nullptr);
};

#endif
