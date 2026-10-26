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

#include "FigurePanel.h"

#include <QApplication>
#include <QFileDialog>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QSvgGenerator>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPair>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "../methods/common/Density.h"

namespace seldon {

// ---------------------------------------------------------------- CurveSpec

bool CurveSpec::fromJson(const QJsonObject& o, CurveSpec& out, QString* err)
{
    out = CurveSpec();
    out.title  = o.value("title").toString();
    out.xlabel = o.value("xlabel").toString();
    out.ylabel = o.value("ylabel").toString();
    out.fill   = o.value("fill").toBool(false);
    if (o.contains("hpd")) {
        const QJsonValue hv = o.value("hpd");
        if (hv.isBool()) out.hpdLevel = hv.toBool() ? 95.0 : 0.0;
        else out.hpdLevel = std::clamp(hv.toDouble(95.0), 0.0, 100.0);
    }

    out.xReverse = o.value("xreverse").toBool(false);
    out.yFromZero = o.value("yzero").toBool(true);
    out.showYAxis = o.contains("yaxis") ? o.value("yaxis").toBool(true) : !out.fill;
    const QString layout = o.value("layout").toString().trimmed().toLower();
    out.rows = layout == "rows" || (layout.isEmpty() && out.fill);

    auto toVec = [](const QJsonValue& v) {
        QVector<double> r;
        for (const QJsonValue& x : v.toArray()) r.append(x.toDouble(std::nan("")));
        return r;
    };
    const QVector<double> sharedX = toVec(o.value("x"));
    for (const QJsonValue& sv : o.value("series").toArray()) {
        const QJsonObject s = sv.toObject();
        CurveSeries cs;
        cs.name = s.value("name").toString();
        if (s.contains("color")) {
            const QColor col(s.value("color").toString());
            if (col.isValid()) cs.color = col;
        }
        cs.fill = s.value("fill").toBool(true);
        cs.width = std::clamp(s.value("width").toDouble(1.8), 0.3, 6.0);
        cs.y = toVec(s.value("y"));
        cs.x = s.contains("x") ? toVec(s.value("x")) : sharedX;
        if (cs.x.isEmpty() && !cs.y.isEmpty()) {            // sans x : indices
            for (int i = 0; i < cs.y.size(); ++i) cs.x.append(i);
        }
        if (cs.x.size() != cs.y.size() || cs.y.size() < 2) {
            if (err) *err = QObject::tr("Série « %1 » : x et y doivent avoir la même longueur (≥ 2).").arg(cs.name);
            return false;
        }
        out.series.append(cs);
    }
    if (out.series.isEmpty()) {
        if (err) *err = QObject::tr("Aucune série à tracer.");
        return false;
    }
    return true;
}

// -------------------------------------------------------------- CurveWidget

namespace {

double niceStep(double range, int target)
{
    const double raw = range / std::max(1, target);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double f = raw / mag;
    const double nice = f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10;
    return nice * mag;
}

// Couleurs de ChronoModel (Painting::mainColorDark / mainColorLight, vue de calibration : trait foncé, remplissage clair
// avec une opacité de 100/255). Les séries suivantes utilisent cette palette.
const QColor kCmPen(31, 80, 128);
const QColor kCmBrush(49, 112, 176);
const QColor kPalette[] = { kCmPen, QColor("#e1572a"), QColor("#2aa876"),
                            QColor("#a52ae1"), QColor("#d9a400"), QColor("#2ac3d9") };

// Trait et remplissage de la série d'indice i. En lignes, la couleur par défaut est celle de ChronoModel pour toutes.
void seriesColors(const CurveSeries& s, int i, QColor& pen, QColor& brush, bool rows = false)
{
    if (s.color.isValid())  { pen = s.color; brush = s.color; }
    else if (i == 0 || rows) { pen = kCmPen;  brush = kCmBrush; }
    else                    { pen = kPalette[i % 6]; brush = pen; }
    brush.setAlpha(100);
}


// Valeur interpolée linéairement d'une série (x croissant ou non) en xv.
double valueAt(const CurveSeries& s, double xv)
{
    for (int i = 1; i < s.x.size(); ++i) {
        const double a = s.x[i - 1], b = s.x[i];
        if (std::isnan(a) || std::isnan(b) || std::isnan(s.y[i - 1]) || std::isnan(s.y[i])) continue;
        if ((xv >= a && xv <= b) || (xv >= b && xv <= a))
            return b == a ? s.y[i] : s.y[i - 1] + (s.y[i] - s.y[i - 1]) * (xv - a) / (b - a);
    }
    return 0.0;
}

// Régions HPD (niveau en %) d'une série considérée comme une densité. Vide si le niveau est 0 ou si la série
// n'est pas une densité (valeurs négatives, aire nulle).
QVector<QPair<double, double>> hpdRegions(const CurveSeries& s, double level)
{
    QVector<QPair<double, double>> out;
    if (!(level > 0.0) || level >= 100.0) return out;
    std::vector<std::pair<double, double>> pts;
    for (int i = 0; i < s.x.size(); ++i) {
        if (std::isnan(s.x[i]) || std::isnan(s.y[i])) continue;
        if (s.y[i] < 0.0) return out;
        pts.emplace_back(s.x[i], s.y[i]);
    }
    if (pts.size() < 3) return out;
    std::sort(pts.begin(), pts.end());
    seldon::Density d;
    for (const auto& pt : pts) { d.t.push_back(pt.first); d.v.push_back(pt.second); }
    if (!(seldon::densityArea(d) > 0.0)) return out;
    for (const seldon::Interval& iv : seldon::hpdIntervals(d, level)) out.append({ iv.from, iv.to });
    return out;
}

} // namespace

CurveWidget::CurveWidget(const CurveSpec& spec, QWidget* parent) : QWidget(parent)
{
    setSpec(spec);
}

void CurveWidget::setSpec(const CurveSpec& spec)
{
    spec_ = spec;
    setMinimumSize(280, spec_.stacked() ? std::max(200, 80 + 30 * int(spec_.series.size())) : 200);
    update();
}

QSize CurveWidget::exportSize(const QSize& base) const
{
    if (!spec_.stacked()) return base;
    const double k = base.width() / 800.0;
    return QSize(base.width(), std::max(base.height(), int(std::lround(k * (90 + 70 * spec_.series.size())))));
}

void CurveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    paintTo(p, rect());
}

