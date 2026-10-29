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

#include "ChrBuilder.h"

#include <QColor>
#include <QJsonArray>
#include <QDate>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>
#include <QHash>
#include <QSet>
#include <QPair>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "../methods/common/CurveStore.h"
#include "../methods/common/MethodRegistry.h"
#include "../methods/common/MethodRunner.h"
#include "../methods/common/TextUtil.h"

namespace seldon {

namespace {

// Décalage (wiggle) d'une date du bloc chronomodel → champs delta_* du .chr (Date::DeltaType de ChronoModel :
// -1 aucun, 0 fixe, 1 gaussien, 2 intervalle). Accepte {"type":"fixed","value":v}, {"type":"range","min":a,"max":b},
// {"type":"gaussian","mean":m,"error":e} ou un nombre (fixe). Renvoie false (avec `why`) si le décalage est invalide ;
// [lo, hi] = étendue du décalage (pour la plage d'étude).
bool wiggleToChr(const QJsonValue& v, QJsonObject& out, QString& why, double& lo, double& hi)
{
    lo = hi = 0.0;
    if (v.isUndefined() || v.isNull()) return true;
    auto num = [](const QJsonObject& o, std::initializer_list<const char*> keys) {
        for (const char* k : keys) {
            const QJsonValue x = o.value(k);
            if (x.isDouble()) return x.toDouble();
            if (x.isString()) { bool ok = false; const double d = x.toString().trimmed().replace(',', '.').toDouble(&ok); if (ok) return d; }
        }
        return std::nan("");
    };
    QJsonObject o;
    if (v.isDouble()) o = QJsonObject{ { "type", "fixed" }, { "value", v.toDouble() } };
    else o = v.toObject();
    const QString t = o.value("type").toString().trimmed().toLower();
    if (t.isEmpty() || t == "none" || t == "aucun") return true;
    if (t == "fixed" || t == "fixe") {
        const double f = num(o, { "value", "fixed", "delta" });
        if (std::isnan(f)) { why = QStringLiteral("décalage fixe sans « value »"); return false; }
        out["delta_type"] = 0; out["delta_fixed"] = f;
        lo = hi = f;
    } else if (t == "gaussian" || t == "gauss" || t == "normal") {
        const double m = num(o, { "mean", "average", "value" }), e = num(o, { "error", "sd" });
        if (std::isnan(m) || !(e > 0)) { why = QStringLiteral("décalage gaussien : « mean » et « error » > 0 requis"); return false; }
        out["delta_type"] = 1; out["delta_average"] = m; out["delta_error"] = e;
        lo = m - 3 * e; hi = m + 3 * e;
    } else if (t == "range" || t == "intervalle" || t == "uniform") {
        const double a = num(o, { "min" }), b = num(o, { "max" });
        if (std::isnan(a) || std::isnan(b) || !(a < b)) { why = QStringLiteral("décalage « range » : min < max requis"); return false; }
        out["delta_type"] = 2; out["delta_min"] = a; out["delta_max"] = b;
        lo = a; hi = b;
    } else {
        why = QStringLiteral("type de décalage inconnu « %1 » (fixed, range ou gaussian)").arg(t);
        return false;
    }
    return true;
}

// Description du décalage d'une date d'un .chr (champs delta_*), vide sans décalage.
QString wiggleOfChr(const QJsonObject& d)
{
    switch (d.value("delta_type").toInt(-1)) {
    case 0:  return QStringLiteral("δ = %1").arg(d.value("delta_fixed").toDouble());
    case 1:  return QStringLiteral("δ ~ N(%1 ; %2)").arg(d.value("delta_average").toDouble()).arg(d.value("delta_error").toDouble());
    case 2:  return QStringLiteral("δ ∈ [%1 ; %2]").arg(d.value("delta_min").toDouble()).arg(d.value("delta_max").toDouble());
    default: return QString();
    }
}

// Valeurs observées dans des projets ChronoModel 4.0 réels.
constexpr int kDateSampler  = 4;     // « sampler » de toutes les dates (gauss, unif, tl/osl, am, density)
constexpr int kEventSampler = 1;     // « sampler » des événements
const QString kAmCurveI = QStringLiteral("gal2002sph2014_i.ref");
const QString kAmCurveD = QStringLiteral("gal2002sph2014_d.ref");
const QString kAmCurveF = QStringLiteral("eurw_2017_f.ref");

QString uuid4()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString slug(const QString& s)
{
    QString r = s.toLower();
    r.replace(QRegularExpression("[^a-z0-9]+"), "_");
    r.remove(QRegularExpression("^_+|_+$"));
    return r.isEmpty() ? QStringLiteral("modele") : r;
}

// Couleurs par défaut proches de celles de ChronoModel.
void setColor(QJsonObject& o, int r, int g, int b)
{
    o["red"] = r; o["green"] = g; o["blue"] = b;
}

// Couleur écrite dans le bloc (« color ») : nom français ou anglais (« bleu », « en rouge », « vert clair »), « #rrggbb »,
// nom de couleur Qt/SVG, [r, g, b] ou {"red":…, "green":…, "blue":…}. Rouge, orange et bleu reprennent les teintes
// qu'enregistre ChronoModel (relevées dans un projet corrigé à la main, test_strati_corrige.chr). false si inconnue.
bool parseColor(const QJsonValue& v, int& r, int& g, int& b)
{
    auto c8 = [](double x) { return std::clamp(static_cast<int>(std::lround(x)), 0, 255); };
    if (v.isArray() || v.isObject()) {
        QJsonValue cr, cg, cb;
        if (v.isArray()) {
            const QJsonArray a = v.toArray();
            if (a.size() < 3) return false;
            cr = a[0]; cg = a[1]; cb = a[2];
        } else {
            const QJsonObject o = v.toObject();
            cr = o.contains("red") ? o.value("red") : o.value("r");
            cg = o.contains("green") ? o.value("green") : o.value("g");
            cb = o.contains("blue") ? o.value("blue") : o.value("b");
        }
        if (!cr.isDouble() || !cg.isDouble() || !cb.isDouble()) return false;
        r = c8(cr.toDouble()); g = c8(cg.toDouble()); b = c8(cb.toDouble());
        return true;
    }
    if (!v.isString()) return false;
    const QString raw = v.toString().trimmed();
    QStringList words = text::fold(raw).split(QRegularExpression(QStringLiteral("[\\s_-]+")), Qt::SkipEmptyParts);
    bool light = false, dark = false;
    QStringList kept;
    for (const QString& w : std::as_const(words)) {
        if (w == "en" || w == "in" || w == "couleur" || w == "color" || w == "colour") continue;
        if (w == "clair" || w == "claire" || w == "light" || w == "pale") { light = true; continue; }
        if (w == "fonce" || w == "foncee" || w == "dark" || w == "sombre") { dark = true; continue; }
        kept << w;
    }
    static const QHash<QString, QColor> names = {
        { "rouge", QColor(255, 19, 12) },   { "red", QColor(255, 19, 12) },
        { "orange", QColor(255, 178, 97) },
        { "bleu", QColor(0, 0, 255) },      { "bleue", QColor(0, 0, 255) },    { "blue", QColor(0, 0, 255) },
        { "vert", QColor(0, 170, 0) },      { "verte", QColor(0, 170, 0) },    { "green", QColor(0, 170, 0) },
        { "jaune", QColor(255, 215, 0) },   { "yellow", QColor(255, 215, 0) },
        { "violet", QColor(148, 0, 211) },  { "violette", QColor(148, 0, 211) }, { "purple", QColor(148, 0, 211) },
        { "mauve", QColor(190, 130, 220) },
        { "rose", QColor(255, 105, 180) },  { "pink", QColor(255, 105, 180) },
        { "noir", QColor(0, 0, 0) },        { "noire", QColor(0, 0, 0) },      { "black", QColor(0, 0, 0) },
        { "blanc", QColor(255, 255, 255) }, { "blanche", QColor(255, 255, 255) }, { "white", QColor(255, 255, 255) },
        { "gris", QColor(128, 128, 128) },  { "grise", QColor(128, 128, 128) }, { "grey", QColor(128, 128, 128) },
        { "gray", QColor(128, 128, 128) },
        { "marron", QColor(139, 69, 19) },  { "brun", QColor(139, 69, 19) },   { "brune", QColor(139, 69, 19) },
        { "brown", QColor(139, 69, 19) },
        { "cyan", QColor(0, 190, 190) },    { "turquoise", QColor(64, 224, 208) },
        { "bordeaux", QColor(128, 0, 32) }, { "beige", QColor(222, 200, 160) }, { "ocre", QColor(204, 119, 34) },
    };
    QColor c;
    const QString key = kept.join(' ');
    if (names.contains(key)) c = names.value(key);
    else {
        c = QColor::fromString(raw);
        if (!c.isValid()) c = QColor::fromString(kept.join(QString()));
        if (!c.isValid()) return false;
    }
    if (light) c = QColor((c.red() + 255) / 2, (c.green() + 255) / 2, (c.blue() + 255) / 2);
    if (dark)  c = QColor(c.red() * 6 / 10, c.green() * 6 / 10, c.blue() * 6 / 10);
    r = c.red(); g = c.green(); b = c.blue();
    return true;
}

// Disposition des événements dans la scène de ChronoModel (y croît vers le bas) en diagramme stratigraphique : le plus
// ancien en bas, chaque événement une rangée au-dessus du plus haut de ceux qui lui sont antérieurs (plus long chemin
// dans le graphe des contraintes) ; dans une rangée, ordre des barycentres des prédécesseurs pour limiter les
// croisements. Les événements sans contrainte sont rangés en grille à droite ; en cas de cycle, grille simple.
void layoutEvents(QJsonArray& events, const QJsonArray& constraints)
{
    constexpr double dx = 600.0, dy = 450.0, x0 = 300.0, y0 = 100.0;     // écarts larges : flèches et noms lisibles
    const int n = events.size();
    auto place = [&](int i, double x, double y) {
        QJsonObject o = events[i].toObject();
        o["item_x"] = x; o["item_y"] = y;
        events[i] = o;
    };
    QHash<int, int> idx;
    for (int i = 0; i < n; ++i) idx[events[i].toObject().value("id").toInt()] = i;
    QVector<QVector<int>> succ(n), pred(n);
    for (const QJsonValue& cv : constraints) {
        const QJsonObject c = cv.toObject();
        const int b = idx.value(c.value("bwd_id").toInt(), -1), f = idx.value(c.value("fwd_id").toInt(), -1);
        if (b < 0 || f < 0 || b == f) continue;
        const int older = ChrBuilder::kBwdIsOlder ? b : f, younger = ChrBuilder::kBwdIsOlder ? f : b;
        succ[older] << younger; pred[younger] << older;
    }
    // Plus long chemin depuis les événements les plus anciens (tri topologique de Kahn).
    QVector<int> level(n, 0), indeg(n, 0), queue;
    for (int i = 0; i < n; ++i) { indeg[i] = pred[i].size(); if (indeg[i] == 0) queue << i; }
    for (int q = 0; q < queue.size(); ++q) {
        const int u = queue[q];
        for (int v : std::as_const(succ[u])) {
            level[v] = std::max(level[v], level[u] + 1);
            if (--indeg[v] == 0) queue << v;
        }
    }
    if (queue.size() < n) {                                        // cycle (signalé par ailleurs) : grille
        for (int i = 0; i < n; ++i) place(i, x0 + (i % 6) * dx, y0 + (i / 6) * dy);
        return;
    }
    QVector<int> alone;
    int maxLevel = -1;
    for (int i = 0; i < n; ++i) {
        if (succ[i].isEmpty() && pred[i].isEmpty()) alone << i;
        else maxLevel = std::max(maxLevel, level[i]);
    }
    QVector<double> col(n, 0.0);
    int width = 0;
    if (maxLevel >= 0) {
        QVector<QVector<int>> rows(maxLevel + 1);
        for (int i = 0; i < n; ++i)
            if (!succ[i].isEmpty() || !pred[i].isEmpty()) rows[level[i]] << i;
        for (int L = 0; L <= maxLevel; ++L) {
            QVector<int>& row = rows[L];
            if (L > 0) {
                auto bary = [&](int i) {
                    double s = 0.0;
                    for (int p : std::as_const(pred[i])) s += col[p];
                    return pred[i].isEmpty() ? 0.0 : s / pred[i].size();
                };
                std::stable_sort(row.begin(), row.end(), [&](int a, int b) { return bary(a) < bary(b); });
            }
            for (int k = 0; k < row.size(); ++k) col[row[k]] = k - (row.size() - 1) / 2.0;
            width = std::max(width, int(row.size()));
        }
        const double xc = x0 + (width - 1) / 2.0 * dx;
        for (int i = 0; i < n; ++i)
            if (!succ[i].isEmpty() || !pred[i].isEmpty())
                place(i, xc + col[i] * dx, y0 + (maxLevel - level[i]) * dy);
    }
    const int cols = maxLevel >= 0 ? 2 : 6;
    const double xa = maxLevel >= 0 ? x0 + width * dx + dx / 2.0 : x0;
    for (int k = 0; k < alone.size(); ++k) place(alone[k], xa + (k % cols) * dx, y0 + (k / cols) * dy);
}

int curveProcessType(const QString& name, bool* known)
{
    static const QHash<QString, int> m = {
        { "none", 78 }, { "aucune", 78 }, { "sans", 78 },
        { "univariate", 85 }, { "depth", 80 }, { "inclination", 73 }, { "declination", 68 },
        { "intensity", 70 }, { "bivariate", 50 }, { "spherical", 83 }, { "vector", 86 },
        { "trivariate", 51 }, { "unknown_dec", 75 }
    };
    const QString k = name.trimmed().toLower();
    if (known) *known = m.contains(k) || k.isEmpty();
    return m.value(k, 78);
}

// Nom de courbe de référence archéomagnétique tel que ChronoModel le stocke : « nom.ref » (en minuscules).
QString refName(QString n)
{
    n = n.trimmed();
    if (n.isEmpty()) return n;
    return n.endsWith(".ref", Qt::CaseInsensitive) ? n : n + ".ref";
}

// Code « process_type » du plugin archéomagnétisme de ChronoModel 4.0. Les six codes sont vérifiés sur un projet
// réel contenant tous les types de datation AM (tous_type_AM.chr) :
//   1 = inclinaison, 2 = déclinaison, 3 = intensité, 4 = directionnel (I, D), 5 = vectoriel (I, F), 6 = vectoriel (I, D, F).
int amProcessCode(const QString& p)
{
    static const QHash<QString, int> m = { { "I", 1 }, { "D", 2 }, { "F", 3 }, { "ID", 4 }, { "IF", 5 }, { "IDF", 6 } };
    return m.value(p.trimmed().toUpper(), 0);
}

// Nom de courbe de calibration 14C tel que ChronoModel le stocke : « nom.14c » en minuscules (ex. « intcal13.14c »,
// relevé dans un projet réel, Castelnau_version_19-12-2016.chr).
QString c14CurveName(QString n)
{
    n = n.trimmed().toLower();
    if (n.endsWith(".14c")) n.chop(4);
    return n.isEmpty() ? QString() : n + ".14c";
}

// Sens d'une relation d'ordre écrite en toutes lettres entre A et B (« A <relation> B ») :
//   +1 : A est plus ancien que B (avant, antérieur, précède, sous / en dessous, recouvert par, coupé par…) ;
//   −1 : A est plus récent que B (après, postérieur, succède, sur / au-dessus, recouvre, coupe, scelle…) ;
//    0 : relation non reconnue ou contradictoire.
// En stratigraphie, ce qui est au-dessus est plus récent (loi de superposition) ; ce qui coupe, recouvre ou scelle
// une couche est plus récent qu'elle.
int relationOrder(const QString& relation)
{
    QString r = text::fold(relation);
    r.replace(QRegularExpression(QStringLiteral("[-_'’]")), QStringLiteral(" "));
    r = r.simplified();
    if (r.isEmpty()) return 0;
    // Formes passives d'abord : « coupé par », « recouvert par », « scellé par », « cut by », « covered by »…
    // « A est coupé / recouvert / suivi par B » : A plus ancien ; « A est précédé par B » : A plus récent.
    static const QRegularExpression passiveOlder(QStringLiteral(
        R"(\b(?:coupee?s?|recoupee?s?|recouverte?s?|scellee?s?|percee?s?|suivie?s?|succedee?s?|cut|covered|sealed|overlain|overlaid)"
        R"(|truncated|disturbed|followed|succeeded|postdated)\s+(?:par|by)\b)"));
    static const QRegularExpression passiveYounger(QStringLiteral(
        R"(\b(?:precedee?s?|preceded|predated|antedated)\s+(?:par|by)\b)"));
    if (passiveYounger.match(r).hasMatch()) return -1;
    if (passiveOlder.match(r).hasMatch()) return +1;
    static const QRegularExpression older(QStringLiteral(
        R"(\b(?:avant|anterieure?s?|plus anciens?|plus anciennes?|precede\w*|sous|dessous|older|before|earlier|below|beneath|under|underl\w*|preced\w*|antedat\w*|predat\w*)\b)"));
    static const QRegularExpression younger(QStringLiteral(
        R"(\b(?:apres|posterieure?s?|plus recente?s?|succede\w*|suit|suivent|sur|dessus|sus jacente?s?|recouvre\w*|scelle\w*|coupe\w*|recoupe\w*|perce\w*)"
        R"(|younger|after|later|above|over|overl\w*|follow\w*|succeed\w*|postdat\w*|cuts?|covers?|seals?|truncates?)\b)"));
    const bool o = older.match(r).hasMatch(), y = younger.match(r).hasMatch();
    if (o == y) return 0;
    return o ? +1 : -1;
}

// Dates d'un événement du bloc, quelle que soit la forme écrite par le modèle de langage : « dates » (tableau ou objet
// seul), « date », « datations », « datation », « mesures »…, ou champs de la date posés directement sur l'événement
// ({"name":"13 cm","depth":13,"type":"c14","name_date":"Ly-8812","age":310,"error":50}). `flat` : forme à plat utilisée.
QJsonArray eventDates(const QJsonObject& e, bool* flat = nullptr)
{
    if (flat) *flat = false;
    for (const char* k : { "dates", "date", "datations", "datation", "mesures", "measurements", "samples", "echantillons" }) {
        const QJsonValue v = e.value(k);
        if (v.isArray() && !v.toArray().isEmpty()) return v.toArray();
        if (v.isObject()) return QJsonArray{ v };
    }
    const QJsonValue tv = e.contains("date_type") ? e.value("date_type") : e.contains("method") ? e.value("method") : e.value("type");
    const QString t = tv.toString().trimmed().toLower();
    static const QSet<QString> dateTypes = { "gauss", "unif", "uniform", "tl", "osl", "tl/osl", "am", "archeomagnetisme", "f14c",
                                             "density", "densite", "c14", "14c", "radiocarbone" };
    if (!dateTypes.contains(t)) return {};
    QJsonObject d;
    for (const char* k : { "age", "error", "min", "max", "ref_year", "curve", "ref_curve", "delta_r", "delta_r_error", "f14c",
                           "fraction", "process", "inc", "dec", "alpha95", "field", "error_f", "curve_i", "curve_d", "curve_f",
                           "integration_steps", "wiggle", "file" })
        if (e.contains(k)) d[k] = e.value(k);
    d["type"] = t;
    for (const char* k : { "date_name", "name_date", "ref", "reference", "code", "lab_ref", "labo" })
        if (e.contains(k)) { d["name"] = e.value(k); break; }
    if (flat) *flat = true;
    return QJsonArray{ d };
}

struct Range { double lo =  std::numeric_limits<double>::infinity();
               double hi = -std::numeric_limits<double>::infinity();
               void add(double v) { lo = std::min(lo, v); hi = std::max(hi, v); }
               bool valid() const { return lo <= hi; } };

} // namespace

namespace {

// Années écrites avec leur ère dans la spécification (« known_fixed »: "40 BC", « tmin »: "1000 av. J.-C. ») : années
// astronomiques, comme les courbes de calibration (40 BC = -39).
QJsonValue yearsToNumbers(const QJsonValue& v, const QString& key = QString())
{
    static const QStringList years = { "known_fixed", "tmin", "tmax", "age", "min", "max" };
    if (v.isObject()) {
        QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) it.value() = yearsToNumbers(it.value(), it.key());
        return o;
    }
    if (v.isArray()) {
        QJsonArray a = v.toArray();
        for (int i = 0; i < a.size(); ++i) a[i] = yearsToNumbers(a[i], key);
        return a;
    }
    if (v.isString() && years.contains(key)) {
        const double y = seldon::text::yearFromText(v.toString());
        if (std::isfinite(y)) return y;
    }
    return v;
}

} // namespace

