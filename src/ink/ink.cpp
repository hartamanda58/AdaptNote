#include "ink/ink.h"

#include <QLineF>
#include <QtMath>

#include <algorithm>

namespace an {
namespace ink {
namespace {

double distanceToSegment(const QPointF& p, const QPointF& a, const QPointF& b)
{
    const QPointF ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 <= 1e-12) return QLineF(p, a).length();
    double t = ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2;
    t = qBound(0.0, t, 1.0);
    const QPointF proj = a + ab * t;
    return QLineF(p, proj).length();
}

void rdp(const QVector<InkPoint>& pts, int first, int last, double epsilon, QVector<int>* keep)
{
    double maxDist = 0;
    int index = -1;
    for (int i = first + 1; i < last; ++i) {
        const double d = distanceToSegment(pts.at(i).pos, pts.at(first).pos, pts.at(last).pos);
        if (d > maxDist) {
            maxDist = d;
            index = i;
        }
    }
    if (index >= 0 && maxDist > epsilon) {
        rdp(pts, first, index, epsilon, keep);
        keep->append(index);
        rdp(pts, index, last, epsilon, keep);
    }
}

}  // namespace

QVector<InkPoint> simplify(const QVector<InkPoint>& points, double epsilon)
{
    if (points.size() < 3 || epsilon <= 0) return points;

    QVector<int> keep{0};
    rdp(points, 0, points.size() - 1, epsilon, &keep);
    keep.append(points.size() - 1);
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());

    QVector<InkPoint> out;
    out.reserve(keep.size());
    for (int i : keep) out.append(points.at(i));
    return out;
}

QPainterPath smoothPath(const QVector<InkPoint>& points, double baseWidth)
{
    QPainterPath path;
    if (points.isEmpty()) return path;

    const double minWidth = qMax(0.3, baseWidth * 0.55);

    if (points.size() == 1) {
        const double w = qMax(minWidth, baseWidth * points.first().pressure);
        path.addEllipse(points.first().pos, w * 0.5, w * 0.5);
        return path;
    }

    path.moveTo(points.first().pos);
    if (points.size() == 2) {
        path.lineTo(points.last().pos);
        return path;
    }

    // Catmull-Rom → cubic Bézier, per segment.
    for (int i = 0; i < points.size() - 1; ++i) {
        const QPointF p0 = points.at(qMax(0, i - 1)).pos;
        const QPointF p1 = points.at(i).pos;
        const QPointF p2 = points.at(i + 1).pos;
        const QPointF p3 = points.at(qMin(points.size() - 1, i + 2)).pos;

        const QPointF c1 = p1 + (p2 - p0) / 6.0;
        const QPointF c2 = p2 - (p3 - p1) / 6.0;
        path.cubicTo(c1, c2, p2);
    }
    Q_UNUSED(minWidth);
    return path;
}

QRectF bounds(const QVector<InkPoint>& points, double baseWidth)
{
    if (points.isEmpty()) return {};
    // NOTE: do not use QRectF::united here — zero-size rects are "null" for Qt and
    // united() would return the other (also zero-size) rect instead of the union.
    double minX = points.first().pos.x(), maxX = minX;
    double minY = points.first().pos.y(), maxY = minY;
    for (const InkPoint& p : points) {
        minX = qMin(minX, p.pos.x());
        maxX = qMax(maxX, p.pos.x());
        minY = qMin(minY, p.pos.y());
        maxY = qMax(maxY, p.pos.y());
    }
    const double pad = qMax(1.0, baseWidth);
    return QRectF(QPointF(minX, minY), QPointF(maxX, maxY)).adjusted(-pad, -pad, pad, pad);
}

double tiltFromDegrees(double xTiltDeg, double yTiltDeg)
{
    const double magnitude = std::sqrt(xTiltDeg * xTiltDeg + yTiltDeg * yTiltDeg);
    return qBound(0.0, magnitude / 60.0, 1.0);   // Qt reports up to +-60 degrees
}

double widthAt(const InkPoint& point, double baseWidth)
{
    const double p = qBound(0.25, point.pressure, 1.0);
    // a laid-flat pen touches with more of the nib: up to ~1.8x wider
    const double tilt = qBound(0.0, point.tilt, 1.0);
    return qMax(0.6, baseWidth * (0.45 + 0.55 * p) * (1.0 + 0.8 * tilt));
}

double pressureFromTablet(double raw)
{
    if (!(raw > 0.0)) return 0.18;   // NaN or zero -> hairline
    return qBound(0.18, raw, 1.0);
}

double pressureFromSpeed(double speed)
{
    // fast strokes get thinner, slow ones thicker
    const double t = qBound(0.0, speed / 1.2, 1.0);
    return qBound(0.3, 1.0 - 0.62 * t, 1.0);
}

