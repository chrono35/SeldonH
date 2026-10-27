/* ---------------------------------------------------------------------

Copyright © CNRS	2026

Authors :
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

SPDX-License-Identifier: GPL-3.0-or-later
--------------------------------------------------------------------- */

#include "ConversationStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace seldon {

namespace {

constexpr int kFormatVersion = 1;


const char* kindName(DisplayItem::Kind k)
{
    switch (k) {
    case DisplayItem::User:      return "user";
    case DisplayItem::Assistant: return "assistant";
    case DisplayItem::Notice:    return "notice";
    case DisplayItem::Plot:      return "plot";
    }
    return "notice";
}

bool kindFromName(const QString& n, DisplayItem::Kind& k)
{
    if (n == "user")           k = DisplayItem::User;
    else if (n == "assistant") k = DisplayItem::Assistant;
    else if (n == "notice")    k = DisplayItem::Notice;
    else if (n == "plot")      k = DisplayItem::Plot;
    else return false;
    return true;
}

QString isoDate(const QDateTime& d) { return d.toUTC().toString(Qt::ISODateWithMs); }
QDateTime fromIso(const QString& s) { return QDateTime::fromString(s, Qt::ISODate).toLocalTime(); }

} // namespace

bool Conversation::hasContent() const
{
    for (const StoredTurn& t : turns)
        if (!t.synthetic && !t.question.trimmed().isEmpty()) return true;
    return false;
}

int Conversation::exchanges() const
{
    int n = 0;
    for (const StoredTurn& t : turns)
        if (!t.synthetic && !t.question.trimmed().isEmpty()) ++n;
    return n;
}

QString Conversation::newId()
{
    // Date + hasard : tri naturel par date de création, sans collision, uniquement [0-9a-z-].
    return QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + "-"
         + QUuid::createUuid().toString(QUuid::Id128).left(8);
}

QString Conversation::makeTitle(const QString& q)
{
    QString t = q.trimmed();
    const int nl = t.indexOf('\n');
    if (nl >= 0) t = t.left(nl);
    t = t.simplified();
    if (t.isEmpty()) return QStringLiteral("Conversation");
    constexpr int kMax = 60;
    if (t.size() > kMax) t = t.left(kMax - 1).trimmed() + QStringLiteral("…");
    return t;
}

QString ConversationStore::defaultDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/history";
}

bool ConversationStore::validId(const QString& id)
{
    static const QRegularExpression re("^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$");
    return re.match(id).hasMatch();
}

QString ConversationStore::pathFor(const QString& id) const
{
    return QDir(dir_).absoluteFilePath(id + ".json");
}

QJsonObject ConversationStore::toJson(const Conversation& c)
{
    QJsonArray turns;
    for (const StoredTurn& t : c.turns) {
        QJsonObject o{ { "question", t.question }, { "modelUser", t.modelUser }, { "answer", t.answer },
                       { "followUp", t.followUp }, { "synthetic", t.synthetic } };
        if (!t.toolBlocks.isEmpty()) o["toolBlocks"] = t.toolBlocks;
        if (!t.skillIds.isEmpty())   o["skills"] = QJsonArray::fromStringList(t.skillIds);
        if (!t.prompt.isEmpty())     o["prompt"] = t.prompt;
        turns.append(o);
    }
    QJsonArray display;
    for (const DisplayItem& d : c.display) {
        QJsonObject o{ { "kind", kindName(d.kind) } };
        if (d.kind == DisplayItem::Plot) {
            o["plot"] = d.plot;
        } else {
            o["text"] = d.text;
            if (d.kind == DisplayItem::Assistant) {
                o["turn"] = d.turn;
                if (!d.thinking.isEmpty()) o["thinking"] = d.thinking;
            }
        }
        display.append(o);
    }
    return QJsonObject{ { "version", kFormatVersion }, { "id", c.id }, { "title", c.title },
                        { "created", isoDate(c.created) }, { "updated", isoDate(c.updated) },
                        { "backend", c.backend }, { "turns", turns }, { "display", display } };
}