QImage CurveWidget::renderHiDpi(const QSize& logical, qreal dpr) const
{
    const qreal k = std::max<qreal>(1.0, dpr);
    QImage img(QSize(int(std::ceil(logical.width() * k)), int(std::ceil(logical.height() * k))), QImage::Format_ARGB32);
    img.setDevicePixelRatio(k);
    img.fill(Qt::white);
    QPainter p(&img);
    paintTo(p, QRect(QPoint(0, 0), logical));
    return img;
}

QImage CurveWidget::render(const QSize& size) const
{
    QImage img(size, QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    paintTo(p, QRect(QPoint(0, 0), size));
    return img;
}

QByteArray CurveWidget::renderSvg(const QSize& size, const QString& title) const
{
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    QSvgGenerator gen;
    gen.setOutputDevice(&buf);
    gen.setSize(size);
    gen.setViewBox(QRect(QPoint(0, 0), size));
    gen.setResolution(96);                       // tailles de police cohérentes avec l'écran
    gen.setTitle(title.isEmpty() ? spec_.title : title);
    gen.setDescription(QStringLiteral("SeldonH"));
    QPainter p(&gen);
    paintTo(p, QRect(QPoint(0, 0), size));       // p.device() != this : rendu « export » (fond blanc, texte noir)
    p.end();
    return out;
}

void CurveWidget::paintTo(QPainter& p, const QRect& r) const
{
    p.setRenderHint(QPainter::Antialiasing, true);
    const bool exporting = p.device() != this;
    const QColor bg  = exporting ? Qt::white : palette().color(QPalette::Base);
    const QColor fg  = exporting ? Qt::black : palette().color(QPalette::Text);
    QColor grid = fg; grid.setAlphaF(0.12);
    p.fillRect(r, bg);

    // Bornes des données
    double xmin = 1e300, xmax = -1e300, ymin = spec_.yFromZero ? 0.0 : 1e300, ymax = -1e300;
    for (const CurveSeries& s : spec_.series)
        for (int i = 0; i < s.x.size(); ++i) {
            if (std::isnan(s.x[i]) || std::isnan(s.y[i])) continue;
            xmin = std::min(xmin, s.x[i]); xmax = std::max(xmax, s.x[i]);
            ymin = std::min(ymin, s.y[i]); ymax = std::max(ymax, s.y[i]);
        }
    if (!(xmax > xmin)) { xmax = xmin + 1; }
    if (!(ymax > ymin)) { ymax = ymin + 1; }
    if (!spec_.yFromZero) ymin -= 0.05 * (ymax - ymin);
    ymax += 0.05 * (ymax - ymin);

    const bool stacked = spec_.stacked();
    const bool showY = spec_.showYAxis && !stacked;            // en lignes : une échelle par ligne, pas d'axe commun
    const int left = showY ? 56 : 26, right = 26, top = spec_.title.isEmpty() ? 14 : 34, bottom = 46;
    const QRect pr(r.left() + left, r.top() + top, r.width() - left - right, r.height() - top - bottom);
    if (pr.width() < 20 || pr.height() < 20) return;

    auto X = [&](double x) {
        const double t = (x - xmin) / (xmax - xmin);
        return pr.left() + (spec_.xReverse ? 1.0 - t : t) * pr.width();
    };
    auto Y = [&](double y) { return pr.bottom() - (y - ymin) / (ymax - ymin) * pr.height(); };

    QFont f = p.font();
    p.setPen(fg);
    if (!spec_.title.isEmpty()) {
        QFont tf = f; tf.setBold(true); tf.setPointSizeF(f.pointSizeF() + 1);
        p.setFont(tf);
        p.drawText(QRect(r.left(), r.top() + 4, r.width(), 24), Qt::AlignCenter, spec_.title);
        p.setFont(f);
    }

    // Grille et graduations
    const double sx = niceStep(xmax - xmin, std::max(2, pr.width() / 80));
    const double sy = niceStep(ymax - ymin, std::max(2, pr.height() / 50));
    for (double x = std::ceil(xmin / sx) * sx; x <= xmax + 1e-9; x += sx) {
        p.setPen(grid); p.drawLine(QPointF(X(x), pr.top()), QPointF(X(x), pr.bottom()));
        p.setPen(fg);
        p.drawText(QRectF(X(x) - 30, pr.bottom() + 4, 60, 16), Qt::AlignCenter, QString::number(x, 'g', 6));
    }
    for (double y = std::ceil(ymin / sy) * sy; showY && y <= ymax + 1e-9; y += sy) {
        p.setPen(grid); p.drawLine(QPointF(pr.left(), Y(y)), QPointF(pr.right(), Y(y)));
        p.setPen(fg);
        p.drawText(QRectF(r.left() + 2, Y(y) - 8, left - 8, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(y, 'g', 4));
    }
    p.setPen(fg);
    p.drawRect(pr);
    if (!spec_.xlabel.isEmpty())
        p.drawText(QRect(pr.left(), r.bottom() - 20, pr.width(), 18), Qt::AlignCenter, spec_.xlabel);
    if (showY && !spec_.ylabel.isEmpty()) {
        p.save();
        p.translate(r.left() + 12, pr.center().y());
        p.rotate(-90);
        p.drawText(QRect(-pr.height() / 2, -10, pr.height(), 20), Qt::AlignCenter, spec_.ylabel);
        p.restore();
    }

    // Une courbe (trait + remplissage HPD), avec l'échelle verticale `Yf`.
    auto drawSeries = [&](const CurveSeries& s, int ci, const std::function<double(double)>& Yf) {
        QColor c, bc;
        seriesColors(s, ci, c, bc, stacked);
        QPainterPath path;
        bool started = false;
        for (int i = 0; i < s.x.size(); ++i) {
            if (std::isnan(s.x[i]) || std::isnan(s.y[i])) { started = false; continue; }
            const QPointF pt(X(s.x[i]), Yf(s.y[i]));
            if (!started) { path.moveTo(pt); started = true; } else path.lineTo(pt);
        }
        if (spec_.fill && s.fill) {
            const QColor fc = bc;
            const auto intervals = hpdRegions(s, spec_.hpdLevel);
            std::vector<int> order(s.x.size());
            for (int i = 0; i < int(order.size()); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](int a, int b) { return s.x[a] < s.x[b]; });
            if (intervals.isEmpty()) {                     // pas de HPD (niveau 0 ou série non positive) : toute l'aire
                QPainterPath area = path;
                area.lineTo(X(s.x.last()), Yf(0));
                area.lineTo(X(s.x.first()), Yf(0));
                area.closeSubpath();
                p.fillPath(area, fc);
            } else {
                for (const auto& iv : intervals) {
                    QPainterPath area;
                    area.moveTo(X(iv.first), Yf(0));
                    area.lineTo(X(iv.first), Yf(valueAt(s, iv.first)));
                    for (int i : order)       // par x croissant, même si la série est en ordre décroissant (axe d'âges)
                        if (s.x[i] > iv.first && s.x[i] < iv.second && !std::isnan(s.y[i])) area.lineTo(X(s.x[i]), Yf(s.y[i]));
                    area.lineTo(X(iv.second), Yf(valueAt(s, iv.second)));
                    area.lineTo(X(iv.second), Yf(0));
                    area.closeSubpath();
                    p.fillPath(area, fc);
                }
            }
        }
        p.setPen(QPen(c, s.width));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };

    if (stacked) {
        // Une série par ligne : axe horizontal commun, chaque densité mise à l'échelle de sa ligne, nom en haut à gauche.
        const int n = int(spec_.series.size());
        const double rowH = double(pr.height()) / n;
        for (int k = 0; k < n; ++k) {
            const CurveSeries& s = spec_.series[k];
            const QRectF band(pr.left(), pr.top() + k * rowH, pr.width(), rowH);
            double smax = 0.0;
            for (double v : s.y) if (!std::isnan(v)) smax = std::max(smax, v);
            if (!(smax > 0)) smax = 1.0;
            const double head = std::min(16.0, band.height() * 0.3);       // place du nom au-dessus de la courbe
            auto Yk = [&](double y) { return band.bottom() - y / smax * (band.height() - head - 2); };
            p.save();
            p.setClipRect(band.adjusted(0, 0, 0, 1));
            drawSeries(s, k, Yk);
            p.restore();
            if (k > 0) {
                p.setPen(QPen(fg, 0.6));
                p.drawLine(QPointF(pr.left(), band.top()), QPointF(pr.right(), band.top()));
            }
            if (!s.name.isEmpty()) {
                p.setPen(fg);
                p.drawText(QRectF(band.left() + 6, band.top() + 1, band.width() - 12, head + 1), Qt::AlignLeft | Qt::AlignVCenter,
                           p.fontMetrics().elidedText(s.name, Qt::ElideRight, int(band.width()) - 12));
            }
        }
        return;
    }

    // Courbes superposées
    p.setClipRect(pr.adjusted(-1, -1, 1, 1));
    int ci = 0;
    for (const CurveSeries& s : spec_.series) drawSeries(s, ci++, Y);
    p.setClipping(false);

    // Légende (séries nommées), largeur adaptée au texte le plus long
    int legendW = 60;
    for (const CurveSeries& s : spec_.series)
        legendW = std::max(legendW, p.fontMetrics().horizontalAdvance(s.name));
    legendW = std::min(legendW, pr.width() / 2);
    int ly = pr.top() + 6;
    ci = 0;
    for (const CurveSeries& s : spec_.series) {
        QColor c, bc;
        seriesColors(s, ci++, c, bc);
        if (s.name.isEmpty()) continue;
        const int x0 = pr.right() - legendW - 30;
        p.setPen(QPen(c, 3));
        p.drawLine(x0, ly + 7, x0 + 18, ly + 7);
        p.setPen(fg);
        p.drawText(QRect(x0 + 22, ly, legendW + 4, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   p.fontMetrics().elidedText(s.name, Qt::ElideRight, legendW));
        ly += 16;
    }
}

// -------------------------------------------------------------- FigurePanel

FigurePanel::FigurePanel(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    auto* bar = new QHBoxLayout;
    auto* title = new QLabel(tr("<b>Graphiques</b>"));
    auto* saveBtn = new QPushButton(tr("Enregistrer…"));
    bar->addWidget(title);
    bar->addStretch();
    bar->addWidget(saveBtn);
    lay->addLayout(bar);

    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    tabs_->setDocumentMode(true);
    lay->addWidget(tabs_, 1);

    placeholder_ = new QLabel(tr("Aucune figure.\nLes courbes calculées par SeldonH s'ouvrent ici (clic sur une figure de la conversation)."));
    static_cast<QLabel*>(placeholder_)->setAlignment(Qt::AlignCenter);
    static_cast<QLabel*>(placeholder_)->setWordWrap(true);
    lay->addWidget(placeholder_, 1);

    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i) {
        QWidget* w = tabs_->widget(i);
        tabs_->removeTab(i);
        delete w;
        updatePlaceholder();
    });
    connect(saveBtn, &QPushButton::clicked, this, &FigurePanel::saveCurrent);
    updatePlaceholder();
}

