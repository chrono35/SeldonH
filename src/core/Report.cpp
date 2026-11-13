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

#include "Report.h"

#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocument>
#include <QTextDocumentFragment>

#include "ChrBuilder.h"

namespace seldon {
namespace report {

namespace {

// Lignes vides en trop, espaces de fin de ligne.
QString tidy(QString s)
{
    static const QRegularExpression trailing(QStringLiteral("[ \\t\\x{00A0}]+\\n"));
    static const QRegularExpression blank(QStringLiteral("\\n{3,}"));
    s.replace(QChar::LineSeparator, QLatin1Char('\n')).replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    s.replace(trailing, QStringLiteral("\n"));
    s.replace(blank, QStringLiteral("\n\n"));
    return s.trimmed();
}

// Liens d'action de l'application (« 💾 Enregistrer le modèle », « Demander un commentaire ») : sans objet hors de SeldonH.
QString stripActionLinks(QString html)
{
    static const QRegularExpression action(QStringLiteral(R"RX(<a\b[^>]*href="seldon://[^"]*"[^>]*>.*?</a>)RX"),
                                           QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    html.replace(action, QString());
    return html;
}

QString markdownText(const QString& md)
{
    QTextDocument d;
    d.setMarkdown(md);
    return tidy(d.toPlainText());
}

QString markdownHtml(const QString& md)
{
    QTextDocument d;
    d.setMarkdown(md);
    const QString h = d.toHtml();
    const qsizetype b = h.indexOf(QLatin1String("<body"));
    const qsizetype b1 = b < 0 ? -1 : h.indexOf(QLatin1Char('>'), b);
    const qsizetype e = h.lastIndexOf(QLatin1String("</body>"));
    return (b1 < 0 || e < b1) ? md.toHtmlEscaped() : h.mid(b1 + 1, e - b1 - 1);
}

QString clip(const QString& s, int max)
{
    if (max <= 0 || s.size() <= max) return s;
    return s.left(std::max(0, max - 2)).trimmed() + QStringLiteral(" …");
}

QString plotTitle(const QJsonObject& p)
{
    const QString t = p.value(QLatin1String("title")).toString().trimmed();
    return t.isEmpty() ? QStringLiteral("Courbe") : t;
}

QString underline(const QString& s, QChar c)
{
    return s + QLatin1Char('\n') + QString(s.size(), c);
}

} // namespace

QString noticeText(const QString& html)
{
    return tidy(QTextDocumentFragment::fromHtml(stripActionLinks(html)).toPlainText());
}

Report collect(const QVector<DisplayItem>& display, const QStringList& answers)
{
    Report r;
    QSet<QString> seenWarnings;
    QSet<int> seenTurns;
    auto current = [&r]() -> Step& {
        if (r.steps.isEmpty()) r.steps.append(Step());
        return r.steps.last();
    };
    for (const DisplayItem& d : display) {
        switch (d.kind) {
        case DisplayItem::User: {
            Step s;
            s.question = d.text.trimmed();
            r.steps.append(s);
            break;
        }
        case DisplayItem::Notice: {
            const QString text = noticeText(d.text);
            if (text.isEmpty()) break;
            current().notices << stripActionLinks(d.text);
            for (const QString& line : text.split(QLatin1Char('\n'))) {
                const QString l = line.trimmed();
                if (l.contains(QStringLiteral("⚠")) && !seenWarnings.contains(l)) {
                    seenWarnings.insert(l);
                    r.warnings << l;
                }
            }
            break;
        }
        case DisplayItem::Plot:
            current().plots << d.plot;
            break;
        case DisplayItem::Assistant: {
            if (!d.text.trimmed().isEmpty()) current().answers << d.text.trimmed();
            if (d.turn < 0 || d.turn >= answers.size() || seenTurns.contains(d.turn)) break;
            seenTurns.insert(d.turn);
            QJsonObject spec;
            if (!ChrBuilder::extractSpec(answers.at(d.turn), spec)) break;
            const ChrResult b = ChrBuilder::build(spec);
            current().models << (b.ok ? ChrBuilder::summarize(b.chr, 30).trimmed()
                                      : QStringLiteral("Modèle incomplet : %1").arg(b.errors.join(QStringLiteral(" ; "))));
            break;
        }
        }
    }
    // Étape sans question ni contenu (journal vide au début) : rien à rapporter.
    r.steps.erase(std::remove_if(r.steps.begin(), r.steps.end(), [](const Step& s) {
                      return s.question.isEmpty() && s.notices.isEmpty() && s.models.isEmpty() && s.plots.isEmpty();
                  }), r.steps.end());
    return r;
}

QString promptFor(const Report& r, int budget)
{
    const QString head = QStringLiteral(
        "Rédige la SYNTHÈSE de la discussion ci-dessous entre un utilisateur (archéologue ou géologue) et SeldonH, pour un "
        "rapport. Consignes :\n"
        "- en français, 150 à 300 mots, quatre rubriques courtes en gras : **Objet**, **Données**, **Résultats**, "
        "**Points de vigilance et suites** ;\n"
        "- reprends les nombres EXACTEMENT comme ils sont écrits ci-dessous (valeurs, intervalles, unités, format de date) ; "
        "n'en calcule, n'en convertis et n'en arrondis aucun ; n'invente aucune mesure, référence ou résultat ;\n"
        "- un résultat absent du déroulé n'existe pas : dis ce qui reste à faire plutôt que de le deviner ;\n"
        "- n'écris ni bloc de code, ni demande de calcul, ni modèle ChronoModel ;\n"
        "- ce qui vient des fiches de SeldonH est un calcul exact ; ce que l'assistant a écrit est un commentaire.\n\n"
        "=== Déroulé de la discussion ===\n");

    // Contenu par étape ; les éléments longs sont raccourcis pour tenir dans le budget, les questions sont gardées.
    int pieces = 0;
    for (const Step& s : r.steps) pieces += int(s.notices.size() + s.models.size() + s.answers.size());
    int fixed = int(head.size());
    for (const Step& s : r.steps) fixed += int(s.question.size()) + 40;
    const int per = pieces > 0 ? std::max(300, (budget - fixed) / pieces) : budget;

    QString out = head;
    int n = 0;
    for (const Step& s : r.steps) {
        out += s.question.isEmpty() ? QStringLiteral("\n--- Avant la première question ---\n")
                                    : QStringLiteral("\n--- Question %1 de l'utilisateur ---\n%2\n").arg(++n).arg(s.question);
        for (const QString& h : s.notices) out += QStringLiteral("[Fiche de SeldonH]\n%1\n").arg(clip(noticeText(h), per));
        for (const QString& m : s.models) out += QStringLiteral("[Modèle ChronoModel préparé]\n%1\n").arg(clip(m, per));
        for (const QString& a : s.answers) out += QStringLiteral("[Réponse de l'assistant]\n%1\n").arg(clip(markdownText(a), per));
    }
    if (!r.warnings.isEmpty())
        out += QStringLiteral("\n--- Avertissements affichés ---\n%1\n").arg(clip(r.warnings.join(QLatin1Char('\n')), per));
    return clip(out, budget);
}

QString cleanSummary(const QString& answer)
{
    static const QRegularExpression code(QStringLiteral("```.*?(```|$)"), QRegularExpression::DotMatchesEverythingOption);
    QString s = answer;
    s.replace(code, QString());
    return tidy(s);
}

QString toPlainText(const Report& r)
{
    const QLocale fr(QLocale::French);
    QString t = underline(r.title.isEmpty() ? QStringLiteral("Rapport de synthèse SeldonH") : r.title, QLatin1Char('='));
    t += QStringLiteral("\n\nRapport de synthèse produit par SeldonH le %1").arg(fr.toString(QDateTime::currentDateTime(), QLocale::LongFormat));
    if (r.created.isValid()) t += QStringLiteral("\nConversation commencée le %1").arg(fr.toString(r.created, QLocale::LongFormat));
    if (!r.backend.isEmpty()) t += QStringLiteral("\nModèle de langage : %1").arg(r.backend);
    t += QStringLiteral("\nQuestions : %1\n").arg(std::count_if(r.steps.cbegin(), r.steps.cend(), [](const Step& s) { return !s.question.isEmpty(); }));

    t += QLatin1Char('\n') + underline(QStringLiteral("1. Synthèse (rédigée par le modèle de langage)"), QLatin1Char('-')) + QLatin1Char('\n');
    if (r.summary.isEmpty()) {
        t += QStringLiteral("Pas de synthèse rédigée%1.\n").arg(r.summaryNote.isEmpty() ? QString() : QStringLiteral(" : ") + r.summaryNote);
    } else {
        if (!r.summaryModel.isEmpty()) t += QStringLiteral("Rédigée par %1. Les valeurs de référence sont celles du déroulé.\n\n").arg(r.summaryModel);
        t += markdownText(r.summary) + QLatin1Char('\n');
        if (!r.unsourced.isEmpty())
            t += QStringLiteral("\n⚠ Nombre(s) de la synthèse absents du déroulé (à vérifier) : %1\n").arg(r.unsourced.join(QStringLiteral(", ")));
    }

    t += QLatin1Char('\n') + underline(QStringLiteral("2. Déroulé (résultats calculés par SeldonH)"), QLatin1Char('-')) + QLatin1Char('\n');
    int n = 0;
    for (const Step& s : r.steps) {
        t += QLatin1Char('\n') + (s.question.isEmpty() ? QStringLiteral("Avant la première question")
                                                       : QStringLiteral("Question %1 : %2").arg(++n).arg(s.question)) + QLatin1Char('\n');
        for (const QString& h : s.notices) {
            const QString x = noticeText(h);
            t += QStringLiteral("\n") + x + QLatin1Char('\n');
        }
        for (const QString& m : s.models) t += QStringLiteral("\nModèle ChronoModel préparé :\n%1\n").arg(m);
        for (const QJsonObject& p : s.plots) t += QStringLiteral("[Figure : %1]\n").arg(plotTitle(p));
        if (s.notices.isEmpty() && s.models.isEmpty()) t += QStringLiteral("(réponse de l'assistant seulement, voir l'export de la conversation)\n");
    }

    if (!r.warnings.isEmpty()) {
        t += QLatin1Char('\n') + underline(QStringLiteral("3. Avertissements"), QLatin1Char('-')) + QLatin1Char('\n');
        for (const QString& w : r.warnings) t += QStringLiteral("- %1\n").arg(w);
    }
    return t;
}

QString toHtml(const Report& r, const std::function<QString(int, int)>& figureUrl)
{
    const QLocale fr(QLocale::French);
    const QString title = r.title.isEmpty() ? QStringLiteral("Rapport de synthèse SeldonH") : r.title;
    QString h = QStringLiteral("<html><body><h1>%1</h1><p><i>Rapport de synthèse produit par SeldonH le %2")
                    .arg(title.toHtmlEscaped(), fr.toString(QDateTime::currentDateTime(), QLocale::LongFormat).toHtmlEscaped());
    if (r.created.isValid()) h += QStringLiteral("<br>Conversation commencée le %1").arg(fr.toString(r.created, QLocale::LongFormat).toHtmlEscaped());
    if (!r.backend.isEmpty()) h += QStringLiteral("<br>Modèle de langage : %1").arg(r.backend.toHtmlEscaped());
    h += QStringLiteral("</i></p>");

    h += QStringLiteral("<h2>1. Synthèse <small>(rédigée par le modèle de langage)</small></h2>");
    if (r.summary.isEmpty()) {
        h += QStringLiteral("<p><i>Pas de synthèse rédigée%1.</i></p>")
                 .arg(r.summaryNote.isEmpty() ? QString() : QStringLiteral(" : ") + r.summaryNote.toHtmlEscaped());
    } else {
        if (!r.summaryModel.isEmpty())
            h += QStringLiteral("<p><i>Rédigée par %1. Les valeurs de référence sont celles du déroulé.</i></p>").arg(r.summaryModel.toHtmlEscaped());
        h += markdownHtml(r.summary);
        if (!r.unsourced.isEmpty())
            h += QStringLiteral("<p><b>⚠ Nombre(s) de la synthèse absents du déroulé (à vérifier) :</b> %1</p>")
                     .arg(r.unsourced.join(QStringLiteral(", ")).toHtmlEscaped());
    }

    h += QStringLiteral("<h2>2. Déroulé <small>(résultats calculés par SeldonH)</small></h2>");
    int n = 0;
    for (int i = 0; i < r.steps.size(); ++i) {
        const Step& s = r.steps[i];
        h += s.question.isEmpty() ? QStringLiteral("<h3>Avant la première question</h3>")
                                  : QStringLiteral("<h3>Question %1</h3><p style=\"background-color:#eef3fa\">%2</p>")
                                        .arg(++n).arg(s.question.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
        for (const QString& x : s.notices) h += QStringLiteral("<div>%1</div>").arg(x);
        for (const QString& m : s.models)
            h += QStringLiteral("<p><b>Modèle ChronoModel préparé</b></p><pre>%1</pre>").arg(m.toHtmlEscaped());
        for (int k = 0; k < s.plots.size(); ++k) {
            const QString url = figureUrl ? figureUrl(i, k) : QString();
            if (!url.isEmpty()) h += QStringLiteral("<p><img src=\"%1\" width=\"560\"><br><i>%2</i></p>").arg(url, plotTitle(s.plots[k]).toHtmlEscaped());
            else h += QStringLiteral("<p><i>Figure : %1</i></p>").arg(plotTitle(s.plots[k]).toHtmlEscaped());
        }
        if (s.notices.isEmpty() && s.models.isEmpty())
            h += QStringLiteral("<p><i>Réponse de l'assistant seulement (voir l'export de la conversation).</i></p>");
    }

    if (!r.warnings.isEmpty()) {
        h += QStringLiteral("<h2>3. Avertissements</h2><ul>");
        for (const QString& w : r.warnings) h += QStringLiteral("<li>%1</li>").arg(w.toHtmlEscaped());
        h += QStringLiteral("</ul>");
    }
    return h + QStringLiteral("</body></html>");
}

} // namespace report
} // namespace seldon
