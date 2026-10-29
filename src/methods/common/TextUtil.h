/* ---------------------------------------------------------------------

Copyright © CNRS	2014 - 2026

Authors :
	Philippe LANOS
	Helori LANOS
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

This file contains dating methods derived from ChronoModel
(https://chronomodel.com), distributed under the same license.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

SPDX-License-Identifier: GPL-3.0-or-later
--------------------------------------------------------------------- */

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

// Année astronomique signée, à l'année près : « -499 », « 1200 » (affichage par défaut de SeldonH et de ChronoModel).
inline QString yearSigned(double signedYear)
{
    return QString::number(std::lround(signedYear));
}

// Année historique (format « av./apr. J.-C. », sur demande) : « 500 av. J.-C. » pour -499, « 1200 apr. J.-C. ».
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

// ------------------------------------------------------------------ années av. J.-C. et année 0
// SeldonH, ChronoModel et les courbes de calibration (IntCal…) comptent en années ASTRONOMIQUES : l'année 0 existe,
// 0 = 1 av. J.-C., −1 = 2 av. J.-C., −499 = 500 av. J.-C. Une année écrite « N av. J.-C. » (ou N BC, BCE, AEC) vaut
// donc 1 − N ; « N apr. J.-C. » (AD, CE) vaut N. L'affichage fait la conversion inverse (yearLabel).

// Ères écrites après (ou avant, pour AD) un nombre. BC/BCE/AEC et les formes françaises sans casse ; AD et CE en
// majuscules seulement (« ce » est un mot courant).
inline const QString& eraBc()
{
    static const QString re = QStringLiteral(
        R"((?:(?i)av\.?\s*j\.?\s*-?\s*c\.?|avant\s+j\.?\s*-?\s*c\.?|avant\s+(?:notre|l')\s*[eè]re(?:\s+commune)?|av\.?\s*n\.?\s*[eè]\.?|\bbce?\b|\baec\b))");
    return re;
}
inline const QString& eraAd()
{
    static const QString re = QStringLiteral(
        R"((?:(?i:apr\.?\s*j\.?\s*-?\s*c\.?|apr[eè]s\s+j\.?\s*-?\s*c\.?|de\s+notre\s+[eè]re)|\bAD\b|\bCE\b))");
    return re;
}

// « 500 av. J.-C. », « 500 BC », « AD 1200 », « 1200 apr. J.-C. », « -499 », « 2,5 » -> année astronomique.
// NaN si le texte n'est pas une année seule.
inline double yearFromText(QString s)
{
    s = s.trimmed();
    s.remove(QChar(0x00A0)).remove(QChar(0x202F));
    const QString num = QStringLiteral(R"(([-−]?\d+(?:[.,]\d+)?))");
    static const QRegularExpression plain(QStringLiteral("^") + num + QStringLiteral("$"));
    static const QRegularExpression bc(QStringLiteral("^") + num + QStringLiteral(R"(\s*)") + eraBc() + QStringLiteral(R"(\s*$)"));
    static const QRegularExpression adAfter(QStringLiteral("^") + num + QStringLiteral(R"(\s*)") + eraAd() + QStringLiteral(R"(\s*$)"));
    static const QRegularExpression adBefore(QStringLiteral(R"(^(?:AD|ad)\s*)") + num + QStringLiteral("$"));
    if (const auto m = plain.match(s); m.hasMatch()) return toNumber(m.captured(1));
    if (const auto m = bc.match(s); m.hasMatch()) return 1.0 - std::abs(toNumber(m.captured(1)));
    if (const auto m = adAfter.match(s); m.hasMatch()) return toNumber(m.captured(1));
    if (const auto m = adBefore.match(s); m.hasMatch()) return toNumber(m.captured(1));
    return std::nan("");
}

// Texte où chaque année écrite avec son ère devient l'année astronomique signée, avant la lecture des dates :
//   « entre 500 av. J.-C. et 400 av. J.-C. » -> « entre -499 et -399 » ; « entre 800 et 900 av. J.-C. » -> « entre -799 et -899 » ;
//   « 500 ± 20 av. J.-C. » -> « -499 ± 20 » ; « AD 1200 », « 1200 apr. J.-C. » -> « 1200 ». Les âges BP ne sont pas touchés.
inline QString erasToAstronomical(const QString& in)
{
    auto astro = [](const QString& n) { return QString::number(1.0 - std::abs(toNumber(n)), 'g', 12); };
    const QString num = QStringLiteral(R"((?<![\d.,])(\d+(?:[.,]\d+)?))");
    QString s = in;
    // 1. Intervalle dont seule la seconde borne porte l'ère av. J.-C. : elle vaut pour les deux.
    static const QRegularExpression range(QStringLiteral(R"(((?:entre|between|de|from|du)\s+))") + num
                                          + QStringLiteral(R"((\s*(?:et|à|a|au|-|–|to|and)\s*))") + num + QStringLiteral(R"(\s*)") + eraBc(),
                                          QRegularExpression::CaseInsensitiveOption);
    for (QRegularExpressionMatch m = range.match(s); m.hasMatch(); m = range.match(s))
        s.replace(m.capturedStart(), m.capturedLength(), m.captured(1) + astro(m.captured(2)) + m.captured(3) + astro(m.captured(4)));
    // 2. « N ± E av. J.-C. »
    static const QRegularExpression withError(num + QStringLiteral(R"((\s*(?:±|\+\s*/?\s*-)\s*\d+(?:[.,]\d+)?\s*(?:ans\s+)?))") + eraBc());
    for (QRegularExpressionMatch m = withError.match(s); m.hasMatch(); m = withError.match(s))
        s.replace(m.capturedStart(), m.capturedLength(), astro(m.captured(1)) + QLatin1Char(' ') + m.captured(2).trimmed() + QLatin1Char(' '));
    // 3. « N av. J.-C. »
    static const QRegularExpression bc(num + QStringLiteral(R"(\s*)") + eraBc());
    for (QRegularExpressionMatch m = bc.match(s); m.hasMatch(); m = bc.match(s))
        s.replace(m.capturedStart(), m.capturedLength(), astro(m.captured(1)));
    // 4. « N apr. J.-C. », « AD N » : l'ère est retirée.
    static const QRegularExpression adAfter(num + QStringLiteral(R"(\s*)") + eraAd());
    for (QRegularExpressionMatch m = adAfter.match(s); m.hasMatch(); m = adAfter.match(s))
        s.replace(m.capturedStart(), m.capturedLength(), m.captured(1));
    static const QRegularExpression adBefore(QStringLiteral(R"(\bAD\s*)") + num);
    for (QRegularExpressionMatch m = adBefore.match(s); m.hasMatch(); m = adBefore.match(s))
        s.replace(m.capturedStart(), m.capturedLength(), m.captured(1));
    return s;
}

} // namespace text
} // namespace seldon
