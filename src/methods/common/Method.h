// Method : une « méthode de datation » (équivalent d'un plugin de ChronoModel).
//
// Chaque méthode vit dans son dossier (src/methods/<id>/) avec ses courbes de référence :
//   c14, f14c, gauss, unif, tl, am
// Elle sait (1) reconnaître une demande dans une question en français, (2) calculer la densité calendaire
// et les intervalles HPD à partir de paramètres JSON, (3) se décrire en une ligne.
// Les formules reprennent celles des plugins de ChronoModel (CNRS, GPL-3.0-or-later).
//
// Paramètres JSON (clé "method" = identifiant de la méthode) :
//   c14   {age, error, curve?, deltaR?, deltaRerr?}      âge conventionnel en BP
//   f14c  {f14c, error, curve?}
//   gauss {age, error, mode: "none"|"eq", a?, b?, c?}   année calendaire signée
//   unif  {min, max}                                    années calendaires signées
//   tl    {age, error, refYear}                          âge en années avant refYear
//   am    {process: "I"|"D"|"F"|"ID"|"IF"|"IDF", inc?, dec?, alpha95?, field?, errorF?,
//          curveI?, curveD?, curveF?, steps?}
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "Density.h"

namespace seldon {

struct MethodResult {
    bool                  ok = false;
    QString               error;        // si !ok : raison, en français
    QStringList           warnings;
    Density               density;
    std::vector<Interval> hpd95, hpd68;
    DensityStats          stats;

    static MethodResult failure(const QString& why);
    // Calcule HPD et statistiques. `checkEdges` : avertit si la densité touche le bord du domaine
    // (courbe de référence trop courte).
    static MethodResult fromDensity(const Density& d, QStringList warnings, bool checkEdges);
};

class Method {
public:
    virtual ~Method() = default;

    virtual QString id() const = 0;           // "c14"
    virtual QString title() const = 0;        // « Radiocarbone (14C) »
    virtual QStringList curveNames() const { return {}; }

    // Reconnaît les demandes de cette méthode dans une question. Chaque objet contient "method": id().
    virtual QVector<QJsonObject> parse(const QString& question) const = 0;

    virtual MethodResult compute(const QJsonObject& params) const = 0;

    // « 1954 ± 50 BP — IntCal20 »
    virtual QString describe(const QJsonObject& params) const = 0;
};

} // namespace seldon
