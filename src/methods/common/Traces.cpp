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

#include "Traces.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringView>

#include <cmath>
#include <limits>

#include "DateFormat.h"
#include "TextUtil.h"

namespace seldon {
namespace traces {

namespace {

QVector<TraceSet>& store()
{
    static QVector<TraceSet> s;
    return s;
}

// Mise en forme du CSV. ChronoModel règle séparément le séparateur de colonnes (AppSettings::mCSVCellSeparator) et la
// décimale (mCSVDecSeparator) : anglais « , » et « . », français « ; » et « , », mais toute combinaison est possible,
// y compris « , » et « , » (chaque valeur décimale est alors coupée en deux cellules).
struct CsvLayout {
    QChar sep = ',';
    bool  decimalComma = false;
    bool  commaPairs = false;      // séparateur et décimale « , » : partie entière et décimale dans deux cellules
};

QString unquote(QString s);

// Séparateur dont la première cellule de l'en-tête est « iter ».
bool headerSeparator(const QString& header, QChar sep)
{
    const QStringList h = header.split(sep);
    return h.size() >= 2 && unquote(h.first()).compare(QLatin1String("iter"), Qt::CaseInsensitive) == 0;
}

// Repère séparateur et décimale sur l'en-tête et les premières lignes de données : le bon séparateur donne à chaque
// ligne autant de cellules que l'en-tête ; la décimale est celle qu'on trouve dans les valeurs.
bool detectLayout(const QString& header, const QStringList& sample, CsvLayout& L, QString& why)
{
    for (const QChar sep : { QChar('\t'), QChar(';'), QChar(',') }) {
        if (!headerSeparator(header, sep)) continue;
        const qsizetype nh = header.split(sep).size();
        bool same = true, pairs = sep == QLatin1Char(',');
        bool comma = false, point = false;
        for (const QString& l : sample) {
            const QStringList cells = l.split(sep);
            if (cells.size() != nh) same = false;
            if (cells.size() != 2 * nh - 1) pairs = false;
            for (qsizetype i = 1; i < cells.size(); ++i) {
                if (cells[i].contains(',')) comma = true;
                if (cells[i].contains('.')) point = true;
            }
        }
        if (same) {
            if (comma && point) { why = QStringLiteral("décimales mêlées « , » et « . »"); return false; }
            L.sep = sep;
            L.decimalComma = comma;
            return true;
        }
        if (pairs && !point) {
            L.sep = sep;
            L.decimalComma = true;
            L.commaPairs = true;
            return true;
        }
        why = QStringLiteral("les lignes n'ont pas le même nombre de colonnes que l'en-tête (séparateur « %1 »)")
                  .arg(sep == QLatin1Char('\t') ? QStringLiteral("tabulation") : QString(sep));
        return false;
    }
    why = QStringLiteral("première colonne « iter » absente : ce n'est pas un export de chaînes de ChronoModel");
    return false;
}

// Lignes du fichier : ChronoModel (saveCsvTo) les termine par « \r » seul ; « \r\n » et « \n » sont acceptés aussi.
QStringList splitLines(const QString& text)
{
    static const QRegularExpression eol(QStringLiteral("\r\n|\r|\n"));
    return text.split(eol, Qt::SkipEmptyParts);
}

// Première ligne utile : après les commentaires « # ChronoModel 4.0.0 », « # <projet> », « # Date Format : BC/AD ».
QString firstLine(const QString& text)
{
    static const QRegularExpression eol(QStringLiteral("\r\n|\r|\n"));
    qsizetype pos = 0;
    while (pos < text.size()) {
        const QRegularExpressionMatch m = eol.match(text, pos);
        const qsizetype end = m.hasMatch() ? m.capturedStart() : text.size();
        QString l = text.mid(pos, end - pos).trimmed();
        if (l.startsWith(QChar(0xFEFF))) l.remove(0, 1);
        if (!l.isEmpty() && !l.startsWith('#')) return l;
        if (!m.hasMatch()) break;
        pos = m.capturedEnd();
    }
    return QString();
}

// Format de date de la ligne « # Date Format : … » de ChronoModel (DateUtils::dateFormatToString). Faux si inconnu.
bool dateFormatOf(const QString& label, DateFormat& f)
{
    const QString l = text::fold(label).simplified();
    static const QList<QPair<QString, DateFormat>> known = {
        { "bc/ad", DateFormat::BcAd }, { "bce/ce", DateFormat::BceCe }, { "age cal. bp", DateFormat::CalBP },
        { "age cal. b2k", DateFormat::CalB2K }, { "date cal. bp", DateFormat::DatBP }, { "date cal. b2k", DateFormat::DatB2K },
        { "age ka", DateFormat::Ka }, { "age ma", DateFormat::Ma } };
    for (const auto& k : known) if (l == k.first) { f = k.second; return true; }
    return false;
}

double number(QString s, bool decimalComma, bool& ok)
{
    s = s.trimmed();
    if (s.startsWith('"') && s.endsWith('"') && s.size() >= 2) s = s.mid(1, s.size() - 2);
    if (s.isEmpty() || s.compare(QLatin1String("nan"), Qt::CaseInsensitive) == 0) { ok = true; return std::nan(""); }
    if (decimalComma) s.replace(',', '.');
    else if (s.contains(',')) { ok = false; return 0.0; }       // décimale « . » attendue
    return s.toDouble(&ok);
}

QString unquote(QString s)
{
    s = s.trimmed();
    if (s.startsWith('"') && s.endsWith('"') && s.size() >= 2) s = s.mid(1, s.size() - 2);
    return s.trimmed();
}

// Nom de colonne normalisé pour la comparaison : minuscules, sans accents, espaces simples.
QString key(const QString& s)
{
    return text::fold(s).simplified();
}

} // namespace

bool looksLike(const QString& text)
{
    const QString h = firstLine(text);
    return headerSeparator(h, '\t') || headerSeparator(h, ';') || headerSeparator(h, ',');
}

bool parse(const QString& text, const QString& fileName, TraceSet& out, QString* error)
{
    auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    out = TraceSet();
    out.file = fileName;
    QStringList lines = splitLines(text);
    // Commentaires de tête ; « # Date Format : … » dit dans quel format sont les valeurs.
    DateFormat fmt = DateFormat::BcAd;
    while (!lines.isEmpty() && (lines.first().trimmed().isEmpty() || lines.first().trimmed().startsWith('#')
                                || lines.first().trimmed().startsWith(QChar(0xFEFF)))) {
        QString c = lines.takeFirst().trimmed();
        if (c.startsWith(QChar(0xFEFF))) { c.remove(0, 1); if (!c.startsWith('#')) { lines.prepend(c); break; } }
        static const QRegularExpression df(QStringLiteral(R"(^#\s*date\s*format\s*:\s*(.*)$)"), QRegularExpression::CaseInsensitiveOption);
        if (const auto m = df.match(c); m.hasMatch()) {
            out.dateFormat = m.captured(1).trimmed();
            if (!dateFormatOf(out.dateFormat, fmt))
                return fail(QStringLiteral("format de date « %1 » non pris en charge : réexporter les chaînes en BC/AD").arg(out.dateFormat));
        }
    }
    if (lines.size() < 2) return fail(QStringLiteral("fichier de traces vide"));
    // Séparateur et décimale repérés sur les 50 premières lignes de données (export français ou anglais).
    CsvLayout L;
    QString why;
    if (!detectLayout(lines.first(), lines.mid(1, 50), L, why)) return fail(why);
    const QChar sep = L.sep;
    const bool decimalComma = L.decimalComma;
    QStringList head = lines.first().split(sep);
    head.removeFirst();
    for (const QString& h : std::as_const(head)) out.columns << unquote(h);
    out.csvFormat = QStringLiteral("séparateur « %1 », décimale « %2 »")
                        .arg(sep == QLatin1Char('\t') ? QStringLiteral("tabulation") : QString(sep), decimalComma ? QStringLiteral(",") : QStringLiteral("."));
    out.values.assign(size_t(out.columns.size()), {});
    long lastIter = std::numeric_limits<long>::min();
    for (int li = 1; li < lines.size(); ++li) {
        QStringList cells = lines[li].split(sep);
        if (L.commaPairs) {                                  // « , » partout : réunit partie entière et décimale
            if (cells.size() != 2 * out.columns.size() + 1)
                return fail(QStringLiteral("ligne %1 : séparateur et décimale « , » ambigus (une valeur sans décimale ?) : réexporter "
                                           "avec le séparateur « ; »").arg(li + 1));
            QStringList joined{ cells[0] };
            for (int c = 0; c < out.columns.size(); ++c) joined << cells[1 + 2 * c] + QLatin1Char(',') + cells[2 + 2 * c];
            cells = joined;
        }
        if (cells.size() < out.columns.size() + 1) {
            if (lines[li].trimmed().isEmpty()) continue;
            return fail(QStringLiteral("ligne %1 incomplète (%2 colonnes au lieu de %3)").arg(li + 1).arg(cells.size()).arg(out.columns.size() + 1));
        }
        bool ok = false;
        const double it = number(cells[0], decimalComma, ok);
        if (!ok || !std::isfinite(it)) return fail(QStringLiteral("ligne %1 : numéro d'itération illisible « %2 »").arg(li + 1).arg(cells[0]));
        const long iter = std::lround(it);
        // Nouvelle chaîne : le numéro d'itération ne suit pas (saut du burn-in et de l'adaptation de la chaîne suivante).
        if (iter != lastIter + 1) out.chainStarts.push_back(out.values.front().size());
        lastIter = iter;
        for (int c = 0; c < out.columns.size(); ++c) {
            const double v = number(cells[c + 1], decimalComma, ok);
            if (!ok) return fail(QStringLiteral("ligne %1, colonne « %2 » : valeur illisible « %3 »").arg(li + 1).arg(out.columns[c], cells[c + 1]));
            // Valeurs ramenées en années calendaires signées (BC/AD) si l'export est dans un autre format.
            out.values[size_t(c)].push_back(fmt == DateFormat::BcAd || !std::isfinite(v) ? v : dateformat::toCalendar(v, fmt));
        }
    }
    if (out.rows() < 2) return fail(QStringLiteral("moins de deux tirages dans le fichier"));
    if (out.chainStarts.empty()) out.chainStarts.push_back(0);
    return true;
}

QString summary(const TraceSet& s)
{
    QStringList lens;
    for (size_t c = 0; c < s.chainStarts.size(); ++c) {
        const size_t end = c + 1 < s.chainStarts.size() ? s.chainStarts[c + 1] : s.rows();
        lens << QString::number(end - s.chainStarts[c]);
    }
    return QStringLiteral("Traces MCMC de ChronoModel « %1 » (%10) : %2 chaîne%3 (%4 tirages : %5), %6 colonne%7%9 :\n%8\n"
                          "SeldonH garde ces tirages pour la conversation : durée entre deux colonnes, date entre deux colonnes, ou "
                          "densité d'une colonne, avec l'outil analyser_traces (bloc calcul {\"method\":\"traces\",…}). "
                          "Utilise exactement ces noms de colonnes.")
        .arg(s.file).arg(s.chainStarts.size()).arg(s.chainStarts.size() > 1 ? "s" : "").arg(s.rows()).arg(lens.join(" + "))
        .arg(s.columns.size()).arg(s.columns.size() > 1 ? "s" : "").arg(s.columns.join(" ; "))
        .arg(s.dateFormat.isEmpty() || s.dateFormat.compare(QLatin1String("BC/AD"), Qt::CaseInsensitive) == 0
                 ? QString() : QStringLiteral(" (valeurs exportées en %1, converties en années BC/AD)").arg(s.dateFormat))
        .arg(s.csvFormat);
}

void add(const TraceSet& s)
{
    QVector<TraceSet>& st = store();
    for (TraceSet& x : st)
        if (x.file == s.file) { x = s; return; }
    st.push_back(s);
}

void clear() { store().clear(); }

const QVector<TraceSet>& loaded() { return store(); }

bool resolve(const QString& name, int& set, int& column, QString* error, int preferred)
{
    const QVector<TraceSet>& st = store();
    auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    if (st.isEmpty())
        return fail(QStringLiteral("aucune trace chargée : joindre un export de chaînes de ChronoModel (Chain_all_Events.csv, "
                                   "Chain_all_Phases.csv…)"));
    // Formes acceptées pour les bornes de phase : « début de X », « X début », « begin X »… -> « X begin » / « X end ».
    QStringList wanted{ key(name) };
    static const QRegularExpression pre(QStringLiteral(R"(^(?:le |la |the )?(debut|fin|begin|end|start)\s+(?:de\s+(?:la\s+|l')?|du\s+|d'|of\s+(?:the\s+)?)?(.+)$)"));
    static const QRegularExpression post(QStringLiteral(R"(^(.+?)\s+(debut|fin|begin|end|start)$)"));
    auto bound = [](const QString& w) { return (w == "debut" || w == "begin" || w == "start") ? QStringLiteral("begin") : QStringLiteral("end"); };
    QString rest, b;
    if (const auto m = pre.match(wanted.first()); m.hasMatch()) { rest = m.captured(2).trimmed(); b = bound(m.captured(1)); }
    else if (const auto m2 = post.match(wanted.first()); m2.hasMatch()) { rest = m2.captured(1).trimmed(); b = bound(m2.captured(2)); }
    if (!rest.isEmpty()) {
        // « phase A » ou « A » : le nom de la phase dans l'export peut contenir le mot « phase » ou non.
        wanted << rest + " " + b;
        if (rest.startsWith(QLatin1String("phase "))) wanted << rest.mid(6).trimmed() + " " + b;
        else wanted << QStringLiteral("phase ") + rest + " " + b;
    }
    for (const QString& w : std::as_const(wanted)) {
        int found = 0;
        for (int s = 0; s < st.size(); ++s)
            for (int c = 0; c < st[s].columns.size(); ++c)
                if (key(st[s].columns[c]) == w) {
                    if (!found || s == preferred) { set = s; column = c; }
                    ++found;
                }
        // Même nom dans deux fichiers (Event de Chain_all_Events et de Chain_Phase_x) : mêmes tirages, le premier suffit.
        if (found >= 1) return true;
    }
    QStringList all;
    for (const TraceSet& t : st) all << t.columns;
    all.removeDuplicates();
    const int kMax = 60;
    QString list = QStringList(all.mid(0, kMax)).join(" ; ");
    if (all.size() > kMax) list += QStringLiteral(" ; … (+%1)").arg(all.size() - kMax);
    return fail(QStringLiteral("colonne « %1 » introuvable dans les traces chargées (colonnes : %2)").arg(name, list));
}

int setWithAll(const QStringList& names)
{
    for (int s = 0; s < store().size(); ++s) {
        bool all = !names.isEmpty();
        for (const QString& n : names) {
            int ss = -1, c = -1;
            all = all && resolve(n, ss, c, nullptr, s) && ss == s;
        }
        if (all) return s;
    }
    return -1;
}

QVector<Mention> mentions(const QString& question)
{
    const QString q = text::fold(question);
    // Formes cherchées pour chaque colonne : son nom, et pour une borne de phase « X Begin » / « X End » les tournures
    // « début de la phase X », « début de X », « X début », « begin of X »…
    struct Form { QString text, column; };
    QVector<Form> forms;
    for (const TraceSet& t : store())
        for (const QString& c : t.columns) {
            const QString k = key(c);
            forms.push_back({ k, c });
            static const QRegularExpression bound(QStringLiteral(R"(^(.+?)\s+(begin|end)$)"));
            if (const auto m = bound.match(k); m.hasMatch()) {
                QString base = m.captured(1);
                const bool begin = m.captured(2) == QLatin1String("begin");
                QStringList bases{ base };
                if (base.startsWith(QLatin1String("phase "))) bases << base.mid(6);
                for (const QString& b : std::as_const(bases)) {
                    const QStringList fr = begin ? QStringList{ "debut", "commencement" } : QStringList{ "fin" };
                    const QStringList en = begin ? QStringList{ "begin", "beginning", "start" } : QStringList{ "end" };
                    for (const QString& w : fr)
                        for (const QString& f : { w + " de la phase " + b, w + " de phase " + b, w + " de la " + b, w + " de " + b,
                                                  w + " du " + b, w + " d'" + b, b + " " + w })
                            forms.push_back({ f, c });
                    for (const QString& w : en)
                        for (const QString& f : { w + " of the phase " + b, w + " of phase " + b, w + " of " + b, b + " " + w })
                            forms.push_back({ f, c });
                }
            }
        }
    // Plus longues d'abord : une citation n'est pas reprise par une forme plus courte qu'elle contient.
    std::stable_sort(forms.begin(), forms.end(), [](const Form& a, const Form& b) { return a.text.size() > b.text.size(); });
    auto isWord = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); };
    QVector<bool> used(q.size(), false);
    QVector<Mention> out;
    for (const Form& f : std::as_const(forms)) {
        if (f.text.isEmpty()) continue;
        qsizetype from = 0;
        while (true) {
            const qsizetype i = q.indexOf(f.text, from);
            if (i < 0) break;
            from = i + 1;
            const qsizetype j = i + f.text.size();
            // Mot entier : pas de lettre ou de chiffre collé avant ou après (« US 1 » n'est pas dans « US 12 »).
            if ((i > 0 && isWord(q[i - 1]) && isWord(f.text.front())) || (j < q.size() && isWord(q[j]) && isWord(f.text.back()))) continue;
            bool overlap = false;
            for (qsizetype k = i; k < j && !overlap; ++k) overlap = used[k];
            if (overlap) continue;
            for (qsizetype k = i; k < j; ++k) used[k] = true;
            bool dup = false;
            for (const Mention& m : std::as_const(out)) dup = dup || m.column == f.column;
            if (!dup) out.push_back({ f.column, int(i) });
        }
    }
    std::sort(out.begin(), out.end(), [](const Mention& a, const Mention& b) { return a.pos < b.pos; });
    return out;
}

QStringList phasesIn(const QString& question)
{
    const QString q = text::fold(question);
    struct Hit { int pos; QString base; };
    QVector<Hit> hits;
    QSet<QString> seen;
    auto isWord = [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); };
    for (const TraceSet& t : store()) {
        for (const QString& c : t.columns) {
            if (!c.endsWith(QLatin1String(" Begin"))) continue;
            const QString base = c.chopped(6);
            if (seen.contains(base) || !t.columns.contains(base + QLatin1String(" End"))) continue;
            seen.insert(base);
            QStringList forms{ key(base) };
            if (forms.first().startsWith(QLatin1String("phase "))) forms << forms.first().mid(6);
            for (const QString& f : std::as_const(forms)) {
                const qsizetype i = q.indexOf(f);
                if (i < 0) continue;
                const qsizetype j = i + f.size();
                if ((i > 0 && isWord(q[i - 1])) || (j < q.size() && isWord(q[j]))) continue;
                hits.push_back({ int(i), base });
                break;
            }
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.pos < b.pos; });
    QStringList out;
    for (const Hit& h : std::as_const(hits)) out << h.base;
    return out;
}

} // namespace traces
} // namespace seldon
