#include "ChatView.h"

#include "../core/MathText.h"

#include <QApplication>
#include <QPalette>
#include <QScrollBar>
#include <QTextDocument>

namespace seldon {

ChatView::ChatView(QWidget* parent) : QTextBrowser(parent)
{
    setOpenLinks(false);              // les liens seldon:// sont traités par MainWindow (anchorClicked)
    setOpenExternalLinks(false);
    setReadOnly(true);
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

void ChatView::clearAll()
{
    items_.clear();
    timer_.stop();
    render();
}

void ChatView::scheduleRender()
{
    dirty_ = true;
    if (!timer_.isActive()) timer_.start();
}

QString ChatView::bubble(const Item& it) const
{
    const QPalette pal = QApplication::palette();
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    const QString fg = pal.color(QPalette::Text).name();
    const QString userBg   = dark ? "#2d4a6b" : "#dbe8f7";
    const QString asstBg   = dark ? "#2b2b2e" : "#f2f2f4";
    const QString noticeBg = dark ? "#4a3f1f" : "#fff4d6";
    const QString muted    = dark ? "#9a9aa0" : "#777777";

    QString inner, bg, who;
    switch (it.kind) {
    case Item::User:
        bg = userBg; who = tr("Vous"); inner = it.html; break;
    case Item::Notice:
        bg = noticeBg; who = QString(); inner = it.html; break;
    case Item::Assistant:
        bg = asstBg; who = QStringLiteral("SeldonH");
        if (it.html.isEmpty())
            inner = QStringLiteral("<i style=\"color:%1\">%2</i>")
                        .arg(muted, it.thinkingOpen ? tr("réflexion en cours…") : tr("…"));
        else
            inner = it.html;
        break;
    }
    QString out = QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"10\" style=\"margin-top:8px\">"
                                 "<tr><td bgcolor=\"%1\" style=\"color:%2\">").arg(bg, fg);
    if (!who.isEmpty())
        out += QStringLiteral("<b style=\"color:%1\">%2</b><br>").arg(muted, who);
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
    for (const Item& it : items_) html += bubble(it);
    return html + "</body></html>";
}

} // namespace seldon
