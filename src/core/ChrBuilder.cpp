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

// Mesures d'un Event en mode courbe (I, D, F ou X, Y, Z), sur l'événement et pas dans une date. Clés de ChronoModel
// (X_Inc_Depth…) ou noms simples écrits par le modèle de langage (inc, dec, alpha95, field, error_f, en français aussi).
// Clés de ChronoModel relevées sur test_curveIDF.chr (projet I, D, F qui fonctionne, Philippe, 12/11/2026).
struct CurveKey { const char* chrKey; std::initializer_list<const char*> aliases; };
const CurveKey kCurveKeys[] = {
    { "X_Inc_Depth",       { "inc", "inclinaison", "inclination", "I" } },
    { "Y_Dec",             { "dec", "declinaison", "déclinaison", "declination", "D" } },
    { "Z_Field",           { "field", "intensite", "intensité", "intensity", "F" } },
    { "sX_Alpha95_sDepth", { "alpha95", "a95" } },
    { "sZ_sF",             { "error_f", "erreur_f", "sF", "sigma_f" } },
    { "sY",                {} },
    { "SDec",              {} },
};

QJsonObject curveMeasures(const QJsonObject& e)
{
    QJsonObject m;
    for (const CurveKey& k : kCurveKeys) {
        QJsonValue v = e.value(k.chrKey);
        for (const char* a : k.aliases) {
            if (!v.isUndefined()) break;
            v = e.value(a);
        }
        if (v.isUndefined() || v.isNull()) continue;
        const double d = v.isString() ? text::toNumber(v.toString()) : v.toDouble(std::nan(""));
        if (std::isfinite(d)) m[k.chrKey] = d;
    }
    return m;
}

// Nombre d'une valeur JSON (nombre ou texte « 20 », « 20,5 ») ; NaN si absent ou illisible.
double jsonNumber(const QJsonValue& v)
{
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) return text::toNumber(v.toString());
    return std::nan("");
}

// Durée d'une phase écrite par le modèle de langage : "duration_max": 20 (durée maximale connue) ou
// "uniform_span": true (« Uniform Span » de ChronoModel). Rien : durée inconnue (type 0, rien à signaler).
struct PhaseDuration { int type = 0; double max = 0.0; QString error; };
PhaseDuration phaseDuration(const QJsonObject& o)
{
    PhaseDuration d;
    QJsonValue mv(QJsonValue::Undefined);
    for (const char* k : { "duration_max", "duree_max", "durée_max", "max_duration", "tau_fixed" })
        if (o.contains(k)) { mv = o.value(k); break; }
    bool span = false;
    for (const char* k : { "uniform_span", "span_uniforme", "span" }) {
        const QJsonValue sv = o.value(k);
        span = span || sv.toBool(false) || (sv.isString() && text::fold(sv.toString()).contains(QLatin1String("unif")));
    }
    const QString dt = text::fold(o.value("duration").toString(o.value("duree").toString()));
    span = span || dt.contains(QLatin1String("unif")) || dt.contains(QLatin1String("span"));
    if (!mv.isUndefined()) {
        const double m = jsonNumber(mv);
        if (!std::isfinite(m) || m <= 0) { d.error = QStringLiteral("durée maximale illisible ou nulle (« duration_max »)."); return d; }
        if (span) { d.error = QStringLiteral("durée maximale ET span uniforme : choisir l'un des deux."); return d; }
        d.type = 1; d.max = m;
    } else if (span) d.type = 2;
    return d;
}

// Hiatus entre deux phases (PhaseConstraint::GammaType de ChronoModel) : "hiatus": 50 (hiatus ≥ 50, type 1) ou
// "hiatus_min" / "hiatus_max" (hiatus minimal dans [min ; max], type 2). Rien : inconnu (type 0).
struct Hiatus { int type = 0; double fixed = 1.0, min = 0.0, max = 1.0; QString error; };
Hiatus hiatusOf(const QJsonObject& o)
{
    Hiatus h;
    const QJsonValue f = o.contains("hiatus") ? o.value("hiatus") : o.value("gamma");
    const QJsonValue a = o.contains("hiatus_min") ? o.value("hiatus_min") : o.value("gamma_min");
    const QJsonValue b = o.contains("hiatus_max") ? o.value("hiatus_max") : o.value("gamma_max");
    if (!f.isUndefined()) {
        const double v = jsonNumber(f);
        if (!std::isfinite(v) || v < 0) { h.error = QStringLiteral("hiatus illisible ou négatif."); return h; }
        h.type = 1; h.fixed = v;
    } else if (!a.isUndefined() || !b.isUndefined()) {
        const double lo = jsonNumber(a), hi = jsonNumber(b);
        if (!std::isfinite(lo) || !std::isfinite(hi) || lo < 0 || hi < lo) {
            h.error = QStringLiteral("hiatus : il faut « hiatus_min » et « hiatus_max » avec 0 ≤ min ≤ max."); return h;
        }
        h.type = 2; h.min = lo; h.max = hi;
    }
    return h;
}

// ---------- Clés en double dans le bloc du modèle d'IA ----------
// Prompt réel du 13/11/2026 (mistral-small-3.2) : le bloc avait deux clés "events", la seconde ne servant qu'à mettre
// deux Events dans une phase ; QJsonDocument garde la dernière, tous les Events datés étaient perdus.

// Membres du premier niveau d'un objet JSON, dans l'ordre et doublons compris : (clé, texte de la valeur).
bool topLevelMembers(const QByteArray& s, QVector<QPair<QString, QByteArray>>& out)
{
    qsizetype i = 0;
    const qsizetype n = s.size();
    auto ws = [&] { while (i < n && QChar::isSpace(uchar(s[i]))) ++i; };
    auto skipString = [&]() -> bool {                     // s[i] == '"' ; i après le guillemet fermant
        ++i;
        while (i < n) {
            if (s[i] == '\\') { i += 2; continue; }
            if (s[i] == '"') { ++i; return true; }
            ++i;
        }
        return false;
    };
    ws();
    if (i >= n || s[i] != '{') return false;
    ++i;
    for (;;) {
        ws();
        if (i < n && s[i] == '}') return true;
        if (i >= n || s[i] != '"') return false;
        const qsizetype k0 = i;
        if (!skipString()) return false;
        const QJsonArray ka = QJsonDocument::fromJson("[" + s.mid(k0, i - k0) + "]").array();
        const QString key = ka.isEmpty() ? QString() : ka.at(0).toString();
        ws();
        if (i >= n || s[i] != ':') return false;
        ++i;
        ws();
        const qsizetype v0 = i;
        int depth = 0;
        while (i < n) {
            const char c = s[i];
            if (c == '"') { if (!skipString()) return false; continue; }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (depth == 0) break; --depth; }
            else if (c == ',' && depth == 0) break;
            ++i;
        }
        out.append({ key, s.mid(v0, i - v0).trimmed() });
        if (i >= n) return false;
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == '}') return true;
        return false;
    }
}

// Ajoute à `a` les éléments de `b` qui n'y sont pas déjà.
void appendNew(QJsonArray& a, const QJsonArray& b)
{
    for (const QJsonValue& v : b) if (!a.contains(v)) a.append(v);
}

// Clé de comparaison d'un nom d'Event ou de phase : sans accents, casse ni espaces (« US 3 » = « us3 »).
QString nameKey(const QString& n) { return text::fold(n).remove(QLatin1Char(' ')); }

