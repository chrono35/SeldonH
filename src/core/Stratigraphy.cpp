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

#include "Stratigraphy.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <functional>

#include "ChrBuilder.h"
#include "Provenance.h"
#include "../methods/common/TextUtil.h"
#include "../methods/common/MethodRunner.h"
#include <algorithm>
#include <cmath>

namespace seldon {
namespace strat {

namespace {

// Unité nommée dans un texte passé par fold() : type + numéro (« us 2 », « u.s. 2 », « us2b », « couche 3 »).
const QRegularExpression& unitRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(\b(us|u\.s\.?|unite stratigraphique|unites stratigraphiques|su|couches?|layers?|niveaux?|levels?)\s*(?:n\s*[°o]\s*|#|-)?\s*(\d{1,4}[a-z]?))"
        // liste : « les niveaux 1 et 2 », « US 4, 5 et 6 » (pas une mesure : « niveau 2 et 3860 ± 35 », « US 1 et 2 dates »)
        R"(((?:\s*(?:,|et|and|&)\s*\d{1,4}[a-z]?\b(?!\s*(?:±|\+\s*/?\s*-|bp|ans|years|cal|%|dates?|datations?|echantillons?|samples?)))*)\b)"));
    return re;
}

QString keyOf(const QString& type, const QString& num)
{
    QString t = type;
    if (t.startsWith(QLatin1String("u")) || t == QLatin1String("su")) t = QStringLiteral("us");
    else if (t.startsWith(QLatin1String("couche")) || t.startsWith(QLatin1String("layer"))) t = QStringLiteral("couche");
    else if (t.startsWith(QLatin1String("niveau")) || t.startsWith(QLatin1String("level"))) t = QStringLiteral("niveau");
    return t + num;
}

// Nom d'Event comparable au texte : fold, espaces simplifiés, apostrophes droites.
QString foldName(const QString& name)
{
    QString f = text::fold(name);
    f.replace(QChar(0x2019), QLatin1Char('\''));
    return f.simplified();
}
QString nameKey(const QString& foldedName) { return QStringLiteral("n:") + foldedName; }

// Noms d'Events cherchés dans le texte : pas trop courts, pas des mots courants, pas des US numérotées (déjà reconnues).
QStringList searchableNames(const QStringList& eventNames)
{
    static const QSet<QString> common = { "event", "evenement", "date", "dates", "phase", "borne", "bound", "debut", "fin", "les", "des", "une" };
    QStringList out;
    for (const QString& n : eventNames) {
        const QString f = foldName(n);
        if (f.size() < 3 || common.contains(f) || out.contains(f)) continue;
        if (unitRe().match(f).hasMatch()) continue;          // « US 12 », « Fosse US 3 » : reconnues par leur numéro
        out << f;
    }
    return out;
}

struct Mention { QString key; qsizetype start, end; };

QVector<Mention> numbered(const QString& folded)
{
    QVector<Mention> out;
    static const QRegularExpression item(QStringLiteral(R"(\d{1,4}[a-z]?)"));
    for (auto it = unitRe().globalMatch(folded); it.hasNext();) {
        const auto m = it.next();
        out.append({ keyOf(m.captured(1), m.captured(2)), m.capturedStart(), m.capturedEnd() });
        for (auto li = item.globalMatch(m.captured(3)); li.hasNext();)      // membres d'une liste : même passage du texte
            out.append({ keyOf(m.captured(1), li.next().captured(0)), m.capturedStart(), m.capturedEnd() });
    }
    return out;
}

// Relation entre deux unités : texte entre elles (A … B). +1 : A plus récent que B (A au-dessus), -1 : A plus ancien.
int relationBetween(QString between)
{
    static const QRegularExpression stop(QStringLiteral(R"([;.!?\n]|\bet\b|\band\b|\bpuis\b|\bthen\b|\bmais\b|\bbut\b)"));
    if (stop.match(between).hasMatch()) return 0;      // une relation ne traverse ni phrase ni coordination
    between.replace(QRegularExpression(QStringLiteral(R"([()\[\],:"«»’'])")), QStringLiteral(" "));
    between = between.simplified();
    static const QString filler = QStringLiteral(
        R"((?:est|sont|se trouve|se trouvent|se situe|situee?s?|placee?s?|qui|elle|il|is|are|lies|lie|which|bien|directement|juste|)"
        R"(immediatement|de|du|des|d|la|le|les|l|a|the|of|to|than|que|qu|par|by|et|puis|encore|plus haut|plus bas|)"
        // « US7 est aussi au dessus de US3 », « au dessus de l'Event US6 » (prompt réel du 13/11/2026)
        R"(aussi|egalement|also|too|event|events|evenement|evenements)?)");
    static const QRegularExpression younger(QStringLiteral(
        R"(^(?:%1\s+)*(?:sur|au dessus|au-dessus|recouvre|recouvrent|scelle|scellent|recoupe|recoupent|coupe|coupent|surmonte|)"
        R"(surmontent|above|over|overlies|overlie|seals|cuts|cut|posterieure?s?|plus recente?s?|apres|after|later|younger)(?:\s+%1)*$)").arg(filler));
    static const QRegularExpression older(QStringLiteral(
        R"(^(?:%1\s+)*(?:sous|au dessous|au-dessous|en dessous|en-dessous|est recoupee?|est coupee?|est scellee?|est recouverte?|)"
        R"(sont recoupee?s|sont coupee?s|sont scellee?s|sont recouverte?s|recoupee?|coupee?|scellee?|recouverte?|est surmontee?|surmontee?|is covered|covered|)"
        R"(below|under|underlies|underlie|beneath|is cut|is sealed|is overlain|cut|sealed|overlain|anterieure?s?|plus ancienne?s?|)"
        R"(avant|before|earlier|older)(?:\s+%1)*$)").arg(filler));
    // « recoupée par » / « scellée par » : forme passive, le sujet est plus ancien (traitée avant la forme active).
    static const QRegularExpression passive(QStringLiteral(R"(\b(?:recoupee?s?|coupee?s?|scellee?s?|recouverte?s?|surmontee?s?|cut|sealed|overlain|covered)\s+(?:par|by)\b)"));
    if (between.isEmpty() || between.size() > 60) return 0;
    if (passive.match(between).hasMatch() && older.match(between).hasMatch()) return -1;
    if (younger.match(between).hasMatch()) return 1;
    if (older.match(between).hasMatch()) return -1;
    return 0;
}

// Position absolue dans une proposition : 2 en haut (plus récent), 1 au milieu, 0 au fond (plus ancien).
struct Position { int rank; qsizetype start, end; QString words; };

QVector<Position> positions(const QString& clause)
{
    static const QRegularExpression top(QStringLiteral(
        R"(\b(?:en haut de la (?:sequence|stratigraphie|coupe|serie|sequence stratigraphique)|au sommet de la (?:sequence|stratigraphie|coupe|serie)|)"
        R"((?:au[ -]dessus|en haut|au sommet|en surface|superieure?s?|la plus haute|le plus haut|tout en haut|la plus superficielle|le plus superficiel|)"
        R"(superficielle?s?|la plus recente|le plus recent|on top|at the top|top|uppermost|upper|topmost|shallowest)\b(?!\s*(?:de|du|des|d'|of|a)\b)))"));
    static const QRegularExpression mid(QStringLiteral(
        R"(\b(?:du milieu|au milieu|intermediaires?|au centre|intercalee?s?|entre les deux|middle|in between|in the middle|between them)\b)"));
    static const QRegularExpression bottom(QStringLiteral(
        R"(\b(?:a la base de la (?:sequence|stratigraphie|coupe|serie)|en bas de la (?:sequence|stratigraphie|coupe|serie)|)"
        R"((?:au fond|en bas|a la base|tout en bas|inferieure?s?|la plus basse|le plus bas|la plus profonde|le plus profond|la plus ancienne|le plus ancien|)"
        R"(au[ -]dessous|en[ -]dessous|at the bottom|bottom|lowermost|lower|at the base|deepest)\b(?!\s*(?:de|du|des|d'|of|a)\b)))"));
    QVector<Position> out;
    auto add = [&](const QRegularExpression& re, int rank) {
        for (auto it = re.globalMatch(clause); it.hasNext();) {
            const auto m = it.next();
            out.append({ rank, m.capturedStart(), m.capturedEnd(), m.captured(0) });
        }
    };
    add(top, 2);
    add(mid, 1);
    add(bottom, 0);
    return out;
}

// Unités citées : noms d'Events donnés (« surface », « fond de fosse », du plus long au plus court), puis US numérotées.
// Un nom qui fait partie d'une position (« en surface », « au fond ») est une position, pas une unité.
QVector<Mention> mentionsIn(const QString& folded, const QStringList& names)
{
    QVector<Mention> out;
    auto overlaps = [&](qsizetype a, qsizetype b) {
        for (const Mention& m : std::as_const(out)) if (a < m.end && b > m.start) return true;
        return false;
    };
    const QVector<Position> pos = positions(folded);
    QStringList sorted = names;
    std::sort(sorted.begin(), sorted.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
    for (const QString& n : std::as_const(sorted)) {
        const QRegularExpression re(QStringLiteral("(?<![\\w])%1(?![\\w])").arg(QRegularExpression::escape(n)));
        for (auto it = re.globalMatch(folded); it.hasNext();) {
            const auto m = it.next();
            const qsizetype a = m.capturedStart(), b = m.capturedEnd();
            bool inPosition = false;
            for (const Position& p : pos) if (a >= p.start && b <= p.end) inPosition = true;
            if (!inPosition && !overlaps(a, b)) out.append({ nameKey(n), a, b });
        }
    }
    const qsizetype nNames = out.size();
    for (const Mention& m : numbered(folded)) {
        bool hit = false;                                   // chevauchement avec un nom seulement (une liste partage son passage)
        for (qsizetype i = 0; i < nNames; ++i) if (m.start < out[i].end && m.end > out[i].start) hit = true;
        if (!hit) out.append(m);
    }
    std::sort(out.begin(), out.end(), [](const Mention& a, const Mention& b) { return a.start < b.start; });
    return out;
}

void addPair(QVector<Relation>& rels, const QString& older, const QString& younger, const QString& ev)
{
    if (older == younger) return;
    for (int i = rels.size() - 1; i >= 0; --i)
        if ((rels[i].older == older && rels[i].younger == younger) || (rels[i].older == younger && rels[i].younger == older))
            rels.removeAt(i);
    rels.append({ older, younger, ev });
}

bool isBound(const QJsonObject& e)
{
    return e.value("type").toString().compare(QLatin1String("bound"), Qt::CaseInsensitive) == 0 || e.contains("known_fixed");
}

bool hasDate(const QJsonObject& e)
{
    for (const char* k : { "dates", "date" }) {
        const QJsonValue v = e.value(k);
        if (v.isArray() && !v.toArray().isEmpty()) return true;
        if (v.isObject() && !v.toObject().isEmpty()) return true;
    }
    for (const char* k : { "age", "f14c", "fraction", "inc", "dec", "field", "min", "max", "mean" })
        if (e.contains(k)) return true;
    return false;
}

} // namespace

QString unitKey(const QString& name)
{
    const QString f = text::fold(name);
    const auto m = unitRe().match(f);
    return m.hasMatch() ? keyOf(m.captured(1), m.captured(2)) : QString();
}

QVector<Relation> closure(const QVector<Relation>& rels);

QVector<Relation> relationsFromText(const QString& text, const QStringList& eventNames)
{
    QString f = text::fold(text);
    f.replace(QChar(0x2019), QLatin1Char('\''));
    const QStringList names = searchableNames(eventNames);

    // Unités citées, regroupées par passage du texte (« les niveaux 1 et 2 » : deux unités au même endroit).
    struct Slot { QStringList keys; qsizetype start, end; };
    auto slotsOf = [&](const QString& t) {
        QVector<Slot> out;
        for (const Mention& m : mentionsIn(t, names)) {
            if (!out.isEmpty() && out.last().start == m.start) { if (!out.last().keys.contains(m.key)) out.last().keys << m.key; }
            else out.append({ { m.key }, m.start, m.end });
        }
        return out;
    };
    static const QRegularExpression sentenceSep(QStringLiteral(R"((?:[;!?\n]|\.(?=\s|$)))"));
    // Pronom en tête de phrase : l'unité citée en premier dans la phrase précédente (« Le niveau 3, à la base… Il est
    // recouvert par le niveau 2 » : niveau 3 recouvert par le niveau 2).
    {
        static const QRegularExpression pron(QStringLiteral(R"(^\s*(il|elle|ils|elles|celui-ci|celle-ci|ceux-ci|celles-ci|it|they)\b)"));
        QVector<QPair<qsizetype, qsizetype>> sentences;
        qsizetype from = 0;
        for (auto it = sentenceSep.globalMatch(f); it.hasNext();) {
            const auto m = it.next();
            sentences.append({ from, m.capturedStart() });
            from = m.capturedEnd();
        }
        sentences.append({ from, f.size() });
        for (int i = int(sentences.size()) - 1; i >= 1; --i) {       // de la fin : les positions précédentes restent justes
            const QString sent = f.mid(sentences[i].first, sentences[i].second - sentences[i].first);
            const auto pm = pron.match(sent);
            if (!pm.hasMatch()) continue;
            const QString prev = f.mid(sentences[i - 1].first, sentences[i - 1].second - sentences[i - 1].first);
            const QVector<Slot> ps = slotsOf(prev);
            if (ps.isEmpty()) continue;
            f.replace(sentences[i].first + pm.capturedStart(1), pm.capturedLength(1), prev.mid(ps.first().start, ps.first().end - ps.first().start));
        }
    }

    QVector<Relation> rels;
    const QVector<Slot> all = slotsOf(f);
    QVector<QPair<qsizetype, qsizetype>> used;                // passages pris par une relation entre deux unités
    auto relate = [&](const Slot& a, const Slot& b, int r, const QString& words) {
        for (const QString& ka : a.keys)
            for (const QString& kb : b.keys) {
                const QString ev = QStringLiteral("%1 %2 %3").arg(ka, words, kb);
                if (r > 0) addPair(rels, kb, ka, ev);         // a au-dessus de b : b plus ancien
                else       addPair(rels, ka, kb, ev);
            }
    };
    // 1. Relations entre deux unités voisines (« US 2 sous US 1 », « l'US 4 est recoupée par l'US 5 », « une fosse recoupe
    //    les niveaux 1 et 2 ») ; sujet sous-entendu après « et » (« l'US 2 est sous l'US 1 et au-dessus de l'US 3 »).
    //    Second complément de la même relation (« l'US 6 est en dessous de l'US 10 et de l'US 7 », « US1 est en dessous de
    //    US6 et US5 ») : même sujet, même sens, sauf si ce complément commence une nouvelle proposition (« … et l'US 3 est
    //    au fond »). Prompt réel du 13/11/2026.
    static const QRegularExpression coord(QStringLiteral(R"(^\W*(?:et|and|,)\s+(.*)$)"));
    static const QRegularExpression sameRelation(QStringLiteral(
        R"(^[\s,'"«»]*(?:et|and|ou|or|,)(?:[\s'"«»]+(?:de|du|des|d|de la|la|le|les|l|a|au|aux|the|of|event|evenement|unite))*[\s'"«»]*$)"));
    static const QRegularExpression newClause(QStringLiteral(
        R"(^[\s,]*(?:est|sont|etait|se|s|repose|reposent|recoup\w*|coup\w*|scell\w*|recouvr\w*|surmont\w*|qui|is|are|was|lies|lie|)"
        R"(cuts?|seals?|overl\w*|which|that|au[ -]dessus|au[ -]dessous|en[ -]dessous|sous|sur|au fond|en haut|en bas|au milieu)\b)"));
    int lastSubject = -1;                                     // sujet de la dernière relation, si elle finit sur l'unité i
    int lastRel = 0;
    QString lastWords;
    for (int i = 0; i + 1 < all.size(); ++i) {
        const QString between = f.mid(all[i].end, all[i + 1].start - all[i].end);
        const int r = relationBetween(between);
        if (r != 0) {
            relate(all[i], all[i + 1], r, between.simplified());
            used.append({ all[i].end, all[i + 1].start });
            lastSubject = i;
            lastRel = r;
            lastWords = between.simplified();
            continue;
        }
        if (lastSubject >= 0 && lastSubject <= i - 1) {
            const auto m = coord.match(between);
            const int r2 = m.hasMatch() && lastSubject == i - 1 ? relationBetween(m.captured(1)) : 0;
            if (r2 != 0) {
                relate(all[lastSubject], all[i + 1], r2, m.captured(1).simplified());
                used.append({ all[i].end, all[i + 1].start });
                continue;
            }
            const qsizetype tailEnd = i + 2 < all.size() ? all[i + 2].start : f.size();
            const QString tail = f.mid(all[i + 1].end, tailEnd - all[i + 1].end);
            if (sameRelation.match(between).hasMatch() && !newClause.match(tail).hasMatch()) {
                relate(all[lastSubject], all[i + 1], lastRel, lastWords);
                used.append({ all[i].end, all[i + 1].start });
                continue;                                     // « … de l'US 10, de l'US 7 et de l'US 8 » : même sujet
            }
        }
        lastSubject = -1;
    }
    // 2. Positions absolues. Chaque position va à l'unité la plus proche de sa proposition (entre virgules, « et ») ;
    //    sans unité dans la proposition, à la plus proche de la phrase (« dans l'US 3, au fond, un os »).
    QHash<QString, QPair<int, QString>> rank;                // clé -> rang, mots
    QStringList order;                                       // clés dans l'ordre du texte
    static const QRegularExpression clauseSep(QStringLiteral(R"((?:,(?=\s|$)|\bet\b|\band\b|\bpuis\b|\bthen\b))"));
    auto nearest = [](const QVector<Slot>& ms, qsizetype ps, qsizetype pe, qsizetype* dist) -> const Slot* {
        const Slot* best = nullptr;
        for (const Slot& m : ms) {
            const qsizetype d = m.start >= pe ? m.start - pe : (ps >= m.end ? ps - m.end : 0);
            if (!best || d < *dist) { best = &m; *dist = d; }
        }
        return best;
    };
    auto sentenceDone = [&](qsizetype a, qsizetype b) {
        const QString sent = f.mid(a, b - a);
        const QVector<Slot> sms = slotsOf(sent);
        if (sms.isEmpty()) return;
        QVector<QPair<qsizetype, qsizetype>> clauses;
        qsizetype c0 = 0;
        for (auto it = clauseSep.globalMatch(sent); it.hasNext();) {
            const auto m = it.next();
            clauses.append({ c0, m.capturedStart() });
            c0 = m.capturedEnd();
        }
        clauses.append({ c0, sent.size() });
        for (const Position& p : positions(sent)) {
            bool inRelation = false;
            for (const auto& u : std::as_const(used)) if (a + p.start >= u.first && a + p.end <= u.second) inRelation = true;
            for (const Slot& m : sms) if (p.start < m.end && p.end > m.start) inRelation = true;     // mot d'un nom d'Event
            if (inRelation) continue;
            QVector<Slot> local;
            for (const auto& c : std::as_const(clauses))
                if (p.start >= c.first && p.end <= c.second)
                    for (const Slot& m : sms) if (m.start >= c.first && m.end <= c.second) local.append(m);
            qsizetype d = 0;
            const Slot* best = nearest(local.isEmpty() ? sms : local, p.start, p.end, &d);
            if (!best || d > 40) continue;
            for (const QString& k : best->keys) {
                if (!rank.contains(k)) order << k;
                rank[k] = { p.rank, p.words };
            }
        }
    };
    qsizetype from = 0;
    for (auto it = sentenceSep.globalMatch(f); it.hasNext();) {
        const auto m = it.next();
        sentenceDone(from, m.capturedStart());
        from = m.capturedEnd();
    }
    sentenceDone(from, f.size());
    auto knownEitherWay = [](const QVector<Relation>& rs, const QString& a, const QString& b) {
        for (const Relation& r : rs)
            if ((r.older == a && r.younger == b) || (r.older == b && r.younger == a)) return true;
        return false;
    };
    for (int i = 0; i < order.size(); ++i)
        for (int j = 0; j < order.size(); ++j) {
            const auto& a = rank[order[i]];
            const auto& b = rank[order[j]];
            if (a.first < b.first && !knownEitherWay(rels, order[i], order[j]))
                rels.append({ order[i], order[j], QStringLiteral("%1 : %2 ; %3 : %4").arg(order[i], a.second, order[j], b.second) });
        }
    // 3. Position extrême (« à la base de la séquence », « au fond », « tout en haut ») : plus ancienne (ou plus récente)
    //    que toutes les autres unités du texte, sauf relation contraire écrite (« la fosse recoupe le niveau 1 »).
    QStringList allKeys;
    for (const Slot& sl : all) for (const QString& k : sl.keys) if (!allKeys.contains(k)) allKeys << k;
    const QVector<Relation> known = closure(rels);
    for (auto it = rank.cbegin(); it != rank.cend(); ++it) {
        if (it.value().first == 1) continue;
        for (const QString& o : std::as_const(allKeys)) {
            if (o == it.key() || (rank.contains(o) && rank[o].first == it.value().first) || knownEitherWay(known, it.key(), o)) continue;
            const QString ev = QStringLiteral("%1 : %2").arg(it.key(), it.value().second);
            if (it.value().first == 0) rels.append({ it.key(), o, ev });
            else rels.append({ o, it.key(), ev });
        }
    }
    return rels;
}

QVector<Relation> relationsFromTexts(const QStringList& texts, const QStringList& eventNames)
{
    QVector<Relation> rels;
    for (const QString& t : texts)
        for (const Relation& r : relationsFromText(t, eventNames)) addPair(rels, r.older, r.younger, r.evidence);
    return rels;
}

QStringList unitsIn(const QStringList& texts, const QStringList& eventNames)
{
    const QStringList names = searchableNames(eventNames);
    QStringList out;
    for (const QString& t : texts) {
        QString f = text::fold(t);
        f.replace(QChar(0x2019), QLatin1Char('\''));
        for (const Mention& m : mentionsIn(f, names))
            if (!out.contains(m.key)) out << m.key;
    }
    return out;
}

QString unitLabel(const QString& key)
{
    if (key.startsWith(QLatin1String("n:"))) return key.mid(2);
    static const QRegularExpression re(QStringLiteral(R"(^(us|couche|niveau)(\d+[a-z]?)$)"));
    const auto m = re.match(key);
    if (!m.hasMatch()) return key;
    return (m.captured(1) == QLatin1String("us") ? QStringLiteral("US") : m.captured(1)) + QLatin1Char(' ') + m.captured(2).toUpper();
}

QString eventKey(const QString& name)
{
    const QString k = unitKey(name);
    return k.isEmpty() ? nameKey(foldName(name)) : k;
}

QHash<QString, QStringList> phaseMembersFromTexts(const QStringList& texts, const QStringList& eventNames, const QStringList& phaseNames)
{
    QHash<QString, QStringList> out;
    const QStringList names = searchableNames(eventNames);
    static const QRegularExpression sentenceSep(QStringLiteral(R"((?:[;!?\n]|\.(?=\s|$)))"));
    // Unités AVANT le nom de la phase : « US3 et US5 sont dans la même phase "habitation" », « l'US 2 fait partie de la phase A ».
    static const QRegularExpression memberBefore(QStringLiteral(
        R"(\b(?:sont|est|font partie|fait partie|appartiennent|appartient|forment|forme|vont|va|sont regroupee?s|sont mise?s?|)"
        R"(are|is|belong|belongs|form|forms)\b[^.;]*\bphases?\s*['"«»\s]*$)"));
    // Unités APRÈS le nom : « la phase A regroupe l'US 2 et l'US 4 », « phase habitation : US3, US5 ».
    static const QRegularExpression memberAfter(QStringLiteral(
        R"(^\s*['"«»]*\s*(?::|=|contient|contiennent|regroupe|regroupent|comprend|comprennent|rassemble|avec|qui contient|)"
        R"(contains|includes|groups|with)(?![a-z]))"));
    // Une relation stratigraphique dans la même partie de phrase : ce n'est pas une liste de membres.
    static const QRegularExpression relWords(QStringLiteral(
        R"(\b(?:au[ -]dessus|au[ -]dessous|en[ -]dessous|sous|sur|recoup\w*|scell\w*|recouvr\w*|surmont\w*|avant|apres|anterieur\w*|)"
        R"(posterieur\w*|plus ancien\w*|plus recent\w*|above|below|under|over|cuts?|seals?|before|after)\b)"));
    for (const QString& t0 : texts) {
        QString f = text::fold(t0);
        f.replace(QChar(0x2019), QLatin1Char('\''));
        qsizetype from = 0;
        QVector<QPair<qsizetype, qsizetype>> sentences;
        for (auto it = sentenceSep.globalMatch(f); it.hasNext();) { const auto m = it.next(); sentences.append({ from, m.capturedStart() }); from = m.capturedEnd(); }
        sentences.append({ from, f.size() });
        for (const auto& se : std::as_const(sentences)) {
            const QString sent = f.mid(se.first, se.second - se.first);
            if (!sent.contains(QLatin1String("phase"))) continue;
            QString found;
            qsizetype at = -1, len = 0;
            int hits = 0;
            for (const QString& pn : phaseNames) {
                const QString fp = text::fold(pn).trimmed();
                if (fp.isEmpty()) continue;
                const QRegularExpression re(QStringLiteral("(?<![\\w])%1(?![\\w])").arg(QRegularExpression::escape(fp)));
                const auto m = re.match(sent);
                if (!m.hasMatch()) continue;
                ++hits; found = pn; at = m.capturedStart(); len = m.capturedLength();
            }
            if (hits != 1) continue;
            const QString before = sent.left(at), after = sent.mid(at + len);
            QString part;
            if (memberBefore.match(before).hasMatch()) part = before;
            else if (memberAfter.match(after).hasMatch()) part = after;
            if (part.isEmpty() || relWords.match(part).hasMatch()) continue;
            for (const Mention& m : mentionsIn(part, names))
                if (!out[found].contains(m.key)) out[found] << m.key;
        }
    }
    return out;
}

SpecCheck checkSpec(const QJsonObject& specIn, const QStringList& texts, const QStringList& attached)
{
    // Réglages écrits par l'utilisateur et absents du bloc (prompt réel « Teilleul 2 » du 12/11/2026 : période, pas et
    // « pas de recuit » oubliés par le modèle d'IA) : repris tels quels, seulement les clés absentes.
    QJsonObject spec = specIn;
    QStringList pre;
    {
        const QJsonObject st = studySettingsFromTexts(texts);
        QStringList got;
        const QJsonObject sIn = st.value("settings").toObject();
        QJsonObject sOut = spec.value("settings").toObject();
        if (sIn.contains("tmin") && sIn.contains("tmax") && !sOut.contains("tmin") && !sOut.contains("tmax")) {
            sOut["tmin"] = sIn.value("tmin");
            sOut["tmax"] = sIn.value("tmax");
            got << QStringLiteral("période d'étude %1 à %2").arg(sIn.value("tmin").toDouble()).arg(sIn.value("tmax").toDouble());
        }
        if (sIn.contains("step") && !sOut.contains("step")) {
            sOut["step"] = sIn.value("step");
            got << QStringLiteral("pas %1").arg(sIn.value("step").toInt());
        }
        if (sOut != spec.value("settings").toObject()) spec["settings"] = sOut;
        const QJsonObject mIn = st.value("mcmc").toObject();
        QJsonObject mOut = spec.value("mcmc").toObject();
        static const QHash<QString, QString> labels = {
            { "num_iter", QStringLiteral("itérations") }, { "num_burn", QStringLiteral("burn-in") },
            { "num_proc", QStringLiteral("chaînes") }, { "thinning_interval", QStringLiteral("thinning") } };
        for (auto it = mIn.begin(); it != mIn.end(); ++it) {
            if (mOut.contains(it.key())) continue;
            mOut[it.key()] = it.value();
            got << (it.key() == QLatin1String("anneal_recurrence") ? QStringLiteral("sans recuit")
                                                                     : QStringLiteral("%1 %2").arg(labels.value(it.key())).arg(it.value().toDouble()));
        }
        if (mOut != spec.value("mcmc").toObject()) spec["mcmc"] = mOut;
        if (!got.isEmpty())
            pre << QStringLiteral("Réglages repris de votre texte (absents du modèle écrit par l'IA) : %1.").arg(got.join(QStringLiteral(", ")));
    }
    const SpecCheck ad = checkAgeDepth(spec, texts + attached);
    spec = ad.spec;
    pre << ad.notes;
    bool phasesFixed = false;
    // Date historique écrite en borne par le modèle d'IA (règle de Philippe, 13/11/2026) : sauf mention contraire, une
    // date historique (« éruption historique de 1718 ») est une datation uniforme dans l'Event, de l'année à l'année
    // suivante, pas une borne. Converti seulement si une phrase du texte cite le nom de l'Event, l'année et « historique »,
    // sans « borne ».
    bool historicFixed = false;
    {
        static const QRegularExpression sentenceSep(QStringLiteral(R"((?:[;!?\n]|\.(?=\s|$)))"));
        static const QRegularExpression historic(QStringLiteral(R"(\b(?:historique\w*|historical|historic)\b)"));
        static const QRegularExpression boundWord(QStringLiteral(R"(\b(?:borne\w*|bound\w*|known fixed|date fixe)\b)"));
        QStringList sentences;
        for (const QString& t : texts) sentences << text::fold(t).split(sentenceSep, Qt::SkipEmptyParts);
        QJsonArray evs = spec.value("events").toArray();
        for (int i = 0; i < evs.size(); ++i) {
            QJsonObject e = evs[i].toObject();
            if (text::fold(e.value("type").toString()) != QLatin1String("bound")) continue;
            const QJsonValue kf = e.value("known_fixed");
            const double y = kf.isDouble() ? kf.toDouble() : kf.isString() ? text::yearFromText(kf.toString()) : std::nan("");
            if (!std::isfinite(y) || y < 1 || y != std::floor(y)) continue;          // années après J.-C., entières
            const QString name = e.value("name").toString().trimmed();
            const QRegularExpression nameRe(QStringLiteral("(?<![\\w])%1(?![\\w])").arg(QRegularExpression::escape(text::fold(name))));
            const QRegularExpression yearRe(QStringLiteral("(?<![\\d.,])%1(?![\\d.,]\\d)").arg(qint64(y)));
            bool hit = false;
            for (const QString& se : std::as_const(sentences))
                if (nameRe.match(se).hasMatch() && yearRe.match(se).hasMatch() && historic.match(se).hasMatch() && !boundWord.match(se).hasMatch()) {
                    hit = true;
                    break;
                }
            if (!hit) continue;
            e.remove("type");
            e.remove("known_fixed");
            e["dates"] = QJsonArray{ QJsonObject{ { "type", "unif" }, { "min", y }, { "max", y + 1 } } };
            evs[i] = e;
            pre << QStringLiteral("« %1 » : date historique %2 mise en datation uniforme [%2 ; %3] dans l'Event, pas en borne "
                                  "(à valider ou à préciser).").arg(name).arg(qint64(y)).arg(qint64(y) + 1);
            historicFixed = true;
        }
        if (historicFixed) spec["events"] = evs;
    }
    // Contrainte de phases écrite avec des noms d'Events (prompt réel du 13/11/2026 : ["US3", "US5", "habitation"], pour
    // dire que US3 et US5 sont dans la phase) : retirée, l'appartenance s'écrit sur l'événement.
    {
        QSet<QString> phaseKeys, eventKeys;
        for (const QJsonValue& v : spec.value("phases").toArray())
            phaseKeys.insert(text::fold(v.isObject() ? v.toObject().value("name").toString() : v.toString()).trimmed());
        for (const QJsonValue& v : spec.value("events").toArray()) eventKeys.insert(text::fold(v.toObject().value("name").toString()).trimmed());
        QJsonArray keptPc;
        for (const QJsonValue& v : spec.value("phase_constraints").toArray()) {
            QStringList bad, all;
            auto look = [&](const QJsonValue& x) {
                if (!x.isString()) return;
                const QString k = text::fold(x.toString()).trimmed();
                all << x.toString();
                if (!phaseKeys.contains(k) && eventKeys.contains(k)) bad << x.toString();
            };
            if (v.isArray()) for (const QJsonValue& x : v.toArray()) look(x);
            else for (const char* k : { "older", "younger", "before", "after", "a", "b", "from", "to" }) look(v.toObject().value(k));
            if (bad.isEmpty()) { keptPc.append(v); continue; }
            pre << QStringLiteral("Contrainte de phases « %1 » retirée : %2 %3 des événements, pas des phases (l'appartenance à une "
                                  "phase s'écrit sur l'événement).")
                       .arg(all.join(QStringLiteral(", ")), bad.join(QStringLiteral(", ")),
                            bad.size() > 1 ? QStringLiteral("sont") : QStringLiteral("est"));
        }
        if (keptPc.size() != spec.value("phase_constraints").toArray().size()) {
            if (keptPc.isEmpty()) spec.remove("phase_constraints"); else spec["phase_constraints"] = keptPc;
            phasesFixed = true;
        }
    }
    // Phases : Event cité par l'utilisateur dans une phase du modèle mais oublié par le modèle d'IA (prompt réel du
    // 13/11/2026 : « US3 et US5 sont dans la même phase "habitation" », seule US3 y était). Ajout seulement.
    bool phasesChanged = false;
    {
        QStringList evNames, phNames;
        for (const QJsonValue& v : spec.value("events").toArray()) evNames << v.toObject().value("name").toString();
        for (const QJsonValue& v : spec.value("phases").toArray())
            phNames << (v.isObject() ? v.toObject().value("name").toString() : v.toString());
        const QHash<QString, QStringList> members = phaseMembersFromTexts(texts, evNames, phNames);
        QJsonArray evs = spec.value("events").toArray();
        for (auto it = members.cbegin(); it != members.cend(); ++it)
            for (int i = 0; i < evs.size(); ++i) {
                QJsonObject e = evs[i].toObject();
                const QString n = e.value("name").toString().trimmed();
                if (!it.value().contains(eventKey(n))) continue;
                QJsonArray ph = e.value("phases").toArray();
                bool has = false;
                for (const QJsonValue& x : std::as_const(ph)) has = has || text::fold(x.toString()).trimmed() == text::fold(it.key()).trimmed();
                if (has) continue;
                ph.append(it.key());
                e["phases"] = ph;
                evs[i] = e;
                pre << QStringLiteral("« %1 » mis dans la phase « %2 » d'après votre texte (oublié par le modèle d'IA).").arg(n, it.key());
                phasesChanged = true;
            }
        if (phasesChanged) spec["events"] = evs;
    }

    QStringList names;
    for (const QJsonValue& v : spec.value("events").toArray()) names << v.toObject().value("name").toString();
    SpecCheck R = checkSpec(spec, relationsFromTexts(texts, names), unitsIn(texts, names), texts.join(QLatin1Char('\n')));
    if (!pre.isEmpty() || ad.changed || phasesChanged || phasesFixed || historicFixed) {
        R.notes = pre + R.notes;
        R.changed = true;
    }
    return R;
}

namespace {

QString foldQuotes(const QString& t)
{
    QString f = text::fold(t);
    f.replace(QChar(0x2019), QLatin1Char('\''));
    f.replace(QChar(0x2212), QLatin1Char('-'));        // signe moins typographique
    return f;
}

double yearOf(const QJsonValue& v)
{
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) return text::yearFromText(v.toString());
    return std::nan("");
}

bool isAgeDepthSpec(const QJsonObject& spec)
{
    const QString c = text::fold(spec.value("curve").toString());
    if (c.contains(QLatin1String("depth")) || c.contains(QLatin1String("profondeur")) || c.contains(QLatin1String("niveau"))
        || c.contains(QLatin1String("level")))
        return true;
    for (const QJsonValue& v : spec.value("events").toArray())
        if (v.toObject().contains("depth") || v.toObject().contains("profondeur")) return true;
    return false;
}

QString fmtYear(double y) { return QString::number(y, 'g', 10); }

} // namespace

bool superpositionStated(const QString& text)
{
    const QString f = foldQuotes(text);
    static const QRegularExpression denied(QStringLiteral(
        R"(\b(?:pas|non|plus|ni)\s+(?:en\s+|dans\s+l'|selon\s+l'|d')?ordre\s+stratigraphique\b|\bordre\s+stratigraphique\s+(?:invers|perturb|remani|incertain|inconnu))"
        R"(|\bremani\w*|\bperturb\w*|\binvers\w*|\bnot\s+in\s+stratigraphic\s+order\b|\breworked\b|\bdisturbed\b)"));
    if (denied.match(f).hasMatch()) return false;
    static const QRegularExpression re(QStringLiteral(
        R"(\bchacun(?:e)?s?\s+(?:est\s+|etant\s+|sont\s+)?plus\s+ancien(?:ne)?s?\s+que\s+(?:celui|celle|ceux|celles|le\s+niveau|la\s+couche)\s+(?:du|d'au|de|d'en)\s*[- ]?dessus\b)"
        R"(|\bchacun(?:e)?s?\s+(?:est\s+|etant\s+|sont\s+)?plus\s+recent(?:e)?s?\s+que\s+(?:celui|celle|ceux|celles|le\s+niveau|la\s+couche)\s+(?:du|d'en|de)\s*[- ]?dessous\b)"
        R"(|\b(?:du|de\s+la|des)\s+plus\s+recent(?:e|s|es)?\s+(?:au|a\s+la|aux)\s+plus\s+ancien)"
        R"(|\b(?:du|de\s+la|des)\s+plus\s+ancien(?:ne|s|nes)?\s+(?:au|a\s+la|aux)\s+plus\s+recent)"
        R"(|\bordre\s+stratigraphique\b)"
        R"(|\b(?:loi\s+de\s+(?:la\s+)?superposition|superposition\s+normale|stratigraphie\s+(?:normale|en\s+place|continue))\b)"
        R"(|\b(?:le|la|les)\s+plus\s+profond(?:e|s|es)?\s+(?:est|sont)\s+(?:le|la|les)\s+plus\s+ancien)"
        R"(|\bplus\s+(?:c'est\s+|on\s+descend\s*,?\s*|on\s+est\s+)?profond\w*\s*,?\s*plus\s+(?:c'est\s+|on\s+est\s+)?ancien)"
        R"(|\bplus\s+on\s+descend\s*,?\s*plus\s+(?:c'est\s+)?ancien)"
        R"(|\beach\s+(?:\w+\s+)?(?:is\s+|being\s+)?older\s+than\s+the\s+(?:one|\w+)\s+above\b)"
        R"(|\beach\s+(?:\w+\s+)?(?:is\s+|being\s+)?younger\s+than\s+the\s+(?:one|\w+)\s+below\b)"
        R"(|\bfrom\s+(?:the\s+)?youngest\s+to\s+(?:the\s+)?oldest\b|\bfrom\s+(?:the\s+)?oldest\s+to\s+(?:the\s+)?youngest\b)"
        R"(|\bstratigraphic\s+order\b|\bdeeper\s+(?:is|are|means)\s+older\b|\blaw\s+of\s+superposition\b)"
        // Convention de ChronoModel écrite telle quelle : « la profondeur la plus petite est la plus ancienne ».
        R"(|(?:profondeur\s+la\s+plus\s+(?:petite|basse|faible)|plus\s+(?:petite|basse|faible)\s+profondeur)[^.\n]{0,30}?plus\s+(?:vieu|ancien))"
        R"(|\b(?:smallest|lowest)\s+depth\s+(?:is|=)\s+(?:the\s+)?oldest\b)"));
    return re.match(f).hasMatch();
}

QJsonObject studySettingsFromTexts(const QStringList& texts)
{
    QJsonObject settings, mcmc;
    for (int i = int(texts.size()) - 1; i >= 0; --i) {
        const QString f = foldQuotes(texts[i]);
        if (!settings.contains("tmin")) {
            // « période (d'étude) de -1500 à 2000 », « study period from -1000 to 500 » ; pas un âge BP ni en ka.
            static const QRegularExpression period(QStringLiteral(
                R"(\b(?:periode(?:\s+d'etude)?|study\s+period|period|intervalle\s+d'etude)\s*(?:de|du|from|of|:|=|entre|between|allant\s+de)?\s*)"
                R"((-?\d+)\s*(?:a|au|-|–|→|et|to|and)\s*(-?\d+)\b(?!\s*(?:cal\s*)?(?:bp|b2k|ka|ma)\b))"));
            const auto m = period.match(f);
            if (m.hasMatch() && m.captured(1).toDouble() < m.captured(2).toDouble()) {
                settings["tmin"] = m.captured(1).toDouble();
                settings["tmax"] = m.captured(2).toDouble();
            }
        }
        if (!settings.contains("step")) {
            static const QRegularExpression stepOne(QStringLiteral(R"(\bau\s+pas\s+(?:d'(?:un|une)|de\s+(?:un|une))\s+(?:an|annee)\b|\bstep\s+of\s+(?:one|1)\s+year\b)"));
            static const QRegularExpression stepN(QStringLiteral(R"(\bau\s+pas\s+(?:de\s+)?(\d+)\s*(?:ans?|annees?)?\b|\bstep\s*(?:of|=|:)?\s*(\d+)\b)"));
            if (stepOne.match(f).hasMatch()) settings["step"] = 1;
            else if (const auto m = stepN.match(f); m.hasMatch())
                settings["step"] = (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1)).toInt();
        }
        const QJsonObject m = MethodRunner::mcmcFromQuestion(texts[i]);
        for (const char* k : { "anneal_recurrence", "num_iter", "num_burn", "num_proc", "thinning_interval" })
            if (m.contains(k) && !mcmc.contains(k)) mcmc[k] = m.value(k);
    }
    QJsonObject out;
    if (!settings.isEmpty()) out["settings"] = settings;
    if (!mcmc.isEmpty()) out["mcmc"] = mcmc;
    return out;
}

QVector<CsvDepthRow> chronoModelCsvRows(const QString& text)
{
    QVector<CsvDepthRow> rows;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r?\n")));
    static const QRegularExpression header(QStringLiteral(R"(^\s*method\s*([;,\t])\s*dating\s+name)"),
                                           QRegularExpression::CaseInsensitiveOption);
    int h = -1;
    QChar sep;
    for (int i = 0; i < lines.size() && h < 0; ++i)
        if (const auto m = header.match(lines[i]); m.hasMatch()) { h = i; sep = m.captured(1).at(0); }
    if (h < 0) return rows;
    const QStringList head = lines[h].split(sep);
    int cDepth = -1, cErr = -1;
    for (int c = 0; c < head.size(); ++c) {
        const QString k = text::fold(head[c]).simplified();
        if (k == QLatin1String("depth") || k == QLatin1String("profondeur")) cDepth = c;
        else if (k == QLatin1String("err depth") || k == QLatin1String("error depth") || k == QLatin1String("err profondeur")) cErr = c;
    }
    auto num = [](QString x, bool* ok) {
        x = x.trimmed().replace(QChar(0x2212), QLatin1Char('-')).replace(QLatin1Char(','), QLatin1Char('.'));
        return x.toDouble(ok);
    };
    for (int i = h + 1; i < lines.size(); ++i) {
        const QStringList f = lines[i].split(sep);
        if (f.size() < 3 || f[0].trimmed().isEmpty()) {
            if (lines[i].trimmed().isEmpty()) continue;
            break;                                         // fin du tableau
        }
        CsvDepthRow r;
        r.method = text::fold(f[0]).trimmed();
        r.name = f.value(1).trimmed();
        bool ok = false;
        r.a = num(f.value(2), &ok);
        r.b = num(f.value(3), &ok);
        if (cDepth >= 0) {
            r.depth = num(f.value(cDepth), &r.hasDepth);
            bool okE = false;
            const double e = num(f.value(cErr), &okE);
            r.depthError = okE ? std::abs(e) : 0.0;
        }
        rows << r;
    }
    return rows;
}

SpecCheck checkAgeDepth(const QJsonObject& specIn, const QStringList& texts)
{
    SpecCheck R;
    R.spec = specIn;
    QJsonObject& spec = R.spec;
    QJsonArray events = spec.value("events").toArray();

    // 0. Profondeurs du tableau CSV de ChronoModel : recopiées avec leur signe (Philippe, 12/11/2026 : « il faut garder le
    //    signe du tableau » ; parfois 0 au fond et croissante avec le temps, la plus petite reste la plus ancienne).
    QVector<CsvDepthRow> csv;
    for (const QString& t : texts) { const auto r = chronoModelCsvRows(t); if (!r.isEmpty()) csv = r; }
    bool csvDepths = false;
    for (const CsvDepthRow& r : std::as_const(csv)) csvDepths = csvDepths || r.hasDepth;
    if (!csvDepths && !isAgeDepthSpec(specIn)) return R;
    if (csvDepths) {
        // Event d'une ligne : mêmes valeurs de date (âge et erreur, min et max), sinon même rang dans le tableau.
        auto dateMatches = [](const QJsonObject& e, const CsvDepthRow& r) {
            for (const QJsonValue& dv : e.value("dates").toArray()) {
                const QJsonObject d = dv.toObject();
                const QString ty = text::fold((d.contains("type") ? d.value("type") : d.value("method")).toString());
                const bool unifRow = r.method.startsWith(QLatin1String("unif"));
                const double x = yearOf(unifRow ? d.value("min") : d.value("age"));
                const double y = unifRow ? yearOf(d.value("max")) : yearOf(d.value("error"));
                if (unifRow != ty.startsWith(QLatin1String("unif"))) continue;
                if (x == r.a && y == r.b) return true;
            }
            return false;
        };
        QVector<int> rowOf(events.size(), -1);
        QVector<bool> used(csv.size(), false);
        for (int i = 0; i < events.size(); ++i)
            for (int k = 0; k < csv.size(); ++k)
                if (!used[k] && csv[k].hasDepth && dateMatches(events[i].toObject(), csv[k])) { rowOf[i] = k; used[k] = true; break; }
        if (events.size() == csv.size())
            for (int i = 0; i < events.size(); ++i)
                if (rowOf[i] < 0 && !used[i] && csv[i].hasDepth) { rowOf[i] = i; used[i] = true; }
        int changedDepths = 0;
        for (int i = 0; i < events.size(); ++i) {
            if (rowOf[i] < 0) continue;
            const CsvDepthRow& r = csv[rowOf[i]];
            QJsonObject e = events[i].toObject();
            const double before = yearOf(e.contains("depth") ? e.value("depth") : e.value("profondeur"));
            if (before != r.depth) ++changedDepths;
            e.remove("profondeur");
            e["depth"] = r.depth;
            if (r.depthError > 0 || !e.contains("depth_error")) e["depth_error"] = r.depthError;
            events[i] = e;
        }
        spec["events"] = events;
        if (spec.value("depth_sign").toString() != QLatin1String("as_given")) spec["depth_sign"] = QStringLiteral("as_given");
        if (spec.value("curve").toString().isEmpty()) spec["curve"] = QStringLiteral("age_depth");
        if (changedDepths > 0) {
            R.notes << QStringLiteral("Profondeurs reprises du tableau telles quelles, avec leur signe (%1 corrigée%2) : pour ChronoModel, "
                                      "la profondeur la plus petite est l'Event le plus ancien.")
                           .arg(changedDepths).arg(changedDepths > 1 ? QStringLiteral("s") : QString());
            R.changed = true;
        } else if (spec != specIn) R.changed = true;
    } else {
        // Profondeur mesurée depuis la base : gardée telle quelle (la plus petite reste la plus ancienne).
        static const QRegularExpression fromBase(QStringLiteral(
            R"(\bhauteurs?\b|\bdepuis\s+(?:la\s+)?base\b|\b0\s*(?:cm|m|mm)?\s*(?:=|:|est|au|a\s+la)\s*(?:le\s+|la\s+)?(?:fond|base)\b)"
            R"(|\bprofondeurs?\s+(?:qui\s+)?(?:augmente|croit|croissante)\w*\s+avec\s+le\s+temps\b|\bheight\s+above\b|\bfrom\s+the\s+base\b)"));
        bool base = false;
        for (const QString& t : texts) base = base || fromBase.match(foldQuotes(t)).hasMatch();
        if (base && spec.value("depth_sign").toString() != QLatin1String("as_given")) {
            spec["depth_sign"] = QStringLiteral("as_given");
            R.notes << QStringLiteral("Profondeurs mesurées depuis la base : gardées telles quelles (pour ChronoModel, la plus petite est "
                                      "l'Event le plus ancien).");
            R.changed = true;
        }
    }

    // Niveaux avec une profondeur, dans la convention de ChronoModel (X_Inc_Depth) : de la plus basse (Event le plus
    // ancien) à la plus haute (le plus récent, la surface).
    const bool depthsSigned = ChrBuilder::signedDepths(spec);
    QVector<QPair<double, int>> byDepth;
    for (int i = 0; i < events.size(); ++i) {
        const double d = ChrBuilder::chronoDepth(events[i].toObject(), depthsSigned);
        if (!std::isnan(d)) byDepth.append({ d, i });
    }
    std::stable_sort(byDepth.begin(), byDepth.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (byDepth.isEmpty()) return R;

    // 1. Surface : le présent de l'étude, borne à la fin de la période d'étude.
    {
        QJsonObject top = events[byDepth.last().second].toObject();
        const QString name = top.value("name").toString().trimmed();
        static const QRegularExpression surfName(QStringLiteral(
            R"(\b(?:surface|sommet|top|actuel(?:le)?|present|sol\s+actuel|haut\s+de\s+(?:la\s+)?carotte)\b)"));
        static const QRegularExpression current(QStringLiteral(
            R"(\b(?:niveau|sol|surface)\s+actuel(?:le)?\b|\bc'est\s+(?:le\s+)?(?:niveau\s+)?(?:actuel|present)\b|\bpresent[\s-]day\b|\bcurrent\s+(?:level|surface)\b|\bmodern\s+surface\b)"));
        bool isSurface = surfName.match(foldQuotes(name)).hasMatch();
        for (const QString& t : texts) isSurface = isSurface || current.match(foldQuotes(t)).hasMatch();
        const bool isBoundAlready = top.value("type").toString().toLower() == QLatin1String("bound");
        const QJsonArray ds = top.value("dates").toArray();
        QJsonObject unif;
        if (ds.size() == 1) {
            const QJsonObject d0 = ds.first().toObject();
            const QString ty = text::fold((d0.contains("type") ? d0.value("type") : d0.value("method")).toString().trimmed());
            if (ty == QLatin1String("unif") || ty == QLatin1String("uniform") || ty == QLatin1String("uniforme")) unif = d0;
        }
        const double tmax = yearOf(spec.value("settings").toObject().value("tmax"));
        const double umax = unif.isEmpty() ? std::nan("") : yearOf(unif.value("max"));
        const double umin = unif.isEmpty() ? std::nan("") : yearOf(unif.value("min"));
        // Intervalle qui va jusqu'à la fin de la période (ou période non donnée) : c'est le présent, pas une datation.
        const bool toEnd = !std::isnan(umax) && (std::isnan(tmax) || umax >= tmax);
        if (isSurface && !isBoundAlready && (ds.isEmpty() || !unif.isEmpty()) && (toEnd || (ds.isEmpty() && !std::isnan(tmax)))) {
            const double y = std::isnan(tmax) ? umax : tmax;
            top.remove("dates");
            top["type"] = QStringLiteral("bound");
            top["known_fixed"] = y;
            events[byDepth.last().second] = top;
            spec["events"] = events;
            R.notes << (unif.isEmpty()
                ? QStringLiteral("« %1 » (surface, niveau actuel) : borne à %2, fin de la période d'étude.").arg(name, fmtYear(y))
                : QStringLiteral("« %1 » (surface, niveau actuel) : borne à %2, fin de la période d'étude, au lieu d'un intervalle "
                                 "uniforme de %3 à %4 : la surface est le présent de l'étude, pas un niveau à dater.")
                      .arg(name, fmtYear(y), fmtYear(umin), fmtYear(umax)));
            R.changed = true;
        }
    }

    // 2. Stratigraphie : ordre de superposition écrit par l'utilisateur -> séquence des profondeurs.
    bool stated = false;
    for (const QString& t : texts) stated = stated || superpositionStated(t);
    if (!stated || byDepth.size() < 2) return R;
    for (int i = 1; i < byDepth.size(); ++i)
        if (byDepth[i].first == byDepth[i - 1].first) return R;     // même profondeur : ordre non défini, ChronoModel le traite
    QStringList seq;                                                  // de la profondeur la plus basse (plus ancien) à la plus haute
    for (const auto& pr : std::as_const(byDepth)) seq << events[pr.second].toObject().value("name").toString().trimmed();

    // Ordre déjà donné par les contraintes du modèle d'IA (lues par ChrBuilder, transitives) ?
    bool same = false;
    {
        QJsonObject probe = spec;
        const ChrResult built = ChrBuilder::build(probe);
        if (built.ok && !spec.value("event_constraints").toArray().isEmpty()) {
            QHash<int, QString> nameOf;
            for (const QJsonValue& v : built.chr.value("events").toArray())
                nameOf.insert(v.toObject().value("id").toInt(), v.toObject().value("name").toString().trimmed());
            QVector<QPair<QString, QString>> pairs;
            for (const QJsonValue& v : built.chr.value("events_constraints").toArray()) {
                const QJsonObject c = v.toObject();
                pairs.append({ nameOf.value(c.value(ChrBuilder::kBwdIsOlder ? "bwd_id" : "fwd_id").toInt()),
                               nameOf.value(c.value(ChrBuilder::kBwdIsOlder ? "fwd_id" : "bwd_id").toInt()) });
            }
            auto olderThan = [&](const QString& a, const QString& b) {
                QSet<QString> seen;
                std::function<bool(const QString&)> go = [&](const QString& n) {
                    if (seen.contains(n)) return false;
                    seen.insert(n);
                    for (const auto& p : std::as_const(pairs))
                        if (p.first == n && (p.second == b || go(p.second))) return true;
                    return false;
                };
                return go(a);
            };
            same = true;
            for (int i = 1; i < seq.size() && same; ++i) same = olderThan(seq[i - 1], seq[i]) && !olderThan(seq[i], seq[i - 1]);
        }
    }
    if (same) return R;
    const bool had = !spec.value("event_constraints").toArray().isEmpty();
    spec["event_constraints"] = QJsonArray{ QJsonValue(QJsonArray::fromStringList(seq)) };   // QJsonValue : sinon QJsonArray{ QJsonArray } copie (liste plate)
    R.notes << QStringLiteral("Ordre des niveaux %1 d'après votre texte et les profondeurs : de la profondeur la plus basse, "
                              "l'Event le plus ancien (« %2 »), à la plus haute (« %3 »).")
                   .arg(had ? QStringLiteral("corrigé") : QStringLiteral("ajouté"), seq.first(), seq.last());
    R.changed = true;
    return R;
}

// Fermeture transitive des couples du texte (« A sous B », « B sous C » -> A plus ancien que C), avec la chaîne en preuve.
QVector<Relation> closure(const QVector<Relation>& rels)
{
    QVector<Relation> out = rels;
    for (bool grew = true; grew;) {
        grew = false;
        const QVector<Relation> cur = out;
        for (const Relation& a : cur)
            for (const Relation& b : cur) {
                if (a.younger != b.older || a.older == b.younger) continue;
                bool known = false;
                for (const Relation& r : std::as_const(out))
                    if ((r.older == a.older && r.younger == b.younger) || (r.older == b.younger && r.younger == a.older)) known = true;
                if (known) continue;
                out.append({ a.older, b.younger, a.evidence + QStringLiteral(" ; ") + b.evidence });
                grew = true;
            }
    }
    return out;
}

SpecCheck checkSpec(const QJsonObject& specIn, const QVector<Relation>& relsIn, const QStringList& textUnits, const QString& source)
{
    const QVector<Relation> rels = closure(relsIn);
    SpecCheck R;
    R.spec = specIn;
    QJsonObject& spec = R.spec;

    // Unités nommées par l'utilisateur, sans date ni borne : pas des Events du modèle (elles se datent avec les traces).
    QSet<QString> removed;
    QHash<QString, QString> inventedBound;                  // nom -> valeur de la borne absente du texte
    // Borne d'une unité nommée dont l'année n'est pas dans le texte de l'utilisateur : inventée par le modèle d'IA pour
    // l'US à dater (prompt réel du 12/11/2026 : « US2 », borne à -1). Une borne vient d'une ANNÉE écrite : N, 1 − N
    // (N av. J.-C.), 1950 − N (N BP). Jamais d'une mesure « valeur ± erreur » : un âge 14C de 4520 ± 40 BP n'est pas la
    // borne -4520 (prompt réel du 12/11/2026, 15 h 14 : « Level 3 », borne -4520 acceptée avant cette règle).
    QString masked = text::fold(source);
    {
        const QVector<text::ValueError> ms = text::findValueErrors(masked);
        for (int i = int(ms.size()) - 1; i >= 0; --i) masked.replace(ms[i].start, ms[i].length, QStringLiteral(" "));
    }
    const seldon::provenance::NumberPool pool(masked, false);
    auto sourced = [&](const QJsonValue& v) {
        QVector<double> xs;
        if (v.isDouble()) xs << v.toDouble();
        static const QRegularExpression num(QStringLiteral(R"(-?\d+(?:[.,]\d+)?)"));
        for (auto it = num.globalMatch(v.toString()); it.hasNext();) xs << text::toNumber(it.next().captured(0));
        for (double x : std::as_const(xs))
            if (pool.contains(x) || pool.contains(1 - x) || pool.contains(1950 - x)) return true;
        return false;
    };
    QJsonArray kept, probe;                                 // probe : Events sans date munis d'une date factice, pour lire les contraintes
    for (const QJsonValue& v : spec.value("events").toArray()) {
        QJsonObject e = v.toObject();
        const QString name = e.value("name").toString().trimmed();
        const QString key = eventKey(name);
        const bool invented = !source.isEmpty() && isBound(e) && !sourced(e.value("known_fixed"));
        if (textUnits.contains(key) && ((!isBound(e) && !hasDate(e)) || invented)) {
            removed.insert(name);
            if (invented) {
                const QJsonValue kf = e.value("known_fixed");
                inventedBound.insert(name, kf.isDouble() ? QString::number(kf.toDouble()) : kf.toString());
                e.remove("known_fixed");
                e["type"] = QStringLiteral("event");
            }
            e["dates"] = QJsonArray{ QJsonObject{ { "type", "unif" }, { "min", 0 }, { "max", 1 } } };
            probe.append(e);
            continue;
        }
        kept.append(v);
        probe.append(v);
    }
    if (removed.isEmpty() && rels.isEmpty()) return R;

    // Contraintes du modèle lues par ChrBuilder (mêmes règles que le .chr), avec les Events retirés encore présents.
    QJsonObject probeSpec = spec;
    probeSpec["events"] = probe;
    probeSpec.remove("phase_constraints");                  // une contrainte de phases fautive ne bloque pas l'ordre des Events
    const ChrResult built = ChrBuilder::build(probeSpec);
    if (!built.ok) {
        if (removed.isEmpty()) return R;                    // erreurs du modèle : ChrBuilder les montrera
        // Repli : retrait simple, sans relecture des contraintes (une séquence garde les autres noms).
        spec["events"] = kept;
        QJsonArray cons;
        for (const QJsonValue& v : spec.value("event_constraints").toArray()) {
            QJsonArray k;
            bool hit = false;
            for (const QJsonValue& x : v.toArray()) { if (removed.contains(x.toString().trimmed())) hit = true; else k.append(x); }
            if (!hit) cons.append(v);
        }
        spec["event_constraints"] = cons;
        R.changed = true;
    } else {
        QHash<int, QString> nameOf;
        for (const QJsonValue& v : built.chr.value("events").toArray())
            nameOf.insert(v.toObject().value("id").toInt(), v.toObject().value("name").toString().trimmed());
        QVector<QPair<QString, QString>> pairs;             // (plus ancien, plus récent), tous les Events
        for (const QJsonValue& v : built.chr.value("events_constraints").toArray()) {
            const QJsonObject c = v.toObject();
            pairs.append({ nameOf.value(c.value(ChrBuilder::kBwdIsOlder ? "bwd_id" : "fwd_id").toInt()),
                           nameOf.value(c.value(ChrBuilder::kBwdIsOlder ? "fwd_id" : "bwd_id").toInt()) });
        }
        auto olderThan = [&](const QString& a, const QString& b) {    // a plus ancien que b (transitif)
            QSet<QString> seen;
            std::function<bool(const QString&)> go = [&](const QString& n) {
                if (seen.contains(n)) return false;
                seen.insert(n);
                for (const auto& p : std::as_const(pairs))
                    if (p.first == n && (p.second == b || go(p.second))) return true;
                return false;
            };
            return go(a);
        };
        QStringList names;
        QHash<QString, QString> nameOfKey;                  // clé d'unité -> nom de l'Event gardé
        for (const QJsonValue& v : std::as_const(kept)) {
            const QString n = v.toObject().value("name").toString().trimmed();
            names << n;
            const QString k = eventKey(n);
            if (!nameOfKey.contains(k)) nameOfKey.insert(k, n);
        }
        // Unités à dater : Events retirés, et unités du texte ordonnées mais absentes du modèle ; voisines d'après le texte.
        {
            auto textOlder = [&](const QString& a, const QString& b) {
                for (const Relation& r : rels) if (r.older == a && r.younger == b) return true;
                return false;
            };
            QStringList keys, labels;
            QStringList gone(removed.begin(), removed.end());
            gone.sort();
            for (const QString& n : std::as_const(gone)) { keys << eventKey(n); labels << n; }
            for (const Relation& r : rels)
                for (const QString& k : { r.older, r.younger })
                    if (!nameOfKey.contains(k) && !keys.contains(k)) { keys << k; labels << unitLabel(k); }
            for (int i = 0; i < keys.size(); ++i) {
                QStringList below, above;
                for (auto it = nameOfKey.cbegin(); it != nameOfKey.cend(); ++it) {
                    if (textOlder(it.key(), keys[i])) below << it.key();
                    if (textOlder(keys[i], it.key())) above << it.key();
                }
                QString o, y;
                for (const QString& b : std::as_const(below)) {
                    bool closest = true;
                    for (const QString& c : std::as_const(below)) if (c != b && textOlder(b, c)) closest = false;
                    if (closest) { o = nameOfKey[b]; break; }
                }
                for (const QString& a : std::as_const(above)) {
                    bool closest = true;
                    for (const QString& c : std::as_const(above)) if (c != a && textOlder(c, a)) closest = false;
                    if (closest) { y = nameOfKey[a]; break; }
                }
                if (!o.isEmpty() || !y.isEmpty()) R.undated.append({ labels[i], o, y });
            }
        }
        // Couples du texte entre deux Events gardés : contredits, absents.
        QVector<QPair<QString, QString>> wanted;
        QStringList contradicted, missing;
        QStringList contradictedDirect, missingDirect;      // sans les couples déduits par transitivité (note plus courte)
        QVector<QPair<QString, QString>> all;
        for (const Relation& r : rels)
            if (nameOfKey.contains(r.older) && nameOfKey.contains(r.younger)) all.append({ nameOfKey[r.older], nameOfKey[r.younger] });
        // Couple déduit d'autres couples du texte (A < C donné par A < B < C) : pas une contrainte de plus (prompt réel du
        // 13/11/2026 : US1 → US10 ajoutée en plus de US1 → US6 → US10).
        auto implied = [&](const QString& o, const QString& y) {
            for (const auto& p : std::as_const(all))
                if (p.first == o && p.second != y && all.contains(qMakePair(p.second, y))) return true;
            return false;
        };
        for (const Relation& r : rels) {
            if (!nameOfKey.contains(r.older) || !nameOfKey.contains(r.younger)) continue;
            const QString o = nameOfKey[r.older], y = nameOfKey[r.younger];
            const bool direct = !implied(o, y);
            if (direct) wanted.append({ o, y });
            const QString line = QStringLiteral("« %1 » plus ancien que « %2 » (%3)").arg(o, y, r.evidence);
            if (olderThan(y, o)) { contradicted << line; if (direct) contradictedDirect << line; }
            else if (!olderThan(o, y)) { missing << line; if (direct) missingDirect << line; }
        }
        if (!contradictedDirect.isEmpty()) contradicted = contradictedDirect;
        if (!missingDirect.isEmpty()) missing = missingDirect;
        if (!removed.isEmpty() || !contradicted.isEmpty() || !missing.isEmpty()) {
            QSet<QString> ordered;                          // Events dont l'ordre vient du texte
            for (const auto& p : std::as_const(wanted)) { ordered.insert(p.first); ordered.insert(p.second); }
            auto textOlderName = [&](const QString& o, const QString& y) {    // le texte (fermeture) dit o plus ancien que y
                const QString ko = eventKey(o), ky = eventKey(y);
                for (const Relation& r : rels) if (r.older == ko && r.younger == ky) return true;
                return false;
            };
            // Ordre du modèle entre Events gardés (à travers les retirés), réduit aux relations directes. Une contrainte du
            // modèle entre deux Events ordonnés par le texte est GARDÉE si le texte ne la contredit pas (prompt réel du
            // 13/11/2026, Teilleul 2 : « au fond, à 106 cm » ordonnait tous les niveaux, et la séquence des profondeurs
            // était jetée) ; repli : toutes retirées si l'ensemble forme un cycle.
            auto assemble = [&](bool keepConsistent) {
                QJsonArray cons;
                for (const QString& a : std::as_const(names))
                    for (const QString& b : std::as_const(names)) {
                        if (a == b || !olderThan(a, b) || wanted.contains(qMakePair(a, b))) continue;
                        if (ordered.contains(a) && ordered.contains(b) && (!keepConsistent || textOlderName(b, a))) continue;
                        if (keepConsistent && textOlderName(b, a)) continue;
                        bool direct = true;
                        for (const QString& c : std::as_const(names))
                            if (c != a && c != b && olderThan(a, c) && olderThan(c, b)) { direct = false; break; }
                        if (direct) cons.append(QJsonArray{ a, b });     // séquence de deux : du plus ancien au plus récent
                    }
                for (const auto& p : std::as_const(wanted)) cons.append(QJsonArray{ p.first, p.second });
                return cons;
            };
            // Courbe âge-profondeur : la séquence vient des profondeurs (checkAgeDepth), elle est gardée si le texte ne la
            // contredit pas. Autres modèles : seul l'ordre du texte vaut entre les Events qu'il ordonne (une contrainte du
            // modèle d'IA absente du texte n'est pas gardée).
            QJsonArray cons = assemble(isAgeDepthSpec(spec));
            {
                QJsonObject trial = spec;
                trial["events"] = kept;
                trial["event_constraints"] = cons;
                trial.remove("phase_constraints");
                if (ChrBuilder::build(trial).errors.join(QLatin1Char(' ')).contains(QLatin1String("cycle"))) cons = assemble(false);
            }
            spec["events"] = kept;
            spec["event_constraints"] = cons;
            if (!contradicted.isEmpty())
                R.notes << QStringLiteral("Contraintes corrigées d'après votre texte (le modèle d'IA avait écrit l'ordre inverse) : %1.")
                               .arg(contradicted.join(QStringLiteral(" ; ")));
            if (!missing.isEmpty())
                R.notes << QStringLiteral("Contraintes ajoutées d'après votre texte : %1.").arg(missing.join(QStringLiteral(" ; ")));
            R.changed = true;
        }
    }
    QStringList gone(removed.begin(), removed.end());
    gone.sort();
    for (const QString& n : std::as_const(gone))
        R.notes.prepend(inventedBound.contains(n)
            ? QStringLiteral("« %1 » : borne à %2, valeur absente de votre texte (inventée par le modèle d'IA) ; retirée du modèle. "
                             "Une US sans date n'est ni un Event ni une borne : elle se date ensuite avec les traces de ChronoModel "
                             "(par exemple « date entre » ses deux voisines).").arg(n, inventedBound.value(n))
            : QStringLiteral("« %1 » n'a pas de date : ce n'est pas un Event du modèle. Elle se date ensuite avec les traces "
                             "de ChronoModel (par exemple « date entre » ses deux voisines).").arg(n));
    return R;
}

QString replaceModelBlock(const QString& md, const QJsonObject& spec)
{
    static const QRegularExpression re(QStringLiteral("^[ \\t]*```[ \\t]*(chronomodel|json)[ \\t]*\\n(.*?)^[ \\t]*```"),
                                       QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption
                                       | QRegularExpression::CaseInsensitiveOption);
    for (auto it = re.globalMatch(md); it.hasNext();) {
        const auto m = it.next();
        const QJsonDocument d = QJsonDocument::fromJson(m.captured(2).toUtf8());
        if (!d.isObject() || !d.object().contains("events")) continue;
        QString out = md;
        out.replace(m.capturedStart(2), m.capturedLength(2), QString::fromUtf8(QJsonDocument(spec).toJson(QJsonDocument::Indented)));
        return out;
    }
    return md;
}

} // namespace strat
} // namespace seldon
