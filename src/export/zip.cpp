#include "export/zip.h"

#include <QDateTime>
#include <QFileInfo>

#include <zlib.h>

#include <cstring>

namespace an {
namespace {

constexpr quint32 kLocalSig = 0x04034b50;
constexpr quint32 kCentralSig = 0x02014b50;
constexpr quint32 kEocdSig = 0x06054b50;

void put16(QByteArray& out, quint16 v)
{
    out.append(static_cast<char>(v & 0xff));
    out.append(static_cast<char>((v >> 8) & 0xff));
}

void put32(QByteArray& out, quint32 v)
{
    out.append(static_cast<char>(v & 0xff));
    out.append(static_cast<char>((v >> 8) & 0xff));
    out.append(static_cast<char>((v >> 16) & 0xff));
    out.append(static_cast<char>((v >> 24) & 0xff));
}

quint16 get16(const QByteArray& a, int off)
{
    return static_cast<quint16>(static_cast<uchar>(a.at(off)) |
                                (static_cast<uchar>(a.at(off + 1)) << 8));
}

quint32 get32(const QByteArray& a, int off)
{
    return static_cast<quint32>(static_cast<uchar>(a.at(off))) |
           (static_cast<quint32>(static_cast<uchar>(a.at(off + 1))) << 8) |
           (static_cast<quint32>(static_cast<uchar>(a.at(off + 2))) << 16) |
           (static_cast<quint32>(static_cast<uchar>(a.at(off + 3))) << 24);
}

QByteArray deflateRaw(const QByteArray& in)
{
    z_stream s;
    std::memset(&s, 0, sizeof(s));
    if (deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    QByteArray out;
    out.resize(static_cast<int>(in.size() + in.size() / 2 + 64));
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    s.avail_in = static_cast<uInt>(in.size());
    s.next_out = reinterpret_cast<Bytef*>(out.data());
    s.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&s, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&s);
        return {};
    }
    out.resize(static_cast<int>(s.total_out));
    deflateEnd(&s);
    return out;
}

QByteArray inflateRaw(const QByteArray& in, quint32 expected)
{
    z_stream s;
    std::memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return {};
    QByteArray out;
    out.resize(static_cast<int>(expected) + 16);
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    s.avail_in = static_cast<uInt>(in.size());
    s.next_out = reinterpret_cast<Bytef*>(out.data());
    s.avail_out = static_cast<uInt>(out.size());
    const int rc = inflate(&s, Z_FINISH);
    if (rc != Z_STREAM_END) {
        inflateEnd(&s);
        return {};
    }
    out.resize(static_cast<int>(s.total_out));
    inflateEnd(&s);
    return out;
}

void dosTimestamp(quint16* dosTime, quint16* dosDate)
{
    const QDateTime now = QDateTime::currentDateTime();
    const QDate d = now.date();
    const QTime t = now.time();
    *dosTime = static_cast<quint16>((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2));
    *dosDate = static_cast<quint16>(((d.year() - 1980) << 9) | (d.month() << 5) | d.day());
}

}  // namespace

// ------------------------------------------------------------------- writer

ZipWriter::ZipWriter(const QString& path)
{
    file_.setFileName(path);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
}

ZipWriter::~ZipWriter()
{
    if (file_.isOpen()) file_.close();
}

bool ZipWriter::addFile(const QString& name, const QByteArray& data)
{
    if (!file_.isOpen() || closed_) return false;

    Entry e;
    e.name = name;
    e.uncompSize = static_cast<quint32>(data.size());
    e.crc = static_cast<quint32>(crc32(0, reinterpret_cast<const Bytef*>(data.constData()),
                                       static_cast<uInt>(data.size())));
    dosTimestamp(&e.dosTime, &e.dosDate);

    QByteArray payload = deflateRaw(data);
    if (payload.isEmpty() || payload.size() >= data.size()) {
        payload = data;
        e.method = 0;
    } else {
        e.method = 8;
    }
    e.compSize = static_cast<quint32>(payload.size());
    e.offset = static_cast<quint32>(file_.pos());

    if (!writeLocalHeader(e)) return false;
    if (file_.write(payload) != payload.size()) return false;

    entries_.append(e);
    return true;
}

