#include "ChrBuilder.h"

#include <QJsonArray>
#include <QDate>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>
#include <QHash>
#include <cmath>
#include <limits>

namespace seldon {

namespace {

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

// Code « process_type » du plugin archéomagnétisme de ChronoModel. 1 (I) et 3 (F) sont vérifiés sur des
// projets réels ; les autres codes suivent l'ordre I, D, F, ID, IF, IDF et restent à confirmer.
int amProcessCode(const QString& p, bool* verified)
{
    const QString k = p.trimmed().toUpper();
    static const QHash<QString, int> m = { { "I", 1 }, { "D", 2 }, { "F", 3 }, { "ID", 4 }, { "IF", 5 }, { "IDF", 6 } };
    if (verified) *verified = (k == "I" || k == "F");
    return m.value(k, 0);
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
    int nextEvent = 1, row = 0, amWithoutSpan = 0, nextDate = 0;

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
                span.add(age - 3 * err); span.add(age + 3 * err);
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
                span.add(mn); span.add(mx);
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
                span.add(ref - age - 3 * err); span.add(ref - age + 3 * err);
            } else if (type == "am" || type == "archeomagnetisme") {
                // Archéomagnétisme : mesure(s) comparée(s) aux courbes de référence de ChronoModel.
                bool procVerified = true;
                const QString proc = d.value("process").toString("I").trimmed().toUpper();
                const int code = amProcessCode(proc, &procVerified);
                if (code == 0) {
                    R.errors << QStringLiteral("Date archéomagnétique « %1 » : « process » doit valoir I, D, F, ID, IF ou IDF.").arg(out["name"].toString());
                    continue;
                }
                const bool needI = proc.contains('I'), needD = proc.contains('D'), needF = proc.contains('F');
                auto num = [&](const char* k) { return d.value(k).toDouble(std::nan("")); };
                QString missing;
                if (needI && std::isnan(num("inc")))  missing += " inc";
                if (needD && std::isnan(num("dec")))  missing += " dec";
                if ((needI || needD) && !(num("alpha95") > 0)) missing += " alpha95";
                if (needF && std::isnan(num("field"))) missing += " field";
                if (needF && !(num("error_f") > 0))    missing += " error_f";
                if (!missing.isEmpty()) {
                    R.errors << QStringLiteral("Date archéomagnétique « %1 » (process %2) : valeur(s) manquante(s) ou invalide(s) :%3.")
                                    .arg(out["name"].toString(), proc, missing);
                    continue;
                }
                if (!procVerified)
                    R.warnings << QStringLiteral("Date archéomagnétique « %1 » : le code du processus %2 n'a pas été vérifié sur un projet réel ; "
                                                 "contrôlez-le dans ChronoModel.").arg(out["name"].toString(), proc);
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
                    { "inc",     needI ? num("inc") : 0.0 },
                    { "dec",     needD ? num("dec") : 0.0 },
                    { "alpha95", (needI || needD) ? num("alpha95") : 0.0 },
                    { "field",   needF ? num("field") : 0.0 },
                    { "error_f", needF ? num("error_f") : 0.0 },
                    { "integration_steps", d.value("integration_steps").toInt(500) },
                    { "refI_curve", cur("curve_i", kAmCurveI) },
                    { "refD_curve", cur("curve_d", kAmCurveD) },
                    { "refF_curve", cur("curve_f", kAmCurveF) }
                };
                // L'étendue de la date n'est pas connue sans la courbe : on ne l'ajoute pas à la plage d'étude.
                ++amWithoutSpan;
            } else {
                R.errors << QStringLiteral("Type de date « %1 » non géré (événement « %2 »). Types gérés : gauss, unif, tl (TL/OSL), am (archéomagnétisme).")
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
    auto buildConstraints = [&](const QJsonArray& list, const QHash<QString, int>& ids,
                                const QString& what) {
        QJsonArray out;
        int cid = 1;
        for (const QJsonValue& v : list) {
            const QJsonArray pair = v.toArray();
            if (pair.size() != 2) { R.warnings << QStringLiteral("Contrainte %1 ignorée (2 noms attendus).").arg(what); continue; }
            const QString a = pair[0].toString().trimmed(), b = pair[1].toString().trimmed();
            if (!ids.contains(a) || !ids.contains(b)) {
                R.warnings << QStringLiteral("Contrainte %1 « %2 → %3 » ignorée : nom inconnu.").arg(what, a, b);
                continue;
            }
            if (a == b) { R.warnings << QStringLiteral("Contrainte %1 « %2 » ignorée : même élément.").arg(what, a); continue; }
            QJsonObject c;
            c["id"] = cid++;
            c["bwd_id"] = ids[kBwdIsOlder ? a : b];
            c["fwd_id"] = ids[kBwdIsOlder ? b : a];
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
        R.warnings << QStringLiteral("Plage d'étude par défaut pour les dates archéomagnétiques : -1000 à 2000. "
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
        R.warnings << QStringLiteral("La plage d'étude calculée ne tient pas compte des dates archéomagnétiques : vérifiez tmin et tmax.");
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
    s += QStringLiteral("MCMC : %1 itérations, %2 burn-in, %3 chaînes\n").arg(mc.value("num_iter").toInt()).arg(mc.value("num_burn").toInt()).arg(mc.value("num_proc").toInt());

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
            else if (pl == "14c")   ds << QStringLiteral("14C %1±%2").arg(dd.value("age").toDouble()).arg(dd.value("error").toDouble());
            else if (pl == "tl/osl") ds << QStringLiteral("TL/OSL %1±%2 (réf. %3)").arg(dd.value("age").toDouble()).arg(dd.value("error").toDouble()).arg(dd.value("ref_year").toDouble());
            else if (pl == "density") ds << QStringLiteral("densité tabulée « %1 »").arg(dd.value("curve").toString());
            else if (pl == "am") {
                static const char* names[] = { "?", "I", "D", "F", "ID", "IF", "IDF" };
                const int pt = dd.value("process_type").toInt();
                const QString pr = (pt >= 1 && pt <= 6) ? QString::fromLatin1(names[pt]) : QString::number(pt);
                QStringList v;
                if (pr.contains('I')) v << QStringLiteral("I=%1").arg(dd.value("inc").toDouble());
                if (pr.contains('D')) v << QStringLiteral("D=%1").arg(dd.value("dec").toDouble());
                if (pr.contains('I') || pr.contains('D')) v << QStringLiteral("α95=%1").arg(dd.value("alpha95").toDouble());
                if (pr.contains('F')) v << QStringLiteral("F=%1±%2").arg(dd.value("field").toDouble()).arg(dd.value("error_f").toDouble());
                ds << QStringLiteral("archéomag. %1 (%2)").arg(pr, v.join(", "));
            }
            else                    ds << pl;
        }
        if (!ds.isEmpty()) s += " : " + ds.join("; ");
        s += "\n";
    }
    s += QStringLiteral("Contraintes d'événements : %1 ; de phases : %2\n")
             .arg(chr.value("events_constraints").toArray().size()).arg(chr.value("phases_constraints").toArray().size());
    return s;
}

} // namespace seldon
