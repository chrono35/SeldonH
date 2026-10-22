#include "Tools.h"

#include <QJsonDocument>
#include <QStringList>

#include "CurveStore.h"
#include "MethodRegistry.h"

namespace seldon {
namespace tools {

namespace {

QJsonObject num(const QString& desc) { return { { "type", "number" }, { "description", desc } }; }

QJsonObject str(const QString& desc, const QStringList& allowed = {})
{
    QJsonObject o{ { "type", "string" }, { "description", desc } };
    if (!allowed.isEmpty()) o["enum"] = QJsonArray::fromStringList(allowed);
    return o;
}

QJsonObject schema(const QJsonObject& props, const QStringList& required)
{
    return { { "type", "object" }, { "properties", props }, { "required", QJsonArray::fromStringList(required) },
             { "additionalProperties", false } };
}

QString names(const QString& methodId) { return CurveStore::names(methodId).join(", "); }

QVector<ToolDef> build()
{
    QVector<ToolDef> v;

    v.append({ "calibrer_radiocarbone",
               QStringLiteral("Calibre une date radiocarbone (14C) avec les courbes de ChronoModel et renvoie la densité calendaire, "
                              "les intervalles HPD à 95 % et 68 %, le MAP, la moyenne et les quartiles. À utiliser pour toute date 14C "
                              "(âge conventionnel en BP avec son erreur) : ne calibre jamais de tête."),
               schema({ { "age", num(QStringLiteral("Âge radiocarbone conventionnel, en années BP (ex. 2500).")) },
                        { "error", num(QStringLiteral("Erreur (écart-type) de la mesure, en années, strictement positive (ex. 30).")) },
                        { "curve", str(QStringLiteral("Courbe de calibration. Défaut : intcal20 (atmosphère, hémisphère nord) ; shcal20 pour le sud ; "
                                                      "marine20 pour un échantillon marin. Disponibles : %1.").arg(names("c14"))) },
                        { "deltaR", num(QStringLiteral("Effet réservoir régional ΔR, en années (échantillon marin seulement).")) },
                        { "deltaRerr", num(QStringLiteral("Erreur sur ΔR, en années (avec deltaR).")) } },
                      { "age", "error" }),
               "c14" });

    v.append({ "calibrer_f14c",
               QStringLiteral("Calibre une fraction moderne F14C (échantillon post-bombe, après 1950) avec les courbes de la bombe atomique "
                              "et renvoie la densité calendaire et les intervalles HPD à 95 % et 68 %."),
               schema({ { "f14c", num(QStringLiteral("Fraction moderne F14C (ex. 1.2345).")) },
                        { "error", num(QStringLiteral("Erreur sur F14C, strictement positive (ex. 0.0030).")) },
                        { "curve", str(QStringLiteral("Courbe. Défaut : bomb21nhz1 (zone 1 de l'hémisphère nord) ; bomb21shz1 pour le sud ; "
                                                      "intcal20 pour une date avant la bombe. Disponibles : %1.")
                                           .arg(names("f14c") + ", " + names("c14"))) } },
                      { "f14c", "error" }),
               "f14c" });

    v.append({ "dater_luminescence",
               QStringLiteral("Densité calendaire et intervalles HPD pour une datation par luminescence (TL/OSL) : un âge en années "
                              "avant l'année de mesure, avec son erreur. L'année de mesure est obligatoire : demande-la à l'utilisateur si elle manque."),
               schema({ { "age", num(QStringLiteral("Âge de luminescence, en années avant l'année de mesure.")) },
                        { "error", num(QStringLiteral("Erreur (écart-type) de l'âge, en années, strictement positive.")) },
                        { "refYear", num(QStringLiteral("Année de la mesure (ex. 2015).")) } },
                      { "age", "error", "refYear" }),
               "tl" });

    v.append({ "densite_gauss",
               QStringLiteral("Densité calendaire et intervalles HPD pour une date donnée directement en années calendaires par une loi "
                              "normale (moyenne, écart-type), sans calibration. Les années avant J.-C. sont négatives."),
               schema({ { "age", num(QStringLiteral("Moyenne, en année calendaire signée (négatif = avant J.-C., ex. -500 pour 500 av. J.-C.).")) },
                        { "error", num(QStringLiteral("Écart-type, en années, strictement positif.")) } },
                      { "age", "error" }),
               "gauss" });

    v.append({ "densite_uniforme",
               QStringLiteral("Densité calendaire uniforme entre deux années (terminus post quem / terminus ante quem) et ses intervalles "
                              "HPD. Les années avant J.-C. sont négatives."),
               schema({ { "min", num(QStringLiteral("Borne inférieure, en année calendaire signée.")) },
                        { "max", num(QStringLiteral("Borne supérieure, en année calendaire signée.")) } },
                      { "min", "max" }),
               "unif" });

    v.append({ "dater_archeomagnetisme",
               QStringLiteral("Datation archéomagnétique par comparaison à une courbe de référence de la variation séculaire : "
                              "inclinaison (I), déclinaison (D), intensité (F) ou une combinaison. Renvoie la densité calendaire et les "
                              "intervalles HPD. Pour I ou D, α95 est obligatoire ; pour F, l'erreur sur l'intensité est obligatoire. "
                              "Demande à l'utilisateur toute valeur manquante."),
               schema({ { "process", str(QStringLiteral("Grandeurs mesurées : I (inclinaison), D (déclinaison), F (intensité), "
                                                         "ou ID, IF, IDF."), { "I", "D", "F", "ID", "IF", "IDF" }) },
                        { "inc", num(QStringLiteral("Inclinaison, en degrés (si process contient I).")) },
                        { "dec", num(QStringLiteral("Déclinaison, en degrés (si process contient D).")) },
                        { "alpha95", num(QStringLiteral("Cône de confiance α95, en degrés, strictement positif (si I ou D).")) },
                        { "field", num(QStringLiteral("Intensité du champ, en µT (si process contient F).")) },
                        { "errorF", num(QStringLiteral("Erreur sur l'intensité, en µT, strictement positive (si F).")) },
                        { "curveI", str(QStringLiteral("Courbe d'inclinaison (défaut gal2002sph2014_i ; bulgaria_i pour la Bulgarie). Disponibles : %1.").arg(names("am"))) },
                        { "curveD", str(QStringLiteral("Courbe de déclinaison (défaut gal2002sph2014_d ; bulgaria_d).")) },
                        { "curveF", str(QStringLiteral("Courbe d'intensité (défaut gwh2013uni_f ; bulgaria_f).")) } },
                      { "process" }),
               "am" });

    v.append({ "calculer_evenement",
               QStringLiteral("Estime la date θ d'un ÉVÉNEMENT (Event de ChronoModel) à partir de plusieurs datations du même "
                              "événement, éventuellement de méthodes différentes (14C, F14C, TL/OSL, archéomagnétisme, Gauss, uniforme), "
                              "par MCMC avec le modèle d'événement de ChronoModel (dispersion individuelle σ_i de chaque date). "
                              "Paramètres MCMC par défaut de ChronoModel si l'utilisateur n'en précise pas. Renvoie la densité a posteriori "
                              "de θ, ses HPD, les valeurs a posteriori de chaque date et les critères de convergence (R̂, ESS)."),
               schema({ { "name", str(QStringLiteral("Nom de l'événement (ex. « US 12 »).")) },
                        { "dates", QJsonObject{ { "type", "array" },
                                                { "description", QStringLiteral("Datations : objets avec « method » (c14, f14c, tl, gauss, unif, am) et les "
                                                                                "paramètres de cette méthode (mêmes noms que les autres outils, ex. "
                                                                                "{\"method\":\"c14\",\"age\":2500,\"error\":30}), et facultativement "
                                                                                "« name » et « wiggle » ({\"type\":\"fixed\",\"value\":20}, "
                                                                                "{\"type\":\"range\",\"min\":10,\"max\":30} ou "
                                                                                "{\"type\":\"gaussian\",\"mean\":20,\"error\":5}) pour un décalage δ entre "
                                                                                "l'échantillon et l'événement.") },
                                                { "items", QJsonObject{ { "type", "object" } } } } },
                        { "tmin", num(QStringLiteral("Début de la période d'étude (année signée). Facultatif : calculé d'après les datations.")) },
                        { "tmax", num(QStringLiteral("Fin de la période d'étude (année signée). Facultatif.")) },
                        { "mcmc", QJsonObject{ { "type", "object" },
                                               { "description", QStringLiteral("Facultatif, seulement si l'utilisateur le demande : num_iter (100000), num_burn (1000), "
                                                                               "num_proc = nombre de chaînes (4), max_batches (200), iter_per_batch (100), "
                                                                               "thinning_interval (10), mixing_level (0.99), anneal_temp (10), "
                                                                               "anneal_recurrence (1000), anneal_dwell (1000), seeds (liste d'entiers).") } } } },
                      { "dates" }),
               "event" });

    v.append({ "courbes_disponibles",
               QStringLiteral("Liste les courbes de référence intégrées à SeldonH (calibration 14C, F14C bombe, archéomagnétisme). "
                              "À appeler avant un calcul quand l'utilisateur nomme une courbe dont tu n'es pas sûr."),
               schema({ { "method", str(QStringLiteral("Méthode : c14, f14c ou am. Vide = toutes."), { "c14", "f14c", "am" }) } }, {}),
               QString() });

    // Niveau de confiance facultatif, commun à toutes les datations.
    for (ToolDef& d : v) {
        if (d.methodId.isEmpty()) continue;
        QJsonObject props = d.parameters.value("properties").toObject();
        props["level"] = num(QStringLiteral("Niveau de confiance des intervalles HPD, en % (défaut 95). À donner seulement si "
                                            "l'utilisateur demande un autre niveau (ex. 99 ou 90) ; les HPD à 95 % et 68 % sont toujours fournies."));
        if (d.methodId != "event")
            props["wiggle"] = QJsonObject{ { "type", "object" },
                                           { "description", QStringLiteral("Facultatif, seulement si l'utilisateur donne un décalage δ (wiggle-matching) entre "
                                                                           "l'échantillon et ce qu'on veut dater : {\"type\":\"fixed\",\"value\":20}, "
                                                                           "{\"type\":\"range\",\"min\":10,\"max\":30} ou "
                                                                           "{\"type\":\"gaussian\",\"mean\":20,\"error\":5}. Le résultat est la densité de t + δ.") } };
        d.parameters["properties"] = props;
    }
    return v;
}

// Convertit en nombre les valeurs données comme chaînes ; retourne un message d'erreur si impossible.
QString coerce(const ToolDef& def, QJsonObject& args)
{
    const QJsonObject props = def.parameters.value("properties").toObject();
    QJsonObject out;
    for (auto it = props.begin(); it != props.end(); ++it) {
        if (!args.contains(it.key())) continue;
        QJsonValue v = args.value(it.key());
        if (v.isNull() || v.isUndefined()) continue;
        const QString type = it.value().toObject().value("type").toString();
        if (type == "number") {
            if (v.isString()) {
                QString s = v.toString().trimmed();
                s.replace(QChar(0x00A0), QString()).replace(' ', QString()).replace(',', '.');
                if (s.isEmpty()) continue;
                bool ok = false;
                const double d = s.toDouble(&ok);
                if (!ok) return QStringLiteral("la valeur de « %1 » n'est pas un nombre : « %2 »").arg(it.key(), v.toString());
                v = d;
            } else if (!v.isDouble()) {
                return QStringLiteral("la valeur de « %1 » doit être un nombre").arg(it.key());
            }
        } else if (type == "string") {
            if (!v.isString()) v = QString::number(v.toDouble());
            const QString t = v.toString().trimmed();
            if (t.isEmpty()) continue;
            v = t;
        }
        out[it.key()] = v;
    }
    args = out;
    return {};
}

QStringList missing(const ToolDef& def, const QJsonObject& a)
{
    QStringList m;
    for (const QJsonValue& r : def.parameters.value("required").toArray())
        if (!a.contains(r.toString())) m << r.toString();
    if (def.methodId == "am") {
        const QString p = a.value("process").toString();
        if ((p.contains('I') || p.contains('D')) && !a.contains("alpha95")) m << "alpha95";
        if (p.contains('I') && !a.contains("inc")) m << "inc";
        if (p.contains('D') && !a.contains("dec")) m << "dec";
        if (p.contains('F')) {
            if (!a.contains("field")) m << "field";
            if (!a.contains("errorF")) m << "errorF";
        }
    }
    return m;
}

QString curvesText(const QString& which)
{
    struct Row { const char* id; const char* title; const char* def; };
    const Row rows[] = { { "c14", "Calibration radiocarbone (calibrer_radiocarbone)", "intcal20" },
                         { "f14c", "F14C post-bombe (calibrer_f14c ; accepte aussi les courbes 14C)", "bomb21nhz1" },
                         { "am", "Archéomagnétisme (dater_archeomagnetisme)", "gal2002sph2014_i / _d, gwh2013uni_f" } };
    QString t;
    for (const Row& r : rows) {
        if (!which.isEmpty() && which != r.id) continue;
        QStringList n = CurveStore::names(r.id);
        if (QString(r.id) == "f14c") n += CurveStore::names("c14");
        t += QStringLiteral("%1 — défaut : %2\n  %3\n").arg(QString::fromUtf8(r.title), QString::fromUtf8(r.def), n.join(", "));
    }
    return t.isEmpty() ? QStringLiteral("Méthode inconnue : « %1 » (c14, f14c ou am).").arg(which) : t.trimmed();
}

} // namespace

const QVector<ToolDef>& all()
{
    static const QVector<ToolDef> defs = build();
    return defs;
}

const ToolDef* find(const QString& name)
{
    for (const ToolDef& d : all())
        if (d.name == name) return &d;
    return nullptr;
}

QJsonArray schemaForApi()
{
    QJsonArray a;
    for (const ToolDef& d : all())
        a.append(QJsonObject{ { "type", "function" },
                              { "function", QJsonObject{ { "name", d.name }, { "description", d.description }, { "parameters", d.parameters } } } });
    return a;
}

ToolOutcome execute(const QString& name, const QString& argumentsJson, DateFormat format)
{
    ToolOutcome r;
    const ToolDef* def = find(name.trimmed());
    if (!def) {
        QStringList ns;
        for (const ToolDef& d : all()) ns << d.name;
        r.content = QStringLiteral("Outil inconnu : « %1 ». Outils disponibles : %2.").arg(name, ns.join(", "));
        return r;
    }
    r.known = true;

    QJsonObject args;
    const QString raw = argumentsJson.trimmed();
    if (!raw.isEmpty()) {
        QJsonParseError pe;
        const QJsonDocument d = QJsonDocument::fromJson(raw.toUtf8(), &pe);
        if (!d.isObject()) {
            r.content = QStringLiteral("Arguments invalides pour %1 : un objet JSON est attendu%2.")
                            .arg(def->name, pe.error != QJsonParseError::NoError ? QStringLiteral(" (%1)").arg(pe.errorString()) : QString());
            return r;
        }
        args = d.object();
    }

    const QString bad = coerce(*def, args);
    if (!bad.isEmpty()) { r.content = QStringLiteral("Arguments invalides pour %1 : %2.").arg(def->name, bad); return r; }

    if (def->methodId.isEmpty()) {              // outil d'information
        r.content = curvesText(args.value("method").toString().toLower());
        return r;
    }

    if (def->methodId == "event") {
        // Datations normalisées (alias de méthode, format .chr, nombres en texte) avant la vérification.
        QJsonArray ds;
        for (const QJsonValue& v : args.value("dates").toArray())
            ds.append(v.isObject() ? QJsonValue(MethodRunner::normalizeRequest(v.toObject())) : v);
        args["dates"] = ds;
        if (ds.isEmpty()) {
            r.content = QStringLiteral("Calcul non lancé : « dates » doit contenir au moins une datation (objets avec « method »).");
            return r;
        }
        for (int i = 0; i < ds.size(); ++i)
            if (!ds[i].isObject() || ds[i].toObject().value("method").toString().isEmpty()) {
                r.content = QStringLiteral("Calcul non lancé : la datation %1 n'a pas de « method » (c14, f14c, tl, gauss, unif ou am).").arg(i + 1);
                return r;
            }
    }
    if (def->methodId == "am" && args.contains("process")) args["process"] = args.value("process").toString().toUpper();
    if (args.contains("curve")) args["curve"] = args.value("curve").toString().toLower();

    const QStringList miss = missing(*def, args);
    if (!miss.isEmpty()) {
        r.content = QStringLiteral("Calcul non lancé : il manque %1. Demande ces valeurs à l'utilisateur ; n'en invente aucune.")
                        .arg(miss.join(", "));
        return r;
    }

    QJsonObject req = args;
    req["method"] = def->methodId;
    if (def->methodId == "c14" && req.contains("deltaR")) req["hasDeltaR"] = true;
    if (def->methodId == "am") req["steps"] = 500;

    r.run = MethodRunner::run({ req }, format);
    r.executed = r.run.found;
    r.request = req;
    r.content = r.run.promptBlock;
    return r;
}

QJsonObject mergePlots(const QVector<QJsonObject>& plots)
{
    QJsonObject merged;
    QJsonArray series;
    QStringList names;
    bool sameLevel = true;
    for (const QJsonObject& p : plots) {
        if (p.isEmpty()) continue;
        if (merged.isEmpty()) merged = p;
        else if (p.value("hpd").toDouble(95) != merged.value("hpd").toDouble(95)) sameLevel = false;
        for (const QJsonValue& s : p.value("series").toArray()) {
            const QString n = s.toObject().value("name").toString();
            if (!n.isEmpty() && names.contains(n)) continue;          // même datation déjà tracée
            names << n;
            series.append(s);
        }
    }
    if (!merged.isEmpty()) {
        merged["series"] = series;
        if (!sameLevel) merged["hpd"] = 95;                            // niveaux différents : 95 % pour toutes
    }
    return merged;
}

} // namespace tools
} // namespace seldon