bool ZipWriter::addFileFromPath(const QString& name, const QString& localPath)
{
    QFile f(localPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return addFile(name, f.readAll());
}

bool ZipWriter::writeLocalHeader(const Entry& e)
{
    const QByteArray nameBytes = e.name.toUtf8();
    QByteArray h;
    put32(h, kLocalSig);
    put16(h, 20);            // version needed
    put16(h, 0x0800);        // UTF-8 names
    put16(h, e.method);
    put16(h, e.dosTime);
    put16(h, e.dosDate);
    put32(h, e.crc);
    put32(h, e.compSize);
    put32(h, e.uncompSize);
    put16(h, static_cast<quint16>(nameBytes.size()));
    put16(h, 0);             // extra length
    h.append(nameBytes);
    return file_.write(h) == h.size();
}

bool ZipWriter::writeCentralDirectory()
{
    const qint64 cdOffset = file_.pos();
    for (const Entry& e : entries_) {
        const QByteArray nameBytes = e.name.toUtf8();
        QByteArray h;
        put32(h, kCentralSig);
        put16(h, 20);        // version made by
        put16(h, 20);        // version needed
        put16(h, 0x0800);
        put16(h, e.method);
        put16(h, e.dosTime);
        put16(h, e.dosDate);
        put32(h, e.crc);
        put32(h, e.compSize);
        put32(h, e.uncompSize);
        put16(h, static_cast<quint16>(nameBytes.size()));
        put16(h, 0);         // extra
        put16(h, 0);         // comment
        put16(h, 0);         // disk
        put16(h, 0);         // internal attrs
        put32(h, 0);         // external attrs
        put32(h, e.offset);
        h.append(nameBytes);
        if (file_.write(h) != h.size()) return false;
    }
    const qint64 cdSize = file_.pos() - cdOffset;

    QByteArray eocd;
    put32(eocd, kEocdSig);
    put16(eocd, 0);
    put16(eocd, 0);
    put16(eocd, static_cast<quint16>(entries_.size()));
    put16(eocd, static_cast<quint16>(entries_.size()));
    put32(eocd, static_cast<quint32>(cdSize));
    put32(eocd, static_cast<quint32>(cdOffset));
    put16(eocd, 0);
    return file_.write(eocd) == eocd.size();
}

bool ZipWriter::close(QString* error)
{
    if (!file_.isOpen()) {
        if (error) *error = QStringLiteral("zip file not open");
        return false;
    }
    if (closed_) return true;
    closed_ = true;
    if (!writeCentralDirectory()) {
        if (error) *error = QStringLiteral("cannot write central directory");
        return false;
    }
    file_.flush();
    file_.close();
    return true;
}

// ------------------------------------------------------------------- reader

ZipReader::ZipReader(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    data_ = f.readAll();

    // locate end of central directory
    int eocd = -1;
    for (int i = data_.size() - 22; i >= 0 && i >= data_.size() - 22 - 65535; --i) {
        if (get32(data_, i) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return;

    const quint16 count = get16(data_, eocd + 10);
    quint32 off = get32(data_, eocd + 16);

    for (quint16 i = 0; i < count; ++i) {
        if (off + 46 > static_cast<quint32>(data_.size()) || get32(data_, static_cast<int>(off)) != kCentralSig)
            return;
        Entry e;
        e.method = get16(data_, static_cast<int>(off) + 10);
        e.compSize = get32(data_, static_cast<int>(off) + 20);
        e.uncompSize = get32(data_, static_cast<int>(off) + 24);
        const quint16 nameLen = get16(data_, static_cast<int>(off) + 28);
        const quint16 extraLen = get16(data_, static_cast<int>(off) + 30);
        const quint16 commentLen = get16(data_, static_cast<int>(off) + 32);
        e.offset = get32(data_, static_cast<int>(off) + 42);
        const int nameOff = static_cast<int>(off) + 46;
        if (nameOff + nameLen > data_.size()) return;
        e.name = QString::fromUtf8(data_.mid(nameOff, nameLen));
        entries_.append(e);
        names_.append(e.name);
        off += 46 + nameLen + extraLen + commentLen;
    }
    ok_ = true;
}

QByteArray ZipReader::read(const QString& name) const
{
    for (const Entry& e : entries_) {
        if (e.name != name) continue;
        const int base = static_cast<int>(e.offset);
        if (base + 30 > data_.size() || get32(data_, base) != kLocalSig) return {};
        const quint16 nameLen = get16(data_, base + 26);
        const quint16 extraLen = get16(data_, base + 28);
        const int dataOff = base + 30 + nameLen + extraLen;
        if (dataOff + static_cast<int>(e.compSize) > data_.size()) return {};
        const QByteArray raw = data_.mid(dataOff, static_cast<int>(e.compSize));
        if (e.method == 0) return raw;
        if (e.method == 8) return inflateRaw(raw, e.uncompSize);
        return {};
    }
    return {};
}

}  // namespace an