bool ConversationStore::fromJson(const QJsonObject& o, Conversation& out, QString* err)
{
    auto fail = [&](const QString& m) { if (err) *err = m; return false; };
    if (o.value("version").toInt() != kFormatVersion) return fail(QStringLiteral("version de fichier inconnue"));
    Conversation c;
    c.id = o.value("id").toString();
    if (!validId(c.id)) return fail(QStringLiteral("identifiant invalide"));
    c.title = o.value("title").toString();
    c.created = fromIso(o.value("created").toString());
    c.updated = fromIso(o.value("updated").toString());
    c.backend = o.value("backend").toString();
    for (const QJsonValue& v : o.value("turns").toArray()) {
        const QJsonObject t = v.toObject();
        StoredTurn s;
        s.question = t.value("question").toString();
        s.modelUser = t.value("modelUser").toString();
        s.answer = t.value("answer").toString();
        s.toolBlocks = t.value("toolBlocks").toString();
        s.followUp = t.value("followUp").toBool();
        s.synthetic = t.value("synthetic").toBool();
        for (const QJsonValue& id : t.value("skills").toArray()) s.skillIds << id.toString();
        s.prompt = t.value("prompt").toObject();
        c.turns.append(s);
    }
    for (const QJsonValue& v : o.value("display").toArray()) {
        const QJsonObject d = v.toObject();
        DisplayItem it;
        if (!kindFromName(d.value("kind").toString(), it.kind)) continue;     // élément inconnu : ignoré
        it.text = d.value("text").toString();
        it.thinking = d.value("thinking").toString();
        it.turn = d.value("turn").toInt(-1);
        it.plot = d.value("plot").toObject();
        if (it.kind == DisplayItem::Assistant && (it.turn < 0 || it.turn >= c.turns.size())) continue;
        if (it.kind == DisplayItem::Plot && it.plot.isEmpty()) continue;
        c.display.append(it);
    }
    out = c;
    return true;
}

bool ConversationStore::save(const Conversation& c, QString* err) const
{
    auto fail = [&](const QString& m) { if (err) *err = m; return false; };
    if (!validId(c.id)) return fail(QStringLiteral("identifiant invalide"));
    if (!QDir().mkpath(dir_)) return fail(QStringLiteral("dossier impossible à créer : ") + dir_);
    QSaveFile f(pathFor(c.id));
    if (!f.open(QIODevice::WriteOnly)) return fail(f.errorString());
    f.write(QJsonDocument(toJson(c)).toJson(QJsonDocument::Compact));
    if (!f.commit()) return fail(f.errorString());
    return true;
}

bool ConversationStore::load(const QString& id, Conversation& out, QString* err) const
{
    auto fail = [&](const QString& m) { if (err) *err = m; return false; };
    if (!validId(id)) return fail(QStringLiteral("identifiant invalide"));
    QFile f(pathFor(id));
    if (!f.open(QIODevice::ReadOnly)) return fail(f.errorString());
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject()) return fail(QStringLiteral("fichier illisible"));
    if (!fromJson(d.object(), out, err)) return false;
    if (out.id != id) return fail(QStringLiteral("identifiant incohérent"));
    return true;
}

bool ConversationStore::remove(const QString& id) const
{
    if (!validId(id)) return false;
    return QFile::remove(pathFor(id));
}

int ConversationStore::removeAll() const
{
    int n = 0;
    const QStringList files = QDir(dir_).entryList({ "*.json" }, QDir::Files);
    for (const QString& name : files) {
        const QString id = QFileInfo(name).completeBaseName();
        if (validId(id) && QFile::remove(QDir(dir_).absoluteFilePath(name))) ++n;
    }
    return n;
}

QVector<ConversationInfo> ConversationStore::list() const
{
    QVector<ConversationInfo> v;
    const QStringList files = QDir(dir_).entryList({ "*.json" }, QDir::Files);
    for (const QString& name : files) {
        const QString id = QFileInfo(name).completeBaseName();
        Conversation c;
        if (!load(id, c)) continue;
        ConversationInfo i;
        i.id = c.id;
        i.title = c.title.isEmpty() ? QStringLiteral("Conversation") : c.title;
        i.updated = c.updated.isValid() ? c.updated : QFileInfo(QDir(dir_).absoluteFilePath(name)).lastModified();
        i.exchanges = c.exchanges();
        i.backend = c.backend;
        v.append(i);
    }
    std::sort(v.begin(), v.end(), [](const ConversationInfo& a, const ConversationInfo& b) {
        return a.updated != b.updated ? a.updated > b.updated : a.id > b.id;
    });
    return v;
}

} // namespace seldon
