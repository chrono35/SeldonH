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
#include "../core/DocxWriter.h"

#include <QFile>
#include <QRegularExpression>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QUrl>
#include <QPalette>
#include <QScrollBar>
#include <QTextDocument>
#include <memory>

namespace seldon {

namespace {

// Couleurs de ChronoModel (Painting::mainColorDark / mainColorLight) : bandeau des fiches de calcul.
const char* kCmDark  = "#1f5080";
const char* kCmLight = "#3170b0";
const QString kBaseStyle = QStringLiteral(
    "p { margin-top: 4px; margin-bottom: 4px; }"
    "pre, code { font-family: 'Source Code Pro', Menlo, Consolas, monospace; }"
    "table { border-collapse: collapse; }"
    "th, td { padding: 3px 8px; }");

// Fiche de calcul : marque « <!--seldon-calc:titre--> » (MethodRunner::cardMarker), ou en-tête des anciennes versions
// (« Calcul fait par SeldonH », conversations de l'historique). Renvoie le titre et le contenu sans l'en-tête.
bool calcCard(const QString& html, QString* title, QString* body)
{
    static const QLatin1String mark("<!--seldon-calc:");
    if (html.startsWith(mark)) {
        const qsizetype e = html.indexOf(QLatin1String("-->"), mark.size());
        if (e > 0) {
            *title = html.mid(mark.size(), e - mark.size());
            *body = html.mid(e + 3);
            return true;
        }
    }
    static const QRegularExpression old(QStringLiteral(
        R"RX(^\s*(<p>)?\s*<img src=":/icons/(?:mini_)?seldonh_32\.png"[^>]*>\s*<b>Calcul fait par SeldonH</b>\s*(<i>\(([^<]*)\)</i>)?\s*(</p>)?)RX"));
    const QRegularExpressionMatch m = old.match(html);
    if (!m.hasMatch()) return false;
    *title = m.captured(3).isEmpty() ? QString() : m.captured(3).toHtmlEscaped();
    *body = html.mid(m.capturedEnd());
    return true;
}

enum class NoticeTone { Info, Warning, Error };
NoticeTone toneOf(const QString& html)
{
    const QString h = html.left(160);
    if (h.contains(QLatin1String("<b>Erreur")) || h.contains(QStringLiteral("<b>Échec")) || h.contains(QLatin1String("impossible")))
        return NoticeTone::Error;
    if (h.contains(QChar(0x26A0)) || h.contains(QStringLiteral("<b>Modèle distant incomplet")) || h.contains(QStringLiteral("a refusé")))
        return NoticeTone::Warning;
    return NoticeTone::Info;
}

// Boîte avec un liseré coloré à gauche (QTextDocument ne connaît ni coins arrondis ni bordure d'un seul côté : le liseré
// est une cellule étroite colorée).
QString barBox(const QString& bar, const QString& bg, const QString& fg, const QString& content, const QString& align = QString())
{
    return QStringLiteral("<table width=\"%5\"%6 cellspacing=\"0\" cellpadding=\"0\" style=\"margin-top:10px\"><tr>"
                          "<td width=\"4\" bgcolor=\"%1\"></td>"
                          "<td%2 style=\"color:%3; padding:8px 12px\">%4</td></tr></table>")
        .arg(bar, bg.isEmpty() ? QString() : QStringLiteral(" bgcolor=\"%1\"").arg(bg), fg, content,
             align.isEmpty() ? QStringLiteral("100%") : QStringLiteral("82%"),
             align.isEmpty() ? QString() : QStringLiteral(" align=\"%1\"").arg(align));
}

} // namespace

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
    document()->setDefaultStyleSheet(kBaseStyle);

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
    it.plain = text;
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
    it.final = final;
    if (!final) it.appNote = false;      // le modèle écrit dans la bulle (commentaire demandé) : ce n'est plus une note
    if (final) { timer_.stop(); render(); }
    else scheduleRender();
}

void ChatView::setShowThinking(bool on)
{
    if (showThinking_ == on) return;
    showThinking_ = on;
    render();
}

void ChatView::toggleThinking(int index)
{
    if (index < 0 || index >= items_.size()) return;
    items_[index].thinkingShown = !items_[index].thinkingShown;
    render();
}

void ChatView::setActions(int index, const QStringList& linksHtml)
{
    if (index < 0 || index >= items_.size()) return;
    items_[index].actions = linksHtml.join(" &nbsp;·&nbsp; ");
    render();
}

