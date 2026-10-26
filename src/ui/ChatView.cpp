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

#include "ChatView.h"

#include "../core/MathText.h"

#include <QApplication>
#include <QBuffer>
#include <QUrl>
#include <QPalette>
#include <QScrollBar>
#include <QTextDocument>

namespace seldon {

ChatView::ChatView(QWidget* parent) : QTextBrowser(parent)
{
    setOpenLinks(false);              // les liens seldon:// sont traités par MainWindow (anchorClicked)
    setOpenExternalLinks(false);
    setReadOnly(true);
    // Boîte du dialogue à coins arrondis : cadre dessiné par la feuille de style, zone d'affichage transparente
    // pour que le fond ne déborde pas des coins.
    setFrameShape(QFrame::NoFrame);
    setStyleSheet(QStringLiteral("QTextBrowser { border: 1px solid palette(mid); border-radius: 12px; "
                                 "background: palette(base); padding: 6px; }"));
    viewport()->setAutoFillBackground(false);
    document()->setDefaultStyleSheet(
        "p { margin-top: 4px; margin-bottom: 4px; }"
        "pre, code { font-family: Menlo, Consolas, monospace; }"
        "table { border-collapse: collapse; }"
        "th, td { padding: 3px 8px; }");

    timer_.setSingleShot(true);
    timer_.setInterval(80);           // ~12 rendus/s pendant le streaming
    connect(&timer_, &QTimer::timeout, this, &ChatView::render);
}

QString ChatView::markdownToHtml(const QString& md)
{
    // Formules LaTeX : remplacées par des marqueurs avant Markdown (qui interpréterait _ et *), puis restituées en HTML.
    QStringList formulas;
    const QString protectedMd = math::protect(md, formulas);
    QTextDocument doc;
    doc.setMarkdown(protectedMd);
    const QString full = doc.toHtml();
    const int b = full.indexOf("<body");
    if (b < 0) return math::restore(full, formulas);
    const int s = full.indexOf('>', b) + 1;
    const int e = full.lastIndexOf("</body>");
    return math::restore(full.mid(s, e > s ? e - s : -1), formulas);
}

int ChatView::addUser(const QString& text)
{
    Item it;
    it.kind = Item::User;
    it.html = text.toHtmlEscaped().replace("\n", "<br>");
    items_.append(it);
    render();
    return items_.size() - 1;
}

int ChatView::addAssistant()
{
    Item it;
    it.kind = Item::Assistant;
    it.thinkingOpen = true;           // "réflexion en cours" jusqu'au premier token
    items_.append(it);
    render();
    return items_.size() - 1;
}

void ChatView::addNotice(const QString& html)
{
    Item it;
    it.kind = Item::Notice;
    it.html = html;
    items_.append(it);
    render();
}

void ChatView::setAssistant(int index, const QString& markdown, const QString& thinking,
                            bool thinkingOpen, bool final)
{
    if (index < 0 || index >= items_.size()) return;
    Item& it = items_[index];
    it.html = markdown.isEmpty() ? QString() : markdownToHtml(markdown);
    it.thinking = thinking;
    it.thinkingOpen = thinkingOpen;
    if (final) { timer_.stop(); render(); }
    else scheduleRender();
}

void ChatView::setActions(int index, const QStringList& linksHtml)
{
    if (index < 0 || index >= items_.size()) return;
    items_[index].actions = linksHtml.join(" &nbsp;·&nbsp; ");
    render();
}

void ChatView::addFigure(int id, const QImage& img, const QString& caption)
{
    if (img.isNull()) return;
    figures_[id] = FigureImage{ img, 1, caption };
    Item it;
    it.kind = Item::Figure;
    it.figure = id;
    items_.append(it);
    render();
}

void ChatView::setFigure(int id, const QImage& img)
{
    auto f = figures_.find(id);
    if (f == figures_.end() || img.isNull()) return;
    f->img = img;
    ++f->version;              // nouvelle adresse : le document ne réutilise pas l'ancienne image en cache
    render();
}

QVariant ChatView::loadResource(int type, const QUrl& name)
{
    if (type == QTextDocument::ImageResource && name.scheme() == QLatin1String("seldon-fig")) {
        const auto f = figures_.constFind(name.path().toInt());
        return f == figures_.constEnd() ? QVariant() : QVariant::fromValue(f->img);
    }
    return QTextBrowser::loadResource(type, name);
}

void ChatView::clearAll()
{
    items_.clear();
    figures_.clear();
    timer_.stop();
    render();
}

void ChatView::scheduleRender()
{
    dirty_ = true;
    if (!timer_.isActive()) timer_.start();
}

QString ChatView::bubble(const Item& it, bool forExport) const
{
    const QPalette pal = QApplication::palette();
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    const QString fg = pal.color(QPalette::Text).name();
    const QString asstBg   = dark ? "#2b2b2e" : "#f2f2f4";
    const QString noticeBg = dark ? "#4a3f1f" : "#fff4d6";
    const QString muted    = dark ? "#9a9aa0" : "#777777";

    QString inner, bg, who;
    QString text = fg, label = muted;
    switch (it.kind) {
    case Item::User:
        // Rappel de la question : fond blanc entouré de noir, quel que soit le thème (texte noir pour rester lisible).
        bg = QStringLiteral("#ffffff"); text = QStringLiteral("#000000"); label = QStringLiteral("#555555");
        who = tr("Vous"); inner = it.html; break;
    case Item::Notice:
        bg = noticeBg; who = QString(); inner = it.html; break;
    case Item::Figure: {
        // Vignette cliquable (ouvre la figure dans le panneau de droite) ; à l'export, l'image est intégrée en PNG.
        bg = asstBg; who = QString();
        const FigureImage f = figures_.value(it.figure);
        const QSize sz = f.img.deviceIndependentSize().toSize();
        QString src = QStringLiteral("seldon-fig:%1?v=%2").arg(it.figure).arg(f.version);
        if (forExport) {
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            f.img.save(&buf, "PNG");
            src = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
        }
        const QString img = QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\">").arg(src).arg(sz.width()).arg(sz.height());
        inner = forExport ? img : QStringLiteral("<a href=\"seldon://figure/%1\">%2</a>").arg(it.figure).arg(img);
        inner += QStringLiteral("<br><small style=\"color:%1\">📈 %2%3</small>")
                     .arg(muted, f.caption.toHtmlEscaped(),
                          forExport ? QString() : tr(" — cliquer pour l'ouvrir dans le panneau de droite (zoom, export)"));
        break;
    }
    case Item::Assistant:
        bg = asstBg; who = QStringLiteral("SeldonH");
        if (it.html.isEmpty())
            inner = QStringLiteral("<i style=\"color:%1\">%2</i>")
                        .arg(muted, it.thinkingOpen ? tr("réflexion en cours…") : tr("…"));
        else
            inner = it.html;
        break;
    }
    const QString border = it.kind == Item::User
        ? QStringLiteral(" border=\"1\" style=\"margin-top:8px; border-collapse:collapse; border-style:solid; border-color:#000000\"")
        : QStringLiteral(" style=\"margin-top:8px\"");
    QString out = QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"10\"%3>"
                                 "<tr><td bgcolor=\"%1\" style=\"color:%2\">").arg(bg, text, border);
    if (!who.isEmpty())
        out += QStringLiteral("<b style=\"color:%1\">%2</b><br>").arg(label, who);
    out += inner;
    if (!it.actions.isEmpty())
        out += QStringLiteral("<p style=\"margin-top:8px\">%1</p>").arg(it.actions);
    out += "</td></tr></table>";
    return out;
}

void ChatView::render()
{
    dirty_ = false;
    QScrollBar* bar = verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 8;
    const int  oldValue = bar->value();

    QString html = "<html><body>";
    for (const Item& it : std::as_const(items_)) html += bubble(it);
    html += "</body></html>";
    setHtml(html);

    bar->setValue(atBottom ? bar->maximum() : std::min(oldValue, bar->maximum()));
}

QString ChatView::exportHtml() const
{
    QString html = "<!doctype html><html><head><meta charset=\"utf-8\"><title>SeldonH</title></head>"
                   "<body style=\"font-family:-apple-system,Helvetica,Arial,sans-serif;max-width:900px;margin:2em auto\">";
    for (const Item& it : items_) html += bubble(it, true);
    return html + "</body></html>";
}

} // namespace seldon
