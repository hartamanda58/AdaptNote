#include "sync/sync_manager.h"

#include "sync/vv.h"
#include "sync/webdav_client.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace an {
namespace {

constexpr const char* kConflictTag = "conflict";

QString conflictTag()
{
    return QString::fromLatin1(kConflictTag);
}

void addTag(Markup& m, const QString& tag)
{
    if (!m.tags.contains(tag)) m.tags.append(tag);
}

}  // namespace

QString SyncReport::summary() const
{
    if (!ok) return QStringLiteral("同步失败：%1").arg(error);
    return QStringLiteral("同步完成：%1 个文档，拉取 %2，推送 %3，冲突 %4")
        .arg(documents)
        .arg(pulled)
        .arg(pushed)
        .arg(conflicts);
}

SyncManager::SyncManager(Store& store)
    : store_(store)
{
}

SyncSettings SyncManager::settings() const
{
    SyncSettings s;
    s.url = store_.setting(QStringLiteral("sync.url"));
    s.user = store_.setting(QStringLiteral("sync.user"));
    s.password = store_.setting(QStringLiteral("sync.pass"));
    s.remotePath = store_.setting(QStringLiteral("sync.path"), QStringLiteral("adaptnote"));
    return s;
}

void SyncManager::setSettings(const SyncSettings& s)
{
    store_.setSetting(QStringLiteral("sync.url"), s.url.trimmed());
    store_.setSetting(QStringLiteral("sync.user"), s.user);
    store_.setSetting(QStringLiteral("sync.pass"), s.password);
    store_.setSetting(QStringLiteral("sync.path"), s.remotePath.trimmed());
}

QString SyncManager::docKeyFor(const QString& docPath)
{
    // Identity is content, not name or size: two different books can share both.
    QFile f(docPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha1);
    if (!hash.addData(&f)) return {};
    return QString::fromLatin1(hash.result().toHex().left(16));
}

QByteArray SyncManager::documentJson(const QString& docPath, const QString& docKey,
                                     const QVector<Markup>& markups,
                                     const QVector<MindGroup>& groups)
{
    QJsonObject root;
    root["schema"] = 1;
    root["docKey"] = docKey;
    root["docName"] = QFileInfo(docPath).fileName();
    root["updatedAt"] = static_cast<double>(QDateTime::currentMSecsSinceEpoch());

    // The local absolute path is device specific: never send it. Before writing a
    // pulled note we re-attach the local path (see syncDocument).
    QJsonArray list;
    for (const Markup& m : markups) {
        Markup wire = m;
        wire.docPath.clear();
        // Same reasoning for the excerpt image: only the file name travels, the
        // file itself lives in <remote>/images/<name>.
        if (!wire.imagePath.isEmpty()) wire.imagePath = QFileInfo(wire.imagePath).fileName();
        list.append(wire.toJson());
    }
    root["markups"] = list;

    if (!groups.isEmpty()) {
        QJsonArray groupList;
        for (const MindGroup& g : groups) {
            MindGroup wire = g;
            wire.docPath.clear();   // device specific, like markups
            groupList.append(wire.toJson());
        }
        root["groups"] = groupList;
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QVector<Markup> SyncManager::parseDocument(const QByteArray& json, QString* error)
{
    QVector<Markup> out;
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError) {
        if (error) *error = QStringLiteral("远端数据解析失败：%1").arg(perr.errorString());
        return out;
    }
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("远端数据不是 JSON 对象");
        return out;
    }
    const QJsonArray list = doc.object().value("markups").toArray();
    for (const QJsonValue& v : list) {
        Markup m = Markup::fromJson(v.toObject());
        if (m.id.isEmpty()) continue;
        out.append(m);
    }
    return out;
}

