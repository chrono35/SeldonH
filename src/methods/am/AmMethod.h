// Méthode « AM » : datation archéomagnétique (plugin_am de ChronoModel).
// Inclinaison (I), déclinaison (D), intensité (F) ou leurs combinaisons ID, IF, IDF, comparées aux courbes
// de variation séculaire. Pour les combinaisons, intégration sur la variance supplémentaire σ² (steps pas).
//
// Paramètres : process, inc, dec, alpha95, field, errorF, curveI, curveD, curveF, steps (défaut 500).
#pragma once

#include "../common/Method.h"

namespace seldon {

class AmMethod : public Method {
public:
    QString id() const override { return QStringLiteral("am"); }
    QString title() const override { return QStringLiteral("Archéomagnétisme"); }
    QStringList curveNames() const override;
    QVector<QJsonObject> parse(const QString& question) const override;
    MethodResult compute(const QJsonObject& p) const override;
    QString describe(const QJsonObject& p) const override;
};

} // namespace seldon