void ChatView::setSuggestions(int index, const QString& html)
{
    if (index < 0 || index >= items_.size() || items_[index].suggestions == html) return;
    items_[index].suggestions = html;
    render();
}

void ChatView::addFigure(int id, const QImage& img, const QString& caption)
{
    if (img.isNull()) return;
    figures_[id] = FigureImage{ img, 1, caption };
    // Boîte d'accueil : la fiche de calcul la plus récente, s'il n'y a ni question ni réponse du modèle après elle ;
    // sinon la boîte « Graphique » qui termine le dialogue ; sinon une nouvelle boîte.
    for (int i = int(items_.size()) - 1; i >= 0; --i) {
        Item& h = items_[i];
        if (h.kind == Item::User || (h.kind == Item::Assistant && !h.appNote)) break;
        QString t, b;
        if ((h.kind == Item::Notice && calcCard(h.html, &t, &b)) || (h.kind == Item::Figure && i == items_.size() - 1)) {
            h.figs.append(id);
            render();
            return;
        }
    }
    Item it;
    it.kind = Item::Figure;
    it.figure = id;
    it.figs.append(id);
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

QString ChatView::shortModelName(const QString& name, int maxChars)
{
    QString n = name.simplified();
    if (n.size() > maxChars) n = n.left(maxChars - 1).trimmed() + QChar(0x2026);
    return n;
}

int ChatView::lastUserIndex() const
{
    for (int i = int(items_.size()) - 1; i >= 0; --i)
        if (items_[i].kind == Item::User) return i;
    return -1;
}

QString ChatView::userText(int index) const
{
    return index >= 0 && index < items_.size() && items_[index].kind == Item::User ? items_[index].plain : QString();
}

void ChatView::truncate(int from)
{
    if (from < 0 || from >= items_.size()) return;
    for (int i = from; i < items_.size(); ++i)
        for (int id : std::as_const(items_[i].figs)) figures_.remove(id);
    items_.resize(from);
    timer_.stop();
    render();
}

void ChatView::setModelLabel(int index, const QString& label)
{
    if (index < 0 || index >= items_.size()) return;
    items_[index].model = label;
    render();
}

void ChatView::setAppNote(int index, bool on)
{
    if (index < 0 || index >= items_.size() || items_[index].appNote == on) return;
    items_[index].appNote = on;
    render();
}

void ChatView::setEditEnabled(bool on)
{
    if (editEnabled_ == on) return;
    editEnabled_ = on;
    render();
}


namespace {
// Icônes des ressources (:/icons/…) remplacées par des images intégrées (fichier exporté, presse-papiers).
void embedIcons(QString& b)
{
    if (!b.contains(QLatin1String(":/icons/"))) return;
    static const QRegularExpression res(QStringLiteral(R"RX(src="(:/icons/[^"]+\.png)")RX"));
    QRegularExpressionMatchIterator m = res.globalMatch(b);
    QStringList done;
    while (m.hasNext()) {
        const QString path = m.next().captured(1);
        if (done.contains(path)) continue;
        done << path;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly))
            b.replace(path, QStringLiteral("data:image/png;base64,") + QString::fromLatin1(f.readAll().toBase64()));
    }
}
} // namespace

QMimeData* ChatView::cardMimeData(int index) const
{
    if (index < 0 || index >= items_.size()) return nullptr;
    const Item& it = items_[index];
    QString t, b;
    if (!(it.kind == Item::Figure || (it.kind == Item::Notice && calcCard(it.html, &t, &b)))) return nullptr;
    forceLight_ = true;
    QString html = bubble(it, true);                       // images intégrées (data:), sans les liens d'action
    plainFigUrls_ = true;
    const QString docHtml = bubble(it, true);              // même fiche, images en ressources du document (ODF)
    plainFigUrls_ = false;
    forceLight_ = false;
    embedIcons(html);
    auto* m = new QMimeData;
    // Police précisée : sans elle, un traitement de texte prend sa police par défaut (souvent Times).
    m->setHtml(QStringLiteral("<html><head><meta charset=\"utf-8\"></head><body style=\"font-family:'Source Sans 3','Source Sans Pro',"
                              "Arial,Helvetica,sans-serif\">%1</body></html>").arg(html));
    QTextDocument doc;
    doc.setDefaultStyleSheet(kBaseStyle);
    for (int id : it.figs)
        if (const auto f = figures_.constFind(id); f != figures_.constEnd())
            doc.addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("fig_%1.png").arg(id)), QVariant::fromValue(f->img));
    doc.setHtml(docHtml);
    m->setText(doc.toPlainText());
    // Pas de format ODF : l'écrivain ODF de Qt perd le contenu des cellules imbriquées et les fonds, et LibreOffice le
    // préférerait au HTML (qui, lui, garde bandeau, tableaux et graphiques).
    return m;
}