QByteArray SyncManager::topicsJson(const QVector<Topic>& topics,
                                    const QVector<TopicDoc>& docs,
                                    const QVector<MindLink>& links)
{
    QJsonObject root;
    root["schema"] = 1;
    QJsonArray ts, ds, ls;
    for (const Topic& t : topics) ts.append(t.toJson());
    for (const TopicDoc& d : docs) {
        QJsonObject o;
        o["topicId"] = d.topicId;
        o["docKey"] = docKeyFor(d.docPath);
        o["docName"] = QFileInfo(d.docPath).fileName();
        o["order"] = d.order;
        o["updated"] = static_cast<double>(d.updated);
        o["deleted"] = d.deleted;
        o["vv"] = d.vv;
        o["deviceId"] = d.deviceId;
        ds.append(o);
    }
    for (const MindLink& l : links) ls.append(l.toJson());
    root["topics"] = ts;
    root["topicDocs"] = ds;
    root["links"] = ls;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void SyncManager::parseTopics(const QByteArray& json, QVector<Topic>* topics,
                              QVector<TopicDoc>* docs, QVector<MindLink>* links,
                              QString* error)
{
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject()) {
        if (error) *error = QStringLiteral("主题同步数据解析失败：%1").arg(pe.errorString());
        return;
    }
    for (const QJsonValue& v : d.object().value("topics").toArray())
        topics->append(Topic::fromJson(v.toObject()));
    for (const QJsonValue& v : d.object().value("links").toArray())
        links->append(MindLink::fromJson(v.toObject()));
    for (const QJsonValue& v : d.object().value("topicDocs").toArray()) {
        const QJsonObject o = v.toObject();
        TopicDoc x;
        x.topicId = o.value("topicId").toString();
        // The remote carries a stable document key; the local path is resolved by the
        // sync coordinator before this record is persisted. Keep docName as a safe
        // placeholder here so parsing itself remains lossless and testable.
        x.docPath = o.value("docName").toString();
        x.order = o.value("order").toInt();
        x.updated = static_cast<qint64>(o.value("updated").toDouble());
        x.deleted = o.value("deleted").toBool();
        x.vv = o.value("vv").toString();
        x.deviceId = o.value("deviceId").toString();
        if (!x.topicId.isEmpty() && !x.docPath.isEmpty()) docs->append(x);
    }
}

SyncManager::TopicMergeResult SyncManager::mergeTopics(const QVector<Topic>& localTopics,
                                                        const QVector<Topic>& remoteTopics,
                                                        const QVector<TopicDoc>& localDocs,
                                                        const QVector<TopicDoc>& remoteDocs,
                                                        const QVector<MindLink>& localLinks,
                                                        const QVector<MindLink>& remoteLinks,
                                                        const QString& deviceId, qint64 nowMs)
{
    TopicMergeResult out;

    QHash<QString, Topic> lt, rt;
    for (const Topic& t : localTopics) lt.insert(t.id, t);
    for (const Topic& t : remoteTopics) rt.insert(t.id, t);
    QSet<QString> topicIds;
    for (auto it = lt.cbegin(); it != lt.cend(); ++it) topicIds.insert(it.key());
    for (auto it = rt.cbegin(); it != rt.cend(); ++it) topicIds.insert(it.key());
    QStringList sortedTopics = topicIds.values(); sortedTopics.sort();
    for (const QString& id : sortedTopics) {
        if (!lt.contains(id)) { out.topics.append(rt.value(id)); ++out.pulled; continue; }
        if (!rt.contains(id)) { out.topics.append(lt.value(id)); ++out.pushed; continue; }
        Topic l = lt.value(id), r = rt.value(id);
        const VersionVector lv = VersionVector::parse(l.vv), rv = VersionVector::parse(r.vv);
        if (lv.dominates(rv) || (lv.equal(rv) && l.updated >= r.updated)) out.topics.append(l);
        else if (rv.dominates(lv)) out.topics.append(r);
        else {
            VersionVector joined = lv; joined.join(rv); joined.bump(deviceId, static_cast<quint64>(nowMs));
            Topic keep = l.updated >= r.updated ? l : r;
            keep.vv = joined.toString(); keep.updated = qMax(l.updated, r.updated);
            out.topics.append(keep); ++out.conflicts;
        }
    }

    auto docKey = [](const TopicDoc& d) { return d.topicId + QLatin1Char('|') + d.docPath; };
    QHash<QString, TopicDoc> ld, rd;
    for (const TopicDoc& d : localDocs) ld.insert(docKey(d), d);
    for (const TopicDoc& d : remoteDocs) rd.insert(docKey(d), d);
    QSet<QString> docIds;
    for (auto it = ld.cbegin(); it != ld.cend(); ++it) docIds.insert(it.key());
    for (auto it = rd.cbegin(); it != rd.cend(); ++it) docIds.insert(it.key());
    QStringList sortedDocs = docIds.values(); sortedDocs.sort();
    for (const QString& id : sortedDocs) {
        if (!ld.contains(id)) { out.docs.append(rd.value(id)); ++out.pulled; continue; }
        if (!rd.contains(id)) { out.docs.append(ld.value(id)); ++out.pushed; continue; }
        TopicDoc l = ld.value(id), r = rd.value(id);
        const VersionVector lv = VersionVector::parse(l.vv), rv = VersionVector::parse(r.vv);
        if (lv.dominates(rv) || (lv.equal(rv) && l.updated >= r.updated)) out.docs.append(l);
        else if (rv.dominates(lv)) out.docs.append(r);
        else {
            VersionVector joined = lv; joined.join(rv); joined.bump(deviceId, static_cast<quint64>(nowMs));
            TopicDoc keep = l.updated >= r.updated ? l : r;
            keep.vv = joined.toString(); keep.updated = qMax(l.updated, r.updated);
            out.docs.append(keep); ++out.conflicts;
        }
    }

    QHash<QString, MindLink> ll, rl;
    for (const MindLink& l : localLinks) ll.insert(l.id, l);
    for (const MindLink& l : remoteLinks) rl.insert(l.id, l);
    QSet<QString> linkIds;
    for (auto it = ll.cbegin(); it != ll.cend(); ++it) linkIds.insert(it.key());
    for (auto it = rl.cbegin(); it != rl.cend(); ++it) linkIds.insert(it.key());
    QStringList sortedLinks = linkIds.values(); sortedLinks.sort();
    for (const QString& id : sortedLinks) {
        if (!ll.contains(id)) { out.links.append(rl.value(id)); ++out.pulled; continue; }
        if (!rl.contains(id)) { out.links.append(ll.value(id)); ++out.pushed; continue; }
        MindLink l = ll.value(id), r = rl.value(id);
        const VersionVector lv = VersionVector::parse(l.vv), rv = VersionVector::parse(r.vv);
        if (lv.dominates(rv) || (lv.equal(rv) && l.updated >= r.updated)) out.links.append(l);
        else if (rv.dominates(lv)) out.links.append(r);
        else {
            VersionVector joined = lv; joined.join(rv); joined.bump(deviceId, static_cast<quint64>(nowMs));
            MindLink keep = l.updated >= r.updated ? l : r;
            keep.vv = joined.toString(); keep.updated = qMax(l.updated, r.updated);
            out.links.append(keep); ++out.conflicts;
        }
    }
    return out;
}

