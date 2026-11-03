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

#include "Spreadsheet.h"

#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <private/qzipreader_p.h>

#include <algorithm>
#include <cmath>

namespace seldon {
namespace spreadsheet {

namespace {

constexpr int kMaxRows = 20000;
constexpr int kMaxCols = 200;

// Nombre tel qu'Excel l'a enregistré (« 2500 », « 0.85 », « 1.5E-2 ») : entier sans « .0 », sinon 15 chiffres significatifs.
QString numberText(const QString& v)
{
    bool ok = false;
    const double d = v.toDouble(&ok);
    if (!ok || !std::isfinite(d)) return v;
    if (std::abs(d) < 1e15 && d == std::floor(d)) return QString::number(static_cast<long long>(d));
    return QString::number(d, 'g', 15);
}

// Formats de nombre qui affichent une date ou une heure (identifiants intégrés d'Excel, ou format personnalisé).
bool isDateFormat(int id, const QString& code)
{
    if ((id >= 14 && id <= 22) || (id >= 27 && id <= 36) || (id >= 45 && id <= 47) || (id >= 50 && id <= 58)) return true;
    if (code.isEmpty()) return false;
    QString c = code;
    c.remove(QRegularExpression(QStringLiteral(R"("[^"]*"|\[[^\]]*\]|\\.)")));      // textes, couleurs, échappements
    return QRegularExpression(QStringLiteral("[dmyhs]"), QRegularExpression::CaseInsensitiveOption).match(c).hasMatch()
           && !c.contains(QLatin1String("General"), Qt::CaseInsensitive);
}

QString attr(const QXmlStreamReader& x, const char* localName)
{
    for (const QXmlStreamAttribute& a : x.attributes())
        if (a.name() == QLatin1String(localName)) return a.value().toString();
    return {};
}

QString zipText(QZipReader& z, const QString& path)
{
    QString p = path;
    if (p.startsWith(QLatin1Char('/'))) p.remove(0, 1);
    return QString::fromUtf8(z.fileData(p));
}

// ---- xlsx

struct XlsxBook { QStringList names; QStringList targets; };

XlsxBook xlsxBook(QZipReader& z)
{
    XlsxBook b;
    QHash<QString, QString> rel;
    QXmlStreamReader r(zipText(z, QStringLiteral("xl/_rels/workbook.xml.rels")));
    while (!r.atEnd()) {
        if (r.readNext() == QXmlStreamReader::StartElement && r.name() == QLatin1String("Relationship")) {
            QString target = attr(r, "Target");
            if (!target.startsWith(QLatin1Char('/'))) target = QStringLiteral("xl/") + target;
            rel.insert(attr(r, "Id"), target);
        }
    }
    QXmlStreamReader w(zipText(z, QStringLiteral("xl/workbook.xml")));
    while (!w.atEnd()) {
        if (w.readNext() == QXmlStreamReader::StartElement && w.name() == QLatin1String("sheet")) {
            b.names << attr(w, "name");
            b.targets << rel.value(attr(w, "id"));
        }
    }
    return b;
}

QStringList sharedStrings(QZipReader& z)
{
    QStringList out;
    QXmlStreamReader r(zipText(z, QStringLiteral("xl/sharedStrings.xml")));
    QString cur;
    bool inSi = false, inPhonetic = false;
    while (!r.atEnd()) {
        const auto t = r.readNext();
        if (t == QXmlStreamReader::StartElement) {
            if (r.name() == QLatin1String("si")) { inSi = true; cur.clear(); }
            else if (r.name() == QLatin1String("rPh")) inPhonetic = true;
            else if (r.name() == QLatin1String("t") && inSi && !inPhonetic) cur += r.readElementText();
        } else if (t == QXmlStreamReader::EndElement) {
            if (r.name() == QLatin1String("si")) { out << cur; inSi = false; }
            else if (r.name() == QLatin1String("rPh")) inPhonetic = false;
        }
    }
    return out;
}

QSet<int> dateStyles(QZipReader& z)
{
    QSet<int> out;
    QHash<int, QString> custom;
    QXmlStreamReader r(zipText(z, QStringLiteral("xl/styles.xml")));
    bool inXfs = false;
    int xf = 0;
    while (!r.atEnd()) {
        const auto t = r.readNext();
        if (t == QXmlStreamReader::StartElement) {
            if (r.name() == QLatin1String("numFmt")) custom.insert(attr(r, "numFmtId").toInt(), attr(r, "formatCode"));
            else if (r.name() == QLatin1String("cellXfs")) { inXfs = true; xf = 0; }
            else if (r.name() == QLatin1String("xf") && inXfs) {
                const int id = attr(r, "numFmtId").toInt();
                if (isDateFormat(id, custom.value(id))) out.insert(xf);
                ++xf;
            }
        } else if (t == QXmlStreamReader::EndElement && r.name() == QLatin1String("cellXfs")) {
            inXfs = false;
        }
    }
    return out;
}

bool readXlsx(QZipReader& z, const QString& sheet, Sheet& out, QString* error)
{
    const XlsxBook b = xlsxBook(z);
    if (b.names.isEmpty()) { if (error) *error = QStringLiteral("classeur sans feuille (xl/workbook.xml illisible)"); return false; }
    int idx = sheet.isEmpty() ? 0 : int(b.names.indexOf(sheet));
    if (idx < 0) { if (error) *error = QStringLiteral("feuille « %1 » absente (feuilles : %2)").arg(sheet, b.names.join(QStringLiteral(", "))); return false; }
    out.name = b.names[idx];
    const QStringList shared = sharedStrings(z);
    const QSet<int> dates = dateStyles(z);
    QXmlStreamReader r(zipText(z, b.targets.value(idx)));
    QMap<int, QMap<int, QString>> grid;
    int dateCells = 0, nextRow = 1;
    QStringList dateRefs;
    while (!r.atEnd()) {
        if (r.readNext() != QXmlStreamReader::StartElement) continue;
        if (r.name() == QLatin1String("row")) {
            const int rn = attr(r, "r").toInt();
            nextRow = rn > 0 ? rn : nextRow;
            continue;
        }
        if (r.name() != QLatin1String("c")) continue;
        const QString ref = attr(r, "r");
        const QString type = attr(r, "t");
        const int style = attr(r, "s").toInt();
        const int row = ref.isEmpty() ? nextRow : rowNumber(ref);
        int col = ref.isEmpty() ? (grid.value(row).isEmpty() ? 0 : grid.value(row).lastKey() + 1) : columnIndex(ref);
        QString value;
        bool hasValue = false;
        while (!(r.isEndElement() && r.name() == QLatin1String("c")) && !r.atEnd()) {
            r.readNext();
            if (r.isStartElement() && r.name() == QLatin1String("v")) { value = r.readElementText(); hasValue = true; }
            else if (r.isStartElement() && r.name() == QLatin1String("t")) { value += r.readElementText(); hasValue = true; }   // inlineStr
        }
        if (!hasValue) continue;
        if (type == QLatin1String("s")) value = shared.value(value.toInt());
        else if (type == QLatin1String("b")) value = value == QLatin1String("1") ? QStringLiteral("VRAI") : QStringLiteral("FAUX");
        else if (type.isEmpty() || type == QLatin1String("n")) {
            if (dates.contains(style)) { ++dateCells; if (dateRefs.size() < 5) dateRefs << ref; }
            value = numberText(value);
        }
        if (row < 1 || row > kMaxRows || col < 0 || col >= kMaxCols) continue;
        if (!value.trimmed().isEmpty()) grid[row][col] = value;
        nextRow = row;
    }
    if (dateCells > 0)
        out.notes << QStringLiteral("%1 cellule(s) au format date dans Excel (%2…) : leur valeur est un numéro de jour (45000 = 2023), "
                                    "pas un âge ; mettez ces cellules au format Nombre si ce sont des âges").arg(dateCells).arg(dateRefs.join(QStringLiteral(", ")));
    int maxCol = 0;
    for (const auto& cols : grid) if (!cols.isEmpty()) maxCol = std::max(maxCol, cols.lastKey());
    for (auto it = grid.begin(); it != grid.end(); ++it) {
        QStringList l;
        for (int c = 0; c <= maxCol; ++c) l << it.value().value(c);
        out.rows << l;
        out.lineNumbers << it.key();
    }
    return true;
}

// ---- ods

bool readOds(QZipReader& z, const QString& sheet, Sheet& out, QString* error, QStringList* namesOnly)
{
    QXmlStreamReader r(zipText(z, QStringLiteral("content.xml")));
    if (r.atEnd()) { if (error) *error = QStringLiteral("content.xml illisible"); return false; }
    bool want = false, done = false;
    int row = 0, dateCells = 0;
    QStringList cur;
    while (!r.atEnd() && !done) {
        const auto t = r.readNext();
        if (t == QXmlStreamReader::StartElement && r.name() == QLatin1String("table") && r.namespaceUri().contains(QLatin1String("table"))) {
            const QString n = attr(r, "name");
            if (namesOnly) { *namesOnly << n; continue; }
            want = sheet.isEmpty() || n == sheet;
            if (want) { out.name = n; row = 0; }
            continue;
        }
        if (t == QXmlStreamReader::EndElement && r.name() == QLatin1String("table") && want) { done = true; continue; }
        if (!want || namesOnly) continue;
        if (t == QXmlStreamReader::StartElement && r.name() == QLatin1String("table-row")) {
            const int rep = std::max(1, attr(r, "number-rows-repeated").toInt());
            cur.clear();
            // Lecture des cellules de la ligne.
            while (!r.atEnd() && !(r.isEndElement() && r.name() == QLatin1String("table-row"))) {
                r.readNext();
                if (!r.isStartElement() || (r.name() != QLatin1String("table-cell") && r.name() != QLatin1String("covered-table-cell"))) continue;
                const int crep = std::max(1, attr(r, "number-columns-repeated").toInt());
                const QString vtype = attr(r, "value-type");
                QString value;
                if (vtype == QLatin1String("float") || vtype == QLatin1String("percentage") || vtype == QLatin1String("currency"))
                    value = numberText(attr(r, "value"));
                else if (vtype == QLatin1String("date")) { value = attr(r, "date-value"); ++dateCells; }
                QString textValue;
                const QString cellName = r.name().toString();
                while (!r.atEnd() && !(r.isEndElement() && r.name() == cellName)) {
                    r.readNext();
                    if (r.isStartElement() && r.name() == QLatin1String("p")) { if (!textValue.isEmpty()) textValue += QLatin1Char('\n'); }
                    else if (r.isStartElement() && r.name() == QLatin1String("s")) textValue += QString(std::max(1, attr(r, "c").toInt()), QLatin1Char(' '));
                    else if (r.isCharacters()) textValue += r.text();
                }
                if (value.isEmpty()) value = textValue;
                // Colonnes répétées vides (fin de ligne) : non développées.
                const int n = value.isEmpty() ? std::min(crep, kMaxCols - int(cur.size())) : std::min(crep, kMaxCols);
                for (int i = 0; i < n && cur.size() < kMaxCols; ++i) cur << value;
            }
            while (!cur.isEmpty() && cur.last().trimmed().isEmpty()) cur.removeLast();
            if (!cur.isEmpty()) {
                for (int i = 0; i < rep && out.rows.size() < kMaxRows; ++i) { out.rows << cur; out.lineNumbers << row + i + 1; }
            }
            row += rep;
        }
    }
    if (namesOnly) return true;
    if (out.name.isEmpty() && !sheet.isEmpty()) { if (error) *error = QStringLiteral("feuille « %1 » absente").arg(sheet); return false; }
    if (dateCells > 0) out.notes << QStringLiteral("%1 cellule(s) au format date : vérifiez qu'il ne s'agit pas d'âges").arg(dateCells);
    int maxCol = 0;
    for (const QStringList& l : out.rows) maxCol = std::max<int>(maxCol, int(l.size()));
    for (QStringList& l : out.rows) while (l.size() < maxCol) l << QString();
    return true;
}

} // namespace

int columnIndex(const QString& cellRef)
{
    int c = 0;
    for (const QChar ch : cellRef) {
        if (!ch.isLetter()) break;
        c = c * 26 + (ch.toUpper().unicode() - 'A' + 1);
    }
    return c - 1;
}

int rowNumber(const QString& cellRef)
{
    int i = 0;
    while (i < cellRef.size() && cellRef[i].isLetter()) ++i;
    return cellRef.mid(i).toInt();
}

bool isSpreadsheet(const QString& path)
{
    const QString e = QFileInfo(path).suffix().toLower();
    return e == QLatin1String("xlsx") || e == QLatin1String("xlsm") || e == QLatin1String("ods");
}

QStringList sheetNames(const QString& path, QString* error)
{
    QZipReader z(path);
    if (!z.isReadable() || z.status() != QZipReader::NoError) { if (error) *error = QStringLiteral("fichier illisible ou pas au format zip"); return {}; }
    if (QFileInfo(path).suffix().toLower() == QLatin1String("ods")) {
        QStringList n;
        Sheet dummy;
        readOds(z, QString(), dummy, error, &n);
        return n;
    }
    return xlsxBook(z).names;
}

bool read(const QString& path, const QString& sheet, Sheet& out, QString* error)
{
    out = Sheet();
    QZipReader z(path);
    if (!z.isReadable() || z.status() != QZipReader::NoError) {
        if (error) *error = QFileInfo(path).suffix().toLower() == QLatin1String("xls")
                                ? QStringLiteral("ancien format Excel (.xls) : enregistrez le classeur en .xlsx")
                                : QStringLiteral("fichier illisible ou pas au format zip");
        return false;
    }
    const bool ok = QFileInfo(path).suffix().toLower() == QLatin1String("ods") ? readOds(z, sheet, out, error, nullptr)
                                                                                 : readXlsx(z, sheet, out, error);
    if (ok && out.rows.size() >= kMaxRows) out.notes << QStringLiteral("feuille tronquée à %1 lignes").arg(kMaxRows);
    return ok;
}

} // namespace spreadsheet
} // namespace seldon
