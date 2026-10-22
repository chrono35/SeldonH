// Petits utilitaires de texte partagés par les méthodes (Qt).
#pragma once

#include <QRegularExpression>
#include <QVector>
#include <cmath>
#include <QString>

namespace seldon {
namespace text {

// minuscules, sans accents
inline QString fold(QString s)
{
    s = s.toLower().normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(s.size());
    for (const QChar c : s)
        if (c.category() != QChar::Mark_NonSpacing) out += c;
    return out;
}

// « 1,5 » -> 1.5
inline double toNumber(QString s)
{
    s.replace(',', '.').replace(QChar(0x2212), '-');
    return s.toDouble();
}

// Format français : 1 234,5
inline QString num(double v, int decimals = 0)
{
    return QString::number(v, 'f', decimals).replace('.', ',');
}

// 3 décimales significatives max, sans zéros inutiles (pour F14C, intensités...)
inline QString numAuto(double v, int maxDecimals = 4)
{
    QString s = QString::number(v, 'f', maxDecimals);
    if (s.contains('.')) {
        while (s.endsWith('0')) s.chop(1);
        if (s.endsWith('.')) s.chop(1);
    }
    return s.replace('.', ',');
}

// Signe ±, +/-, +-, -/+ (sur du texte déjà passé par fold())
inline const QString& plusMinus()
{
    static const QString pm = QStringLiteral("(?:±|\\+\\s*/\\s*-|\\+\\s*-|\\+/−|-\\s*/\\s*\\+)");
    return pm;
}

inline QString yearLabel(double signedYear)
{
    const long v = std::lround(signedYear);
    if (v >= 1) return QString("%1 apr. J.-C.").arg(v);
    return QString("%1 av. J.-C.").arg(1 - v);   // année astronomique 0 = 1 av. J.-C.
}

struct ValueError {
    double value = 0, error = 0;
    int start = 0, length = 0;
};

// Toutes les paires « valeur ± erreur » d'un texte (déjà passé par fold()).
inline QVector<ValueError> findValueErrors(const QString& folded)
{
    static const QRegularExpression re(
        QStringLiteral("(?<![\\d.,])(-?\\d{1,7}(?:[.,]\\d+)?)\\s*") + plusMinus() + QStringLiteral("\\s*(\\d{1,6}(?:[.,]\\d+)?)"));
    QVector<ValueError> out;
    auto it = re.globalMatch(folded);
    while (it.hasNext()) {
        const auto m = it.next();
        ValueError v;
        v.value = toNumber(m.captured(1));
        v.error = toNumber(m.captured(2));
        v.start = m.capturedStart();
        v.length = m.capturedLength();
        if (v.error > 0) out << v;
    }
    return out;
}

} // namespace text
} // namespace seldon