int FigurePanel::count() const { return tabs_->count(); }

void FigurePanel::clearCurves()
{
    for (int i = tabs_->count() - 1; i >= 0; --i) {
        if (!qobject_cast<CurveWidget*>(tabs_->widget(i))) continue;
        QWidget* w = tabs_->widget(i);
        tabs_->removeTab(i);
        delete w;
    }
    updatePlaceholder();
}

void FigurePanel::updatePlaceholder()
{
    const bool empty = tabs_->count() == 0;
    tabs_->setVisible(!empty);
    placeholder_->setVisible(empty);
}

void FigurePanel::showPage(QWidget* page, const QString& title)
{
    int i = tabs_->indexOf(page);
    if (i < 0) i = tabs_->addTab(page, title);
    tabs_->setCurrentIndex(i);
    updatePlaceholder();
}

void FigurePanel::addCurves(const CurveSpec& spec)
{
    auto* w = new CurveWidget(spec);
    lastCurves_ = w;
    const int i = tabs_->addTab(w, spec.title.isEmpty() ? tr("Courbe") : spec.title);
    tabs_->setCurrentIndex(i);
    updatePlaceholder();
}

bool FigurePanel::replaceCurvesJson(QWidget* page, const QJsonObject& o, QString* err)
{
    auto* w = qobject_cast<CurveWidget*>(page);
    const int i = w ? tabs_->indexOf(w) : -1;
    if (i < 0) { if (err) *err = tr("onglet de courbes introuvable"); return false; }
    CurveSpec spec;
    if (!CurveSpec::fromJson(o, spec, err)) return false;
    w->setSpec(spec);
    tabs_->setTabText(i, spec.title.isEmpty() ? tr("Courbe") : spec.title);
    tabs_->setCurrentIndex(i);
    return true;
}