ChrResult ChrBuilder::build(const QJsonObject& specIn)
{
    ChrResult R;
    const QJsonObject spec = yearsToNumbers(specIn).toObject();
    const QJsonArray evSpec = spec.value("events").toArray();
    if (evSpec.isEmpty()) {
        R.errors << QStringLiteral("La spécification ne contient aucun événement (« events »).");
        return R;
    }

    bool curveKnown = true;
    QString curveName = spec.value("curve").toString();
    // Courbe âge-profondeur : type Profondeur de ChronoModel (process_type 80, 'P'). Convention de ChronoModel : la
    // profondeur est négative, 0 à la surface (10 m s'écrit -10) ; elle va dans X_Inc_Depth, son erreur dans
    // sX_Alpha95_sDepth. Des profondeurs sans « curve » suffisent à la demander.
    static const QSet<QString> ageDepthNames = { "age_depth", "age-depth", "agedepth", "age_profondeur", "age-profondeur",
                                                 "age profondeur", "profondeur", "courbe age-profondeur", "depth" };
    bool anyDepth = false;
    for (const QJsonValue& ev : evSpec) anyDepth = anyDepth || ev.toObject().contains("depth") || ev.toObject().contains("profondeur");
    if (curveName.trimmed().isEmpty() && anyDepth) {
        curveName = QStringLiteral("age_depth");
        R.warnings << QStringLiteral("Profondeurs données sans type de courbe : courbe âge-profondeur utilisée.");
    }
    const bool ageDepth = ageDepthNames.contains(text::fold(curveName).trimmed());
    const int processType = ageDepth ? 80 : curveProcessType(curveName, &curveKnown);
    if (!curveKnown)
        R.warnings << QStringLiteral("Type de courbe « %1 » inconnu : modèle sans courbe utilisé.").arg(curveName);
    const bool withCurve = processType != 78;

    // Couleur facultative (« color » ou « couleur ») d'une phase, d'un événement ou d'une date.
    auto applyColor = [&](QJsonObject& target, const QJsonObject& src, const QString& what) {
        const QJsonValue cv = src.contains("color") ? src.value("color") : src.value("couleur");
        if (cv.isUndefined() || cv.isNull() || (cv.isString() && cv.toString().trimmed().isEmpty())) return;
        int r = 0, g = 0, b = 0;
        if (parseColor(cv, r, g, b)) setColor(target, r, g, b);
        else R.warnings << QStringLiteral("Couleur « %1 » non reconnue pour %2 : couleur par défaut utilisée.")
                               .arg(cv.isString() ? cv.toString() : QString::fromUtf8(QJsonDocument(QJsonArray{ cv }).toJson(QJsonDocument::Compact)), what);
    };

    // ---------- Phases ----------
    QJsonArray phases;
    QHash<QString, int> phaseId;
    int nextPhase = 1;
    auto ensurePhase = [&](const QString& name) -> int {
        const QString key = name.trimmed();
        if (phaseId.contains(key)) return phaseId[key];
        const int id = nextPhase++;
        phaseId[key] = id;
        QJsonObject p;
        p["id"] = id;
        p["name"] = key;
        setColor(p, 45, 100 + (id * 37) % 120, 150);
        p["item_x"] = 0.0;       // positionnées plus bas
        p["item_y"] = 0.0;
        p["is_current"] = false;
        p["is_selected"] = false;
        p["tau_type"] = 0;
        p["tau_fixed"] = 0.0;
        p["tau_min"] = 0.0;
        p["tau_max"] = 0.0;
        phases.append(p);
        return id;
    };
    for (const QJsonValue& v : spec.value("phases").toArray()) {
        const QString n = v.isObject() ? v.toObject().value("name").toString() : v.toString();
        if (n.trimmed().isEmpty()) continue;
        const int pid = ensurePhase(n);
        if (v.isObject())
            for (int i = 0; i < phases.size(); ++i)
                if (phases[i].toObject().value("id").toInt() == pid) {
                    QJsonObject p = phases[i].toObject();
                    applyColor(p, v.toObject(), QStringLiteral("la phase « %1 »").arg(n.trimmed()));
                    phases[i] = p;
                }
    }

    // ---------- Événements ----------
    QJsonArray events;
    QHash<QString, int> eventId;
    Range span;
    int nextEvent = 1, amWithoutSpan = 0, nextDate = 0;     // amWithoutSpan : dates AM ou densités (étendue inconnue)

    for (const QJsonValue& ev : evSpec) {
        const QJsonObject e = ev.toObject();
        QString name = e.value("name").toString().trimmed();
        if (name.isEmpty()) name = QStringLiteral("Evenement %1").arg(nextEvent);
        if (eventId.contains(name)) {
            R.errors << QStringLiteral("Nom d'événement en double : « %1 ».").arg(name);
            continue;
        }

        QJsonObject o;
        const int id = nextEvent++;
        eventId[name] = id;
        o["id"] = id;
        o["name"] = name;
        setColor(o, 71, 112, 76);
        applyColor(o, e, QStringLiteral("l'événement « %1 »").arg(name));
        o["item_x"] = 0.0;       // diagramme stratigraphique, une fois les contraintes connues (layoutEvents)
        o["item_y"] = 0.0;
        o["is_current"] = false;
        o["is_selected"] = false;

        QStringList pids;
        for (const QJsonValue& p : e.value("phases").toArray()) {
            const QString pn = p.toString().trimmed();
            if (pn.isEmpty()) continue;
            if (!phaseId.contains(pn))
                R.warnings << QStringLiteral("Phase « %1 » créée automatiquement (événement « %2 »).").arg(pn, name);
            pids << QString::number(ensurePhase(pn));
        }
        o["phase_ids"] = pids.join(',');

        const bool isBound = e.value("type").toString().toLower() == "bound";
        o["type"] = isBound ? 1 : 0;
        o["point_type"] = e.value("node").toBool(false) ? 1 : 0;

        if (isBound) {
            if (!e.contains("known_fixed")) {
                R.errors << QStringLiteral("La borne « %1 » n'a pas de « known_fixed ».").arg(name);
            } else {
                const double kf = e.value("known_fixed").toDouble();
                o["known_fixed"] = kf;
                span.add(kf);
            }
        }

        // Dates
        QJsonArray dates;
        int dn = 0;
        bool flatDate = false;
        const QJsonArray dateSpecs = eventDates(e, &flatDate);
        if (flatDate)
            R.warnings << QStringLiteral("Événement « %1 » : datation écrite directement sur l'événement (sans « dates »), reprise telle quelle.").arg(name);
        for (const QJsonValue& dv : dateSpecs) {
            QJsonObject d = dv.toObject();
            if (!d.contains("name"))
                for (const char* k : { "ref", "reference", "code", "lab_ref", "labo" })
                    if (d.value(k).isString()) { d["name"] = d.value(k); break; }
            const QString type = (d.contains("type") ? d.value("type") : d.value("method")).toString().trimmed().toLower();
            // Erreurs que le modèle de langage doit corriger (pas de devinette silencieuse) :
            if (type == QLatin1String("bound") || type == QLatin1String("borne")) {          // borne écrite comme une date
                R.errors << QStringLiteral("« %1 » : une borne se déclare sur l'événement ({\"name\":\"%1\",\"type\":\"bound\",\"known_fixed\":…}), "
                                           "pas dans ses dates.").arg(name);
                continue;
            }
            if (const QString bad = MethodRunner::obsoleteFieldError(d); !bad.isEmpty()) {     // refYear, errorF…
                R.errors << QStringLiteral("Date de « %1 » : %2.").arg(name, bad);
                continue;
            }
            QJsonObject out;
            out["UUID"] = uuid4();
            out["id"] = nextDate;               // incrémenté seulement si la date est retenue (voir plus bas)
            out["name"] = d.value("name").toString(QStringLiteral("%1-%2").arg(name).arg(++dn));
            out["valid"] = true;
            setColor(out, 71, 112, 76);
            applyColor(out, d, QStringLiteral("la date « %1 »").arg(out["name"].toString()));
            out["is_selected"] = false;
            out["origin"] = 0;
            out["subdates"] = QJsonArray();
            out["delta_type"] = -1;
            out["delta_average"] = 0; out["delta_error"] = 0; out["delta_fixed"] = 0;
            out["delta_max"] = 0;     out["delta_min"] = 0;
            double wlo = 0.0, whi = 0.0;
            {
                QString why;
                if (!wiggleToChr(d.value("wiggle"), out, why, wlo, whi)) {
                    R.errors << QStringLiteral("Date « %1 » (événement « %2 ») : %3.").arg(out["name"].toString(), name, why);
                    continue;
                }
            }
            // Plage d'étude : la date t ET l'événement t + δ doivent y tenir.
            auto addSpan = [&](double a, double b) { span.add(a); span.add(b); span.add(a + wlo); span.add(b + whi); };

            if (type == "gauss") {
                const double age = d.value("age").toDouble(std::nan(""));
                const double err = d.value("error").toDouble(std::nan(""));
                if (std::isnan(age) || std::isnan(err) || err <= 0) {
                    R.errors << QStringLiteral("Date gaussienne invalide dans « %1 » (âge et erreur > 0 requis).").arg(name);
                    continue;
                }
                out["plugin_id"] = "gauss";
                out["sampler"] = kDateSampler;
                out["data"] = QJsonObject{ { "age", age }, { "error", err }, { "mode", "none" },
                                           { "curve", "" }, { "a", 0 }, { "b", 1 }, { "c", 0 } };
                addSpan(age - 3 * err, age + 3 * err);
            } else if (type == "unif" || type == "uniform") {
                const double mn = d.value("min").toDouble(std::nan(""));
                const double mx = d.value("max").toDouble(std::nan(""));
                if (std::isnan(mn) || std::isnan(mx) || mn >= mx) {
                    R.errors << QStringLiteral("Date uniforme invalide dans « %1 » (min < max requis).").arg(name);
                    continue;
                }
                out["plugin_id"] = "unif";
                out["sampler"] = kDateSampler;
                out["data"] = QJsonObject{ { "min", mn }, { "max", mx } };
                addSpan(mn, mx);
            } else if (type == "tl" || type == "osl" || type == "tl/osl") {
                // Luminescence : âge « avant ref_year » (années), erreur 1σ. L'année de référence est l'année du prélèvement/de la mesure.
                const double age = d.value("age").toDouble(std::nan(""));
                const double err = d.value("error").toDouble(std::nan(""));
                if (std::isnan(age) || std::isnan(err) || err <= 0) {
                    R.errors << QStringLiteral("Date TL/OSL invalide dans « %1 » (âge et erreur > 0 requis).").arg(name);
                    continue;
                }
                // L'âge TL/OSL est compté depuis l'année de la mesure : sans elle, la date est fausse (pas de valeur par défaut).
                if (!d.value("ref_year").isDouble()) {
                    R.errors << QStringLiteral("Date TL/OSL « %1 » (événement « %2 ») : année de la mesure « ref_year » manquante "
                                               "(demander l'année de la mesure à l'utilisateur).").arg(out["name"].toString(), name);
                    continue;
                }
                const double ref = d.value("ref_year").toDouble();
                out["plugin_id"] = "tl/osl";
                out["sampler"] = kDateSampler;
                out["data"] = QJsonObject{ { "age", age }, { "error", err }, { "ref_year", ref } };
                addSpan(ref - age - 3 * err, ref - age + 3 * err);
            } else if (type == "am" || type == "archeomagnetisme") {
                // Archéomagnétisme : mesure(s) comparée(s) aux courbes de référence de ChronoModel.
                const QString proc = d.value("process").toString("I").trimmed().toUpper();
                const int code = amProcessCode(proc);
                if (code == 0) {
                    R.errors << QStringLiteral("Date archéomagnétique « %1 » : « process » doit valoir I, D, F, ID, IF ou IDF.").arg(out["name"].toString());
                    continue;
                }
                const bool needI = proc.contains('I'), needD = proc.contains('D'), needF = proc.contains('F');
                // La déclinaison seule demande aussi l'inclinaison : son erreur vaut α95 / (2,448 · cos I).
                const bool useInc = needI || needD;
                auto num = [&](const char* k) { return d.value(k).toDouble(std::nan("")); };
                QString missing;
                if (useInc && std::isnan(num("inc"))) missing += " inc";
                if (needD && std::isnan(num("dec")))  missing += " dec";
                if ((needI || needD) && !(num("alpha95") > 0)) missing += " alpha95";
                if (needF && std::isnan(num("field"))) missing += " field";
                if (needF && !(num("error_f") > 0))    missing += " error_f";
                if (!missing.isEmpty()) {
                    R.errors << QStringLiteral("Date archéomagnétique « %1 » (process %2) : valeur(s) manquante(s) ou invalide(s) :%3.")
                                    .arg(out["name"].toString(), proc, missing);
                    continue;
                }
                // Courbe inconnue de SeldonH : souvent la référence de l'échantillon (« am Rennes2025 inclinaison 65 »)
                // prise pour une courbe par le modèle de langage. Sans « name » et sans suffixe de composante (_i, _d, _f,
                // convention des courbes de ChronoModel), elle devient le nom de la date et la courbe par défaut est
                // utilisée ; sinon elle est gardée (elle peut être installée dans ChronoModel) avec un avertissement.
                const QStringList knownCurves = CurveStore::names(QStringLiteral("am"));
                static const QRegularExpression compSuffix(QStringLiteral("_[idf]$"));
                for (const char* k : { "curve_i", "curve_d", "curve_f" }) {
                    if (!d.contains(k) || knownCurves.isEmpty()) continue;
                    const QString given = d.value(k).toString().trimmed();
                    QString base = given.toLower();
                    if (base.endsWith(".ref")) base.chop(4);
                    if (base.isEmpty() || knownCurves.contains(base)) continue;
                    if (!d.contains("name") && !compSuffix.match(base).hasMatch()) {
                        out["name"] = given;
                        d.insert("name", given);
                        d.remove(k);
                        R.warnings << QStringLiteral("Date archéomagnétique « %1 » : « %1 » n'est pas une courbe de référence connue, "
                                                     "elle est prise comme nom (référence) de la date.").arg(given);
                    } else {
                        R.warnings << QStringLiteral("Date archéomagnétique « %1 » : courbe « %2 » inconnue de SeldonH ; vérifiez "
                                                     "qu'elle est installée dans ChronoModel (dossier Calib/AM).")
                                        .arg(out["name"].toString(), refName(given));
                    }
                }
                const bool hasCurves = d.contains("curve_i") || d.contains("curve_d") || d.contains("curve_f");
                if (!hasCurves)
                    R.warnings << QStringLiteral("Date archéomagnétique « %1 » : courbes de référence par défaut (%2, %3, %4). "
                                                 "Choisissez celles qui conviennent à la région.")
                                    .arg(out["name"].toString(), kAmCurveI, kAmCurveD, kAmCurveF);
                auto cur = [&](const char* k, const QString& def) { return refName(d.value(k).toString(def)); };
                out["plugin_id"] = "am";
                out["sampler"] = kDateSampler;
                out["data"] = QJsonObject{
                    { "process_type", code },
                    // Grandeurs non utilisées par le processus : 0, erreurs non utilisées : 1 (jamais d'erreur nulle,
                    // comme les valeurs par défaut que ChronoModel laisse dans ces champs).
                    { "inc",     useInc ? num("inc") : 0.0 },
                    { "dec",     needD ? num("dec") : 0.0 },
                    { "alpha95", useInc ? num("alpha95") : 1.0 },
                    { "field",   needF ? num("field") : 0.0 },
                    { "error_f", needF ? num("error_f") : 1.0 },
                    { "integration_steps", d.value("integration_steps").toInt(500) },
                    { "refI_curve", cur("curve_i", kAmCurveI) },
                    { "refD_curve", cur("curve_d", kAmCurveD) },
                    { "refF_curve", cur("curve_f", kAmCurveF) }
                };
                // L'étendue de la date n'est pas connue sans la courbe : on ne l'ajoute pas à la plage d'étude.
                ++amWithoutSpan;
            } else if (type == "f14c") {
                // F14C (plugin « f14c ») : fraction moderne, erreur, courbe « xxx.14c ». Format relevé dans
                // test_tous_plugin_wiggle.chr : "data": {"error":0.01, "fraction":1.2, "ref_curve":"bomb13nh1.14c"}.
                const double F = d.value("f14c").toDouble(d.value("fraction").toDouble(std::nan("")));
                const double err = d.value("error").toDouble(std::nan(""));
                if (std::isnan(F) || !(F > 0) || std::isnan(err) || err <= 0) {
                    R.errors << QStringLiteral("Date F14C invalide dans « %1 » (fraction « f14c » > 0 et erreur > 0 requises).").arg(name);
                    continue;
                }
                QString curve = c14CurveName(d.value("curve").toString(d.value("ref_curve").toString()));
                if (curve.isEmpty()) {
                    curve = QStringLiteral("bomb21nhz1.14c");
                    R.warnings << QStringLiteral("Date F14C « %1 » : courbe bomb21nhz1 (hémisphère nord, zone 1) utilisée par défaut. "
                                                 "Précisez « curve » et vérifiez qu'elle est installée dans ChronoModel.").arg(out["name"].toString());
                }
                out["plugin_id"] = "f14c";
                out["sampler"] = kDateSampler;
                out["data"] = QJsonObject{ { "error", err }, { "fraction", F }, { "ref_curve", curve } };
                const MethodResult cal = MethodRegistry::instance().compute(
                    QJsonObject{ { "method", "f14c" }, { "f14c", F }, { "error", err }, { "curve", curve.chopped(4) } });
                if (cal.ok && !cal.hpd95.empty()) {
                    double lo = cal.hpd95.front().from, hi = cal.hpd95.front().to;
                    for (const Interval& iv : cal.hpd95) { lo = std::min(lo, iv.from); hi = std::max(hi, iv.to); }
                    addSpan(lo - 5, hi + 5);
                } else {
                    R.warnings << QStringLiteral("Date F14C « %1 » : courbe « %2 » inconnue de SeldonH ; plage d'étude estimée "
                                                 "(1950 à aujourd'hui), à vérifier.").arg(out["name"].toString(), curve);
                    addSpan(1950, QDate::currentDate().year());
                }
            } else if (type == "density" || type == "densite") {
                // Densité tabulée (plugin « density ») : "data": {"curve":"fichier.csv"} ; le fichier doit être dans le dossier
                // Calib/Density de ChronoModel (nom comparé en minuscules). SeldonH n'a pas le fichier : étendue inconnue.
                QString file = d.value("curve").toString(d.value("file").toString()).trimmed();
                if (file.isEmpty()) {
                    R.errors << QStringLiteral("Densité tabulée dans « %1 » : nom du fichier CSV (« curve ») requis.").arg(name);
                    continue;
                }
                if (!file.endsWith(".csv", Qt::CaseInsensitive)) file += QStringLiteral(".csv");
                out["plugin_id"] = "density";
                out["sampler"] = kDateSampler;
                out["data"] = QJsonObject{ { "curve", file } };
                R.warnings << QStringLiteral("Densité « %1 » : le fichier « %2 » doit se trouver dans le dossier Calib/Density de "
                                             "ChronoModel.").arg(out["name"].toString(), file);
                ++amWithoutSpan;
            } else if (type == "c14" || type == "14c" || type == "radiocarbone") {
                // Radiocarbone (plugin « 14c ») : âge conventionnel BP, erreur 1σ, courbe « xxx.14c », ΔR et son erreur
                // (0 si non marin). Format relevé dans un projet réel (Castelnau_version_19-12-2016.chr) :
                //   "data": {"age":1290, "delta_r":0, "delta_r_error":0, "error":30, "ref_curve":"intcal13.14c"}
                const double age = d.value("age").toDouble(std::nan(""));
                const double err = d.value("error").toDouble(std::nan(""));
                if (std::isnan(age) || std::isnan(err) || err <= 0) {
                    R.errors << QStringLiteral("Date radiocarbone invalide dans « %1 » (âge BP et erreur > 0 requis).").arg(name);
                    continue;
                }
                QString curve = c14CurveName(d.value("curve").toString(d.value("ref_curve").toString()));
                if (curve.isEmpty()) {
                    curve = QStringLiteral("intcal20.14c");
                    R.warnings << QStringLiteral("Date radiocarbone « %1 » : courbe IntCal20 utilisée par défaut. Précisez « curve » "
                                                 "(shcal20 pour l'hémisphère sud, marine20 pour un échantillon marin).").arg(out["name"].toString());
                }
                const bool marine = curve.startsWith("marine");
                const double dR = d.value("delta_r").toDouble(0.0);
                const double dRerr = d.value("delta_r_error").toDouble(0.0);
                if (marine && !d.contains("delta_r"))
                    R.warnings << QStringLiteral("Date radiocarbone « %1 » : courbe marine sans ΔR (0 utilisé). Précisez « delta_r » et "
                                                 "« delta_r_error ».").arg(out["name"].toString());
                out["plugin_id"] = "14c";
                out["sampler"] = kDateSampler;
                out["data"] = QJsonObject{ { "age", age }, { "delta_r", dR }, { "delta_r_error", dRerr },
                                           { "error", err }, { "ref_curve", curve } };
                // Plage d'étude : HPD à 95 % de la calibration (méthode de SeldonH, même courbe), avec une marge.
                QJsonObject rq{ { "method", "c14" }, { "age", age }, { "error", err }, { "curve", curve.chopped(4) } };
                if (dR != 0.0 || dRerr != 0.0) { rq["delta_r"] = dR; rq["delta_r_error"] = dRerr; rq["has_delta_r"] = true; }
                const MethodResult cal = MethodRegistry::instance().compute(rq);
                if (cal.ok && !cal.hpd95.empty()) {
                    double lo = cal.hpd95.front().from, hi = cal.hpd95.front().to;
                    for (const Interval& iv : cal.hpd95) { lo = std::min(lo, iv.from); hi = std::max(hi, iv.to); }
                    addSpan(lo - 2 * err, hi + 2 * err);
                } else {
                    // Courbe absente de SeldonH (elle peut exister dans ChronoModel) : estimation grossière 1950 − âge.
                    R.warnings << QStringLiteral("Date radiocarbone « %1 » : courbe « %2 » inconnue de SeldonH ; vérifiez qu'elle est "
                                                 "installée dans ChronoModel et ajustez la plage d'étude.").arg(out["name"].toString(), curve);
                    addSpan(1950 - age - dR - 4 * err - 300, 1950 - age - dR + 4 * err + 300);
                }
            } else {
                R.errors << QStringLiteral("Type de date « %1 » non géré (événement « %2 »). Types gérés : gauss, unif, tl (TL/OSL), am (archéomagnétisme), c14 (radiocarbone), f14c, density (densité tabulée).")
                                .arg(type, name);
                continue;
            }
            ++nextDate;
            dates.append(out);
        }
        // Modèle incomplet : un événement sans date ne peut pas être calculé par ChronoModel.
        if (!isBound && dates.isEmpty())
            R.errors << QStringLiteral("L'événement « %1 » n'a aucune date : ajoutez au moins une datation (ou faites-en une borne).").arg(name);
        o["dates"] = dates;

        // Champs propres aux événements de courbe : toujours présents dans les projets réels (valeur 0 hors courbe).
        o["sampler"] = kEventSampler;
        for (const char* k : { "X_Inc_Depth", "Y_Dec", "Z_Field", "sX_Alpha95_sDepth", "sY", "sZ_sF" })
            o[k] = 0.0;
        if (ageDepth) {
            const QJsonValue dv = e.contains("depth") ? e.value("depth") : e.value("profondeur");
            const QJsonValue de = e.contains("depth_error") ? e.value("depth_error") : e.value("erreur_profondeur");
            auto toNum = [](const QJsonValue& v) {
                if (v.isDouble()) return v.toDouble();
                if (v.isString()) { bool ok = false; const double x = v.toString().trimmed().replace(',', '.').toDouble(&ok); if (ok) return x; }
                return std::nan("");
            };
            const double depth = toNum(dv), derr = toNum(de);
            if (std::isnan(depth))
                R.errors << QStringLiteral("Courbe âge-profondeur : l'événement « %1 » n'a pas de profondeur (« depth »).").arg(name);
            else {
                o["X_Inc_Depth"] = -std::abs(depth);
                if (!std::isnan(derr) && derr > 0) o["sX_Alpha95_sDepth"] = derr;
                else R.warnings << QStringLiteral("Courbe âge-profondeur : pas d'erreur sur la profondeur de « %1 » (0 utilisé).").arg(name);
            }
            o["sampler"] = isBound ? -1 : 1;
        } else if (withCurve) {
            o["sampler"] = isBound ? -1 : (processType == 85 ? 1 : 2);
            for (const char* k : { "X_Inc_Depth", "Y_Dec", "Z_Field", "SDec",
                                   "sX_Alpha95_sDepth", "sY", "sZ_sF" })
                if (e.contains(k)) o[k] = e.value(k).toDouble();
        }
        events.append(o);
    }

    // ---------- Contraintes ----------
    // Contraintes : chaque élément de la liste peut s'écrire
    //   ["A", "B"]                      A plus ancien que B (forme historique) ;
    //   ["A", "B", "C", …]              séquence du plus ancien au plus récent (tous des noms connus) ;
    //   ["A", "<relation>", "B"]        relation en toutes lettres, français ou anglais (voir relationOrder) ;
    //   {"older": "A", "younger": "B"}  ou {"before": "A", "after": "B"} ;
    //   {"a": "A", "relation": "<relation>", "b": "B"}.
    // Les doublons sont ignorés ; deux contraintes opposées ou un cycle (A < B < C < A) sont des erreurs.
    auto buildConstraints = [&](const QJsonArray& list, const QHash<QString, int>& ids,
                                const QString& what) {
        QJsonArray out;
        int cid = 1;
        QVector<QPair<QString, QString>> pairs;          // (plus ancien, plus récent)
        auto add = [&](const QString& olderName, const QString& youngerName, const QString& shown) {
            if (!ids.contains(olderName) || !ids.contains(youngerName)) {
                R.errors << QStringLiteral("Contrainte %1 « %2 » : nom inconnu (il ne correspond à aucun élément du modèle).").arg(what, shown);
                return;
            }
            if (olderName == youngerName) { R.warnings << QStringLiteral("Contrainte %1 « %2 » ignorée : même élément.").arg(what, shown); return; }
            if (pairs.contains({ olderName, youngerName })) return;
            if (pairs.contains({ youngerName, olderName })) {
                R.errors << QStringLiteral("Contraintes %1 contradictoires : « %2 » est donné à la fois plus ancien et plus récent que « %3 ».")
                                .arg(what, olderName, youngerName);
                return;
            }
            pairs.append({ olderName, youngerName });
        };
        auto relationFail = [&](const QString& shown) {
            R.errors << QStringLiteral("Contrainte %1 « %2 » : relation non reconnue (préciser par ex. avant / après, "
                                       "antérieur / postérieur, en dessous / au-dessus, précède / succède).").arg(what, shown);
        };
        for (const QJsonValue& v : list) {
            if (v.isObject()) {
                const QJsonObject o = v.toObject();
                auto str = [&](std::initializer_list<const char*> keys) {
                    for (const char* k : keys) if (o.contains(k)) return o.value(k).toString().trimmed();
                    return QString();
                };
                const QString od = str({ "older", "before", "anterieur", "antérieur", "plus_ancien" });
                const QString yg = str({ "younger", "after", "posterieur", "postérieur", "plus_recent" });
                if (!od.isEmpty() && !yg.isEmpty()) { add(od, yg, od + " → " + yg); continue; }
                const QString a = str({ "a", "from", "x", "first" }), b = str({ "b", "to", "y", "second" });
                const QString rel = str({ "relation", "rel", "type" });
                const QString shown = QStringLiteral("%1 %2 %3").arg(a, rel, b);
                const int ord = relationOrder(rel);
                if (a.isEmpty() || b.isEmpty() || ord == 0) { relationFail(shown); continue; }
                if (ord > 0) add(a, b, shown); else add(b, a, shown);
                continue;
            }
            const QJsonArray arr = v.toArray();
            QStringList names;
            for (const QJsonValue& x : arr) names << x.toString().trimmed();
            const QString shown = names.join(" → ");
            if (names.size() == 3 && !ids.contains(names[1])) {          // ["A", "relation", "B"]
                const int ord = relationOrder(names[1]);
                const QString rs = names.join(' ');
                if (ord == 0) { relationFail(rs); continue; }
                if (ord > 0) add(names[0], names[2], rs); else add(names[2], names[0], rs);
            } else if (names.size() >= 2) {                               // séquence du plus ancien au plus récent
                for (int i = 0; i + 1 < names.size(); ++i) add(names[i], names[i + 1], shown);
            } else {
                R.warnings << QStringLiteral("Contrainte %1 ignorée (2 noms attendus).").arg(what);
            }
        }
        // Cycle (A < B < C < A) : ChronoModel le refuserait.
        QHash<QString, QStringList> next;
        for (const auto& pr : pairs) next[pr.first] << pr.second;
        QHash<QString, int> state;                       // 0 non vu, 1 en cours, 2 fini
        QStringList stack;
        std::function<bool(const QString&)> dfs = [&](const QString& n) -> bool {
            state[n] = 1; stack << n;
            for (const QString& m : next.value(n)) {
                if (state.value(m) == 1) { stack << m; return true; }
                if (state.value(m) == 0 && dfs(m)) return true;
            }
            state[n] = 2; stack.removeLast();
            return false;
        };
        for (const auto& pr : pairs)
            if (state.value(pr.first) == 0 && dfs(pr.first)) {
                const int from = stack.indexOf(stack.last());
                R.errors << QStringLiteral("Contraintes %1 contradictoires (cycle) : %2.").arg(what, stack.mid(from).join(" → "));
                break;
            }
        for (const auto& pr : pairs) {
            QJsonObject c;
            c["id"] = cid++;
            c["bwd_id"] = ids[kBwdIsOlder ? pr.first : pr.second];
            c["fwd_id"] = ids[kBwdIsOlder ? pr.second : pr.first];
            out.append(c);
        }
        return out;
    };
    QJsonArray evConsSpec = spec.value("event_constraints").toArray();
    // Courbe âge-profondeur sans contrainte donnée : loi de superposition, du plus profond (plus ancien) au moins profond.
    if (ageDepth && evConsSpec.isEmpty()) {
        QVector<QPair<double, QString>> byDepth;
        for (const QJsonValue& v : std::as_const(events)) {
            const QJsonObject o = v.toObject();
            if (o.contains("X_Inc_Depth")) byDepth.append({ -o.value("X_Inc_Depth").toDouble(), o.value("name").toString() });
        }
        std::stable_sort(byDepth.begin(), byDepth.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        QJsonArray seq;
        for (const auto& pr : std::as_const(byDepth)) seq.append(pr.second);
        if (seq.size() >= 2) {
            evConsSpec.append(seq);
            R.warnings << QStringLiteral("Courbe âge-profondeur : contraintes ajoutées dans l'ordre des profondeurs (le plus profond est le plus ancien).");
        }
    }
    const QJsonArray evCons = buildConstraints(evConsSpec, eventId, "d'événements");
    const QJsonArray phCons = buildConstraints(spec.value("phase_constraints").toArray(), phaseId, "de phases");

    // Événements en diagramme stratigraphique (le plus récent en haut).
    layoutEvents(events, evCons);

    // Phases : même disposition que les événements (la plus récente en haut), d'après les contraintes de phases.
    layoutEvents(phases, phCons);

    // ---------- Paramètres ----------
    QJsonObject sIn = spec.value("settings").toObject();
    int tmin, tmax, step;
    if (sIn.contains("tmin") && sIn.contains("tmax")) {
        tmin = static_cast<int>(std::lround(sIn.value("tmin").toDouble()));
        tmax = static_cast<int>(std::lround(sIn.value("tmax").toDouble()));
    } else if (!span.valid() && amWithoutSpan > 0) {
        tmin = -1000; tmax = 2000;
        R.warnings << QStringLiteral("Plage d'étude par défaut pour les dates archéomagnétiques ou densités tabulées : -1000 à 2000. "
                                     "Ajustez « settings » (tmin, tmax) à la période couverte par les courbes choisies.");
    } else if (span.valid()) {
        const double margin = std::max(50.0, 0.1 * (span.hi - span.lo));
        tmin = static_cast<int>(std::floor((span.lo - margin) / 10.0) * 10.0);
        tmax = static_cast<int>(std::ceil((span.hi + margin) / 10.0) * 10.0);
    } else {
        R.errors << QStringLiteral("Impossible de déterminer tmin/tmax (aucune date) : précisez « settings ».");
        return R;
    }
    if (amWithoutSpan > 0 && span.valid() && !sIn.contains("tmin"))
        R.warnings << QStringLiteral("La plage d'étude calculée ne tient pas compte des dates archéomagnétiques ou des densités tabulées : vérifiez tmin et tmax.");
    if (tmin >= tmax) { R.errors << QStringLiteral("tmin doit être inférieur à tmax."); return R; }
    step = sIn.contains("step") ? std::max(1, sIn.value("step").toInt(1))
                                : std::max(1, static_cast<int>(std::ceil((tmax - tmin) / 5000.0)));
    if (!sIn.contains("tmin") && !(amWithoutSpan > 0 && !span.valid()))
        R.warnings << QStringLiteral("Plage d'étude calculée automatiquement : %1 à %2 (pas %3).").arg(tmin).arg(tmax).arg(step);

    // Valeurs par défaut relevées dans des projets ChronoModel 4.0 ; 4 chaînes = MCMC_NUM_CHAINS_DEFAULT de ChronoModel
    // (mcmc/MCMCSettings.h). Une graine entière tirée au hasard par chaîne.
    constexpr int kChains = 4;
    QJsonArray seeds;
    for (int i = 0; i < kChains; ++i) seeds.append(static_cast<int>(QRandomGenerator::global()->bounded(1, 100000)));
    QJsonObject mcmc{
        { "anneal_dwell", 1000 }, { "anneal_recurrence", 1000 }, { "anneal_temp", 10 },
        { "iter_per_batch", 100 }, { "max_batches", 100 }, { "mixing_level", 0.99 },
        { "num_burn", 1000 }, { "num_iter", 100000 }, { "num_proc", kChains },
        { "seeds", seeds }, { "thinning_interval", 10 }
    };
    const QJsonObject mIn = spec.value("mcmc").toObject();
    for (auto it = mIn.begin(); it != mIn.end(); ++it)
        if (mcmc.contains(it.key())) mcmc[it.key()] = it.value();

    // Sans courbe (process_type 78), ChronoModel écrit alpha 0, speed_threshold 0, use_err_mesure vrai, variance_fixed 1.
    QJsonObject curve{
        { "alpha", withCurve ? 1.0 : 0.0 }, { "coeff_lissage_type", 66 }, { "process_type", processType },
        { "speed_threshold", withCurve ? 1.0 : 0.0 }, { "time_type", 66 }, { "use_err_mesure", !withCurve },
        { "variance_fixed", 1.0 }, { "variance_type", 66 }
    };
    if (ageDepth)            // réglages de teilleulc2.chr (variance globale « G », erreurs de mesure utilisées)
        curve = QJsonObject{ { "alpha", 0.0 }, { "coeff_lissage_type", 66 }, { "process_type", 80 }, { "speed_threshold", 0.0 },
                             { "time_type", 66 }, { "use_err_mesure", true }, { "variance_fixed", 1.0 }, { "variance_type", 71 } };
    else if (withCurve)
        R.warnings << QStringLiteral("Paramètres de courbe (alpha, speed_threshold, variance_fixed) : valeurs par défaut arbitraires.");

    QJsonObject root;
    root["app_version"] = "4.0.0";
    root["curve"] = curve;
    root["dates_trash"] = QJsonArray();
    // Clés qu'écrit ChronoModel 4.0.0 à l'enregistrement (relevées dans test_strati_corrige.chr) ; ajoutées pour que le
    // fichier produit soit identique à un fichier réenregistré par ChronoModel.
    root["event_model"] = 1;
    root["format_date"] = 0;
    root["format_date_custom"] = QStringLiteral("Years");
    root["events"] = events;
    root["events_constraints"] = evCons;
    root["events_trash"] = QJsonArray();
    root["mcmc"] = mcmc;
    root["phases"] = phases;
    root["phases_constraints"] = phCons;
    root["settings"] = QJsonObject{ { "step", step }, { "step_forced", false }, { "tmax", tmax }, { "tmin", tmin } };

    R.chr = root;
    R.suggestedName = slug(spec.value("name").toString());
    R.ok = R.errors.isEmpty();
    return R;
}

namespace {

// Résumé affiché à la place du bloc de modèle.
QString modelCard(const QString& json)
{
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(json.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject())
        return QStringLiteral("> ⚠ **Modèle ChronoModel illisible** : la description produite n'est pas valide. "
                              "Demandez à l'assistant de la réécrire.");
    const QJsonObject spec = d.object();
    const ChrResult r = ChrBuilder::build(spec);
    if (!r.ok) {
        QString s = QStringLiteral("> ⚠ **Modèle ChronoModel incomplet : pas encore enregistrable.**\n>\n");
        for (const QString& e : r.errors) s += QStringLiteral("> - %1\n").arg(e);
        return s + QStringLiteral(">\n> Donnez les informations manquantes : SeldonH proposera l'enregistrement d'un fichier complet.");
    }
    int dates = 0;
    const QJsonArray events = spec.value("events").toArray();
    for (const QJsonValue& e : events) dates += e.toObject().value("dates").toArray().size();
    const int phases = spec.value("phases").toArray().size();
    const int constraints = spec.value("event_constraints").toArray().size() + spec.value("phase_constraints").toArray().size();
    QStringList what;
    what << QStringLiteral("%1 événement%2").arg(events.size()).arg(events.size() > 1 ? "s" : "")
         << QStringLiteral("%1 datation%2").arg(dates).arg(dates > 1 ? "s" : "");
    if (phases > 0) what << QStringLiteral("%1 phase%2").arg(phases).arg(phases > 1 ? "s" : "");
    if (constraints > 0) what << QStringLiteral("%1 contrainte%2").arg(constraints).arg(constraints > 1 ? "s" : "");
    const QString name = spec.value("name").toString().trimmed();
    return QStringLiteral("> 📦 **Modèle ChronoModel prêt**%1 : %2. Enregistrez-le avec « 💾 Enregistrer le modèle (.chr) » "
                          "sous cette réponse.")
        .arg(name.isEmpty() ? QString() : QStringLiteral(" « %1 »").arg(name), what.join(", "));
}

QString foldText(const QString& s)
{
    QString out;
    for (const QChar c : s.normalized(QString::NormalizationForm_D))
        if (c.category() != QChar::Mark_NonSpacing) out += c.toLower();
    return out;
}

} // namespace

QString ChrBuilder::displayMarkdown(const QString& md, bool showJson)
{
    static const QRegularExpression block("^```[ \\t]*(chronomodel|json)[ \\t]*\\n(.*?)^```[ \\t]*$",
                                          QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption |
                                          QRegularExpression::CaseInsensitiveOption);
    QString out;
    qsizetype pos = 0;
    auto it = block.globalMatch(md);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString body = m.captured(2);
        const bool model = m.captured(1).compare(QLatin1String("chronomodel"), Qt::CaseInsensitive) == 0
                        || QJsonDocument::fromJson(body.toUtf8()).object().contains("events");
        if (!model) continue;
        out += md.mid(pos, m.capturedStart() - pos);
        if (showJson) out += m.captured(0) + QStringLiteral("\n\n");
        out += modelCard(body);
        pos = m.capturedEnd();
    }
    QString rest = md.mid(pos);
    // Bloc de modèle encore ouvert (réponse en cours d'écriture, ou coupée) : rien d'incomplet à l'écran.
    static const QRegularExpression open("^```[ \\t]*chronomodel[ \\t]*$",
                                         QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    if (!showJson)
        if (const auto mo = open.match(rest); mo.hasMatch())
            rest = rest.left(mo.capturedStart()) + QStringLiteral("*📦 Préparation du modèle ChronoModel…*");
    return out + rest;
}

bool ChrBuilder::asksForJson(const QString& question)
{
    static const QRegularExpression re(QStringLiteral("\\bjson\\b|bloc (de code )?chronomodel|specification (json |du modele|de modele)|"
                                                      "code (source )?du modele|structure (json|du (fichier|modele))"));
    return re.match(foldText(question)).hasMatch();
}

bool ChrBuilder::extractSpec(const QString& md, QJsonObject& spec, QString* err)
{
    // Les clôtures ``` doivent être en début de ligne (une mention « ```chronomodel » au milieu d'une
    // phrase ne doit pas être prise pour un bloc).
    static const QRegularExpression re("^```[ \\t]*(chronomodel|json)[ \\t]*\\n(.*?)^```",
                                       QRegularExpression::DotMatchesEverythingOption |
                                       QRegularExpression::MultilineOption |
                                       QRegularExpression::CaseInsensitiveOption);
    auto it = re.globalMatch(md);
    QString lastError;
    while (it.hasNext()) {
        const auto m = it.next();
        QJsonParseError pe;
        const QJsonDocument d = QJsonDocument::fromJson(m.captured(2).toUtf8(), &pe);
        if (pe.error != QJsonParseError::NoError) {
            lastError = QStringLiteral("JSON invalide : %1 (position %2)").arg(pe.errorString()).arg(pe.offset);
            continue;
        }
        if (d.isObject() && d.object().contains("events")) {
            spec = d.object();
            return true;
        }
    }
    if (err) *err = lastError.isEmpty() ? QStringLiteral("Aucun bloc de modèle trouvé.") : lastError;
    return false;
}

QStringList ChrBuilder::crossCheck(const QJsonObject& spec, const QString& source)
{
    QStringList out;
    const QString src = text::fold(source);
    // Nombres de la source (valeurs absolues ; un âge en ka compte aussi en années).
    QVector<double> nums;
    static const QRegularExpression numRe(QStringLiteral(R"((?<![\w.,])-?(\d+(?:[.,]\d+)?))"));
    for (auto it = numRe.globalMatch(src); it.hasNext();) {
        const double v = text::toNumber(it.next().captured(1));
        nums << v << v * 1000.0;
    }
    auto known = [&](double v) {
        const double a = std::abs(v);
        for (double n : std::as_const(nums)) if (std::abs(n - a) <= 1e-9 * std::max(1.0, a)) return true;
        return false;
    };
    auto fmt = [](double v) { return QString::number(v, 'g', 10); };
    static const char* const valueKeys[] = { "age", "error", "min", "max", "inc", "dec", "alpha95", "field", "error_f", "f14c",
                                             "ref_year", "delta_r", "delta_r_error" };
    struct Pair { double v, e; };
    QVector<Pair> specPairs;
    QStringList specNames;
    QStringList unknownValues;
    for (const QJsonValue& ev : spec.value("events").toArray()) {
        const QJsonObject e = ev.toObject();
        const QString en = e.value("name").toString();
        if (e.contains("known_fixed") && !known(e.value("known_fixed").toDouble()))
            unknownValues << QStringLiteral("%1 (borne « %2 »)").arg(fmt(e.value("known_fixed").toDouble()), en);
        for (const char* k : { "depth", "depth_error" })
            if (e.value(k).isDouble() && !known(e.value(k).toDouble()))
                unknownValues << QStringLiteral("%1 (%2 de « %3 »)").arg(fmt(e.value(k).toDouble()), QString::fromLatin1(k), en);
        for (const QJsonValue& dv : eventDates(e)) {
            const QJsonObject d = dv.toObject();
            for (const char* k : valueKeys)
                if (d.value(k).isDouble() && !known(d.value(k).toDouble()))
                    unknownValues << QStringLiteral("%1 (%2 d'une date de « %3 »)").arg(fmt(d.value(k).toDouble()), QString::fromLatin1(k), en);
            const QString dn = d.value("name").toString().trimmed();
            if (!dn.isEmpty()) {
                specNames << text::fold(dn);
                if (!src.contains(text::fold(dn)))
                    out << QStringLiteral("Référence « %1 » (événement « %2 ») absente de votre texte : inventée par le modèle ?").arg(dn, en);
            }
            const double v = d.value("age").toDouble(d.value("f14c").toDouble(d.value("field").toDouble(std::nan(""))));
            const double er = d.value("error").toDouble(d.value("error_f").toDouble(std::nan("")));
            if (std::isfinite(v) && std::isfinite(er)) specPairs << Pair{ v, er };
        }
    }
    if (!unknownValues.isEmpty())
        out << QStringLiteral("Valeur(s) absente(s) de votre texte : %1. Inventée(s) ou recopiée(s) d'un exemple par le modèle ?")
                   .arg(unknownValues.join(QStringLiteral(", ")));
    // Données de la source oubliées : paires « valeur ± erreur » et références de laboratoire.
    for (const text::ValueError& ve : text::findValueErrors(src)) {
        bool found = false;
        for (const Pair& p : std::as_const(specPairs))
            if (std::abs(std::abs(p.v) - std::abs(ve.value)) < 1e-9 && std::abs(p.e - ve.error) < 1e-9) { found = true; break; }
        if (!found) out << QStringLiteral("« %1 ± %2 » de votre texte n'est dans aucune date du modèle.").arg(fmt(ve.value), fmt(ve.error));
    }
    static const QRegularExpression refRe(QStringLiteral(R"(\b([a-z]{2,6}-\d{2,})\b)"));
    QStringList seen;
    for (auto it = refRe.globalMatch(src); it.hasNext();) {
        const QString ref = it.next().captured(1);
        if (seen.contains(ref)) continue;
        seen << ref;
        bool used = false;
        for (const QString& n : std::as_const(specNames)) used = used || n.contains(ref);
        if (!used) out << QStringLiteral("Référence « %1 » de votre texte absente du modèle.").arg(ref);
    }
    return out;
}

QString ChrBuilder::summarize(const QJsonObject& chr, int maxEvents)
{
    QString s;
    const QJsonObject st = chr.value("settings").toObject();
    const QJsonObject mc = chr.value("mcmc").toObject();
    s += QStringLiteral("Projet ChronoModel (version %1)\n").arg(chr.value("app_version").toString("?"));
    s += QStringLiteral("Plage d'étude : %1 à %2, pas %3\n").arg(st.value("tmin").toInt()).arg(st.value("tmax").toInt()).arg(st.value("step").toInt());
    s += QStringLiteral("Courbe : process_type=%1\n").arg(chr.value("curve").toObject().value("process_type").toInt(78));
    s += QStringLiteral("MCMC : %1 itérations, %2 burn-in, %3 chaînes, %4\n").arg(mc.value("num_iter").toInt()).arg(mc.value("num_burn").toInt())
             .arg(mc.value("num_proc").toInt())
             .arg(mc.value("anneal_recurrence").toInt(1000) > 0
                      ? QStringLiteral("recuit 2^%1 tous les %2 itérations").arg(mc.value("anneal_temp").toDouble(10)).arg(mc.value("anneal_recurrence").toInt(1000))
                      : QStringLiteral("sans recuit (anneal_recurrence = 0)"));

    QHash<int, QString> phaseName;
    const QJsonArray phases = chr.value("phases").toArray();
    for (const QJsonValue& p : phases) phaseName[p.toObject().value("id").toInt()] = p.toObject().value("name").toString();
    s += QStringLiteral("Phases (%1) : ").arg(phases.size());
    QStringList pn; for (auto it = phaseName.begin(); it != phaseName.end(); ++it) pn << it.value();
    s += pn.join(", ") + "\n";

    const QJsonArray events = chr.value("events").toArray();
    s += QStringLiteral("Événements (%1) :\n").arg(events.size());
    int n = 0;
    for (const QJsonValue& v : events) {
        if (n++ >= maxEvents) { s += QStringLiteral("  ... (%1 autres)\n").arg(events.size() - maxEvents); break; }
        const QJsonObject e = v.toObject();
        QStringList ph;
        for (const QString& id : e.value("phase_ids").toString().split(',', Qt::SkipEmptyParts)) ph << phaseName.value(id.toInt(), id);
        s += QStringLiteral("  - %1%2").arg(e.value("name").toString(), ph.isEmpty() ? "" : " [" + ph.join("/") + "]");
        if (e.value("type").toInt() == 1) s += QStringLiteral(" borne=%1").arg(e.value("known_fixed").toDouble());
        QStringList ds;
        for (const QJsonValue& dv : e.value("dates").toArray()) {
            const QJsonObject d = dv.toObject(), dd = d.value("data").toObject();
            const QString pl = d.value("plugin_id").toString();
            if (pl == "gauss")      ds << QStringLiteral("gauss %1±%2").arg(dd.value("age").toDouble()).arg(dd.value("error").toDouble());
            else if (pl == "unif")  ds << QStringLiteral("unif [%1;%2]").arg(dd.value("min").toDouble()).arg(dd.value("max").toDouble());
            else if (pl == "f14c")  ds << QStringLiteral("F14C %1±%2").arg(dd.value("fraction").toDouble()).arg(dd.value("error").toDouble());
            else if (pl == "14c")   ds << QStringLiteral("14C %1±%2").arg(dd.value("age").toDouble()).arg(dd.value("error").toDouble());
            else if (pl == "tl/osl") ds << QStringLiteral("TL/OSL %1±%2 (réf. %3)").arg(dd.value("age").toDouble()).arg(dd.value("error").toDouble()).arg(dd.value("ref_year").toDouble());
            else if (pl == "density") ds << QStringLiteral("densité tabulée « %1 »").arg(dd.value("curve").toString());
            else if (pl == "am") {
                static const char* names[] = { "?", "I", "D", "F", "ID", "IF", "IDF" };
                const int pt = dd.value("process_type").toInt();
                const QString pr = (pt >= 1 && pt <= 6) ? QString::fromLatin1(names[pt]) : QString::number(pt);
                QStringList v;
                if (pr.contains('I') || pr.contains('D')) v << QStringLiteral("I=%1").arg(dd.value("inc").toDouble());
                if (pr.contains('D')) v << QStringLiteral("D=%1").arg(dd.value("dec").toDouble());
                if (pr.contains('I') || pr.contains('D')) v << QStringLiteral("α95=%1").arg(dd.value("alpha95").toDouble());
                if (pr.contains('F')) v << QStringLiteral("F=%1±%2").arg(dd.value("field").toDouble()).arg(dd.value("error_f").toDouble());
                ds << QStringLiteral("archéomag. %1 (%2)").arg(pr, v.join(", "));
            }
            else                    ds << pl;
            if (const QString w = wiggleOfChr(d); !w.isEmpty()) ds.last() += QStringLiteral(" [wiggle %1]").arg(w);
        }
        if (!ds.isEmpty()) s += " : " + ds.join("; ");
        s += "\n";
    }
    // Contraintes en clair (plus ancien → plus récent), pour vérifier le sens.
    QHash<int, QString> eventName;
    for (const QJsonValue& v : events) eventName[v.toObject().value("id").toInt()] = v.toObject().value("name").toString();
    auto cons = [&](const QJsonArray& list, const QHash<int, QString>& names) {
        QStringList l;
        for (const QJsonValue& v : list) {
            const QJsonObject c = v.toObject();
            const int older = c.value(kBwdIsOlder ? "bwd_id" : "fwd_id").toInt(), younger = c.value(kBwdIsOlder ? "fwd_id" : "bwd_id").toInt();
            l << QStringLiteral("%1 → %2").arg(names.value(older, QString::number(older)), names.value(younger, QString::number(younger)));
        }
        return l;
    };
    const QStringList ec = cons(chr.value("events_constraints").toArray(), eventName);
    const QStringList pc = cons(chr.value("phases_constraints").toArray(), phaseName);
    s += QStringLiteral("Contraintes d'événements : %1 ; de phases : %2 (plus ancien → plus récent)\n").arg(ec.size()).arg(pc.size());
    if (!ec.isEmpty()) s += QStringLiteral("  Événements : %1\n").arg(ec.join(" ; "));
    if (!pc.isEmpty()) s += QStringLiteral("  Phases : %1\n").arg(pc.join(" ; "));
    return s;
}

} // namespace seldon
