// SPDX-License-Identifier: GPL-3.0-only
#include "TransferContentDialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFuture>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <utility>

#include "Application.h"
#include "InstanceList.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/WorldList.h"
#include "minecraft/mod/ModFolderModel.h"

namespace {
struct TransferJob {
    QString type;
    QString name;
    QString sourcePath;
    QString destinationPath;
};

struct TransferResult {
    QStringList copied;
    QStringList skipped;
    QStringList failed;
};

QString loaderDescription(const MinecraftInstance* instance)
{
    if (!instance || !instance->getPackProfile())
        return QObject::tr("unknown loader");

    static const QStringList loaders = { "net.fabricmc.fabric-loader", "org.quiltmc.quilt-loader", "net.minecraftforge", "net.neoforged" };
    for (const QString& id : loaders) {
        const QString version = instance->getPackProfile()->getComponentVersion(id);
        if (!version.isEmpty())
            return id.section('.', -1) + ' ' + version;
    }
    return QObject::tr("vanilla");
}

QString profileDescription(const MinecraftInstance* instance)
{
    if (!instance)
        return {};
    QString version = QObject::tr("unknown version");
    if (instance->getPackProfile()) {
        const QString componentVersion = instance->getPackProfile()->getComponentVersion("net.minecraft");
        if (!componentVersion.isEmpty())
            version = componentVersion;
    }
    return QStringLiteral("%1 — %2 (%3)").arg(instance->name(), version, loaderDescription(instance));
}

bool copyDirectoryContents(const QString& source, const QString& destination)
{
    QDir sourceDir(source);
    if (!sourceDir.exists() || !QDir().mkpath(destination))
        return false;

    QDirIterator iterator(source, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QFileInfo sourceInfo(iterator.next());
        if (sourceInfo.isSymLink())
            continue;

        const QString relative = sourceDir.relativeFilePath(sourceInfo.filePath());
        const QString destinationPath = QDir(destination).filePath(relative);
        if (sourceInfo.isDir()) {
            if (!QDir().mkpath(destinationPath))
                return false;
            continue;
        }

        if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath()) || !QFile::copy(sourceInfo.filePath(), destinationPath))
            return false;
    }
    return true;
}

bool copyEntry(const TransferJob& job, QString& error)
{
    const QFileInfo sourceInfo(job.sourcePath);
    if (!sourceInfo.exists() || sourceInfo.isSymLink()) {
        error = QObject::tr("The source item is missing or is a symbolic link.");
        return false;
    }

    if (QFileInfo::exists(job.destinationPath)) {
        error = QObject::tr("An item with this name already exists in the destination.");
        return false;
    }

    const QFileInfo destinationInfo(job.destinationPath);
    if (!QDir().mkpath(destinationInfo.absolutePath())) {
        error = QObject::tr("Could not create the destination folder.");
        return false;
    }

    const QString temporaryName = QStringLiteral(".%1.zeng-transfer-%2")
                                      .arg(destinationInfo.fileName(), QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString temporaryPath = QDir(destinationInfo.absolutePath()).filePath(temporaryName);

    if (sourceInfo.isDir()) {
        if (!copyDirectoryContents(job.sourcePath, temporaryPath)) {
            QDir(temporaryPath).removeRecursively();
            error = QObject::tr("Could not copy the world completely.");
            return false;
        }
        if (QFileInfo::exists(job.destinationPath)) {
            QDir(temporaryPath).removeRecursively();
            error = QObject::tr("An item with this name already exists in the destination.");
            return false;
        }
        if (!QDir(destinationInfo.absolutePath()).rename(temporaryName, destinationInfo.fileName())) {
            QDir(temporaryPath).removeRecursively();
            error = QObject::tr("Could not finish copying the world.");
            return false;
        }
        return true;
    }

    if (!QFile::copy(job.sourcePath, temporaryPath)) {
        error = QObject::tr("Could not copy the mod file.");
        return false;
    }
    if (QFileInfo::exists(job.destinationPath) || !QFile(temporaryPath).rename(job.destinationPath)) {
        QFile::remove(temporaryPath);
        error = QObject::tr("Could not finish copying the mod file; the destination may already contain it.");
        return false;
    }
    return true;
}

TransferResult copyEntries(const QList<TransferJob>& jobs)
{
    TransferResult result;
    for (const TransferJob& job : jobs) {
        QString error;
        if (copyEntry(job, error))
            result.copied.append(job.type + QStringLiteral(": ") + job.name);
        else if (error.contains(QObject::tr("already exists")))
            result.skipped.append(job.type + QStringLiteral(": ") + job.name);
        else
            result.failed.append(job.type + QStringLiteral(": ") + job.name + QStringLiteral(" — ") + error);
    }
    return result;
}
}  // namespace

