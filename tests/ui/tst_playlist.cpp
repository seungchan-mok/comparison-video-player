#include "ui/Playlist.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace scvp::ui;

namespace {

// A folder under `root` holding empty files at these paths (pairing reads names only).
QString folder(const QTemporaryDir& root, const QString& name, const QStringList& files)
{
    QDir(root.path()).mkpath(name);
    const QString dir = QDir(root.path()).filePath(name);
    for (const QString& f : files) {
        QDir(dir).mkpath(QFileInfo(f).path());
        QFile file(QDir(dir).filePath(f));
        file.open(QIODevice::WriteOnly);
    }
    return dir;
}

QStringList names(const Playlist& p)
{
    QStringList n;
    for (const PlaylistItem& i : p.items())
        n << i.name;
    return n;
}

QStringList fileNames(const Playlist& p, int slot)
{
    QStringList n;
    for (const PlaylistItem& i : p.items())
        n << (i.paths[size_t(slot)].isEmpty() ? QStringLiteral("-") : QFileInfo(i.paths[size_t(slot)]).fileName());
    return n;
}

} // namespace

class TstPlaylist : public QObject {
    Q_OBJECT

private slots:
    // Pairs by the name without the extension; a video only one folder has is an item with the
    // other side empty. Natural order: c2 before c10.
    void pairsByNameWithoutExtension()
    {
        QTemporaryDir root;
        Playlist p;
        p.setFolder(0, folder(root, QStringLiteral("a"), {QStringLiteral("c10.mp4"), QStringLiteral("c2.mp4"), QStringLiteral("only_a.mov")}));
        // (Upper-case extension: also a video. Names differing only in case would collide on
        // macOS and Windows file systems.)
        p.setFolder(1, folder(root, QStringLiteral("b"), {QStringLiteral("c2.MKV"), QStringLiteral("c10.mkv"), QStringLiteral("only_b.mp4")}));
        QCOMPARE(names(p), (QStringList{QStringLiteral("c2"), QStringLiteral("c10"), QStringLiteral("only_a"), QStringLiteral("only_b")}));
        QCOMPARE(fileNames(p, 0),
                 (QStringList{QStringLiteral("c2.mp4"), QStringLiteral("c10.mp4"), QStringLiteral("only_a.mov"), QStringLiteral("-")}));
        QCOMPARE(fileNames(p, 1),
                 (QStringList{QStringLiteral("c2.MKV"), QStringLiteral("c10.mkv"), QStringLiteral("-"), QStringLiteral("only_b.mp4")}));
        QVERIFY(QFileInfo(p.items()[0].paths[0]).isAbsolute());
        QCOMPARE(p.current(), 0);
    }

    // Two videos of one name in a folder (x.mkv and x.mp4) pair up in order: none is left out.
    void sameNameTwiceKeepsBoth()
    {
        QTemporaryDir root;
        Playlist p;
        p.setFolder(0, folder(root, QStringLiteral("a"), {QStringLiteral("x.mp4"), QStringLiteral("x.mkv")}));
        p.setFolder(1, folder(root, QStringLiteral("b"), {QStringLiteral("x.mov")}));
        QCOMPARE(names(p), (QStringList{QStringLiteral("x"), QStringLiteral("x")}));
        QCOMPARE(fileNames(p, 0), (QStringList{QStringLiteral("x.mkv"), QStringLiteral("x.mp4")}));
        QCOMPARE(fileNames(p, 1), (QStringList{QStringLiteral("x.mov"), QStringLiteral("-")}));
    }

    // Only videos: not images, other files, hidden files or subfolders.
    void picksUpVideosOnly()
    {
        QTemporaryDir root;
        const QString a = folder(root, QStringLiteral("a"), {QStringLiteral("v.mp4"), QStringLiteral("notes.txt"), QStringLiteral("f_0001.png"),
                                                            QStringLiteral(".hidden.mp4"), QStringLiteral("raw_320x240_nv12.yuv")});
        QDir(a).mkpath(QStringLiteral("sub.mp4"));
        Playlist p;
        p.setFolder(0, a);
        QCOMPARE(names(p), (QStringList{QStringLiteral("raw_320x240_nv12"), QStringLiteral("v")}));
        QVERIFY(folderHasVideos(a));
        QVERIFY(!folderHasVideos(folder(root, QStringLiteral("images"), {QStringLiteral("f_0001.png"), QStringLiteral("f_0002.png")})));
        QVERIFY(!folderHasVideos(QDir(a).filePath(QStringLiteral("v.mp4"))));  // a file is not a folder
        QVERIFY(!folderHasVideos(QDir(root.path()).filePath(QStringLiteral("missing"))));
    }