QVector<MindGroup> SyncManager::parseGroups(const QByteArray& json, QString* error)
{
    QVector<MindGroup> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("远端数据不是 JSON 对象");
        return out;
    }
    for (const QJsonValue& v : doc.object().value("groups").toArray()) {
        MindGroup g = MindGroup::fromJson(v.toObject());
        if (!g.id.isEmpty()) out.append(g);
    }
    return out;
}

SyncManager::GroupMergeResult SyncManager::mergeGroups(const QVector<MindGroup>& local,
                                                     const QVector<MindGroup>& remote,
                                                     const QString& deviceId, qint64 nowMs)
{
    GroupMergeResult res;
    QHash<QString, MindGroup> lmap, rmap;
    for (const MindGroup& g : local) lmap.insert(g.id, g);
    for (const MindGroup& g : remote) rmap.insert(g.id, g);

    QSet<QString> ids;
    for (auto it = lmap.constBegin(); it != lmap.constEnd(); ++it) ids.insert(it.key());
    for (auto it = rmap.constBegin(); it != rmap.constEnd(); ++it) ids.insert(it.key());
    QStringList sortedIds = ids.values();
    sortedIds.sort();

    for (const QString& id : sortedIds) {
        const bool hasL = lmap.contains(id);
        const bool hasR = rmap.contains(id);
        if (hasL && !hasR) {
            res.merged.append(lmap.value(id));
            ++res.pushed;
            continue;
        }
        if (!hasL && hasR) {
            res.merged.append(rmap.value(id));
            ++res.pulled;
            continue;
        }

        MindGroup l = lmap.value(id);
        MindGroup r = rmap.value(id);
        const VersionVector lv = VersionVector::parse(l.vv);
        const VersionVector rv = VersionVector::parse(r.vv);

        if (lv.equal(rv)) {
            res.merged.append(l.updated >= r.updated ? l : r);
            continue;
        }
        if (lv.dominates(rv)) {
            res.merged.append(l);
            ++res.pushed;
            continue;
        }
        if (rv.dominates(lv)) {
            res.merged.append(r);
            ++res.pulled;
            continue;
        }

        // concurrent rename: newest name wins (merge the vectors so it converges)
        VersionVector joined = lv;
        joined.join(rv);
        joined.bump(deviceId, static_cast<quint64>(nowMs));
        MindGroup kept = (l.updated >= r.updated) ? l : r;
        kept.vv = joined.toString();
        kept.updated = qMax(l.updated, r.updated);
        res.merged.append(kept);
        ++res.conflicts;
    }
    return res;
}

