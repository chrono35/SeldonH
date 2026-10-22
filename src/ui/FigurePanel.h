// FigurePanel : boîte latérale pour graphiques et images.
//   - images (PNG, JPG...) : glisser-déposer ou bouton "Ouvrir" ;
//   - courbes : bloc ```plot {...}``` produit par l'assistant, ou addCurves().
//
// Format JSON d'un bloc plot :
//   { "title": "Densités a posteriori", "xlabel": "Année", "ylabel": "Densité", "fill": true, "hpd": 95,
//     "series": [ { "name": "US 101", "x": [-520, -519, ...], "y": [0.0, 0.01, ...] } ] }
// "x" peut aussi être donné une seule fois au niveau racine pour toutes les séries.
#pragma once

#include <QImage>
#include <QJsonObject>
#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

class QTabWidget;
class QPainter;
class QDragEnterEvent;
class QDropEvent;

namespace seldon {

struct CurveSeries {
    QString          name;
    QVector<double>  x, y;
    QColor           color;          // facultatif ("color": "#rrggbb") ; sinon bleu de ChronoModel pour la 1re série
};

struct CurveSpec {
    QString              title, xlabel, ylabel;
    bool                 fill = false;
    // Avec fill : seule la région HPD de chaque série est remplie (niveau en %, 95 par défaut ; "hpd": 0 ou false
    // remplit toute l'aire). Les densités calendaires ne sont pas gaussiennes : le remplissage suit les HPD.
    double               hpdLevel = 95.0;
    // Échelle verticale (graduations, titre, grille horizontale). Par défaut masquée pour une densité (fill), affichée sinon ;
    // "yaxis": true/false force le choix. Une densité n'a pas d'unité utile en ordonnée.
    bool                 showYAxis = true;
    // Axe horizontal inversé (les valeurs décroissent vers la droite) : formats d'âge (cal BP…) en ordre chronologique.
    bool                 xReverse = false;
    QVector<CurveSeries> series;

    // Retourne false (et err) si le JSON est inexploitable.
    static bool fromJson(const QJsonObject& o, CurveSpec& out, QString* err = nullptr);
};

class CurveWidget : public QWidget {
    Q_OBJECT
public:
    explicit CurveWidget(const CurveSpec& spec, QWidget* parent = nullptr);
    QImage render(const QSize& size) const;      // pour l'export PNG
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void paintTo(QPainter& p, const QRect& r) const;
    CurveSpec spec_;
};

class ImageView : public QWidget {
    Q_OBJECT
public:
    explicit ImageView(const QImage& img, QWidget* parent = nullptr);
    QImage image() const { return img_; }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QImage img_;
};

class FigurePanel : public QWidget {
    Q_OBJECT
public:
    explicit FigurePanel(QWidget* parent = nullptr);

    bool addImageFile(const QString& path);
    void addImage(const QImage& img, const QString& title);
    bool addCurvesJson(const QJsonObject& o, QString* err = nullptr);
    void addCurves(const CurveSpec& spec);
    int  count() const;
    void clearCurves();                  // retire les onglets de courbes (garde les images et l'aide)
    // Ajoute (ou active si déjà présent) un onglet quelconque, par ex. l'aide. Le panneau en prend la propriété.
    void showPage(QWidget* page, const QString& title);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private slots:
    void openImage();
    void saveCurrent();

private:
    QTabWidget* tabs_;
    QWidget*    placeholder_;
    void updatePlaceholder();
};

} // namespace seldon
