// Tools : les « outils » que le modèle de langage peut appeler pour faire un calcul exact.
//
// Une seule définition pour tous les moteurs :
//   - modèle distant : appel d'outils natif (API Chat Completions, paramètre « tools ») -> schemaForApi() + execute() ;
//   - modèle local   : bloc ```calcul (JSON avec « method ») -> MethodRunner::extractCalcBlocks() + MethodRunner::run().
// Dans les deux cas, le calcul est fait par l'application sur l'ordinateur de l'utilisateur, avec les mêmes méthodes
// (MethodRegistry -> méthodes de ChronoModel). Le modèle ne reçoit que le texte du résultat.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "DateFormat.h"
#include "MethodRunner.h"

namespace seldon {
namespace tools {

struct ToolDef {
    QString     name;          // identifiant ASCII (a-z, 0-9, _) accepté par toutes les API
    QString     description;   // ce que voit le modèle
    QJsonObject parameters;    // JSON Schema (type: object, properties, required)
    QString     methodId;      // méthode de MethodRegistry ; vide pour les outils d'information
};

const QVector<ToolDef>& all();
const ToolDef* find(const QString& name);

// [{"type":"function","function":{"name","description","parameters"}}, …] : format « tools » de l'API Chat Completions.
QJsonArray schemaForApi();

struct ToolOutcome {
    bool        known = false;        // l'outil existe
    bool        executed = false;     // un calcul a été lancé (fiche et courbe disponibles dans `run`)
    QString     content;              // texte rendu au modèle (résultat exact, ou explication de l'échec)
    RunOutcome  run;                  // fiche HTML + courbe pour l'interface (vide si rien n'a été calculé)
    QJsonObject request;              // demande normalisée {"method":"c14",…} : le même JSON que le bloc ```calcul (sert à l'historique)
};

// Exécute un appel d'outil. `argumentsJson` : chaîne JSON envoyée par le modèle. Tolère les nombres écrits
// comme des chaînes (« "2500" ») et les virgules décimales. Ne lève jamais d'exception : toute erreur devient
// un texte explicatif dans `content`, que le modèle peut lire pour corriger son appel ou poser la question à l'utilisateur.
ToolOutcome execute(const QString& name, const QString& argumentsJson, DateFormat format);

// Réunit plusieurs courbes (même format de dates) en un seul graphique pour comparer.
// Superpose les courbes de plusieurs blocs « plot » (une datation déjà présente, de même nom, n'est pas répétée).
// Les graphiques de datation (clé « dates » = noms des datations) reçoivent un titre refait avec tous les noms.
QJsonObject mergePlots(const QVector<QJsonObject>& plots);

// Titre d'un graphique de datation : « Densité calendaire — Ly-1234 », « Densités calendaires — US 12, US 10 »,
// ou « Densités calendaires (calcul SeldonH) » si aucune datation n'a de nom.
QString datingPlotTitle(const QStringList& names, int seriesCount);

} // namespace tools
} // namespace seldon
