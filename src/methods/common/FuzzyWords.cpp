#include "FuzzyWords.h"
#include "TextUtil.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <vector>

namespace seldon::fuzzy {

namespace {

// Mots déclencheurs des détecteurs (forme repliée sans accents → forme affichée). Liste à compléter quand un détecteur
// apprend un nouveau mot ; régénérer alors la liste des mots protégés (tests/data/fuzzy/gen_protected.py).
const std::vector<std::pair<const char*, const char*>>& triggers()
{
    static const std::vector<std::pair<const char*, const char*>> t = {
        { "acceleration", "accélération" },
        { "accelerations", "accélérations" },
        { "accumulation", "accumulation" },
        { "altitude", "altitude" },
        { "ancien", "ancien" },
        { "ancienne", "ancienne" },
        { "anterieur", "anterieur" },
        { "anterieure", "antérieure" },
        { "archeomagnetique", "archéomagnétique" },
        { "archeomagnetisme", "archéomagnétisme" },
        { "atteindre", "atteindre" },
        { "atteint", "atteint" },
        { "between", "between" },
        { "calibrate", "calibrate" },
        { "calibration", "calibration" },
        { "calibre", "calibre" },
        { "calibree", "calibrée" },
        { "calibrer", "calibrer" },
        { "combinaison", "combinaison" },
        { "combine", "combine" },
        { "combiner", "combiner" },
        { "composante", "composante" },
        { "correlation", "corrélation" },
        { "cote", "cote" },
        { "courbe", "courbe" },
        { "courbes", "courbes" },
        { "croissance", "croissance" },
        { "curve", "curve" },
        { "debut", "début" },
        { "declinaison", "déclinaison" },
        { "decroissance", "décroissance" },
        { "densite", "densité" },
        { "depth", "depth" },
        { "derivee", "dérivée" },
        { "duration", "duration" },
        { "duree", "durée" },
        { "evenement", "événement" },
        { "evenements", "événements" },
        { "extraire", "extraire" },
        { "extrais", "extrais" },
        { "extreme", "extrême" },
        { "extremum", "extremum" },
        { "fastest", "fastest" },
        { "gaussienne", "gaussienne" },
        { "hauteur", "hauteur" },
        { "height", "height" },
        { "inclinaison", "inclinaison" },
        { "intensite", "intensité" },
        { "intervalle", "intervalle" },
        { "inverse", "inverse" },
        { "largest", "largest" },
        { "level", "level" },
        { "luminescence", "luminescence" },
        { "maximal", "maximal" },
        { "maximale", "maximale" },
        { "maximum", "maximum" },
        { "mesure", "mesure" },
        { "mesuree", "mesurée" },
        { "minimal", "minimal" },
        { "minimale", "minimale" },
        { "minimum", "minimum" },
        { "niveau", "niveau" },
        { "niveaux", "niveaux" },
        { "pente", "pente" },
        { "pentes", "pentes" },
        { "phase", "phase" },
        { "phases", "phases" },
        { "posterieure", "postérieure" },
        { "probabilite", "probabilité" },
        { "profondeur", "profondeur" },
        { "profondeurs", "profondeurs" },
        { "radiocarbon", "radiocarbon" },
        { "radiocarbone", "radiocarbone" },
        { "recent", "récent" },
        { "recente", "récente" },
        { "sedimentation", "sédimentation" },
        { "separe", "sépare" },
        { "separent", "séparent" },
        { "slope", "slope" },
        { "speed", "speed" },
        { "spline", "spline" },
        { "splines", "splines" },
        { "steepest", "steepest" },
        { "strongest", "strongest" },
        { "tableau", "tableau" },
        { "trace", "trace" },
        { "tracer", "tracer" },
        { "uniforme", "uniforme" },
        { "valeur", "valeur" },
        { "variation", "variation" },
        { "variations", "variations" },
        { "velocity", "velocity" },
        { "vitesse", "vitesse" },
        { "vitesses", "vitesses" } };
    return t;
}

// Mots qui existent (dictionnaires wfrench et wamerican, repliés) à une ou deux lettres d'un déclencheur : jamais corrigés
// (« courte » n'est pas « courbe », « nivaux » n'est pas « niveau »). Généré par tests/data/fuzzy/gen_protected.py.
const QSet<QString>& protectedWords()
{
    static const QSet<QString> p = [] {
        static const char* const w[] = {
        "accelerassions", "accelerating", "accelerator", "accelerators", "accelererions", "accelerions", "accumulating",
        "accumulations", "accumulative", "accumulator", "altitudes", "anciennes", "anciens", "ancient", "anterieures",
        "anterieurs", "antienne", "aptitude", "astreindre", "atteindra", "atteindrai", "atteindras", "atteindrez",
        "atteindriez", "atteins", "atteinte", "atteints", "attendre", "attendrez", "attendrie", "attitude", "auteur",
        "avaleur", "avenement", "aviations", "biveau", "biveaux", "bourbe", "bourbes", "cableau", "caliber", "calibra",
        "calibrage", "calibrages", "calibrais", "calibrait", "calibrames", "calibrant", "calibrasse", "calibrat",
        "calibrated", "calibrates", "calibrating", "calibrations", "calibrator", "calibrees", "calibrera", "calibres",
        "calibreur", "calibrez", "caliphate", "carnation", "carnations", "celebrate", "celebration", "cesure", "chases",
        "citerieur", "citerieure", "combien", "combina", "combinais", "combinaisons", "combination", "combined",
        "combinee", "combinera", "combines", "combinez", "combing", "comparante", "complante", "composait", "composant",
        "composantes", "composants", "composasse", "composates", "composent", "composite", "compostant", "compotant",
        "congelation", "correction", "correlatif", "correlatifs", "correlating", "correlations", "correlative",
        "corrugation", "courba", "courbas", "courbee", "courbees", "courber", "courbet", "courbez", "coure", "coures",
        "courge", "courges", "courre", "course", "courses", "courte", "courtes", "courue", "courues", "croissant",
        "croissante", "croissantes", "croissants", "deceleration", "decelerations", "decent", "decente", "declinais",
        "declinaisons", "declination", "decroissant", "decroissante", "decroissantes", "decroissants", "densites",
        "density", "dentes", "depare", "deparent", "deridee", "derive", "derived", "derivees", "deriver", "derives",
        "derivez", "deteindre", "drivee", "egrenement", "eight", "entes", "entrais", "enveniment", "etetement",
        "etetements", "etreindre", "evasement", "evasements", "eventerent", "eventrent", "evidement", "evidements",
        "evincement", "evincements", "evitement", "evitements", "excroissance", "exterieur", "exterieure", "extraie",
        "extraies", "extraira", "extrairez", "extrait", "extraite", "extraits", "extras", "extremer", "extremes",
        "extremums", "fastes", "fattest", "fauteur", "fentes", "fourbe", "fourbes", "gentes", "haleur", "hases",
        "hauteurs", "heights", "immensite", "imposante", "improbabilite", "inclinais", "inclinaisons", "inclination",
        "inferieur", "inferieure", "ingenuite", "insensate", "integrite", "intensifie", "intensifs", "intensify",
        "intensites", "intensities", "intensity", "intensive", "intensives", "interallie", "intercale", "intercalee",
        "interdite", "interieur", "interieure", "interieures", "interieurs", "internally", "interpelle", "intervalles",
        "intervals", "inversa", "inversee", "inverser", "inverses", "inversez", "larges", "largess", "larget", "latitude",
        "lentes", "libration", "luminescent", "luminescente", "luminescentes", "luminescents", "masure", "maxima",
        "maximales", "maximums", "measure", "mensure", "mensuree", "mentes", "menure", "mestre", "mesura", "mesurees",
        "mesurer", "mesures", "mesurez", "mesuse", "mesusee", "meure", "minima", "minimales", "minimums", "minium",
        "narration", "narrations", "nivaux", "nivea", "niveal", "opposante", "paleur", "peintes", "penates", "pendes",
        "penes", "penses", "pertes", "pestes", "petes", "phages", "phares", "phased", "phrases", "pintes", "pontes",
        "portabilite", "posterieur", "posterieures", "posterieurs", "probabilisme", "probabiliste", "probabilistes",
        "probabilites", "probabilities", "probability", "profondes", "profounder", "prussienne", "racer", "radiation",
        "radiations", "raleur", "recant", "recence", "recense", "recenter", "recentes", "recentre", "recents", "recette",
        "reclinaison", "recombinaison", "redent", "redente", "reent", "regent", "regente", "regimentation", "relent",
        "rentes", "repare", "reparent", "repent", "repente", "resent", "reteindre", "retente", "revent", "revente",
        "revetement", "revetements", "saleur", "saline", "salines", "sauteur", "segmentation", "sentes", "sepale",
        "separa", "separant", "separee", "separer", "separes", "separez", "seriation", "seriations", "severement", "spare",
        "spine", "spines", "splice", "splices", "splint", "splints", "strangest", "striation", "striations", "stringent",
        "stringiest", "tableaus", "tableaux", "tenements", "tentes", "tourbe", "tourbes", "traced", "tracee", "tracera",
        "tracers", "tracery", "traces", "traceur", "tracey", "tracez", "tracker", "tracter", "trader", "tramer",
        "ulterieur", "ulterieure", "unciforme", "uniform", "uniformed", "uniformes", "uniforms", "vacations", "valeurs",
        "validation", "validations", "valseur", "valuation", "valuations", "vapeur", "variassions", "varierions",
        "vastest", "vehements", "velements", "velocite", "ventes", "vernation", "vetements", "viciation", "viciations",
        "vitiation", "voleur", "weight" };
        QSet<QString> s;
        for (const char* x : w) s.insert(QString::fromUtf8(x));
        return s;
    }();
    return p;
}

QString foldWord(const QString& w) { return text::fold(w).toLower().replace(QChar(0x2019), QLatin1Char('\'')); }

int maxDistance(qsizetype word, qsizetype trigger)
{
    if (word < 5 || trigger < 6) return 0;
    return word >= 9 && trigger >= 9 ? 2 : 1;
}

} // namespace

int distance(const QString& a, const QString& b, int maxd)
{
    const qsizetype n = a.size(), m = b.size();
    if (std::abs(int(n - m)) > maxd) return maxd + 1;
    std::vector<int> prev2(size_t(m + 1)), prev(size_t(m + 1)), cur(size_t(m + 1));
    for (qsizetype j = 0; j <= m; ++j) prev[size_t(j)] = int(j);
    for (qsizetype i = 1; i <= n; ++i) {
        cur[0] = int(i);
        int best = cur[0];
        for (qsizetype j = 1; j <= m; ++j) {
            const int c = a[i - 1] == b[j - 1] ? 0 : 1;
            int v = std::min({ prev[size_t(j)] + 1, cur[size_t(j - 1)] + 1, prev[size_t(j - 1)] + c });
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) v = std::min(v, prev2[size_t(j - 2)] + 1);
            cur[size_t(j)] = v;
            best = std::min(best, v);
        }
        if (best > maxd) return maxd + 1;
        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    return prev[size_t(m)];
}

QString correct(const QString& question, QVector<Fix>* fixes, const QStringList& keep)
{
    QSet<QString> keepWords;                                  // mots des noms protégés (« Pomigliano del Arco »)
    static const QRegularExpression wordRe(QStringLiteral(R"(\p{L}+)"));
    for (const QString& k : keep)
        for (auto it = wordRe.globalMatch(k); it.hasNext();) keepWords.insert(foldWord(it.next().captured(0)));
    QSet<QString> trig;
    for (const auto& t : triggers()) trig.insert(QString::fromUtf8(t.first));

    QString out = question;
    qsizetype shift = 0;
    for (auto it = wordRe.globalMatch(question); it.hasNext();) {
        const auto m = it.next();
        const QString w = m.captured(0);
        // Mot collé à un chiffre ou à « _ » / « . » (référence de laboratoire, nom de fichier, unité) : pas touché.
        const qsizetype s = m.capturedStart(), e = m.capturedEnd();
        auto glued = [&](qsizetype k) { return k >= 0 && k < question.size() && (question[k].isDigit() || question[k] == '_' || question[k] == '.' || question[k] == '-'); };
        if (glued(s - 1) || glued(e)) continue;
        const QString f = foldWord(w);
        if (f.size() < 5 || trig.contains(f) || protectedWords().contains(f) || keepWords.contains(f)) continue;
        if (f.endsWith('s') && (trig.contains(f.chopped(1)) || protectedWords().contains(f.chopped(1)))) continue;
        int bestD = 99;
        QString best, bestShown;
        bool tie = false;
        for (const auto& t : triggers()) {
            const QString tf = QString::fromUtf8(t.first);
            const int md = maxDistance(f.size(), tf.size());
            if (md == 0) continue;
            const int d = distance(f, tf, md);
            if (d > md) continue;
            if (d < bestD) { bestD = d; best = tf; bestShown = QString::fromUtf8(t.second); tie = false; }
            else if (d == bestD && tf != best) tie = true;
        }
        if (best.isEmpty() || tie) continue;
        out.replace(s + shift, w.size(), best);
        shift += best.size() - w.size();
        if (fixes) fixes->append({ w, bestShown });
    }
    return out;
}

QString describe(const QVector<Fix>& fixes)
{
    QStringList parts;
    for (const Fix& f : fixes) parts << QStringLiteral("« %1 » → « %2 »").arg(f.from, f.to);
    return parts.join(QStringLiteral(", "));
}

} // namespace seldon::fuzzy
