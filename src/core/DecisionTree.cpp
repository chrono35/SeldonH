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

#include "DecisionTree.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <functional>

namespace seldon {
namespace decision {

const Choice* Node::choice(const QString& cid) const
{
    for (const Choice& c : choices) if (c.id == cid) return &c;
    return nullptr;
}

bool Tree::load(const QByteArray& json, QString* error)
{
    root_.clear();
    nodes_.clear();
    order_.clear();
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject()) {
        if (error) *error = QStringLiteral("JSON invalide : %1 (position %2)").arg(pe.errorString()).arg(pe.offset);
        return false;
    }
    const QJsonObject o = d.object();
    root_ = o.value("racine").toString();
    const QJsonObject ns = o.value("noeuds").toObject();
    if (ns.isEmpty()) {
        if (error) *error = QStringLiteral("aucun nœud (« noeuds »)");
        return false;
    }
    for (auto it = ns.begin(); it != ns.end(); ++it) {
        const QJsonObject jn = it.value().toObject();
        Node n;
        n.id = it.key();
        n.question = jn.value("question").toString();
        n.title = jn.value("titre").toString(n.id);
        for (const QJsonValue& v : jn.value("choix").toArray()) {
            const QJsonObject jc = v.toObject();
            Choice c;
            c.id = jc.value("id").toString();
            c.label = jc.value("libelle").toString();
            c.next = jc.value("suite").toString();
            auto readLeaf = [](const QJsonObject& jl, Leaf& l) {
                for (const QJsonValue& t : jl.value("outils").toArray()) l.tools << t.toString();
                for (const QJsonValue& f : jl.value("fiches").toArray()) l.skills << f.toString();
                l.action = jl.value("action").toString();
                for (auto k = jl.begin(); k != jl.end(); ++k)
                    if (k.key() != QLatin1String("outils") && k.key() != QLatin1String("fiches") && k.key() != QLatin1String("action"))
                        l.params.insert(k.key(), k.value());
            };
            readLeaf(jc.value("feuille").toObject(), c.leaf);
            readLeaf(jc.value("ajoute").toObject(), c.adds);
            // Choix sans « suite » ni « feuille » : signalé par validate().
            if (c.next.isEmpty() && !jc.contains("feuille")) c.leaf.params.insert(QStringLiteral("_sans_feuille"), true);
            n.choices.append(c);
        }
        order_ << n.id;
        nodes_.insert(n.id, n);
    }
    return true;
}

const Tree& Tree::builtin()
{
    static const Tree t = [] {
        Tree x;
        QFile f(QStringLiteral(":/decision/arbre.json"));
        if (f.open(QIODevice::ReadOnly)) x.load(f.readAll());
        return x;
    }();
    return t;
}

const Node* Tree::node(const QString& id) const
{
    const auto it = nodes_.constFind(id);
    return it == nodes_.cend() ? nullptr : &it.value();
}

QStringList Tree::validate(const QStringList& knownTools, const QStringList& knownSkills) const
{
    QStringList out;
    if (nodes_.isEmpty()) return { QStringLiteral("arbre vide") };
    if (!nodes_.contains(root_)) out << QStringLiteral("racine « %1 » inexistante").arg(root_);
    static const QRegularExpression idRe(QStringLiteral("^[a-z][a-z0-9_]*$"));
    for (const QString& nid : order_) {
        const Node& n = nodes_[nid];
        if (n.question.trimmed().isEmpty()) out << QStringLiteral("nœud « %1 » : question vide").arg(nid);
        if (n.title.trimmed().isEmpty()) out << QStringLiteral("nœud « %1 » : titre vide").arg(nid);
        if (n.choices.size() < 2) out << QStringLiteral("nœud « %1 » : moins de deux choix").arg(nid);
        QSet<QString> seen;
        for (const Choice& c : n.choices) {
            const QString where = QStringLiteral("nœud « %1 », choix « %2 »").arg(nid, c.id);
            if (!idRe.match(c.id).hasMatch()) out << where + QStringLiteral(" : identifiant invalide (minuscules ASCII)");
            if (seen.contains(c.id)) out << where + QStringLiteral(" : identifiant en double");
            seen.insert(c.id);
            if (c.label.trimmed().isEmpty()) out << where + QStringLiteral(" : libellé vide");
            if (c.leaf.params.contains(QStringLiteral("_sans_feuille"))) out << where + QStringLiteral(" : ni suite ni feuille");
            if (!c.next.isEmpty() && !nodes_.contains(c.next)) out << where + QStringLiteral(" : suite « %1 » inexistante").arg(c.next);
            if (c.isLeaf() && c.leaf.tools.isEmpty() && c.leaf.action.isEmpty() && !c.leaf.params.contains(QStringLiteral("_sans_feuille")))
                out << where + QStringLiteral(" : feuille sans outil ni action");
            if (!knownTools.isEmpty())
                for (const QString& t : c.leaf.tools + c.adds.tools)
                    if (!knownTools.contains(t)) out << where + QStringLiteral(" : outil « %1 » inconnu").arg(t);
            if (!knownSkills.isEmpty())
                for (const QString& s : c.leaf.skills + c.adds.skills)
                    if (!knownSkills.contains(s)) out << where + QStringLiteral(" : fiche « %1 » inconnue").arg(s);
            if (!c.adds.action.isEmpty()) out << where + QStringLiteral(" : « ajoute » ne peut pas porter d'action");
        }
    }
    // Accessibilité depuis la racine et cycles.
    QHash<QString, int> state;                       // 0 non vu, 1 en cours, 2 fini
    std::function<void(const QString&)> visit = [&](const QString& id) {
        state[id] = 1;
        for (const Choice& c : nodes_[id].choices) {
            if (c.next.isEmpty() || !nodes_.contains(c.next)) continue;
            if (state.value(c.next) == 1) out << QStringLiteral("cycle : « %1 » → « %2 »").arg(id, c.next);
            else if (state.value(c.next) == 0) visit(c.next);
        }
        state[id] = 2;
    };
    if (nodes_.contains(root_)) visit(root_);
    for (const QString& nid : order_)
        if (state.value(nid) == 0) out << QStringLiteral("nœud « %1 » inaccessible depuis la racine").arg(nid);
    return out;
}

QStringList Tree::pathsTo(const QString& tool) const
{
    QStringList out;
    std::function<void(const QString&, QStringList, QSet<QString>, bool)> walk = [&](const QString& id, QStringList path,
                                                                                     QSet<QString> seen, bool carried) {
        if (seen.contains(id) || !nodes_.contains(id)) return;
        seen.insert(id);
        for (const Choice& c : nodes_[id].choices) {
            QStringList p = path;
            p << QStringLiteral("%1=%2").arg(id, c.id);
            const bool has = carried || c.adds.tools.contains(tool);
            if (c.isLeaf()) { if (has || c.leaf.tools.contains(tool)) out << p.join(QStringLiteral(" › ")); }
            else walk(c.next, p, seen, has);
        }
    };
    walk(root_, {}, {}, false);
    return out;
}

QStringList Tree::uncoveredTools(const QStringList& tools) const
{
    QStringList out;
    for (const QString& t : tools)
        if (pathsTo(t).isEmpty()) out << t;
    return out;
}

Leaf accumulate(const Tree& t, const QVector<QPair<QString, QString>>& path)
{
    Leaf out;
    for (const auto& step : path) {
        const Node* n = t.node(step.first);
        const Choice* c = n ? n->choice(step.second) : nullptr;
        if (!c) continue;
        const Leaf& part = c->isLeaf() ? c->leaf : c->adds;
        out.tools << part.tools;
        out.skills << part.skills;
        for (auto k = part.params.begin(); k != part.params.end(); ++k) out.params.insert(k.key(), k.value());
        if (c->isLeaf()) out.action = c->leaf.action;
    }
    out.tools.removeDuplicates();
    out.skills.removeDuplicates();
    out.params.remove(QStringLiteral("_sans_feuille"));
    return out;
}

bool mentionsCurve(const QString& text)
{
    QString f = text.toLower().normalized(QString::NormalizationForm_D);
    f.remove(QRegularExpression(QStringLiteral("[\\x{0300}-\\x{036f}]")));
    static const QRegularExpression re(QStringLiteral(
        R"(\b(?:courbes?|curves?|inclinaisons?|inclination|declinaisons?|declination|intensites?|intensity|profondeurs?|depths?|)"
        R"(carottes?|carottages?|core|splines?|variation seculaire|secular variation|univariee?s?|bivariee?s?|trivariee?s?|)"
        R"(univariate|bivariate|trivariate|age-profondeur|age-depth|archeomagneti\w*|archaeomagneti\w*|paleomagneti\w*|)"
        R"(en fonction du temps|au cours du temps|over time|through time)\b)"));
    return re.match(f).hasMatch();
}

namespace {
QString folded(const QString& text)
{
    QString f = text.toLower().normalized(QString::NormalizationForm_D);
    f.remove(QRegularExpression(QStringLiteral("[\\x{0300}-\\x{036f}]")));
    return f;
}
} // namespace

QString curveTypeFromText(const QString& text)
{
    const QString f = folded(text);
    // Lettres I, D, F : dans le texte tel qu'écrit (majuscules), « I = 39 », « (I, D, F) ».
    static const QRegularExpression triple(QStringLiteral(R"(\(\s*I\s*,\s*D\s*,\s*F\s*\))"));
    static const QRegularExpression pairID(QStringLiteral(R"(\(\s*I\s*,\s*D\s*\))"));
    static const QRegularExpression pairIF(QStringLiteral(R"(\(\s*I\s*,\s*F\s*\))"));
    static const QRegularExpression iVal(QStringLiteral(R"(\bI\s*=\s*-?\d)")), dVal(QStringLiteral(R"(\bD\s*=\s*-?\d)")),
        fVal(QStringLiteral(R"(\bF\s*=\s*\d)"));
    static const QRegularExpression iWord(QStringLiteral(R"(\binclinaisons?\b|\binclinations?\b)"));
    static const QRegularExpression dWord(QStringLiteral(R"(\bdeclinaisons?\b|\bdeclinations?\b)"));
    static const QRegularExpression fWord(QStringLiteral(R"(\bintensites?\b|\bintensity\b|\bpaleointensit\w*|\barcheointensit\w*|\bµt\b)"));
    static const QRegularExpression archeo(QStringLiteral(R"(\barch[a]?eomagneti\w*|\bpaleomagneti\w*|\bvariation seculaire|\bsecular variation|\balpha\s*95|α95)"));
    bool I = iWord.match(f).hasMatch() || iVal.match(text).hasMatch();
    bool D = dWord.match(f).hasMatch() || dVal.match(text).hasMatch();
    bool F = fWord.match(f).hasMatch() || fVal.match(text).hasMatch();
    if (triple.match(text).hasMatch() || f.contains(QLatin1String("courbe vectorielle")) || f.contains(QLatin1String("vector curve"))) I = D = F = true;
    else if (pairID.match(text).hasMatch()) I = D = true;
    else if (pairIF.match(text).hasMatch()) I = F = true;
    const int n = int(I) + int(D) + int(F);
    // Archéomagnétisme sûr : un mot du domaine, ou au moins deux grandeurs (une « intensité » seule peut être autre chose).
    if (n > 0 && (archeo.match(f).hasMatch() || n >= 2)) {
        if (I && D && F) return QStringLiteral("inc_dec_int");
        if (I && D) return QStringLiteral("inc_dec");
        if (I && F) return QStringLiteral("inc_int");
        if (n == 1) return I ? QStringLiteral("inclinaison") : D ? QStringLiteral("declinaison") : QStringLiteral("intensite");
        return QString();                                       // D et F sans I : pas de choix, le modèle de langage tranche
    }
    if (n == 0) {
        static const QRegularExpression uni(QStringLiteral(R"(\bunivari\w*)")), bi(QStringLiteral(R"(\bbivari\w*)")),
            tri(QStringLiteral(R"(\btrivari\w*)"));
        if (tri.match(f).hasMatch()) return QStringLiteral("trivariee");
        if (bi.match(f).hasMatch()) return QStringLiteral("bivariee");
        if (uni.match(f).hasMatch()) return QStringLiteral("univariee");
    }
    return QString();
}

bool deniesStratigraphy(const QString& text)
{
    // « Il n'y a ni phase ni ordre stratigraphique entre les coulées » (prompt réel « Açores », 13/11/2026), « sans
    // stratigraphie », « aucune relation stratigraphique », « no stratigraphic order ».
    static const QRegularExpression re(QStringLiteral(
        R"(\b(?:ni|sans|pas d'|pas de|aucune?|no|without)\s+(?:phases?\s+ni\s+(?:d')?)?(?:ordres?|relations?|contraintes?|liens?)\s+)"
        R"((?:stratigraphiques?|stratigraphic)|\bsans stratigraphie|\bpas de stratigraphie|\baucune stratigraphie|\bno stratigraphy\b|)"
        R"(\bni phase ni ordre)"));
    return re.match(folded(text)).hasMatch();
}

QString grammarFor(const Node& n)
{
    QStringList alts;
    for (const Choice& c : n.choices) alts << QStringLiteral("\"%1\"").arg(c.id);
    return QStringLiteral("root ::= %1\n").arg(alts.join(QStringLiteral(" | ")));
}

QString promptFor(const Node& n, const QString& userQuestion)
{
    QString s = QStringLiteral("[Aiguillage de SeldonH : réponds par UN SEUL identifiant de la liste, sans rien d'autre.]\n\n"
                               "Message de l'utilisateur :\n<<<\n%1\n>>>\n\n%2\n").arg(userQuestion.trimmed(), n.question);
    for (const Choice& c : n.choices) s += QStringLiteral("- %1 : %2\n").arg(c.id, c.label);
    s += QStringLiteral("\nIdentifiant :");
    return s;
}

bool requestsCurveModel(const QString& text)
{
    QString f = text.toLower().normalized(QString::NormalizationForm_D);
    f.remove(QRegularExpression(QStringLiteral("[\\x{0300}-\\x{036f}]")));
    // Demande explicite d'un modèle en mode courbe (prompt réel « Açores », 13/11/2026 : « modèle ChronoModel en mode
    // courbe vectorielle (I, D, F) » lu « non » parce que la réponse du modèle de langage était son raisonnement).
    static const QRegularExpression re(QStringLiteral(
        R"(\b(?:mode courbe|curve mode|courbes? (?:vectorielles?|univariees?|bivariees?|trivariees?|de variation seculaire|)"
        R"(age-profondeur|archeomagneti\w*)|(?:vector|univariate|bivariate|trivariate|secular variation|age-depth|)"
        R"(archaeomagnetic) curves?)\b)"));
    return re.match(f).hasMatch();
}

QString parseAnswer(const Node& n, const QString& answer)
{
    // Raisonnement d'un modèle « à réflexion » (gpt-oss, Qwen… : reasoning_content remis entre <think> et </think>) : il
    // recopie souvent la liste des choix (« - non : … »), il ne compte pas. Raisonnement non refermé (réponse coupée par
    // max_tokens) : aucune réponse.
    QString a = answer;
    a.remove(QRegularExpression(QStringLiteral("<think>.*?</think>"), QRegularExpression::DotMatchesEverythingOption));
    if (a.contains(QLatin1String("<think>"))) return QString();
    a = a.trimmed().toLower();
    a.remove(QRegularExpression(QStringLiteral(R"([`*"'«»“”.\s]+$)")));
    a.remove(QRegularExpression(QStringLiteral(R"(^[`*"'«»“”\s-]+)")));
    for (const Choice& c : n.choices) if (a == c.id) return c.id;
    QString found;
    for (const Choice& c : n.choices) {
        const QRegularExpression w(QStringLiteral("(?<![a-z0-9_])%1(?![a-z0-9_])").arg(QRegularExpression::escape(c.id)));
        if (w.match(a).hasMatch()) {
            if (!found.isEmpty()) return QString();          // plusieurs identifiants : réponse ambiguë
            found = c.id;
        }
    }
    return found;
}

} // namespace decision
} // namespace seldon
