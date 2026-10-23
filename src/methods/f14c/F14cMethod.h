// Méthode « F14C » : calibration d'une fraction moderne (F14C), surtout pour les échantillons post-bombe
// (courbes de Hua et al. 2013/2021) ; les courbes d'âge IntCal/SHCal/Marine sont converties en F14C.
// Formule de ChronoModel (plugin_F14C) :  L(t) = exp( -½ (F - μ(t))² / v ) / √v ,  v = σ_courbe² + σ_mesure²
#pragma once

#include <QRegularExpression>

#include "../common/Method.h"
#include "../common/RefCurve.h"

namespace seldon {

class F14cMethod : public Method {
public:
    QString id() const override { return QStringLiteral("f14c"); }
    QString title() const override { return QStringLiteral("Fraction moderne (F14C)"); }
    QStringList curveNames() const override;
    QVector<QJsonObject> parse(const QString& question) const override;
    QRegularExpression context() const override;
    bool usesValueErrors() const override { return true; }
    MethodResult compute(const QJsonObject& p) const override;
    QString describe(const QJsonObject& p) const override;
    QStringList referenceCurves(const QJsonObject& p) const override;

    // Résout un nom de courbe : dossier f14c d'abord, puis courbes d'âge de c14 (converties).
    static const RefCurve* resolveCurve(const QString& name);
};

} // namespace seldon