TransferContentDialog::TransferContentDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Transfer mods & worlds"));
    resize(680, 500);

    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(tr("Move content between Minecraft profiles"), this);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    headingFont.setPointSize(headingFont.pointSize() + 2);
    heading->setFont(headingFont);
    layout->addWidget(heading);

    auto* profileForm = new QFormLayout();
    m_source = new QComboBox(this);
    m_destination = new QComboBox(this);
    profileForm->addRow(tr("From profile"), m_source);
    profileForm->addRow(tr("To profile"), m_destination);
    layout->addLayout(profileForm);

    m_compatibility = new QLabel(this);
    m_compatibility->setWordWrap(true);
    m_compatibility->setStyleSheet(QStringLiteral("QLabel { color: palette(highlight); padding: 4px; }"));
    layout->addWidget(m_compatibility);

    m_items = new QTreeWidget(this);
    m_items->setColumnCount(3);
    m_items->setHeaderLabels({ tr("Type"), tr("Name"), tr("Destination status") });
    m_items->setRootIsDecorated(false);
    m_items->setSelectionMode(QAbstractItemView::NoSelection);
    m_items->header()->setStretchLastSection(true);
    m_items->setColumnWidth(0, 90);
    m_items->setColumnWidth(1, 290);
    layout->addWidget(m_items, 1);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto* footer = new QHBoxLayout();
    auto* selectAll = new QPushButton(tr("Select available"), this);
    auto* selectNone = new QPushButton(tr("Clear selection"), this);
    auto* closeButton = new QPushButton(tr("Close"), this);
    m_copyButton = new QPushButton(tr("Copy selected"), this);
    m_copyButton->setDefault(true);
    footer->addWidget(selectAll);
    footer->addWidget(selectNone);
    footer->addStretch(1);
    footer->addWidget(closeButton);
    footer->addWidget(m_copyButton);
    layout->addLayout(footer);

    for (int i = 0; i < APPLICATION->instances()->count(); ++i) {
        auto* instance = dynamic_cast<MinecraftInstance*>(APPLICATION->instances()->at(i));
        if (!instance || instance->isRunning() || instance->currentStatus() != BaseInstance::Status::Present)
            continue;
        const QString id = instance->id();
        const QString description = profileDescription(instance);
        m_source->addItem(description, id);
        m_destination->addItem(description, id);
    }

    connect(m_source, qOverload<int>(&QComboBox::currentIndexChanged), this, &TransferContentDialog::updatePreview);
    connect(m_destination, qOverload<int>(&QComboBox::currentIndexChanged), this, &TransferContentDialog::updatePreview);
    connect(m_items, &QTreeWidget::itemChanged, this, &TransferContentDialog::updateCopyButton);
    connect(selectAll, &QPushButton::clicked, this, [this] {
        for (const Entry& entry : std::as_const(m_entries)) {
            if (!entry.conflict)
                entry.row->setCheckState(0, Qt::Checked);
        }
    });
    connect(selectNone, &QPushButton::clicked, this, [this] {
        for (const Entry& entry : std::as_const(m_entries)) {
            if (!entry.conflict)
                entry.row->setCheckState(0, Qt::Unchecked);
        }
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_copyButton, &QPushButton::clicked, this, &TransferContentDialog::copySelected);

    updatePreview();
}

MinecraftInstance* TransferContentDialog::selectedInstance(QComboBox* combo) const
{
    if (combo->currentIndex() < 0)
        return nullptr;
    return dynamic_cast<MinecraftInstance*>(APPLICATION->instances()->getInstanceById(combo->currentData().toString()));
}

