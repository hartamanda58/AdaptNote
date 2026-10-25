#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QVector>

namespace an {

// Minimal ZIP writer/reader (deflate or store). Needed for .xmind and .apkg,
// which are both plain ZIP containers. No external dependency beyond zlib.
class ZipWriter {
public:
    explicit ZipWriter(const QString& path);
    ~ZipWriter();

    bool isOpen() const { return file_.isOpen(); }
    bool addFile(const QString& name, const QByteArray& data);
    bool addFileFromPath(const QString& name, const QString& localPath);
    bool close(QString* error = nullptr);

private:
    struct Entry {
        QString name;
        quint32 crc = 0;
        quint32 compSize = 0;
        quint32 uncompSize = 0;
        quint32 offset = 0;
        quint16 method = 0;
        quint16 dosTime = 0;
        quint16 dosDate = 0;
    };

    bool writeLocalHeader(const Entry& e);
    bool writeCentralDirectory();

    QFile file_;
    QVector<Entry> entries_;
    bool closed_ = false;
};

class ZipReader {
public:
    explicit ZipReader(const QString& path);
    bool isOpen() const { return ok_; }
    QStringList names() const { return names_; }
    bool contains(const QString& name) const { return names_.contains(name); }
    QByteArray read(const QString& name) const;

private:
    struct Entry {
        QString name;
        quint32 compSize = 0;
        quint32 uncompSize = 0;
        quint32 offset = 0;
        quint16 method = 0;
    };

    QByteArray data_;
    QVector<Entry> entries_;
    QStringList names_;
    bool ok_ = false;
};

}  // namespace an
