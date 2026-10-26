// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_APP_PROJECTCREATIONCONTEXT_H_
#define SCANTAILOR_APP_PROJECTCREATIONCONTEXT_H_

#include <QObject>
#include <QPointer>
#include <QString>
#include <Qt>
#include <vector>

#include "ImageFileInfo.h"
#include "NonCopyable.h"

class ProjectFilesDialog;
class FixDpiDialog;
class QWidget;

class ProjectCreationContext : public QObject {
  Q_OBJECT
  DECLARE_NON_COPYABLE(ProjectCreationContext)

 public:
  explicit ProjectCreationContext(QWidget* parent);

  ~ProjectCreationContext() override;

  const std::vector<ImageFileInfo>& files() const { return m_files; }

  const QString& outDir() const { return m_outDir; }

  Qt::LayoutDirection layoutDirection() const { return m_layoutDirection; }

  /** Resolution of the imported PDF pages, or 0 if the project has no PDF. */
  int pdfDpi() const { return m_pdfDpi; }

  bool pdfIsBilevel() const { return m_pdfIsBilevel; }

 signals:

  void done(ProjectCreationContext* context);

 private slots:

  void projectFilesSubmitted();

  void projectFilesDialogDestroyed();

  void fixedDpiSubmitted();

  void fixDpiDialogDestroyed();

 private:
  void showProjectFilesDialog();

  void showFixDpiDialog();

  int m_pdfDpi = 0;
  bool m_pdfIsBilevel = false;
  QPointer<ProjectFilesDialog> m_projectFilesDialog;
  QPointer<FixDpiDialog> m_fixDpiDialog;
  QString m_outDir;
  std::vector<ImageFileInfo> m_files;
  Qt::LayoutDirection m_layoutDirection;
  QWidget* m_parent;
};


#endif  // ifndef SCANTAILOR_APP_PROJECTCREATIONCONTEXT_H_
