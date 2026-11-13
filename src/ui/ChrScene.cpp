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

#include "ChrScene.h"

#include <QBuffer>
#include <QFontMetricsF>
#include <QHash>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSvgGenerator>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "../core/ChrBuilder.h"
#include "../methods/common/TextUtil.h"

namespace seldon {

namespace {

// Géométrie reprise des items de ChronoModel (AbstractItem, EventItem, DateItem, PhaseItem) : largeur 180,
// titre 20, marges 4, barre des phases 20, peau d'un nœud 7.
constexpr qreal kMargin = 60.0, kItemW = 180.0, kTitleH = 22.0, kElt = 4.0, kLineH = 17.0, kPhasesH = 20.0, kNodeSkin = 7.0;
constexpr qreal kPhaseRowH = 22.0, kPhaseTitleH = 26.0;
const QColor kCurveText(77, 37, 121), kCurveBack(243, 231, 255);      // CURVE_COLOR_* (Painting.h)
const QColor kNodeColor(31, 80, 128);                                 // Painting::mainColorDark

QFont titleFont() { QFont f; f.setPixelSize(14); f.setBold(true); return f; }
QFont phaseTitleFont() { QFont f; f.setPixelSize(16); f.setBold(true); return f; }
QFont lineFont()  { QFont f; f.setPixelSize(12); return f; }
QFont detailFont(){ QFont f; f.setPixelSize(11); return f; }

QColor colorOf(const QJsonObject& o, const QColor& def)
{
    if (!o.contains("red") || !o.contains("green") || !o.contains("blue")) return def;
    return QColor(std::clamp(o.value("red").toInt(), 0, 255), std::clamp(o.value("green").toInt(), 0, 255),
                  std::clamp(o.value("blue").toInt(), 0, 255));
}

// Texte lisible sur un fond : noir sur clair, blanc sur foncé (luminance relative).
QColor textOn(const QColor& bg)
{
    const double l = 0.2126 * bg.redF() + 0.7152 * bg.greenF() + 0.0722 * bg.blueF();
    return l > 0.55 ? QColor(20, 20, 20) : QColor(255, 255, 255);
}

QString num(double v) { return QString::number(v, 'g', 6); }

// Résumé court d'une date d'un .chr : « 14C 310±50 BP », « Unif [1980 ; 2000] »…
QString dateDetail(const QJsonObject& d)
{
    const QString pl = d.value("plugin_id").toString().toLower();
    const QJsonObject x = d.value("data").toObject();
    if (pl == "14c")     return QStringLiteral("14C %1±%2 BP").arg(num(x.value("age").toDouble()), num(x.value("error").toDouble()));
    if (pl == "gauss")   return QStringLiteral("Gauss %1±%2").arg(num(x.value("age").toDouble()), num(x.value("error").toDouble()));
    if (pl == "unif")    return QStringLiteral("Unif [%1 ; %2]").arg(num(x.value("min").toDouble()), num(x.value("max").toDouble()));
    if (pl == "tl/osl")  return QStringLiteral("TL/OSL %1±%2").arg(num(x.value("age").toDouble()), num(x.value("error").toDouble()));
    if (pl == "f14c")    return QStringLiteral("F14C %1±%2").arg(num(x.value("fraction").toDouble()), num(x.value("error").toDouble()));
    if (pl == "density") return QStringLiteral("densité");
    if (pl == "am") {                                   // mesure selon le type (data.process_type du plugin am, 1 à 6)
        const int pt = x.value("process_type").toInt();
        const QString i = QStringLiteral("I = %1").arg(num(x.value("inc").toDouble()));
        const QString d = QStringLiteral("D = %1").arg(num(x.value("dec").toDouble()));
        const QString a = QStringLiteral("α95 %1").arg(num(x.value("alpha95").toDouble()));
        const QString f = QStringLiteral("F = %1 ± %2 µT").arg(num(x.value("field").toDouble()), num(x.value("error_f").toDouble()));
        switch (pt) {
        case 1: return QStringLiteral("AM %1 (%2)").arg(i, a);
        case 2: return QStringLiteral("AM %1 (%2, %3)").arg(d, i, a);
        case 3: return QStringLiteral("AM %1").arg(f);
        case 4: return QStringLiteral("AM %1, %2 (%3)").arg(i, d, a);
        case 5: return QStringLiteral("AM %1 (%2), %3").arg(i, a, f);
        case 6: return QStringLiteral("AM %1, %2 (%3), %4").arg(i, d, a, f);
        default: return QStringLiteral("AM");
        }
    }
    return pl;
}

// Ellipse d'une borne (BoundItem de ChronoModel), couchée : demi-axes kOvalX·w/2 et kOvalY·h/2 autour du rectangle
// w × h du contenu. Le rectangle est inscrit quand 1/kOvalX² + 1/kOvalY² = 1 (coins sur l'ellipse) ; kOvalY = 1,3.
constexpr qreal kOvalY = 1.3;
const qreal kOvalX = 1.0 / std::sqrt(1.0 - 1.0 / (kOvalY * kOvalY));

// Point du bord de `r` (rectangle, ou ovale inscrit dans `r` si `oval`) sur la demi-droite qui part de son centre vers `toward`.
QPointF edgePoint(const QRectF& r, const QPointF& toward, bool oval = false)
{
    const QPointF c = r.center(), d = toward - c;
    if (std::abs(d.x()) < 1e-9 && std::abs(d.y()) < 1e-9) return c;
    if (oval) {
        const qreal a = r.width() / 2, b = r.height() / 2;
        return c + d / std::sqrt((d.x() * d.x()) / (a * a) + (d.y() * d.y()) / (b * b));
    }
    const qreal tx = std::abs(d.x()) > 1e-9 ? (r.width() / 2) / std::abs(d.x()) : 1e18;
    const qreal ty = std::abs(d.y()) > 1e-9 ? (r.height() / 2) / std::abs(d.y()) : 1e18;
    return c + d * std::min(tx, ty);
}

} // namespace

QStringList ChrSceneWidget::curveParams(const QJsonObject& e, int process)
{
    // Texte du cadre des paramètres de courbe de ChronoModel (EventItem::paintBoxCurveParameter), noms courts de
    // CurveSettings (Inc, Dec, F, X, Y, Z, Profondeur). Un nœud n'a pas de ligne : il a un double contour.
    const double x = e.value("X_Inc_Depth").toDouble(), y = e.value("Y_Dec").toDouble(), z = e.value("Z_Field").toDouble();
    const double sx = e.value("sX_Alpha95_sDepth").toDouble(), sy = e.value("sY").toDouble(), sz = e.value("sZ_sF").toDouble();
    auto pm = [](const QString& n, double v, double s) { return QStringLiteral("%1 = %2 ± %3").arg(n, num(v), num(s)); };
    auto eq = [](const QString& n, double v) { return QStringLiteral("%1 = %2").arg(n, num(v)); };
    // Codes de ChronoModel (CurveSettings::ProcessType) : 'U' 85, 'P' 80, 'I' 73, 'D' 68, 'F' 70, 'S' 83, 'V' 86, 'K' 75, '2' 50, '3' 51.
    switch (process) {
    case 85: return { pm(QStringLiteral("X"), x, sx) };
    case 80: return { pm(tr("Profondeur"), x, sx) };
    case 73: return { pm(QStringLiteral("Inc"), x, sx) };
    case 68: return { eq(QStringLiteral("Dec"), y), pm(QStringLiteral("Inc"), x, sx) };
    case 70: return { pm(QStringLiteral("F"), z, sz) };
    case 83: return { pm(QStringLiteral("Inc"), x, sx) + QStringLiteral("  ") + eq(QStringLiteral("Dec"), y) };
    case 86: return { pm(QStringLiteral("Inc"), x, sx) + QStringLiteral("  ") + eq(QStringLiteral("Dec"), y), pm(QStringLiteral("F"), z, sz) };
    case 75: return { pm(QStringLiteral("Inc"), x, sx), pm(QStringLiteral("F"), z, sz) };
    case 50: return { pm(QStringLiteral("X"), x, sx), pm(QStringLiteral("Y"), y, sy) };
    case 51: return { pm(QStringLiteral("X"), x, sx), pm(QStringLiteral("Y"), y, sy), pm(QStringLiteral("Z"), z, sz) };
    default: return {};
    }
}

QImage ChrSceneWidget::thumbnail(const QJsonObject& chr, Kind kind, int width, qreal dpr)
{
    const ChrSceneWidget w(chr, kind);           // jamais affiché : sert seulement à dessiner
    const QSize full = w.exportSize();
    const int h = std::clamp(int(std::lround(width * double(full.height()) / std::max(1, full.width()))), 160, 360);
    const qreal k = std::max<qreal>(1.0, dpr);
    QImage img(QSize(int(std::ceil(width * k)), int(std::ceil(h * k))), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    {
        QPainter p(&img);
        p.scale(k, k);
        w.paintScene(p, QRectF(0, 0, width, h), 1.0, QPointF());
    }
    img.setDevicePixelRatio(k);
    return img;
}

ChrSceneWidget::ChrSceneWidget(const QJsonObject& chr, Kind kind, QWidget* parent) : QWidget(parent), kind_(kind)
{
    setMinimumSize(200, 160);
    setMouseTracking(false);
    setToolTip(tr("Molette : zoom · glisser : déplacer · double-clic : vue d'ensemble"));
    setChr(chr);
}

void ChrSceneWidget::setChr(const QJsonObject& chr)
{
    chr_ = chr;
    zoom_ = 1.0;
    pan_ = QPointF();
    build();
    update();
}

bool ChrSceneWidget::hasPhases(const QJsonObject& chr)
{
    return !chr.value("phases").toArray().isEmpty();
}

bool ChrSceneWidget::asksForDrawing(const QString& question)
{
    const QString q = text::fold(question);
    static const QRegularExpression verb(QStringLiteral(
        R"(\b(dessine\w*|affiche\w*|montre\w*|trace\w*|visualis\w*|represent\w*|schematis\w*|draw\w*|show\w*|display\w*|plot\w*|sketch\w*)\b)"));
    static const QRegularExpression object(QStringLiteral(
        R"(\b(modeles?|scenes?|events?|evenements?|phases?|diagrammes?|graphes?|schemas?|stratigraph\w*|chr|models?|contraintes?)\b)"));
    return verb.match(q).hasMatch() && object.match(q).hasMatch();
}

bool ChrSceneWidget::isOnlyDrawing(const QString& question)
{
    if (!asksForDrawing(question) || question.contains(QLatin1Char('?'))) return false;
    const QString q = text::fold(question);
    // Autre demande dans la même phrase : explication, analyse, calcul, modification…
    static const QRegularExpression other(QStringLiteral(
        R"(\b(expliqu\w*|comment\w*|interpret\w*|analys\w*|pourquoi|resum\w*|verifi\w*|corrig\w*|calcul\w*|ajout\w*|modifi\w*|chang\w*)"
        R"(|supprim\w*|critiqu\w*|discut\w*|decri\w*|dis|donne\w*|indique\w*|evalu\w*|compar\w*|explain\w*|comment\w*|interpret\w*)"
        R"(|analy[sz]\w*|why|summar\w*|check\w*|fix\w*|add\w*|modify|change\w*|describe\w*|tell|give|compare\w*|evaluate\w*)\b)"));
    return !other.match(q).hasMatch() && q.size() < 160;
}

QString ChrSceneWidget::stripPlotBlocks(const QString& markdown)
{
    static const QRegularExpression block(QStringLiteral(R"(^[ \t]*```plot[^\n]*\n.*?^[ \t]*```[ \t]*\n?)"),
                                          QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption);
    QString s = markdown;
    s.remove(block);
    return s.trimmed();
}

void ChrSceneWidget::build()
{
    boxes_.clear();
    links_.clear();
    const QJsonArray events = chr_.value("events").toArray();
    const QFontMetricsF tf(titleFont()), pf(phaseTitleFont()), lf(lineFont()), df(detailFont());
    const int process = chr_.value("curve").toObject().value("process_type").toInt(78);

    QHash<int, QJsonObject> phaseById;
    for (const QJsonValue& v : chr_.value("phases").toArray()) phaseById.insert(v.toObject().value("id").toInt(), v.toObject());

    QHash<int, int> index;                                     // id du .chr -> indice de boîte
    // Taille du contenu (comme les items de ChronoModel), puis cadre extérieur : ellipse d'une borne, peau d'un nœud.
    auto addBox = [&](Box b, const QJsonObject& src) {
        qreal w = kItemW, h = 0;
        if (kind_ == Events) {
            w = std::max(w, tf.horizontalAdvance(b.title) + 4 * kElt);
            for (const Line& l : std::as_const(b.lines))
                w = std::max(w, std::max(lf.horizontalAdvance(l.text), df.horizontalAdvance(l.detail)) + 4 * kElt);
            for (const QString& pr : std::as_const(b.params)) w = std::max(w, lf.horizontalAdvance(pr) + 4 * kElt);
            h = kTitleH + b.lines.size() * (2 * kLineH + kElt) + (b.params.isEmpty() ? 0.0 : b.params.size() * kLineH + 4 + kElt)
                + kPhasesH + 2 * kElt;
        } else {
            w = std::max(w, pf.horizontalAdvance(b.title) + 4 * kElt);
            for (const Line& l : std::as_const(b.lines)) w = std::max(w, lf.horizontalAdvance(l.text) + 6 * kElt);
            w = std::max(w, lf.horizontalAdvance(b.footer) + 6 * kElt);
            h = kPhaseTitleH + kElt + b.lines.size() * (kPhaseRowH + kElt) + (b.footer.isEmpty() ? 0.0 : kPhaseRowH + kElt) + kElt;
        }
        const QPointF c(src.value("item_x").toDouble(), src.value("item_y").toDouble());
        b.content = QRectF(c.x() - w / 2, c.y() - h / 2, w, h);
        QRectF r = b.bound ? QRectF(c.x() - kOvalX * w / 2, c.y() - kOvalY * h / 2, kOvalX * w, kOvalY * h) : b.content;
        b.shape = r;
        if (b.node) r.adjust(-kNodeSkin, -kNodeSkin, kNodeSkin, kNodeSkin);
        b.rect = r;
        index[b.id] = boxes_.size();
        boxes_.append(b);
    };

    if (kind_ == Events) {
        for (const QJsonValue& v : events) {
            const QJsonObject e = v.toObject();
            Box b;
            b.id = e.value("id").toInt();
            b.title = e.value("name").toString();
            b.color = colorOf(e, QColor(71, 112, 76));
            b.bound = e.value("type").toInt() == 1;
            b.params = curveParams(e, process);          // borne comprise : avec une courbe, elle porte aussi sa mesure
            b.node = !b.params.isEmpty() && e.value("point_type").toInt() == 1;
            if (b.bound)
                b.lines.append({ tr("Borne"), QStringLiteral("t = %1").arg(num(e.value("known_fixed").toDouble())), b.color });
            // Une case par date, comme DateItem : son nom, puis la mesure (à la place de la miniature de la calibration).
            for (const QJsonValue& dv : e.value("dates").toArray()) {
                const QJsonObject d = dv.toObject();
                b.lines.append({ d.value("name").toString(), dateDetail(d), colorOf(d, QColor(71, 112, 76)) });
            }
            for (const QString& id : e.value("phase_ids").toString().split(',', Qt::SkipEmptyParts))
                if (phaseById.contains(id.trimmed().toInt())) {
                    const QJsonObject ph = phaseById.value(id.trimmed().toInt());
                    b.chips.append({ ph.value("name").toString(), QString(), colorOf(ph, QColor(45, 120, 150)) });
                }
            addBox(b, e);
        }
        for (const QJsonValue& cv : chr_.value("events_constraints").toArray()) {
            const QJsonObject c = cv.toObject();
            const int a = index.value(c.value("bwd_id").toInt(), -1), z = index.value(c.value("fwd_id").toInt(), -1);
            if (a >= 0 && z >= 0 && a != z) links_.append({ a, z, QString() });
        }
    } else {
        QHash<int, QVector<QJsonObject>> members;
        for (const QJsonValue& v : events) {
            const QJsonObject e = v.toObject();
            for (const QString& id : e.value("phase_ids").toString().split(',', Qt::SkipEmptyParts))
                members[id.trimmed().toInt()].append(e);
        }
        for (const QJsonValue& v : chr_.value("phases").toArray()) {
            const QJsonObject ph = v.toObject();
            Box b;
            b.id = ph.value("id").toInt();
            b.title = ph.value("name").toString();
            b.color = colorOf(ph, QColor(45, 120, 150));
            b.footer = ChrBuilder::durationText(ph);       // « Durée ≤ 20 », « Span uniforme », rien si inconnue
            QVector<QJsonObject> ev = members.value(b.id);
            std::stable_sort(ev.begin(), ev.end(), [](const QJsonObject& x, const QJsonObject& y) {
                return x.value("item_y").toDouble() < y.value("item_y").toDouble(); });      // comme PhaseItem : de haut en bas
            for (const QJsonObject& e : std::as_const(ev)) {
                const bool bound = e.value("type").toInt() == 1;
                const int n = e.value("dates").toArray().size();
                const QString det = bound ? tr("borne") : (n == 1 ? tr("1 date") : tr("%1 dates").arg(n));
                b.lines.append({ e.value("name").toString(), det, colorOf(e, QColor(71, 112, 76)), bound });
            }
            addBox(b, ph);
        }
        for (const QJsonValue& cv : chr_.value("phases_constraints").toArray()) {
            const QJsonObject c = cv.toObject();
            const int a = index.value(c.value("bwd_id").toInt(), -1), z = index.value(c.value("fwd_id").toInt(), -1);
            if (a < 0 || z < 0 || a == z) continue;
            links_.append({ a, z, ChrBuilder::hiatusText(c) });               // bulle « hiatus ≥ 50 », comme ArrowItem
        }
    }

    bounds_ = QRectF();
    for (const Box& b : std::as_const(boxes_)) bounds_ = bounds_.isNull() ? b.rect : bounds_.united(b.rect);
    if (bounds_.isNull()) bounds_ = QRectF(0, 0, 400, 200);
    bounds_.adjust(-kMargin, -kMargin, kMargin, kMargin);
}

void ChrSceneWidget::paintScene(QPainter& p, const QRectF& target, qreal zoom, const QPointF& pan) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.fillRect(target, QColor(250, 250, 248));
    if (boxes_.isEmpty()) {
        p.setPen(QColor(90, 90, 90));
        p.setFont(lineFont());
        p.drawText(target, Qt::AlignCenter, kind_ == Events ? tr("Aucun événement dans ce projet.") : tr("Aucune phase dans ce projet."));
        p.restore();
        return;
    }
    const qreal s0 = std::min({ target.width() / bounds_.width(), target.height() / bounds_.height(), 2.0 });
    const qreal s = s0 * zoom;
    p.translate(target.center() + pan);
    p.scale(s, s);
    p.translate(-bounds_.center());

    // Liaisons (sous les boîtes), du plus ancien vers le plus récent. Comme ArrowItem de ChronoModel : la flèche
    // (triangle 15 × 25) est au MILIEU du trait ; avec une bulle de hiatus au milieu, deux flèches au quart et aux trois quarts.
    constexpr qreal arrowW = 15.0, arrowL = 25.0;
    QPen lp(QColor(70, 70, 70), 1.6);
    lp.setCosmetic(true);
    QVector<QPair<QPointF, QString>> bubbles;
    for (const Link& l : links_) {
        const Box& ba = boxes_[l.from];
        const Box& bz = boxes_[l.to];
        const QPointF a = edgePoint(ba.rect, bz.rect.center(), ba.bound), z = edgePoint(bz.rect, ba.rect.center(), bz.bound);
        p.setPen(lp);
        p.drawLine(a, z);
        const QPointF d = z - a;
        const qreal len = std::hypot(d.x(), d.y());
        if (len > 1e-6) {
            const QPointF u = d / len, n(-u.y(), u.x());
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(70, 70, 70));
            const QVector<qreal> at = l.label.isEmpty() ? QVector<qreal>{ 0.5 } : QVector<qreal>{ 0.25, 0.75 };
            for (qreal t : at) {                                       // triangle centré sur le point, pointe vers le récent
                const QPointF m = a + d * t;
                QPolygonF head;
                head << m + u * (arrowL / 2) << m - u * (arrowL / 2) + n * (arrowW / 2) << m - u * (arrowL / 2) - n * (arrowW / 2);
                p.drawPolygon(head);
            }
        }
        if (!l.label.isEmpty()) bubbles.append({ (a + z) / 2, l.label });
    }

