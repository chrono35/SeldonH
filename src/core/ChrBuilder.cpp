#include "ChrBuilder.h"

#include <QJsonArray>
#include <QDate>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>
#include <QHash>
#include <QPair>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "../methods/common/MethodRegistry.h"
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

struct Range { double lo =  std::numeric_limits<double>::infinity();
               double hi = -std::numeric_limits<double>::infinity();
               void add(double v) { lo = std::min(lo, v); hi = std::max(hi, v); }
               bool valid() const { return lo <= hi; } };

} // namespace

ChrResult ChrBuilder::build(const QJsonObject& spec)
{
    ChrResult R;
    const QJsonArray evSpec = spec.value("events").toArray();
    if (evSpec.isEmpty()) {
        R.errors << QStringLiteral("La spécification ne contient aucun événement (« events »).");
        return R;
    }

    bool curveKnown = true;
    const QString curveName = spec.value("curve").toString();
    const int processType = curveProcessType(curveName, &curveKnown);
    if (!curveKnown)
        R.warnings << QStringLiteral("Type de courbe « %1 » inconnu : modèle sans courbe utilisé.").arg(curveName);
    const bool withCurve = processType != 78;

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
        if (!n.trimmed().isEmpty()) ensurePhase(n);
    }

    // ---------- Événements ----------
    QJsonArray events;
    QHash<QString, int> eventId;
    Range span;
    int nextEvent = 1, row = 0, amWithoutSpan = 0, nextDate = 0;     // amWithoutSpan : dates AM ou densités (étendue inconnue)

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
        o["item_x"] = 300.0 + (row % 6) * 130.0;
        o["item_y"] = 100.0 + (row / 6) * 110.0;
        ++row;
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
        for (const QJsonValue& dv : e.value("dates").toArray()) {
            const QJsonObject d = dv.toObject();
            const QString type = d.value("type").toString().toLower();
            QJsonObject out;
            out["UUID"] = uuid4();
            out["id"] = nextDate;               // incrémenté seulement si la date est retenue (voir plus bas)
            out["name"] = d.value("name").toString(QStringLiteral("%1-%2").arg(name).arg(++dn));
            out["valid"] = true;
            setColor(out, 71, 112, 76);
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
                const bool hasRef = d.contains("ref_year");
                const double ref = hasRef ? d.value("ref_year").toDouble() : QDate::currentDate().year();
                if (!hasRef)
                    R.warnings << QStringLiteral("Date TL/OSL « %1 » : année de référence absente, %2 utilisée (année en cours). "
                                                 "Précisez « ref_year » (année de la mesure).").arg(out["name"].toString()).arg(static_cast<int>(ref));
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
                const double dR = d.value("delta_r").toDouble(d.value("deltaR").toDouble(0.0));
                const double dRerr = d.value("delta_r_error").toDouble(d.value("deltaRerr").toDouble(0.0));
                if (marine && !d.contains("delta_r") && !d.contains("deltaR"))
                    R.warnings << QStringLiteral("Date radiocarbone « %1 » : courbe marine sans ΔR (0 utilisé). Précisez « delta_r » et "
                                                 "« delta_r_error ».").arg(out["name"].toString());
                out["plugin_id"] = "14c";
                out["sampler"] = kDateSampler;
                out["data"] = QJsonObject{ { "age", age }, { "delta_r", dR }, { "delta_r_error", dRerr },
                                           { "error", err }, { "ref_curve", curve } };
                // Plage d'étude : HPD à 95 % de la calibration (méthode de SeldonH, même courbe), avec une marge.
                QJsonObject rq{ { "method", "c14" }, { "age", age }, { "error", err }, { "curve", curve.chopped(4) } };
                if (dR != 0.0 || dRerr != 0.0) { rq["deltaR"] = dR; rq["deltaRerr"] = dRerr; rq["hasDeltaR"] = true; }
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
        if (!isBound && dates.isEmpty())
            R.warnings << QStringLiteral("L'événement « %1 » n'a aucune date.").arg(name);
        o["dates"] = dates;

        // Champs propres aux événements de courbe : toujours présents dans les projets réels (valeur 0 hors courbe).
        o["sampler"] = kEventSampler;
        for (const char* k : { "X_Inc_Depth", "Y_Dec", "Z_Field", "sX_Alpha95_sDepth", "sY", "sZ_sF" })
            o[k] = 0.0;
        if (withCurve) {
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
                R.warnings << QStringLiteral("Contrainte %1 « %2 » ignorée : nom inconnu.").arg(what, shown);
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
            R.warnings << QStringLiteral("Contrainte %1 « %2 » ignorée : relation non reconnue (utiliser par ex. avant / après, "
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
    const QJsonArray evCons = buildConstraints(spec.value("event_constraints").toArray(), eventId, "d'événements");
    const QJsonArray phCons = buildConstraints(spec.value("phase_constraints").toArray(), phaseId, "de phases");

    // Positions des phases en colonne
    for (int i = 0; i < phases.size(); ++i) {
        QJsonObject p = phases[i].toObject();
        p["item_x"] = 100.0;
        p["item_y"] = 100.0 + i * 110.0;
        phases[i] = p;
    }

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
    if (withCurve)
        R.warnings << QStringLiteral("Paramètres de courbe (alpha, speed_threshold, variance_fixed) : valeurs par défaut arbitraires.");

    QJsonObject root;
    root["app_version"] = "4.0.0";
    root["curve"] = curve;
    root["dates_trash"] = QJsonArray();
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
