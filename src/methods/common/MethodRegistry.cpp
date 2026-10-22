#include "MethodRegistry.h"

#include "../am/AmMethod.h"
#include "../c14/C14Method.h"
#include "../f14c/F14cMethod.h"
#include "../gauss/GaussMethod.h"
#include "../tl/TlMethod.h"
#include "../unif/UnifMethod.h"

#include "TextUtil.h"

#include <QRegularExpression>
#include <algorithm>
#include <climits>

namespace seldon {

MethodRegistry::MethodRegistry()
{
    // Ordre = priorité de reconnaissance (du plus spécifique au plus général).
    methods_ << new F14cMethod << new TlMethod << new AmMethod << new UnifMethod << new GaussMethod << new C14Method;
}

const MethodRegistry& MethodRegistry::instance()
{
    static const MethodRegistry r;
    return r;
}

const Method* MethodRegistry::byId(const QString& id) const
{
    for (const Method* m : methods_)
        if (m->id() == id) return m;
    return nullptr;
}

QVector<QJsonObject> MethodRegistry::parse(const QString& question) const
{
    const QString q = text::fold(question);

    // Mots-clés présents, par méthode.
    struct Hit { int start, end, method; };
    QVector<Hit> hits;
    QVector<int> present;
    for (int i = 0; i < methods_.size(); ++i) {
        const QRegularExpression re = methods_[i]->context();
        if (re.pattern().isEmpty()) continue;
        bool any = false;
        auto it = re.globalMatch(q);
        while (it.hasNext()) {
            const auto m = it.next();
            if (m.capturedLength() == 0) continue;
            hits.append({ int(m.capturedStart()), int(m.capturedEnd()), i });
            any = true;
        }
        if (any) present << i;
    }

    // Une seule méthode citée (cas courant) ou aucune : la première méthode qui reconnaît quelque chose l'emporte.
    if (present.size() <= 1) {
        for (const Method* m : methods_) {
            QVector<QJsonObject> v = m->parse(question);
            if (!v.isEmpty()) {
                for (QJsonObject& o : v) o["method"] = m->id();
                return v;
            }
        }
        return {};
    }

    // Plusieurs méthodes. Un mot-clé recouvert par celui d'une méthode prioritaire est ignoré (« 14c » dans « f14c »).
    QVector<Hit> kept;
    for (const Hit& h : hits) {
        bool dominated = false;
        for (const Hit& o : hits)
            if (o.method < h.method && o.start < h.end && h.start < o.end) { dominated = true; break; }
        if (!dominated && methods_[h.method]->usesValueErrors()) kept << h;
    }

    // Chaque paire « valeur ± erreur » va au mot-clé qui la précède de plus près, sauf s'il en suit un immédiatement
    // (unité : « 2500 ± 30 BP »). Sans mot-clé avant, le plus proche après.
    const QVector<text::ValueError> pairs = text::findValueErrors(q);
    QVector<int> owner(pairs.size(), -1);
    for (int p = 0; p < pairs.size(); ++p) {
        const int s = pairs[p].start, e = s + pairs[p].length;
        int before = -1, beforeD = INT_MAX, after = -1, afterD = INT_MAX;
        for (const Hit& h : kept) {
            if (h.end <= s)        { if (s - h.end < beforeD) { beforeD = s - h.end; before = h.method; } }
            else if (h.start >= e) { if (h.start - e < afterD) { afterD = h.start - e; after = h.method; } }
        }
        // « Juste après » : séparé seulement par des espaces (une ponctuation « , ; » ouvre une autre date).
        const bool unit = after >= 0 && afterD <= 3 && q.mid(e, afterD).trimmed().isEmpty();
        owner[p] = unit ? after : (before >= 0 ? before : after);
    }

    // Chaque méthode lit la question où les paires des autres méthodes sont masquées.
    struct Found { int pos, order; QJsonObject o; };
    QVector<Found> found;
    for (int mi : present) {
        const Method* m = methods_[mi];
        QString masked = q;
        int pos = INT_MAX;
        for (const Hit& h : hits) if (h.method == mi) pos = std::min(pos, h.start);
        if (m->usesValueErrors())
            for (int p = 0; p < pairs.size(); ++p) {
                if (owner[p] == mi) pos = std::min(pos, pairs[p].start);
                else masked.replace(pairs[p].start, pairs[p].length, QString(pairs[p].length, QChar(' ')));
            }
        const QVector<QJsonObject> v = m->parse(masked);
        for (int k = 0; k < v.size(); ++k) {
            QJsonObject o = v[k];
            o["method"] = m->id();
            found.append({ pos, int(found.size()), o });
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.pos < b.pos; });
    QVector<QJsonObject> out;
    for (const Found& f : found) if (out.size() < 12) out << f.o;
    return out;
}

MethodResult MethodRegistry::compute(const QJsonObject& request) const
{
    const Method* m = byId(request.value("method").toString());
    if (!m) return MethodResult::failure(QStringLiteral("méthode inconnue : « %1 »").arg(request.value("method").toString()));
    return m->compute(request);
}

QString MethodRegistry::describe(const QJsonObject& request) const
{
    const Method* m = byId(request.value("method").toString());
    return m ? m->describe(request) : QString();
}

} // namespace seldon
