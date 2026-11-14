/*
    SPDX-FileCopyrightText: 2026 Kate fork contributors

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "kateapp.h"
#include "katecrashrecovery.h"
#include "katemainwindow.h"
#include "katesessionmanager.h"
#include "kateviewmanager.h"

#include <KConfig>
#include <KLocalizedString>
#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace Qt::Literals::StringLiterals;

class CrashRecoveryTest : public QObject
{
    Q_OBJECT

public:
    CrashRecoveryTest()
    {
        QStandardPaths::setTestModeEnabled(true);
        qApp->setApplicationName(u"kate"_s);
        KLocalizedString::setApplicationDomain(QByteArrayLiteral("kate"));
    }

private Q_SLOTS:
    void init();
    void cleanup();
    void savesUntitledContentOnEdit();
    void savedDocumentDropsContentFile();
    void listsNamedFilesWithCursor();
    void oldLockOfLiveInstanceIsNoOrphan();
    void deadOwnerMakesOrphan();
    void restoreOpensDocumentsPerWindow();
    void restoreKeepsDataIfNothingIsLost();
    void emptyOrphanIsRemoved();
    void unreadableIndexIsKept();
    void folderWithoutIndexIsLeftAlone();
    void cleanStopRemovesData();
    void uncleanStopKeepsData();
    void namedSessionIsNotTracked();

private:
    QJsonObject readIndex(const QString &dir) const;
    QString makeOrphan(const QString &name, const QJsonArray &windows, const QHash<QString, QByteArray> &files = {});
    KateMainWindow *mainWindow() const
    {
        return app->activeKateMainWindow();
    }

    std::unique_ptr<QTemporaryDir> m_tempdir;
    std::unique_ptr<KateApp> app;
    QString m_base;
};

QTEST_MAIN(CrashRecoveryTest)

void CrashRecoveryTest::init()
{
    m_tempdir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempdir->isValid());
    KConfig::setMainConfigName(m_tempdir->path() + u"/testconfigfilerc"_s);

    static QCommandLineParser parser;
    app.reset(); // KateApp mimics a singleton
    app = std::make_unique<KateApp>(parser, KateApp::ApplicationKate, m_tempdir->path() + u"/sessions"_s);
    app->sessionManager()->activateAnonymousSession();
    if (app->mainWindowsCount() == 0) {
        KateApp::newMainWindow();
    }

    m_base = m_tempdir->path() + u"/recovery"_s;
    app->crashRecovery()->setBaseDir(m_base);
}

void CrashRecoveryTest::cleanup()
{
    app->crashRecovery()->stop(true);
    app.reset();
}

QJsonObject CrashRecoveryTest::readIndex(const QString &dir) const
{
    QFile f(dir + u"/index.json"_s);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(f.readAll()).object();
}

QString CrashRecoveryTest::makeOrphan(const QString &name, const QJsonArray &windows, const QHash<QString, QByteArray> &files)
{
    const QString dir = m_base + u'/' + name;
    QDir().mkpath(dir);
    QJsonObject index;
    index[u"version"_s] = 1;
    index[u"windows"_s] = windows;
    QFile f(dir + u"/index.json"_s);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(index).toJson());
    }
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        QFile c(dir + u'/' + it.key());
        if (c.open(QIODevice::WriteOnly)) {
            c.write(it.value());
        }
    }
    return dir;
}

static QStringList untitledFiles(const QJsonObject &index)
{
    QStringList files;
    const auto windows = index.value(u"windows"_s).toArray();
    for (const auto &w : windows) {
        for (const auto &d : w.toArray()) {
            if (d.toObject().contains(u"untitled"_s)) {
                files << d.toObject().value(u"untitled"_s).toString();
            }
        }
    }
    return files;
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void CrashRecoveryTest::savesUntitledContentOnEdit()
{
    auto recovery = app->crashRecovery();
    recovery->start();
    QVERIFY(recovery->isActive());
    QVERIFY(QFile::exists(recovery->instanceDir() + u"/index.json"_s));

    auto doc = mainWindow()->viewManager()->createView()->document();
    doc->setText(u"first"_s);
    const auto content = [&] {
        const auto files = untitledFiles(readIndex(recovery->instanceDir()));
        return files.size() == 1 ? readFile(recovery->instanceDir() + u'/' + files[0]) : QByteArray();
    };
    QTRY_COMPARE_WITH_TIMEOUT(content(), QByteArray("first"), 5000);

    // a plain edit, no document opened or closed
    doc->insertText(doc->documentEnd(), u" second"_s);
    QTRY_COMPARE_WITH_TIMEOUT(content(), QByteArray("first second"), 5000);
}

void CrashRecoveryTest::savedDocumentDropsContentFile()
{
    auto recovery = app->crashRecovery();
    recovery->start();
    auto doc = mainWindow()->viewManager()->createView()->document();
    doc->setText(u"to be saved"_s);
    QVERIFY(recovery->saveNow());
    const QString file = untitledFiles(readIndex(recovery->instanceDir())).value(0);
    QVERIFY(!file.isEmpty());
    QVERIFY(QFile::exists(recovery->instanceDir() + u'/' + file));

    const QString path = m_tempdir->path() + u"/saved.txt"_s;
    QVERIFY(doc->saveAs(QUrl::fromLocalFile(path)));
    QVERIFY(recovery->saveNow());
    QVERIFY(untitledFiles(readIndex(recovery->instanceDir())).isEmpty());
    QVERIFY(!QFile::exists(recovery->instanceDir() + u'/' + file));
    QVERIFY(QJsonDocument(readIndex(recovery->instanceDir())).toJson().contains(QUrl::fromLocalFile(path).toString().toUtf8()));
}

void CrashRecoveryTest::listsNamedFilesWithCursor()
{
    const QString path = m_tempdir->path() + u"/named.txt"_s;
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("line 0\nline 1\nline 2\n");
    f.close();

    auto recovery = app->crashRecovery();
    recovery->start();
    auto view = mainWindow()->openUrl(QUrl::fromLocalFile(path));
    QVERIFY(view);
    view->setCursorPosition({2, 3});
    QVERIFY(recovery->saveNow());

    const auto docs = readIndex(recovery->instanceDir()).value(u"windows"_s).toArray().at(0).toArray();
    bool found = false;
    for (const auto &d : docs) {
        const auto o = d.toObject();
        if (o.value(u"url"_s).toString() == QUrl::fromLocalFile(path).toString()) {
            found = true;
            QCOMPARE(o.value(u"line"_s).toInt(), 2);
            QCOMPARE(o.value(u"column"_s).toInt(), 3);
        }
    }
    QVERIFY(found);
}

// An old lock of a live instance must not make it an orphan. QLockFile treats locks older than 30s as stale by
// default; on local file systems its native lock protects a live owner anyway, setStaleLockTime(0) covers the rest.
void CrashRecoveryTest::oldLockOfLiveInstanceIsNoOrphan()
{
    auto recovery = app->crashRecovery();
    recovery->start();
    auto doc = mainWindow()->viewManager()->createView()->document();
    doc->setText(u"alive"_s);
    QVERIFY(recovery->saveNow());

    QFile lock(recovery->instanceDir() + u"/lock"_s);
    QVERIFY(lock.open(QIODevice::ReadWrite));
    QVERIFY(lock.setFileTime(QDateTime::currentDateTime().addDays(-1), QFileDevice::FileModificationTime));
    lock.close();

    KateCrashRecovery other;
    other.setBaseDir(m_base);
    QVERIFY(other.claimOrphans().empty());
    QVERIFY(QFile::exists(recovery->instanceDir() + u"/index.json"_s));
}

void CrashRecoveryTest::deadOwnerMakesOrphan()
{
    QJsonArray windows{QJsonArray{QJsonObject{{u"untitled"_s, u"untitled-0.txt"_s}}}};
    const QString dir = makeOrphan(u"dead"_s, windows, {{u"untitled-0.txt"_s, "lost text"}});
    // a lock of a process that doesn't exist anymore, in the format of QLockFile
    QFile lock(dir + u"/lock"_s);
    QVERIFY(lock.open(QIODevice::WriteOnly));
    lock.write("2147483000\nkate\n" + QSysInfo::machineHostName().toUtf8() + "\n");
    lock.close();

    auto orphans = app->crashRecovery()->claimOrphans();
    QCOMPARE(orphans.size(), 1u);
    QCOMPARE(orphans[0].dir, dir);
    QCOMPARE(orphans[0].untitledCount, 1);
    // claimed: a second scanner doesn't get it
    KateCrashRecovery other;
    other.setBaseDir(m_base);
    QVERIFY(other.claimOrphans().empty());
    app->crashRecovery()->release(orphans);
    // released: offered again
    QCOMPARE(other.claimOrphans().size(), 1u);
}

void CrashRecoveryTest::restoreOpensDocumentsPerWindow()
{
    const QString path = m_tempdir->path() + u"/file.txt"_s;
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("on disk\n");
    f.close();

    QJsonArray windows{
        QJsonArray{QJsonObject{{u"untitled"_s, u"untitled-0.txt"_s}, {u"highlighting"_s, u"Python"_s}},
                   QJsonObject{{u"url"_s, QUrl::fromLocalFile(path).toString()}, {u"line"_s, 0}, {u"column"_s, 2}}},
        QJsonArray{QJsonObject{{u"untitled"_s, u"untitled-1.txt"_s}}},
        QJsonArray{QJsonObject{{u"url"_s, QUrl::fromLocalFile(m_tempdir->path() + u"/gone.txt"_s).toString()}}},
    };
    const QString dir = makeOrphan(u"crashed"_s, windows, {{u"untitled-0.txt"_s, "print('hi')\n"}, {u"untitled-1.txt"_s, "notes\n"}});

    auto recovery = app->crashRecovery();
    recovery->start();
    const int windowsBefore = app->mainWindowsCount();
    // only the initial empty document is open, like on a fresh start
    if (app->documents().isEmpty()) {
        mainWindow()->viewManager()->createView();
    }
    QCOMPARE(app->documents().size(), 1);

    auto orphans = recovery->claimOrphans();
    QCOMPARE(orphans.size(), 1u);
    QCOMPARE(recovery->restore(orphans), 3);

    QStringList texts;
    bool hasFile = false;
    bool python = false;
    const auto docs = app->documents();
    for (auto doc : docs) {
        if (doc->url().isEmpty()) {
            texts << doc->text();
            QVERIFY(doc->isModified());
            python |= doc->highlightingMode() == u"Python"_s;
        } else if (doc->url() == QUrl::fromLocalFile(path)) {
            hasFile = true;
        }
    }
    texts.sort();
    // the initial empty document was replaced
    QCOMPARE(texts, (QStringList{u"notes\n"_s, u"print('hi')\n"_s}));
    QVERIFY(hasFile);
    QVERIFY(python);
    // first window reused, one new window for the second, none for the window with only a missing file
    QCOMPARE(app->mainWindowsCount(), windowsBefore + 2);

    // the orphan is gone, its documents are in our recovery data now
    QVERIFY(!QFileInfo::exists(dir));
    QCOMPARE(untitledFiles(readIndex(recovery->instanceDir())).size(), 2);
}

void CrashRecoveryTest::restoreKeepsDataIfNothingIsLost()
{
    // restore() keeps the orphan when its documents could not be written to our folder,
    // here: we never started, so saveNow() fails
    QJsonArray windows{QJsonArray{QJsonObject{{u"untitled"_s, u"untitled-0.txt"_s}}}};
    const QString dir = makeOrphan(u"keep"_s, windows, {{u"untitled-0.txt"_s, "keep me"}});
    auto recovery = app->crashRecovery();
    auto orphans = recovery->claimOrphans();
    QCOMPARE(orphans.size(), 1u);
    QCOMPARE(recovery->restore(orphans), 1);
    QVERIFY(QFile::exists(dir + u"/untitled-0.txt"_s));
}

void CrashRecoveryTest::emptyOrphanIsRemoved()
{
    const QString dir = makeOrphan(u"empty"_s, QJsonArray{QJsonArray{}});
    QVERIFY(app->crashRecovery()->claimOrphans().empty());
    QVERIFY(!QFileInfo::exists(dir));
}

void CrashRecoveryTest::unreadableIndexIsKept()
{
    const QString dir = m_base + u"/broken"_s;
    QDir().mkpath(dir);
    QFile f(dir + u"/index.json"_s);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{ not json");
    f.close();
    QVERIFY(app->crashRecovery()->claimOrphans().empty());
    QVERIFY(QFile::exists(dir + u"/index.json"_s));
}

void CrashRecoveryTest::folderWithoutIndexIsLeftAlone()
{
    const QString dir = m_base + u"/starting"_s;
    QDir().mkpath(dir);
    QVERIFY(app->crashRecovery()->claimOrphans().empty());
    QVERIFY(QFileInfo::exists(dir));
    // and no lock was left behind that would block its owner
    QVERIFY(!QFile::exists(dir + u"/lock"_s));
}

void CrashRecoveryTest::cleanStopRemovesData()
{
    auto recovery = app->crashRecovery();
    recovery->start();
    const QString dir = recovery->instanceDir();
    QVERIFY(QFileInfo::exists(dir));
    recovery->stop(true);
    QVERIFY(!QFileInfo::exists(dir));
}

void CrashRecoveryTest::uncleanStopKeepsData()
{
    auto recovery = app->crashRecovery();
    recovery->start();
    auto doc = mainWindow()->viewManager()->createView()->document();
    doc->setText(u"keep"_s);
    QVERIFY(recovery->saveNow());
    const QString dir = recovery->instanceDir();
    recovery->stop(false);
    QCOMPARE(untitledFiles(readIndex(dir)).size(), 1);
}

void CrashRecoveryTest::namedSessionIsNotTracked()
{
    app->sessionManager()->activateSession(u"named"_s, false, false);
    auto recovery = app->crashRecovery();
    recovery->start();
    auto doc = mainWindow()->viewManager()->createView()->document();
    doc->setText(u"in the stash instead"_s);
    QVERIFY(recovery->saveNow());
    QVERIFY(untitledFiles(readIndex(recovery->instanceDir())).isEmpty());
}

#include "crash_recovery_test.moc"