void TransferContentDialog::addEntries(const QString& type, const QString& sourceDir, const QString& destinationDir, bool directories)
{
    const QDir dir(sourceDir);
    const QDir::Filters filters = directories ? (QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks)
                                             : (QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
    const QFileInfoList files = dir.entryInfoList(filters, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& file : files) {
        if (file.isSymLink())
            continue;
        if (directories && !QFileInfo(QDir(file.absoluteFilePath()).filePath("level.dat")).isFile())
            continue;

        Entry entry;
        entry.type = type;
        entry.name = file.fileName();
        entry.sourcePath = file.absoluteFilePath();
        entry.destinationPath = QDir(destinationDir).filePath(entry.name);
        entry.conflict = QFileInfo::exists(entry.destinationPath);

        const QString destinationStatus = entry.conflict ? tr("Already exists — skipped") : tr("Ready to copy");
        auto* row = new QTreeWidgetItem(m_items);
        row->setText(0, type);
        row->setText(1, entry.name);
        row->setText(2, destinationStatus);
        Qt::ItemFlags flags = Qt::ItemIsEnabled;
        if (!entry.conflict)
            flags |= Qt::ItemIsUserCheckable;
        row->setFlags(flags);
        row->setCheckState(0, entry.conflict ? Qt::Unchecked : Qt::Checked);
        if (entry.conflict)
            row->setToolTip(2, tr("This destination item will not be overwritten."));
        entry.row = row;
        m_entries.append(entry);
    }
}

void TransferContentDialog::updatePreview()
{
    const QSignalBlocker blocker(m_items);
    m_items->clear();
    m_entries.clear();

    auto* source = selectedInstance(m_source);
    auto* destination = selectedInstance(m_destination);
    const bool profilesDiffer = source && destination && source->id() != destination->id();
    if (profilesDiffer) {
        addEntries(tr("Mod"), source->modsRoot(), destination->modsRoot(), false);
        addEntries(tr("World"), source->worldDir(), destination->worldDir(), true);
    }

    QStringList warnings;
    if (profilesDiffer) {
        const auto* sourceProfile = source->getPackProfile();
        const auto* destinationProfile = destination->getPackProfile();
        const QString sourceVersion = sourceProfile ? sourceProfile->getComponentVersion("net.minecraft") : tr("unknown");
        const QString destinationVersion = destinationProfile ? destinationProfile->getComponentVersion("net.minecraft") : tr("unknown");
        if (sourceVersion != destinationVersion)
            warnings.append(tr("Minecraft versions differ (%1 → %2); mods may not be compatible.").arg(sourceVersion, destinationVersion));
        if (loaderDescription(source) != loaderDescription(destination))
            warnings.append(tr("The mod loaders differ; some mods may not work in the destination profile."));
    } else if (source && destination) {
        warnings.append(tr("Choose two different profiles to preview content."));
    } else {
        warnings.append(tr("Create at least one Minecraft profile before transferring content."));
    }
    warnings.append(tr("Opening a world in a different Minecraft version can change its save format. Source files stay in place."));
    m_compatibility->setText(warnings.join(' '));
    m_summary->setText(tr("%1 items available. Existing names are skipped and never overwritten.").arg(m_entries.size()));
    updateCopyButton();
}

void TransferContentDialog::updateCopyButton()
{
    int selectedCount = 0;
    for (const Entry& entry : std::as_const(m_entries)) {
        if (!entry.conflict && entry.row->checkState(0) == Qt::Checked)
            ++selectedCount;
    }
    m_copyButton->setEnabled(selectedCount > 0 && selectedInstance(m_source) && selectedInstance(m_destination) &&
                             selectedInstance(m_source)->id() != selectedInstance(m_destination)->id());
    if (m_summary)
        m_summary->setText(tr("%1 selected; %2 existing-name conflicts skipped. Source files are kept.")
                               .arg(selectedCount)
                               .arg(std::count_if(m_entries.cbegin(), m_entries.cend(), [](const Entry& entry) { return entry.conflict; })));
}

void TransferContentDialog::copySelected()
{
    auto* source = selectedInstance(m_source);
    auto* destination = selectedInstance(m_destination);
    if (!source || !destination || source->id() == destination->id() || source->isRunning() || destination->isRunning())
        return;

    QList<TransferJob> jobs;
    bool includesWorlds = false;
    for (const Entry& entry : std::as_const(m_entries)) {
        if (entry.conflict || entry.row->checkState(0) != Qt::Checked)
            continue;
        jobs.append({ entry.type, entry.name, entry.sourcePath, entry.destinationPath });
        includesWorlds |= entry.type == tr("World");
    }
    if (jobs.isEmpty())
        return;

    QString confirmation = tr("Copy %1 items from %2 to %3? Existing files will be skipped, and the originals will stay in the source profile.")
                               .arg(jobs.size())
                               .arg(source->name(), destination->name());
    if (includesWorlds)
        confirmation += QStringLiteral("\n\n") + tr("Opening a transferred world in a different Minecraft version may change its save format.");
    if (QMessageBox::question(this, tr("Confirm transfer"), confirmation, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) !=
        QMessageBox::Yes)
        return;

    QProgressDialog progress(tr("Copying selected content…"), QString(), 0, 0, this);
    progress.setWindowTitle(tr("Transfer content"));
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);

    QFutureWatcher<TransferResult> watcher;
    connect(&watcher, &QFutureWatcher<TransferResult>::finished, &progress, &QDialog::accept);
    watcher.setFuture(QtConcurrent::run([jobs] { return copyEntries(jobs); }));
    progress.exec();
    watcher.waitForFinished();
    const TransferResult result = watcher.result();

    destination->loaderModList()->update();
    destination->worldList()->update();
    updatePreview();

    QString summary = tr("Copied %1 item(s).").arg(result.copied.size());
    if (!result.skipped.isEmpty())
        summary += QStringLiteral("\n") + tr("Skipped %1 item(s) because a destination with that name already exists.").arg(result.skipped.size());
    if (!result.failed.isEmpty())
        summary += QStringLiteral("\n\n") + tr("Could not copy:") + QStringLiteral("\n") + result.failed.join('\n');
    QMessageBox::information(this, result.failed.isEmpty() ? tr("Transfer complete") : tr("Transfer finished with errors"), summary);
}
