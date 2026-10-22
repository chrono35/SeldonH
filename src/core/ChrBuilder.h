// ChrBuilder : transforme une SPÉCIFICATION SIMPLIFIÉE (JSON court produit par le LLM) en un
// projet ChronoModel complet (.chr, JSON).
//
// Pourquoi ? Un petit modèle local écrit mal un JSON de 200 champs, mais il sait très bien
// remplir un JSON de 10 champs. Le code déterministe ajoute ensuite tous les champs obligatoires
// (identifiants, couleurs, positions, MCMC, plages tmin/tmax...), ce qui garantit un fichier
// valide quelle que soit la qualité du modèle.
//
// Spécification acceptée (tout est optionnel sauf `events`) :
// {
//   "name": "Site X",
//   "phases":  [ {"name": "Phase 1"} ],
//   "events":  [ {"name": "US 101", "phases": ["Phase 1"],
//                 "dates": [ {"type":"gauss","name":"Ech-1","age":-520,"error":35},
//                            {"type":"unif","name":"TPQ-TAQ","min":-600,"max":-450},
//                            {"type":"tl","name":"TL-1","age":488,"error":19,"ref_year":2025},
//                            {"type":"am","name":"Four","process":"IF","inc":65,"alpha95":1.5,
//                             "field":54.2,"error_f":0.8,"curve_i":"gal2002sph2014_i","curve_f":"eurw_2017_f"} ] },
//                {"name": "Fondation", "type":"bound", "known_fixed": -700} ],
//   "event_constraints": [ ["US 101", "US 102"] ],   // [plus ancien, plus récent]
//   "phase_constraints": [ ["Phase 1", "Phase 2"] ], // [plus ancienne, plus récente]
//   "settings": {"tmin": -1000, "tmax": 200, "step": 1},
//   "mcmc": {"num_iter": 100000}
// }
// Archéomagnétisme : "process" = I (inclinaison), D (déclinaison), F (intensité), ID (directionnel), IF ou IDF (vectoriel).
// I demande inc + alpha95 ; D demande dec + inc + alpha95 (l'erreur sur D dépend de I) ; F demande field + error_f.
// Les âges gauss/unif sont en années calendaires (négatif = avant J.-C.) ; pour « tl », « age » est un âge en années
// avant « ref_year » (année de la mesure). Les champs écrits (échantillonneurs, MCMC, courbe, clés des dates et des
// événements) reprennent ceux de projets ChronoModel 4.0 réels.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace seldon {

struct ChrResult {
    bool        ok = false;
    QJsonObject chr;           // document .chr complet
    QStringList errors;        // bloquantes
    QStringList warnings;      // à montrer à l'utilisateur
    QString     suggestedName; // nom de fichier proposé (sans extension)
};

class ChrBuilder {
public:
    static ChrResult build(const QJsonObject& spec);

    // Extrait le premier bloc de code « chronomodel » (ou « json » contenant "events") d'un texte.
    static bool extractSpec(const QString& markdown, QJsonObject& spec, QString* err = nullptr);

    // Résumé compact d'un .chr existant, injectable dans un prompt (interprétation, relecture).
    static QString summarize(const QJsonObject& chr, int maxEvents = 60);

    // Convention de contrainte : [a, b] = a plus ancien que b. ChronoModel stocke bwd_id (plus ancien) / fwd_id (plus récent) ;
    // sens vérifié sur trois projets réels.
    static constexpr bool kBwdIsOlder = true;
};

} // namespace seldon
