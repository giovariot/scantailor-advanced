// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_PROJECTFILESDIALOG_H_
#define SCANTAILOR_APP_PROJECTFILESDIALOG_H_

#include <QDialog>
#include <QSet>
#include <QString>
#include <memory>
#include <vector>

#include "ImageFileInfo.h"
#include "ui_ProjectFilesDialog.h"

class ProjectFilesDialog : public QDialog, private Ui::ProjectFilesDialog {
  Q_OBJECT
 public:
  explicit ProjectFilesDialog(QWidget* parent = nullptr);

  ~ProjectFilesDialog() override;

  QString inputDirectory() const;

  QString outputDirectory() const;

  std::vector<ImageFileInfo> inProjectFiles() const;

  bool isRtlLayout() const;

  bool isDpiFixingForced() const;

  /** Resolution of the imported PDF pages (0 if no PDF was imported). */
  int importedPdfDpi() const { return m_importedPdfDpi; }

  /** True if every page of the imported PDFs is a 1-bit image. */
  bool importedPdfIsBilevel() const { return m_importedPdfBilevel; }

 private slots:

  static QString sanitizePath(const QString& path);

  void inpDirBrowse();

  void outDirBrowse();

  void inpDirEdited(const QString& text);

  void outDirEdited(const QString& text);

  void addToProject();

  void removeFromProject();

  void onOK();

 private:
  class Item;
  class FileList;
  class SortedFileList;
  class ItemVisualOrdering;

  void setInputDir(const QString& dir, bool autoAddFiles = true);

  /** Replaces the PDF files among the items with the page images extracted from them. */
  void importPdfs(std::vector<Item>& items);

  void setOutputDir(const QString& dir);

  void startLoadingMetadata();

  void timerEvent(QTimerEvent* event) override;

  void finishLoadingMetadata();

  void setupIcons();

  QSet<QString> m_supportedExtensions;
  std::unique_ptr<FileList> m_offProjectFiles;
  std::unique_ptr<SortedFileList> m_offProjectFilesSorted;
  std::unique_ptr<FileList> m_inProjectFiles;
  std::unique_ptr<SortedFileList> m_inProjectFilesSorted;
  int m_loadTimerId;
  bool m_metadataLoadFailed;
  bool m_autoOutDir;
  int m_importedPdfDpi = 0;
  bool m_importedPdfBilevel = false;
};


#endif  // ifndef SCANTAILOR_APP_PROJECTFILESDIALOG_H_