SyncManager::MergeResult SyncManager::merge(const QVector<Markup>& local,
                                           const QVector<Markup>& remote,
                                           const QString& deviceId, qint64 nowMs)
{
    MergeResult res;

    QHash<QString, Markup> lmap, rmap;
    for (const Markup& m : local) lmap.insert(m.id, m);
    for (const Markup& m : remote) rmap.insert(m.id, m);

    QSet<QString> ids;
    for (auto it = lmap.constBegin(); it != lmap.constEnd(); ++it) ids.insert(it.key());
    for (auto it = rmap.constBegin(); it != rmap.constEnd(); ++it) ids.insert(it.key());

    QStringList sortedIds = ids.values();
    sortedIds.sort();

    for (const QString& id : sortedIds) {
        const bool hasL = lmap.contains(id);
        const bool hasR = rmap.contains(id);

        if (hasL && !hasR) {
            res.merged.append(lmap.value(id));
            ++res.pushed;
            continue;
        }
        if (!hasL && hasR) {
            res.merged.append(rmap.value(id));
            ++res.pulled;
            continue;
        }

        Markup l = lmap.value(id);
        Markup r = rmap.value(id);
        const VersionVector lv = VersionVector::parse(l.vv);
        const VersionVector rv = VersionVector::parse(r.vv);

        if (lv.equal(rv)) {
            // Same logical version: prefer the one modified later, tie-break by device.
            if (l.updated >= r.updated)
                res.merged.append(l);
            else
                res.merged.append(r);
            continue;
        }
        if (lv.dominates(rv)) {
            res.merged.append(l);
            ++res.pushed;
            continue;
        }
        if (rv.dominates(lv)) {
            res.merged.append(r);
            ++res.pulled;
            continue;
        }

        // ---- concurrent versions ----
        if (l.deleted && r.deleted) {
            // both sides deleted the same note: nothing to reconcile
            res.merged.append(l.updated >= r.updated ? l : r);
            continue;
        }

        VersionVector joined = lv;
        joined.join(rv);
        joined.bump(deviceId, static_cast<quint64>(nowMs));

        // Delete-vs-edit: deletion never wins over content. Keep the edited note under
        // its original id, tag it so the user can re-delete it deliberately.
        if (l.deleted != r.deleted) {
            const Markup& live = l.deleted ? r : l;
            Markup kept = live;
            kept.deleted = false;
            kept.vv = joined.toString();
            kept.updated = qMax(l.updated, r.updated);
            addTag(kept, conflictTag());
            res.merged.append(kept);
            ++res.conflicts;
            res.messages << QStringLiteral("删除与修改冲突：已保留修改并标记 conflict（%1）")
                                .arg(id.left(8));
            continue;
        }

        const bool localWins = l.updated >= r.updated;
        const Markup& winner = localWins ? l : r;
        const Markup& loser = localWins ? r : l;

        Markup kept = winner;
        kept.vv = joined.toString();
        kept.updated = qMax(l.updated, r.updated);
        res.merged.append(kept);

        // Keep the other version as a separate note so no edit is ever lost.
        Markup copy = loser;
        copy.id = newId();
        copy.conflictedFrom = id;
        copy.vv = joined.toString();
        copy.updated = qMax(l.updated, r.updated);
        copy.created = nowMs;
        addTag(copy, conflictTag());
        res.merged.append(copy);

        ++res.conflicts;
        res.messages << QStringLiteral("同一笔记两端都改过：已保留两个版本（%1）").arg(id.left(8));
    }

    return res;
}

