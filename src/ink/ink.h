#pragma once

#include "core/model.h"

#include <QPainterPath>
#include <QRectF>
#include <QVector>

namespace an {

// Handwriting helpers: simplify the raw pointer samples, then turn them into a
// smooth painter path (Catmull-Rom → cubic Bézier). Pure geometry, no UI.
namespace ink {

// Ramer–Douglas–Peucker: drops redundant points of a jittery/slow stroke.
QVector<InkPoint> simplify(const QVector<InkPoint>& points, double epsilon);

// Smooth path through the points; widths come from `baseWidth` * pressure.
QPainterPath smoothPath(const QVector<InkPoint>& points, double baseWidth);

QRectF bounds(const QVector<InkPoint>& points, double baseWidth);

// Total stroke length in page units (useful to reject accidental dots).
double length(const QVector<InkPoint>& points);

// Width in page units for one sample (pressure * base), clamped so a stroke never
// disappears or turns into a blob.
double widthAt(const InkPoint& point, double baseWidth);

// Maps a raw stylus tilt (degrees on both axes) to 0 (upright) .. 1 (flat).
double tiltFromDegrees(double xTiltDeg, double yTiltDeg);

// Maps a raw stylus pressure (0..1) into the range the renderer likes: light touches
// still leave a visible hairline, full pressure reaches the maximum width.
double pressureFromTablet(double raw);

// Maps pointer speed (page units per millisecond) to a synthetic pressure, used when
// the input device reports no pressure (mouse, trackpad).
double pressureFromSpeed(double speed);

// Stamp positions and radii for tapered rendering: one entry per stamp, spaced by a
// fraction of the local radius so the stroke looks continuous without too many items.
struct Stamp {
    QPointF pos;
    double radius = 0;    // semi-minor axis (page units)
    double angle = 0;     // nib direction in radians
    double elongation = 1.0;   // major/minor ratio (>1 when the pen is tilted)
};
QVector<Stamp> stamps(const QVector<InkPoint>& points, double baseWidth);

// True when the point is within `tolerance` page units of the stroke.
bool hits(const QVector<InkPoint>& points, const QPointF& p, double tolerance);

// Partial erase: removes the parts of the stroke that pass within `radius` of the
// eraser path and returns the surviving pieces (0..n). Pieces shorter than
// `minLength` are dropped so the eraser does not leave dust behind.
QVector<QVector<InkPoint>> splitStroke(const QVector<InkPoint>& stroke,
                                       const QVector<QPointF>& erasePath, double radius,
                                       double minLength = 2.0);

}  // namespace ink
}  // namespace an
