/*
    SPDX-FileCopyrightText: 2026 Kate fork contributors

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "katecrashrecovery.h"

#include "doc_or_widget.h"
#include "kateapp.h"
#include "katedebug.h"
#include "katedocmanager.h"
#include "katemainwindow.h"
#include "katesessionmanager.h"
#include "katestashmanager.h"
#include "kateviewmanager.h"

#include <KLocalizedString>
#include <KMessageBox>
#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

using namespace Qt::Literals::StringLiterals;

static constexpr auto IndexFile = "index.json"_L1;
static constexpr auto LockFile = "lock"_L1;

static QString envMode()
{
    return qEnvironmentVariable("KATE_CRASH_RECOVERY").toLower();
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(data);
    return file.commit();
}

static std::unique_ptr<QLockFile> makeLock(const QString &dir)
{
    auto lock = std::make_unique<QLockFile>(dir + u'/' + LockFile);
    // never treat the lock of a running instance as stale because of its age,
    // only a dead owner makes it stale
    lock->setStaleLockTime(0);
    return lock;
}

KateCrashRecovery::KateCrashRecovery(QObject *parent)
    : QObject(parent)
    , m_baseDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + u"/recovery"_s)
{
    // at most one write every 2 seconds while typing, the delay does not restart on each change
    m_saveTimer.setInterval(2000);
    m_saveTimer.setSingleShot(true);
    m_saveTimer.callOnTimeout(this, &KateCrashRecovery::saveNow);
}

KateCrashRecovery::~KateCrashRecovery()
{
    // a destructor without an explicit stop() is no clean exit, keep the data
    if (m_active) {
        stop(false);
    }
}

bool KateCrashRecovery::isDisabled()
{
    return envMode() == "off"_L1;
}

void KateCrashRecovery::setBaseDir(const QString &dir)
{
    Q_ASSERT(!m_active);
    m_baseDir = dir;
}

bool KateCrashRecovery::acquireInstanceDir()
{
    if (!QDir().mkpath(m_baseDir)) {
        qCWarning(LOG_KATE, "Crash recovery: could not create %ls", qUtf16Printable(m_baseDir));
        return false;
    }

    // a scanning instance may grab the lock of a just created folder, then try the next name
    const QString stem = u"%1-%2"_s.arg(QCoreApplication::applicationPid()).arg(QDateTime::currentMSecsSinceEpoch());
    for (int i = 0; i < 100; ++i) {
        const QString dir = m_baseDir + u'/' + stem + (i ? u"-%1"_s.arg(i) : QString());
        if (QFileInfo::exists(dir) || !QDir().mkpath(dir)) {
            continue;
        }
        auto lock = makeLock(dir);
        if (lock->tryLock(0)) {
            m_instanceDir = dir;
            m_lock = std::move(lock);
            return true;
        }
    }
    qCWarning(LOG_KATE, "Crash recovery: could not create an instance folder in %ls", qUtf16Printable(m_baseDir));
    return false;
}

void KateCrashRecovery::start()
{
    if (m_active || isDisabled() || !acquireInstanceDir()) {
        return;
    }
    m_active = true;

    auto dm = KateApp::self()->documentManager();
    connect(dm, &KateDocManager::documentCreated, this, [this](KTextEditor::Document *doc) {
        trackDocument(doc);
        scheduleSave();
    });
    connect(dm, &KateDocManager::documentWillBeDeleted, this, [this](KTextEditor::Document *doc) {
        m_saved.remove(doc);
        scheduleSave();
    });
    connect(dm, &KateDocManager::documentDeleted, this, &KateCrashRecovery::scheduleSave);

    const auto docs = KateApp::self()->documents();
    for (auto doc : docs) {
        trackDocument(doc);
    }
    saveNow();
}

void KateCrashRecovery::trackDocument(KTextEditor::Document *doc)
{
    connect(doc, &KTextEditor::Document::textChanged, this, &KateCrashRecovery::scheduleSave);
    connect(doc, &KTextEditor::Document::documentUrlChanged, this, &KateCrashRecovery::scheduleSave);
}

void KateCrashRecovery::scheduleSave()
{
    if (m_active && !m_saveTimer.isActive()) {
        m_saveTimer.start();
    }
}

void KateCrashRecovery::stop(bool removeData)
{
    if (!m_active) {
        return;
    }
    m_active = false;
    m_saveTimer.stop();
    disconnect(KateApp::self()->documentManager(), nullptr, this, nullptr);
    const auto docs = KateApp::self()->documents();
    for (auto doc : docs) {
        disconnect(doc, nullptr, this, nullptr);
    }
    m_saved.clear();

    if (removeData) {
        m_lock->unlock();
        QDir(m_instanceDir).removeRecursively();
    }
    // else the lock file stays, it is stale once this process is gone
    m_lock.reset();
}

bool KateCrashRecovery::tracksDocuments() const
{
    // a named session restores its documents itself (the stash covers untitled ones),
    // restoring them here too would open them twice
    auto stash = KateApp::self()->stashManager();
    return !(stash->canStash() && stash->stashNewUnsavedFiles);
}

bool KateCrashRecovery::saveNow()
{
    if (!m_active) {
        return false;
    }
    m_saveTimer.stop();

    // our folder must still be there and be ours
    if (!QFileInfo::exists(m_instanceDir + u'/' + LockFile)) {
        qCWarning(LOG_KATE, "Crash recovery: instance folder %ls vanished, recreating it", qUtf16Printable(m_instanceDir));
        m_lock.reset();
        m_saved.clear();
        if (!acquireInstanceDir()) {
            return false;
        }
    }

    // map the documents to the first window that shows them, documents without views go to the first window
    const int windowCount = std::max(1, KateApp::self()->mainWindowsCount());
    std::vector<std::vector<KTextEditor::Document *>> windows(windowCount);
    QHash<KTextEditor::Document *, KTextEditor::Cursor> cursors;
    QSet<KTextEditor::Document *> seen;
    if (tracksDocuments()) {
        for (int i = 0; i < KateApp::self()->mainWindowsCount(); ++i) {
            const auto views = KateApp::self()->mainWindow(i)->views();
            for (auto view : views) {
                auto doc = view->document();
                if (!seen.contains(doc)) {
                    seen.insert(doc);
                    windows[i].push_back(doc);
                    cursors.insert(doc, view->cursorPosition());
                }
            }
        }
        const auto docs = KateApp::self()->documents();
        for (auto doc : docs) {
            if (!seen.contains(doc)) {
                seen.insert(doc);
                windows[0].push_back(doc);
            }
        }
    }

    // 1. the content of untitled documents, only if changed since the last write
    bool ok = true;
    QSet<QString> usedFiles;
    QJsonArray jsonWindows;
    for (const auto &docs : windows) {
        QJsonArray jsonDocs;
        for (auto doc : docs) {
            QJsonObject entry;
            if (doc->url().isEmpty()) {
                if (doc->isEmpty()) {
                    m_saved.remove(doc);
                    continue;
                }
                auto it = m_saved.find(doc);
                if (it == m_saved.end()) {
                    it = m_saved.insert(doc, {.file = u"untitled-%1.txt"_s.arg(m_nextFileId++)});
                }
                const QString path = m_instanceDir + u'/' + it->file;
                if (it->revision != doc->revision() || !QFileInfo::exists(path)) {
                    if (writeFile(path, doc->text().toUtf8())) {
                        it->revision = doc->revision();
                    } else {
                        qCWarning(LOG_KATE, "Crash recovery: could not write %ls", qUtf16Printable(path));
                        it->revision = -1;
                        ok = false;
                    }
                }
                usedFiles.insert(it->file);
                entry[u"untitled"_s] = it->file;
                entry[u"name"_s] = doc->documentName();
                entry[u"mode"_s] = doc->mode();
                entry[u"highlighting"_s] = doc->highlightingMode();
            } else {
                // saved meanwhile, no content file anymore
                m_saved.remove(doc);
                entry[u"url"_s] = doc->url().toString();
                entry[u"encoding"_s] = doc->encoding();
                const auto cursor = cursors.value(doc, KTextEditor::Cursor::invalid());
                if (cursor.isValid()) {
                    entry[u"line"_s] = cursor.line();
                    entry[u"column"_s] = cursor.column();
                }
            }
            jsonDocs.append(entry);
        }
        jsonWindows.append(jsonDocs);
    }

    // 2. the index, it only refers to content files that are written
    QJsonObject index;
    index[u"version"_s] = 1;
    index[u"pid"_s] = QCoreApplication::applicationPid();
    index[u"saved"_s] = QDateTime::currentDateTime().toString(Qt::ISODate);
    index[u"windows"_s] = jsonWindows;
    if (!writeFile(m_instanceDir + u'/' + IndexFile, QJsonDocument(index).toJson())) {
        qCWarning(LOG_KATE, "Crash recovery: could not write the index in %ls", qUtf16Printable(m_instanceDir));
        return false;
    }

    // 3. only now remove content files the index doesn't refer to anymore
    const auto files = QDir(m_instanceDir).entryList(QDir::Files);
    for (const QString &file : files) {
        if (file != IndexFile && file != LockFile && !usedFiles.contains(file)) {
            QFile::remove(m_instanceDir + u'/' + file);
        }
    }
    return ok;
}

std::vector<KateCrashRecovery::Orphan> KateCrashRecovery::claimOrphans()
{
    std::vector<Orphan> orphans;
    const auto dirs = QDir(m_baseDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time | QDir::Reversed);
    for (const QFileInfo &info : dirs) {
        const QString dir = info.absoluteFilePath();
        if (dir == m_instanceDir) {
            continue;
        }
        // no index: the owner is just starting or died before writing anything, leave it alone
        QFile indexFile(dir + u'/' + IndexFile);
        if (!indexFile.exists()) {
            continue;
        }
        std::shared_ptr<QLockFile> lock = makeLock(dir);
        if (!lock->tryLock(0)) {
            continue; // the owner is alive, or another instance claimed it
        }

        Orphan orphan;
        orphan.dir = dir;
        orphan.lock = lock;
        if (indexFile.open(QIODevice::ReadOnly)) {
            orphan.index = QJsonDocument::fromJson(indexFile.readAll()).object();
        }
        const auto windows = orphan.index.value(u"windows"_s).toArray();
        for (const auto &window : windows) {
            const auto docs = window.toArray();
            orphan.windowCount += docs.isEmpty() ? 0 : 1;
            for (const auto &doc : docs) {
                if (doc.toObject().contains(u"untitled"_s)) {
                    ++orphan.untitledCount;
                } else {
                    ++orphan.fileCount;
                }
            }
        }

        if (orphan.index.isEmpty()) {
            // unreadable index: keep the folder for manual inspection, never delete content we can't parse
            qCWarning(LOG_KATE, "Crash recovery: could not read %ls, leaving it in place", qUtf16Printable(indexFile.fileName()));
            lock->unlock();
            continue;
        }
        if (orphan.untitledCount + orphan.fileCount == 0) {
            // nothing to restore
            lock->unlock();
            QDir(dir).removeRecursively();
            continue;
        }
        orphans.push_back(std::move(orphan));
    }
    return orphans;
}

int KateCrashRecovery::restore(std::vector<Orphan> &orphans)
{
    KateMainWindow *first = KateApp::self()->activeKateMainWindow();
    if (!first) {
        first = KateApp::newMainWindow();
    }

    // the initial empty document of a fresh start is replaced by the restored ones
    KTextEditor::Document *pristine = nullptr;
    const auto docs = KateApp::self()->documents();
    if (docs.size() == 1 && docs[0]->url().isEmpty() && docs[0]->isEmpty() && !docs[0]->isModified()) {
        pristine = docs[0];
    }

    int restored = 0;
    bool firstUsed = false;
    QStringList missing;
    for (const auto &orphan : orphans) {
        const auto windows = orphan.index.value(u"windows"_s).toArray();
        for (const auto &window : windows) {
            const auto entries = window.toArray();
            if (entries.isEmpty()) {
                continue;
            }
            KateMainWindow *target = firstUsed ? KateApp::newMainWindow() : first;
            firstUsed = true;
            KTextEditor::Document *firstDoc = nullptr;

            for (const auto &value : entries) {
                const QJsonObject entry = value.toObject();
                KTextEditor::Document *doc = nullptr;
                if (entry.contains(u"untitled"_s)) {
                    QFile file(orphan.dir + u'/' + entry.value(u"untitled"_s).toString());
                    if (!file.open(QIODevice::ReadOnly)) {
                        qCWarning(LOG_KATE, "Crash recovery: could not read %ls", qUtf16Printable(file.fileName()));
                        continue;
                    }
                    const QString text = QString::fromUtf8(file.readAll());
                    auto view = target->viewManager()->createView();
                    if (!view) {
                        continue;
                    }
                    doc = view->document();
                    doc->setText(text);
                    const QString mode = entry.value(u"mode"_s).toString();
                    if (!mode.isEmpty()) {
                        doc->setMode(mode);
                    }
                    const QString hl = entry.value(u"highlighting"_s).toString();
                    if (!hl.isEmpty()) {
                        doc->setHighlightingMode(hl);
                    }
                } else {
                    const QUrl url(entry.value(u"url"_s).toString());
                    if (url.isEmpty()) {
                        continue;
                    }
                    // e.g. on an unmounted drive: don't create an empty document for it, tell the user
                    if (url.isLocalFile() && !QFileInfo::exists(url.toLocalFile())) {
                        missing << url.toLocalFile();
                        continue;
                    }
                    KateDocumentInfo info;
                    if (entry.contains(u"line"_s)) {
                        info.startCursor = KTextEditor::Cursor(entry.value(u"line"_s).toInt(), entry.value(u"column"_s).toInt());
                    }
                    doc = target->viewManager()->openUrl(url, entry.value(u"encoding"_s).toString(), /*activate=*/false, false, &info);
                }
                if (doc) {
                    ++restored;
                    if (!firstDoc) {
                        firstDoc = doc;
                    }
                }
            }
            if (firstDoc) {
                target->viewManager()->activateView(DocOrWidget(firstDoc));
            }
        }
    }

    if (pristine && restored > 0 && KateApp::self()->documents().contains(pristine) && pristine->isEmpty() && !pristine->isModified()) {
        KateApp::self()->documentManager()->closeDocument(pristine);
    }

    // only drop the orphans once their documents are safe in our own folder
    if (saveNow()) {
        discard(orphans);
    } else {
        qCWarning(LOG_KATE, "Crash recovery: could not save the restored documents, keeping the old recovery data");
        release(orphans);
    }

    if (!missing.isEmpty()) {
        qCWarning(LOG_KATE, "Crash recovery: %d files to restore don't exist: %ls", int(missing.size()), qUtf16Printable(missing.join(u", "_s)));
        if (envMode().isEmpty() && !QStandardPaths::isTestModeEnabled()) {
            KMessageBox::informationList(KateApp::self()->activeKateMainWindow(),
                                         i18n("These files were open, but they don't exist anymore, e.g. because their drive is not mounted:"),
                                         missing,
                                         i18n("Files Not Restored"));
        }
    }
    return restored;
}