SyncReport SyncManager::syncDocument(const QString& docPath)
{
    SyncReport rep;
    const SyncSettings s = settings();
    if (!s.configured()) {
        rep.error = QStringLiteral("尚未配置 WebDAV 地址");
        return rep;
    }

    WebDavClient dav;
    dav.setBaseUrl(s.url);
    if (!s.user.isEmpty()) dav.setCredentials(s.user, s.password);

    const QString docKey = docKeyFor(docPath);
    const QString rel = s.remotePath + QLatin1Char('/') + docKey + QStringLiteral(".json");

    QString err;
    dav.mkcol(s.remotePath, &err);  // ignore "already exists"

    for (int attempt = 0; attempt < 2; ++attempt) {
        QByteArray remoteBytes;
        QString etag;
        bool notFound = false;
        const bool got = dav.get(rel, &remoteBytes, &etag, &err, &notFound);
        if (!got && !notFound) {
            rep.error = err;
            return rep;
        }

        QVector<Markup> remote;
        if (got && !remoteBytes.isEmpty()) {
            remote = parseDocument(remoteBytes, &err);
            if (!err.isEmpty()) {
                rep.error = err;
                return rep;
            }
        }

        const QVector<Markup> local = store_.markupsIncludingDeleted(docPath);
        MergeResult merged =
            merge(local, remote, store_.deviceId(), QDateTime::currentMSecsSinceEpoch());

        const QVector<MindGroup> remoteGroups = parseGroups(remoteBytes, &err);
        const QVector<MindGroup> localGroups = store_.mindGroupsIncludingDeleted(docPath);
        const GroupMergeResult mergedGroups =
            mergeGroups(localGroups, remoteGroups, store_.deviceId(),
                        QDateTime::currentMSecsSinceEpoch());

        // Attach device-local paths BEFORE persisting and before reconciling images:
        // the wire format only carries bare file names.
        for (Markup& m : merged.merged) {
            m.docPath = docPath;
            if (!m.imagePath.isEmpty() && !QFileInfo(m.imagePath).isAbsolute())
                m.imagePath = store_.imagesDir() + QLatin1Char('/') + m.imagePath;
        }
        store_.upsertMarkups(merged.merged, &err);   // single transaction
        for (MindGroup g : mergedGroups.merged) {
            g.docPath = docPath;
            store_.upsertMindGroup(g, nullptr, false);
        }
        merged.mergedGroups = mergedGroups.merged;
        merged.groupConflicts = mergedGroups.conflicts;
        store_.exportMirror(docPath);

        // ---- binary payloads (image excerpts) ----
        if (settings().configured()) {
            QVector<Markup> images;
            for (const Markup& m : merged.merged)
                if (m.type == MarkupType::Image && !m.imagePath.isEmpty() && !m.deleted)
                    images.append(m);
            if (!images.isEmpty()) {
                const QString imagesPath = s.remotePath + QStringLiteral("/images");
                dav.mkcol(imagesPath, &err);

                QVector<DavEntry> remoteImages;
                QString listErr;
                if (dav.list(imagesPath, &remoteImages, &listErr)) {
                    QSet<QString> remoteNames;
                    for (const DavEntry& e : remoteImages) remoteNames.insert(e.name);

                    for (const Markup& m : images) {
                        const QString name = QFileInfo(m.imagePath).fileName();
                        const QFileInfo local(m.imagePath);
                        const bool haveLocal = local.exists();
                        const bool haveRemote = remoteNames.contains(name);

                        if (haveLocal && !haveRemote) {
                            QFile f(m.imagePath);
                            if (f.open(QIODevice::ReadOnly))
                                dav.put(imagesPath + QLatin1Char('/') + name, f.readAll(), QString(),
                                        &err);
                        } else if (!haveLocal && haveRemote) {
                            QByteArray bytes;
                            QString etag;
                            if (dav.get(imagesPath + QLatin1Char('/') + name, &bytes, &etag, &err)) {
                                QDir().mkpath(store_.imagesDir());
                                QFile out(m.imagePath);
                                if (out.open(QIODevice::WriteOnly)) out.write(bytes);
                            }
                        }
                    }
                }
            }
        }

        const QByteArray payload =
            documentJson(docPath, docKey, merged.merged, mergedGroups.merged);
        bool precond = false;
        if (dav.put(rel, payload, got ? etag : QString(), &err, &precond)) {
            rep.ok = true;
            rep.documents = 1;
            rep.pulled = merged.pulled;
            rep.pushed = merged.pushed;
            rep.conflicts = merged.conflicts;
            rep.messages = merged.messages;
            if (merged.groupConflicts > 0)
                rep.messages << QStringLiteral("脑图分组有 %1 处并发改名，已保留较新的名字")
                                    .arg(merged.groupConflicts);
            return rep;
        }
        if (precond && attempt == 0) continue;  // remote changed under us: re-merge once

        rep.error = err;
        return rep;
    }

    rep.error = QStringLiteral("同步重试后仍然冲突");
    return rep;
}

