#include "sync/vv.h"

#include <QStringList>

namespace an {

VersionVector VersionVector::parse(const QString& text)
{
    VersionVector vv;
    for (const QString& part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const int colon = part.indexOf(QLatin1Char(':'));
        if (colon <= 0) continue;
        const QString dev = part.left(colon).trimmed();
        bool ok = false;
        const quint64 n = part.mid(colon + 1).trimmed().toULongLong(&ok);
        if (!dev.isEmpty() && ok) vv.entries_.insert(dev, n);
    }
    return vv;
}

QString VersionVector::toString() const
{
    QStringList parts;
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it)
        parts.append(QStringLiteral("%1:%2").arg(it.key()).arg(it.value()));
    return parts.join(QLatin1Char(','));
}

void VersionVector::setCounter(const QString& device, quint64 value)
{
    if (device.isEmpty()) return;
    entries_.insert(device, value);
}

void VersionVector::bump(const QString& device, quint64 stamp)
{
    if (device.isEmpty()) return;
    const quint64 current = entries_.value(device, 0);
    entries_.insert(device, stamp > current ? stamp : current + 1);
}

void VersionVector::join(const VersionVector& other)
{
    for (auto it = other.entries_.constBegin(); it != other.entries_.constEnd(); ++it) {
        const quint64 mine = entries_.value(it.key(), 0);
        if (it.value() > mine) entries_.insert(it.key(), it.value());
    }
}

bool VersionVector::dominates(const VersionVector& other) const
{
    bool strictlyNewer = false;
    for (auto it = other.entries_.constBegin(); it != other.entries_.constEnd(); ++it) {
        const quint64 mine = entries_.value(it.key(), 0);
        if (mine < it.value()) return false;
        if (mine > it.value()) strictlyNewer = true;
    }
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it) {
        if (!other.entries_.contains(it.key()) && it.value() > 0) strictlyNewer = true;
    }
    return strictlyNewer;
}

bool VersionVector::equal(const VersionVector& other) const
{
    return entries_ == other.entries_;
}

bool VersionVector::concurrentWith(const VersionVector& other) const
{
    if (equal(other)) return false;
    return !dominates(other) && !other.dominates(*this);
}

}  // namespace an
