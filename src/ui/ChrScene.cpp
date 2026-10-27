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

#include "../methods/common/TextUtil.h"

namespace seldon {

namespace {

constexpr qreal kPad = 10.0, kHeaderH = 26.0, kLineH = 20.0, kMinW = 150.0, kMargin = 60.0;

QFont titleFont() { QFont f; f.setPixelSize(14); f.setBold(true); return f; }
QFont lineFont()  { QFont f; f.setPixelSize(13); return f; }
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
    if (pl == "am")      return QStringLiteral("AM");
    return pl;
}

// Point du bord de `r` sur la demi-droite qui part de son centre vers `toward`.
QPointF edgePoint(const QRectF& r, const QPointF& toward)
{
    const QPointF c = r.center(), d = toward - c;
    if (std::abs(d.x()) < 1e-9 && std::abs(d.y()) < 1e-9) return c;
    const qreal tx = std::abs(d.x()) > 1e-9 ? (r.width() / 2) / std::abs(d.x()) : 1e18;
    const qreal ty = std::abs(d.y()) > 1e-9 ? (r.height() / 2) / std::abs(d.y()) : 1e18;
    return c + d * std::min(tx, ty);
}

} // namespace

QStringList ChrSceneWidget::curveParams(const QJsonObject& e, int process)
{
    const double x = e.value("X_Inc_Depth").toDouble(), y = e.value("Y_Dec").toDouble(), z = e.value("Z_Field").toDouble();
    const double sx = e.value("sX_Alpha95_sDepth").toDouble(), sy = e.value("sY").toDouble(), sz = e.value("sZ_sF").toDouble();
    auto pm = [](double v, double s) { return s > 0 ? QStringLiteral("%1 ± %2").arg(num(v), num(s)) : num(v); };
    QStringList out;
    // Codes de ChronoModel (CurveSettings::ProcessType) : 'U' 85, 'P' 80, 'I' 73, 'D' 68, 'F' 70, 'S' 83, 'V' 86, 'K' 75, '2' 50, '3' 51.
    switch (process) {
    case 85: out << tr("Mesure : %1").arg(pm(x, sx)); break;
    case 80: out << tr("Profondeur : %1").arg(pm(x, sx)); break;
    case 73: out << tr("Inclinaison : %1").arg(num(x)) << QStringLiteral("α95 : %1").arg(num(sx)); break;
    case 68: out << tr("Déclinaison : %1").arg(num(y)) << tr("Inclinaison : %1").arg(num(x)) << QStringLiteral("α95 : %1").arg(num(sx)); break;
    case 70: out << tr("Intensité : %1").arg(pm(z, sz)); break;
    case 83: out << tr("Inclinaison : %1").arg(num(x)) << tr("Déclinaison : %1").arg(num(y)) << QStringLiteral("α95 : %1").arg(num(sx)); break;
    case 86: out << tr("Inclinaison : %1").arg(num(x)) << tr("Déclinaison : %1").arg(num(y)) << QStringLiteral("α95 : %1").arg(num(sx))
                 << tr("Intensité : %1").arg(pm(z, sz)); break;
    case 75: out << tr("Inclinaison : %1").arg(num(x)) << QStringLiteral("α95 : %1").arg(num(sx)) << tr("Intensité : %1").arg(pm(z, sz)); break;
    case 50: out << QStringLiteral("X : %1").arg(pm(x, sx)) << QStringLiteral("Y : %1").arg(pm(y, sy)); break;
    case 51: out << QStringLiteral("X : %1").arg(pm(x, sx)) << QStringLiteral("Y : %1").arg(pm(y, sy)) << QStringLiteral("Z : %1").arg(pm(z, sz)); break;
    default: return {};
    }
    if (e.value("point_type").toInt() == 1) out << tr("Nœud (la courbe passe par ce point)");
    return out;
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
    static const QRegularExpression block(QStringLiteral(R"(^```plot[^\n]*\n.*?^```[ \t]*\n?)"),
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
    const QFontMetricsF tf(titleFont()), lf(lineFont()), df(detailFont());
    const int process = chr_.value("curve").toObject().value("process_type").toInt(78);

    QHash<int, int> index;                                     // id du .chr -> indice de boîte
    auto addBox = [&](Box b, const QJsonObject& src) {
        qreal w = tf.horizontalAdvance(b.title) + 2 * kPad;
        for (const QString& pr : std::as_const(b.params)) w = std::max(w, lf.horizontalAdvance(pr) + 3 * kPad);
        for (const Line& l : std::as_const(b.lines)) {
            qreal lw = kPad + lf.horizontalAdvance(l.text) + kPad;
            if (!l.detail.isEmpty()) lw += 8 + df.horizontalAdvance(l.detail);
            w = std::max(w, lw);
        }
        w = std::max(w, kMinW);
        const qreal h = kHeaderH + (b.params.isEmpty() ? 0.0 : b.params.size() * kLineH + 8)
                        + std::max<qsizetype>(1, b.lines.size()) * kLineH + kPad / 2;
        const QPointF c(src.value("item_x").toDouble(), src.value("item_y").toDouble());
        b.rect = QRectF(c.x() - w / 2, c.y() - h / 2, w, h);
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
            if (b.bound)
                b.lines.append({ tr("Borne"), QStringLiteral("t = %1").arg(num(e.value("known_fixed").toDouble())), b.color });
            for (const QJsonValue& dv : e.value("dates").toArray()) {
                const QJsonObject d = dv.toObject();
                b.lines.append({ d.value("name").toString(), dateDetail(d), colorOf(d, QColor(71, 112, 76)) });
            }
            if (!b.bound) b.params = curveParams(e, process);
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
            for (const QJsonObject& e : members.value(b.id)) {
                const int n = e.value("dates").toArray().size();
                const QString det = e.value("type").toInt() == 1 ? tr("borne") : (n == 1 ? tr("1 date") : tr("%1 dates").arg(n));
                b.lines.append({ e.value("name").toString(), det, colorOf(e, QColor(71, 112, 76)) });
            }
            addBox(b, ph);
        }
        for (const QJsonValue& cv : chr_.value("phases_constraints").toArray()) {
            const QJsonObject c = cv.toObject();
            const int a = index.value(c.value("bwd_id").toInt(), -1), z = index.value(c.value("fwd_id").toInt(), -1);
            if (a < 0 || z < 0 || a == z) continue;
            QString label;
            switch (c.value("gamma_type").toInt(0)) {
            case 1: label = QStringLiteral("γ = %1").arg(num(c.value("gamma_fixed").toDouble())); break;
            case 2: label = QStringLiteral("γ ∈ [%1 ; %2]").arg(num(c.value("gamma_min").toDouble()), num(c.value("gamma_max").toDouble())); break;
            default: break;
            }
            links_.append({ a, z, label });
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

    // Liaisons (sous les boîtes) : flèche du plus ancien vers le plus récent.
    const qreal ah = std::clamp(11.0 / s, 8.0, 80.0);
    QPen lp(QColor(70, 70, 70), 1.6);
    lp.setCosmetic(true);
    for (const Link& l : links_) {
        const QRectF& ra = boxes_[l.from].rect;
        const QRectF& rz = boxes_[l.to].rect;
        const QPointF a = edgePoint(ra, rz.center()), z = edgePoint(rz, ra.center());
        p.setPen(lp);
        p.drawLine(a, z);
        const QPointF d = z - a;
        const qreal len = std::hypot(d.x(), d.y());
        if (len > 1e-6) {
            const QPointF u = d / len, n(-u.y(), u.x());
            QPolygonF head;
            head << z << z - u * ah + n * ah * 0.45 << z - u * ah - n * ah * 0.45;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(70, 70, 70));
            p.drawPolygon(head);
        }
        if (!l.label.isEmpty()) {
            p.setPen(QColor(40, 40, 40));
            p.setFont(detailFont());
            p.drawText(QRectF((a + z) / 2 - QPointF(80, 20), QSizeF(160, 16)), Qt::AlignCenter, l.label);
        }
    }

    // Boîtes : bandeau à la couleur de l'élément, puis une ligne par date (ou par événement pour une phase).
    const QFontMetricsF lf(lineFont());
    for (const Box& b : boxes_) {
        QPen border(b.color.darker(140), 1.6);
        border.setCosmetic(true);
        if (b.bound) border.setStyle(Qt::DashLine);
        QPainterPath path;
        path.addRoundedRect(b.rect, 6, 6);
        p.setPen(border);
        p.setBrush(Qt::white);
        p.drawPath(path);

        const QRectF head(b.rect.left(), b.rect.top(), b.rect.width(), kHeaderH);
        QPainterPath headClip;
        headClip.addRect(head);
        p.setPen(Qt::NoPen);
        p.setBrush(b.color);
        p.drawPath(path.intersected(headClip));                      // haut arrondi, bas droit
        p.setPen(textOn(b.color));
        p.setFont(titleFont());
        p.drawText(head.adjusted(kPad, 0, -kPad, 0), Qt::AlignVCenter | Qt::AlignHCenter, b.title);

        qreal y = head.bottom() + 2;
        if (!b.params.isEmpty()) {                                     // cadre des paramètres de courbe
            const QRectF frame(b.rect.left() + kPad / 2, y + 2, b.rect.width() - kPad, b.params.size() * kLineH + 2);
            QPen fp(b.color.darker(120), 1);
            fp.setCosmetic(true);
            p.setPen(fp);
            p.setBrush(QColor(244, 244, 240));
            p.drawRoundedRect(frame, 3, 3);
            p.setPen(QColor(35, 35, 35));
            p.setFont(lineFont());
            qreal py = frame.top() + 1;
            for (const QString& pr : b.params) {
                p.drawText(QRectF(frame.left() + kPad / 2, py, frame.width() - kPad, kLineH), Qt::AlignVCenter | Qt::AlignLeft, pr);
                py += kLineH;
            }
            y = frame.bottom() + 4;
        }
        for (const Line& l : b.lines) {
            const QRectF txt(b.rect.left() + kPad, y, b.rect.width() - 2 * kPad, kLineH);
            p.setPen(QColor(25, 25, 25));
            p.setFont(lineFont());
            p.drawText(txt, Qt::AlignVCenter | Qt::AlignLeft, l.text);
            if (!l.detail.isEmpty()) {
                p.setPen(QColor(105, 105, 105));
                p.setFont(detailFont());
                p.drawText(txt.adjusted(lf.horizontalAdvance(l.text) + 8, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, l.detail);
            }
            y += kLineH;
        }
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
