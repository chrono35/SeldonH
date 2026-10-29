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
//   Sans unité      valeur signée brute, sans suffixe (données qui ne sont pas des dates : profondeur, indice…)
//
// Décimales : réglage global (Préférences), fixe (0 à 6) ou automatique. En automatique, la précision suit
// l'incertitude du résultat affiché (PrecisionScope) : 2 chiffres significatifs sur l'écart-type, dans l'unité.
#pragma once

#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <string>

#include "TextUtil.h"

namespace seldon {

// BcAd : années astronomiques signées (l'année 0 existe), format de ChronoModel et des .chr, par défaut.
// Historic : les mêmes années écrites av./apr. J.-C. (sans année 0 : -499 = 500 av. J.-C.), sur demande.
enum class DateFormat { BcAd, Historic, BceCe, CalBP, CalB2K, DatBP, DatB2K, Ka, Ma, Numeric };

namespace dateformat {

inline const char* id(DateFormat f)
{
    switch (f) {
    case DateFormat::BcAd:   return "bcad";
    case DateFormat::Historic: return "historic";
    case DateFormat::BceCe:  return "bcece";
    case DateFormat::CalBP:  return "calbp";
    case DateFormat::CalB2K: return "calb2k";
    case DateFormat::DatBP:  return "datbp";
    case DateFormat::DatB2K: return "datb2k";
    case DateFormat::Ka:     return "ka";
    case DateFormat::Ma:     return "ma";
    case DateFormat::Numeric: return "numeric";
    }
    return "bcad";
}

// Tous les formats, dans l'ordre des menus.
inline const std::array<DateFormat, 10>& all()
{
    static const std::array<DateFormat, 10> l = { DateFormat::BcAd, DateFormat::Historic, DateFormat::BceCe, DateFormat::CalBP, DateFormat::CalB2K,
                                                         DateFormat::DatBP, DateFormat::DatB2K, DateFormat::Ka, DateFormat::Ma,
                                                         DateFormat::Numeric };
    return l;
}

inline DateFormat fromId(const QString& s)
{
    for (DateFormat f : all())
        if (s == QLatin1String(id(f))) return f;
    return DateFormat::BcAd;
}

// Nom affiché (menus, titres d'axe).
inline QString title(DateFormat f)
{
    switch (f) {
    case DateFormat::BcAd:   return QStringLiteral("BC/AD (années astronomiques signées, comme ChronoModel : 0 = 1 av. J.-C.)");
    case DateFormat::Historic: return QStringLiteral("Années historiques (av./apr. J.-C., sans année 0)");
    case DateFormat::BceCe:  return QStringLiteral("BCE/CE (années signées)");
    case DateFormat::CalBP:  return QStringLiteral("Âge cal. BP (1950 − année)");
    case DateFormat::CalB2K: return QStringLiteral("Âge cal. B2K (2000 − année)");
    case DateFormat::DatBP:  return QStringLiteral("Date cal. BP (année − 1950)");
    case DateFormat::DatB2K: return QStringLiteral("Date cal. B2K (année − 2000)");
    case DateFormat::Ka:     return QStringLiteral("Âge ka (milliers d'années)");
    case DateFormat::Ma:     return QStringLiteral("Âge Ma (millions d'années)");
    case DateFormat::Numeric: return QStringLiteral("Sans unité (valeurs signées, sans suffixe)");
    }
    return {};
}

// Libellé court de l'axe horizontal d'un graphique.
inline QString axisLabel(DateFormat f)
{
    switch (f) {
    case DateFormat::BcAd:   return QStringLiteral("Année calendaire (astronomique, 0 = 1 av. J.-C.)");
    case DateFormat::Historic: return QStringLiteral("Année calendaire (négatif = avant J.-C.)");
    case DateFormat::BceCe:  return QStringLiteral("Année calendaire BCE/CE (négatif = BCE)");
    case DateFormat::CalBP:  return QStringLiteral("Âge cal. BP");
    case DateFormat::CalB2K: return QStringLiteral("Âge cal. B2K");
    case DateFormat::DatBP:  return QStringLiteral("Date cal. BP (année − 1950)");
    case DateFormat::DatB2K: return QStringLiteral("Date cal. B2K (année − 2000)");
    case DateFormat::Ka:     return QStringLiteral("Âge (ka)");
    case DateFormat::Ma:     return QStringLiteral("Âge (Ma)");
    case DateFormat::Numeric: return QStringLiteral("Valeur");
    }
    return {};
}

// Les formats « âge » croissent vers le passé : l'axe des graphiques est inversé pour garder l'ordre chronologique.
inline bool isAge(DateFormat f)
{
    return f == DateFormat::CalBP || f == DateFormat::CalB2K || f == DateFormat::Ka || f == DateFormat::Ma;
}

// ------------------------------------------------------------------ décimales

constexpr int kAutoDecimals = -1;      // réglage « automatique »
constexpr int kMaxDecimals  = 6;

// Réglage global (Préférences) : kAutoDecimals ou 0..kMaxDecimals.
inline std::atomic<int>& decimalsSetting() { static std::atomic<int> d{ kAutoDecimals }; return d; }
inline void setDecimals(int d) { decimalsSetting() = d < 0 ? kAutoDecimals : std::min(d, kMaxDecimals); }

// Années par unité affichée.
inline double unitYears(DateFormat f) { return f == DateFormat::Ka ? 1e3 : f == DateFormat::Ma ? 1e6 : 1.0; }

// Écart-type (années) du résultat en cours d'affichage, pour le mode automatique ; NaN = inconnu.
inline double& precisionContext() { thread_local double sd = std::numeric_limits<double>::quiet_NaN(); return sd; }

// Fixe le contexte de précision le temps d'un bloc (fiche d'une datation, d'un événement…).
class PrecisionScope {
public:
    explicit PrecisionScope(double sdYears) : saved_(precisionContext()) { precisionContext() = sdYears; }
    ~PrecisionScope() { precisionContext() = saved_; }
    PrecisionScope(const PrecisionScope&) = delete;
    PrecisionScope& operator=(const PrecisionScope&) = delete;
private:
    double saved_;
};

// Décimales nécessaires pour 2 chiffres significatifs sur une incertitude (années), dans l'unité du format.
inline int decimalsForSpread(double sdYears, DateFormat f)
{
    const double u = sdYears / unitYears(f);
    if (!std::isfinite(u) || !(u > 0.0)) return -1;
    return std::clamp(1 - int(std::floor(std::log10(u))), 0, kMaxDecimals);
}

// Décimales à utiliser pour une date dans le format f : réglage fixe, sinon automatique.
inline int decimals(DateFormat f)
{
    if (const int d = decimalsSetting(); d >= 0) return d;
    if (const int d = decimalsForSpread(precisionContext(), f); d >= 0) return d;
    return f == DateFormat::Ka ? 3 : f == DateFormat::Ma ? 5 : 0;      // sans contexte : à l'année près
}

// Nombre à d décimales, virgule décimale, sans « -0 ».
inline QString number(double v, int d)
{
    const double p = std::pow(10.0, d);
    double r = std::round(v * p) / p;
    if (r == 0.0) r = 0.0;
    return text::num(r, d);
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

// Valeur affichée avec son unité, ex. « 3848 cal BP », « 1899 av. J.-C. », « 1,898 ka », « -1898 » (sans unité).
// Décimales : decimals(f) (Préférences, ou automatique selon le PrecisionScope en cours).
inline QString format(double year, DateFormat f)
{
    const int d = decimals(f);
    const double v = convert(year, f);
    switch (f) {
    case DateFormat::BcAd:                       // année astronomique signée, comme ChronoModel et les .chr
        return number(year, d);
    case DateFormat::Historic: {                 // année astronomique 0 = 1 av. J.-C.
        if (d == 0) return text::yearLabel(year);
        return year >= 0.5 ? QStringLiteral("%1 apr. J.-C.").arg(number(year, d)) : QStringLiteral("%1 av. J.-C.").arg(number(1.0 - year, d));
    }
    case DateFormat::BceCe:
        return year >= 0.5 ? QStringLiteral("%1 CE").arg(number(year, d)) : QStringLiteral("%1 BCE").arg(number(1.0 - year, d));
    case DateFormat::CalBP:   return QStringLiteral("%1 cal BP").arg(number(v, d));
    case DateFormat::CalB2K:  return QStringLiteral("%1 cal B2K").arg(number(v, d));
    case DateFormat::DatBP:   return QStringLiteral("%1 (date cal. BP)").arg(number(v, d));
    case DateFormat::DatB2K:  return QStringLiteral("%1 (date cal. B2K)").arg(number(v, d));
    case DateFormat::Ka:      return QStringLiteral("%1 ka").arg(number(v, d));
    case DateFormat::Ma:      return QStringLiteral("%1 Ma").arg(number(v, d));
    case DateFormat::Numeric: return number(v, d);
    }
    return {};
}

// Écart-type d'une densité (en années) exprimé dans l'unité du format. En automatique : 2 chiffres significatifs.
inline QString spread(double sdYears, DateFormat f)
{
    int d = decimalsSetting();
    if (d < 0) d = decimalsForSpread(sdYears, f);
    if (d < 0) d = f == DateFormat::Ka ? 3 : f == DateFormat::Ma ? 5 : 1;
    const QString n = number(sdYears / unitYears(f), d);
    if (f == DateFormat::Ka) return n + QStringLiteral(" ka");
    if (f == DateFormat::Ma) return n + QStringLiteral(" Ma");
    if (f == DateFormat::Numeric) return n;
    return n + QStringLiteral(" ans");
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

    if (has(R"(\bsans\s+unites?\b|\bwithout\s+units?\b|\bno\s+units?\b|\bunitless\b)")) return DateFormat::Numeric;
    if (has(R"(\bdate\s+cal\.?\s*b2k\b|\bb2k\s+date\b)")) return DateFormat::DatB2K;
    if (has(R"(\bdate\s+cal\.?\s*bp\b)"))                 return DateFormat::DatBP;
    if (has(R"(\bcal\.?\s*b2k\b|\bb2k\b)"))              return DateFormat::CalB2K;
    if (has(R"(\bcal\.?\s*bp\b|\bbp\s+cal\w*\b|\bage\s+cal)")) return DateFormat::CalBP;
    if (has(R"(\bbce\b)"))                               return DateFormat::BceCe;
    // Années historiques demandées : « donne les dates en av. J.-C. », « en années historiques », « in BC/AD years (historical) ».
    if (has(R"(\bannees?\s+historiques?\b|\bnumerotation\s+historique\b|\bhistorical\s+years?\b|\bav\.?\s*j\.?-?c\b.*\bapr\.?\s*j)")
        || after(QStringLiteral(R"((?:av\.?\s*j\.?\s*-?\s*c|avant\s+j\.?\s*-?\s*c))")))
        return DateFormat::Historic;
    if (has(R"(\bbc/ad\b|\ben\s+annees\s+calendaires\b|\bin\s+calendar\s+years\b|\bannees?\s+astronomiques?\b)")) return DateFormat::BcAd;
    if (after(QStringLiteral("bp")))                     return DateFormat::CalBP;
    if (after(QStringLiteral("ka")))                     return DateFormat::Ka;
    if (after(QStringLiteral("ma")))                     return DateFormat::Ma;
    return fallback;
}

// Rappel pour le message système : format des dates choisi par l'utilisateur.
inline std::string systemHint(DateFormat f)
{
    const int d = decimalsSetting();
    std::string h = "\n\n## Format des dates\n";
    if (f == DateFormat::BcAd)
        h += "Les années sont des années astronomiques signées, comme dans ChronoModel et les fichiers .chr : l'année 0 "
             "existe (0 = 1 av. J.-C., -499 = 500 av. J.-C.). Écris-les ainsi, signées (« -499 », « 1200 »), en reprenant "
             "les valeurs des blocs de résultat de SeldonH ; ne les convertis PAS en « av. J.-C. » sauf si l'utilisateur le demande.";
    else if (f == DateFormat::Historic)
        h += "L'utilisateur a choisi les années historiques : les blocs de résultat de SeldonH les donnent déjà en "
             "« av. J.-C. » / « apr. J.-C. » (sans année 0) ; reprends-les telles quelles.";
    else if (f == DateFormat::Numeric)
        h += "L'utilisateur a choisi le mode « sans unité » : donne les valeurs signées telles quelles, sans unité ni "
             "mention av./apr. J.-C., BP ou ka, en reprenant les valeurs des blocs de résultat de SeldonH.";
    else
        h += std::string("L'utilisateur a choisi le format « ") + title(f).toStdString() +
             "». Exprime les dates dans ce format, en reprenant les valeurs des blocs de résultat de SeldonH (déjà converties). "
             "Si tu dois convertir toi-même une année calendaire signée A : "
             "cal BP = 1950 − A ; cal B2K = 2000 − A ; ka = −A / 1000.";
    if (d >= 0) h += std::string(" Garde ") + std::to_string(d) +
                     (d > 1 ? " décimales" : " décimale") + " pour les valeurs, comme dans les blocs de résultat.";
    return h;
}

} // namespace dateformat
} // namespace seldon
