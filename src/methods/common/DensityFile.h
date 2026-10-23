// DensityFile : fichier de densité tabulée pour le plugin Density de ChronoModel, à partir d'une loi classique.
//
// Format lu par PluginDensity::loadRefFile (ChronoModel) : CSV anglais, seule la 1re colonne est lue ; lignes de
// commentaire commençant par //, #, /* ou ! ; 1re valeur = tmin, 2e = tmax, puis N valeurs de densité sur une grille
// régulière t_k = tmin + k (tmax − tmin) / (N − 1). ChronoModel ramène l'aire à 1, interpole linéairement et met la
// densité à 0 hors de [tmin ; tmax]. Le fichier va dans <GenericDataLocation>/CNRS/ChronoModel/Calib/Density.
//
// Spécification (JSON, clés du bloc « calcul » {"method":"density_file", …} et de l'outil creer_densite) :
//   law     : normal (mean, sd) | lognormal (meanlog, sdlog — ou median, factor) | uniform (min, max)
//             | triangular (min, mode, max) | trapezoidal (min, mode_min, mode_max, max)
//             | exponential (mean ou rate) | gamma (shape + scale ou rate — ou mean + sd) | beta (alpha, beta, min, max)
//   offset  : placement sur l'axe du temps, t = offset + x (0 par défaut) ;
//   reverse : vrai -> t = offset − x (une durée comptée vers le passé, ex. « au plus 50 ans avant 1200 ») ;
//   tmin, tmax : bornes du fichier (années) ; sinon calculées (support de la loi ou quantiles extrêmes) ;
//   step ou points : pas de la grille (années) ou nombre de valeurs ; sinon ≈ 1 an (101 à 2001 valeurs) ;
//   name    : nom du fichier (.csv ajouté) ; sinon construit d'après la loi.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "Density.h"

namespace seldon {

struct DensityFileResult {
    bool        ok = false;
    QString     error;            // si !ok, en français
    QStringList warnings;
    QString     law;              // loi normalisée (normal, lognormal…)
    QString     description;      // ex. « loi normale N(−250 ; 50) »
    QString     placement;        // ex. « t = 1200 − x » ; vide si t = x
    QString     fileName;         // nom proposé, en minuscules, .csv
    double      tmin = 0.0, tmax = 0.0, step = 0.0;
    int         points = 0;
    Density     density;          // grille et densité (aire 1), pour les statistiques et le graphique
    QString     csv;              // contenu complet du fichier
};

DensityFileResult buildDensityFile(const QJsonObject& spec);

// Nom de loi reconnu (français ou anglais : « normale », « log-normale », « triangulaire »…) ; vide sinon.
QString densityLaw(const QString& name);

} // namespace seldon
