#include "core/model.h"

#include <QUuid>

namespace an {

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString markupTypeName(MarkupType t)
{
    switch (t) {
    case MarkupType::Highlight: return QStringLiteral("highlight");
    case MarkupType::Underline: return QStringLiteral("underline");
    case MarkupType::Squiggly: return QStringLiteral("squiggly");
    case MarkupType::Note: return QStringLiteral("note");
    case MarkupType::Image: return QStringLiteral("image");
    case MarkupType::Ink: return QStringLiteral("ink");
    }
    return QStringLiteral("highlight");
}

MarkupType markupTypeFromName(const QString& name)
{
    if (name == QLatin1String("underline")) return MarkupType::Underline;
    if (name == QLatin1String("squiggly")) return MarkupType::Squiggly;
    if (name == QLatin1String("note")) return MarkupType::Note;
    if (name == QLatin1String("image")) return MarkupType::Image;
    if (name == QLatin1String("ink")) return MarkupType::Ink;
    return MarkupType::Highlight;
}

QJsonArray InkPoint::toJson() const
{
    QJsonArray a;
    a.append(pos.x());
    a.append(pos.y());
    a.append(pressure);
    if (tilt != 0.0 || angle != 0.0) {
        a.append(tilt);
        a.append(angle);
    }
    return a;
}

InkPoint InkPoint::fromJson(const QJsonArray& a)
{
    InkPoint p;
    if (a.size() >= 2) {
        p.pos = QPointF(a.at(0).toDouble(), a.at(1).toDouble());
        p.pressure = a.size() > 2 ? a.at(2).toDouble() : 1.0;
        p.tilt = a.size() > 3 ? a.at(3).toDouble() : 0.0;
        p.angle = a.size() > 4 ? a.at(4).toDouble() : 0.0;
    }
    return p;
}

QRectF Quad::boundingRect() const
{
    QPolygonF p{{ul, ur, lr, ll}};
    return p.boundingRect();
}

QPolygonF Quad::polygon() const
{
    return QPolygonF{{ul, ur, lr, ll}};
}

QJsonObject Quad::toJson() const
{
    auto pt = [](const QPointF& p) {
        QJsonArray a;
        a.append(p.x());
        a.append(p.y());
        return a;
    };
    QJsonObject o;
    o["ul"] = pt(ul);
    o["ur"] = pt(ur);
    o["ll"] = pt(ll);
    o["lr"] = pt(lr);
    return o;
}

static QPointF readPt(const QJsonObject& o, const char* key)
{
    const QJsonArray a = o[key].toArray();
    if (a.size() != 2) return {};
    return QPointF(a.at(0).toDouble(), a.at(1).toDouble());
}

Quad Quad::fromJson(const QJsonObject& o)
{
    Quad q;
    q.ul = readPt(o, "ul");
    q.ur = readPt(o, "ur");
    q.ll = readPt(o, "ll");
    q.lr = readPt(o, "lr");
    return q;
}

Quad Quad::fromRect(const QRectF& r)
{
    Quad q;
    q.ul = r.topLeft();
    q.ur = r.topRight();
    q.ll = r.bottomLeft();
    q.lr = r.bottomRight();
    return q;
}

QJsonObject Markup::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["docPath"] = docPath;
    o["page"] = page;
    o["type"] = markupTypeName(type);
    o["color"] = color.name(QColor::HexRgb);
    QJsonArray qs;
    for (const Quad& q : quads) qs.append(q.toJson());
    o["quads"] = qs;
    o["text"] = text;
    o["comment"] = comment;
    o["tags"] = QJsonArray::fromStringList(tags);
    if (!ink.isEmpty()) {
        QJsonArray pts;
        for (const InkPoint& p : ink) pts.append(p.toJson());
        o["ink"] = pts;
    }
    if (!imagePath.isEmpty()) o["imagePath"] = imagePath;
    o["created"] = created;
    o["updated"] = updated;
    o["written"] = writtenToPdf;
    if (!vv.isEmpty()) o["vv"] = vv;
    if (!deviceId.isEmpty()) o["deviceId"] = deviceId;
    if (deleted) o["deleted"] = true;
    if (!conflictedFrom.isEmpty()) o["conflictedFrom"] = conflictedFrom;
    if (!anchorChapter.isEmpty()) {
        o["anchorChapter"] = anchorChapter;
        o["anchorOffset"] = anchorOffset;
    }
    if (!mindGroup.isEmpty()) o["mindGroup"] = mindGroup;
    if (mindOrder != 0) o["mindOrder"] = mindOrder;
    return o;
}

