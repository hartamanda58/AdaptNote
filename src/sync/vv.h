#pragma once

#include <QMap>
#include <QString>

namespace an {

// Version vector: device id -> logical counter (millisecond timestamps).
// Used to decide, for two versions of the same note, whether one supersedes the
// other or whether they are concurrent (= a real conflict to surface to the user).
class VersionVector {
public:
    VersionVector() = default;

    static VersionVector parse(const QString& text);   // "devA:1700000000000,devB:1700000000123"
    QString toString() const;

    bool isEmpty() const { return entries_.isEmpty(); }
    bool contains(const QString& device) const { return entries_.contains(device); }
    quint64 counter(const QString& device) const { return entries_.value(device, 0); }

    void setCounter(const QString& device, quint64 value);
    void bump(const QString& device, quint64 stamp);   // keep the max(old, stamp)
    void join(const VersionVector& other);             // element-wise max

    // A dominates B when A is at least as new on every device and strictly newer somewhere.
    bool dominates(const VersionVector& other) const;
    bool equal(const VersionVector& other) const;
    // Neither side dominates: both devices changed the note since they last agreed.
    bool concurrentWith(const VersionVector& other) const;

    QStringList devices() const { return entries_.keys(); }

private:
    QMap<QString, quint64> entries_;
};

}  // namespace an
