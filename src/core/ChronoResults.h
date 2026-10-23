// ChronoResults : résumé d'un export des résultats de ChronoModel (« Log_Posterior_Distrib_Stats.html », onglet
// Résultats > Log) pour le modèle de langage. Le fichier complet fait plusieurs centaines de milliers de caractères :
// SeldonH en extrait les valeurs exactes utiles au diagnostic (convergence, σ des dates), le modèle les commente.
//
// Lecture : blocs « Event : nom », « Data : nom », sous-blocs « Posterior Event Date », « Posterior Shrinkage param. »,
// « Posterior calib. date », « Posterior Std ti » ; critères « R̂ = … · ESS bulk = … · ESS tail = … ✅ » (une ligne) ou
// une ligne par critère avec ✅ / ⚠️ ; statistiques de densité « MAP = … ; Mean = … ; Std = … ».
#pragma once

#include <QString>
#include <QVector>

namespace seldon {
namespace chronoresults {

struct Variable {
    QString kind;              // « date de l'événement », « σ de l'événement (shrinkage) », « date calibrée », « σ de la date (Std ti) »
    QString event;             // Event auquel appartient la variable
    QString data;              // Data (vide pour une variable d'Event)
    double  rhat = 0.0;
    int     essBulk = 0, essTail = 0;
    bool    rhatOk = true, bulkOk = true, tailOk = true;
    double  map = 0.0, stdev = 0.0;    // statistiques de la densité (MAP, écart-type)
    bool    hasStats = false;
    bool ok() const { return rhatOk && bulkOk && tailOk; }
};

struct Report {
    QStringList header;        // projet, version, durées, critère global, lignes R̂ / ESS
    QVector<Variable> vars;
    int essThreshold = 1500;   // lu dans « (< 1500) » s'il est présent
    int events = 0, data = 0;
};

// HTML -> texte (balises de bloc -> retours à la ligne, entités décodées).
QString htmlToText(const QString& html);
// Vrai si le texte est un export de résultats de ChronoModel.
bool looksLike(const QString& text);
Report parse(const QString& text);
// Résumé pour le modèle (français) : critère global, variables hors critères de la pire à la meilleure, facteur
// d'itérations suggéré, dates au σ grand (outliers possibles), Events dont toutes les dates ont un σ grand.
QString summarize(const Report& r, const QString& fileName, int maxListed = 30);

} // namespace chronoresults
} // namespace seldon
