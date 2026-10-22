// MethodRunner : orchestre les méthodes pour l'interface.
//   question  ->  demandes reconnues  ->  calculs  ->  fiche HTML + bloc de texte pour le modèle + courbes
// Le modèle de langage peut aussi demander un calcul lui-même avec un bloc de code « calcul » (JSON).
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include "DateFormat.h"

namespace seldon {

struct RunOutcome {
    bool        found = false;      // au moins une demande exécutée (même en échec)
    int         count = 0;
    QString     noticeHtml;         // fiche affichée dans le dialogue
    QString     promptBlock;        // valeurs exactes transmises au modèle
    QJsonObject plot;               // bloc « plot » (vide si rien à tracer)
};

class MethodRunner {
public:
    // Reconnaît les dates de la question (voir MethodRegistry::parse) et les calcule.
    // `format` : format d'affichage par défaut (Préférences) ; une demande explicite dans la question le remplace
    // (« donne le résultat en cal BP »).
    // Vrai si la question demande de CRÉER un modèle ChronoModel (« fais-moi un modèle avec deux phases… ») : les valeurs
    // citées (âges, mesures) sont alors des données du modèle, pas des datations à calculer une par une.
    static bool isModelRequest(const QString& question);

    // Une demande de création de modèle ne déclenche aucun calcul automatique (résultat vide).
    static RunOutcome analyzeQuestion(const QString& question, DateFormat format = DateFormat::BcAd);

    // Exécute des demandes déjà structurées (JSON avec "method").
    static RunOutcome run(const QVector<QJsonObject>& requests, DateFormat format = DateFormat::BcAd);

    // Blocs ```calcul ... ``` d'une réponse du modèle : objet JSON ou tableau d'objets, "method" obligatoire.
    static QVector<QJsonObject> extractCalcBlocks(const QString& answer);
};

} // namespace seldon
