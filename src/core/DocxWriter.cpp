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


#include "DocxWriter.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>
#include <QUrl>
#include <QVector>
#include <algorithm>
#include <cmath>

#include <private/qzipwriter_p.h>

namespace seldon::docx {

namespace {

constexpr int kTwipsPerPx = 15;              // 96 px par pouce, 1440 twips par pouce
constexpr qint64 kEmuPerPx = 9525;           // 914 400 EMU par pouce
constexpr int kPageW = 11906, kPageH = 16838, kMargin = 1134;   // A4, marges de 2 cm
constexpr int kTextW = kPageW - 2 * kMargin;

QString esc(const QString& s)
{
    QString o;
    o.reserve(s.size() + 8);
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if (u == '&') o += QLatin1String("&amp;");
        else if (u == '<') o += QLatin1String("&lt;");
        else if (u == '>') o += QLatin1String("&gt;");
        else if (u == '"') o += QLatin1String("&quot;");
        else if (u < 0x20 && u != '\t' && u != '\n' && u != '\r') continue;     // caractères interdits en XML 1.0
        else if (u == 0xFFFE || u == 0xFFFF) continue;
        else o += c;
    }
    return o;
}

QString hexColor(const QColor& c) { return c.name().mid(1).toUpper(); }

bool solid(const QBrush& b) { return b.style() == Qt::SolidPattern && b.color().alpha() > 0; }

// Fond de cellule ou de paragraphe mélangé au blanc s'il est transparent (Word n'a pas d'opacité).
QString fillOf(const QBrush& b)
{
    QColor c = b.color();
    if (c.alpha() < 255) {
        const double a = c.alphaF();
        c = QColor::fromRgbF(a * c.redF() + (1 - a), a * c.greenF() + (1 - a), a * c.blueF() + (1 - a));
    }
    return hexColor(c);
}

bool isMono(const QTextCharFormat& cf)
{
    if (cf.fontFixedPitch()) return true;
    // fontFamilies seulement : QTextCharFormat::fontFamily() plante sur une liste vide (Qt 6.4).
    const QStringList fam = cf.hasProperty(QTextFormat::FontFamilies) ? cf.fontFamilies().toStringList() : QStringList();
    for (const QString& f : fam) {
        const QString l = f.toLower();
        if (l.contains(QLatin1String("mono")) || l.contains(QLatin1String("code")) || l.contains(QLatin1String("consolas"))
            || l.contains(QLatin1String("menlo")) || l.contains(QLatin1String("courier")))
            return true;
    }
    return false;
}

class Writer {
public:
    Writer(const QTextDocument& d, const Options& o) : doc_(d), opt_(o) {}

    QByteArray run(QString* error);

private:
    struct Media { QString file; QByteArray png; };
    const QTextDocument& doc_;
    const Options& opt_;
    QString body_;
    QVector<Media> media_;
    QStringList rels_;                       // relations ajoutées (images, liens)
    QHash<QString, QString> imageRel_;       // nom de la ressource → rId (une seule copie par image)
    QHash<QString, QString> linkRel_;
    QHash<const QTextList*, int> numIds_;    // une numérotation par liste (les listes numérotées repartent de 1)
    QVector<int> numAbstract_;               // numId - 1 → abstractNum (0 puces, 1 numéros)
    int nextRel_ = 10;
    int nextDrawing_ = 1;

    QString newRel(const QString& type, const QString& target, bool external)
    {
        const QString id = QStringLiteral("rId%1").arg(nextRel_++);
        rels_ << QStringLiteral("<Relationship Id=\"%1\" Type=\"%2\" Target=\"%3\"%4/>")
                     .arg(id, type, esc(target), external ? QStringLiteral(" TargetMode=\"External\"") : QString());
        return id;
    }