// Réunit les Events (ou phases) de même nom : champs manquants complétés, listes (dates, phases) jointes ; en cas de
// valeurs différentes, la première est gardée.
QJsonArray mergeByName(const QJsonArray& list, const QString& what, QStringList* notes)
{
    QJsonArray out;
    QHash<QString, int> at;
    for (const QJsonValue& v : list) {
        if (!v.isObject()) { out.append(v); continue; }
        const QJsonObject o = v.toObject();
        const QString key = nameKey(o.value("name").toString());
        if (key.isEmpty() || !at.contains(key)) { if (!key.isEmpty()) at.insert(key, out.size()); out.append(o); continue; }
        QJsonObject m = out.at(at.value(key)).toObject();
        QStringList added, kept;
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() == QLatin1String("name")) continue;
            if (!m.contains(it.key())) { m.insert(it.key(), it.value()); added << it.key(); continue; }
            const QJsonValue old = m.value(it.key());
            if (old == it.value()) continue;
            if (old.isArray() && it.value().isArray()) { QJsonArray a = old.toArray(); appendNew(a, it.value().toArray()); m.insert(it.key(), a); added << it.key(); }
            else kept << it.key();
        }
        out[at.value(key)] = m;
        if (notes) {
            QString n = QStringLiteral("%1 « %2 » écrit deux fois par le modèle d'IA : réuni").arg(what, o.value("name").toString());
            if (!added.isEmpty()) n += QStringLiteral(" (ajouté : %1)").arg(added.join(QStringLiteral(", ")));
            if (!kept.isEmpty()) n += QStringLiteral(" ; valeurs différentes, la première est gardée : %1").arg(kept.join(QStringLiteral(", ")));
            *notes << n + QLatin1Char('.');
        }
    }
    return out;
}

// Objet du bloc : clés en double réunies (listes jointes, objets complétés, sinon première valeur), puis Events et
// phases de même nom réunis.
QJsonObject mergedSpec(const QByteArray& raw, const QJsonObject& parsed, QStringList* notes)
{
    QJsonObject spec = parsed;
    QVector<QPair<QString, QByteArray>> members;
    if (topLevelMembers(raw, members)) {
        QHash<QString, QVector<QJsonValue>> byKey;
        QStringList order;
        for (const auto& m : std::as_const(members)) {
            if (!byKey.contains(m.first)) order << m.first;
            const QJsonArray wrap = QJsonDocument::fromJson("[" + m.second + "]").array();
            byKey[m.first].append(wrap.isEmpty() ? QJsonValue() : wrap.at(0));
        }
        for (const QString& k : std::as_const(order)) {
            const QVector<QJsonValue>& vals = byKey.value(k);
            if (vals.size() < 2) continue;
            QJsonValue merged = vals.first();
            for (int j = 1; j < vals.size(); ++j) {
                if (merged.isArray() && vals[j].isArray()) { QJsonArray a = merged.toArray(); for (const QJsonValue& x : vals[j].toArray()) a.append(x); merged = a; }
                else if (merged.isObject() && vals[j].isObject()) {
                    QJsonObject o = merged.toObject();
                    const QJsonObject b = vals[j].toObject();
                    for (auto it = b.begin(); it != b.end(); ++it) if (!o.contains(it.key())) o.insert(it.key(), it.value());
                    merged = o;
                }
            }
            spec.insert(k, merged);
            if (notes) *notes << QStringLiteral("Le bloc du modèle d'IA contenait %1 fois la clé « %2 » : les %1 ont été réunies "
                                                "(sinon seule la dernière était lue).").arg(vals.size()).arg(k);
        }
    }
    for (const char* k : { "events", "phases" })
        if (spec.value(k).isArray())
            spec.insert(k, mergeByName(spec.value(k).toArray(), k == QLatin1String("events") ? QStringLiteral("L'événement")
                                                                                              : QStringLiteral("La phase"), notes));
    return spec;
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

namespace {
double depthNumber(const QJsonValue& v)
{
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double x = v.toString().trimmed().replace(QChar(0x2212), QLatin1Char('-')).replace(',', '.').toDouble(&ok);
        if (ok) return x;
    }
    return std::nan("");
}
double givenDepth(const QJsonObject& e) { return depthNumber(e.contains("depth") ? e.value("depth") : e.value("profondeur")); }
} // namespace

bool ChrBuilder::signedDepths(const QJsonArray& events)
{
    for (const QJsonValue& v : events) {
        const double d = givenDepth(v.toObject());
        if (!std::isnan(d) && d < 0) return true;
    }
    return false;
}

bool ChrBuilder::signedDepths(const QJsonObject& spec)
{
    const QString sign = text::fold(spec.value("depth_sign").toString()).trimmed();
    if (sign == QLatin1String("as_given") || sign == QLatin1String("signed") || sign == QLatin1String("tel_quel")) return true;
    return signedDepths(spec.value("events").toArray());
}

