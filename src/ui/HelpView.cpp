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

#include "HelpView.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QPalette>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextDocument>
#include <QUrl>
#include <QUrlQuery>

namespace seldon {

namespace {

// Texte brut d'un fragment HTML simple (entités usuelles, balises retirées). Un <br> devient un retour à la ligne
// (exemple sur plusieurs lignes, par ex. un tableau CSV) ; les autres blancs sont simplifiés.
QString plainText(QString s)
{
    static const QRegularExpression br(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    QStringList lines;
    for (QString part : s.split(br)) {
        part.remove(QRegularExpression("<[^>]*>"));
        part.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"")
            .replace("&nbsp;", QStringLiteral(" ")).replace("&#39;", "'");
        lines << part.simplified();
    }
    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

const QRegularExpression& exRegex()
{
    // <a class="ex" ...>texte</a> (attributs dans n'importe quel ordre, pas de href attendu)
    static const QRegularExpression re(R"re(<a\b([^>]*\bclass\s*=\s*"ex"[^>]*)>(.*?)</a>)re",
                                       QRegularExpression::DotMatchesEverythingOption);
    return re;
}

} // namespace

HelpView::HelpView(QWidget* parent) : QTextBrowser(parent)
{
    setOpenLinks(false);
    setOpenExternalLinks(false);
    document()->setDefaultStyleSheet(styleSheet(palette()));
    setHtml(toQtHtml(sourceHtml()));
    connect(this, &QTextBrowser::anchorClicked, this, &HelpView::onAnchor);
}

QString HelpView::sourceHtml()
{
    QFile f(QStringLiteral(":/help/aide.html"));
    if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("<p>Aide introuvable.</p>");
    return QString::fromUtf8(f.readAll());
}

QList<HelpView::Example> HelpView::examples(const QString& html)
{
    QList<Example> out;
    static const QRegularExpression methodRe(R"re(data-method\s*=\s*"([^"]*)")re");
    auto it = exRegex().globalMatch(html);
    while (it.hasNext()) {
        const auto m = it.next();
        Example e;
        e.text = plainText(m.captured(2));
        const auto mm = methodRe.match(m.captured(1));
        if (mm.hasMatch()) e.method = mm.captured(1);
        out << e;
    }
    return out;
}

QString HelpView::toQtHtml(const QString& html)
{
    QString s = html;
    s.remove(QRegularExpression("<!--.*?-->", QRegularExpression::DotMatchesEverythingOption));
    s.remove(QRegularExpression("<style\\b.*?</style>", QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption));
    s.remove(QRegularExpression("<script\\b.*?</script>", QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption));

    // Exemples cliquables : <a class="ex">texte</a> -> lien seldon://insert?q=...
    QString out;
    int last = 0;
    auto it = exRegex().globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        const QString text = plainText(m.captured(2));
        const QString href = QStringLiteral("seldon://insert?q=") + QString::fromLatin1(QUrl::toPercentEncoding(text));
        out += QStringLiteral("<a class=\"ex\" href=\"%1\">%2</a>").arg(href, m.captured(2));
        last = m.capturedEnd();
    }
    out += s.mid(last);
    return out;
}

QString HelpView::styleSheet(const QPalette& pal)
{
    const QString link = pal.color(QPalette::Link).name();
    const QString band = pal.color(QPalette::AlternateBase).name();
    const QString text = pal.color(QPalette::Text).name();
    return QStringLiteral(
        "body { color: %1; }"
        "h1 { font-size: 20px; margin-top: 4px; margin-bottom: 2px; }"
        "h2 { font-size: 16px; margin-top: 18px; margin-bottom: 4px; }"
        "h3 { font-size: 13px; margin-top: 10px; margin-bottom: 2px; }"
        "p { margin-top: 4px; margin-bottom: 4px; }"
        "p.lead { margin-top: 0px; }"
        "p.small { font-size: 11px; }"
        "a { color: %2; }"
        "a.ex { color: %2; font-family: Menlo, Consolas, monospace; }"
        "pre, code { font-family: Menlo, Consolas, monospace; }"
        "table { border-collapse: collapse; }"
        "th, td { padding: 4px 8px; }"
        "th { background-color: %3; }"
        "td.note, td.warn { background-color: %3; }"
        "ul.toc { margin-top: 2px; }").arg(text, link, band);
}

QString HelpView::exportForBrowser()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (dir.isEmpty()) return {};
    const QString path = QDir(dir).filePath(QStringLiteral("SeldonH_mode_d_emploi.html"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(sourceHtml().toUtf8());
    return path;
}

void HelpView::onAnchor(const QUrl& url)
{
    if (url.scheme() == QLatin1String("seldon")) {
        if (url.host() == QLatin1String("insert"))
            emit insertRequested(QUrlQuery(url).queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded));
        return;
    }
    if (url.scheme().isEmpty() && url.hasFragment()) { scrollToAnchor(url.fragment()); return; }
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) QDesktopServices::openUrl(url);
}

} // namespace seldon