    void frame(QTextFrame::iterator it, QTextFrame::iterator end, int availTw, bool* endsWithTable);
    void block(const QTextBlock& b, int availTw);
    void table(QTextTable* t, int availTw);
    QString runProps(const QTextCharFormat& cf) const;
    QString textRuns(const QString& text, const QString& rPr) const;
    QString image(const QTextImageFormat& f, int availTw);
};

// Parcourt une suite de blocs et de cadres (racine du document ou contenu d'une cellule).
void Writer::frame(QTextFrame::iterator it, QTextFrame::iterator end, int availTw, bool* endsWithTable)
{
    bool lastTable = false;
    for (; it != end; ++it) {
        if (QTextFrame* child = it.currentFrame()) {
            if (auto* t = qobject_cast<QTextTable*>(child)) {
                table(t, availTw);
                lastTable = true;
            } else {
                bool e = false;
                frame(child->begin(), child->end(), availTw, &e);
                lastTable = e;
            }
        } else if (it.currentBlock().isValid()) {
            block(it.currentBlock(), availTw);
            lastTable = false;
        }
    }
    if (endsWithTable) *endsWithTable = lastTable;
}

QString Writer::runProps(const QTextCharFormat& cf) const
{
    QString p;
    if (isMono(cf))
        p += QStringLiteral("<w:rFonts w:ascii=\"%1\" w:hAnsi=\"%1\" w:cs=\"%1\"/>").arg(esc(opt_.monoFont));
    if (cf.fontWeight() >= QFont::DemiBold) p += QLatin1String("<w:b/>");
    if (cf.fontItalic()) p += QLatin1String("<w:i/>");
    if (cf.fontStrikeOut()) p += QLatin1String("<w:strike/>");
    const QBrush fg = cf.foreground();
    if (solid(fg)) p += QStringLiteral("<w:color w:val=\"%1\"/>").arg(hexColor(fg.color()));
    double pt = 0.0;
    if (cf.hasProperty(QTextFormat::FontPointSize) && cf.fontPointSize() > 0) pt = cf.fontPointSize();
    else if (cf.hasProperty(QTextFormat::FontPixelSize) && cf.intProperty(QTextFormat::FontPixelSize) > 0)
        pt = cf.intProperty(QTextFormat::FontPixelSize) * 0.75;
    else if (cf.hasProperty(QTextFormat::FontSizeAdjustment)) {
        // HTML « small » = −1, « large » = +1… : facteurs proches de ceux d'un navigateur
        static const double f[] = { 0.6, 0.75, 0.85, 1.0, 1.2, 1.45, 1.8 };
        const int a = std::clamp(cf.intProperty(QTextFormat::FontSizeAdjustment), -3, 3);
        pt = opt_.bodyPt * f[a + 3];
    }
    if (pt > 0) p += QStringLiteral("<w:sz w:val=\"%1\"/><w:szCs w:val=\"%1\"/>").arg(qRound(pt * 2));
    if (cf.fontUnderline() || cf.underlineStyle() != QTextCharFormat::NoUnderline) p += QLatin1String("<w:u w:val=\"single\"/>");
    const QBrush bg = cf.background();
    if (solid(bg)) p += QStringLiteral("<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%1\"/>").arg(fillOf(bg));
    if (cf.verticalAlignment() == QTextCharFormat::AlignSuperScript) p += QLatin1String("<w:vertAlign w:val=\"superscript\"/>");
    else if (cf.verticalAlignment() == QTextCharFormat::AlignSubScript) p += QLatin1String("<w:vertAlign w:val=\"subscript\"/>");
    return p.isEmpty() ? QString() : QStringLiteral("<w:rPr>%1</w:rPr>").arg(p);
}

// Texte d'un fragment : retours à la ligne (U+2028, \n) et tabulations à part.
QString Writer::textRuns(const QString& text, const QString& rPr) const
{
    QString out, cur;
    auto flush = [&] {
        if (!cur.isEmpty()) out += QStringLiteral("<w:t xml:space=\"preserve\">%1</w:t>").arg(esc(cur));
        cur.clear();
    };
    for (const QChar c : text) {
        if (c == QChar::LineSeparator || c == '\n') { flush(); out += QLatin1String("<w:br/>"); }
        else if (c == '\t') { flush(); out += QLatin1String("<w:tab/>"); }
        else if (c == QChar::ObjectReplacementCharacter || c == QChar::ParagraphSeparator) continue;
        else cur += c;
    }
    flush();
    return out.isEmpty() ? QString() : QStringLiteral("<w:r>%1%2</w:r>").arg(rPr, out);
}

QString Writer::image(const QTextImageFormat& f, int availTw)
{
    const QString name = f.name();
    QImage img;
    const QVariant r = doc_.resource(QTextDocument::ImageResource, QUrl(name));
    if (r.canConvert<QImage>()) img = r.value<QImage>();
    if (img.isNull() && r.canConvert<QPixmap>()) img = r.value<QPixmap>().toImage();
    if (img.isNull() && r.typeId() == QMetaType::QByteArray) img.loadFromData(r.toByteArray());
    if (img.isNull() && name.startsWith(QLatin1String("data:image/"))) {
        const qsizetype comma = name.indexOf(',');
        if (comma > 0) img.loadFromData(QByteArray::fromBase64(name.mid(comma + 1).toLatin1()));
    }
    if (img.isNull() && name.startsWith(QLatin1String(":/"))) img.load(name);
    if (img.isNull()) return QString();
    // Taille affichée (pixels indépendants de l'écran), réduite à la largeur disponible ; l'image garde sa résolution.
    const QSizeF dip = img.deviceIndependentSize();
    double w = f.width() > 0 ? f.width() : dip.width(), h = f.height() > 0 ? f.height() : dip.height();
    if (f.width() > 0 && !(f.height() > 0) && dip.width() > 0) h = w * dip.height() / dip.width();
    if (f.height() > 0 && !(f.width() > 0) && dip.height() > 0) w = h * dip.width() / dip.height();
    const double maxW = double(availTw) / kTwipsPerPx;
    if (w > maxW && w > 0) { h *= maxW / w; w = maxW; }
    QString rid = imageRel_.value(name);
    if (rid.isEmpty()) {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "PNG");
        const QString file = QStringLiteral("image%1.png").arg(media_.size() + 1);
        media_.append({ file, png });
        rid = newRel(QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/image"),
                     QStringLiteral("media/") + file, false);
        imageRel_.insert(name, rid);
    }
    const qint64 cx = qint64(std::llround(w * kEmuPerPx)), cy = qint64(std::llround(h * kEmuPerPx));
    const int id = nextDrawing_++;
    return QStringLiteral(
               "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"%1\" cy=\"%2\"/>"
               "<wp:docPr id=\"%3\" name=\"Image %3\"/><wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
               "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic>"
               "<pic:nvPicPr><pic:cNvPr id=\"%3\" name=\"image%3.png\"/><pic:cNvPicPr/></pic:nvPicPr>"
               "<pic:blipFill><a:blip r:embed=\"%4\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
               "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%1\" cy=\"%2\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>"
               "</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>")
        .arg(cx).arg(cy).arg(id).arg(rid);
}

