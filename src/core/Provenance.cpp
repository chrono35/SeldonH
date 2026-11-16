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

#include "Provenance.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

#include "../methods/common/TextUtil.h"

namespace seldon {
namespace provenance {

namespace {

// Nombres d'un texte : signe éventuel (« - », « − »), séparateur de milliers (espace ordinaire, insécable ou fine, groupes
// de trois chiffres), décimales (virgule ou point). Pas de nombre collé à une lettre (« IntCal20 », « Q1 », « US6 ») ni
// pris dans une référence (« Ly-8812 ») : la lettre devant le tiret l'exclut.
const QRegularExpression& numberRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"((?<![\p{L}\d_.,])(?<!\p{L}[-−])((?:(?<![\p{L}\d])[-−])?)(\d{1,3}(?:[ \x{00A0}\x{202F}]\d{3}(?!\d))+|\d+)(?:[.,](\d+))?(\s*%)?)"));
    return re;
}

double parse(const QString& sign, const QString& intPart, const QString& dec)
{
    QString s = intPart;
    s.remove(QChar(' ')).remove(QChar(0x00A0)).remove(QChar(0x202F));
    if (!dec.isEmpty()) s += QLatin1Char('.') + dec;
    const double v = s.toDouble();
    return sign.isEmpty() ? v : -v;
}

bool close(double a, double b)
{
    return std::abs(a - b) <= 1e-6 * std::max(1.0, std::abs(a));
}

// Clés des mesures (bloquantes) et des bornes / paramètres (avertissement) dans les demandes de calcul.
const QSet<QString>& measureKeys()
{
    static const QSet<QString> k = { "age", "error", "lab_error", "f14c", "fraction", "inc", "dec", "alpha95", "field",
                                     "error_f", "ref_year", "delta_r", "delta_r_error", "t", "t_error" };
    return k;
}
const QSet<QString>& boundKeys()
{
    static const QSet<QString> k = { "min", "max", "tmin", "tmax", "t_min", "t_max", "known_fixed", "mean", "sd", "median",
                                     "factor", "meanlog", "sdlog", "mode", "mode_min", "mode_max", "offset", "shape", "scale",
                                     "rate", "alpha", "beta", "value", "y", "y1", "y2", "ymin", "ymax" };
    return k;
}

double numericValue(const QJsonValue& v)
{
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        const double y = text::yearFromText(v.toString());       // « 500 av. J.-C. », « -499 », « 12,5 »
        if (std::isfinite(y)) return y;
        double written = std::nan("");                           // « 6000 cal BP » : le nombre tel qu'écrit
        if (std::isfinite(text::yearFromAgeText(v.toString(), &written))) return written;
    }
    return std::nan("");
}

QString fmt(double v) { return QString::number(v, 'g', 12); }

void walk(const QJsonObject& o, const QString& where, const NumberPool& pool, RequestCheck& out)
{
    const QString label = o.value("name").toString().trimmed();
    const QString here = label.isEmpty() ? where : label;
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString key = it.key();
        const QJsonValue v = it.value();
        if (v.isObject()) { walk(v.toObject(), here, pool, out); continue; }
        if (v.isArray()) {
            for (const QJsonValue& e : v.toArray())
                if (e.isObject()) walk(e.toObject(), here, pool, out);
            continue;
        }
        // Formule de la calculatrice ({"method":"expr","expr":"sqrt(36)"}) : chacun de ses nombres est une mesure, sauf
        // les petits entiers (exposants, diviseurs : « x^2 », « (a+b)/2 », « ^(1/3) ») ; les chiffres d'un nom de
        // fonction (log10, c14tof14c) ne comptent pas.
        static const QStringList exprKeys{ "expr", "expression", "formule", "formula" };
        if (exprKeys.contains(key) && v.isString()) {
            static const QRegularExpression num(QStringLiteral(R"((?<![A-Za-z_\d.,])(\d+(?:[.,]\d+)?(?:[eE][-+]?\d+)?))"));
            for (auto m = num.globalMatch(v.toString()); m.hasNext();) {
                const QString s = m.next().captured(1);
                const double x = QString(s).replace(QLatin1Char(','), QLatin1Char('.')).toDouble();
                if (!std::isfinite(x) || pool.contains(x)) continue;
                if (x <= 10 && x == std::floor(x)) continue;
                out.measures << (here.isEmpty() ? QStringLiteral("expr : %1").arg(s) : QStringLiteral("expr : %1 (« %2 »)").arg(s, here));
            }
            continue;
        }
        const bool measure = measureKeys().contains(key), bound = boundKeys().contains(key);
        if (!measure && !bound) continue;
        const double x = numericValue(v);
        if (!std::isfinite(x) || pool.contains(x)) continue;
        const QString item = here.isEmpty() ? QStringLiteral("%1 = %2").arg(key, fmt(x))
                                            : QStringLiteral("%1 = %2 (« %3 »)").arg(key, fmt(x), here);
        (measure ? out.measures : out.bounds) << item;
    }
}

} // namespace