bool ChatView::copyCard(int index) const
{
    QMimeData* m = cardMimeData(index);
    if (!m) return false;
    QApplication::clipboard()->setMimeData(m);
    return true;
}

QString ChatView::figuresHtml(const Item& it, bool forExport, const QString& muted) const
{
    QString out;
    for (int id : it.figs) {
        const auto fi = figures_.constFind(id);
        if (fi == figures_.constEnd()) continue;
        const FigureImage& f = *fi;
        const QSize sz = f.img.deviceIndependentSize().toSize();
        QString src = QStringLiteral("seldon-fig:%1?v=%2").arg(id).arg(f.version);
        if (plainFigUrls_) src = QStringLiteral("fig_%1.png").arg(id);
        else if (forExport) {
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            f.img.save(&buf, "PNG");
            src = QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
        }
        const QString img = QStringLiteral("<img src=\"%1\" width=\"%2\" height=\"%3\">").arg(src).arg(sz.width()).arg(sz.height());
        out += QStringLiteral("<p style=\"margin-top:10px\">%1<br><span style=\"color:%2; font-size:small\">📈 %3%4</span></p>")
                   .arg(forExport || plainFigUrls_ ? img : QStringLiteral("<a href=\"seldon://figure/%1\">%2</a>").arg(id).arg(img),
                        muted, f.caption.toHtmlEscaped(),
                        forExport ? QString() : tr(" — cliquer pour l'ouvrir dans le panneau de droite (zoom, export)"));
    }
    return out;
}