    const QFontMetricsF lf(lineFont());
    for (const Box& b : boxes_) {
        const QRectF& cr = b.content.isNull() ? b.rect : b.content;
        const QRectF& sh = b.shape.isNull() ? b.rect : b.shape;
        const QColor front = textOn(b.color);
        QPainterPath path;
        if (b.bound)            path.addEllipse(sh);
        else if (kind_ == Phases) path.addRoundedRect(sh, 10, 10);
        else                    path.addRect(sh);
        QPen border(b.color.darker(150), 1.0);
        border.setCosmetic(true);
        p.setPen(border);
        p.setBrush(b.color);                                          // fond à la couleur de l'élément, comme ChronoModel
        p.drawPath(path);
        if (b.node) {                                                 // nœud de courbe : double contour
            QPen np(kNodeColor, 2.5);
            np.setCosmetic(true);
            p.setPen(np);
            p.setBrush(Qt::NoBrush);
            if (b.bound) p.drawEllipse(b.rect);
            else         p.drawRoundedRect(b.rect, 4, 4);
        }

        if (kind_ == Events) {
            p.setPen(front);
            p.setFont(titleFont());
            const QRectF head(cr.left() + kElt, cr.top() + 2, cr.width() - 2 * kElt, kTitleH);
            p.drawText(head, Qt::AlignCenter, QFontMetricsF(titleFont()).elidedText(b.title, Qt::ElideRight, head.width()));
            qreal y = head.bottom();
            for (const Line& l : b.lines) {                           // case blanche : nom, puis mesure
                const QRectF box(cr.left() + kElt, y, cr.width() - 2 * kElt, 2 * kLineH);
                p.setPen(Qt::NoPen);
                p.setBrush(Qt::white);
                p.drawRect(box);
                p.setPen(QColor(20, 20, 20));
                p.setFont(lineFont());
                p.drawText(QRectF(box.left(), box.top(), box.width(), kLineH), Qt::AlignCenter, l.text);
                if (!l.detail.isEmpty()) {
                    p.setPen(QColor(90, 90, 90));
                    p.setFont(detailFont());
                    p.drawText(QRectF(box.left(), box.top() + kLineH, box.width(), kLineH), Qt::AlignCenter, l.detail);
                }
                y = box.bottom() + kElt;
            }
            if (!b.params.isEmpty()) {                                // cadre des paramètres de courbe (violet de ChronoModel)
                const QRectF frame(cr.left() + kElt, y, cr.width() - 2 * kElt, b.params.size() * kLineH + 4);
                QPen fp(kCurveText, 1);
                fp.setCosmetic(true);
                p.setPen(fp);
                p.setBrush(kCurveBack);
                p.drawRoundedRect(frame, 4, 4);
                p.setPen(kCurveText);
                p.setFont(lineFont());
                qreal py = frame.top() + 2;
                for (const QString& pr : b.params) {
                    p.drawText(QRectF(frame.left(), py, frame.width(), kLineH), Qt::AlignCenter, pr);
                    py += kLineH;
                }
            }
            // Barre des phases en bas (EventItem::paintBoxPhases) : une case par phase, à sa couleur.
            const QRectF bar(cr.left() + kElt, cr.bottom() - kElt - kPhasesH, cr.width() - 2 * kElt, kPhasesH);
            p.setFont(detailFont());
            if (b.chips.isEmpty()) {
                p.setPen(front);
                p.drawText(bar, Qt::AlignCenter, tr("Pas de phase"));
            } else {
                const qreal cw = (bar.width() - (b.chips.size() - 1) * kElt) / b.chips.size();
                for (int i = 0; i < b.chips.size(); ++i) {
                    const QRectF chip(bar.left() + i * (cw + kElt), bar.top(), cw, bar.height());
                    p.setPen(QColor(0, 0, 0));
                    p.setBrush(b.chips[i].color);
                    p.drawRoundedRect(chip, 4, 4);
                    p.setPen(textOn(b.chips[i].color));
                    p.drawText(chip.adjusted(2, 0, -2, 0), Qt::AlignCenter,
                               QFontMetricsF(detailFont()).elidedText(b.chips[i].text, Qt::ElideRight, chip.width() - 4));
                }
            }
        } else {
            // Phase (PhaseItem) : nom, une ligne par Event à sa couleur (borne : forme arrondie), durée en bas.
            p.setPen(front);
            p.setFont(phaseTitleFont());
            const QRectF head(cr.left() + kElt, cr.top() + 2, cr.width() - 2 * kElt, kPhaseTitleH);
            p.drawText(head, Qt::AlignCenter, QFontMetricsF(phaseTitleFont()).elidedText(b.title, Qt::ElideRight, head.width()));
            qreal y = head.bottom() + kElt;
            p.setFont(lineFont());
            for (const Line& l : b.lines) {
                const QRectF row(cr.left() + 2 * kElt, y, cr.width() - 4 * kElt, kPhaseRowH);
                p.setPen(QPen(textOn(l.color), 1));
                p.setBrush(l.color);
                if (l.bound) p.drawRoundedRect(row, row.width() / 7, row.height() / 2);
                else         p.drawRoundedRect(row, 1, 1);
                p.setPen(textOn(l.color));
                p.drawText(row, Qt::AlignCenter, lf.elidedText(l.text, Qt::ElideRight, row.width() - 6));
                y = row.bottom() + kElt;
            }
            if (!b.footer.isEmpty()) {
                const QRectF tpr(cr.left() + 2 * kElt, cr.bottom() - kElt - kPhaseRowH, cr.width() - 4 * kElt, kPhaseRowH);
                p.setPen(Qt::black);
                p.setBrush(Qt::white);
                p.drawRect(tpr);
                p.drawText(tpr, Qt::AlignCenter, b.footer);
            }
        }
    }