SyncReport SyncManager::syncAll()
{
    SyncReport total;
    const QVector<QString> docs = store_.documentPaths();
    for (const QString& doc : docs) {
        const SyncReport one = syncDocument(doc);
        if (!one.ok) {
            total.ok = false;
            total.error = one.error;
            return total;
        }
        total.documents += one.documents;
        total.pulled += one.pulled;
        total.pushed += one.pushed;
        total.conflicts += one.conflicts;
        total.messages += one.messages;
    }

    const SyncSettings cfg = settings();
    if (!cfg.configured()) {
        total.ok = !docs.isEmpty();
        return total;
    }
    const QString deviceId = store_.deviceId();
    const QString rel = cfg.remotePath + QStringLiteral("/topics.json");
    WebDavClient dav; dav.setBaseUrl(cfg.url); dav.setCredentials(cfg.user, cfg.password);

    for (int attempt = 0; attempt < 2; ++attempt) {
        QString err, etag;
        QByteArray remoteBytes;
        const bool got = dav.get(rel, &remoteBytes, &etag, &err);
        if (!got && !err.contains(QStringLiteral("404"))) {
            total.ok = false; total.error = err; return total;
        }
        QVector<Topic> remoteTopics; QVector<TopicDoc> remoteDocs; QVector<MindLink> remoteLinks;
        if (got) {
            parseTopics(remoteBytes, &remoteTopics, &remoteDocs, &remoteLinks, &err);
            if (!err.isEmpty()) { total.ok = false; total.error = err; return total; }
        }

        QHash<QString, QString> keyToPath;
        for (const QString& path : store_.documentPaths()) {
            const QString key = docKeyFor(path);
            if (!key.isEmpty()) keyToPath.insert(key, path);
        }
        for (TopicDoc& d : remoteDocs) {
            const QString key = d.docPath; // parseTopics currently stores docName; recover from raw below
            Q_UNUSED(key);
        }
        // Re-parse topic docs directly so unresolved identities stay as unresolved:key.
        remoteDocs.clear();
        if (got) {
            const QJsonArray arr = QJsonDocument::fromJson(remoteBytes).object().value("topicDocs").toArray();
            for (const QJsonValue& v : arr) {
                const QJsonObject o = v.toObject();
                TopicDoc d;
                d.topicId = o.value("topicId").toString();
                const QString key = o.value("docKey").toString();
                d.docPath = keyToPath.value(key, QStringLiteral("unresolved:") + key);
                d.order = o.value("order").toInt();
                d.updated = static_cast<qint64>(o.value("updated").toDouble());
                d.deleted = o.value("deleted").toBool();
                d.vv = o.value("vv").toString();
                d.deviceId = o.value("deviceId").toString();
                if (!d.topicId.isEmpty() && !key.isEmpty()) remoteDocs.append(d);
            }
        }

        const auto merged = mergeTopics(store_.topics(true), remoteTopics,
                                         store_.topicDocs(QString(), true), remoteDocs,
                                         store_.mindLinks(QString(), true), remoteLinks,
                                         deviceId, QDateTime::currentMSecsSinceEpoch());
        for (const Topic& t : merged.topics) store_.upsertTopic(t);
        for (const TopicDoc& d : merged.docs) store_.upsertTopicDoc(d);
        for (const MindLink& l : merged.links) store_.upsertMindLink(l);

        const QByteArray payload = topicsJson(store_.topics(true), store_.topicDocs(QString(), true),
                                               store_.mindLinks(QString(), true));
        bool precond = false;
        if (dav.put(rel, payload, got ? etag : QString(), &err, &precond)) {
            total.ok = true;
            total.pulled += merged.pulled;
            total.pushed += merged.pushed;
            total.conflicts += merged.conflicts;
            return total;
        }
        if (precond && attempt == 0) continue;
        total.ok = false; total.error = err; return total;
    }
    total.ok = false;
    total.error = QStringLiteral("主题同步重试后仍然冲突");
    return total;
}

}  // namespace an
