#include "MethodRunner.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>

#include "MethodRegistry.h"
#include "TextUtil.h"

namespace seldon {

namespace {

QString intervalsText(const std::vector<Interval>& v, DateFormat fmt)
{
    QStringList items;
    for (const Interval& iv : v) {
        if (iv.prob < 0.005) continue;          // miettes de bruit numérique
        const bool point = std::lround(iv.from) == std::lround(iv.to);
        items << (point ? dateformat::format(iv.from, fmt)
                        : QStringLiteral("%1 → %2").arg(dateformat::format(iv.from, fmt), dateformat::format(iv.to, fmt)))
                     + QStringLiteral(" : %1 %").arg(text::num(iv.prob * 100.0, 1));
    }
    return items.join("\n");
}

QJsonObject plotSeries(const QString& name, const Density& d, DateFormat fmt)
{
    // Ne garde que la partie utile de la densité (> 1e-3 du maximum, marges de 20 pas), ≤ ~400 points.
    const double mx = *std::max_element(d.v.begin(), d.v.end());
    size_t i0 = 0, i1 = d.v.size() - 1;
    while (i0 < i1 && d.v[i0] < mx * 1e-3) ++i0;
    while (i1 > i0 && d.v[i1] < mx * 1e-3) --i1;
    i0 = i0 > 20 ? i0 - 20 : 0;
    i1 = std::min(d.v.size() - 1, i1 + 20);
    const size_t step = std::max<size_t>(1, (i1 - i0 + 1) / 400);
    QJsonArray xs, ys;
    for (size_t k = i0; k <= i1; k += step) {
        xs.append(dateformat::convert(d.t[k], fmt));
        ys.append(d.v[k]);
    }
    QJsonObject s;
    s["name"] = name;
    s["x"] = xs;
    s["y"] = ys;
    return s;
}

} // namespace

RunOutcome MethodRunner::run(const QVector<QJsonObject>& requests, DateFormat fmt)
{
    RunOutcome out;
    if (requests.isEmpty()) return out;
    out.found = true;
    out.count = requests.size();

    const MethodRegistry& reg = MethodRegistry::instance();
    QString html = QStringLiteral("<b>🧮 Calcul fait par SeldonH</b> <i>(méthodes de ChronoModel — valeurs exactes, non générées par le modèle)</i>");
    QString prompt = QStringLiteral("[Résultat de calcul de SeldonH — valeurs exactes, à reprendre telles quelles sans les recalculer ni les contredire]\n");
    QJsonArray series;
    if (fmt != DateFormat::BcAd)
        prompt += QStringLiteral("Format des dates : %1. Toutes les dates ci-dessous sont déjà exprimées dans ce format.\n").arg(dateformat::title(fmt));

    for (const QJsonObject& rq : requests) {
        const QString head = reg.describe(rq);
        const MethodResult r = reg.compute(rq);

        html += QStringLiteral("<p><b>%1</b></p>").arg(head.toHtmlEscaped());
        prompt += QStringLiteral("\n%1\n").arg(head);

        if (!r.ok) {
            html += QStringLiteral("<p><i>Calcul impossible : %1.</i></p>").arg(r.error.toHtmlEscaped());
            prompt += QStringLiteral("Calcul impossible : %1.\n").arg(r.error);
            continue;
        }

        const QString h95 = intervalsText(r.hpd95, fmt), h68 = intervalsText(r.hpd68, fmt);
        html += QStringLiteral("<table cellspacing='0' cellpadding='3'>"
                               "<tr><td valign='top'><b>HPD 95 %</b></td><td>%1</td></tr>"
                               "<tr><td valign='top'><b>HPD 68 %</b></td><td>%2</td></tr>"
                               "<tr><td><b>MAP (mode)</b></td><td>%3</td></tr>"
                               "<tr><td><b>Moyenne ; écart-type</b></td><td>%4 ; %5</td></tr>"
                               "<tr><td><b>Q1 ; Q2 (médiane) ; Q3</b></td><td>%6 ; %7 ; %8</td></tr>"
                               "</table>")
                    .arg(QString(h95).replace('\n', "<br>"), QString(h68).replace('\n', "<br>"),
                         dateformat::format(r.stats.mode, fmt), dateformat::format(r.stats.mean, fmt), dateformat::spread(r.stats.sd, fmt),
                         dateformat::format(r.stats.q1, fmt), dateformat::format(r.stats.median, fmt), dateformat::format(r.stats.q3, fmt));
        for (const QString& w : r.warnings) html += QStringLiteral("<p><i>⚠ %1</i></p>").arg(w.toHtmlEscaped());

        prompt += QStringLiteral("Intervalles HPD à 95 % :\n- %1\n").arg(QString(h95).replace("\n", "\n- "));
        prompt += QStringLiteral("Intervalles HPD à 68 % :\n- %1\n").arg(QString(h68).replace("\n", "\n- "));
        prompt += QStringLiteral("MAP (mode) : %1. Moyenne : %2, écart-type de la densité : %3 (ce n'est pas un intervalle). "
                                 "Quartiles : Q1 = %4, Q2 (médiane) = %5, Q3 = %6.\n")
                      .arg(dateformat::format(r.stats.mode, fmt), dateformat::format(r.stats.mean, fmt), dateformat::spread(r.stats.sd, fmt),
                           dateformat::format(r.stats.q1, fmt), dateformat::format(r.stats.median, fmt), dateformat::format(r.stats.q3, fmt));
        for (const QString& w : r.warnings) prompt += QStringLiteral("Avertissement : %1.\n").arg(w);

        series.append(plotSeries(head, r.density, fmt));
    }

    prompt += QStringLiteral("\nConsigne : présente ces résultats (intervalles, éventuelle multimodalité, précision) "
                             "pour un archéologue et rappelle les hypothèses de la méthode. Le graphique est DÉJÀ tracé par l'application "
                             "dans le panneau de droite (courbe bleue, région HPD à 95 % remplie, sans échelle verticale) : n'écris aucun "
                             "bloc « plot » pour ces densités et ne décris aucun autre élément graphique (couleurs, légende, intervalles "
                             "en rouge…). %1 Résume par les HPD à 95 % et 68 % : jamais de « 1σ » ou « 2σ » pour une densité calibrée. N'invente aucune autre valeur.\n")
                  .arg(fmt == DateFormat::BcAd ? QStringLiteral("Les années sont signées dans les graphiques (négatif = avant J.-C.).")
                                               : QStringLiteral("Les dates sont exprimées en %1.").arg(dateformat::title(fmt)));

    if (!series.isEmpty()) {
        out.plot["title"] = QStringLiteral("Densités calendaires (calcul SeldonH)");
        out.plot["xlabel"] = dateformat::axisLabel(fmt);
        if (dateformat::isAge(fmt)) out.plot["xreverse"] = true;      // ordre chronologique de gauche à droite
        out.plot["ylabel"] = QStringLiteral("Densité de probabilité");
        out.plot["fill"] = true;
        out.plot["yaxis"] = false;       // pas d'échelle verticale pour une densité
        out.plot["hpd"] = 95;            // seule la région HPD à 95 % est remplie
        out.plot["series"] = series;
    }
    out.noticeHtml = html;
    out.promptBlock = prompt;
    return out;
}

bool MethodRunner::isModelRequest(const QString& question)
{
    const QString q = text::fold(question);
    static const QRegularExpression model(QStringLiteral(R"(\b(modele|modeles|modeliser|modelisation)\b|\.chr\b|projet chronomodel)"));
    static const QRegularExpression context(QStringLiteral(
        R"(\b(chronomodel|evenement|evenements|phase|phases|contrainte|contraintes|stratigraph\w*|fais|faire|cree|creer|construi\w*|genere|generer|prepare|exporte\w*)\b)"));
    return model.match(q).hasMatch() && context.match(q).hasMatch();
}

RunOutcome MethodRunner::analyzeQuestion(const QString& question, DateFormat format)
{
    if (isModelRequest(question)) return RunOutcome();
    return run(MethodRegistry::instance().parse(question), dateformat::detect(question, format));
}

QVector<QJsonObject> MethodRunner::extractCalcBlocks(const QString& answer)
{
    QVector<QJsonObject> out;
    static const QRegularExpression re("^```[ \\t]*calcul[ \\t]*\\n(.*?)^```",
                                       QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption);
    auto it = re.globalMatch(answer);
    while (it.hasNext()) {
        const QJsonDocument d = QJsonDocument::fromJson(it.next().captured(1).toUtf8());
        auto take = [&](const QJsonValue& v) {
            if (v.isObject() && out.size() < 6 && !v.toObject().value("method").toString().isEmpty())
                out << v.toObject();
        };
        if (d.isObject()) take(d.object());
        else if (d.isArray()) for (const QJsonValue& v : d.array()) take(v);
    }
    return out;
}

} // namespace seldon