Markup Markup::fromJson(const QJsonObject& o)
{
    Markup m;
    m.id = o["id"].toString();
    m.docPath = o["docPath"].toString();
    m.page = o["page"].toInt();
    m.type = markupTypeFromName(o["type"].toString());
    m.color = QColor(o["color"].toString());
    if (!m.color.isValid()) m.color = QColor(255, 213, 0);
    for (const QJsonValue& v : o["quads"].toArray()) m.quads.append(Quad::fromJson(v.toObject()));
    m.text = o["text"].toString();
    m.comment = o["comment"].toString();
    for (const QJsonValue& v : o["tags"].toArray()) m.tags.append(v.toString());
    for (const QJsonValue& v : o["ink"].toArray()) m.ink.append(InkPoint::fromJson(v.toArray()));
    m.imagePath = o["imagePath"].toString();
    m.created = static_cast<qint64>(o["created"].toDouble());
    m.updated = static_cast<qint64>(o["updated"].toDouble());
    m.writtenToPdf = o["written"].toBool();
    m.vv = o["vv"].toString();
    m.deviceId = o["deviceId"].toString();
    m.deleted = o["deleted"].toBool();
    m.conflictedFrom = o["conflictedFrom"].toString();
    m.anchorChapter = o["anchorChapter"].toString();
    m.anchorOffset = o.contains("anchorOffset") ? o["anchorOffset"].toInt() : -1;
    m.mindGroup = o["mindGroup"].toString();
    m.mindOrder = o["mindOrder"].toInt();
    return m;
}

QJsonObject MindGroup::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["docPath"] = docPath;
    o["name"] = name;
    o["order"] = order;
    if (!parentId.isEmpty()) o["parentId"] = parentId;
    if (!topicId.isEmpty()) o["topicId"] = topicId;
    o["updated"] = updated;
    if (deleted) o["deleted"] = true;
    if (!vv.isEmpty()) o["vv"] = vv;
    if (!deviceId.isEmpty()) o["deviceId"] = deviceId;
    return o;
}

MindGroup MindGroup::fromJson(const QJsonObject& o)
{
    MindGroup g;
    g.id = o["id"].toString();
    g.docPath = o["docPath"].toString();
    g.name = o["name"].toString();
    g.parentId = o["parentId"].toString();
    g.topicId = o["topicId"].toString();
    g.order = o["order"].toInt();
    g.deleted = o["deleted"].toBool();
    g.vv = o["vv"].toString();
    g.deviceId = o["deviceId"].toString();
    g.updated = static_cast<qint64>(o["updated"].toDouble());
    return g;
}

QJsonObject Topic::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["name"] = name;
    if (!description.isEmpty()) o["description"] = description;
    o["updated"] = updated;
    if (deleted) o["deleted"] = true;
    if (!vv.isEmpty()) o["vv"] = vv;
    if (!deviceId.isEmpty()) o["deviceId"] = deviceId;
    return o;
}

Topic Topic::fromJson(const QJsonObject& o)
{
    Topic t;
    t.id = o["id"].toString();
    t.name = o["name"].toString();
    t.description = o["description"].toString();
    t.deleted = o["deleted"].toBool();
    t.vv = o["vv"].toString();
    t.deviceId = o["deviceId"].toString();
    t.updated = static_cast<qint64>(o["updated"].toDouble());
    return t;
}

QJsonObject MindLink::toJson() const
{
    QJsonObject o;
    o["id"] = id;
    o["topicId"] = topicId;
    o["from"] = fromNode;
    o["to"] = toNode;
    if (!points.isEmpty()) o["points"] = points;
    if (!arrow.isEmpty()) o["arrow"] = arrow;
    if (!label.isEmpty()) o["label"] = label;
    o["updated"] = updated;
    if (deleted) o["deleted"] = true;
    if (!vv.isEmpty()) o["vv"] = vv;
    if (!deviceId.isEmpty()) o["deviceId"] = deviceId;
    return o;
}

MindLink MindLink::fromJson(const QJsonObject& o)
{
    MindLink l;
    l.id = o["id"].toString();
    l.topicId = o["topicId"].toString();
    l.fromNode = o["from"].toString();
    l.toNode = o["to"].toString();
    l.points = o["points"].toString();
    l.arrow = o["arrow"].toString();
    l.label = o["label"].toString();
    l.deleted = o["deleted"].toBool();
    l.vv = o["vv"].toString();
    l.deviceId = o["deviceId"].toString();
    l.updated = static_cast<qint64>(o["updated"].toDouble());
    return l;
}

QRectF Markup::boundingRect() const
{
    QRectF r;
    for (const Quad& q : quads) r = r.isNull() ? q.boundingRect() : r.united(q.boundingRect());
    return r;
}

}  // namespace an