void Writer::block(const QTextBlock& b, int availTw)
{
    const QTextBlockFormat bf = b.blockFormat();
    QString pPr;
    const int heading = bf.headingLevel();
    if (heading >= 1 && heading <= 6) pPr += QStringLiteral("<w:pStyle w:val=\"Heading%1\"/>").arg(std::min(heading, 3));
    const QTextList* list = b.textList();
    if (list) {
        int numId = numIds_.value(list, 0);
        if (numId == 0) {
            const QTextListFormat::Style st = list->format().style();
            const bool bullet = st == QTextListFormat::ListDisc || st == QTextListFormat::ListCircle || st == QTextListFormat::ListSquare
                                || st == QTextListFormat::ListStyleUndefined;
            numAbstract_.append(bullet ? 0 : 1);
            numId = int(numAbstract_.size());
            numIds_.insert(list, numId);
        }
        const int lvl = std::clamp(list->format().indent() - 1, 0, 8);
        pPr += QStringLiteral("<w:numPr><w:ilvl w:val=\"%1\"/><w:numId w:val=\"%2\"/></w:numPr>").arg(lvl).arg(numId);
    }
    if (solid(bf.background())) pPr += QStringLiteral("<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%1\"/>").arg(fillOf(bf.background()));
    const int before = qRound(std::max(0.0, bf.topMargin()) * kTwipsPerPx), after = qRound(std::max(0.0, bf.bottomMargin()) * kTwipsPerPx);
    pPr += QStringLiteral("<w:spacing w:before=\"%1\" w:after=\"%2\"/>").arg(before).arg(after);
    const int left = qRound((bf.leftMargin() + (list ? 0 : bf.indent() * doc_.indentWidth())) * kTwipsPerPx);
    const int right = qRound(bf.rightMargin() * kTwipsPerPx);
    if (!list && (left > 0 || right > 0 || bf.textIndent() != 0))
        pPr += QStringLiteral("<w:ind w:left=\"%1\" w:right=\"%2\" w:firstLine=\"%3\"/>")
                   .arg(left).arg(right).arg(qRound(std::max(0.0, bf.textIndent()) * kTwipsPerPx));
    const Qt::Alignment al = bf.alignment();
    if (al & Qt::AlignHCenter) pPr += QLatin1String("<w:jc w:val=\"center\"/>");
    else if (al & Qt::AlignRight) pPr += QLatin1String("<w:jc w:val=\"right\"/>");
    else if (al & Qt::AlignJustify) pPr += QLatin1String("<w:jc w:val=\"both\"/>");

    QString runs;
    for (QTextBlock::iterator fi = b.begin(); !fi.atEnd(); ++fi) {
        const QTextFragment f = fi.fragment();
        if (!f.isValid()) continue;
        const QTextCharFormat cf = f.charFormat();
        if (cf.isImageFormat()) { runs += image(cf.toImageFormat(), availTw); continue; }
        QString r = textRuns(f.text(), runProps(cf));
        const QString href = cf.isAnchor() ? cf.anchorHref() : QString();
        if (!r.isEmpty() && (href.startsWith(QLatin1String("http://")) || href.startsWith(QLatin1String("https://"))
                             || href.startsWith(QLatin1String("mailto:")))) {
            QString rid = linkRel_.value(href);
            if (rid.isEmpty()) {
                rid = newRel(QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink"), href, true);
                linkRel_.insert(href, rid);
            }
            r = QStringLiteral("<w:hyperlink r:id=\"%1\">%2</w:hyperlink>").arg(rid, r);
        }
        runs += r;
    }
    // Paragraphe vide (souvent laissé par Qt après un tableau) : tout petit, pour ne pas ajouter de grands blancs.
    if (runs.isEmpty() && !list && heading == 0)
        pPr += QLatin1String("<w:rPr><w:sz w:val=\"4\"/><w:szCs w:val=\"4\"/></w:rPr>");
    body_ += QStringLiteral("<w:p><w:pPr>%1</w:pPr>%2</w:p>").arg(pPr, runs);
}

void Writer::table(QTextTable* t, int availTw)
{
    const QTextTableFormat tf = t->format();
    const int rows = t->rows(), cols = t->columns();
    if (rows <= 0 || cols <= 0) return;
    // Largeur du tableau : % de la largeur disponible, fixe (pixels), ou toute la largeur.
    int tw = availTw;
    const QTextLength wl = tf.width();
    if (wl.type() == QTextLength::PercentageLength) tw = int(availTw * wl.rawValue() / 100.0);
    else if (wl.type() == QTextLength::FixedLength) tw = std::min(availTw, int(wl.rawValue() * kTwipsPerPx));
    tw = std::max(tw, 200);
    // Colonnes : contraintes de largeur (pixels, %), le reste partagé entre les colonnes libres.
    const QVector<QTextLength> cons = tf.columnWidthConstraints();
    std::vector<int> cw(size_t(cols), 0);
    int used = 0, freeCols = 0;
    for (int c = 0; c < cols; ++c) {
        const QTextLength l = c < cons.size() ? cons[c] : QTextLength();
        if (l.type() == QTextLength::FixedLength) cw[size_t(c)] = std::max(20, int(l.rawValue() * kTwipsPerPx));
        else if (l.type() == QTextLength::PercentageLength) cw[size_t(c)] = int(tw * l.rawValue() / 100.0);
        else { ++freeCols; continue; }
        used += cw[size_t(c)];
    }
    for (int c = 0; c < cols; ++c)
        if (cw[size_t(c)] == 0) cw[size_t(c)] = freeCols > 0 ? std::max(200, (tw - used) / freeCols) : tw / cols;
    int total = 0;
    for (int w : cw) total += w;
    tw = total;

    const double pad = tf.cellPadding();
    const bool border = tf.border() > 0 && tf.borderStyle() != QTextFrameFormat::BorderStyle_None;
    const QString bColor = solid(tf.borderBrush()) ? hexColor(tf.borderBrush().color()) : QStringLiteral("808080");
    const int bSz = std::max(4, qRound(tf.border() * 6));         // huitièmes de point (1 px = 0,75 pt)
    auto borders = [&](const QString& tag) {
        QString s;
        for (const char* side : { "top", "left", "bottom", "right", "insideH", "insideV" })
            s += border ? QStringLiteral("<w:%1 w:val=\"single\" w:sz=\"%2\" w:space=\"0\" w:color=\"%3\"/>").arg(QLatin1String(side)).arg(bSz).arg(bColor)
                        : QStringLiteral("<w:%1 w:val=\"nil\"/>").arg(QLatin1String(side));
        return QStringLiteral("<w:%1>%2</w:%1>").arg(tag, s);
    };
    QString x = QStringLiteral("<w:tbl><w:tblPr><w:tblW w:w=\"%1\" w:type=\"dxa\"/>").arg(tw);
    if (tf.alignment() & Qt::AlignRight) x += QLatin1String("<w:jc w:val=\"right\"/>");
    else if (tf.alignment() & Qt::AlignHCenter) x += QLatin1String("<w:jc w:val=\"center\"/>");
    x += borders(QStringLiteral("tblBorders"));
    x += QLatin1String("<w:tblLayout w:type=\"fixed\"/>");
    const int padTw = qRound(pad * kTwipsPerPx);
    x += QStringLiteral("<w:tblCellMar><w:top w:w=\"%1\" w:type=\"dxa\"/><w:left w:w=\"%1\" w:type=\"dxa\"/>"
                        "<w:bottom w:w=\"%1\" w:type=\"dxa\"/><w:right w:w=\"%1\" w:type=\"dxa\"/></w:tblCellMar>").arg(padTw);
    x += QLatin1String("<w:tblLook w:val=\"0000\"/></w:tblPr><w:tblGrid>");
    for (int w : cw) x += QStringLiteral("<w:gridCol w:w=\"%1\"/>").arg(w);
    x += QLatin1String("</w:tblGrid>");
    body_ += x;

    for (int r = 0; r < rows; ++r) {
        body_ += QLatin1String("<w:tr>");          // ligne coupable entre deux pages : une longue réponse ne laisse pas de page blanche
        for (int c = 0; c < cols;) {
            const QTextTableCell cell = t->cellAt(r, c);
            const int span = std::max(1, cell.columnSpan());
            int width = 0;
            for (int k = c; k < std::min(cols, c + span); ++k) width += cw[size_t(k)];
            QString tcPr = QStringLiteral("<w:tcW w:w=\"%1\" w:type=\"dxa\"/>").arg(width);
            if (span > 1) tcPr += QStringLiteral("<w:gridSpan w:val=\"%1\"/>").arg(span);
            const bool continued = cell.row() != r;                   // suite d'une fusion verticale
            if (cell.rowSpan() > 1) tcPr += continued ? QLatin1String("<w:vMerge/>") : QLatin1String("<w:vMerge w:val=\"restart\"/>");
            const QTextTableCellFormat cf = cell.format().toTableCellFormat();
            if (solid(cf.background())) tcPr += QStringLiteral("<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%1\"/>").arg(fillOf(cf.background()));
            // Marges intérieures propres à la cellule (style « padding »)
            auto padOf = [&](QTextFormat::Property p) { return cf.hasProperty(p) ? qRound(cf.doubleProperty(p) * kTwipsPerPx) : -1; };
            const int pt = padOf(QTextFormat::TableCellTopPadding), pl = padOf(QTextFormat::TableCellLeftPadding),
                      pb = padOf(QTextFormat::TableCellBottomPadding), pr = padOf(QTextFormat::TableCellRightPadding);
            if (pt >= 0 || pl >= 0 || pb >= 0 || pr >= 0)
                tcPr += QStringLiteral("<w:tcMar><w:top w:w=\"%1\" w:type=\"dxa\"/><w:left w:w=\"%2\" w:type=\"dxa\"/>"
                                       "<w:bottom w:w=\"%3\" w:type=\"dxa\"/><w:right w:w=\"%4\" w:type=\"dxa\"/></w:tcMar>")
                            .arg(std::max(pt, padTw)).arg(std::max(pl, padTw)).arg(std::max(pb, padTw)).arg(std::max(pr, padTw));
            const QTextCharFormat::VerticalAlignment va = cell.format().verticalAlignment();
            if (va == QTextCharFormat::AlignMiddle) tcPr += QLatin1String("<w:vAlign w:val=\"center\"/>");
            else if (va == QTextCharFormat::AlignBottom) tcPr += QLatin1String("<w:vAlign w:val=\"bottom\"/>");
            body_ += QStringLiteral("<w:tc><w:tcPr>%1</w:tcPr>").arg(tcPr);
            const qsizetype before = body_.size();
            bool endsWithTable = false;
            if (!continued) {
                const int inner = std::max(200, width - std::max(pl, padTw) - std::max(pr, padTw));
                frame(cell.begin(), cell.end(), inner, &endsWithTable);
            }
            if (body_.size() == before || endsWithTable)                 // une cellule Word finit toujours par un paragraphe
                body_ += QLatin1String("<w:p><w:pPr><w:spacing w:before=\"0\" w:after=\"0\"/><w:rPr><w:sz w:val=\"4\"/></w:rPr></w:pPr></w:p>");
            body_ += QLatin1String("</w:tc>");
            c += span;
        }
        body_ += QLatin1String("</w:tr>");
    }
    body_ += QLatin1String("</w:tbl>");
}

QByteArray Writer::run(QString* error)
{
    QTextFrame* root = doc_.rootFrame();
    if (!root) { if (error) *error = QStringLiteral("document vide"); return {}; }
    bool endsWithTable = false;
    frame(root->begin(), root->end(), kTextW, &endsWithTable);
    if (endsWithTable) body_ += QLatin1String("<w:p/>");

    const QString ns = QStringLiteral(
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"");
    // Concaténation (pas de .arg en chaîne) : le texte de la conversation peut contenir « %1 », « %2 »…
    const QString document = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document ") + ns
                             + QStringLiteral("><w:body>") + body_
                             + QStringLiteral("<w:sectPr><w:pgSz w:w=\"%1\" w:h=\"%2\"/><w:pgMar w:top=\"%3\" w:right=\"%3\" w:bottom=\"%3\" "
                                              "w:left=\"%3\" w:header=\"567\" w:footer=\"567\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>")
                                   .arg(kPageW).arg(kPageH).arg(kMargin);

    const int sz = qRound(opt_.bodyPt * 2);
    const QString font = esc(opt_.bodyFont);
    const QString styles = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"%1\" w:hAnsi=\"%1\" w:eastAsia=\"%1\" w:cs=\"%1\"/>"
        "<w:sz w:val=\"%2\"/><w:szCs w:val=\"%2\"/><w:lang w:val=\"fr-FR\"/></w:rPr></w:rPrDefault>"
        "<w:pPrDefault><w:pPr><w:spacing w:after=\"60\" w:line=\"264\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>"
        "<w:qFormat/><w:pPr><w:keepNext/><w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr><w:b/><w:color w:val=\"1F5080\"/><w:sz w:val=\"32\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>"
        "<w:qFormat/><w:pPr><w:keepNext/><w:outlineLvl w:val=\"1\"/></w:pPr><w:rPr><w:b/><w:color w:val=\"1F5080\"/><w:sz w:val=\"28\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\"><w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/>"
        "<w:qFormat/><w:pPr><w:keepNext/><w:outlineLvl w:val=\"2\"/></w:pPr><w:rPr><w:b/><w:color w:val=\"1F5080\"/><w:sz w:val=\"24\"/></w:rPr></w:style>"
        "</w:styles>").arg(font).arg(sz);

    QString numbering = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                                       "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">");
    for (int a = 0; a < 2; ++a) {
        numbering += QStringLiteral("<w:abstractNum w:abstractNumId=\"%1\"><w:multiLevelType w:val=\"hybridMultilevel\"/>").arg(a);
        for (int l = 0; l < 9; ++l) {
            const QString fmt = a == 0 ? QStringLiteral("bullet") : QStringLiteral("decimal");
            const QString txt = a == 0 ? (l % 2 == 0 ? QStringLiteral("•") : QStringLiteral("◦")) : QStringLiteral("%%1.").arg(l + 1);
            numbering += QStringLiteral("<w:lvl w:ilvl=\"%1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"%2\"/><w:lvlText w:val=\"%3\"/>"
                                        "<w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%4\" w:hanging=\"357\"/></w:pPr></w:lvl>")
                             .arg(QString::number(l), fmt, txt, QString::number(357 * (l + 1) + 140));
        }
        numbering += QLatin1String("</w:abstractNum>");
    }
    for (int i = 0; i < numAbstract_.size(); ++i)
        numbering += QStringLiteral("<w:num w:numId=\"%1\"><w:abstractNumId w:val=\"%2\"/>%3</w:num>")
                         .arg(i + 1).arg(numAbstract_[i])
                         .arg(numAbstract_[i] == 1 ? QStringLiteral("<w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"1\"/></w:lvlOverride>") : QString());
    numbering += QLatin1String("</w:numbering>");

    const QString docRels = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>"
        "%1</Relationships>").arg(rels_.join(QString()));
    const QString rootRels = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
        "</Relationships>");
    const QString types = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"png\" ContentType=\"image/png\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
        "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
        "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
        "</Types>");
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QString core = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
        "<dc:title>%1</dc:title><dc:creator>%2</dc:creator>"
        "<dcterms:created xsi:type=\"dcterms:W3CDTF\">%3</dcterms:created><dcterms:modified xsi:type=\"dcterms:W3CDTF\">%3</dcterms:modified>"
        "</cp:coreProperties>").arg(esc(opt_.title), esc(opt_.author), now);

    QByteArray zip;
    {
        QBuffer buf(&zip);
        buf.open(QIODevice::WriteOnly);
        QZipWriter w(&buf);
        w.setCompressionPolicy(QZipWriter::AutoCompress);
        w.addFile(QStringLiteral("[Content_Types].xml"), types.toUtf8());
        w.addFile(QStringLiteral("_rels/.rels"), rootRels.toUtf8());
        w.addFile(QStringLiteral("docProps/core.xml"), core.toUtf8());
        w.addFile(QStringLiteral("word/document.xml"), document.toUtf8());
        w.addFile(QStringLiteral("word/styles.xml"), styles.toUtf8());
        w.addFile(QStringLiteral("word/numbering.xml"), numbering.toUtf8());
        w.addFile(QStringLiteral("word/_rels/document.xml.rels"), docRels.toUtf8());
        for (const Media& m : media_) w.addFile(QStringLiteral("word/media/") + m.file, m.png);
        w.close();
        if (w.status() != QZipWriter::NoError) {
            if (error) *error = QStringLiteral("écriture de l'archive impossible");
            return {};
        }
    }
    return zip;
}

} // namespace

QByteArray build(const QTextDocument& doc, const Options& options, QString* error)
{
    Writer w(doc, options);
    return w.run(error);
}

bool write(const QTextDocument& doc, const QString& path, const Options& options, QString* error)
{
    const QByteArray data = build(doc, options, error);
    if (data.isEmpty()) return false;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    if (f.write(data) != data.size()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

} // namespace seldon::docx
