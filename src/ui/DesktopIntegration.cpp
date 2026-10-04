#include "ui/DesktopIntegration.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

namespace scvp::ui::desktop {
namespace {

constexpr const char* kId = "scvp.desktop";

QString run(const QString& program, const QStringList& args, bool* ok = nullptr, QString* err = nullptr)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));  // parsed output: untranslated
    p.setProcessEnvironment(env);
    p.start(program, args);
    const bool finished = p.waitForFinished(5000);
    const bool good = finished && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    if (ok)
        *ok = good;
    if (err && !good)
        *err = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

// "Key=value" of the [Desktop Entry] group (untranslated keys only).
QString entryValue(const QString& path, const QString& key)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream in(&f);
    bool inEntry = false;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith(QLatin1Char('[')))
            inEntry = line == QLatin1String("[Desktop Entry]");
        else if (inEntry && line.startsWith(key + QLatin1Char('=')))
            return line.mid(key.size() + 1);
    }
    return {};
}

} // namespace

bool supported()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    return !QStandardPaths::findExecutable(QStringLiteral("xdg-mime")).isEmpty();
#else
    return false;
#endif
}

QString installedEntry() { return QStandardPaths::locate(QStandardPaths::ApplicationsLocation, QString::fromLatin1(kId)); }

QStringList handledTypes()
{
    const QString path = installedEntry();
    return path.isEmpty() ? QStringList() : entryValue(path, QStringLiteral("MimeType")).split(QLatin1Char(';'), Qt::SkipEmptyParts);
}

// As the file manager resolves it: GIO follows type aliases, "xdg-mime query" does not.
QString defaultFor(const QString& mimeType)
{
    if (!QStandardPaths::findExecutable(QStringLiteral("gio")).isEmpty()) {
        const QString out = run(QStringLiteral("gio"), {QStringLiteral("mime"), mimeType});
        const QString marker = QStringLiteral("Default application for ");
        for (const QString& line : out.split(QLatin1Char('\n')))
            if (line.startsWith(marker) && line.contains(QLatin1String(": ")))
                return line.mid(line.lastIndexOf(QLatin1String(": ")) + 2).trimmed();
        return {};
    }
    return run(QStringLiteral("xdg-mime"), {QStringLiteral("query"), QStringLiteral("default"), mimeType});
}

bool isDefault() { return defaultFor(QStringLiteral("video/mp4")) == QLatin1String(kId); }

bool setAsDefault(QString* error)
{
    const QStringList types = handledTypes();
    if (types.isEmpty()) {
        if (error)
            *error = QStringLiteral("SCVP is not installed for this user (run install.sh from the SCVP folder).");
        return false;
    }
    bool ok = false;
    run(QStringLiteral("xdg-mime"), QStringList{QStringLiteral("default"), QString::fromLatin1(kId)} + types, &ok, error);
    return ok;
}

QString applicationName(const QString& desktopId)
{
    if (desktopId.isEmpty())
        return {};
    const QString path = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, desktopId);
    const QString name = path.isEmpty() ? QString() : entryValue(path, QStringLiteral("Name"));
    return name.isEmpty() ? desktopId : name;
}

} // namespace scvp::ui::desktop
