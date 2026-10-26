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

// Formats d'affichage des dates, comme dans ChronoModel (DateUtils : convertToFormat, dateFormatToString).
// Les calculs restent en années calendaires astronomiques signées (BC/AD natif de ChronoModel) ; seul l'affichage change.
//
//   BC/AD, BCE/CE   année signée (négatif = avant J.-C.)              (inchangée)
//   Age cal. BP     1950 − année          Age cal. B2K   2000 − année
//   Date cal. BP    année − 1950          Date cal. B2K  année − 2000
//   Age ka          −année / 1000         Age Ma         −année / 1 000 000
#pragma once

#include <QRegularExpression>
#include <QString>

#include <cmath>
#include <string>

#include "TextUtil.h"

namespace seldon {

enum class DateFormat { BcAd, BceCe, CalBP, CalB2K, DatBP, DatB2K, Ka, Ma };

namespace dateformat {

inline const char* id(DateFormat f)
{
    switch (f) {
    case DateFormat::BcAd:   return "bcad";
    case DateFormat::BceCe:  return "bcece";
    case DateFormat::CalBP:  return "calbp";
    case DateFormat::CalB2K: return "calb2k";
    case DateFormat::DatBP:  return "datbp";
    case DateFormat::DatB2K: return "datb2k";
    case DateFormat::Ka:     return "ka";
    case DateFormat::Ma:     return "ma";
    }
    return "bcad";
}

inline DateFormat fromId(const QString& s)
{
    for (DateFormat f : { DateFormat::BcAd, DateFormat::BceCe, DateFormat::CalBP, DateFormat::CalB2K,
                          DateFormat::DatBP, DateFormat::DatB2K, DateFormat::Ka, DateFormat::Ma })
        if (s == QLatin1String(id(f))) return f;
    return DateFormat::BcAd;
}

// Nom affiché (menus, titres d'axe).
inline QString title(DateFormat f)
{
    switch (f) {
    case DateFormat::BcAd:   return QStringLiteral("BC/AD (années signées, av./apr. J.-C.)");
    case DateFormat::BceCe:  return QStringLiteral("BCE/CE (années signées)");
    case DateFormat::CalBP:  return QStringLiteral("Âge cal. BP (1950 − année)");
    case DateFormat::CalB2K: return QStringLiteral("Âge cal. B2K (2000 − année)");
    case DateFormat::DatBP:  return QStringLiteral("Date cal. BP (année − 1950)");
    case DateFormat::DatB2K: return QStringLiteral("Date cal. B2K (année − 2000)");
    case DateFormat::Ka:     return QStringLiteral("Âge ka (milliers d'années)");
    case DateFormat::Ma:     return QStringLiteral("Âge Ma (millions d'années)");
    }
    return {};
}

// Libellé court de l'axe horizontal d'un graphique.
inline QString axisLabel(DateFormat f)
{
    switch (f) {
    case DateFormat::BcAd:   return QStringLiteral("Année calendaire (négatif = avant J.-C.)");
    case DateFormat::BceCe:  return QStringLiteral("Année calendaire BCE/CE (négatif = BCE)");
    case DateFormat::CalBP:  return QStringLiteral("Âge cal. BP");
    case DateFormat::CalB2K: return QStringLiteral("Âge cal. B2K");
    case DateFormat::DatBP:  return QStringLiteral("Date cal. BP (année − 1950)");
    case DateFormat::DatB2K: return QStringLiteral("Date cal. B2K (année − 2000)");
    case DateFormat::Ka:     return QStringLiteral("Âge (ka)");
    case DateFormat::Ma:     return QStringLiteral("Âge (Ma)");
    }
    return {};
}

// Les formats « âge » croissent vers le passé : l'axe des graphiques est inversé pour garder l'ordre chronologique.
inline bool isAge(DateFormat f)
{
    return f == DateFormat::CalBP || f == DateFormat::CalB2K || f == DateFormat::Ka || f == DateFormat::Ma;
}

inline double convert(double year, DateFormat f)
{
    switch (f) {
    case DateFormat::CalBP:  return 1950.0 - year;
    case DateFormat::CalB2K: return 2000.0 - year;
    case DateFormat::DatBP:  return year - 1950.0;
    case DateFormat::DatB2K: return year - 2000.0;
    case DateFormat::Ka:     return -year / 1e3;
    case DateFormat::Ma:     return -year / 1e6;
    default:                 return year;
    }
}

// Inverse de convert : valeur exprimée dans le format f -> année calendaire signée.
inline double toCalendar(double v, DateFormat f)
{
    switch (f) {
    case DateFormat::CalBP:  return 1950.0 - v;
    case DateFormat::CalB2K: return 2000.0 - v;
    case DateFormat::DatBP:  return v + 1950.0;
    case DateFormat::DatB2K: return v + 2000.0;
    case DateFormat::Ka:     return -v * 1e3;
    case DateFormat::Ma:     return -v * 1e6;
    default:                 return v;
    }
}

// Valeur affichée avec son unité, ex. « 3848 cal BP », « 1899 av. J.-C. », « 1,898 ka ».
inline QString format(double year, DateFormat f)
{
    const double v = convert(year, f);
    switch (f) {
    case DateFormat::BcAd:   return text::yearLabel(year);
    case DateFormat::BceCe: {
        const long r = std::lround(year);
        return r >= 1 ? QStringLiteral("%1 CE").arg(r) : QStringLiteral("%1 BCE").arg(1 - r);
    }
    case DateFormat::CalBP:  return QStringLiteral("%1 cal BP").arg(std::lround(v));
    case DateFormat::CalB2K: return QStringLiteral("%1 cal B2K").arg(std::lround(v));
    case DateFormat::DatBP:  return QStringLiteral("%1 (date cal. BP)").arg(std::lround(v));
    case DateFormat::DatB2K: return QStringLiteral("%1 (date cal. B2K)").arg(std::lround(v));
    case DateFormat::Ka:     return QStringLiteral("%1 ka").arg(text::num(v, 3));
    case DateFormat::Ma:     return QStringLiteral("%1 Ma").arg(text::num(v, 5));
    }
    return {};
}

// Écart-type d'une densité (en années) exprimé dans l'unité du format.
inline QString spread(double sdYears, DateFormat f)
{
    if (f == DateFormat::Ka) return QStringLiteral("%1 ka").arg(text::num(sdYears / 1e3, 3));
    if (f == DateFormat::Ma) return QStringLiteral("%1 Ma").arg(text::num(sdYears / 1e6, 5));
    return QStringLiteral("%1 ans").arg(text::num(sdYears, 1));
}

// Format demandé dans une question (« donne le résultat en cal BP », « en B2K », « en ka »…) ; `fallback` sinon.
// « 3560 ± 20 BP » (âge d'entrée) n'est pas une demande de format : « en BP » ne compte que près d'un verbe de sortie.
inline DateFormat detect(const QString& question, DateFormat fallback)
{
    const QString q = text::fold(question);
    auto has = [&](const char* re) { return QRegularExpression(QString::fromLatin1(re)).match(q).hasMatch(); };

    const char* outVerb = "(?:resultats?|donne\\w*|exprim\\w*|affich\\w*|present\\w*|sortie|intervalles?|densites?|dates?|ages?|annees?)";
    // Anglais : « give the results in ka », « express the intervals in BP ».
    const char* outVerbEn = "(?:results?|give\\w*|express\\w*|display\\w*|show\\w*|output|intervals?|densit\\w*|dates?|ages?|years?)";
    auto after = [&](const QString& unit) {
        return QRegularExpression(QStringLiteral("\\b%1\\b[^.\\n\\d±]{0,60}\\ben\\s+(?:annees\\s+|age\\s+|date\\s+)?%2\\b")
                                      .arg(QString::fromLatin1(outVerb), unit)).match(q).hasMatch()
            || QRegularExpression(QStringLiteral("\\b%1\\b[^.\\n\\d±]{0,60}\\bin\\s+(?:years\\s+|age\\s+)?%2\\b")
                                      .arg(QString::fromLatin1(outVerbEn), unit)).match(q).hasMatch();
    };

    if (has(R"(\bdate\s+cal\.?\s*b2k\b|\bb2k\s+date\b)")) return DateFormat::DatB2K;
    if (has(R"(\bdate\s+cal\.?\s*bp\b)"))                 return DateFormat::DatBP;
    if (has(R"(\bcal\.?\s*b2k\b|\bb2k\b)"))              return DateFormat::CalB2K;
    if (has(R"(\bcal\.?\s*bp\b|\bbp\s+cal\w*\b|\bage\s+cal)")) return DateFormat::CalBP;
    if (has(R"(\bbce\b)"))                               return DateFormat::BceCe;
    if (has(R"(\bbc/ad\b|\ben\s+annees\s+calendaires\b|\bin\s+calendar\s+years\b|\bav\.?\s*j\.?-?c\b.*\bapr\.?\s*j)")) return DateFormat::BcAd;
    if (after(QStringLiteral("bp")))                     return DateFormat::CalBP;
    if (after(QStringLiteral("ka")))                     return DateFormat::Ka;
    if (after(QStringLiteral("ma")))                     return DateFormat::Ma;
    return fallback;
}

// Rappel pour le message système quand l'utilisateur a choisi un format autre que BC/AD.
inline std::string systemHint(DateFormat f)
{
    if (f == DateFormat::BcAd) return {};
    return std::string("\n\n## Format des dates\nL'utilisateur a choisi le format « ") + title(f).toStdString() +
           "». Exprime les dates dans ce format, en reprenant les valeurs des blocs de résultat de SeldonH (déjà converties). "
           "Si tu dois convertir toi-même une année calendaire signée A : "
           "cal BP = 1950 − A ; cal B2K = 2000 − A ; ka = −A / 1000.";
}

} // namespace dateformat
} // namespace seldon