void KateCrashRecovery::discard(std::vector<Orphan> &orphans)
{
    for (auto &orphan : orphans) {
        orphan.lock->unlock();
        QDir(orphan.dir).removeRecursively();
    }
    orphans.clear();
}

void KateCrashRecovery::release(std::vector<Orphan> &orphans)
{
    for (auto &orphan : orphans) {
        orphan.lock->unlock();
    }
    orphans.clear();
}

void KateCrashRecovery::offerRecovery()
{
    const QString mode = envMode();
    if (mode == "off"_L1) {
        return;
    }
    // never block automated runs with a dialog
    const bool canAsk = mode.isEmpty() && !QStandardPaths::isTestModeEnabled();
    if (mode.isEmpty() && !canAsk) {
        return;
    }

    auto orphans = claimOrphans();
    if (orphans.empty()) {
        return;
    }

    int untitled = 0;
    int files = 0;
    int windows = 0;
    for (const auto &orphan : orphans) {
        untitled += orphan.untitledCount;
        files += orphan.fileCount;
        windows += orphan.windowCount;
    }
    qCDebug(LOG_KATE, "Crash recovery: %d orphans with %d untitled documents and %d files", int(orphans.size()), untitled, files);

    KMessageBox::ButtonCode answer = KMessageBox::Cancel;
    if (mode == "restore"_L1) {
        answer = KMessageBox::PrimaryAction;
    } else if (mode == "discard"_L1) {
        answer = KMessageBox::SecondaryAction;
    } else if (canAsk) {
        const QString text = i18n(
            "Kate was not closed properly. The following can be restored:<ul><li>%1</li><li>%2</li><li>%3</li></ul>"
            "Modified files may also offer to recover their unsaved changes from their swap files.",
            i18np("1 unsaved new document", "%1 unsaved new documents", untitled),
            i18np("1 open file", "%1 open files", files),
            i18np("in 1 window", "in %1 windows", windows));
        answer = KMessageBox::questionTwoActionsCancel(KateApp::self()->activeKateMainWindow(),
                                                       text,
                                                       i18n("Restore Documents"),
                                                       KGuiItem(i18nc("@action:button", "Restore"), u"document-revert"_s),
                                                       KGuiItem(i18nc("@action:button", "Discard"), u"edit-delete"_s),
                                                       KGuiItem(i18nc("@action:button", "Ask Again Later"), u"dialog-cancel"_s));
    }

    if (answer == KMessageBox::PrimaryAction) {
        restore(orphans);
    } else if (answer == KMessageBox::SecondaryAction) {
        discard(orphans);
    } else {
        release(orphans);
    }
}

#include "moc_katecrashrecovery.cpp"