QString ChatView::bubble(const Item& it, bool forExport, bool last, bool firstAi) const
{
    const QPalette pal = QApplication::palette();
    const bool dark = !forceLight_ && pal.color(QPalette::Window).lightness() < 128;
    const QString fg      = dark ? pal.color(QPalette::Text).name() : QStringLiteral("#000000");
    const QString muted   = dark ? "#a0a0a6" : "#6b6b70";
    const QString frame   = dark ? "#c8c8cc" : "#000000";       // cadre noir des fiches, comme les boîtes de ChronoModel
    const QString cardBg  = dark ? "#232326" : "#ffffff";
    const QString aiBg    = dark ? "#2b2b2f" : "#f3f4f6";
    const QString aiBar   = dark ? "#76767c" : "#9a9ca3";
    const QString userBg  = dark ? "#1e1e21" : "#ffffff";
    const QString userBd  = dark ? "#6a6a70" : "#b4b4b8";
    const QString link    = dark ? "#8fb8e8" : kCmLight;

    auto tail = [&](QString& out) {        // liens d'action et suites proposées (ni l'un ni l'autre dans un export)
        if (!it.actions.isEmpty() && !forExport)
            out += QStringLiteral("<p style=\"margin-top:8px; font-size:small\">%1</p>").arg(it.actions);
        if (!it.suggestions.isEmpty() && !forExport)
            out += QStringLiteral("<p style=\"margin-top:4px; color:%1; font-size:small\">%2</p>").arg(muted, it.suggestions);
    };

    switch (it.kind) {
    case Item::User: {
        QString c = QStringLiteral("<p style=\"color:%1; font-size:small; margin-bottom:2px\"><b>Vous</b>").arg(muted);
        if (last && editEnabled_ && !forExport)
            c += QStringLiteral(" &nbsp;·&nbsp; <a href=\"seldon://edit\" style=\"color:%1\" title=\"%2\">%3</a>")
                     .arg(link, tr("Remettre cette question dans la zone de saisie pour la corriger, puis la renvoyer"), tr("✎ Modifier"));
        c += QStringLiteral("</p>") + it.html;
        return QStringLiteral("<table width=\"82%\" align=\"right\" border=\"1\" cellspacing=\"0\" cellpadding=\"0\" "
                              "style=\"margin-top:12px; border-collapse:collapse; border-style:solid; border-color:%1\">"
                              "<tr><td bgcolor=\"%2\" style=\"color:%3; padding:8px 12px\">%4</td></tr></table>")
            .arg(userBd, userBg, fg, c);
    }
    case Item::Notice:
    case Item::Figure: {
        QString title, body;
        const bool calc = it.kind == Item::Notice && calcCard(it.html, &title, &body);
        if (it.kind == Item::Figure) title = tr("Graphique");
        if (calc || it.kind == Item::Figure) {
            // Fiche de calcul (ou graphique) de SeldonH : cadre noir, bandeau bleu de ChronoModel avec l'opération calculée,
            // puis les valeurs et les vignettes des graphiques, dans la même boîte.
            const QString head = QStringLiteral("<img src=\":/icons/mini_seldonh_32.png\" width=\"14\" height=\"14\"> "
                                                "<b>%1</b>%2")
                                     .arg(it.kind == Item::Figure ? tr("SeldonH") : tr("Calcul SeldonH"),
                                          title.isEmpty() ? QString() : QStringLiteral(" &nbsp;·&nbsp; ") + title);
            QString content = body + figuresHtml(it, forExport, muted);
            if (!forExport) {          // « Copier » : toute la fiche, mise en forme, tableaux et vignettes compris
                const int self = int(&it - items_.constData());
                content += QStringLiteral("<p style=\"margin-top:8px; font-size:small\"><a href=\"seldon://copycard/%1\" title=\"%2\">%3</a>%4</p>")
                               .arg(self).arg(tr("Copier la fiche telle quelle (texte mis en forme, tableaux, graphiques)"), tr("Copier"),
                                    it.actions.isEmpty() ? QString() : QStringLiteral(" &nbsp;·&nbsp; ") + it.actions);
                if (!it.suggestions.isEmpty())
                    content += QStringLiteral("<p style=\"margin-top:4px; color:%1; font-size:small\">%2</p>").arg(muted, it.suggestions);
            } else tail(content);
            return QStringLiteral("<table width=\"100%\" border=\"1\" cellspacing=\"0\" cellpadding=\"0\" "
                                  "style=\"margin-top:10px; border-collapse:collapse; border-style:solid; border-color:%1\">"
                                  "<tr><td bgcolor=\"%2\" style=\"color:#ffffff; padding:5px 10px\">%3</td></tr>"
                                  "<tr><td bgcolor=\"%4\" style=\"color:%5; padding:6px 12px 10px 12px\">%6</td></tr></table>")
                .arg(frame, kCmDark, head, cardBg, fg, content);
        }
        // Message de l'application : liseré gris (information), orange (avertissement) ou rouge (erreur), sans fond.
        const NoticeTone tone = toneOf(it.html);
        const QString bar = tone == NoticeTone::Error ? QStringLiteral("#c0392b")
                          : tone == NoticeTone::Warning ? QStringLiteral("#d9822b") : (dark ? QStringLiteral("#5a5a60") : QStringLiteral("#c4c4c8"));
        QString content = it.html;
        tail(content);
        return barBox(bar, QString(), fg, content);
    }
    case Item::Assistant: {
        if (it.appNote) {                  // mode calculatrice : simple note de SeldonH sous la fiche, sans boîte
            QString c = QStringLiteral("<div style=\"color:%1; font-size:small\">%2</div>").arg(muted, it.html);
            tail(c);
            return QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" style=\"margin-top:4px\"><tr>"
                                  "<td style=\"padding:2px 12px\">%1</td></tr></table>").arg(c);
        }
        QString inner;
        if (it.html.isEmpty())
            inner = QStringLiteral("<i style=\"color:%1\">%2</i>")
                        .arg(muted, it.thinkingOpen ? tr("réflexion en cours…") : tr("…"));
        else
            inner = it.html;
        // Raisonnement : en direct pendant la génération, replié une fois la réponse terminée (dépliable).
        if (showThinking_ && !it.thinking.trimmed().isEmpty()) {
            const QString tbody = QStringLiteral("<div style=\"color:%1; font-size:small; margin-left:8px\"><i>%2</i></div>")
                                      .arg(muted, it.thinking.trimmed().toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
            const int words = int(it.thinking.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size());
            const int self = int(&it - items_.constData());
            const bool open = !it.final || it.thinkingShown;
            QString head = !it.final ? tr("💭 Raisonnement en cours")
                                     : tr("%1 Raisonnement (%2 mots)").arg(open ? QStringLiteral("▾") : QStringLiteral("▸")).arg(words);
            if (it.final && !forExport)
                head = QStringLiteral("<a href=\"seldon://thinking/%1\" style=\"color:%2\">%3</a>").arg(self).arg(muted, head);
            const QString block = QStringLiteral("<p style=\"color:%1; font-size:small\">%2</p>").arg(muted, head)
                                  + (open ? tbody : QString());
            if (it.html.isEmpty() && !it.final) inner = block;
            else if (!it.final) inner = block + inner;
            else inner += block;
        }
        QString c = QStringLiteral("<p style=\"color:%1; font-size:small; margin-bottom:2px\"><b>%2</b>%3</p>")
                        .arg(muted, tr("Modèle d'IA"),
                             it.model.isEmpty() ? QString() : QStringLiteral(" &nbsp;·&nbsp; ") + it.model.toHtmlEscaped())
                    + inner;
        if (firstAi && it.final && !it.html.isEmpty())
            c += QStringLiteral("<p style=\"color:%1; font-size:small; margin-top:6px\"><i>%2</i></p>")
                     .arg(muted, tr("Texte rédigé par un modèle d'IA : à vérifier. Les valeurs qui font foi sont celles des fiches « Calcul SeldonH »."));
        tail(c);
        return barBox(aiBar, aiBg, fg, c);
    }
    }
    return QString();
}

