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

// ChrSceneWidget : dessin d'un projet ChronoModel (.chr), à la manière de ses deux scènes.
//   - « scène des Events » : une boîte par événement à sa position (item_x, item_y = centre de la boîte), bandeau de titre
//     à la couleur de l'événement, cadre des paramètres de courbe (modèle avec courbe), puis le nom de chacune de ses
//     dates avec un court résumé ; une borne
//     affiche sa valeur fixe ; flèches des contraintes stratigraphiques, du plus ancien (bwd_id) vers le plus récent (fwd_id).
//   - « scène des phases » : une boîte par phase avec les noms des événements qu'elle contient ; flèches de succession.
// Le dessin s'ajuste au panneau ; molette : zoom, glisser : déplacer, double-clic : vue d'ensemble. Export PNG et SVG.
#pragma once

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QRectF>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QPainter;

namespace seldon {

class ChrSceneWidget : public QWidget {
    Q_OBJECT
public:
    enum Kind { Events, Phases };

    struct Line { QString text; QString detail; QColor color; };
    struct Box {
        int          id = -1;
        QString      title;
        QColor       color;
        QStringList  params;               // paramètres de courbe de l'événement (cadre sous le titre)
        QVector<Line> lines;
        QRectF       rect;                 // coordonnées de la scène (celles du .chr)
        bool         bound = false;
    };
    struct Link { int from = -1, to = -1; QString label; };   // indices dans boxes() ; from = plus ancien ; label : hiatus γ

    explicit ChrSceneWidget(const QJsonObject& chr, Kind kind, QWidget* parent = nullptr);
    void setChr(const QJsonObject& chr);
    Kind kind() const { return kind_; }

    const QVector<Box>&  boxes() const { return boxes_; }
    const QVector<Link>& links() const { return links_; }
    QRectF sceneBounds() const { return bounds_; }

    // Taille d'export : la scène à l'échelle 1, bornée entre 600 et 4000 points de côté.
    QSize exportSize() const;
    QImage render(const QSize& size) const;
    QByteArray renderSvg(const QSize& size, const QString& title = QString()) const;

    // Vignette pour la discussion : la scène entière dans `width` points de large (hauteur 160 à 360 points).
    static QImage thumbnail(const QJsonObject& chr, Kind kind, int width, qreal dpr);
    // Paramètres de courbe d'un événement selon le process_type du .chr (vide sans courbe) : « Profondeur : 13 ± 2 »…
    static QStringList curveParams(const QJsonObject& event, int processType);

    // Le projet a-t-il des phases (deuxième dessin) ?
    static bool hasPhases(const QJsonObject& chr);
    // La question demande de dessiner le modèle (« dessine le modèle », « affiche les scènes », « montre les phases »…).
    static bool asksForDrawing(const QString& question);
    // La question ne demande QUE le dessin (« dessine le modèle ») : rien à demander au modèle de langage.
    static bool isOnlyDrawing(const QString& question);
    // Retire d'une réponse les blocs ```plot (le modèle de langage ne sait pas dessiner une scène de .chr).
    static QString stripPlotBlocks(const QString& markdown);

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    void build();
    void paintScene(QPainter& p, const QRectF& target, qreal zoom, const QPointF& pan) const;

    QJsonObject   chr_;
    Kind          kind_;
    QVector<Box>  boxes_;
    QVector<Link> links_;
    QRectF        bounds_;
    qreal         zoom_ = 1.0;
    QPointF       pan_;
    QPoint        lastMouse_;
};

} // namespace seldon