    // Subfolders count: videos pair by their path below the folder without the extension, in
    // natural order folder by folder (day2 before day10); hidden folders are skipped.
    void subfoldersPairByPath()
    {
        QTemporaryDir root;
        Playlist p;
        p.setFolder(0, folder(root, QStringLiteral("a"), {QStringLiteral("clip.mp4"), QStringLiteral("day1/clip.mp4"), QStringLiteral("day10/x.mp4"),
                                                          QStringLiteral("day2/x.mp4"), QStringLiteral("day2/deep/y.mp4"), QStringLiteral(".git/z.mp4"),
                                                          QStringLiteral("imgs/f_0001.png")}));
        p.setFolder(1, folder(root, QStringLiteral("b"), {QStringLiteral("day1/clip.mkv"), QStringLiteral("day2/x.mkv"), QStringLiteral("clip.mov")}));
        QCOMPARE(names(p), (QStringList{QStringLiteral("clip"), QStringLiteral("day1/clip"), QStringLiteral("day2/deep/y"), QStringLiteral("day2/x"),
                                        QStringLiteral("day10/x")}));
        QCOMPARE(fileNames(p, 0), (QStringList{QStringLiteral("clip.mp4"), QStringLiteral("clip.mp4"), QStringLiteral("y.mp4"), QStringLiteral("x.mp4"),
                                               QStringLiteral("x.mp4")}));
        QCOMPARE(fileNames(p, 1), (QStringList{QStringLiteral("clip.mov"), QStringLiteral("clip.mkv"), QStringLiteral("-"), QStringLiteral("x.mkv"),
                                               QStringLiteral("-")}));
        QCOMPARE(QDir(root.path()).relativeFilePath(p.items()[1].paths[1]), QStringLiteral("b/day1/clip.mkv"));
        // Images on top, a video below: a playlist.
        QVERIFY(folderHasVideos(folder(root, QStringLiteral("shots"), {QStringLiteral("f_0001.png"), QStringLiteral("sub/v.mp4")})));
        QVERIFY(!folderHasVideos(folder(root, QStringLiteral("hidden"), {QStringLiteral("f_0001.png"), QStringLiteral(".cache/v.mp4")})));
    }

    // Pairing again (the second folder opened, one dropped) keeps the item being watched.
    void currentItemStaysWhenPairedAgain()
    {
        QTemporaryDir root;
        Playlist p;
        p.setFolder(0, folder(root, QStringLiteral("a"), {QStringLiteral("1.mp4"), QStringLiteral("2.mp4"), QStringLiteral("3.mp4")}));
        p.setCurrent(2);
        p.setFolder(1, folder(root, QStringLiteral("b"), {QStringLiteral("0.mp4"), QStringLiteral("3.mp4")}));
        QCOMPARE(names(p), (QStringList{QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3")}));
        QCOMPARE(p.current(), 3);
        p.setFolder(0, folder(root, QStringLiteral("c"), {QStringLiteral("9.mp4")}));  // no "3" in A any more, but in B
        QCOMPARE(p.items()[size_t(p.current())].name, QStringLiteral("3"));
        p.setFolder(1, QString());
        QCOMPARE(names(p), QStringList{QStringLiteral("9")});
        QCOMPARE(p.current(), 0);  // gone: the first
        p.setFolder(0, QString());
        QVERIFY(!p.active());
        QCOMPARE(p.current(), -1);
    }

    void swapSwapsFoldersAndPaths()
    {
        QTemporaryDir root;
        Playlist p;
        const QString a = folder(root, QStringLiteral("a"), {QStringLiteral("x.mp4")});
        p.setFolder(0, a);
        p.swapSlots();
        QCOMPARE(p.folder(1), a);
        QVERIFY(p.folder(0).isEmpty());
        QCOMPARE(fileNames(p, 1), QStringList{QStringLiteral("x.mp4")});
        QCOMPARE(fileNames(p, 0), QStringList{QStringLiteral("-")});
    }
};

QTEST_MAIN(TstPlaylist)
#include "tst_playlist.moc"