void ChatView::render()
{
    dirty_ = false;
    QScrollBar* bar = verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 8;
    const int  oldValue = bar->value();

    const int lastUser = lastUserIndex();
    int firstAi = -1;
    for (int i = 0; i < items_.size() && firstAi < 0; ++i)
        if (items_[i].kind == Item::Assistant && !items_[i].appNote) firstAi = i;
    // Liens : bleu de ChronoModel (plus clair en thème sombre, pour rester lisible).
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    document()->setDefaultStyleSheet(kBaseStyle + QStringLiteral("a { color: %1; }").arg(dark ? QStringLiteral("#8fb8e8") : QString::fromLatin1(kCmLight)));
    QString html = "<html><body>";
    for (int i = 0; i < items_.size(); ++i) html += bubble(items_[i], false, i == lastUser, i == firstAi);
    html += "</body></html>";
    setHtml(html);

    bar->setValue(atBottom ? bar->maximum() : std::min(oldValue, bar->maximum()));
}

QString ChatView::exportHtml() const
{
    forceLight_ = true;                    // page blanche : couleurs du thème clair, même si l'application est en thème sombre
    QString html = "<!doctype html><html><head><meta charset=\"utf-8\"><title>SeldonH</title></head>"
                   "<body style=\"font-family:'Source Sans 3','Source Sans Pro',Arial,Helvetica,sans-serif;max-width:900px;margin:2em auto\">";
    int firstAi = -1;
    for (int i = 0; i < items_.size() && firstAi < 0; ++i)
        if (items_[i].kind == Item::Assistant && !items_[i].appNote) firstAi = i;
    for (int i = 0; i < items_.size(); ++i) {
        QString b = bubble(items_[i], true, false, i == firstAi);
        embedIcons(b);
        html += b;
    }
    forceLight_ = false;
    return html + "</body></html>";
}

QTextDocument* ChatView::exportDocument(QObject* parent) const
{
    forceLight_ = true;
    plainFigUrls_ = true;                  // images en ressources du document (« fig_<id>.png »)
    int firstAi = -1;
    for (int i = 0; i < items_.size() && firstAi < 0; ++i)
        if (items_[i].kind == Item::Assistant && !items_[i].appNote) firstAi = i;
    QString html = QStringLiteral("<html><body>");
    for (int i = 0; i < items_.size(); ++i) html += bubble(items_[i], true, false, i == firstAi);
    html += QStringLiteral("</body></html>");
    plainFigUrls_ = false;
    forceLight_ = false;
    auto* doc = new QTextDocument(parent);
    doc->setDefaultStyleSheet(kBaseStyle + QStringLiteral("a { color: %1; }").arg(QString::fromLatin1(kCmLight)));
    for (auto it = figures_.constBegin(); it != figures_.constEnd(); ++it)
        doc->addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("fig_%1.png").arg(it.key())), QVariant::fromValue(it->img));
    static const QRegularExpression icons(QStringLiteral(R"RX(src="(:/icons/[^"]+\.png)")RX"));
    for (auto m = icons.globalMatch(html); m.hasNext();) {
        const QString path = m.next().captured(1);
        doc->addResource(QTextDocument::ImageResource, QUrl(path), QVariant::fromValue(QImage(path)));
    }
    doc->setHtml(html);
    return doc;
}

bool ChatView::exportDocx(const QString& path, const QString& title, QString* error) const
{
    std::unique_ptr<QTextDocument> doc(exportDocument(nullptr));
    docx::Options o;
    o.title = title;
    return docx::write(*doc, path, o, error);
}

} // namespace seldon