QVector<Stamp> stamps(const QVector<InkPoint>& points, double baseWidth)
{
    QVector<Stamp> out;
    if (points.isEmpty()) return out;
    if (points.size() == 1) {
        out.append({points.first().pos, widthAt(points.first(), baseWidth) * 0.5});
        return out;
    }

    for (int i = 0; i < points.size() - 1; ++i) {
        const QPointF a = points.at(i).pos;
        const QPointF b = points.at(i + 1).pos;
        const double wa = widthAt(points.at(i), baseWidth);
        const double wb = widthAt(points.at(i + 1), baseWidth);
        const double segLen = QLineF(a, b).length();
        const double step = qMax(0.6, qMin(wa, wb) * 0.35);
        const int n = qMax(1, static_cast<int>(segLen / step));
        const double angA = points.at(i).angle;
        const double angB = points.at(i + 1).angle;
        const double elA = 1.0 + 0.7 * qBound(0.0, points.at(i).tilt, 1.0);
        const double elB = 1.0 + 0.7 * qBound(0.0, points.at(i + 1).tilt, 1.0);
        for (int k = 0; k < n; ++k) {
            const double t = static_cast<double>(k) / n;
            Stamp s;
            s.pos = a + (b - a) * t;
            s.radius = (wa + (wb - wa) * t) * 0.5;
            s.angle = angA + (angB - angA) * t;
            s.elongation = elA + (elB - elA) * t;
            out.append(s);
        }
    }
    const InkPoint& last = points.last();
    out.append({last.pos, widthAt(last, baseWidth) * 0.5});
    return out;
}

bool hits(const QVector<InkPoint>& points, const QPointF& p, double tolerance)
{
    if (points.isEmpty()) return false;
    if (points.size() == 1) return QLineF(points.first().pos, p).length() <= tolerance;
    for (int i = 0; i < points.size() - 1; ++i) {
        if (distanceToSegment(p, points.at(i).pos, points.at(i + 1).pos) <= tolerance) return true;
    }
    return false;
}

QVector<QVector<InkPoint>> splitStroke(const QVector<InkPoint>& stroke,
                                       const QVector<QPointF>& erasePath, double radius,
                                       double minLength)
{
    QVector<QVector<InkPoint>> pieces;
    if (stroke.size() < 2 || erasePath.isEmpty()) return pieces;

    auto erased = [&](const QPointF& p) {
        if (erasePath.size() == 1) return QLineF(erasePath.first(), p).length() <= radius;
        for (int i = 0; i < erasePath.size() - 1; ++i) {
            if (distanceToSegment(p, erasePath.at(i), erasePath.at(i + 1)) <= radius) return true;
        }
        return false;
    };

    // Interpolate the exact crossing point instead of dropping whole samples: the
    // surviving pieces then end right at the eraser edge (visually clean cut) and the
    // gap is exactly 2*radius wide regardless of the sampling rate.
    auto crossing = [&](const InkPoint& a, const InkPoint& b) {
        double lo = 0.0;   // erased side
        double hi = 1.0;   // kept side
        for (int i = 0; i < 24; ++i) {
            const double mid = 0.5 * (lo + hi);
            const QPointF p = a.pos + (b.pos - a.pos) * mid;
            if (erased(p))
                lo = mid;
            else
                hi = mid;
        }
        InkPoint cut;
        const double t = hi;
        cut.pos = a.pos + (b.pos - a.pos) * t;
        cut.pressure = a.pressure + (b.pressure - a.pressure) * t;
        return cut;
    };

    QVector<InkPoint> current;
    auto flush = [&]() {
        if (current.size() >= 2 && length(current) >= minLength) pieces.append(current);
        current.clear();
    };

    bool prevErased = erased(stroke.first().pos);
    if (!prevErased) current.append(stroke.first());

    for (int i = 1; i < stroke.size(); ++i) {
        const InkPoint& prev = stroke.at(i - 1);
        const InkPoint& cur = stroke.at(i);
        const bool curErased = erased(cur.pos);

        if (!prevErased && curErased) {
            // leaving the stroke right at the eraser edge
            current.append(crossing(cur, prev));   // from erased side to kept side
            flush();
        } else if (prevErased && !curErased) {
            // re-entering after the eraser passed
            current.append(crossing(prev, cur));
            current.append(cur);
        } else if (!curErased) {
            current.append(cur);
        }
        prevErased = curErased;
    }
    flush();
    return pieces;
}

double length(const QVector<InkPoint>& points)
{
    double total = 0;
    for (int i = 1; i < points.size(); ++i) total += QLineF(points.at(i - 1).pos, points.at(i).pos).length();
    return total;
}

}  // namespace ink
}  // namespace an
