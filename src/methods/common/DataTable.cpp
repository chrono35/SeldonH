/* ---------------------------------------------------------------------

Copyright © CNRS	2026

Authors :
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

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

#include "DataTable.h"

#include <QHash>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

#include "MethodRunner.h"
#include "TextUtil.h"

namespace seldon {
namespace datatable {

namespace {

const QRegularExpression::PatternOption CI = QRegularExpression::CaseInsensitiveOption;

QString clean(QString s)
{
    s.replace(QChar(0x00A0), QLatin1Char(' ')).replace(QChar(0x202F), QLatin1Char(' ')).replace(QChar(0x2009), QLatin1Char(' '));
    return s.trimmed();
}

// En-tête → rôle. L'ordre compte : « erreur ΔR » avant ΔR et erreur, « année de mesure » avant « mesure »…
Role roleOf(const QString& header)
{
    QString h = text::fold(clean(header));
    h.replace(QRegularExpression(QStringLiteral("[_*\"'`]")), QStringLiteral(" "));
    h = h.simplified();
    if (h.isEmpty()) return Role::Other;
    auto has = [&](const char* re) { return QRegularExpression(QString::fromUtf8(re), CI).match(h).hasMatch(); };
    const bool err = has(R"((\berr\w*|σ|sigma|±|\+/-|\+-|incert\w*|uncert\w*|\bsd\b|ecart.type|\bstd\b))");
    if (has(R"(\b(decalage|wiggle|offset)\b)")) return Role::Wiggle;
    if (has(R"((δ\s*r\b|delta\s*r\b|\bdr\b|reservoir))")) return err ? Role::DeltaRError : Role::DeltaR;
    if (has(R"(\b(intensit\w*|paleointensit\w*|field|champ)\b)")) return err ? Role::ErrorF : Role::Field;
    if (has(R"((annee|date|year)\s*(de |of )?(la )?(mesure|measure\w*|reference)|ref\s*year|mesure\w* en\b|^annee$)")) return Role::RefYear;
    if (has(R"(\b(f14c|fraction|fmc)\b)")) return err ? Role::Error : Role::F14C;
    if (has(R"((alpha\s*95|α\s*95|\ba95\b))")) return Role::Alpha95;
    if (has(R"(^(inclinaison|inclination|inc|i)(\s*\(.*\))?$)")) return Role::Inc;
    if (has(R"(^(declinaison|declination|dec|d)(\s*\(.*\))?$)")) return Role::Dec;
    if (has(R"(^(f)(\s*\(.*\))?$)")) return Role::Field;
    if (err) return Role::Error;
    if (has(R"(\b(methode|method|technique|type de datation|type|dating method)\b)")) return Role::Method;
    if (has(R"(\b(courbe|curve)\b)")) return Role::Curve;
    if (has(R"(\b(couleur|color|colour)\b)")) return Role::Color;
    if (has(R"(^(min|minimum|debut|borne inf\w*|tpq|terminus post quem|from|start)\b)")) return Role::Min;
    if (has(R"(^(max|maximum|fin|borne sup\w*|taq|terminus ante quem|to|end)\b)")) return Role::Max;
    if (has(R"(\b(age|ages|valeur|value|mesure|measure|measurement|date|bp|resultat|result|radiocarbon age|14c age)\b)")) return Role::Age;
    if (has(R"(\b(nom|name|code|labo|lab|laboratoire|reference|ref|echantillon|sample|id|identifiant|label|numero|dating name)\b|^n°|^no\b)"))
        return Role::Name;
    return Role::Other;
}

bool isValueRole(Role r) { return r == Role::Age || r == Role::F14C || r == Role::Inc || r == Role::Dec || r == Role::Field || r == Role::Min; }

// Premier nombre d'une cellule : « 2 500 BP » → 2500, « 0,85 » → 0.85, « 1.5E-2 ».
bool numberOf(const QString& cell, double& v)
{
    QString s = text::fold(clean(cell));
    s.replace(QRegularExpression(QStringLiteral(R"((?<=\d)[ '](?=\d{3}\b))")), QString());   // séparateur de milliers
    s.replace(QChar(0x2212), QLatin1Char('-'));
    static const QRegularExpression re(QStringLiteral(R"((?<![\w.])([-+]?\d+(?:[.,]\d+)?(?:e[-+]?\d+)?))"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return false;
    QString n = m.captured(1);
    n.replace(QLatin1Char(','), QLatin1Char('.'));
    bool ok = false;
    v = n.toDouble(&ok);
    return ok && std::isfinite(v);
}

QString methodFromText(const QString& cell)
{
    const QString c = text::fold(clean(cell));
    if (c.isEmpty()) return {};
    static const QList<QPair<QRegularExpression, QString>> map = {
        { QRegularExpression(QStringLiteral(R"(\bf14c\b|fraction moderne|post.?bomb)")), QStringLiteral("f14c") },
        { QRegularExpression(QStringLiteral(R"(\b(14c|c14|c-14|14-c|radiocarbone?|radiocarbon|carbone ?14|ams|rc)\b)")), QStringLiteral("c14") },
        { QRegularExpression(QStringLiteral(R"(\b(tl|osl|irsl|tl/osl|luminescence|thermoluminescence|ost)\b)")), QStringLiteral("tl") },
        { QRegularExpression(QStringLiteral(R"(\b(am|archeomag\w*|paleomag\w*|magnet\w*|archaeomag\w*))")), QStringLiteral("am") },
        { QRegularExpression(QStringLiteral(R"(\b(gauss\w*|normale?|normal law|dendro\w*|typo\w*))")), QStringLiteral("gauss") },
        { QRegularExpression(QStringLiteral(R"(\b(unif\w*|intervalle|interval|range|tpq.?taq))")), QStringLiteral("unif") },
    };
    for (const auto& p : map) if (p.first.match(c).hasMatch()) return p.second;
    return {};
}

// Codes de laboratoires de radiocarbone courants (Ly-1234, GrA-5678, Beta-123456, Poz-…, SacA …).
bool looksLikeC14LabCode(const QString& name)
{
    static const QRegularExpression re(QStringLiteral(
        R"(^(ly|lyon|gra|grn|grl|gr|beta|poz|saca|sac|oxa|ox|kia|eth|uba|mams|erl|hd|kik|ltl|ua|aa|cams|uciams|wk|d-ams|gif|gifa|pa|lv|sua|bln|kn|vera|isgs|tka|nza|ozk|lu|ki|suerc|deb|cna|lsc|rome|ciral|fi)\s*[-_ ]?\s*\d)"), CI);
    return re.match(clean(name)).hasMatch();
}

QString curveOf(const QString& cell)
{
    QString c = text::fold(clean(cell));
    c.remove(QRegularExpression(QStringLiteral(R"(\.(14c|ref)$)")));
    c.remove(QLatin1Char(' '));
    return c;
}

QString methodTitle(const QString& m)
{
    static const QHash<QString, QString> t = { { "c14", "14C" }, { "f14c", "F14C" }, { "tl", "TL/OSL" }, { "am", "archéomagnétisme" },
                                               { "gauss", "gaussienne" }, { "unif", "uniforme" } };
    return t.value(m, m);
}

// Une ligne → datation. `problem` : raison si la ligne n'est pas datable (vide sinon).
QJsonObject rowRequest(const Table& t, const QStringList& row, QString& problem)
{
    auto cell = [&](Role r) -> QString {
        const int i = t.roles.indexOf(r);
        return (i >= 0 && i < row.size()) ? clean(row[i]) : QString();
    };
    auto num = [&](Role r, double& v) { const QString c = cell(r); return !c.isEmpty() && numberOf(c, v); };
    bool emptyRow = true;
    for (const QString& c : row) if (!clean(c).isEmpty()) { emptyRow = false; break; }
    if (emptyRow) { problem = QStringLiteral("ligne vide"); return {}; }

    const QString name = cell(Role::Name);
    QString method = methodFromText(cell(Role::Method));
    const QString ageCell = cell(Role::Age);
    if (method.isEmpty()) {                             // méthode déduite des colonnes remplies, de l'unité ou du code labo
        double x;
        if (num(Role::Inc, x) || num(Role::Dec, x) || num(Role::Field, x)) method = QStringLiteral("am");
        else if (num(Role::F14C, x)) method = QStringLiteral("f14c");
        else if (!cell(Role::RefYear).isEmpty()) method = QStringLiteral("tl");
        else if (ageCell.isEmpty() && num(Role::Min, x) && num(Role::Max, x)) method = QStringLiteral("unif");
        else {
            const int ai = t.roles.indexOf(Role::Age);
            const bool bpHeader = ai >= 0 && QRegularExpression(QStringLiteral(R"(\bbp\b|14c|radiocarbon)"), CI).match(text::fold(t.headers[ai])).hasMatch();
            const bool bpCell = QRegularExpression(QStringLiteral(R"(\bbp\b)"), CI).match(text::fold(ageCell)).hasMatch();
            if (bpHeader || bpCell || looksLikeC14LabCode(name)) method = QStringLiteral("c14");
        }
    }
    if (method.isEmpty()) {
        problem = cell(Role::Method).isEmpty() ? QStringLiteral("méthode inconnue (ajoutez une colonne « Méthode » : 14C, TL, gaussienne…)")
                                               : QStringLiteral("méthode « %1 » non reconnue").arg(cell(Role::Method));
        return {};
    }

    QJsonObject o{ { "method", method } };
    if (!name.isEmpty()) o["name"] = name;
    double age = 0, err = 0;
    bool hasAge = false, hasErr = num(Role::Error, err);
    if (!ageCell.isEmpty()) {
        const QVector<text::ValueError> ve = text::findValueErrors(text::fold(ageCell));
        if (!ve.isEmpty()) { age = ve.first().value; hasAge = true; if (!hasErr) { err = ve.first().error; hasErr = true; } }
        else hasAge = numberOf(ageCell, age);
    }

    if (method == QLatin1String("unif")) {
        double a, b;
        if (!(num(Role::Min, a) && num(Role::Max, b))) { problem = QStringLiteral("bornes min et max manquantes"); return {}; }
        o["min"] = std::min(a, b); o["max"] = std::max(a, b);
    } else if (method == QLatin1String("am")) {
        double inc, dec, a95, f, ef;
        QString process;
        if (num(Role::Inc, inc)) { o["inc"] = inc; process += 'I'; }
        if (num(Role::Dec, dec)) {
            if (!o.contains("inc")) { problem = QStringLiteral("inclinaison nécessaire pour dater une déclinaison"); return {}; }
            o["dec"] = dec; process += 'D';
        }
        if (num(Role::Field, f)) { o["field"] = f; process += 'F'; }
        if (process.isEmpty()) { problem = QStringLiteral("aucune mesure archéomagnétique (inclinaison, déclinaison ou intensité)"); return {}; }
        if (process.contains('I') || process.contains('D')) {
            if (!num(Role::Alpha95, a95) || a95 <= 0) { problem = QStringLiteral("α95 manquant"); return {}; }
            o["alpha95"] = a95;
        }
        if (process.contains('F')) {
            if (!num(Role::ErrorF, ef) || ef <= 0) { problem = QStringLiteral("erreur de l'intensité manquante"); return {}; }
            o["error_f"] = ef;
        }
        o["process"] = process;
    } else if (method == QLatin1String("f14c")) {
        double f;
        if (!num(Role::F14C, f)) { if (!hasAge) { problem = QStringLiteral("valeur F14C manquante"); return {}; } f = age; }
        if (!hasErr || err <= 0) { problem = QStringLiteral("erreur manquante"); return {}; }
        o["f14c"] = f; o["error"] = err;
    } else {
        if (!hasAge) { problem = QStringLiteral("âge manquant"); return {}; }
        if (!hasErr || err <= 0) { problem = QStringLiteral("erreur manquante"); return {}; }
        o["age"] = age; o["error"] = err;
        if (method == QLatin1String("tl")) {
            double ry;
            if (!num(Role::RefYear, ry)) { problem = QStringLiteral("année de mesure manquante (TL/OSL)"); return {}; }
            o["ref_year"] = ry;
        }
        if (method == QLatin1String("c14")) {
            double dr, dre;
            if (num(Role::DeltaR, dr)) { o["delta_r"] = dr; o["delta_r_error"] = num(Role::DeltaRError, dre) ? dre : 0.0; o["has_delta_r"] = true; }
        }
    }
    if (method == QLatin1String("c14") || method == QLatin1String("f14c")) {
        const QString c = curveOf(cell(Role::Curve));
        if (!c.isEmpty()) o["curve"] = c;
    }
    const QString wcell = cell(Role::Wiggle);
    if (!wcell.isEmpty() && !QRegularExpression(QStringLiteral(R"(^(0|aucun|none|non|-|—)$)"), CI).match(text::fold(wcell)).hasMatch()) {
        QJsonObject w = MethodRunner::wiggleFromQuestion(QStringLiteral("décalage ") + wcell);
        double fx;
        if (w.isEmpty() && numberOf(wcell, fx)) w = QJsonObject{ { "type", "fixed" }, { "value", fx } };
        if (w.isEmpty()) { problem = QStringLiteral("décalage « %1 » non compris").arg(wcell); return {}; }
        o["wiggle"] = w;
    }
    if (const QString col = cell(Role::Color); !col.isEmpty()) o["color"] = col;
    problem.clear();
    return o;
}

Table g_current;
bool g_has = false;

QString esc(const QString& s) { return s.toHtmlEscaped(); }

int columnFor(const Table& t, const QString& wanted, Role* pseudo)
{
    const QString w = text::fold(clean(wanted));
    *pseudo = Role::Other;
    for (int i = 0; i < t.headers.size(); ++i) if (text::fold(clean(t.headers[i])) == w) return i;
    static const QHash<QString, Role> alias = { { "methode", Role::Method }, { "method", Role::Method }, { "nom", Role::Name },
                                                { "name", Role::Name }, { "age", Role::Age }, { "erreur", Role::Error },
                                                { "error", Role::Error } };
    if (alias.contains(w)) {
        *pseudo = alias.value(w);
        const int i = t.roles.indexOf(*pseudo);
        if (i >= 0) return i;
        return -2;                                     // pseudo-colonne (méthode reconnue par SeldonH)
    }
    for (int i = 0; i < t.headers.size(); ++i) if (text::fold(clean(t.headers[i])).contains(w)) return i;
    return -1;
}

QString valueAt(const Table& t, int row, int col, Role pseudo)
{
    if (col == -2) {
        if (pseudo == Role::Method) return t.requests[row].value("method").toString();
        if (pseudo == Role::Name) return t.requests[row].value("name").toString();
        return {};
    }
    return col >= 0 && col < t.cells[row].size() ? clean(t.cells[row][col]) : QString();
}

bool compareValue(const QString& cell, const QString& op, const QJsonValue& value)
{
    const QString c = text::fold(cell);
    auto eq = [&](const QJsonValue& v) {
        const QString s = v.isDouble() ? QString::number(v.toDouble(), 'g', 15) : v.toString();
        double a, b;
        if (numberOf(cell, a) && numberOf(s, b) && QRegularExpression(QStringLiteral(R"(^[-+]?\d)")).match(clean(s)).hasMatch()
            && QRegularExpression(QStringLiteral(R"(^[-+]?\d)")).match(clean(cell)).hasMatch())
            return std::abs(a - b) < 1e-9;
        const QString f = text::fold(clean(s));
        return c == f || methodFromText(c) == f || (!methodFromText(c).isEmpty() && methodFromText(c) == methodFromText(f));
    };
    if (op == QLatin1String("=") || op == QLatin1String("==") || op.isEmpty()) return eq(value);
    if (op == QLatin1String("!=") || op == QLatin1String("<>")) return !eq(value);
    if (op == QLatin1String("in")) { for (const QJsonValue& v : value.toArray()) if (eq(v)) return true; return false; }
    if (op == QLatin1String("not_in")) { for (const QJsonValue& v : value.toArray()) if (eq(v)) return false; return true; }
    const QString vs = text::fold(value.isDouble() ? QString::number(value.toDouble(), 'g', 15) : value.toString());
    if (op == QLatin1String("contains")) return c.contains(vs);
    if (op == QLatin1String("not_contains")) return !c.contains(vs);
    double a, b;
    if (!numberOf(cell, a) || !(value.isDouble() ? (b = value.toDouble(), true) : numberOf(value.toString(), b))) return false;
    if (op == QLatin1String("<")) return a < b;
    if (op == QLatin1String(">")) return a > b;
    if (op == QLatin1String("<=")) return a <= b;
    if (op == QLatin1String(">=")) return a >= b;
    return false;
}

QStringList stringsOf(const QJsonValue& v)
{
    QStringList out;
    if (v.isArray()) { for (const QJsonValue& x : v.toArray()) out << (x.isDouble() ? QString::number(x.toDouble(), 'g', 15) : x.toString()); }
    else if (v.isString()) { for (const QString& s : v.toString().split(QRegularExpression(QStringLiteral("[,;]")), Qt::SkipEmptyParts)) out << s.trimmed(); }
    else if (v.isDouble()) out << QString::number(v.toDouble(), 'g', 15);
    return out;
}

QVector<int> intsOf(const QJsonValue& v)
{
    QVector<int> out;
    for (const QString& s : stringsOf(v)) {
        const auto m = QRegularExpression(QStringLiteral(R"(^\s*(\d+)\s*(?:-|à|a|to|\.\.)\s*(\d+)\s*$)")).match(s);
        if (m.hasMatch()) {
            for (int i = m.captured(1).toInt(); i <= m.captured(2).toInt() && out.size() < 100000; ++i) out << i;
            continue;
        }
        bool ok = false;
        const int i = s.trimmed().toInt(&ok);
        if (ok) out << i;
    }
    return out;
}

} // namespace

QString roleTitle(Role r)
{
    switch (r) {
    case Role::Name: return QStringLiteral("nom");
    case Role::Method: return QStringLiteral("méthode");
    case Role::Age: return QStringLiteral("âge / valeur");
    case Role::Error: return QStringLiteral("erreur");
    case Role::RefYear: return QStringLiteral("année de mesure");
    case Role::Curve: return QStringLiteral("courbe");
    case Role::DeltaR: return QStringLiteral("ΔR");
    case Role::DeltaRError: return QStringLiteral("erreur ΔR");
    case Role::F14C: return QStringLiteral("F14C");
    case Role::Inc: return QStringLiteral("inclinaison");
    case Role::Dec: return QStringLiteral("déclinaison");
    case Role::Alpha95: return QStringLiteral("α95");
    case Role::Field: return QStringLiteral("intensité");
    case Role::ErrorF: return QStringLiteral("erreur de l'intensité");
    case Role::Min: return QStringLiteral("borne min");
    case Role::Max: return QStringLiteral("borne max");
    case Role::Wiggle: return QStringLiteral("décalage");
    case Role::Color: return QStringLiteral("couleur");
    case Role::Other: break;
    }
    return QStringLiteral("autre");
}

int Table::datable() const
{
    int n = 0;
    for (const QJsonObject& r : requests) if (!r.isEmpty()) ++n;
    return n;
}

int Table::column(const QString& header) const
{
    Role p;
    const int c = columnFor(*this, header, &p);
    return c >= 0 ? c : -1;
}

QString Table::title() const
{
    QString s = source.isEmpty() ? QStringLiteral("tableau collé") : source;
    if (!sheet.isEmpty()) s += QStringLiteral(" — feuille « %1 »").arg(sheet);
    return s;
}

Table fromGrid(const QVector<QStringList>& grid, const QString& source, QString* error, const QVector<int>& lineNumbers)
{
    Table t;
    t.source = source;
    int header = -1;
    QVector<Role> roles;
    for (int r = 0; r < std::min<int>(grid.size(), 15) && header < 0; ++r) {
        QVector<Role> rr;
        int known = 0;
        bool value = false;
        QSet<int> seen;
        for (const QString& h : grid[r]) {
            Role ro = roleOf(h);
            if (ro != Role::Other && seen.contains(int(ro))) ro = Role::Other;      // seule la première colonne d'un rôle compte
            if (ro != Role::Other) { seen.insert(int(ro)); ++known; }
            value = value || isValueRole(ro);
            rr << ro;
        }
        if (known >= 2 && value) { header = r; roles = rr; }
    }
    if (header < 0) {
        if (error) *error = QStringLiteral("aucune ligne d'en-tête reconnue (il faut au moins une colonne de valeur — âge, F14C, inclinaison… — "
                                           "et une autre colonne connue : erreur, nom, méthode…)");
        return t;
    }
    t.headerLine = lineNumbers.size() > header ? lineNumbers[header] : header + 1;
    t.headers = grid[header];
    for (QString& h : t.headers) h = clean(h);
    t.roles = roles;
    // Colonne « Type » ou « Méthode » dont aucune valeur n'est une méthode de datation (type de matériau…) : colonne ordinaire.
    if (const int mc = t.roles.indexOf(Role::Method); mc >= 0) {
        bool any = false;
        for (int r = header + 1; r < grid.size() && !any; ++r) any = mc < grid[r].size() && !methodFromText(grid[r][mc]).isEmpty();
        if (!any) t.roles[mc] = Role::Other;
    }
    if (header > 0) t.notes << QStringLiteral("%1 ligne(s) au-dessus de l'en-tête ignorée(s) (titre, notes)").arg(header);
    for (int r = header + 1; r < grid.size(); ++r) {
        QStringList row = grid[r];
        bool empty = true;
        for (const QString& c : row) if (!clean(c).isEmpty()) { empty = false; break; }
        if (empty) continue;
        while (row.size() < t.headers.size()) row << QString();
        if (row.size() > t.headers.size()) {                   // cellules sans en-tête : gardées sous « colonne N »
            for (int c = t.headers.size(); c < row.size(); ++c) { t.headers << QStringLiteral("colonne %1").arg(c + 1); t.roles << Role::Other; }
            for (QStringList& prev : t.cells) while (prev.size() < t.headers.size()) prev << QString();
        }
        QString problem;
        const QJsonObject rq = rowRequest(t, row, problem);
        t.cells << row;
        t.sourceLines << (lineNumbers.size() > r ? lineNumbers[r] : r + 1);
        t.requests << rq;
        t.problems << problem;
    }
    if (t.cells.isEmpty() && error) *error = QStringLiteral("en-tête reconnu, mais aucune ligne de données");
    // Méthode déduite (pas de colonne « Méthode ») : à dire à l'utilisateur.
    if (!t.roles.contains(Role::Method) && t.datable() > 0)
        t.notes << QStringLiteral("pas de colonne « Méthode » : méthode déduite des colonnes remplies, de l'unité BP ou du code de laboratoire");
    return t;
}

QVector<QStringList> splitText(const QString& text)
{
    QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
    // Séparateur le plus régulier : tabulation, point-virgule, barre verticale (Markdown), virgule en dernier recours.
    const QList<QChar> seps = { QLatin1Char('\t'), QLatin1Char(';'), QLatin1Char('|'), QLatin1Char(',') };
    QChar sep;
    for (const QChar s : seps) {
        int n = 0;
        for (const QString& l : lines) if (l.count(s) >= (s == QLatin1Char('|') ? 2 : 2)) ++n;
        if (n >= 2) { sep = s; break; }
    }
    QVector<QStringList> grid;
    if (sep.isNull()) return grid;
    static const QRegularExpression mdRule(QStringLiteral(R"(^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$)"));
    for (const QString& l0 : lines) {
        if (mdRule.match(l0).hasMatch()) continue;          // ligne |---|---| du Markdown
        QString l = l0;
        if (sep == QLatin1Char('|')) {
            l = l.trimmed();
            if (l.startsWith('|')) l.remove(0, 1);
            if (l.endsWith('|')) l.chop(1);
        }
        QStringList cells;
        QString cur;
        bool quoted = false;
        for (int i = 0; i < l.size(); ++i) {                 // guillemets du CSV : "a; b" reste une cellule
            const QChar c = l[i];
            if (c == QLatin1Char('"')) {
                if (quoted && i + 1 < l.size() && l[i + 1] == QLatin1Char('"')) { cur += c; ++i; }
                else quoted = !quoted;
            } else if (c == sep && !quoted) { cells << cur.trimmed(); cur.clear(); }
            else cur += c;
        }
        cells << cur.trimmed();
        grid << cells;
    }
    return grid;
}

Table parseText(const QString& text, const QString& source, QString* error)
{
    const QVector<QStringList> grid = splitText(text);
    if (grid.isEmpty()) {
        if (error) *error = QStringLiteral("pas de séparateur de colonnes reconnu (tabulation, « ; », « | » ou « , »)");
        return Table();
    }
    return fromGrid(grid, source, error);
}

bool looksLikeTable(const QString& text, int* start, int* length)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    const QList<QChar> seps = { QLatin1Char('\t'), QLatin1Char(';'), QLatin1Char('|') };
    for (const QChar s : seps) {
        // Plus long bloc de lignes consécutives avec au moins 2 séparateurs (3 cellules), ligne |---| comprise.
        int bestFirst = -1, bestLast = -1, first = -1;
        for (int i = 0; i <= lines.size(); ++i) {
            const bool in = i < lines.size() && (lines[i].count(s) >= 2);
            if (in && first < 0) first = i;
            if (!in && first >= 0) {
                if (i - first > bestLast - bestFirst + 1 || bestFirst < 0) { bestFirst = first; bestLast = i - 1; }
                first = -1;
            }
        }
        if (bestFirst >= 0 && bestLast - bestFirst + 1 >= 2) {
            if (start || length) {
                int a = 0;
                for (int i = 0; i < bestFirst; ++i) a += lines[i].size() + 1;
                int b = a;
                for (int i = bestFirst; i <= bestLast; ++i) b += lines[i].size() + 1;
                if (start) *start = a;
                if (length) *length = std::min<int>(b, text.size()) - a;
            }
            return true;
        }
    }
    return false;
}

bool isChronoModelImport(const QString& text)
{
    static const QRegularExpression csvHeader(QStringLiteral(R"(\bmethod\s*[;,\t]\s*dating\s+name)"), CI);
    return csvHeader.match(text).hasMatch();
}

void setCurrent(const Table& t) { g_current = t; g_has = t.isValid(); }
void clear() { g_current = Table(); g_has = false; }
bool hasCurrent() { return g_has; }
const Table& current() { return g_current; }

Selection select(const Table& t, const QJsonObject& spec)
{
    Selection s;
    QStringList crit;
    QVector<bool> keep(t.cells.size(), true);

    const QStringList methods = stringsOf(spec.value("methods").isUndefined() ? spec.value("method_filter") : spec.value("methods"));
    if (!methods.isEmpty()) {
        QStringList ms;
        for (const QString& m : methods) { const QString n = methodFromText(m); ms << (n.isEmpty() ? text::fold(m) : n); }
        for (int r = 0; r < t.cells.size(); ++r) keep[r] = keep[r] && ms.contains(t.requests[r].value("method").toString());
        QStringList titles;
        for (const QString& m : ms) titles << methodTitle(m);
        crit << QStringLiteral("méthode %1").arg(titles.join(QStringLiteral(" ou ")));
    }
    const QVector<int> rows = intsOf(spec.value("rows"));
    if (!rows.isEmpty()) {
        for (int r = 0; r < t.cells.size(); ++r) keep[r] = keep[r] && rows.contains(r + 1);
        QStringList l; for (int r : rows) l << QString::number(r);
        crit << QStringLiteral("lignes %1").arg(l.join(QStringLiteral(", ")));
    }
    const QVector<int> xrows = intsOf(spec.value("exclude_rows"));
    if (!xrows.isEmpty()) {
        for (int r = 0; r < t.cells.size(); ++r) keep[r] = keep[r] && !xrows.contains(r + 1);
        QStringList l; for (int r : xrows) l << QString::number(r);
        crit << QStringLiteral("sauf lignes %1").arg(l.join(QStringLiteral(", ")));
    }
    const int nameCol = t.roles.indexOf(Role::Name);
    auto nameOf = [&](int r) { return nameCol >= 0 && nameCol < t.cells[r].size() ? text::fold(clean(t.cells[r][nameCol])) : QString(); };
    const QStringList names = stringsOf(spec.value("names"));
    if (!names.isEmpty()) {
        QStringList f; for (const QString& n : names) f << text::fold(n.trimmed());
        for (int r = 0; r < t.cells.size(); ++r) keep[r] = keep[r] && f.contains(nameOf(r));
        crit << QStringLiteral("noms %1").arg(names.join(QStringLiteral(", ")));
        for (const QString& n : f) {
            bool found = false;
            for (int r = 0; r < t.cells.size() && !found; ++r) found = nameOf(r) == n;
            if (!found) { s.error = QStringLiteral("nom « %1 » absent du tableau").arg(n); return s; }
        }
    }
    const QStringList xnames = stringsOf(spec.value("exclude_names"));
    if (!xnames.isEmpty()) {
        QStringList f; for (const QString& n : xnames) f << text::fold(n.trimmed());
        for (int r = 0; r < t.cells.size(); ++r) keep[r] = keep[r] && !f.contains(nameOf(r));
        crit << QStringLiteral("sauf %1").arg(xnames.join(QStringLiteral(", ")));
    }
    QJsonArray where = spec.value("where").toArray();
    if (spec.value("where").isObject()) where.append(spec.value("where"));
    for (const QJsonValue& wv : where) {
        const QJsonObject w = wv.toObject();
        const QString colName = w.value("column").toString();
        const QString op = w.value("op").toString(QStringLiteral("=")).trimmed().toLower();
        Role pseudo;
        const int col = columnFor(t, colName, &pseudo);
        if (col == -1) {
            s.error = QStringLiteral("colonne « %1 » absente du tableau (colonnes : %2)").arg(colName, t.headers.join(QStringLiteral(", ")));
            return s;
        }
        static const QStringList ops = { "=", "==", "!=", "<>", "in", "not_in", "contains", "not_contains", "<", ">", "<=", ">=" };
        if (!ops.contains(op)) { s.error = QStringLiteral("opérateur « %1 » inconnu (=, !=, contains, not_contains, <, >, <=, >=, in)").arg(op); return s; }
        for (int r = 0; r < t.cells.size(); ++r) keep[r] = keep[r] && compareValue(valueAt(t, r, col, pseudo), op, w.value("value"));
        const QJsonValue v = w.value("value");
        const QString vs = v.isArray() ? stringsOf(v).join(QStringLiteral(", ")) : (v.isDouble() ? QString::number(v.toDouble(), 'g', 15) : v.toString());
        crit << QStringLiteral("%1 %2 %3").arg(colName, op, vs);
    }
    for (int r = 0; r < t.cells.size(); ++r) if (keep[r]) s.rows << r;
    s.criteria = crit.isEmpty() ? QStringLiteral("toutes les lignes") : crit.join(QStringLiteral(" ; "));
    return s;
}

QVector<QJsonObject> requestsOf(const Table& t, const QVector<int>& rows, QStringList* skipped)
{
    QVector<QJsonObject> out;
    for (int r : rows) {
        if (r < 0 || r >= t.requests.size()) continue;
        if (t.requests[r].isEmpty()) {
            if (skipped) *skipped << QStringLiteral("ligne %1 : %2").arg(r + 1).arg(t.problems[r]);
            continue;
        }
        QJsonObject o = t.requests[r];
        if (!o.contains("name")) o["name"] = QStringLiteral("ligne %1").arg(r + 1);
        out << o;
    }
    return out;
}

QString rowsHtml(const Table& t, const QVector<int>& rows, int maxRows)
{
    QString h = QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"3\"><tr><th>#</th>");
    for (const QString& c : t.headers) h += QStringLiteral("<th>%1</th>").arg(esc(c));
    h += QStringLiteral("<th>Lu par SeldonH</th></tr>");
    int n = 0;
    for (int r : rows) {
        if (r < 0 || r >= t.cells.size()) continue;
        if (++n > maxRows) break;
        h += QStringLiteral("<tr><td>%1</td>").arg(r + 1);
        for (int c = 0; c < t.headers.size(); ++c) h += QStringLiteral("<td>%1</td>").arg(c < t.cells[r].size() ? esc(t.cells[r][c]) : QString());
        const QString m = t.requests[r].value("method").toString();
        h += t.requests[r].isEmpty() ? QStringLiteral("<td><i>⚠ %1</i></td>").arg(esc(t.problems[r]))
                                     : QStringLiteral("<td>%1</td>").arg(esc(methodTitle(m)));
        h += QStringLiteral("</tr>");
    }
    h += QStringLiteral("</table>");
    if (n > maxRows) h += QStringLiteral("<p><i>… %1 lignes de plus (voir le fichier CSV).</i></p>").arg(rows.size() - maxRows);
    return h;
}

QString toCsv(const Table& t, const QVector<int>& rows)
{
    auto q = [](const QString& s) {
        if (s.contains(QLatin1Char(';')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n')))
            return QStringLiteral("\"%1\"").arg(QString(s).replace(QLatin1Char('"'), QStringLiteral("\"\"")));
        return s;
    };
    QStringList lines;
    QStringList head;
    for (const QString& h : t.headers) head << q(h);
    lines << head.join(QLatin1Char(';'));
    for (int r : rows) {
        if (r < 0 || r >= t.cells.size()) continue;
        QStringList l;
        for (int c = 0; c < t.headers.size(); ++c) l << q(c < t.cells[r].size() ? t.cells[r][c] : QString());
        lines << l.join(QLatin1Char(';'));
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

namespace {
QString methodCounts(const Table& t, const QVector<int>* rows = nullptr)
{
    QMap<QString, int> by;
    auto add = [&](int r) { if (!t.requests[r].isEmpty()) by[t.requests[r].value("method").toString()]++; };
    if (rows) for (int r : *rows) add(r); else for (int r = 0; r < t.requests.size(); ++r) add(r);
    QStringList l;
    for (auto it = by.begin(); it != by.end(); ++it) l << QStringLiteral("%1 %2").arg(it.value()).arg(methodTitle(it.key()));
    return l.join(QStringLiteral(", "));
}
}

QString summaryHtml(const Table& t)
{
    QString h = QStringLiteral("<p><b>📋 Tableau lu par SeldonH</b> — %1 : %2 ligne(s), dont <b>%3 datable(s)</b>%4.</p>")
                    .arg(esc(t.title())).arg(t.cells.size()).arg(t.datable())
                    .arg(t.datable() ? QStringLiteral(" (%1)").arg(esc(methodCounts(t))) : QString());
    QStringList known, other;
    for (int c = 0; c < t.headers.size(); ++c)
        (t.roles[c] == Role::Other ? other : known) << (t.roles[c] == Role::Other ? esc(t.headers[c])
                                                                                   : QStringLiteral("%1 → %2").arg(esc(t.headers[c]), esc(roleTitle(t.roles[c]))));
    h += QStringLiteral("<p>Colonnes reconnues : %1.").arg(known.join(QStringLiteral(" ; ")));
    if (!other.isEmpty()) h += QStringLiteral(" Gardées pour les sélections : %1.").arg(other.join(QStringLiteral(", ")));
    h += QStringLiteral("</p>");
    QStringList bad;
    for (int r = 0; r < t.cells.size(); ++r) if (t.requests[r].isEmpty()) bad << QStringLiteral("ligne %1 : %2").arg(r + 1).arg(t.problems[r]);
    if (!bad.isEmpty()) {
        h += QStringLiteral("<p><i>⚠ %1 ligne(s) non datable(s) : %2%3</i></p>")
                 .arg(bad.size()).arg(esc(bad.mid(0, 8).join(QStringLiteral(" ; "))), bad.size() > 8 ? QStringLiteral(" …") : QString());
    }
    for (const QString& n : t.notes) h += QStringLiteral("<p><i>ℹ %1.</i></p>").arg(esc(n));
    QVector<int> all;
    for (int r = 0; r < t.cells.size(); ++r) all << r;
    h += rowsHtml(t, all, 30);
    h += QStringLiteral("<p><i>Vous pouvez demander : « extrais les 14C », « date les TL du tableau », « fais un événement avec les "
                        "lignes 2 à 5 », « un modèle ChronoModel avec un événement par US »…</i></p>");
    return h;
}

QString digest(const Table& t, int budget)
{
    QString s = QStringLiteral("[Tableau courant « %1 », lu et gardé par SeldonH : %2 ligne(s) numérotées #1 à #%2, dont %3 datable(s)%4.\n")
                    .arg(t.title()).arg(t.cells.size()).arg(t.datable())
                    .arg(t.datable() ? QStringLiteral(" (%1)").arg(methodCounts(t)) : QString());
    QStringList cols;
    for (int c = 0; c < t.headers.size(); ++c) {
        QString d = t.headers[c];
        if (t.roles[c] != Role::Other) d += QStringLiteral(" (%1)").arg(roleTitle(t.roles[c]));
        if (t.roles[c] == Role::Other || t.roles[c] == Role::Method) {        // valeurs distinctes : utiles pour les sélections
            QStringList vals;
            for (const QStringList& r : t.cells) { const QString v = c < r.size() ? clean(r[c]) : QString(); if (!v.isEmpty() && !vals.contains(v)) vals << v; }
            if (!vals.isEmpty() && vals.size() <= 25) d += QStringLiteral(" : valeurs %1").arg(vals.join(QStringLiteral(", ")));
            else if (!vals.isEmpty()) d += QStringLiteral(" : %1 valeurs différentes").arg(vals.size());
        }
        cols << d;
    }
    s += QStringLiteral("Colonnes : %1.\n").arg(cols.join(QStringLiteral(" ; ")));
    QStringList bad;
    for (int r = 0; r < t.cells.size(); ++r) if (t.requests[r].isEmpty()) bad << QStringLiteral("#%1 (%2)").arg(r + 1).arg(t.problems[r]);
    if (!bad.isEmpty()) s += QStringLiteral("Lignes non datables : %1.\n").arg(bad.mid(0, 10).join(QStringLiteral(", ")));
    // Lignes (dans le budget) : pour choisir par nom ou par valeur. Le modèle ne les recopie jamais.
    QString rows;
    for (int r = 0; r < t.cells.size(); ++r) {
        QStringList l;
        for (int c = 0; c < t.headers.size(); ++c) { const QString v = c < t.cells[r].size() ? clean(t.cells[r][c]) : QString(); if (!v.isEmpty()) l << v; }
        const QString line = QStringLiteral("#%1 %2\n").arg(r + 1).arg(l.join(QStringLiteral(" | ")));
        if (s.size() + rows.size() + line.size() > budget) { rows += QStringLiteral("… (%1 lignes de plus)\n").arg(t.cells.size() - r); break; }
        rows += line;
    }
    if (!rows.isEmpty()) s += QStringLiteral("Lignes (pour choisir ; ne les recopie pas) :\n") + rows;
    s += QStringLiteral("Pour extraire, dater, regrouper (événement, combinaison) ou faire un modèle ChronoModel avec des lignes de ce tableau : "
                        "outil « tableau » ou bloc calcul {\"method\":\"table\",\"action\":…} avec une sélection (fiche « Tableaux de "
                        "datations ») ; SeldonH prend les valeurs dans le tableau. N'écris jamais les valeurs du tableau toi-même.]\n");
    return s;
}

QJsonObject modelSpec(const Table& t, const QVector<int>& rows, const QString& groupBy, const QString& name, QString* error)
{
    int gcol = -1;
    if (!groupBy.trimmed().isEmpty()) {
        Role p;
        gcol = columnFor(t, groupBy, &p);
        if (gcol < 0) { if (error) *error = QStringLiteral("colonne « %1 » absente du tableau").arg(groupBy); return {}; }
    }
    QStringList order;
    QHash<QString, QJsonArray> groups;
    QStringList skipped;
    const QVector<QJsonObject> rq = requestsOf(t, rows, &skipped);
    int k = 0;
    for (int r : rows) {
        if (r < 0 || r >= t.requests.size() || t.requests[r].isEmpty()) continue;
        QJsonObject d = rq[k++];
        d["type"] = d.take("method").toString();
        d.remove("has_delta_r");
        const QString ev = gcol >= 0 ? clean(t.cells[r].value(gcol)) : d.value("name").toString();
        const QString key = ev.isEmpty() ? QStringLiteral("sans %1").arg(groupBy) : ev;
        if (!groups.contains(key)) order << key;
        groups[key].append(d);
    }
    if (order.isEmpty()) { if (error) *error = QStringLiteral("aucune ligne datable dans la sélection"); return {}; }
    QJsonArray events;
    static const QRegularExpression onlyNumber(QStringLiteral(R"(^[-+]?\d+(?:[.,]\d+)?$)"));
    for (const QString& e : order) {
        // Groupe « 12 » de la colonne « US » : événement « US 12 ».
        const QString evName = gcol >= 0 && onlyNumber.match(e).hasMatch() ? QStringLiteral("%1 %2").arg(t.headers[gcol], e) : e;
        events.append(QJsonObject{ { "name", evName }, { "dates", groups.value(e) } });
    }
    return QJsonObject{ { "name", name.trimmed().isEmpty() ? QStringLiteral("Modèle tableau") : name.trimmed() }, { "events", events } };
}

QJsonObject requestFromQuestion(const QString& question)
{
    if (!hasCurrent()) return {};
    const QString q = text::fold(question);
    if (!text::findValueErrors(q).isEmpty()) return {};                // la question donne ses propres datations
    static const QRegularExpression ref(QStringLiteral(
        R"(\b(tableau|table|feuille|fichier|classeur|spreadsheet|sheet|toutes les dates|toutes les datations|tous les echantillons|all (?:the )?dates|all samples)\b)"));
    static const QRegularExpression other(QStringLiteral(
        R"(\b(sauf|except\w*|hormis|ceux|celles?|celui|dont|qui|ou |us|phase\w*|niveau\w*|couche\w*|site\w*|sondage\w*|secteur\w*|structure\w*|)"
        R"(sur (?:os|charbon|bois|coquilles?|graines?|ceramiques?)|materiau\w*|plus (?:ancien|recent)\w*|superieur\w*|inferieur\w*|entre|lignes? \d|)"
        R"(where|only those|which|groupe\w*|par\b|by\b|chaque|each|contrainte\w*|stratigraph\w*)\b|[<>])"));
    static const QList<QPair<QRegularExpression, QString>> methods = {
        { QRegularExpression(QStringLiteral(R"(\bf14c\b)")), QStringLiteral("f14c") },
        { QRegularExpression(QStringLiteral(R"(\b(14c|c14|radiocarbones?|radiocarbon)\b)")), QStringLiteral("c14") },
        { QRegularExpression(QStringLiteral(R"(\b(tl|osl|luminescence|thermoluminescence)\b)")), QStringLiteral("tl") },
        { QRegularExpression(QStringLiteral(R"(\b(archeomag\w*|archaeomag\w*|paleomag\w*)\b)")), QStringLiteral("am") },
        { QRegularExpression(QStringLiteral(R"(\b(gaussiennes?|gaussian)\b)")), QStringLiteral("gauss") },
        { QRegularExpression(QStringLiteral(R"(\b(uniformes?|uniform)\b)")), QStringLiteral("unif") } };
    QJsonArray ms;
    for (const auto& p : methods) if (p.first.match(q).hasMatch() && !ms.contains(p.second)) ms.append(p.second);
    if (!ref.match(q).hasMatch() && ms.isEmpty()) return {};
    if (other.match(q).hasMatch()) return {};                           // autres critères : le modèle traduit la sélection

    QString action;
    if (QRegularExpression(QStringLiteral(R"(\b(evenement|event|regroup\w*)\b)")).match(q).hasMatch()) action = QStringLiteral("event");
    else if (QRegularExpression(QStringLiteral(R"(\bcombin\w*)")).match(q).hasMatch()) action = QStringLiteral("combine");
    else if (QRegularExpression(QStringLiteral(R"(\b(modele|model)\b)")).match(q).hasMatch()) action = QStringLiteral("model");
    else if (QRegularExpression(QStringLiteral(R"((calibr\w*|\b(?:dater|datez|date|dates)\s+(?:les|la|le|toutes|tous|ces|l')|\bcalcul\w*|\bdate (?:them|all)\b))"))
                 .match(q).hasMatch()) action = QStringLiteral("date");
    else if (QRegularExpression(QStringLiteral(R"(\b(extrai\w*|extract\w*|liste\w*|list|donne\w*|selectionn\w*|select|filtr\w*|garde\w*|montre\w*|affiche\w*|isole\w*|sors|sortir|show|keep|tableau des|table of)\b)"))
                 .match(q).hasMatch()) action = QStringLiteral("extract");
    if (action.isEmpty()) return {};

    QJsonObject o{ { "method", "table" }, { "action", action } };
    if (!ms.isEmpty()) o["methods"] = ms;
    const auto nm = QRegularExpression(QStringLiteral("«\\s*([^»]+?)\\s*»|\"([^\"]+)\"")).match(question);
    if (nm.hasMatch()) o["name"] = nm.captured(1).isEmpty() ? nm.captured(2) : nm.captured(1);
    return o;
}

} // namespace datatable
} // namespace seldon
