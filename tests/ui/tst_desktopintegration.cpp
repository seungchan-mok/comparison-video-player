#include "ui/DesktopIntegration.h"

#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace scvp::ui;

// Runs against a private XDG home so the user's real default applications are never touched.
class TstDesktopIntegration : public QObject {
    Q_OBJECT

    QTemporaryDir m_home;

private slots:
    void initTestCase()
    {
        if (!desktop::supported())
            QSKIP("no xdg-mime");
        QVERIFY(m_home.isValid());
        qputenv("HOME", m_home.path().toUtf8());
        qputenv("XDG_DATA_HOME", (m_home.path() + QStringLiteral("/data")).toUtf8());
        qputenv("XDG_CONFIG_HOME", (m_home.path() + QStringLiteral("/config")).toUtf8());
        QStandardPaths::setTestModeEnabled(false);
        QVERIFY(QDir().mkpath(m_home.path() + QStringLiteral("/data/applications")));
        QVERIFY(QDir().mkpath(m_home.path() + QStringLiteral("/config")));
    }

    void notInstalledYet()
    {
        QVERIFY(desktop::installedEntry().isEmpty());
        QVERIFY(desktop::handledTypes().isEmpty());
        QString err;
        QVERIFY(!desktop::setAsDefault(&err));
        QVERIFY(err.contains(QLatin1String("install.sh")));
    }

    void becomesTheDefault()
    {
        QFile src(QStringLiteral(SCVP_SOURCE_DIR "/packaging/scvp.desktop"));
        QVERIFY(src.open(QIODevice::ReadOnly));
        QByteArray entry = src.readAll();
        // GIO ignores entries whose program it cannot find; the test machine need not have scvp.
        entry.replace("TryExec=scvp\n", "");
        entry.replace("Exec=scvp %F", "Exec=/bin/true %F");
        QFile dst(m_home.path() + QStringLiteral("/data/applications/scvp.desktop"));
        QVERIFY(dst.open(QIODevice::WriteOnly));
        dst.write(entry);
        dst.close();
        QVERIFY(!desktop::installedEntry().isEmpty());
        const QStringList types = desktop::handledTypes();
        QVERIFY(types.contains(QStringLiteral("video/mp4")));
        QVERIFY(types.contains(QStringLiteral("video/x-raw-yuv")));
        QCOMPARE(desktop::applicationName(QStringLiteral("scvp.desktop")), QStringLiteral("SCVP"));

        QString err;
        QVERIFY2(desktop::setAsDefault(&err), qPrintable(err));
        QFile list(m_home.path() + QStringLiteral("/config/mimeapps.list"));
        QVERIFY(list.open(QIODevice::ReadOnly));
        const QByteArray written = list.readAll();
        QVERIFY(written.contains("video/mp4=scvp.desktop"));
        QVERIFY(written.contains("video/x-matroska=scvp.desktop"));
        QVERIFY(desktop::isDefault());
        QCOMPARE(desktop::defaultFor(QStringLiteral("video/webm")), QStringLiteral("scvp.desktop"));
    }
};

QTEST_GUILESS_MAIN(TstDesktopIntegration)
#include "tst_desktopintegration.moc"