QImage FigurePanel::thumbnailJson(const QJsonObject& o, int width, qreal dpr)
{
    CurveSpec spec;
    if (!CurveSpec::fromJson(o, spec)) return {};
    const CurveWidget w(spec);                  // jamais affiché : sert seulement à dessiner
    const QSize size = w.exportSize(QSize(width, int(std::lround(width * 0.55))));
    return w.renderHiDpi(size, dpr);
}

bool FigurePanel::addCurvesJson(const QJsonObject& o, QString* err)
{
    CurveSpec spec;
    if (!CurveSpec::fromJson(o, spec, err)) return false;
    addCurves(spec);
    return true;
}

void FigurePanel::saveCurrent()
{
    QWidget* w = tabs_->currentWidget();
    if (!qobject_cast<CurveWidget*>(w)) return;     // ex. onglet d'aide : rien à enregistrer
    const QString pngFilter = tr("Image PNG (*.png)"), svgFilter = tr("Image vectorielle SVG (*.svg)");
    QString selected = pngFilter;
    QString path = QFileDialog::getSaveFileName(this, tr("Enregistrer la figure"), tabs_->tabText(tabs_->currentIndex()) + ".png",
                                                pngFilter + ";;" + svgFilter, &selected);
    if (path.isEmpty()) return;
    // Sans extension (ou extension inconnue), le filtre choisi décide.
    const QString suf = QFileInfo(path).suffix().toLower();
    if (suf != "png" && suf != "svg") path += (selected == svgFilter) ? ".svg" : ".png";
    QString err;
    if (!saveCurrentTo(path, &err))
        QMessageBox::warning(this, tr("Enregistrer la figure"), err);
}

bool FigurePanel::saveCurrentTo(const QString& path, QString* err) const
{
    QWidget* w = tabs_->currentWidget();
    auto* c = qobject_cast<CurveWidget*>(w);
    if (!c) { if (err) *err = tr("Aucune figure à enregistrer."); return false; }
    const QString title = tabs_->tabText(tabs_->currentIndex());
    const QSize size = c->exportSize(QSize(1600, 1000));
    if (QFileInfo(path).suffix().compare("svg", Qt::CaseInsensitive) == 0) {
        const QByteArray svg = c->renderSvg(c->exportSize(QSize(800, 500)), title);
        QFile f(path);
        if (svg.isEmpty() || !f.open(QIODevice::WriteOnly) || f.write(svg) != svg.size()) {
            if (err) *err = tr("Impossible d'écrire %1.").arg(path);
            return false;
        }
        return true;
    }
    const QImage img = c->render(size);
    if (img.isNull() || !img.save(path, "PNG")) { if (err) *err = tr("Impossible d'écrire %1.").arg(path); return false; }
    return true;
}

} // namespace seldon