NumberPool::NumberPool(const QString& text, bool derived) : derived_(derived)
{
    if (!text.isEmpty()) add(text);
}

void NumberPool::insert(double v)
{
    values_ << v << -v;                                          // année signée ou non
    if (!derived_) return;
    const double a = std::abs(v);
    values_ << a * 1e3 << a * 1e6 << a / 1e3 << a / 1e6;        // ka, Ma
    // Changements d'origine, seulement pour des années ou des âges (|v| ≥ 100) : les petits nombres des fiches
    // (« 2 », « 30 ») rendraient sinon « connues » toutes les années de 1850 à 1950.
    if (a >= 100)
        values_ << 1.0 - a                                       // N av. J.-C. -> année astronomique
                << 1950.0 - v << 1950.0 + a                      // cal BP <-> année (et année négative)
                << 2000.0 - v;                                   // b2k
}

void NumberPool::add(const QString& text)
{
    // Le texte tel qu'écrit, puis avec les ères converties (« 500 av. J.-C. » -> -499).
    for (const QString& t : { text, text::erasToAstronomical(text) }) {
        for (auto it = numberRe().globalMatch(t); it.hasNext();) {
            const auto m = it.next();
            insert(parse(m.captured(1), m.captured(2), m.captured(3)));
        }
    }
    std::sort(values_.begin(), values_.end());
    values_.erase(std::unique(values_.begin(), values_.end()), values_.end());
}

bool NumberPool::contains(double v) const
{
    // Recherche dans le tableau trié, avec la tolérance de close().
    const double tol = 1e-6 * std::max(1.0, std::abs(v));
    auto it = std::lower_bound(values_.begin(), values_.end(), v - tol);
    return it != values_.end() && close(*it, v);
}

RequestCheck checkRequest(const QJsonObject& request, const NumberPool& userNumbers)
{
    RequestCheck out;
    walk(request, QString(), userNumbers, out);
    out.measures.removeDuplicates();
    out.bounds.removeDuplicates();
    return out;
}

QStringList unsourcedNumbers(const QString& answer, const NumberPool& received)
{
    // Blocs de code retirés (```calcul, ```chronomodel, ```plot : vérifiés ou exécutés ailleurs), et code en ligne.
    QString a = answer;
    static const QRegularExpression fenced(QStringLiteral("^[ \\t]*```.*?^[ \\t]*```[ \\t]*$"),
                                           QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption);
    a.remove(fenced);
    static const QRegularExpression inlineCode(QStringLiteral("`[^`\\n]*`"));
    a.remove(inlineCode);
    QStringList out;
    QSet<QString> seen;
    for (auto it = numberRe().globalMatch(a); it.hasNext();) {
        const auto m = it.next();
        const double v = parse(m.captured(1), m.captured(2), m.captured(3));
        const bool percent = !m.captured(4).isEmpty(), decimal = !m.captured(3).isEmpty();
        if (!percent && !decimal && std::abs(v) < 100) continue;        // petits entiers : numéros, effectifs
        if (percent && (v == 0.0 || v == 100.0)) continue;              // « 100 % par construction »
        if (!percent && v == 1950.0) continue;                          // origine du BP (« 1950 − année »)
        if (received.contains(v)) continue;
        const QString shown = m.captured(0).trimmed();
        if (seen.contains(shown)) continue;
        seen.insert(shown);
        out << shown;
    }
    return out;
}

QString blockedMessage(const QStringList& measures)
{
    return QStringLiteral("Calcul non lancé : valeur(s) absente(s) des messages de l'utilisateur : %1. N'invente et ne "
                          "convertis aucune mesure : reprends exactement celles que l'utilisateur a écrites, ou demande-les-lui.")
        .arg(measures.join(QStringLiteral(", ")));
}

QString boundsMessage(const QStringList& bounds)
{
    return QStringLiteral("Attention : valeur(s) choisie(s) par le modèle, absente(s) des messages de l'utilisateur : %1. "
                          "Le calcul a été fait avec elles ; dis-le à l'utilisateur et demande-lui de les confirmer.")
        .arg(bounds.join(QStringLiteral(", ")));
}

} // namespace provenance
} // namespace seldon
