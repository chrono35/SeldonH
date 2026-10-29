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

#include "Lexicon.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <algorithm>

#include "../methods/common/TextUtil.h"

namespace seldon {

namespace {

// « ß » et « ẞ » n'ont pas de forme décomposée : on les écrit « ss » comme dans le lexique.
QString foldKey(const QString& s)
{
    QString f = text::fold(s);
    f.replace(QChar(0x00DF), QLatin1String("ss"));
    return f.simplified();
}

QString readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(f.readAll()) : QString();
}

bool isWordChar(QChar c) { return c.isLetterOrNumber(); }

} // namespace

int Lexicon::reload(const QString& userDir)
{
    words_.clear();
    prefixes_.clear();
    phrases_.clear();
    addText(readFile(QStringLiteral(":/lexicon/lexique.txt")));
    if (!userDir.isEmpty()) addText(readFile(QDir(userDir).filePath(QStringLiteral("lexique.txt"))));
    return size();
}

void Lexicon::addText(const QString& text)
{
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const int eq = int(line.indexOf(QLatin1Char('=')));
        if (eq <= 0) continue;
        const QString fr = foldKey(line.left(eq));
        if (fr.isEmpty()) continue;
        for (const QString& t : line.mid(eq + 1).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            QString k = foldKey(t);
            if (k.isEmpty() || k == fr) continue;
            if (k.endsWith(QLatin1Char('*'))) {
                k.chop(1);
                if (k.size() >= 3) prefixes_ << qMakePair(k, fr);
            } else if (std::any_of(k.cbegin(), k.cend(), [](QChar c) { return !isWordChar(c); })) {     // « por que », « alter-tiefen-modell »
                phrases_ << qMakePair(k, fr);
            } else {
                words_.insert(k, fr);         // un fichier lu plus tard (utilisateur) remplace la traduction
            }
        }
    }
    auto longerFirst = [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) { return a.first.size() > b.first.size(); };
    std::stable_sort(prefixes_.begin(), prefixes_.end(), longerFirst);
    std::stable_sort(phrases_.begin(), phrases_.end(), longerFirst);
}

QString Lexicon::translate(const QString& question) const
{
    QString q = foldKey(question);
    q.replace(QChar(0x00BF), QLatin1Char(' ')).replace(QChar(0x00A1), QLatin1Char(' '));     // ¿ ¡
    // Expressions d'abord (les plus longues en tête), entre deux limites de mot.
    for (const auto& p : phrases_) {
        for (int i = int(q.indexOf(p.first)); i >= 0; i = int(q.indexOf(p.first, i))) {
            const int e = i + int(p.first.size());
            const bool left = i == 0 || !isWordChar(q.at(i - 1));
            const bool right = e >= q.size() || !isWordChar(q.at(e));
            if (left && right) { q.replace(i, int(p.first.size()), p.second); i += int(p.second.size()); }
            else i = e;
        }
    }
    // Puis mot à mot ; ponctuation, nombres et mots inconnus restent tels quels.
    QString out;
    out.reserve(q.size() + 16);
    int i = 0;
    while (i < q.size()) {
        if (!isWordChar(q.at(i))) { out += q.at(i++); continue; }
        int e = i;
        while (e < q.size() && isWordChar(q.at(e))) ++e;
        const QString w = q.mid(i, e - i);
        QString fr = words_.value(w);
        if (fr.isEmpty())
            for (const auto& p : prefixes_)
                if (w.startsWith(p.first)) { fr = p.second; break; }
        out += fr.isEmpty() ? w : fr;
        i = e;
    }
    return out;
}

} // namespace seldon