    // Bulles des hiatus (au-dessus des boîtes), rectangle blanc comme ArrowItem.
    p.setFont(lineFont());
    for (const auto& bu : std::as_const(bubbles)) {
        const QSizeF sz(lf.horizontalAdvance(bu.second) + 12, lf.height() + 8);
        const QRectF br(bu.first - QPointF(sz.width() / 2, sz.height() / 2), sz);
        p.setPen(QPen(QColor(40, 40, 40), 1));
        p.setBrush(Qt::white);
        p.drawRect(br);
        p.drawText(br, Qt::AlignCenter, bu.second);
    }
    p.restore();
}

void ChrSceneWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    paintScene(p, QRectF(rect()), zoom_, pan_);
}

void ChrSceneWidget::wheelEvent(QWheelEvent* e)
{
    const qreal f = std::pow(1.15, e->angleDelta().y() / 120.0);
    const qreal nz = std::clamp(zoom_ * f, 0.2, 20.0);
    // Zoom autour du pointeur : le point sous la souris reste en place.
    const QPointF c = QRectF(rect()).center();
    const QPointF m = e->position();
    pan_ = (pan_ + c - m) * (nz / zoom_) - (c - m);
    zoom_ = nz;
    update();
    e->accept();
}

void ChrSceneWidget::mousePressEvent(QMouseEvent* e)
{
    lastMouse_ = e->position().toPoint();
    setCursor(Qt::ClosedHandCursor);
}

void ChrSceneWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!(e->buttons() & Qt::LeftButton)) return;
    const QPoint pos = e->position().toPoint();
    pan_ += QPointF(pos - lastMouse_);
    lastMouse_ = pos;
    update();
}

void ChrSceneWidget::mouseDoubleClickEvent(QMouseEvent*)
{
    zoom_ = 1.0;
    pan_ = QPointF();
    unsetCursor();
    update();
}

QSize ChrSceneWidget::exportSize() const
{
    QSizeF s = bounds_.size();
    const qreal big = std::max(s.width(), s.height());
    if (big > 4000) s *= 4000 / big;
    else if (big < 600) s *= 600 / big;
    return QSize(std::max(200, int(std::ceil(s.width()))), std::max(160, int(std::ceil(s.height()))));
}

QImage ChrSceneWidget::render(const QSize& size) const
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    paintScene(p, QRectF(QPointF(0, 0), QSizeF(size)), 1.0, QPointF());
    return img;
}

QByteArray ChrSceneWidget::renderSvg(const QSize& size, const QString& title) const
{
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    QSvgGenerator gen;
    gen.setOutputDevice(&buf);
    gen.setSize(size);
    gen.setViewBox(QRect(QPoint(0, 0), size));
    gen.setTitle(title);
    gen.setDescription(QStringLiteral("SeldonH — %1").arg(kind_ == Events ? tr("scène des Events") : tr("scène des phases")));
    {
        QPainter p(&gen);
        paintScene(p, QRectF(QPointF(0, 0), QSizeF(size)), 1.0, QPointF());
    }
    return buf.data();
}

} // namespace seldon
