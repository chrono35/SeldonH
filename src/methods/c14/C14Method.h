// Méthode « 14C » : calibration d'un âge radiocarbone conventionnel (BP) avec une courbe IntCal/SHCal/Marine.
// Formule de ChronoModel (plugin_14C) :
//   L(t) = exp( -½ (a - ΔR - μ(t))² / v ) / √v ,   v = σ_courbe(t)² + σ_mesure² + σ_ΔR²
#pragma once

#include <QRegularExpression>

#include "../common/Method.h"

namespace seldon {

class C14Method : public Method {
public:
    QString id() const override { return QStringLiteral("c14"); }
    QString title() const override { return QStringLiteral("Radiocarbone (14C)"); }
    QStringList curveNames() const override;
    QVector<QJsonObject> parse(const QString& question) const override;
    QRegularExpression context() const override;
    bool usesValueErrors() const override { return true; }
    MethodResult compute(const QJsonObject& p) const override;
    QString describe(const QJsonObject& p) const override;

    static QString defaultCurve(const QString& foldedQuestion);
    static QString curveTitle(const QString& name);       // « intcal20 » -> « IntCal20 »
};

} // namespace seldon
