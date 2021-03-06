/* This file is part of the KDE project
   Copyright (C) 2015 David Faure <faure@kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include <ksycoca.h>
#include <kbuildsycoca_p.h>
#include <ksycoca_p.h>
#include <QTemporaryDir>
#include <QTest>
#include <QDebug>
#include <kservicetype.h>
#include <kdesktopfile.h>
#include <kconfiggroup.h>
#include <QSignalSpy>
#include <QProcess>
#include <kservice.h>
#include <kservicefactory_p.h>
#include <kservicetypefactory_p.h>

// ## use QFile::setFileTime when it lands in Qt
#include <time.h>
#ifdef Q_OS_UNIX
#include <utime.h>
#include <sys/time.h>
#endif

// taken from tst_qstandardpaths
#if defined(Q_OS_UNIX) && !defined(Q_OS_MAC) && !defined(Q_OS_BLACKBERRY) && !defined(Q_OS_ANDROID)
#define Q_XDG_PLATFORM
#endif

// On Unix, lastModified() finally returns milliseconds as well, since Qt 5.8.0
// Not sure about the situation on Windows though.
static const int s_waitDelay = 10;

extern KSERVICE_EXPORT int ksycoca_ms_between_checks;

class KSycocaTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::enableTestMode(true);

        QVERIFY(m_tempDir.isValid());

        // we don't need the services dir -> ensure there isn't one, so we can check allResourceDirs below.
        QDir(servicesDir()).removeRecursively();

        QDir(menusDir()).removeRecursively();
        QDir().mkpath(menusDir() + "/fakeSubserviceDirectory");

#ifdef Q_XDG_PLATFORM
        qputenv("XDG_DATA_DIRS", QFile::encodeName(m_tempDir.path()));

        // so that vfolder_menu doesn't go look into /etc and /usr
        qputenv("XDG_CONFIG_DIRS", QFile::encodeName(m_tempDir.path()));
#else
        // We need to make changes to a global dir without messing up the system
        QSKIP("This test requires XDG_DATA_DIRS");
#endif
        createGlobalServiceType();
    }

    void cleanupTestCase()
    {
        QFile::remove(serviceTypesDir() + "/fakeLocalServiceType.desktop");
        QFile::remove(KSycoca::absoluteFilePath());
    }
    void ensureCacheValidShouldCreateDB();
    void kBuildSycocaShouldEmitDatabaseChanged();
    void dirInFutureShouldRebuildSycocaOnce();
    void dirTimestampShouldBeCheckedRecursively();
    void recursiveCheckShouldIgnoreLinksGoingUp();
    void testAllResourceDirs();
    void testDeletingSycoca();
    void testGlobalSycoca();
    void testNonReadableSycoca();

private:
    void createGlobalServiceType()
    {
        KDesktopFile file(serviceTypesDir() + "/fakeGlobalServiceType.desktop");
        KConfigGroup group = file.desktopGroup();
        group.writeEntry("Comment", "Fake Global ServiceType");
        group.writeEntry("Type", "ServiceType");
        group.writeEntry("X-KDE-ServiceType", "FakeGlobalServiceType");
        file.sync();
        qDebug() << "created" << serviceTypesDir() + "/fakeGlobalServiceType.desktop";
    }
    QString servicesDir() const { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/kservices5"; }
    QString serviceTypesDir() const { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/kservicetypes5"; }
    QString menusDir() const { return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/menus"; }
    QString appsDir() const { return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + QLatin1Char('/'); }

    static void runKBuildSycoca(const QProcessEnvironment &environment, bool global = false);

    QTemporaryDir m_tempDir;
};

QTEST_MAIN(KSycocaTest)

void KSycocaTest::ensureCacheValidShouldCreateDB() // this is what kded does on startup
{
    QFile::remove(KSycoca::absoluteFilePath());
    KSycoca::self()->ensureCacheValid();
    QVERIFY(QFile::exists(KSycoca::absoluteFilePath()));
    QVERIFY(KServiceType::serviceType(QStringLiteral("FakeGlobalServiceType")));
}

void KSycocaTest::kBuildSycocaShouldEmitDatabaseChanged()
{
    // It used to be a DBus signal, now it's file watching
    QTest::qWait(s_waitDelay);
    // Ensure kbuildsycoca has something to do
    QVERIFY(QFile::remove(serviceTypesDir() + "/fakeGlobalServiceType.desktop"));
    // Run kbuildsycoca
    QSignalSpy spy(KSycoca::self(), SIGNAL(databaseChanged(QStringList)));
    runKBuildSycoca(QProcessEnvironment::systemEnvironment());
    qDebug() << "waiting for signal";
    QVERIFY(spy.wait(20000));
    qDebug() << "got signal";
    // Put it back for other tests
    createGlobalServiceType();
}

void KSycocaTest::dirInFutureShouldRebuildSycocaOnce()
{
    const QDateTime oldTimestamp = QFileInfo(KSycoca::absoluteFilePath()).lastModified();

    // ### use QFile::setFileTime when it lands in Qt...
#ifdef Q_OS_UNIX
    const QString path = serviceTypesDir();
    struct timeval tp;
    gettimeofday(&tp, nullptr);
    struct utimbuf utbuf;
    utbuf.actime = tp.tv_sec;
    utbuf.modtime = tp.tv_sec + 60; // 60 second in the future
    QCOMPARE(utime(QFile::encodeName(path).constData(), &utbuf), 0);
    qDebug("Time changed for %s", qPrintable(path));
    qDebug() << QDateTime::currentDateTime() << QFileInfo(path).lastModified();
#else
    QSKIP("This test requires utime");
#endif
    ksycoca_ms_between_checks = 0;

    QTest::qWait(s_waitDelay);

    KSycoca::self()->ensureCacheValid();
    const QDateTime newTimestamp = QFileInfo(KSycoca::absoluteFilePath()).lastModified();
    QVERIFY(newTimestamp > oldTimestamp);

    QTest::qWait(s_waitDelay);

    KSycoca::self()->ensureCacheValid();
    const QDateTime againTimestamp = QFileInfo(KSycoca::absoluteFilePath()).lastModified();
    QCOMPARE(againTimestamp, newTimestamp); // same mtime, it didn't get rebuilt

    // Ensure we don't pollute the other tests, with our dir in the future.
#ifdef Q_OS_UNIX
    utbuf.modtime = tp.tv_sec;
    QCOMPARE(utime(QFile::encodeName(path).constData(), &utbuf), 0);
    qDebug("Time changed back for %s", qPrintable(path));
    qDebug() << QDateTime::currentDateTime() << QFileInfo(path).lastModified();
#endif
}

void KSycocaTest::dirTimestampShouldBeCheckedRecursively()
{
#ifndef Q_OS_UNIX
    QSKIP("This test requires utime");
#endif
    const QDateTime oldTimestamp = QFileInfo(KSycoca::absoluteFilePath()).lastModified();

    const QString path = menusDir() + QLatin1String("/fakeSubserviceDirectory");

    // ### use QFile::setFileTime when it lands in Qt...
#ifdef Q_OS_UNIX
    struct timeval tp;
    gettimeofday(&tp, nullptr);
    struct utimbuf utbuf;
    utbuf.actime = tp.tv_sec;
    utbuf.modtime = tp.tv_sec + 60; // 60 second in the future
    QCOMPARE(utime(QFile::encodeName(path).constData(), &utbuf), 0);
    qDebug("Time changed for %s", qPrintable(path));
    qDebug() << QDateTime::currentDateTime() << QFileInfo(path).lastModified();
#endif

    ksycoca_ms_between_checks = 0;
    QTest::qWait(s_waitDelay);

    qDebug() << "Waited 1s, calling ensureCacheValid (should rebuild)";
    KSycoca::self()->ensureCacheValid();
    const QDateTime newTimestamp = QFileInfo(KSycoca::absoluteFilePath()).lastModified();
    if (newTimestamp <= oldTimestamp) {
        qWarning() << "oldTimestamp=" << oldTimestamp << "newTimestamp=" << newTimestamp;
    }
    QVERIFY(newTimestamp > oldTimestamp);

    QTest::qWait(s_waitDelay);

    qDebug() << "Waited 1s, calling ensureCacheValid (should not rebuild)";
    KSycoca::self()->ensureCacheValid();
    const QDateTime againTimestamp = QFileInfo(KSycoca::absoluteFilePath()).lastModified();
    QCOMPARE(againTimestamp, newTimestamp); // same mtime, it didn't get rebuilt

    // Ensure we don't pollute the other tests
    QDir(path).removeRecursively();
}

void KSycocaTest::recursiveCheckShouldIgnoreLinksGoingUp()
{
#ifndef Q_OS_UNIX
    QSKIP("This test requires symlinks and utime");
#endif
    ksycoca_ms_between_checks = 0;
    const QString link = menusDir() + QLatin1String("/linkGoingUp");
    QVERIFY(QFile::link("..", link));
    QTest::qWait(s_waitDelay);
    KSycoca::self()->ensureCacheValid();
    QVERIFY2(QFile::exists(KSycoca::absoluteFilePath()), qPrintable(KSycoca::absoluteFilePath()));
    const QDateTime oldTimestamp = QFileInfo(KSycoca::absoluteFilePath()).lastModified();
    QVERIFY(oldTimestamp.isValid());

    const QString path = QFileInfo(menusDir()).absolutePath(); // the parent of the menus dir

    // ### use QFile::setFileTime when it lands in Qt...
#ifdef Q_OS_UNIX
    struct timeval tp;
    gettimeofday(&tp, nullptr);
    struct utimbuf utbuf;
    utbuf.actime = tp.tv_sec;
    utbuf.modtime = tp.tv_sec + 60; // 60 second in the future
    QCOMPARE(utime(QFile::encodeName(path).constData(), &utbuf), 0);
    qDebug("Time changed for %s", qPrintable(path));
    qDebug() << QDateTime::currentDateTime() << QFileInfo(path).lastModified();
#endif

    ksycoca_ms_between_checks = 0;
    QTest::qWait(s_waitDelay);

    qDebug() << "Waited 1s, calling ensureCacheValid (should not rebuild)";
    KSycoca::self()->ensureCacheValid();
    const QDateTime againTimestamp = QFileInfo(KSycoca::absoluteFilePath()).lastModified();
    QCOMPARE(againTimestamp, oldTimestamp); // same mtime, it didn't get rebuilt

    // Ensure we don't pollute the other tests
    QFile(link).remove();
}


void KSycocaTest::runKBuildSycoca(const QProcessEnvironment &environment, bool global)
{
    QProcess proc;
    const QString kbuildsycoca = QStringLiteral(KBUILDSYCOCAEXE);
    QVERIFY(!kbuildsycoca.isEmpty());
    QStringList args;
    args << QStringLiteral("--testmode");
    if (global) {
        args << QStringLiteral("--global");
    }
    proc.setProcessChannelMode(QProcess::ForwardedChannels);
    proc.start(kbuildsycoca, args);
    proc.setProcessEnvironment(environment);

    proc.waitForFinished();
    QCOMPARE(proc.exitStatus(), QProcess::NormalExit);
}

void KSycocaTest::testAllResourceDirs()
{
    // Dirs that exist and dirs that don't exist, should both be in allResourceDirs().
    const QStringList dirs = KSycoca::self()->allResourceDirs();
    QVERIFY2(dirs.contains(servicesDir()), qPrintable(dirs.join(',')));
    QVERIFY2(dirs.contains(serviceTypesDir()), qPrintable(dirs.join(',')));
}

void KSycocaTest::testDeletingSycoca()
{
    // Mostly the same as ensureCacheValidShouldCreateDB, but KSycoca::self() already exists
    // So this is a check that deleting sycoca doesn't make apps crash (bug 343618).
    QFile::remove(KSycoca::absoluteFilePath());
    ksycoca_ms_between_checks = 0;
    QVERIFY(KServiceType::serviceType(QStringLiteral("FakeGlobalServiceType")));
    QVERIFY(QFile::exists(KSycoca::absoluteFilePath()));
}

void KSycocaTest::testGlobalSycoca()
{
    // No local DB
    QFile::remove(KSycoca::absoluteFilePath());

    // Build global DB
    // We could do it in-process, but let's check what a sysadmin would do: run kbuildsycoca5 --global
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_DATA_DIRS"), m_tempDir.path());
    runKBuildSycoca(env, true /*global*/);
    QVERIFY(QFile::exists(KSycoca::absoluteFilePath(KSycoca::GlobalDatabase)));

    KSycoca::self()->ensureCacheValid();
    QVERIFY(!QFile::exists(KSycoca::absoluteFilePath()));

    // Now create a local file, after a 1s delay, until QDateTime includes ms...
    QTest::qWait(s_waitDelay);
    KDesktopFile file(serviceTypesDir() + "/fakeLocalServiceType.desktop");
    KConfigGroup group = file.desktopGroup();
    group.writeEntry("Comment", "Fake Local ServiceType");
    group.writeEntry("Type", "ServiceType");
    group.writeEntry("X-KDE-ServiceType", "FakeLocalServiceType");
    file.sync();
    //qDebug() << "created" << serviceTypesDir() + "/fakeLocalServiceType.desktop at" << QDateTime::currentMSecsSinceEpoch();

    // Using ksycoca should now build a local one
    ksycoca_ms_between_checks = 0;
    QVERIFY(KServiceType::serviceType(QStringLiteral("FakeLocalServiceType")));
    QVERIFY(QFile::exists(KSycoca::absoluteFilePath()));
}

void KSycocaTest::testNonReadableSycoca()
{
    // Lose readability (to simulate e.g. owned by root)
    QFile(KSycoca::absoluteFilePath()).setPermissions(QFile::WriteOwner);
    ksycoca_ms_between_checks = 0;
    KBuildSycoca builder;
    QVERIFY(builder.recreate());
    QVERIFY(KServiceType::serviceType(QStringLiteral("FakeGlobalServiceType")));

    // cleanup
    QFile::remove(KSycoca::absoluteFilePath());
}

#include "ksycocatest.moc"
