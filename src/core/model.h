#pragma once

#include <QColor>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

namespace an {

// Portable pi: MSVC only defines M_PI with _USE_MATH_DEFINES.
constexpr double kPi = 3.14159265358979323846;


enum class MarkupType { Highlight, Underline, Squiggly, Note, Image, Ink };

QString markupTypeName(MarkupType t);
MarkupType markupTypeFromName(const QString& name);

// One sampled point of a handwritten stroke, in page space.
struct InkPoint {
    QPointF pos;
    double pressure = 1.0;
    // Stylus orientation: tilt is 0 (upright) .. 1 (laid flat), angle is the nib
    // direction in radians. A tilted pen lays down a wider, elliptical footprint.
    double tilt = 0.0;
    double angle = 0.0;
    QJsonArray toJson() const;
    static InkPoint fromJson(const QJsonArray& a);
};

// A quadrilateral in PDF *page space* (points, unrotated, unscaled).
struct Quad {
    QPointF ul, ur, ll, lr;

    QRectF boundingRect() const;
    QPolygonF polygon() const;
    QJsonObject toJson() const;
    static Quad fromJson(const QJsonObject& o);
    static Quad fromRect(const QRectF& r);
};

struct Markup {
    QString id;               // uuid
    QString docPath;          // absolute path of owning document
    int page = 0;             // 0-based page index
    MarkupType type = MarkupType::Highlight;
    QColor color{255, 213, 0};
    QVector<Quad> quads;      // page space
    QString text;             // excerpt text
    QString comment;          // user note
    QStringList tags;         // free-form tags
    QString imagePath;        // absolute path for MarkupType::Image
    QVector<InkPoint> ink;    // stroke points for MarkupType::Ink
    qint64 created = 0;       // ms since epoch
    qint64 updated = 0;
    bool writtenToPdf = false;

    // ---- sync metadata ----
    QString vv;               // serialized version vector, see sync/vv.h
    QString deviceId;         // last writer
    bool deleted = false;     // tombstone: hidden locally, propagated to peers
    QString conflictedFrom;   // id of the note this one was forked from (concurrent edit)

    // ---- stable anchor (reflowable documents) ----
    // Page numbers move when an EPUB is re-laid-out; chapter + character offset stay
    // valid, so they are used to relocate a note on another device.
    QString anchorChapter;    // MuPDF chapter index as text, empty = no anchor
    int anchorOffset = -1;    // character offset inside that chapter

    // ---- mind map structure ----
    // mindGroup holds the *id* of a MindGroup (empty = fall back to page grouping).
    QString mindGroup;
    int mindOrder = 0;

    QJsonObject toJson() const;
    static Markup fromJson(const QJsonObject& o);
    QRectF boundingRect() const;
    bool isValid() const { return page >= 0 && !quads.isEmpty(); }
    bool isConflictCopy() const { return !conflictedFrom.isEmpty(); }
};

struct Selection {
    int page = -1;
    int startChar = -1;   // first character index on the page (for stable anchors)
    int endChar = -1;
    QVector<Quad> quads;
    QString text;

    bool isValid() const { return page >= 0 && !quads.isEmpty(); }
    void clear()
    {
        page = -1;
        quads.clear();
        text.clear();
    }
};

// One hit of an in-document text search.
struct TextHit {
    int page = -1;
    int startChar = 0;
    int endChar = 0;
    QVector<Quad> quads;   // page space, merged per line
    QString text;
};

// First class mind map group. Notes reference it through Markup::mindGroup, so the
// group can be renamed or deleted without rewriting every note.
struct MindGroup {
    QString id;
    QString docPath;
    QString name;
    QString parentId;   // empty = top level (groups form a tree per document)
    QString topicId;    // owning reading topic; empty = legacy per-document tree
    int order = 0;
    bool deleted = false;
    QString vv;
    QString deviceId;
    qint64 updated = 0;

    QJsonObject toJson() const;
    static MindGroup fromJson(const QJsonObject& o);
};

// A reading topic gathers several documents (a course, a project, a paper set).
// Notes stay per document; the topic decides which documents are viewed together
// and owns the mind-map tree.
struct Topic {
    QString id;
    QString name;
    QString description;
    bool deleted = false;
    QString vv;
    QString deviceId;
    qint64 updated = 0;

    QJsonObject toJson() const;
    static Topic fromJson(const QJsonObject& o);
};

// Which documents belong to a topic (a document may belong to several topics).
struct TopicDoc {
    QString topicId;
    QString docPath;
    int order = 0;
    bool deleted = false;
    QString vv;
    QString deviceId;
    qint64 updated = 0;
};

// Free-form relation between two mind-map nodes. Deliberately separate from
// MindGroup::parentId: a relation line must never be read as tree structure, and it
// carries its own arrow direction and control points.
struct MindLink {
    QString id;
    QString topicId;
    QString fromNode;
    QString toNode;
    QString points;   // JSON array of [x,y] control points, optional
    QString arrow;    // "", "start", "end", "both"
    QString label;
    bool deleted = false;
    QString vv;
    QString deviceId;
    qint64 updated = 0;

    QJsonObject toJson() const;
    static MindLink fromJson(const QJsonObject& o);
};

struct OutlineItem {
    QString title;
    int page = -1;
    QVector<OutlineItem> children;
};

QString newId();

}  // namespace an
