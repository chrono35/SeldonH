// FigurePanel : boîte latérale pour graphiques et images.
//   - images (PNG, JPG...) : glisser-déposer ou bouton "Ouvrir" ;
//   - courbes : bloc ```plot {...}``` produit par l'assistant, ou addCurves().
//
// Format JSON d'un bloc plot :
//   { "title": "Densités a posteriori", "xlabel": "Année", "ylabel": "Densité", "fill": true, "hpd": 95,
//     "series": [ { "name": "US 101", "x": [-520, -519, ...], "y": [0.0, 0.01, ...] } ] }
// "layout": "rows" -> une série par ligne (axe horizontal commun, échelle verticale propre à chaque ligne, comme les
// densités de ChronoModel) ; "overlay" -> séries superposées. Par défaut : "rows" pour des densités ("fill": true), sinon "overlay".
// "x" peut aussi être donné une seule fois au niveau racine pour toutes les séries.
#pragma once

#include <QImage>
#include <QJsonObject>
#include <QPointer>
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
    QColor           color;          // facultatif ("color": "#rrggbb") ; sinon bleu de ChronoModel (1re série, ou toutes en lignes)
    bool             fill = true;    // "fill": false -> trait seul, même si le graphique remplit les HPD
    double           width = 1.8;    // épaisseur du trait ("width")
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
    // Échelle verticale partant de 0 (densités) ; "yzero": false -> ajustée aux données (traces MCMC…).
    bool                 yFromZero = true;
    // Une série par ligne ("layout": "rows").
    bool                 rows = false;
    QVector<CurveSeries> series;

    bool stacked() const { return rows && series.size() > 1; }

    // Retourne false (et err) si le JSON est inexploitable.
    static bool fromJson(const QJsonObject& o, CurveSpec& out, QString* err = nullptr);
};

class CurveWidget : public QWidget {
    Q_OBJECT
public:
    explicit CurveWidget(const CurveSpec& spec, QWidget* parent = nullptr);
    QImage render(const QSize& size) const;      // pour l'export PNG
    // Export vectoriel : document SVG de la courbe (taille en pixels, texte et tracés en vecteurs).
    QByteArray renderSvg(const QSize& size, const QString& title = QString()) const;
    void setSpec(const CurveSpec& spec);
    // Taille d'export : plus haute que `base` quand les séries sont en lignes (≥ 70 px par ligne à l'échelle 800×500).
    QSize exportSize(const QSize& base) const;
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void paintTo(QPainter& p, const QRect& r) const;
    CurveSpec spec_;
};

class ImageView : public QWidget {
    Q_OBJECT
public:
    // `svgSource` : contenu d'origine quand l'image vient d'un fichier SVG (réenregistré tel quel en SVG).
    explicit ImageView(const QImage& img, const QByteArray& svgSource = QByteArray(), QWidget* parent = nullptr);
    QImage image() const { return img_; }
    QByteArray svgSource() const { return svg_; }
    // SVG de l'image : le source d'origine s'il existe, sinon l'image matricielle incorporée dans un SVG.
    QByteArray toSvg(const QString& title = QString()) const;
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QImage img_;
    QByteArray svg_;
};

class FigurePanel : public QWidget {
    Q_OBJECT
public:
    explicit FigurePanel(QWidget* parent = nullptr);

    bool addImageFile(const QString& path);
    void addImage(const QImage& img, const QString& title, const QByteArray& svgSource = QByteArray());
    // Enregistre l'onglet courant (courbe ou image) en PNG ou en SVG selon l'extension de `path`.
    bool saveCurrentTo(const QString& path, QString* err = nullptr) const;
    bool addCurvesJson(const QJsonObject& o, QString* err = nullptr);
    void addCurves(const CurveSpec& spec);
    // Onglet de courbes ajouté en dernier (nul s'il a été fermé).
    QWidget* lastCurves() const { return lastCurves_; }
    // Remplace le contenu d'un onglet de courbes existant (ex. datations supplémentaires de la même question).
    bool replaceCurvesJson(QWidget* page, const QJsonObject& o, QString* err = nullptr);
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
    QPointer<CurveWidget> lastCurves_;
    QWidget*    placeholder_;
    void updatePlaceholder();
};

} // namespace seldon
