// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <QList>

class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class MinecraftInstance;

class TransferContentDialog : public QDialog {
    Q_OBJECT

   public:
    explicit TransferContentDialog(QWidget* parent = nullptr);

   private slots:
    void updatePreview();
    void updateCopyButton();
    void copySelected();

   private:
    struct Entry {
        QString type;
        QString name;
        QString sourcePath;
        QString destinationPath;
        bool conflict = false;
        QTreeWidgetItem* row = nullptr;
    };

    MinecraftInstance* selectedInstance(QComboBox* combo) const;
    void addEntries(const QString& type, const QString& sourceDir, const QString& destinationDir, bool directories);

    QComboBox* m_source = nullptr;
    QComboBox* m_destination = nullptr;
    QLabel* m_compatibility = nullptr;
    QLabel* m_summary = nullptr;
    QTreeWidget* m_items = nullptr;
    QPushButton* m_copyButton = nullptr;
    QList<Entry> m_entries;
};