double ChrBuilder::chronoDepth(const QJsonObject& event, bool signedDepths)
{
    const double d = givenDepth(event);
    if (std::isnan(d)) return d;
    return signedDepths ? d : -d;          // -0 évité : 0 reste 0
}

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
    // profondeur la plus basse (valeur négative permise) est l'Event le plus ancien ; des profondeurs sous la surface
    // données en positif s'écrivent en négatif (10 m -> -10), des profondeurs signées restent telles quelles (voir
    // signedDepths). Elle va dans X_Inc_Depth, son erreur dans sX_Alpha95_sDepth. Des profondeurs sans « curve »
    // suffisent à la demander.
    static const QSet<QString> ageDepthNames = { "age_depth", "age-depth", "agedepth", "age_profondeur", "age-profondeur",
                                                 "age profondeur", "profondeur", "courbe age-profondeur", "depth",
                                                 // « niveau » : pas un type de ChronoModel, c'est la profondeur (Philippe, 12/11/2026)
                                                 "niveau", "niveaux", "age-niveau", "age niveau", "courbe age-niveau", "level" };
    bool anyDepth = false;
    for (const QJsonValue& ev : evSpec) anyDepth = anyDepth || ev.toObject().contains("depth") || ev.toObject().contains("profondeur");
    if (curveName.trimmed().isEmpty() && anyDepth) {
        curveName = QStringLiteral("age_depth");
        R.warnings << QStringLiteral("Profondeurs données sans type de courbe : courbe âge-profondeur utilisée.");
    }
    const bool ageDepth = ageDepthNames.contains(text::fold(curveName).trimmed());
    const bool depthsSigned = signedDepths(spec);
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
                    // Durée de la phase (Phase::TauType de ChronoModel) : 0 inconnue, 1 durée maximale connue (tau_fixed),
                    // 2 « Uniform Span » (eZOnly). Relevé sur test_unif_span.chr (Philippe, 13/11/2026).
                    const PhaseDuration du = phaseDuration(v.toObject());
                    if (!du.error.isEmpty())
                        R.errors << QStringLiteral("Phase « %1 » : %2").arg(n.trimmed(), du.error);
                    else if (du.type == 1) { p["tau_type"] = 1; p["tau_fixed"] = du.max; }
                    else if (du.type == 2) p["tau_type"] = 2;
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
            // Âge en ka, Ma (« "unit":"ka" ») : ChronoModel attend des années.
            if (type == QLatin1String("gauss")) d = MethodRunner::withAgeUnit(d, QStringLiteral("gauss"));
            else if (type == QLatin1String("tl") || type == QLatin1String("osl") || type == QLatin1String("tl/osl")) d = MethodRunner::withAgeUnit(d, QStringLiteral("tl"));
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
                // Courbe de référence du plugin Gauss (fichier de Calib/Gauss, nom avec « .csv » comme ChronoModel) : mode curve.
                QString gc = d.value("curve").toString().trimmed();
                const bool onCurve = d.value("mode").toString() == QLatin1String("curve") || (!gc.isEmpty() && d.value("mode").toString().isEmpty());
                if (onCurve && !gc.isEmpty() && !gc.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive)) gc += QStringLiteral(".csv");
                out["data"] = QJsonObject{ { "age", age }, { "error", err }, { "mode", onCurve && !gc.isEmpty() ? "curve" : "none" },
                                           { "curve", onCurve ? gc.toLower() : QString() }, { "a", 0 }, { "b", 1 }, { "c", 0 } };
                if (onCurve && !gc.isEmpty()) {
                    R.warnings << QStringLiteral("Date gaussienne sur la courbe « %1 » : le fichier doit être dans Calib/Gauss de ChronoModel.").arg(gc);
                    if (const RefCurve* c = CurveStore::get(QStringLiteral("gauss"), gc, CurveFormat::Simple)) addSpan(c->tmin(), c->tmax());
                } else {
                    addSpan(age - 3 * err, age + 3 * err);
                }
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
            const QJsonValue de = e.contains("depth_error") ? e.value("depth_error") : e.value("erreur_profondeur");
            const double depth = chronoDepth(e, depthsSigned), derr = std::abs(depthNumber(de));
            if (std::isnan(depth))
                R.errors << QStringLiteral("Courbe âge-profondeur : l'événement « %1 » n'a pas de profondeur (« depth »).").arg(name);
            else {
                o["X_Inc_Depth"] = depth == 0.0 ? 0.0 : depth;
                if (!std::isnan(derr) && derr > 0) o["sX_Alpha95_sDepth"] = derr;
                else R.warnings << QStringLiteral("Courbe âge-profondeur : pas d'erreur sur la profondeur de « %1 » (0 utilisé).").arg(name);
            }
            o["sampler"] = isBound ? -1 : 1;
        } else if (withCurve) {
            // test_curveIDF.chr (projet I, D, F qui fonctionne) : sampler 1 pour les Events comme pour la borne, et les
            // sept champs de mesure toujours écrits (SDec et sY compris).
            o["sampler"] = kEventSampler;
            const QJsonObject m = curveMeasures(e);
            for (const CurveKey& k : kCurveKeys) o[k.chrKey] = m.value(k.chrKey).toDouble(0.0);
            // Composantes demandées par le type de courbe (table du schéma) ; magnétisme : erreurs α95 et σF.
            const bool magnetic = processType != 85 && processType != 50;
            const bool useX = processType != 68 && processType != 70;
            const bool useY = processType == 50 || processType == 51 || processType == 68 || processType == 83 || processType == 86;
            const bool useZ = processType == 51 || processType == 70 || processType == 75 || processType == 86;
            QStringList missing, missingErr;
            if (useX && !m.contains("X_Inc_Depth")) missing << (magnetic ? QStringLiteral("inc") : QStringLiteral("X_Inc_Depth"));
            if (useY && !m.contains("Y_Dec"))       missing << (magnetic ? QStringLiteral("dec") : QStringLiteral("Y_Dec"));
            if (useZ && !m.contains("Z_Field"))     missing << QStringLiteral("field");
            if (magnetic && (useX || useY) && m.value("sX_Alpha95_sDepth").toDouble(0.0) <= 0) missingErr << QStringLiteral("alpha95");
            if (useZ && m.value("sZ_sF").toDouble(0.0) <= 0) missingErr << QStringLiteral("error_f");
            if (!missing.isEmpty())
                R.errors << QStringLiteral("Courbe « %1 » : l'événement « %2 » n'a pas %3. En mode courbe, chaque Event porte ses mesures "
                                           "sur l'événement (pas dans une date) : \"inc\", \"dec\", \"alpha95\" (degrés), \"field\", "
                                           "\"error_f\" (µT).").arg(curveName, name, missing.join(QStringLiteral(", ")));
            if (!missingErr.isEmpty())
                R.errors << QStringLiteral("Courbe « %1 » : l'événement « %2 » n'a pas d'erreur %3 (une erreur nulle n'est pas possible) : "
                                           "demandez-la si le texte ne la donne pas.").arg(curveName, name, missingErr.join(QStringLiteral(", ")));
            if (!magnetic && useX && m.value("sX_Alpha95_sDepth").toDouble(0.0) <= 0)
                R.warnings << QStringLiteral("Courbe « %1 » : pas d'erreur sur X pour « %2 » (0 utilisé).").arg(curveName, name);
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
        const bool forPhases = &ids == &phaseId;
        QVector<QPair<QString, QString>> pairs;          // (plus ancien, plus récent)
        QHash<QString, Hiatus> hiatusOfPair;             // « ancien\nrécent » -> hiatus (contraintes de phases)
        Hiatus current;                                  // hiatus de l'élément de liste en cours
        auto add = [&](const QString& olderName, const QString& youngerName, const QString& shown) {
            if (current.type != 0) hiatusOfPair.insert(olderName + QLatin1Char('\n') + youngerName, current);
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
            // Hiatus : clés de l'objet, ou objet en dernier élément d'une liste (["A", "avant", "B", {"hiatus": 50}]).
            current = Hiatus();
            QJsonObject hobj = v.isObject() ? v.toObject() : QJsonObject();
            QJsonArray arrIn = v.toArray();
            if (v.isArray() && !arrIn.isEmpty() && arrIn.last().isObject()) { hobj = arrIn.last().toObject(); arrIn.removeLast(); }
            if (!hobj.isEmpty()) {
                current = hiatusOf(hobj);
                if (!current.error.isEmpty()) { R.errors << QStringLiteral("Contrainte %1 : %2").arg(what, current.error); current = Hiatus(); }
                else if (current.type != 0 && !forPhases) {
                    R.warnings << QStringLiteral("Hiatus ignoré sur une contrainte %1 : ChronoModel ne le permet qu'entre des phases.").arg(what);
                    current = Hiatus();
                }
            }
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
            const QJsonArray arr = arrIn;
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
            if (forPhases) {         // champs écrits par ChronoModel (PhaseConstraint::toJson), valeurs par défaut 0, 1, 0, 1
                const Hiatus h = hiatusOfPair.value(pr.first + QLatin1Char('\n') + pr.second);
                c["gamma_type"] = h.type;
                c["gamma_fixed"] = h.fixed;
                c["gamma_min"] = h.min;
                c["gamma_max"] = h.max;
            }
            out.append(c);
        }
        return out;
    };
    QJsonArray evConsSpec = spec.value("event_constraints").toArray();
    // Courbe âge-profondeur sans contrainte donnée : loi de superposition, de la profondeur la plus basse (Event le plus
    // ancien pour ChronoModel) à la plus haute.
    if (ageDepth && evConsSpec.isEmpty()) {
        QVector<QPair<double, QString>> byDepth;
        for (const QJsonValue& v : std::as_const(events)) {
            const QJsonObject o = v.toObject();
            if (o.contains("X_Inc_Depth")) byDepth.append({ o.value("X_Inc_Depth").toDouble(), o.value("name").toString() });
        }
        std::stable_sort(byDepth.begin(), byDepth.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        QJsonArray seq;
        for (const auto& pr : std::as_const(byDepth)) seq.append(pr.second);
        if (seq.size() >= 2) {
            evConsSpec.append(seq);
            R.warnings << QStringLiteral("Courbe âge-profondeur : contraintes ajoutées dans l'ordre des profondeurs (la plus basse est l'Event le plus ancien).");
        }
    }
    const QJsonArray evCons = buildConstraints(evConsSpec, eventId, "d'événements");
    // Courbe âge-profondeur avec contraintes données : chaque contrainte doit suivre les profondeurs (valeur centrale
    // seulement, sans l'erreur de profondeur) : l'Event le plus ancien est le plus profond. Avertissement, pas une erreur
    // (perturbation, remaniement possibles). Une inversion de DATATIONS n'est pas contrôlée ici : ChronoModel la traite.
    if (ageDepth && !spec.value("event_constraints").toArray().isEmpty()) {
        QHash<int, QPair<QString, double>> byId;          // id -> nom, profondeur de ChronoModel (X_Inc_Depth)
        for (const QJsonValue& v : std::as_const(events)) {
            const QJsonObject o = v.toObject();
            if (o.contains("X_Inc_Depth")) byId.insert(o.value("id").toInt(), { o.value("name").toString(), o.value("X_Inc_Depth").toDouble() });
        }
        for (const QJsonValue& v : evCons) {
            const QJsonObject c = v.toObject();
            const int older = c.value(kBwdIsOlder ? "bwd_id" : "fwd_id").toInt(), younger = c.value(kBwdIsOlder ? "fwd_id" : "bwd_id").toInt();
            if (!byId.contains(older) || !byId.contains(younger)) continue;
            const auto& o = byId[older];
            const auto& y = byId[younger];
            if (o.second > y.second)
                R.warnings << QStringLiteral("Courbe âge-profondeur : la contrainte « %1 » plus ancien que « %2 » contredit les profondeurs "
                                             "(%3 au-dessus de %4 dans ChronoModel) : l'Event le plus ancien devrait avoir la profondeur la "
                                             "plus basse. Vérifiez la contrainte ou les profondeurs.")
                                  .arg(o.first, y.first, QString::number(o.second), QString::number(y.second));
        }
    }
    const QJsonArray phCons = buildConstraints(spec.value("phase_constraints").toArray(), phaseId, "de phases");

    // Événements en diagramme stratigraphique (le plus récent en haut).
    layoutEvents(events, evCons);

    // Phases : même disposition que les événements (la plus récente en haut), d'après les contraintes de phases.
    layoutEvents(phases, phCons);

    // ---------- Paramètres ----------
    // Période et pas écrits par le modèle d'IA à la racine du bloc (prompt réel du 13/11/2026 : "tmin", "tmax", "step"
    // hors de "settings") : repris s'ils manquent dans "settings".
    QJsonObject sIn = spec.value("settings").toObject();
    for (const char* k : { "tmin", "tmax", "step" })
        if (!sIn.contains(k) && spec.contains(k)) sIn[k] = spec.value(k);
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
    // Réglages MCMC écrits dans "settings" (prompt réel Teilleul 2 du 13/11/2026) : repris s'il n'y a pas de "mcmc" à la racine.
    const QJsonObject mIn = spec.contains("mcmc") ? spec.value("mcmc").toObject() : spec.value("settings").toObject().value("mcmc").toObject();
    for (auto it = mIn.begin(); it != mIn.end(); ++it)
        if (mcmc.contains(it.key())) mcmc[it.key()] = it.value();

    // Sans courbe (process_type 78), ChronoModel écrit alpha 0, speed_threshold 0, use_err_mesure vrai, variance_fixed 1.
    // Avec une courbe : réglages de test_curveIDF.chr (projet I, D, F qui fonctionne, 12/11/2026) : lissage bayésien,
    // variance individuelle bayésienne, erreurs de mesure utilisées, alpha 0, speed_threshold 0, variance_fixed 0.
    QJsonObject curve{
        { "alpha", 0.0 }, { "coeff_lissage_type", 66 }, { "process_type", processType },
        { "speed_threshold", 0.0 }, { "time_type", 66 }, { "use_err_mesure", true },
        { "variance_fixed", withCurve ? 0.0 : 1.0 }, { "variance_type", 66 }
    };
    if (!withCurve) curve["variance_individual"] = true;      // présent sans courbe (test_unif_span.chr, 13/11/2026)
    if (ageDepth)            // réglages de teilleulc2.chr (variance globale « G », erreurs de mesure utilisées)
        curve = QJsonObject{ { "alpha", 0.0 }, { "coeff_lissage_type", 66 }, { "process_type", 80 }, { "speed_threshold", 0.0 },
                             { "time_type", 66 }, { "use_err_mesure", true }, { "variance_fixed", 1.0 }, { "variance_type", 71 } };

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

// Noms techniques (champs du bloc chronomodel, réglages MCMC, outils de calcul) remplacés par des mots courants dans le
// texte montré à l'utilisateur (12/11/2026, Philippe : « event_constraints » recopié d'une fiche dans une réponse).
QString ChrBuilder::humanizeNames(const QString& md)
{
    static const QVector<QPair<QString, QString>> names = {
        { "event_constraints", "ordre entre les événements" }, { "events_constraints", "ordre entre les événements" },
        { "phase_constraints", "ordre entre les phases" }, { "phases_constraints", "ordre entre les phases" },
        { "known_fixed", "année de la borne" }, { "ref_year", "année de mesure" }, { "delta_r_error", "erreur sur ΔR" },
        { "delta_r", "ΔR" }, { "alpha95", "α95" }, { "error_f", "erreur sur l'intensité" }, { "depth_error", "erreur de profondeur" },
        { "num_iter", "nombre d'itérations" }, { "num_burn", "itérations de chauffe" }, { "num_proc", "nombre de chaînes" },
        { "thinning_interval", "intervalle d'échantillonnage" }, { "anneal_recurrence", "recuit" }, { "max_adapt", "adaptation" },
        { "tmin", "début de la période d'étude" }, { "tmax", "fin de la période d'étude" }, { "wiggle", "décalage" },
        { "calibrer_radiocarbone", "calibration radiocarbone" }, { "calibrer_f14c", "calibration F14C" },
        { "dater_luminescence", "datation par luminescence" }, { "densite_gauss", "date gaussienne" },
        { "densite_uniforme", "date uniforme" }, { "densite_tabulee", "densité tabulée" },
        { "dater_archeomagnetisme", "datation archéomagnétique" }, { "calculer_evenement", "calcul d'un événement" },
        { "combiner_dates", "combinaison de dates" }, { "comparer_dates", "comparaison de dates" },
        { "analyser_traces", "calcul sur les traces" }, { "courbe_chronomodel", "calcul sur la courbe" },
        { "calibration_inverse", "calibration inverse" }, { "creer_densite", "création d'une densité" },
        { "courbes_disponibles", "liste des courbes" } };
    static const QVector<QPair<QRegularExpression, QString>> res = [] {
        QVector<QPair<QRegularExpression, QString>> v;
        for (const auto& n : names)    // « `event_constraints` », « "event_constraints" », event_constraints
            v.append({ QRegularExpression(QStringLiteral("[`\"]?\\b%1\\b[`\"]?").arg(QRegularExpression::escape(n.first))), n.second });
        // « bloc calcul », « bloc « chronomodel » »
        v.append({ QRegularExpression(QStringLiteral("\\bbloc (?:de code )?(?:«\\s*|`|\")?calcul(?:\\s*»|`|\")?"), QRegularExpression::CaseInsensitiveOption),
                   QStringLiteral("demande de calcul") });
        v.append({ QRegularExpression(QStringLiteral("\\bbloc (?:de code )?(?:«\\s*|`|\")?chronomodel(?:\\s*»|`|\")?"), QRegularExpression::CaseInsensitiveOption),
                   QStringLiteral("modèle") });
        return v;
    }();
    // Les blocs de code (```…```) restent tels quels.
    static const QRegularExpression fence(QStringLiteral("^[ \\t]*```.*?^[ \\t]*```[^\\n]*$"),
                                          QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption);
    QString out;
    qsizetype pos = 0;
    auto fix = [&](QString t) { for (const auto& r : res) t.replace(r.first, r.second); return t; };
    for (auto it = fence.globalMatch(md); it.hasNext();) {
        const auto m = it.next();
        out += fix(md.mid(pos, m.capturedStart() - pos)) + m.captured(0);
        pos = m.capturedEnd();
    }
    return out + fix(md.mid(pos));
}

QString ChrBuilder::displayMarkdown(const QString& md, bool showJson)
{
    static const QRegularExpression block("^[ \\t]*```[ \\t]*(chronomodel|json)[ \\t]*\\n(.*?)^[ \\t]*```[ \\t]*$",
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
    static const QRegularExpression open("^[ \\t]*```[ \\t]*chronomodel[ \\t]*$",
                                         QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    if (!showJson)
        if (const auto mo = open.match(rest); mo.hasMatch())
            rest = rest.left(mo.capturedStart()) + QStringLiteral("*📦 Préparation du modèle ChronoModel…*");
    return out + rest;
}

QString ChrBuilder::stripModelBlocks(const QString& md)
{
    static const QRegularExpression block("^[ \\t]*```[ \\t]*(chronomodel|json)[ \\t]*\\n(.*?)^[ \\t]*```[ \\t]*$\\n?",
                                          QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption |
                                          QRegularExpression::CaseInsensitiveOption);
    // Ligne d'annonce du bloc, juste avant lui : « **Modèle ChronoModel** (à exporter) : ».
    static const QRegularExpression intro(QStringLiteral(R"((?:^|\n)[^\n]*mod[eè]le[^\n]*:[ \t*_]*\n\s*$)"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression rule(QStringLiteral(R"((?:\n\s*-{3,}\s*)+$)"));
    QString out;
    qsizetype pos = 0;
    auto it = block.globalMatch(md);
    while (it.hasNext()) {
        const auto m = it.next();
        const bool model = m.captured(1).compare(QLatin1String("chronomodel"), Qt::CaseInsensitive) == 0
                        || QJsonDocument::fromJson(m.captured(2).toUtf8()).object().contains("events");
        if (!model) continue;
        QString before = md.mid(pos, m.capturedStart() - pos);
        before.remove(intro);
        out += before;
        pos = m.capturedEnd();
    }
    if (pos == 0) {
        // Bloc encore ouvert (réponse en cours d'écriture) : rien ne s'affiche à partir de lui.
        static const QRegularExpression open("^[ \\t]*```[ \\t]*chronomodel[ \\t]*$",
                                             QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
        const auto mo = open.match(md);
        if (!mo.hasMatch()) return md;
        QString before = md.left(mo.capturedStart());
        before.remove(intro);
        return before.remove(rule).trimmed();
    }
    out += md.mid(pos);
    return out.trimmed().remove(rule).trimmed();
}

bool ChrBuilder::asksForJson(const QString& question)
{
    static const QRegularExpression re(QStringLiteral("\\bjson\\b|bloc (de code )?chronomodel|specification (json |du modele|de modele)|"
                                                      "code (source )?du modele|structure (json|du (fichier|modele))"));
    return re.match(foldText(question)).hasMatch();
}

bool ChrBuilder::extractSpec(const QString& md, QJsonObject& spec, QString* err, QStringList* notes)
{
    // Les clôtures ``` doivent être en début de ligne (une mention « ```chronomodel » au milieu d'une
    // phrase ne doit pas être prise pour un bloc).
    static const QRegularExpression re("^[ \\t]*```[ \\t]*(chronomodel|json)[ \\t]*\\n(.*?)^[ \\t]*```",
                                       QRegularExpression::DotMatchesEverythingOption |
                                       QRegularExpression::MultilineOption |
                                       QRegularExpression::CaseInsensitiveOption);
    auto it = re.globalMatch(md);
    QString lastError;
    while (it.hasNext()) {
        const auto m = it.next();
        QJsonParseError pe;
        const QByteArray raw = m.captured(2).toUtf8();
        const QJsonDocument d = QJsonDocument::fromJson(raw, &pe);
        if (pe.error != QJsonParseError::NoError) {
            lastError = QStringLiteral("JSON invalide : %1 (position %2)").arg(pe.errorString()).arg(pe.offset);
            continue;
        }
        if (d.isObject() && d.object().contains("events")) {
            spec = mergedSpec(raw, d.object(), notes);
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
    // Durées de phases et hiatus : nombres pris au texte, comme les dates.
    for (const QJsonValue& pv : spec.value("phases").toArray()) {
        const PhaseDuration du = phaseDuration(pv.toObject());
        if (du.type == 1 && !known(du.max))
            unknownValues << QStringLiteral("%1 (durée de la phase « %2 »)").arg(fmt(du.max), pv.toObject().value("name").toString());
    }
    for (const QJsonValue& cv : spec.value("phase_constraints").toArray()) {
        const QJsonObject ho = cv.isObject() ? cv.toObject() : (cv.toArray().isEmpty() ? QJsonObject() : cv.toArray().last().toObject());
        const Hiatus h = hiatusOf(ho);
        for (double v : h.type == 1 ? QVector<double>{ h.fixed } : h.type == 2 ? QVector<double>{ h.min, h.max } : QVector<double>{})
            if (v != 0.0 && !known(v)) unknownValues << QStringLiteral("%1 (hiatus entre phases)").arg(fmt(v));
    }
    for (const QJsonValue& ev : spec.value("events").toArray()) {
        const QJsonObject e = ev.toObject();
        const QString en = e.value("name").toString();
        if (e.contains("known_fixed") && !known(e.value("known_fixed").toDouble()))
            unknownValues << QStringLiteral("%1 (borne « %2 »)").arg(fmt(e.value("known_fixed").toDouble()), en);
        for (const char* k : { "depth", "depth_error" })
            if (e.value(k).isDouble() && !known(e.value(k).toDouble()))
                unknownValues << QStringLiteral("%1 (%2 de « %3 »)").arg(fmt(e.value(k).toDouble()), QString::fromLatin1(k), en);
        // Mesures de courbe sur l'événement (I, D, F, α95, σF) : contrôlées comme les valeurs des dates.
        const QJsonObject cm = curveMeasures(e);
        for (auto it = cm.begin(); it != cm.end(); ++it)
            if (it.value().toDouble() != 0.0 && !known(it.value().toDouble()))
                unknownValues << QStringLiteral("%1 (%2 de « %3 »)").arg(fmt(it.value().toDouble()), it.key(), en);
        if (cm.contains("Z_Field") && cm.contains("sZ_sF"))
            specPairs << Pair{ cm.value("Z_Field").toDouble(), cm.value("sZ_sF").toDouble() };
        for (const QJsonValue& dv : eventDates(e)) {
            const QJsonObject d = dv.toObject();
            for (const char* k : valueKeys)
                if (d.value(k).isDouble() && !known(d.value(k).toDouble()))
                    unknownValues << QStringLiteral("%1 (%2 d'une date de « %3 »)").arg(fmt(d.value(k).toDouble()), QString::fromLatin1(k), en);
            const QString dn = d.value("name").toString().trimmed();
            if (!dn.isEmpty()) {
                specNames << text::fold(dn);
                if (!src.contains(text::fold(dn)))
                    out << QStringLiteral("Référence « %1 » (événement « %2 ») absente de votre texte : interprétée par le modèle ?").arg(dn, en);
            }
            const double v = d.value("age").toDouble(d.value("f14c").toDouble(d.value("field").toDouble(std::nan(""))));
            const double er = d.value("error").toDouble(d.value("error_f").toDouble(std::nan("")));
            if (std::isfinite(v) && std::isfinite(er)) specPairs << Pair{ v, er };
        }
    }
    // Même référence de laboratoire sur deux dates (prompt réel du 13/11/2026 : « Ly-12345 », donnée pour US6, recopiée
    // par le modèle d'IA sur une date de US10).
    {
        QHash<QString, QStringList> owners;
        QHash<QString, QString> shown;
        for (const QJsonValue& ev : spec.value("events").toArray())
            for (const QJsonValue& dv : eventDates(ev.toObject())) {
                const QString dn = dv.toObject().value("name").toString().trimmed();
                if (dn.isEmpty()) continue;
                owners[text::fold(dn)] << ev.toObject().value("name").toString();
                shown.insert(text::fold(dn), dn);
            }
        for (auto it = owners.cbegin(); it != owners.cend(); ++it)
            if (it.value().size() > 1 && src.count(it.key()) < it.value().size())
                out << QStringLiteral("Référence « %1 » donnée à %2 dates (%3) alors que votre texte la cite %4 fois : "
                                      "mal attribuée par le modèle d'IA ?")
                           .arg(shown.value(it.key())).arg(it.value().size()).arg(it.value().join(QStringLiteral(", ")))
                           .arg(src.count(it.key()));
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

QString ChrBuilder::durationText(const QJsonObject& phase)
{
    switch (phase.value("tau_type").toInt(0)) {
    case 1:  return QStringLiteral("Durée ≤ %1").arg(QString::number(phase.value("tau_fixed").toDouble(), 'g', 10));
    case 2:  return QStringLiteral("Span uniforme");
    default: return QString();
    }
}

QString ChrBuilder::hiatusText(const QJsonObject& c)
{
    auto n = [](double v) { return QString::number(v, 'g', 10); };
    switch (c.value("gamma_type").toInt(0)) {
    case 1:  return QStringLiteral("hiatus ≥ %1").arg(n(c.value("gamma_fixed").toDouble()));
    case 2:  return QStringLiteral("hiatus min ∈ [%1 ; %2]").arg(n(c.value("gamma_min").toDouble()), n(c.value("gamma_max").toDouble()));
    default: return QString();
    }
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
    QStringList pn;
    for (const QJsonValue& p : phases) {
        const QString du = durationText(p.toObject());
        pn << p.toObject().value("name").toString() + (du.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(du));
    }
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
            // Nom de la date (référence de l'échantillon) : à reprendre dans « name », jamais remplacé par celui de l'événement.
            if (const QString dn = d.value("name").toString().trimmed(); !dn.isEmpty()) ds.last() = QStringLiteral("« %1 » ").arg(dn) + ds.last();
        }
        if (!ds.isEmpty()) s += " : " + ds.join("; ");
        // Mesures de courbe I, D, F (modes archéomagnétiques, sauf âge-profondeur qui a sa propre lecture).
        const int cpt = chr.value("curve").toObject().value("process_type").toInt(78);
        if (cpt == 86 || cpt == 83 || cpt == 75 || cpt == 73 || cpt == 68 || cpt == 70 || cpt == 51) {
            QStringList mv;
            if (cpt != 68 && cpt != 70) mv << QStringLiteral("I=%1").arg(e.value("X_Inc_Depth").toDouble());
            if (cpt == 86 || cpt == 83 || cpt == 68 || cpt == 51) mv << QStringLiteral("D=%1").arg(e.value("Y_Dec").toDouble());
            if (cpt != 70) mv << QStringLiteral("α95=%1").arg(e.value("sX_Alpha95_sDepth").toDouble());
            if (cpt == 86 || cpt == 75 || cpt == 70 || cpt == 51)
                mv << QStringLiteral("F=%1±%2").arg(e.value("Z_Field").toDouble()).arg(e.value("sZ_sF").toDouble());
            s += QStringLiteral(" | mesure %1").arg(mv.join(", "));
        }
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
            const QString hi = hiatusText(c);
            l << QStringLiteral("%1 → %2").arg(names.value(older, QString::number(older)), names.value(younger, QString::number(younger)))
                     + (hi.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(hi));
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


// ---------- Mode courbe, étape 2 : mesures de chaque Event lues par SeldonH (16/11/2026) ----------
namespace {

struct MeasurePattern {
    QChar comp;                 // I, D, F, X, Y, Z ; 'a' = alpha95, 'e' = erreur de F
    QRegularExpression re;      // valeur en 1, erreur éventuelle en 2
};

const QVector<MeasurePattern>& measurePatterns()
{
    static const QString num = QStringLiteral(R"((-?\d+(?:[.,]\d+)?))");
    static const QString pm = QStringLiteral(R"((?:\s*(?:±|\+\s*/?\s*-)\s*(\d+(?:[.,]\d+)?))?)");
    static const QString eq = QStringLiteral(R"(\s*[=:]\s*)");
    static const QVector<MeasurePattern> v{
        { 'I', QRegularExpression(QStringLiteral(R"((?:\bI|(?i:\binclinaisons?|\binclinations?|\binc))(?![\w]))") + eq + num) },
        { 'D', QRegularExpression(QStringLiteral(R"((?:\bD|(?i:\bd[ée]clinaisons?|\bdeclinations?|\bdec))(?![\w]))") + eq + num) },
        { 'a', QRegularExpression(QStringLiteral(R"((?i:\balpha\s*-?\s*95|α\s*95|\ba95))") + eq + num) },
        { 'F', QRegularExpression(QStringLiteral(R"((?:\bF|(?i:\bintensit[ée]s?|\bintensity|\bfield|\bpal[ée]ointensit[ée]s?))(?![\w]))") + eq + num + pm) },
        { 'e', QRegularExpression(QStringLiteral(R"((?i:σ\s*F|\bsigma_?f\b|\berror_f\b|\berreur\s+(?:sur\s+)?(?:F|l'intensit[ée])))") + eq + num) },
        { 'X', QRegularExpression(QStringLiteral(R"(\bX(?![\w])(?:\s*\([^)]*\))?)") + eq + num + pm) },
        { 'Y', QRegularExpression(QStringLiteral(R"(\bY(?![\w])(?:\s*\([^)]*\))?)") + eq + num + pm) },
        { 'Z', QRegularExpression(QStringLiteral(R"(\bZ(?![\w])(?:\s*\([^)]*\))?)") + eq + num + pm) },
    };
    return v;
}

bool magneticComponents(const QString& comps) { return comps.contains('I') || comps.contains('D') || comps.contains('F'); }

// Valeurs lues dans un passage : composante -> (valeur, erreur ou NaN).
QHash<QChar, QPair<double, double>> measuresIn(const QString& chunk, const QString& comps)
{
    QHash<QChar, QPair<double, double>> out;
    const bool mag = magneticComponents(comps);
    for (const MeasurePattern& p : measurePatterns()) {
        const bool wanted = mag ? (comps.contains(p.comp) || (p.comp == 'a' && (comps.contains('I') || comps.contains('D')))
                                   || (p.comp == 'e' && comps.contains('F')))
                                : comps.contains(p.comp);
        if (!wanted) continue;
        const auto m = p.re.match(chunk);
        if (!m.hasMatch()) continue;
        const double v = text::toNumber(m.captured(1));
        const double e = m.lastCapturedIndex() >= 2 && !m.captured(2).isEmpty() ? text::toNumber(m.captured(2)) : std::nan("");
        if (std::isfinite(v)) out.insert(p.comp, { v, e });
    }
    return out;
}

QString normalizedText(QString t)
{
    t.replace(QChar(0x2212), QLatin1Char('-'));            // signe moins typographique
    t.replace(QChar(0x00A0), QLatin1Char(' '));
    t.replace(QChar(0x202F), QLatin1Char(' '));
    return t;
}

// Nom d'Event sous une forme proche : casse, espaces, tirets et zéros de tête ignorés (« PI 2 », « pi-02 » pour « PI02 »).
QRegularExpression looseName(const QString& name)
{
    static const QRegularExpression runs(QStringLiteral(R"(\d+|[^\W\d_]+)"));
    QStringList parts;
    for (auto it = runs.globalMatch(name); it.hasNext();) {
        const QString r = it.next().captured(0);
        if (r.at(0).isDigit()) {
            QString d = r;
            while (d.size() > 1 && d.startsWith(QLatin1Char('0'))) d.remove(0, 1);
            parts << QStringLiteral("0*") + d;
        } else {
            parts << QRegularExpression::escape(r);
        }
    }
    if (parts.isEmpty()) return QRegularExpression();
    return QRegularExpression(QStringLiteral("(?<![A-Za-z0-9])%1(?![A-Za-z0-9])").arg(parts.join(QStringLiteral(R"([\s_.\-]*)"))),
                              QRegularExpression::CaseInsensitiveOption);
}

// Composante d'une colonne de tableau d'après son en-tête ; 0 si ce n'en est pas une. 'N' : colonne des noms.
QChar columnComponent(QString h, bool mag)
{
    h = text::fold(h).trimmed();
    h.remove(QRegularExpression(QStringLiteral(R"(\s*\([^)]*\)\s*$|\s*\[[^\]]*\]\s*$|[°*`])")));
    h = h.trimmed();
    static const QStringList names{ "nom", "name", "event", "evenement", "site", "coulee", "echantillon", "sample", "id", "code", "ref",
                                    "reference", "us", "niveau" };
    if (names.contains(h)) return 'N';
    if (mag) {
        static const QStringList I{ "i", "inc", "inclinaison", "inclination" }, D{ "d", "dec", "declinaison", "declination" },
            A{ "alpha95", "a95", "α95", "alpha 95" }, F{ "f", "field", "intensite", "intensity", "paleointensite" },
            E{ "sf", "σf", "error_f", "erreur f", "erreur_f", "sigma f", "sigma_f", "±f", "+-f", "err f", "ef" };
        if (I.contains(h)) return 'I';
        if (D.contains(h)) return 'D';
        if (A.contains(h)) return 'a';
        if (F.contains(h)) return 'F';
        if (E.contains(h)) return 'e';
    } else {
        for (const QChar c : { QChar('X'), QChar('Y'), QChar('Z') }) {
            const QString l = QString(c).toLower();
            if (h == l) return c;
            if (h == QStringLiteral("s") + l || h == QStringLiteral("σ") + l || h == QStringLiteral("err ") + l || h == QStringLiteral("erreur ") + l
                || h == QStringLiteral("error_") + l || h == QStringLiteral("sigma ") + l)
                return QChar(ushort('p' + (c.unicode() - 'X')));   // erreur de X, Y, Z : 'p', 'q', 'r'
        }
    }
    return QChar();
}

QStringList splitCells(const QString& line)
{
    QString l = line.trimmed();
    if (l.startsWith(QLatin1Char('|'))) l.remove(0, 1);
    if (l.endsWith(QLatin1Char('|'))) l.chop(1);
    static const QRegularExpression sep(QStringLiteral(R"(\s*[;\t|]\s*|\s{2,})"));
    QStringList c = l.split(sep);
    for (QString& x : c) x = x.trimmed();
    return c;
}

// Mesures d'un tableau du texte : index d'Event -> composante -> (valeur, erreur).
QHash<int, QHash<QChar, QPair<double, double>>> tableMeasures(const QString& text, const QStringList& eventNames, const QString& comps)
{
    QHash<int, QHash<QChar, QPair<double, double>>> out;
    const bool mag = magneticComponents(comps);
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int h = 0; h < lines.size(); ++h) {
        const QStringList head = splitCells(lines[h]);
        if (head.size() < 2) continue;
        QVector<QChar> kind;
        int nameCol = -1, wanted = 0;
        for (int c = 0; c < head.size(); ++c) {
            const QChar k = columnComponent(head[c], mag);
            kind << k;
            if (k == 'N' && nameCol < 0) nameCol = c;
            else if (!k.isNull() && k != 'N') ++wanted;
        }
        if (wanted == 0) continue;
        if (nameCol < 0) nameCol = 0;
        for (int r = h + 1; r < lines.size(); ++r) {
            if (lines[r].trimmed().isEmpty()) break;
            if (QRegularExpression(QStringLiteral(R"(^[\s|:\-+]+$)")).match(lines[r]).hasMatch()) continue;   // |---|---|
            const QStringList cells = splitCells(lines[r]);
            if (cells.size() <= nameCol) break;
            const QString nm = cells[nameCol];
            int ev = eventNames.indexOf(nm);
            for (int i = 0; ev < 0 && i < eventNames.size(); ++i) {
                const QRegularExpression lre = looseName(eventNames[i]);
                const auto m = lre.match(nm);
                if (m.hasMatch() && m.capturedLength() == nm.size()) ev = i;
            }
            if (ev < 0) continue;
            QHash<QChar, QPair<double, double>>& got = out[ev];
            for (int c = 0; c < cells.size() && c < kind.size(); ++c) {
                const QChar k = kind[c];
                if (k.isNull() || k == 'N') continue;
                static const QRegularExpression vpm(QStringLiteral(R"(^(-?\d+(?:[.,]\d+)?)(?:\s*(?:±|\+\s*/?\s*-)\s*(\d+(?:[.,]\d+)?))?)"));
                QString cell = cells[c];
                cell.replace(QChar(0x2212), QLatin1Char('-'));
                const auto m = vpm.match(cell);
                if (!m.hasMatch()) continue;
                const double v = text::toNumber(m.captured(1));
                const double e = m.captured(2).isEmpty() ? std::nan("") : text::toNumber(m.captured(2));
                if (!std::isfinite(v)) continue;
                const ushort u = k.unicode();
                if (u >= 'p' && u <= 'r') {                     // colonne d'erreur de X, Y, Z
                    const QChar comp(ushort(u - 'p' + 'X'));
                    if (got.contains(comp)) got[comp].second = v;
                    else got.insert(comp, { std::nan(""), v });
                } else {
                    const double keepErr = got.contains(k) ? got.value(k).second : std::nan("");
                    got[k] = { v, std::isfinite(e) ? e : keepErr };
                }
            }
        }
    }
    return out;
}

} // namespace

QString ChrBuilder::curveComponents(const QString& curveType)
{
    static const QHash<QString, QString> c{ { "vector", "IDF" }, { "spherical", "ID" }, { "unknown_dec", "IF" },
                                            { "inclination", "I" }, { "declination", "D" }, { "intensity", "F" },
                                            { "univariate", "X" }, { "bivariate", "XY" }, { "trivariate", "XYZ" } };
    return c.value(curveType.trimmed().toLower());
}

bool ChrBuilder::hasCurveMeasures(const QString& text, const QString& curveType)
{
    const QString comps = curveComponents(curveType);
    if (comps.isEmpty()) return false;
    const QString t = normalizedText(text);
    for (const QString& line : t.split(QLatin1Char('\n')))
        if (!measuresIn(line, comps).isEmpty()) return true;
    // Tableau : un en-tête avec une colonne de mesure et au moins une ligne en dessous.
    const bool mag = magneticComponents(comps);
    const QStringList lines = t.split(QLatin1Char('\n'));
    for (int h = 0; h + 1 < lines.size(); ++h) {
        const QStringList head = splitCells(lines[h]);
        if (head.size() < 2) continue;
        int wanted = 0;
        for (const QString& c : head) { const QChar k = columnComponent(c, mag); if (!k.isNull() && k != 'N') ++wanted; }
        if (wanted > 0 && splitCells(lines[h + 1]).size() >= 2) return true;
    }
    return false;
}

QJsonObject ChrBuilder::fillCurveMeasures(QJsonObject spec, const QString& text, const QString& curveType, QStringList* notes,
                                          QStringList* missingEvents)
{
    const QString comps = curveComponents(curveType);
    if (comps.isEmpty()) return spec;                       // âge-profondeur ou type inconnu : rien à faire ici
    if (spec.value("curve").toString().trimmed().isEmpty()) spec["curve"] = curveType;
    const bool mag = magneticComponents(comps);
    const QString t = normalizedText(text);

    QJsonArray events = spec.value("events").toArray();
    // Occurrences des noms d'Events dans le texte (nom entier, sensible à la casse).
    struct Occ { qsizetype pos, len; int ev; };
    QVector<Occ> occ;
    for (int i = 0; i < events.size(); ++i) {
        const QString name = events[i].toObject().value("name").toString().trimmed();
        if (name.isEmpty()) continue;
        const QRegularExpression re(QStringLiteral("(?<![\\w-])%1(?![\\w-])").arg(QRegularExpression::escape(name)));
        for (auto it = re.globalMatch(t); it.hasNext();) {
            const auto m = it.next();
            occ.append({ m.capturedStart(), m.capturedLength(), i });
        }
    }
    std::sort(occ.begin(), occ.end(), [](const Occ& a, const Occ& b) { return a.pos < b.pos; });
    QStringList eventNames;
    for (const QJsonValue& v : std::as_const(events)) eventNames << v.toObject().value("name").toString().trimmed();
    const auto fromTable = tableMeasures(t, eventNames, comps);
    static const QRegularExpression blank(QStringLiteral(R"(\n\s*\n)"));

    struct Key { QChar comp; const char* value; const char* error; };
    const QVector<Key> keys = mag ? QVector<Key>{ { 'I', "X_Inc_Depth", nullptr }, { 'D', "Y_Dec", nullptr }, { 'F', "Z_Field", "sZ_sF" },
                                                  { 'a', "sX_Alpha95_sDepth", nullptr }, { 'e', "sZ_sF", nullptr } }
                                  : QVector<Key>{ { 'X', "X_Inc_Depth", "sX_Alpha95_sDepth" }, { 'Y', "Y_Dec", "sY" }, { 'Z', "Z_Field", "sZ_sF" } };
    static const char* const aliases[] = { "inc", "inclinaison", "inclination", "I", "dec", "declinaison", "déclinaison", "declination",
                                           "D", "field", "intensite", "intensité", "intensity", "F", "alpha95", "a95", "error_f", "erreur_f",
                                           "sF", "sigma_f", "x", "y", "z", "X", "Y", "Z", "error_x", "error_y", "error_z" };
    int filled = 0, removedAm = 0;
    QStringList changed, incomplete;
    for (int i = 0; i < events.size(); ++i) {
        QJsonObject e = events[i].toObject();
        const QString name = e.value("name").toString().trimmed();
        // Mesures du texte : premier passage de cet Event (nom → nom d'un autre Event ou ligne vide) qui en contient.
        QHash<QChar, QPair<double, double>> found = fromTable.value(i);
        for (auto it = found.begin(); it != found.end();) it = std::isfinite(it->first) ? std::next(it) : found.erase(it);
        for (int k = 0; k < occ.size() && found.isEmpty(); ++k) {
            if (occ[k].ev != i) continue;
            const qsizetype from = occ[k].pos + occ[k].len;
            qsizetype to = t.size();
            for (int j = k + 1; j < occ.size(); ++j) if (occ[j].ev != i && occ[j].pos >= from) { to = occ[j].pos; break; }
            const auto bm = blank.match(t, from);
            if (bm.hasMatch() && bm.capturedStart() < to) to = bm.capturedStart();
            found = measuresIn(t.mid(from, to - from), comps);
        }
        // Nom écrit autrement dans le texte (« PI 2 », « pi-02 ») : passage jusqu'à la fin de la ligne.
        if (found.isEmpty() && !name.isEmpty()) {
            const QRegularExpression lre = looseName(name);
            for (auto it = lre.globalMatch(t); it.hasNext() && found.isEmpty();) {
                const auto m = it.next();
                if (eventNames.contains(m.captured(0)) && m.captured(0) != name) continue;   // c'est un autre Event
                const qsizetype eol = t.indexOf(QLatin1Char('\n'), m.capturedEnd());
                found = measuresIn(t.mid(m.capturedEnd(), (eol < 0 ? t.size() : eol) - m.capturedEnd()), comps);
            }
        }
        const QJsonObject before = curveMeasures(e);
        if (!found.isEmpty()) {
            for (const char* a : aliases) e.remove(QLatin1String(a));
            for (const Key& k : keys) {
                if (!found.contains(k.comp)) continue;
                const auto [v, err] = found.value(k.comp);
                if (before.contains(k.value) && std::abs(before.value(k.value).toDouble() - v) > 1e-9)
                    changed << QStringLiteral("« %1 » %2 : %3 → %4").arg(name, QString::fromLatin1(k.value),
                                                                           QString::number(before.value(k.value).toDouble()), QString::number(v));
                e[k.value] = v;
                if (k.error && std::isfinite(err)) e[k.error] = err;
            }
            // Valeurs écrites par le modèle pour des composantes absentes du texte : gardées (signalées par crossCheck).
            for (auto it = before.begin(); it != before.end(); ++it) if (!e.contains(it.key())) e[it.key()] = it.value();
            ++filled;
        } else {
            // Rien dans le texte : les valeurs du modèle (s'il y en a) restent, sous les clés de ChronoModel.
            for (const char* a : aliases) e.remove(QLatin1String(a));
            for (auto it = before.begin(); it != before.end(); ++it) e[it.key()] = it.value();
        }
        // Composantes demandées par le type de courbe et encore absentes.
        QStringList miss;
        const QJsonObject now = curveMeasures(e);
        for (const Key& k : keys) {
            if (k.comp == 'a' || k.comp == 'e') continue;
            if (comps.contains(k.comp) && !now.contains(k.value))
                miss << (mag ? (k.comp == 'I' ? QStringLiteral("inclinaison") : k.comp == 'D' ? QStringLiteral("déclinaison") : QStringLiteral("intensité"))
                             : QString(k.comp));
        }
        if (mag && (comps.contains('I') || comps.contains('D')) && now.value("sX_Alpha95_sDepth").toDouble(0) <= 0) miss << QStringLiteral("α95");
        if (comps.contains(mag ? 'F' : 'Z') && now.value("sZ_sF").toDouble(0) <= 0 && (mag || comps.contains('Z')))
            miss << (mag ? QStringLiteral("erreur sur F") : QStringLiteral("erreur sur Z"));
        if (!miss.isEmpty() && e.value("type").toString() != QLatin1String("bound")) {
            incomplete << QStringLiteral("« %1 » : %2").arg(name, miss.join(QStringLiteral(", ")));
            if (missingEvents) *missingEvents << name;
        }
        // Archéomagnétisme : une date « am » n'est pas une datation de l'Event, c'est sa mesure.
        if (mag) {
            QJsonArray dates, kept;
            dates = e.value("dates").toArray();
            for (const QJsonValue& d : std::as_const(dates)) {
                const QJsonObject o = d.toObject();
                const QString ty = text::fold(o.value("type").toString(o.value("method").toString(o.value("plugin_id").toString()))).trimmed();
                if (ty == QLatin1String("am") || ty == QLatin1String("archeomagnetisme") || ty == QLatin1String("archeomag")) { ++removedAm; continue; }
                kept.append(d);
            }
            if (kept.size() != dates.size()) e["dates"] = kept;
        }
        events[i] = e;
    }
    spec["events"] = events;
    if (notes) {
        if (filled) *notes << QStringLiteral("Mesures de la courbe recopiées du texte par SeldonH sur %1 Event(s).").arg(filled);
        if (!changed.isEmpty()) *notes << QStringLiteral("Mesures du modèle d'IA remplacées par celles du texte : %1.").arg(changed.join(QStringLiteral(" ; ")));
        if (removedAm) *notes << QStringLiteral("%1 date(s) « am » retirée(s) : en mode courbe, la mesure archéomagnétique est portée par l'Event, ce n'est pas une datation.").arg(removedAm);
        if (!incomplete.isEmpty()) *notes << QStringLiteral("Mesures absentes du texte : %1.").arg(incomplete.join(QStringLiteral(" ; ")));
    }
    return spec;
}


QJsonObject ChrBuilder::mergeCurveMeasures(QJsonObject spec, const QJsonObject& byEvent, const QString& curveType,
                                           const std::function<bool(double)>& allowed, QStringList* notes)
{
    const QString comps = curveComponents(curveType);
    if (comps.isEmpty() || byEvent.isEmpty()) return spec;
    const bool mag = magneticComponents(comps);
    static const QHash<QString, QString> magKeys{
        { "inc", "X_Inc_Depth" }, { "i", "X_Inc_Depth" }, { "inclinaison", "X_Inc_Depth" }, { "inclination", "X_Inc_Depth" },
        { "dec", "Y_Dec" }, { "d", "Y_Dec" }, { "declinaison", "Y_Dec" }, { "declination", "Y_Dec" },
        { "alpha95", "sX_Alpha95_sDepth" }, { "a95", "sX_Alpha95_sDepth" },
        { "field", "Z_Field" }, { "f", "Z_Field" }, { "intensite", "Z_Field" }, { "intensity", "Z_Field" },
        { "error_f", "sZ_sF" }, { "erreur_f", "sZ_sF" }, { "sf", "sZ_sF" }, { "sigma_f", "sZ_sF" } };
    static const QHash<QString, QString> xyzKeys{
        { "x", "X_Inc_Depth" }, { "error_x", "sX_Alpha95_sDepth" }, { "sx", "sX_Alpha95_sDepth" },
        { "y", "Y_Dec" }, { "error_y", "sY" }, { "sy", "sY" }, { "z", "Z_Field" }, { "error_z", "sZ_sF" }, { "sz", "sZ_sF" } };
    static const QStringList chrKeys{ "X_Inc_Depth", "Y_Dec", "Z_Field", "sX_Alpha95_sDepth", "sY", "sZ_sF" };
    QJsonArray events = spec.value("events").toArray();
    int added = 0;
    QStringList refused;
    for (auto it = byEvent.begin(); it != byEvent.end(); ++it) {
        if (!it.value().isObject()) continue;
        int ev = -1;
        for (int i = 0; i < events.size() && ev < 0; ++i) {
            const QString n = events[i].toObject().value("name").toString().trimmed();
            if (n == it.key().trimmed()) ev = i;
        }
        for (int i = 0; i < events.size() && ev < 0; ++i) {
            const QString n = events[i].toObject().value("name").toString().trimmed();
            const auto m = looseName(n).match(it.key().trimmed());
            if (m.hasMatch() && m.capturedLength() == it.key().trimmed().size()) ev = i;
        }
        if (ev < 0) continue;
        QJsonObject e = events[ev].toObject();
        const QJsonObject have = curveMeasures(e);
        const QJsonObject m = it.value().toObject();
        for (auto k = m.begin(); k != m.end(); ++k) {
            QString key = chrKeys.contains(k.key()) ? k.key() : (mag ? magKeys : xyzKeys).value(text::fold(k.key()).trimmed());
            if (key.isEmpty() || have.contains(key) || e.contains(key)) continue;
            const double v = k.value().isString() ? text::toNumber(k.value().toString()) : k.value().toDouble(std::nan(""));
            if (!std::isfinite(v)) continue;
            if (allowed && !allowed(v)) { refused << QStringLiteral("« %1 » %2 = %3").arg(it.key(), k.key(), QString::number(v)); continue; }
            e[key] = v;
            ++added;
        }
        events[ev] = e;
    }
    spec["events"] = events;
    if (notes) {
        if (added) *notes << QStringLiteral("%1 mesure(s) de la courbe lue(s) par le modèle d'IA (format du texte non reconnu par SeldonH) : à vérifier.").arg(added);
        if (!refused.isEmpty()) *notes << QStringLiteral("Mesures du modèle d'IA refusées, absentes de votre texte : %1.").arg(refused.join(QStringLiteral(" ; ")));
    }
    return spec;
}

} // namespace seldon
