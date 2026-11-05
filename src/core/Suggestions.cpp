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
#include "Suggestions.h"

#include <QJsonArray>
#include <QSet>
#include <QUrl>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>


#include "ChronoResults.h"
#include "../methods/common/CurveTraces.h"
#include "../methods/common/DataTable.h"
#include "../methods/common/TextUtil.h"
#include "../methods/common/Traces.h"

namespace seldon {
namespace suggest {

namespace {

// « Phase A » → « la phase A » ; « Bronze final » → « la phase Bronze final ».
QString phaseText(const QString& base)
{
    return text::fold(base).startsWith(QLatin1String("phase ")) ? QStringLiteral("la ") + base.left(1).toLower() + base.mid(1)
                                                                : QStringLiteral("la phase ") + base;
}

void add(QStringList& out, const QString& q, int max)
{
    if (out.size() < max && !q.isEmpty() && !out.contains(q)) out << q;
}

} // namespace

QStringList forCurve(const SplineTrace& s, int max)
{
    QStringList out;
    if (s.rows == 0 || s.dims == 0) return out;
    auto year = [](double t) { return QString::number(std::lround(t)); };
    const double a = s.tmin + 0.25 * s.span, b = s.tmin + 0.75 * s.span, m = s.tmin + 0.5 * s.span;
    // Médiane de la courbe X à une date (pour proposer des valeurs atteintes par la courbe).
    auto median = [&](double t) {
        std::vector<double> v;
        const size_t stride = std::max<size_t>(1, s.rows / 2000);
        for (size_t i = 0; i < s.rows; i += stride) v.push_back(s.value(i, 0, t));
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    auto round2 = [](double v) {                 // deux chiffres significatifs, écrit à la française
        if (v == 0.0 || !std::isfinite(v)) return QStringLiteral("0");
        const double p = std::pow(10.0, std::floor(std::log10(std::abs(v))) - 1);
        return text::numAuto(std::round(v / p) * p, 6);
    };
    const QString D = s.names.value(0, QStringLiteral("X"));
    const QString ya = round2(median(a)), yb = round2(median(b)), ym = round2(median(m));
    add(out, QStringLiteral("Valeur de la courbe %1 en %2").arg(D, year(m)), max);
    add(out, QStringLiteral("Vitesse de la courbe %1 entre %2 et %3").arg(D, year(a), year(b)), max);
    add(out, QStringLiteral("À quelle date la courbe atteint %1 = %2 ?").arg(D, ym), max);
    if (ya != yb) add(out, QStringLiteral("Vitesse de la courbe entre %1 = %2 et %1 = %3").arg(D, ya, yb), max);
    add(out, QStringLiteral("Vitesse instantanée de la courbe %1 en %2").arg(D, year(m)), max);
    add(out, QStringLiteral("Probabilité que la courbe dépasse %1 = %2 en %3").arg(D, ym, year(m)), max);
    return out;
}

QStringList forCurveMap(const CurveMap& m, int max)
{
    QStringList out;
    if (!m.usable || m.t.empty()) return out;
    const QString D = m.component.isEmpty() ? QStringLiteral("X") : m.component;
    const QString t = QString::number(std::lround(m.t[m.t.size() / 2]));
    if (m.derivative) {
        add(out, QStringLiteral("Vitesse instantanée de la courbe %1 en %2").arg(D, t), max);
    } else {
        add(out, QStringLiteral("Valeur de la courbe %1 en %2").arg(D, t), max);
        const double y = m.y[m.y.size() / 2];
        add(out, QStringLiteral("Probabilité que la courbe dépasse %1 = %2 en %3").arg(D, text::numAuto(std::round(y * 10) / 10, 1), t), max);
    }
    return out;
}

QStringList forTraces(const TraceSet& t, int max)
{
    QStringList events, phases;
    for (const QString& c : t.columns) {
        static const QRegularExpression bound(QStringLiteral(R"(^(.+?)\s+(Begin|End)$)"), QRegularExpression::CaseInsensitiveOption);
        if (const auto m = bound.match(c); m.hasMatch()) {
            const QString base = m.captured(1);
            if (!phases.contains(base) && t.columns.contains(base + QStringLiteral(" Begin")) && t.columns.contains(base + QStringLiteral(" End")))
                phases << base;
        } else if (c.compare(QLatin1String("iter"), Qt::CaseInsensitive) != 0) {
            events << c;
        }
    }
    QStringList out;
    if (!phases.isEmpty()) {
        const QString p = phaseText(phases[0]);
        add(out, QStringLiteral("Durée de %1 : la convergence est-elle satisfaisante ?").arg(p), max);
        add(out, QStringLiteral("Début de %1").arg(p), max);
    }
    if (events.size() >= 2) {
        add(out, QStringLiteral("Quel est l'écart entre l'Event %1 et l'Event %2 ?").arg(events[0], events[1]), max);
        add(out, QStringLiteral("%1 est-il antérieur à %2 ?").arg(events[0], events[1]), max);
    }
    if (phases.size() >= 2) {
        const QString a = phases[0], b = phases[1];
        add(out, QStringLiteral("Date d'un niveau situé entre la fin de %1 et le début de %2").arg(phaseText(a), phaseText(b)), max);
    } else if (events.size() >= 2) {
        add(out, QStringLiteral("Date d'un niveau situé entre %1 et %2").arg(events[0], events[1]), max);
    }
    if (!events.isEmpty() && !phases.isEmpty())
        add(out, QStringLiteral("%1 appartient-il à %2 ?").arg(events[0], phaseText(phases[0])), max);
    if (events.size() >= 2) {
        add(out, QStringLiteral("Corrélation entre %1 et %2").arg(events[0], events[1]), max);
    } else if (!phases.isEmpty()) {
        const QString p = phaseText(phases[0]);
        add(out, QStringLiteral("Corrélation entre le début de %1 et la fin de %1").arg(p), max);
    }
    if (phases.size() >= 2) add(out, QStringLiteral("Durée de %1").arg(phaseText(phases[1])), max);
    return out;
}

QStringList forLog(const chronoresults::Report& r, int max)
{
    QStringList out;
    add(out, QStringLiteral("Le modèle a-t-il convergé ? Quelles variables posent problème et que faire ?"), max);
    QString bad;
    for (const chronoresults::Variable& v : r.vars)
        if (!v.ok() && !v.event.isEmpty()) { bad = v.event; break; }
    if (!bad.isEmpty()) add(out, QStringLiteral("Pourquoi l'Event %1 ne converge-t-il pas ?").arg(bad), max);
    if (!r.phases.isEmpty()) add(out, QStringLiteral("Résume les dates de début et de fin des phases et leurs durées"), max);
    if (!r.successions.isEmpty()) add(out, QStringLiteral("Y a-t-il un hiatus entre les phases successives ?"), max);
    add(out, QStringLiteral("Quels réglages MCMC proposer pour relancer le calcul ?"), max);
    return out;
}

QStringList forTable(const datatable::Table& t, int max)
{
    QStringList out;
    QSet<QString> methods;
    for (const QJsonObject& rq : t.requests) if (!rq.isEmpty()) methods.insert(rq.value("method").toString());
    static const QList<QPair<QString, QString>> names = { { "c14", "C14" }, { "f14c", "F14C" }, { "tl", "TL" }, { "am", "archéomagnétiques" } };
    add(out, QStringLiteral("Calibre toutes les dates du tableau"), max);
    if (methods.size() >= 2)
        for (const auto& n : names)
            if (methods.contains(n.first)) { add(out, QStringLiteral("Extrais les dates %1 du tableau").arg(n.second), max); break; }
    if (t.datable() >= 2) add(out, QStringLiteral("Combine les dates du tableau"), max);
    add(out, QStringLiteral("Fais un modèle ChronoModel avec le tableau"), max);
    // Une colonne de regroupement (US, phase, niveau…) : l'assistant traduit le regroupement.
    for (int c = 0; c < t.headers.size(); ++c) {
        if (c < t.roles.size() && t.roles[c] != datatable::Role::Other) continue;
        const QString h = text::fold(t.headers[c]);
        if (h == QLatin1String("us") || h.startsWith(QLatin1String("phase")) || h.startsWith(QLatin1String("niveau"))
            || h.startsWith(QLatin1String("couche")) || h.startsWith(QLatin1String("structure"))) {
            add(out, QStringLiteral("Fais un modèle ChronoModel avec un événement par %1").arg(t.headers[c]), max);
            break;
        }
    }
    return out;
}

QStringList forChr(const QJsonObject& chr, int max)
{
    QStringList out;
    add(out, QStringLiteral("Dessine le modèle"), max);
    add(out, QStringLiteral("Explique ce modèle : événements, phases et contraintes"), max);
    if (!chr.value("events_constraints").toArray().isEmpty() || !chr.value("phases_constraints").toArray().isEmpty())
        add(out, QStringLiteral("Les contraintes de ce modèle sont-elles cohérentes avec les dates ?"), max);
    add(out, QStringLiteral("Quels réglages MCMC conseilles-tu pour ce modèle ?"), max);
    return out;
}

QStringList afterResult(const After& a, int max)
{
    QStringList out;
    if (a.modelBuilt) {
        add(out, QStringLiteral("Dessine le modèle"), max);
        add(out, QStringLiteral("Ajoute une contrainte : … sous …"), max);
        add(out, QStringLiteral("Quels réglages MCMC conseilles-tu pour ce modèle ?"), max);
        return out;
    }
    static const QStringList simple = { "c14", "f14c", "tl", "gauss", "unif", "am" };
    QVector<QJsonObject> datings, relations;
    bool event = false;
    for (const QJsonObject& r : a.requests) {
        const QString m = r.value("method").toString();
        if (m == QLatin1String("relation")) relations << r;
        else if (m == QLatin1String("event")) event = true;
        else if (simple.contains(m)) datings << r;
    }
    // Autre niveau : 95 % et 68 % sont déjà dans la fiche ; 90 % seulement si aucun niveau n'a été demandé.
    const bool levelAsked = !a.requests.isEmpty() && a.requests.last().value("level").toDouble(0.0) > 0.0;
    const QString other = levelAsked ? QString() : QStringLiteral("Et à 90 % ?");
    if (!relations.isEmpty()) {
        const QJsonObject r = relations.last();
        const QString op = r.value("op").toString();
        QStringList cols;                                   // colonnes des traces utilisées
        QJsonArray terms = r.value("terms").toArray();
        for (const char* k : { "from", "to", "of" }) if (r.contains(QLatin1String(k))) terms.append(r.value(QLatin1String(k)));
        for (const QJsonValue& v : terms)
            if (v.isString() && a.traceColumns.contains(v.toString()) && !cols.contains(v.toString())) cols << v.toString();
        if (op == QLatin1String("duration") || op == QLatin1String("value") || op == QLatin1String("between")) add(out, other, max);
        if (cols.size() >= 2 && op != QLatin1String("inside")) {
            const QString A = cols[0], B = cols[1];
            if (op != QLatin1String("correlation")) add(out, QStringLiteral("Corrélation entre %1 et %2").arg(A, B), max);
            if (op != QLatin1String("order")) add(out, QStringLiteral("%1 est-il antérieur à %2 ?").arg(A, B), max);
            if (op != QLatin1String("duration")) add(out, QStringLiteral("Quelle est la durée entre %1 et %2 ?").arg(A, B), max);
        }
        // Une autre paire : le premier Event pas encore utilisé
        static const QRegularExpression bound(QStringLiteral(R"(\s(Begin|End)$)"), QRegularExpression::CaseInsensitiveOption);
        if (!cols.isEmpty())
            for (const QString& c : a.traceColumns)
                if (!cols.contains(c) && !bound.match(c).hasMatch() && !bound.match(cols[0]).hasMatch()) {
                    add(out, QStringLiteral("Quel est l'écart entre l'Event %1 et l'Event %2 ?").arg(cols[0], c), max);
                    break;
                }
        return out;
    }
    if (event) {
        add(out, QStringLiteral("Mets ces dates dans un modèle ChronoModel"), max);
        add(out, QStringLiteral("Explique les σ_i et la convergence de l'événement"), max);
        return out;
    }
    if (!datings.isEmpty()) {
        add(out, other, max);
        if (datings.size() == 1) {
            const QString m = datings[0].value("method").toString();
            const QString next = m == QLatin1String("c14") ? QStringLiteral("C14 … ± … BP")
                               : m == QLatin1String("f14c") ? QStringLiteral("F14C … ± …")
                               : m == QLatin1String("tl") ? QStringLiteral("TL … ± … mesurée en …")
                               : m == QLatin1String("am") ? QStringLiteral("inclinaison …°, alpha95 = …")
                               : QStringLiteral("C14 … ± … BP");
            add(out, QStringLiteral("Combien de temps entre la date précédente et %1 ?").arg(next), max);
            add(out, QStringLiteral("Laquelle est la plus ancienne : la date précédente ou %1 ?").arg(next), max);
        } else if (datings.size() == 2) {
            add(out, QStringLiteral("Quelle durée entre ces deux dates ?"), max);
            add(out, QStringLiteral("Laquelle de ces deux dates est la plus ancienne ?"), max);
        }
        add(out, QStringLiteral("Mets ces dates dans un modèle ChronoModel"), max);
        return out;
    }
    if (a.log) {
        for (const QString& q : forLog(*a.log, 3))
            if (!q.startsWith(QStringLiteral("Le modèle a-t-il convergé"))) add(out, q, max);
    }
    return out;
}

QString html(const QString& title, const QStringList& questions, const QString& footer)
{
    if (questions.isEmpty()) return {};
    QString items;
    for (const QString& q : questions)
        items += QStringLiteral("<li><a href=\"seldon://insert?q=%1\">%2</a></li>")
                     .arg(QString::fromLatin1(QUrl::toPercentEncoding(q)), q.toHtmlEscaped());
    return QStringLiteral("%1<ul>%2</ul>%3").arg(title, items, footer);
}

} // namespace suggest
} // namespace seldon
