// Copyright (C) 2026  ScanTailor Advanced contributors
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_PDFIMPORTER_H_
#define SCANTAILOR_CORE_PDFIMPORTER_H_

#include <QString>
#include <QStringList>
#include <functional>

/**
 * Extracts the pages of a scanned PDF as image files, keeping the original pixels
 * and the original resolution (dpi), so that the output can be rebuilt identical.
 *
 * Each page is handled in the following order:
 *  - an embedded JPEG is copied as is (no recompression), with the dpi written in the JFIF header;
 *  - an embedded CCITT G4 bitmap is wrapped in a TIFF (no recompression);
 *  - an embedded raw/Flate image (gray, RGB, 1-bit, indexed) is saved as PNG with the dpi set;
 *  - everything else (JBIG2, JPEG2000, vector pages...) is rendered with QtPdf at the dpi
 *    estimated from the other pages (or 300).
 */
class PdfImporter {
 public:
  struct Result {
    QStringList files;      // One image file per page, in page order.
    int dpi = 0;            // Median resolution of the extracted pages (0 if unknown).
    bool allBilevel = false;  // True if every page is a 1-bit image.
    QString error;          // Non-empty if nothing could be imported.
  };

  /** Returns true to go on, false to cancel. Arguments: current page, total pages. */
  using ProgressCallback = std::function<bool(int, int)>;

  static bool isPdf(const QString& path);

  /** Pages go to outDir (created if missing), named <prefix>0001.jpg|tif|png. */
  static Result import(const QString& pdfPath,
                       const QString& outDir,
                       const QString& prefix,
                       const ProgressCallback& progress = nullptr);
};

#endif
