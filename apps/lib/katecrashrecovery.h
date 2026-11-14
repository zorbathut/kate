/*
    SPDX-FileCopyrightText: 2026 Kate fork contributors

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#pragma once

#include "kateprivate_export.h"

#include <QHash>
#include <QJsonObject>
#include <QLockFile>
#include <QObject>
#include <QTimer>

#include <memory>
#include <vector>

namespace KTextEditor
{
class Document;
}

/**
 * Crash recovery for all documents of an instance, independent of sessions.
 *
 * Every running instance keeps its own folder <baseDir>/<pid>-<time>, guarded by a lock file.
 * Shortly after each change, the folder gets an index of all open documents per window, plus
 * the content of every untitled document. Named files are only listed: their unsaved changes
 * are covered by the swap files of KTextEditor.
 *
 * A clean exit removes the folder. A folder whose lock is stale belongs to an instance that died;
 * a new instance offers to restore it.
 */
class KATE_PRIVATE_EXPORT KateCrashRecovery : public QObject
{
    Q_OBJECT

public:
    explicit KateCrashRecovery(QObject *parent = nullptr);
    ~KateCrashRecovery() override;

    /**
     * Folder holding the folders of all instances, default: <AppDataLocation>/recovery.
     * Must be set before start().
     */
    void setBaseDir(const QString &dir);
    QString baseDir() const
    {
        return m_baseDir;
    }

    /**
     * Start tracking the documents of this instance and write the first state.
     */
    void start();

    /**
     * Stop tracking. With @p removeData (clean exit) the folder of this instance is removed,
     * else it stays for a later recovery.
     */
    void stop(bool removeData);

    /**
     * Mark that the user went through the close prompts, so the exit is clean.
     */
    void setCleanExit()
    {
        m_cleanExit = true;
    }
    bool isCleanExit() const
    {
        return m_cleanExit;
    }

    bool isActive() const
    {
        return m_active;
    }
    QString instanceDir() const
    {
        return m_instanceDir;
    }

    /**
     * Write the state now instead of after the delay.
     */
    bool saveNow();

    struct Orphan {
        QString dir;
        QJsonObject index;
        std::shared_ptr<QLockFile> lock;
        int untitledCount = 0;
        int fileCount = 0;
        int windowCount = 0;
    };

    /**
     * Find the folders of instances that died and claim them by taking their lock.
     * Claimed folders without anything to restore are removed right away.
     */
    std::vector<Orphan> claimOrphans();

    /**
     * Restore the documents of @p orphans into windows of this instance. The orphan folders are
     * removed only after the restored documents are written to the folder of this instance.
     * @return the number of restored documents
     */
    int restore(std::vector<Orphan> &orphans);

    /**
     * Remove the claimed orphan folders.
     */
    void discard(std::vector<Orphan> &orphans);

    /**
     * Give up the claim, the orphans are offered again by the next instance.
     */
    void release(std::vector<Orphan> &orphans);

    /**
     * Startup: claim orphans, ask the user and restore, discard or release them.
     * KATE_CRASH_RECOVERY=restore|discard|later answers without asking.
     */
    void offerRecovery();

    /**
     * KATE_CRASH_RECOVERY=off disables the feature.
     */
    static bool isDisabled();

private:
    void scheduleSave();
    void trackDocument(KTextEditor::Document *doc);
    bool tracksDocuments() const;
    bool acquireInstanceDir();

    QString m_baseDir;
    QString m_instanceDir;
    std::unique_ptr<QLockFile> m_lock;
    QTimer m_saveTimer;

    // the content file of an untitled document and the revision written to it
    struct Saved {
        QString file;
        qint64 revision = -1;
    };
    QHash<KTextEditor::Document *, Saved> m_saved;
    int m_nextFileId = 0;

    bool m_active = false;
    bool m_cleanExit = false;
};
