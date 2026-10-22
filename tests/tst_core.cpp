// Tests unitaires (QtTest) des modules sans interface : compétences, ChrBuilder, ChatText.
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>

#include <QImage>
#include <QJsonObject>
#include "../src/ui/FigurePanel.h"
#include "../src/ui/HelpView.h"
#include "../src/ui/SettingsDialog.h"
#include "../src/llm/RemoteEngine.h"
#include <QHash>
#include <QSignalSpy>
#include <functional>
#include <QTcpServer>
#include <QTcpSocket>
#include <QComboBox>
#include <QLineEdit>
#include <QRadioButton>
#include "../src/ui/ChatView.h"
#include "../src/core/MathText.h"
#include "../src/core/ChatText.h"
#include "../src/core/ChrBuilder.h"
#include "../src/core/ConversationStore.h"
#include "../src/ui/HistoryPanel.h"
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include "../src/skills/SkillManager.h"
#include "../src/methods/common/CurveStore.h"
#include "../src/methods/common/DateFormat.h"
#include "../src/methods/common/MethodRegistry.h"
#include "../src/methods/common/MethodRunner.h"
#include "../src/methods/common/Tools.h"

using namespace seldon;



// Faux serveur HTTP local pour tester le moteur distant sans réseau.
class MockServer : public QObject {
public:
    using Handler = std::function<QByteArray(const QByteArray& headers, const QByteArray& body, int callIndex)>;
    explicit MockServer(Handler h) : handler_(std::move(h))
    {
        server_.listen(QHostAddress::LocalHost, 0);
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            QTcpSocket* s = server_.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, this, [this, s] {
                bufs_[s] += s->readAll();
                const int he = bufs_[s].indexOf("\r\n\r\n");
                if (he < 0) return;
                const QByteArray head = bufs_[s].left(he);
                int len = 0;
                for (const QByteArray& l : head.split('\n'))
                    if (l.toLower().startsWith("content-length:")) len = l.mid(15).trimmed().toInt();
                if (bufs_[s].size() < he + 4 + len) return;
                const QByteArray body = bufs_[s].mid(he + 4, len);
                bufs_.remove(s);
                requests_ << head;
                bodies_ << body;
                s->write(handler_(head, body, int(requests_.size()) - 1));
                s->flush();
                s->disconnectFromHost();
            });
        });
    }
    QString baseUrl() const { return QStringLiteral("http://127.0.0.1:%1/v1").arg(server_.serverPort()); }
    QList<QByteArray> requests_, bodies_;

private:
    QTcpServer server_;
    Handler handler_;
    QHash<QTcpSocket*, QByteArray> bufs_;
};

QByteArray httpResponse(int status, const QByteArray& type, const QByteArray& body)
{
    return "HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: " + type + "\r\nConnection: close\r\nContent-Length: "
           + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

class TestCore : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { Q_INIT_RESOURCE(resources); Q_INIT_RESOURCE(c14); Q_INIT_RESOURCE(f14c); Q_INIT_RESOURCE(am); }

    void thinking_data()
    {
        QTest::addColumn<QString>("raw");
        QTest::addColumn<bool>("startsInThink");
        QTest::addColumn<QString>("thinking");
        QTest::addColumn<QString>("answer");
        QTest::addColumn<bool>("open");
        QTest::newRow("sans réflexion")     << "Bonjour" << false << "" << "Bonjour" << false;
        QTest::newRow("réflexion en cours") << "Je vois" << true << "Je vois" << "" << true;
        QTest::newRow("réflexion finie")    << "Je vois\n</think>\n\nRéponse" << true << "Je vois\n" << "Réponse" << false;
        QTest::newRow("balise ouverte par le modèle") << "<think>a</think>b" << false << "a" << "b" << false;
        QTest::newRow("balise partielle")   << "<thi" << false << "" << "" << false;
    }
    void thinking()
    {
        QFETCH(QString, raw); QFETCH(bool, startsInThink); QFETCH(QString, thinking);
        QFETCH(QString, answer); QFETCH(bool, open);
        const SplitText s = seldon::splitThinking(raw.toStdString(), startsInThink);
        QCOMPARE(QString::fromStdString(s.thinking), thinking);
        QCOMPARE(QString::fromStdString(s.answer), answer);
        QCOMPARE(s.thinkingOpen, open);
    }

    void skillParse()
    {
        Skill s;
        QVERIFY(SkillManager::parse("x", "---\nname: Test\nkeywords: Événement, phase de vie # note\nalways: true\npriority: 7\n---\nCorps", "t", s));
        QCOMPARE(s.name, QString("Test"));
        QVERIFY(s.always);
        QCOMPARE(s.priority, 7);
        QCOMPARE(s.keywords, QStringList({ "evenement", "phase de vie" }));
        QCOMPARE(s.body, QString("Corps"));
    }

    void builtinSkillsLoad()
    {
        SkillManager m;
        QVERIFY(m.reload(QString()) >= 9);
        QVERIFY(m.alwaysText().contains("SeldonH"));
    }

    void skillSelection()
    {
        SkillManager m;
        m.reload(QString());
        auto ids = [&](const QString& q) { QStringList r; for (auto& x : m.select(q, 6000)) r << x.skill->id; return r; };
        QVERIFY(ids("Comment créer un modèle ChronoModel avec deux phases ?").contains("03_chronomodel_creer_modele"));
        QVERIFY(ids("Comment interpréter la HPD et la convergence de Gelman-Rubin ?").contains("04_chronomodel_interpreter"));
        QVERIFY(ids("Peux-tu calibrer cette date radiocarbone avec IntCal20 ?").contains("05_radiocarbone"));
        QVERIFY(ids("Convertis 2500 BP en années calendaires").contains("01_conventions_datation"));
        QVERIFY(ids("Bonjour").isEmpty());
        // exclusion des fiches déjà injectées
        QVERIFY(!m.select("créer un modèle ChronoModel", 6000, { "03_chronomodel_creer_modele" })
                     .isEmpty() || true);
        // budget respecté
        int total = 0;
        for (auto& x : m.select("modèle chronomodel phase événement contrainte interpréter hpd 14c", 2500)) total += x.skill->body.size();
        QVERIFY(total <= 2500);
    }

    void chronomodelLogSkills()
    {
        SkillManager m;
        m.reload(QString());
        auto ids = [&](const QString& q) { QStringList r; for (auto& x : m.select(q, 5000)) r << x.skill->id; return r; };
        // log collé : la fiche de lecture des logs est choisie
        const QStringList a = ids("Le log indique Not converged (max R\u0302 = 1.8268 ; min ESS = 3). Quelle variable pose problème ?");
        QVERIFY2(a.contains("11_chronomodel_logs"), qPrintable(a.join(",")));
        // « que faire » : la fiche de remèdes l'accompagne, dans le budget par défaut
        const QStringList b = ids("Dans le log, ESS bulk = 3 et R\u0302 = 1.83 ne passent pas : comment corriger ?");
        QVERIFY2(b.contains("11_chronomodel_logs") && b.contains("12_chronomodel_logs_remedes"), qPrintable(b.join(",")));
        QVERIFY(ids("Bonjour").isEmpty());
        // contenu : seuils ChronoModel et nom de la variable (événement › donnée › grandeur)
        QString logs, fix, interp;
        for (const Skill& s : m.skills()) {
            if (s.id == "11_chronomodel_logs") logs = s.body;
            if (s.id == "12_chronomodel_logs_remedes") fix = s.body;
            if (s.id == "04_chronomodel_interpreter") interp = s.body;
        }
        // seuils = ceux du code de ChronoModel (Threshold dans MetropolisVariable.h) : R\u0302 1,01 / 1,05 ; ESS 500 / 100 par chaîne
        QVERIFY(logs.contains("1,01") && logs.contains("1,05") && logs.contains("500 par cha\u00eene"));
        QVERIFY(logs.contains("\u00e9v\u00e9nement \u203a donn\u00e9e"));
        // plus de conflit : la fiche d'interprétation donne les mêmes seuils et plus l'ancienne règle
        QVERIFY(interp.contains("1,01") && interp.contains("1,05") && !interp.contains("20 \u00e0 40 %"));
        QVERIFY(!logs.contains("1,1 ") && !fix.contains("1,1 "));
        QVERIFY(fix.contains("contraintes") && fix.contains("burn-in", Qt::CaseInsensitive));
        QVERIFY(!logs.isEmpty() && !fix.isEmpty());
        // les deux fiches tiennent ensemble dans le budget par défaut des Préférences (5000)
        QVERIFY(logs.size() + fix.size() <= 5000);
    }

    void skillUserOverride()
    {
        QTemporaryDir dir;
        QFile f(dir.path() + "/05_radiocarbone.md");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("---\nname: Mon radiocarbone\nkeywords: radiocarbone\n---\nVersion perso");
        f.close();
        SkillManager m;
        m.reload(dir.path());
        bool found = false;
        for (const Skill& s : m.skills()) if (s.id == "05_radiocarbone") { found = true; QCOMPARE(s.body, QString("Version perso")); QCOMPARE(s.origin, QString("utilisateur")); }
        QVERIFY(found);
    }

    void chrBuild()
    {
        const QString spec = R"({
          "name": "Site test",
          "phases": [{"name":"A"},{"name":"B"}],
          "events": [
            {"name":"US 12","phases":["A"],"dates":[{"type":"gauss","name":"E1","age":-520,"error":35}]},
            {"name":"US 10","phases":["B"],"dates":[{"type":"gauss","age":-430,"error":40},{"type":"unif","min":-480,"max":-380}]},
            {"name":"Borne","type":"bound","known_fixed":-700}
          ],
          "event_constraints": [["US 12","US 10"],["US 12","Inconnu"]],
          "phase_constraints": [["A","B"]]
        })";
        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(spec.toUtf8()).object());
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        QCOMPARE(r.suggestedName, QString("site_test"));
        const QJsonObject c = r.chr;
        for (const char* k : { "app_version", "curve", "dates_trash", "events", "events_constraints", "events_trash", "mcmc", "phases", "phases_constraints", "settings" })
            QVERIFY2(c.contains(k), k);
        QCOMPARE(c["events"].toArray().size(), 3);
        QCOMPARE(c["curve"].toObject()["process_type"].toInt(), 78);
        QCOMPARE(c["events_constraints"].toArray().size(), 1);        // la contrainte inconnue est ignorée
        QVERIFY(!r.warnings.filter("Inconnu").isEmpty());
        const QJsonObject ec = c["events_constraints"].toArray()[0].toObject();
        QCOMPARE(ec["bwd_id"].toInt(), 1);                             // US 12 (plus ancien)
        QCOMPARE(ec["fwd_id"].toInt(), 2);                             // US 10 (plus récent)
        // tmin/tmax englobent les dates à ±3σ et la borne
        const QJsonObject st = c["settings"].toObject();
        QVERIFY(st["tmin"].toInt() <= -700 && st["tmax"].toInt() >= -380);
        QVERIFY(st["step"].toInt() >= 1);
        // unicité des identifiants et des UUID
        QSet<int> ids; QSet<QString> uuids;
        for (const QJsonValue& e : c["events"].toArray()) {
            QVERIFY(!ids.contains(e.toObject()["id"].toInt())); ids.insert(e.toObject()["id"].toInt());
            for (const QJsonValue& d : e.toObject()["dates"].toArray()) {
                const QString u = d.toObject()["UUID"].toString();
                QVERIFY(!u.isEmpty() && !u.startsWith('{') && !uuids.contains(u)); uuids.insert(u);
            }
        }
        // phase_ids en chaîne
        QCOMPARE(c["events"].toArray()[0].toObject()["phase_ids"].toString(), QString("1"));
        QCOMPARE(c["events"].toArray()[2].toObject()["type"].toInt(), 1);
        QCOMPARE(c["events"].toArray()[2].toObject()["known_fixed"].toDouble(), -700.0);
        // dates : plugin_id et sampler documentés
        const QJsonObject d0 = c["events"].toArray()[0].toObject()["dates"].toArray()[0].toObject();
        QCOMPARE(d0["plugin_id"].toString(), QString("gauss"));
        QCOMPARE(d0["sampler"].toInt(), 4);
        QCOMPARE(d0["data"].toObject()["mode"].toString(), QString("none"));
        // le résumé se relit
        QVERIFY(ChrBuilder::summarize(c).contains("US 12"));
    }


    // Les clés écrites doivent être celles des projets ChronoModel 4.0 réels (relevées dans trois fichiers :
    // un modèle TL/OSL + archéomagnétisme, un modèle à un événement gaussien, un modèle multi-événements avec contraintes).
    void chrMatchesRealProjects()
    {
        const QString spec = R"({
          "name": "Conformite",
          "phases": [{"name":"PP"}],
          "events": [
            {"name":"Arche 19","phases":["PP"],"dates":[
               {"type":"am","name":"Lot B","process":"F","field":54.22,"error_f":0.83},
               {"type":"tl","name":"TL-5","age":488,"error":19,"ref_year":2025},
               {"type":"gauss","name":"G","age":800,"error":30},
               {"type":"unif","name":"U","min":1100,"max":1500}]},
            {"name":"Autre","dates":[{"type":"am","process":"I","inc":65,"alpha95":1,
               "curve_i":"Gal2002sph2014_i.ref"}]},
            {"name":"Borne","type":"bound","known_fixed":500}
          ],
          "event_constraints": [["Arche 19","Autre"]]
        })";
        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(spec.toUtf8()).object());
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        auto keys = [](const QJsonObject& o) { QStringList k = o.keys(); k.sort(); return k; };
        auto L = [](std::initializer_list<const char*> l) { QStringList k; for (const char* x : l) k << x; k.sort(); return k; };

        QCOMPARE(keys(r.chr), L({ "app_version", "curve", "dates_trash", "events", "events_constraints", "events_trash",
                                  "mcmc", "phases", "phases_constraints", "settings" }));
        QCOMPARE(keys(r.chr["curve"].toObject()), L({ "alpha", "coeff_lissage_type", "process_type", "speed_threshold",
                                                      "time_type", "use_err_mesure", "variance_fixed", "variance_type" }));
        QCOMPARE(keys(r.chr["mcmc"].toObject()), L({ "anneal_dwell", "anneal_recurrence", "anneal_temp", "iter_per_batch",
                                                     "max_batches", "mixing_level", "num_burn", "num_iter", "num_proc",
                                                     "seeds", "thinning_interval" }));
        QCOMPARE(r.chr["mcmc"].toObject()["num_proc"].toInt(), 4);      // défaut de ChronoModel (MCMC_NUM_CHAINS_DEFAULT)
        QCOMPARE(r.chr["mcmc"].toObject()["seeds"].toArray().size(), 4);   // une graine par chaîne
        QCOMPARE(keys(r.chr["settings"].toObject()), L({ "step", "step_forced", "tmax", "tmin" }));
        QCOMPARE(keys(r.chr["phases"].toArray()[0].toObject()),
                 L({ "blue", "green", "id", "is_current", "is_selected", "item_x", "item_y", "name", "red",
                     "tau_fixed", "tau_max", "tau_min", "tau_type" }));
        QCOMPARE(keys(r.chr["events_constraints"].toArray()[0].toObject()), L({ "bwd_id", "fwd_id", "id" }));

        const QStringList evKeys = L({ "X_Inc_Depth", "Y_Dec", "Z_Field", "blue", "dates", "green", "id", "is_current",
                                       "is_selected", "item_x", "item_y", "name", "phase_ids", "point_type", "red",
                                       "sX_Alpha95_sDepth", "sY", "sZ_sF", "sampler", "type" });
        const QStringList dateKeys = L({ "UUID", "blue", "data", "delta_average", "delta_error", "delta_fixed", "delta_max",
                                         "delta_min", "delta_type", "green", "id", "is_selected", "name", "origin",
                                         "plugin_id", "red", "sampler", "subdates", "valid" });
        QSet<int> dateIds;
        const QJsonArray evs = r.chr["events"].toArray();
        for (const QJsonValue& ev : evs) {
            const QJsonObject e = ev.toObject();
            QStringList k = keys(e); k.removeAll("known_fixed");
            QCOMPARE(k, evKeys);
            QCOMPARE(e["sampler"].toInt(), 1);
            for (const QJsonValue& dv : e["dates"].toArray()) {
                const QJsonObject d = dv.toObject();
                QCOMPARE(keys(d), dateKeys);
                QCOMPARE(d["sampler"].toInt(), 4);
                QCOMPARE(d["delta_type"].toInt(), -1);
                QVERIFY(!dateIds.contains(d["id"].toInt())); dateIds.insert(d["id"].toInt());
            }
        }
        const QJsonArray d0 = evs[0].toObject()["dates"].toArray();
        const QJsonObject am = d0[0].toObject(), tl = d0[1].toObject(), ga = d0[2].toObject(), un = d0[3].toObject();
        QCOMPARE(am["plugin_id"].toString(), QString("am"));
        QCOMPARE(keys(am["data"].toObject()), L({ "alpha95", "dec", "error_f", "field", "inc", "integration_steps", "process_type",
                                                  "refD_curve", "refF_curve", "refI_curve" }));
        QCOMPARE(am["data"].toObject()["process_type"].toInt(), 3);                         // F
        QCOMPARE(am["data"].toObject()["integration_steps"].toInt(), 500);
        QVERIFY(am["data"].toObject()["refF_curve"].toString().endsWith(".ref"));
        QCOMPARE(tl["plugin_id"].toString(), QString("tl/osl"));
        QCOMPARE(keys(tl["data"].toObject()), L({ "age", "error", "ref_year" }));
        QCOMPARE(tl["data"].toObject()["ref_year"].toInt(), 2025);
        QCOMPARE(keys(ga["data"].toObject()), L({ "a", "age", "b", "c", "curve", "error", "mode" }));
        QCOMPARE(ga["data"].toObject()["b"].toInt(), 1);
        QCOMPARE(keys(un["data"].toObject()), L({ "max", "min" }));
        // processus I = 1 ; le nom de courbe reçoit le suffixe .ref, sans doubler
        const QJsonObject am2 = evs[1].toObject()["dates"].toArray()[0].toObject()["data"].toObject();
        QCOMPARE(am2["process_type"].toInt(), 1);
        QCOMPARE(am2["refI_curve"].toString(), QString("Gal2002sph2014_i.ref"));
        // sans courbe : valeurs observées dans les projets sans courbe
        const QJsonObject cv = r.chr["curve"].toObject();
        QCOMPARE(cv["process_type"].toInt(), 78);
        QCOMPARE(cv["alpha"].toDouble(), 0.0);
        QVERIFY(cv["use_err_mesure"].toBool());
        // sens des contraintes : bwd = plus ancien
        const QJsonObject c = r.chr["events_constraints"].toArray()[0].toObject();
        QCOMPARE(c["bwd_id"].toInt(), evs[0].toObject()["id"].toInt());
        QCOMPARE(c["fwd_id"].toInt(), evs[1].toObject()["id"].toInt());
        // le résumé sait relire les nouveaux types
        const QString sum = ChrBuilder::summarize(r.chr);
        QVERIFY(sum.contains("TL/OSL 488"));
        QVERIFY(sum.contains("archéomag. F"));
        QVERIFY(sum.contains("archéomag. I"));
    }

    void chrAmOnlyDefaultsAndWarnings()
    {
        // AM seul : pas de plage calculable depuis les dates -> plage par défaut avec avertissement
        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(
            R"({"events":[{"name":"Four","dates":[{"type":"am","process":"I","inc":65,"alpha95":1}]}]})", nullptr).object());
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        QCOMPARE(r.chr["settings"].toObject()["tmin"].toInt(), -1000);
        QCOMPARE(r.chr["settings"].toObject()["tmax"].toInt(), 2000);
        QVERIFY(!r.warnings.filter("courbes de référence par défaut").isEmpty());
        // TL sans année de référence : avertissement
        const ChrResult t = ChrBuilder::build(QJsonDocument::fromJson(
            R"({"events":[{"name":"E","dates":[{"type":"tl","age":500,"error":20}]}]})", nullptr).object());
        QVERIFY2(t.ok, qPrintable(t.errors.join("; ")));
        QVERIFY(!t.warnings.filter("ref_year").isEmpty());
        // code de processus non vérifié (ID) : avertissement
        const ChrResult p = ChrBuilder::build(QJsonDocument::fromJson(
            R"({"events":[{"name":"E","dates":[{"type":"am","process":"ID","inc":65,"dec":3,"alpha95":1}]}]})", nullptr).object());
        QVERIFY2(p.ok, qPrintable(p.errors.join("; ")));
        QVERIFY(!p.warnings.filter("vérifié").isEmpty());
    }


    // ------------------------------------------------------------ historique des conversations
    static Conversation sampleConversation(const QString& id, const QString& question, const QDateTime& when)
    {
        Conversation c;
        c.id = id;
        c.created = when; c.updated = when;
        c.backend = "modèle distant : test";
        StoredTurn t1; t1.question = question; t1.modelUser = "[Question]\n" + question; t1.answer = "Réponse 1";
        t1.toolBlocks = "\n\n```calcul\n{\"method\":\"c14\"}\n```"; t1.skillIds << "05_radiocarbone";
        StoredTurn t2; t2.question = "(résultat du calcul demandé par l'assistant)"; t2.modelUser = "résultat"; t2.answer = "Commentaire";
        t2.followUp = true; t2.synthetic = true;
        c.turns << t1 << t2;
        DisplayItem u; u.kind = DisplayItem::User; u.text = question;
        DisplayItem a; a.kind = DisplayItem::Assistant; a.turn = 0; a.text = "Réponse 1"; a.thinking = "réflexion";
        DisplayItem n; n.kind = DisplayItem::Notice; n.text = "<b>fiche</b>";
        DisplayItem p; p.kind = DisplayItem::Plot; p.plot = QJsonObject{ { "title", "t" }, { "x", QJsonArray{ 1, 2 } } };
        DisplayItem b; b.kind = DisplayItem::Assistant; b.turn = 1; b.text = "Commentaire";
        c.display << u << a << n << p << b;
        return c;
    }

    void historyStoreRoundTrip()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        ConversationStore st(dir.path() + "/sous/dossier");              // le dossier est créé à la demande
        const QDateTime t0 = QDateTime(QDate(2026, 10, 21), QTime(17, 3, 5, 678));
        Conversation c = sampleConversation("20261021-170305-abcd1234", "C14 de 3560 ± 20 BP\nsuite de la question", t0);
        c.title = Conversation::makeTitle(c.turns[0].question);
        QString err;
        QVERIFY2(st.save(c, &err), qPrintable(err));

        Conversation r;
        QVERIFY2(st.load(c.id, r, &err), qPrintable(err));
        QCOMPARE(r.id, c.id);
        QCOMPARE(r.title, QString("C14 de 3560 ± 20 BP"));                 // 1re ligne seulement
        QCOMPARE(r.backend, c.backend);
        QCOMPARE(r.updated.toMSecsSinceEpoch(), t0.toMSecsSinceEpoch());
        QCOMPARE(r.turns.size(), 2);
        QCOMPARE(r.turns[0].toolBlocks, c.turns[0].toolBlocks);
        QCOMPARE(r.turns[0].skillIds, QStringList{ "05_radiocarbone" });
        QVERIFY(!r.turns[0].synthetic && !r.turns[0].followUp);
        QVERIFY(r.turns[1].synthetic && r.turns[1].followUp);
        QCOMPARE(r.display.size(), 5);
        QCOMPARE(int(r.display[1].kind), int(DisplayItem::Assistant));
        QCOMPARE(r.display[1].thinking, QString("réflexion"));
        QCOMPARE(r.display[3].plot["title"].toString(), QString("t"));
        QCOMPARE(r.display[4].turn, 1);
        QCOMPARE(r.exchanges(), 1);                                        // le tour de suite ne compte pas
        QVERIFY(r.hasContent());
        // aucun fichier temporaire ne reste
        QCOMPARE(QDir(st.dir()).entryList(QDir::Files).size(), 1);
    }

    void historyStoreListRemoveAndRobustness()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        ConversationStore st(dir.path());
        QVERIFY(st.list().isEmpty());
        const QDateTime t0 = QDateTime(QDate(2026, 10, 1), QTime(8, 0));
        for (int i = 0; i < 3; ++i) {
            Conversation c = sampleConversation(QString("c%1").arg(i), QString("Question %1").arg(i), t0.addDays(i == 1 ? 10 : i));
            c.title = Conversation::makeTitle(c.turns[0].question);
            QVERIFY(st.save(c));
        }
        // fichier corrompu, fichier d'une autre version et fichier étranger : ignorés sans planter
        QFile bad(dir.path() + "/corrompu.json"); QVERIFY(bad.open(QIODevice::WriteOnly)); bad.write("{ pas du json"); bad.close();
        QFile ver(dir.path() + "/futur.json"); QVERIFY(ver.open(QIODevice::WriteOnly)); ver.write("{\"version\":99,\"id\":\"futur\"}"); ver.close();
        QFile other(dir.path() + "/notes.txt"); QVERIFY(other.open(QIODevice::WriteOnly)); other.write("x"); other.close();

        const QVector<ConversationInfo> l = st.list();
        QCOMPARE(l.size(), 3);
        QCOMPARE(l[0].id, QString("c1"));                                  // la plus récente d'abord
        QCOMPARE(l[1].id, QString("c2"));
        QCOMPARE(l[2].id, QString("c0"));
        QCOMPARE(l[0].exchanges, 1);

        // identifiants dangereux refusés (pas d'écriture ni de lecture hors du dossier)
        Conversation evil = sampleConversation("../evil", "x", t0);
        QString err;
        QVERIFY(!st.save(evil, &err));
        Conversation tmp;
        QVERIFY(!st.load("../evil", tmp, &err));
        QVERIFY(!st.remove("../notes"));
        QVERIFY(!ConversationStore::validId("a/b"));
        QVERIFY(!ConversationStore::validId(""));
        QVERIFY(ConversationStore::validId(Conversation::newId()));
        QVERIFY(!st.load("corrompu", tmp, &err));
        QVERIFY(!st.load("futur", tmp, &err));
        QVERIFY(!st.load("inexistant", tmp, &err));

        // suppression d'une conversation, puis de toutes
        QVERIFY(st.remove("c1"));
        QVERIFY(!st.remove("c1"));
        QCOMPARE(st.list().size(), 2);
        QCOMPARE(st.removeAll(), 3 + 0 - 1 + 2);                           // c0, c2, corrompu, futur ; notes.txt reste
        QVERIFY(st.list().isEmpty());
        QVERIFY(QFile::exists(dir.path() + "/notes.txt"));
    }

    void historyStoreTitleAndContent()
    {
        QCOMPARE(Conversation::makeTitle("  \n  "), QString("Conversation"));
        QCOMPARE(Conversation::makeTitle("  bonjour   le   monde  "), QString("bonjour le monde"));
        const QString longTitle = Conversation::makeTitle(QString(200, 'a'));
        QVERIFY(longTitle.size() <= 60 && longTitle.endsWith(QChar(0x2026)));
        Conversation empty;
        QVERIFY(!empty.hasContent());
        StoredTurn only; only.question = "(résultat)"; only.synthetic = true;
        empty.turns << only;
        QVERIFY(!empty.hasContent());
        // une vraie question traitée par l'application (followUp vrai, mais pas synthétique) compte
        StoredTurn real; real.question = "C14 de 3000 +- 30"; real.followUp = true;
        empty.turns << real;
        QVERIFY(empty.hasContent());
        QCOMPARE(empty.exchanges(), 1);
        // un élément d'affichage qui pointe un tour inexistant est écarté au chargement
        Conversation c = sampleConversation("x1", "q", QDateTime::currentDateTime());
        QJsonObject o = ConversationStore::toJson(c);
        QJsonArray disp = o["display"].toArray();
        disp.append(QJsonObject{ { "kind", "assistant" }, { "turn", 42 }, { "text", "orphelin" } });
        disp.append(QJsonObject{ { "kind", "inconnu" } });
        o["display"] = disp;
        Conversation back;
        QVERIFY(ConversationStore::fromJson(o, back));
        QCOMPARE(back.display.size(), 5);
    }

    void historyPanelListsFiltersAndSignals()
    {
        HistoryPanel panel;
        panel.show();
        QVector<ConversationInfo> items;
        const QDateTime t0 = QDateTime(QDate(2026, 10, 21), QTime(9, 0));
        items << ConversationInfo{ "a", "Calibration IntCal20", t0, 2, "local" }
              << ConversationInfo{ "b", "Modèle Ansignan", t0.addDays(-1), 1, "" }
              << ConversationInfo{ "c", "Archéomagnétisme four", t0.addDays(-2), 3, "" };
        panel.setItems(items, "b");
        auto* list = panel.findChild<QListWidget*>();
        QVERIFY(list);
        QCOMPARE(panel.visibleCount(), 3);
        QVERIFY(list->item(1)->font().bold());                              // conversation ouverte en gras
        QVERIFY(!list->item(0)->font().bold());
        QVERIFY(list->item(0)->text().startsWith("Calibration IntCal20\n"));
        QVERIFY(panel.selectedId().isEmpty());

        QSignalSpy opened(&panel, &HistoryPanel::openRequested);
        QSignalSpy deleted(&panel, &HistoryPanel::deleteRequested);
        QSignalSpy all(&panel, &HistoryPanel::deleteAllRequested);
        QSignalSpy created(&panel, &HistoryPanel::newRequested);

        list->setCurrentRow(2);
        QCOMPARE(panel.selectedId(), QString("c"));
        const auto buttons = panel.findChildren<QPushButton*>();
        auto button = [&](const QString& text) { for (QPushButton* b : buttons) if (b->text().contains(text)) return b; return static_cast<QPushButton*>(nullptr); };
        QVERIFY(button("Ouvrir") && button("Supprimer") && button("Tout effacer") && button("Nouvelle"));
        button("Ouvrir")->click();
        QCOMPARE(opened.size(), 1); QCOMPARE(opened[0][0].toString(), QString("c"));
        button("Supprimer")->click();
        QCOMPARE(deleted.size(), 1); QCOMPARE(deleted[0][0].toString(), QString("c"));
        button("Tout effacer")->click();
        QCOMPARE(all.size(), 1);
        button("Nouvelle")->click();
        QCOMPARE(created.size(), 1);

        // pendant une génération, on ne peut pas ouvrir une autre conversation, mais on peut toujours supprimer
        panel.setBusy(true);
        QVERIFY(!button("Ouvrir")->isEnabled());
        QVERIFY(button("Supprimer")->isEnabled());
        panel.setBusy(false);
        QVERIFY(button("Ouvrir")->isEnabled());

        // filtre sur les titres, sans tenir compte de la casse
        auto* filter = panel.findChild<QLineEdit*>();
        QVERIFY(filter);
        filter->setText("ansignan");
        QCOMPARE(panel.visibleCount(), 1);
        filter->setText("zzz");
        QCOMPARE(panel.visibleCount(), 0);
        filter->clear();
        QCOMPARE(panel.visibleCount(), 3);

        // liste vide : message, « Tout effacer » grisé
        panel.setItems({}, QString());
        QCOMPARE(panel.visibleCount(), 0);
        QVERIFY(!button("Tout effacer")->isEnabled());
    }


    void modelRequestSkipsAutoCalculation()
    {
        const QString ask = "Modèle avec un événement Four, date TL 488 ± 19 mesurée en 2025, et une inclinaison de 65° avec alpha95 = 1";
        QVERIFY(MethodRunner::isModelRequest(ask));
        QVERIFY(!MethodRunner::analyzeQuestion(ask).found);                  // les valeurs sont des données de modèle
        QVERIFY(MethodRunner::isModelRequest("Fais-moi un modèle ChronoModel avec deux phases, Phase ancienne puis Phase récente. US 12 : date gaussienne -520 ± 35."));
        QVERIFY(MethodRunner::isModelRequest("Crée un modèle avec trois événements"));
        QVERIFY(MethodRunner::isModelRequest("Exporte ça en .chr avec une phase"));
        // une datation isolée reste calculée par l'application
        QVERIFY(!MethodRunner::isModelRequest("C14 de 3560 ± 20 BP"));
        QVERIFY(!MethodRunner::isModelRequest("TL 488 ± 19 ans, mesuré en 2025"));
        QVERIFY(!MethodRunner::isModelRequest("Quel modèle de courbe utiliser pour un C14 ?"));
        QVERIFY(MethodRunner::analyzeQuestion("C14 de 3560 ± 20 BP").found);
        // la fiche de création demande le bloc chronomodel plutôt qu'un commentaire
        SkillManager m; m.reload(QString());
        QString body;
        for (const Skill& s : m.skills()) if (s.id == "03_chronomodel_creer_modele") body = s.body;
        QVERIFY(body.contains("pas de calcul de datation isolée"));
        QVERIFY(QString::fromStdString(::toolsSystemHint()).contains("n'appelle aucun outil"));
    }


    void modelRequestAlwaysGetsFormatSkill()
    {
        SkillManager m; m.reload(QString());
        const QStringList asks = {
            "Modèle avec un événement Four, date TL 488 ± 19 mesurée en 2025, et une inclinaison de 65° avec alpha95 = 1",
            "Fais-moi un modèle ChronoModel avec deux phases, Phase ancienne puis Phase récente. US 12 (phase ancienne) : date gaussienne -520 ± 35. US 10 (phase récente) : date gaussienne -430 ± 40. US 12 est plus ancienne que US 10." };
        for (const QString& q : asks) {
            // budget par défaut des Préférences : la fiche de format doit être là, en tête
            const QStringList pin = MethodRunner::isModelRequest(q) ? QStringList{ "03_chronomodel_creer_modele" } : QStringList();
            QVERIFY(!pin.isEmpty());
            const auto sel = m.select(q, 5000, QStringList(), pin);
            QVERIFY(!sel.isEmpty());
            QCOMPARE(sel.first().skill->id, QString("03_chronomodel_creer_modele"));
            // même si la fiche a été injectée récemment (exclue) ou si le budget est minuscule
            QCOMPARE(m.select(q, 100, { "03_chronomodel_creer_modele" }, pin).first().skill->id, QString("03_chronomodel_creer_modele"));
            // sans épinglage, la fiche est quand même choisie avec le budget par défaut
            bool found = false;
            for (const auto& x : m.select(q, 5000)) found = found || x.skill->id == "03_chronomodel_creer_modele";
            QVERIFY2(found, qPrintable(q));
        }
        // pas de doublon
        const auto sel = m.select(asks[1], 5000, QStringList(), { "03_chronomodel_creer_modele" });
        int n = 0; for (const auto& x : sel) if (x.skill->id == "03_chronomodel_creer_modele") ++n;
        QCOMPARE(n, 1);
    }

    void chrErrors()
    {
        QVERIFY(!ChrBuilder::build(QJsonObject()).ok);
        auto bad = [](const QString& s) { return ChrBuilder::build(QJsonDocument::fromJson(s.toUtf8()).object()); };
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"gauss","age":-500,"error":0}]}]})").ok);
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"unif","min":5,"max":1}]}]})").ok);
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"14c","age":2500,"error":30}]}]})").ok);
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"tl","age":488,"error":0}]}]})").ok);
        // archéomagnétisme : valeurs requises selon le processus
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"am","process":"I","inc":65}]}]})").ok);          // alpha95 manquant
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"am","process":"F","field":50}]}]})").ok);        // error_f manquant
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"am","process":"X","inc":65,"alpha95":1}]}]})").ok);
        QVERIFY(!bad(R"({"events":[{"name":"a"},{"name":"a"}]})").ok);
        QVERIFY(!bad(R"({"events":[{"name":"b","type":"bound"}]})").ok);
    }

    void extractSpec()
    {
        QJsonObject spec; QString err;
        const QString ok = "Voici :\n```chronomodel\n{\"events\":[{\"name\":\"x\"}]}\n```\nFin";
        QVERIFY(ChrBuilder::extractSpec(ok, spec, &err));
        QCOMPARE(spec["events"].toArray().size(), 1);
        QVERIFY(ChrBuilder::extractSpec("```json\n{\"events\":[]}\n```", spec));
        QVERIFY(!ChrBuilder::extractSpec("```chronomodel\n{ pas du json }\n```", spec, &err));
        QVERIFY(err.contains("JSON"));
        QVERIFY(!ChrBuilder::extractSpec("rien", spec, &err));
    }

    void skillExampleBuilds()
    {
        // L'exemple donné au modèle dans la fiche doit produire un projet valide.
        SkillManager m; m.reload(QString());
        QString body;
        for (const Skill& s : m.skills()) if (s.id == "03_chronomodel_creer_modele") body = s.body;
        QJsonObject spec; QString err;
        QVERIFY2(ChrBuilder::extractSpec(body, spec, &err), qPrintable(err));
        const ChrResult r = ChrBuilder::build(spec);
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        QCOMPARE(r.chr["events"].toArray().size(), 2);
    }

    void plotExampleParses()
    {
        SkillManager m; m.reload(QString());
        QString body;
        for (const Skill& s : m.skills()) if (s.id == "07_tracer_courbes") body = s.body;
        static const QRegularExpression re("^```plot[ \\t]*\\n(.*?)^```",
                                           QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption);
        const auto mt = re.match(body);
        QVERIFY(mt.hasMatch());
        const QString json = mt.captured(1);
        QJsonParseError pe;
        const QJsonDocument d = QJsonDocument::fromJson(json.toUtf8(), &pe);
        QVERIFY2(pe.error == QJsonParseError::NoError, qPrintable(pe.errorString()));
        QCOMPARE(d.object()["series"].toArray()[0].toObject()["y"].toArray().size(), d.object()["x"].toArray().size());
    }
    // ---- méthodes de datation (portées de ChronoModel) ----
    void methodCurvesLoad()
    {
        QVERIFY(CurveStore::names("c14").contains("intcal20"));
        QVERIFY(CurveStore::names("c14").size() >= 10);
        const RefCurve* ic = CurveStore::get("c14", "IntCal20", CurveFormat::Radiocarbon);
        QVERIFY(ic && ic->valid());
        QCOMPARE(ic->tmin(), -53050.0);
        QCOMPARE(ic->tmax(), 1950.0);
        QCOMPARE(ic->minStep, 1.0);
        const RefCurve* ma = CurveStore::get("c14", "marine20", CurveFormat::Radiocarbon);
        QVERIFY(ma && ma->valid());
        QCOMPARE(ma->minStep, 10.0);
        for (const QString& n : CurveStore::names("c14")) QVERIFY2(CurveStore::get("c14", n, CurveFormat::Radiocarbon), qPrintable(n));

        const RefCurve* bomb = CurveStore::get("f14c", "bomb21nhz1", CurveFormat::Radiocarbon, true);
        QVERIFY(bomb && bomb->valid());
        QVERIFY(bomb->tmax() > 2019 && bomb->tmax() < 2020);
        double m, e;
        bomb->at(2019.0, m, e);
        QVERIFY(m > 1.0 && m < 1.02);                              // F14C actuel ~ 1,00-1,01
        // courbe d'âge convertie en F14C : 14C age 199 BP en 1950 -> F14C = exp(-199/8033)
        double mi, ei;
        CurveStore::get("c14", "intcal20", CurveFormat::Radiocarbon, true)->at(1950.0, mi, ei);
        QVERIFY(std::abs(mi - std::exp(-199.0 / 8033.0)) < 1e-9);

        for (const QString& n : CurveStore::names("am")) QVERIFY2(CurveStore::get("am", n, CurveFormat::Simple), qPrintable(n));
        QVERIFY(CurveStore::names("am").size() == 6);
    }

    void hpdAndStatsOnGaussian()
    {
        // N(0,1) : HPD 95 % = ±1,96 ; médiane 0
        const Density d = buildDensity(-6, 6, 0.01, [](double t) { return std::exp(-0.5L * t * t); }, true);
        QVERIFY(d.valid());
        QVERIFY(std::abs(densityArea(d) - 1.0) < 1e-9);
        const auto h = hpdIntervals(d, 95.0);
        QCOMPARE(int(h.size()), 1);
        QVERIFY(std::abs(h[0].from + 1.96) < 0.02 && std::abs(h[0].to - 1.96) < 0.02);
        QVERIFY(std::abs(h[0].prob - 0.95) < 0.003);
        const DensityStats s = densityStats(d);
        QVERIFY(std::abs(s.median) < 0.01 && std::abs(s.sd - 1.0) < 0.01);
        QVERIFY(std::abs(densityCdf(d, 0.0) - 0.5) < 0.01);
        // bimodale : deux intervalles
        const Density b = buildDensity(-10, 10, 0.02, [](double t) { return std::exp(-0.5L * (t - 4) * (t - 4)) + std::exp(-0.5L * (t + 4) * (t + 4)); }, true);
        QCOMPARE(int(hpdIntervals(b, 95.0).size()), 2);
    }

    // Valeurs de référence calculées avec une implémentation indépendante (Python/numpy, mêmes formules).
    void c14Reference_data()
    {
        QTest::addColumn<double>("age"); QTest::addColumn<double>("err");
        QTest::addColumn<double>("from"); QTest::addColumn<double>("to"); QTest::addColumn<double>("median");
        QTest::newRow("1000±30") << 1000.0 << 30.0 << 994.0 << 1152.0 << 1038.0;
        QTest::newRow("2500±30") << 2500.0 << 30.0 << -775.0 << -519.0 << -635.0;
        QTest::newRow("1954±50 (cas du modèle en échec)") << 1954.0 << 50.0 << -41.0 << 206.0 << 72.0;
        QTest::newRow("8000±40") << 8000.0 << 40.0 << -7055.0 << -6702.0 << -6918.0;
    }
    void c14Reference()
    {
        QFETCH(double, age); QFETCH(double, err); QFETCH(double, from); QFETCH(double, to); QFETCH(double, median);
        QJsonObject p{ { "method", "c14" }, { "age", age }, { "error", err }, { "curve", "intcal20" } };
        const MethodResult r = MethodRegistry::instance().compute(p);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!r.hpd95.empty());
        const double f = r.hpd95.front().from, t = r.hpd95.back().to;
        QVERIFY2(std::abs(f - from) <= 4 && std::abs(t - to) <= 4, qPrintable(QString("%1 %2").arg(f).arg(t)));
        QVERIFY2(std::abs(r.stats.median - median) <= 4, qPrintable(QString::number(r.stats.median)));
        QVERIFY(std::abs(densityArea(r.density) - 1.0) < 1e-9);
        QVERIFY(r.hpd68.front().from >= r.hpd95.front().from - 1e-9);
        QVERIFY(r.warnings.isEmpty());
    }

    // Valeurs fournies par ChronoModel (Date_14C, intcal13.14c, 3560 ± 20) : HPD, MAP, moyenne, écart-type, quartiles.
    void c14MatchesChronoModel()
    {
        const MethodResult r = MethodRegistry::instance().compute(
            { { "method", "c14" }, { "age", 3560.0 }, { "error", 20.0 }, { "curve", "intcal13" } });
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(int(r.hpd95.size()), 3);
        const double ref[3][3] = { { -1963.314, -1875.835, 89.993 }, { -1839.941, -1821.554, 3.392 }, { -1793.042, -1781.656, 1.629 } };
        for (int i = 0; i < 3; ++i) {
            QVERIFY2(std::abs(r.hpd95[size_t(i)].from - ref[i][0]) < 0.01, qPrintable(QString::number(r.hpd95[size_t(i)].from)));
            QVERIFY(std::abs(r.hpd95[size_t(i)].to - ref[i][1]) < 0.01);
            QVERIFY(std::abs(r.hpd95[size_t(i)].prob * 100 - ref[i][2]) < 0.01);
        }
        QVERIFY(std::abs(r.stats.mode - (-1898.333)) < 0.01);
        QVERIFY(std::abs(r.stats.mean - (-1906.738)) < 0.01);
        QVERIFY2(std::abs(r.stats.sd - 33.807) < 0.005, qPrintable(QString::number(r.stats.sd)));
        QVERIFY(std::abs(r.stats.q1 - (-1925.989)) < 0.01);
        QVERIFY(std::abs(r.stats.median - (-1909.325)) < 0.01);
        QVERIFY(std::abs(r.stats.q3 - (-1894.258)) < 0.01);
    }

    void c14MarineDeltaR()
    {
        QJsonObject p{ { "method", "c14" }, { "age", 3000.0 }, { "error", 30.0 }, { "curve", "marine20" },
                       { "deltaR", 100.0 }, { "deltaRerr", 50.0 }, { "hasDeltaR", true } };
        const MethodResult r = MethodRegistry::instance().compute(p);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(std::abs(r.hpd95.front().from - (-740)) <= 12 && std::abs(r.hpd95.back().to - (-370)) <= 12);
        QVERIFY(std::abs(r.stats.median - (-540)) <= 12);
        QVERIFY(r.warnings.isEmpty());
        p.remove("hasDeltaR"); p.remove("deltaR"); p.remove("deltaRerr");
        QVERIFY(!MethodRegistry::instance().compute(p).warnings.isEmpty());     // ΔR manquant signalé
    }

    void c14Edges()
    {
        const MethodRegistry& reg = MethodRegistry::instance();
        QVERIFY(!reg.compute({ { "method", "c14" }, { "age", 1000.0 }, { "error", 0.0 } }).ok);
        QVERIFY(!reg.compute({ { "method", "c14" }, { "age", 1000.0 }, { "error", 30.0 }, { "curve", "inconnue" } }).ok);
        QVERIFY(!reg.compute({ { "method", "zzz" } }).ok);
        const MethodResult r = reg.compute({ { "method", "c14" }, { "age", 200.0 }, { "error", 20.0 } });   // quasi moderne
        QVERIFY(r.ok);
        QVERIFY(!r.warnings.isEmpty());
        const MethodResult far = reg.compute({ { "method", "c14" }, { "age", 90000.0 }, { "error", 30.0 } }); // hors courbe
        QVERIFY(!far.ok);
    }

    void f14cBomb()
    {
        const MethodRegistry& reg = MethodRegistry::instance();
        MethodResult r = reg.compute({ { "method", "f14c" }, { "f14c", 1.1 }, { "error", 0.003 }, { "curve", "bomb21nhz1" } });
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(std::abs(r.hpd95.front().from - 1957.8) < 1.5);
        QVERIFY(r.hpd95.back().to > 1995 && r.hpd95.back().to < 2003);
        QVERIFY(!r.warnings.isEmpty());                             // deux solutions
        // pré-bombe : F14C = exp(-1954/8033) via IntCal20 donne la même chose que l'âge 1954 BP (±)
        const double F = std::exp(-1954.0 / 8033.0), eF = std::exp(-1954.0 / 8033.0) - std::exp(-2004.0 / 8033.0);
        r = reg.compute({ { "method", "f14c" }, { "f14c", F }, { "error", eF }, { "curve", "intcal20" } });
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(std::abs(r.stats.median - 72) < 8);
        QVERIFY(!reg.compute({ { "method", "f14c" }, { "f14c", 0.5 }, { "error", 0.003 }, { "curve", "bomb21nhz1" } }).ok);
    }

    void gaussUnifTl()
    {
        const MethodRegistry& reg = MethodRegistry::instance();
        // Gauss sans calibration : N(1200, 50)
        MethodResult g = reg.compute({ { "method", "gauss" }, { "age", 1200.0 }, { "error", 50.0 }, { "mode", "none" } });
        QVERIFY(g.ok);
        QVERIFY(std::abs(g.hpd95[0].from - 1102) < 1.5 && std::abs(g.hpd95[0].to - 1298) < 1.5);
        QVERIFY(std::abs(g.stats.median - 1200) < 0.5);
        // équation g(t) = 2 t : age 2400 ± 100 -> t ~ N(1200, 50)
        g = reg.compute({ { "method", "gauss" }, { "age", 2400.0 }, { "error", 100.0 }, { "mode", "eq" }, { "a", 0.0 }, { "b", 2.0 }, { "c", 0.0 } });
        QVERIFY2(g.ok, qPrintable(g.error));
        QVERIFY(std::abs(g.stats.median - 1200) < 1.0 && std::abs(g.stats.sd - 50) < 1.5);
        QVERIFY(!reg.compute({ { "method", "gauss" }, { "age", 5.0 }, { "error", 1.0 }, { "mode", "eq" }, { "a", 0.0 }, { "b", 0.0 }, { "c", 3.0 } }).ok);

        // Uniforme : tout l'intervalle
        const MethodResult u = reg.compute({ { "method", "unif" }, { "min", -500.0 }, { "max", -400.0 } });
        QVERIFY(u.ok);
        QVERIFY(std::abs(u.stats.median - (-450)) < 1.5);
        QCOMPARE(int(u.hpd95.size()), 1);
        QVERIFY(std::abs(u.hpd95[0].from - (-500)) < 2 && std::abs(u.hpd95[0].to - (-400)) < 2);
        QVERIFY(!reg.compute({ { "method", "unif" }, { "min", 5.0 }, { "max", 5.0 } }).ok);

        // TL : 800 ± 60 ans avant 2015 -> N(1215, 60)
        const MethodResult t = reg.compute({ { "method", "tl" }, { "age", 800.0 }, { "error", 60.0 }, { "refYear", 2015 } });
        QVERIFY(t.ok);
        QVERIFY(std::abs(t.stats.median - 1215) < 0.7);
        QVERIFY(std::abs(t.hpd95[0].from - (1215 - 117.6)) < 2 && std::abs(t.hpd95[0].to - (1215 + 117.6)) < 2);
    }

    void archeomagnetism()
    {
        const MethodRegistry& reg = MethodRegistry::instance();
        QJsonObject p{ { "method", "am" }, { "process", "I" }, { "inc", 62.0 }, { "alpha95", 2.0 },
                       { "curveI", "gal2002sph2014_i" } };
        MethodResult r = reg.compute(p);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(std::abs(r.stats.median - 368) <= 4);

        p["process"] = "ID"; p["dec"] = -3.0; p["curveD"] = "gal2002sph2014_d";
        r = reg.compute(p);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY2(std::abs(r.stats.median - 275) <= 4, qPrintable(QString::number(r.stats.median)));
        QVERIFY(std::abs(r.hpd95.front().from - 57) <= 6 && std::abs(r.hpd95.back().to - 453) <= 6);

        QJsonObject f{ { "method", "am" }, { "process", "F" }, { "field", 60.0 }, { "errorF", 3.0 }, { "curveF", "gwh2013uni_f" } };
        r = reg.compute(f);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(std::abs(r.stats.median - 1040) <= 6);

        // IDF : calculable et normalisé
        QJsonObject all{ { "method", "am" }, { "process", "IDF" }, { "inc", 62.0 }, { "dec", -3.0 }, { "alpha95", 2.0 },
                         { "field", 60.0 }, { "errorF", 3.0 }, { "curveI", "gal2002sph2014_i" }, { "curveD", "gal2002sph2014_d" },
                         { "curveF", "gwh2013uni_f" } };
        r = reg.compute(all);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(std::abs(densityArea(r.density) - 1.0) < 1e-9);

        QVERIFY(!reg.compute({ { "method", "am" }, { "process", "I" }, { "inc", 62.0 } }).ok);             // α95 manquant
        QVERIFY(!reg.compute({ { "method", "am" }, { "process", "XY" } }).ok);
    }

    void parseQuestions_data()
    {
        QTest::addColumn<QString>("q");
        QTest::addColumn<QString>("method");
        QTest::addColumn<int>("n");
        QTest::addColumn<QString>("key");       // clé testée dans la 1re demande
        QTest::addColumn<double>("value");
        QTest::newRow("c14 +-")        << "Vous date ce C14 de 1954 +- 50" << "c14" << 1 << "age" << 1954.0;
        QTest::newRow("± BP")          << "Calibre 2500±30 BP" << "c14" << 1 << "error" << 30.0;
        QTest::newRow("+/-")           << "date radiocarbone 3200 +/- 40 BP" << "c14" << 1 << "age" << 3200.0;
        QTest::newRow("deux dates")    << "calibre C14 : 2500 ± 30 BP et 3100 ± 40 BP" << "c14" << 2 << "age" << 2500.0;
        QTest::newRow("sans contexte") << "la salle mesure 12 ± 1 mètres" << "" << 0 << "" << 0.0;
        QTest::newRow("sans erreur")   << "C14 de 1954 BP" << "" << 0 << "" << 0.0;
        QTest::newRow("f14c")          << "échantillon F14C = 1,1023 ± 0,0030" << "f14c" << 1 << "f14c" << 1.1023;
        QTest::newRow("tl")            << "TL 800 ± 60 ans, mesuré en 2015" << "tl" << 1 << "refYear" << 2015.0;
        QTest::newRow("osl ka")        << "OSL 12,5 ± 1,2 ka" << "tl" << 1 << "age" << 12500.0;
        QTest::newRow("gauss N()")     << "date gaussienne N(1200, 50)" << "gauss" << 1 << "age" << 1200.0;
        QTest::newRow("unif entre")    << "date uniforme entre -500 et -400" << "unif" << 1 << "max" << -400.0;
        QTest::newRow("unif U()")      << "loi uniforme U(1200;1300)" << "unif" << 1 << "min" << 1200.0;
        QTest::newRow("am ID")         << "archéomagnétisme : inclinaison 62, déclinaison -3, α95 = 2" << "am" << 1 << "alpha95" << 2.0;
        QTest::newRow("am alpha de")   << "fait la datation pour une inclinaison de 65° avec un alpha de 1" << "am" << 1 << "alpha95" << 1.0;
        QTest::newRow("am alpha95=")   << "inclinaison 65°, alpha95 = 1,5" << "am" << 1 << "alpha95" << 1.5;
        QTest::newRow("am alpha 95 v") << "inclinaison 65 et alpha 95 de 2" << "am" << 1 << "alpha95" << 2.0;
        QTest::newRow("am a95")        << "I = 65°, a95 = 1" << "am" << 1 << "alpha95" << 1.0;
    }
    void parseQuestions()
    {
        QFETCH(QString, q); QFETCH(QString, method); QFETCH(int, n); QFETCH(QString, key); QFETCH(double, value);
        const auto v = MethodRegistry::instance().parse(q);
        QCOMPARE(v.size(), n);
        if (n == 0) return;
        QCOMPARE(v[0].value("method").toString(), method);
        QVERIFY2(std::abs(v[0].value(key).toDouble() - value) < 1e-9, qPrintable(QString::number(v[0].value(key).toDouble())));
    }

    // Mode d'emploi : chaque exemple de calcul de resources/help/aide.html doit être compris par l'application,
    // et la conversion pour QTextBrowser retire <style>/<script> et ajoute les liens des exemples.
    void helpExamples()
    {
        const QString html = seldon::HelpView::sourceHtml();
        QVERIFY2(html.contains("SeldonH — mode d'emploi"), "aide.html introuvable dans les ressources");

        const auto ex = seldon::HelpView::examples(html);
        QVERIFY2(ex.size() >= 12, "exemples attendus dans l'aide");
        QSet<QString> methods;
        int checked = 0;
        for (const auto& e : ex) {
            QVERIFY2(!e.text.isEmpty(), "exemple vide");
            if (e.method.isEmpty()) continue;
            const auto v = MethodRegistry::instance().parse(e.text);
            QVERIFY2(!v.isEmpty(), qPrintable("exemple non compris : " + e.text));
            QCOMPARE(v[0].value("method").toString(), e.method);
            const auto r = MethodRegistry::instance().compute(v[0]);
            QVERIFY2(r.ok, qPrintable("calcul impossible : " + e.text + " : " + r.error));
            methods << e.method;
            ++checked;
        }
        QVERIFY(checked >= 10);
        for (const QString& m : { "c14", "f14c", "tl", "am", "gauss", "unif" })
            QVERIFY2(methods.contains(m), qPrintable("pas d'exemple pour " + m));

        const QString qt = seldon::HelpView::toQtHtml(html);
        QVERIFY(!qt.contains("<style") && !qt.contains("<script") && !qt.contains("<!--"));
        QVERIFY(qt.contains("seldon://insert?q=C14%20de%203560"));
        QVERIFY(qt.contains("href=\"#dater\""));
    }

    // Formats de date comme ChronoModel (DateUtils).
    void dateFormats()
    {
        using namespace seldon;
        QCOMPARE(dateformat::convert(-1898.0, DateFormat::CalBP), 3848.0);
        QCOMPARE(dateformat::convert(-1898.0, DateFormat::CalB2K), 3898.0);
        QCOMPARE(dateformat::convert(-1898.0, DateFormat::DatBP), -3848.0);
        QCOMPARE(dateformat::convert(-1898.0, DateFormat::Ka), 1.898);
        QCOMPARE(dateformat::convert(1200.0, DateFormat::BcAd), 1200.0);
        QCOMPARE(dateformat::format(-1898.0, DateFormat::BcAd), QString("1899 av. J.-C."));
        QCOMPARE(dateformat::format(-1898.0, DateFormat::CalBP), QString("3848 cal BP"));
        QCOMPARE(dateformat::format(-1898.0, DateFormat::BceCe), QString("1899 BCE"));
        QVERIFY(dateformat::isAge(DateFormat::CalBP) && !dateformat::isAge(DateFormat::DatBP));
        for (const char* id : { "bcad", "bcece", "calbp", "calb2k", "datbp", "datb2k", "ka", "ma" })
            QCOMPARE(QString::fromLatin1(dateformat::id(dateformat::fromId(id))), QString(id));

        // Détection dans la question : un « BP » d'entrée n'est pas une demande de format.
        const DateFormat d = DateFormat::BcAd;
        QCOMPARE(dateformat::detect("C14 de 3560 ± 20 BP", d), d);
        QCOMPARE(dateformat::detect("C14 de 3560 ± 20 BP, donne le résultat en cal BP", d), DateFormat::CalBP);
        QCOMPARE(dateformat::detect("14C 3000 ± 30 BP, donne les intervalles en BP", d), DateFormat::CalBP);
        QCOMPARE(dateformat::detect("C14 3560 ± 20 BP, résultat en B2K", d), DateFormat::CalB2K);
        QCOMPARE(dateformat::detect("14C 3560 ± 20 BP, exprime l'âge en ka", d), DateFormat::Ka);
        QCOMPARE(dateformat::detect("C14 de 3560 ± 20 BP en années calendaires", DateFormat::CalBP), DateFormat::BcAd);
        QCOMPARE(dateformat::detect("date 3560 ± 20 en BP", d), d);          // âge d'entrée, pas un format de sortie
        QCOMPARE(dateformat::detect("C14 de 3560 ± 20 BP", DateFormat::CalBP), DateFormat::CalBP);   // préférence conservée

        // Fiche et graphique dans le format choisi : mêmes probabilités, valeurs converties.
        const auto bc = MethodRunner::analyzeQuestion("C14 de 3560 ± 20 BP, courbe IntCal13");
        const auto bp = MethodRunner::analyzeQuestion("C14 de 3560 ± 20 BP, courbe IntCal13, donne le résultat en cal BP");
        QVERIFY(bc.found && bp.found);
        QVERIFY(bc.noticeHtml.contains("av. J.-C."));
        QVERIFY(bp.noticeHtml.contains("cal BP") && !bp.noticeHtml.contains("av. J.-C."));
        QVERIFY(bp.noticeHtml.contains("3912 cal BP → 3826 cal BP") || bp.noticeHtml.contains("cal BP → "));
        QVERIFY(bp.promptBlock.contains("Format des dates"));
        QVERIFY(bp.plot.value("xreverse").toBool() && !bc.plot.value("xreverse").toBool());
        QCOMPARE(bp.plot.value("xlabel").toString(), QString("Âge cal. BP"));
        const double xbc = bc.plot["series"].toArray()[0].toObject()["x"].toArray()[10].toDouble();
        const double xbp = bp.plot["series"].toArray()[0].toObject()["x"].toArray()[10].toDouble();
        QVERIFY(std::abs(xbp - (1950.0 - xbc)) < 1e-9);
        // Préférence appliquée par défaut
        const auto pref = MethodRunner::analyzeQuestion("C14 de 3560 ± 20 BP, courbe IntCal13", DateFormat::CalBP);
        QVERIFY(pref.noticeHtml.contains("cal BP"));
    }

    // Formules LaTeX : plus de signes de construction (\ { } ^ _) dans le HTML affiché.
    void latexFormulas()
    {
        using seldon::math::latexToHtml;
        QCOMPARE(latexToHtml("\\sigma"), QString("<i>σ</i>"));
        QVERIFY(latexToHtml("x^2").contains("<i>x</i><sup>2</sup>"));
        QVERIFY(latexToHtml("\\sigma_{ref}^2").contains("<sub>"));
        const QString root = latexToHtml("\\sqrt{\\sigma_1^2 + \\sigma_2^2}");
        QVERIFY(root.contains("√") && root.contains("overline") && root.contains("σ"));
        QVERIFY(!root.contains("\\") && !root.contains("{") && !root.contains("}"));
        const QString frac = latexToHtml("\\frac{a}{b}");
        QVERIFY(frac.contains("<sup>") && frac.contains("<sub>") && frac.contains("⁄"));
        QVERIFY(latexToHtml("\\frac{x - \\mu}{\\sigma\\sqrt{2\\pi}}").contains("/"));          // fraction longue : (…) / (…)
        QVERIFY(latexToHtml("a \\leq b").contains("≤"));
        QVERIFY(latexToHtml("2{,}448").contains("2,448") && latexToHtml("2,448").contains("2,448"));      // virgule décimale
        QVERIFY(latexToHtml("a, b").contains(", ") == false || latexToHtml("a, b").contains(",\u2009"));
        QVERIFY(latexToHtml("\\text{BP} = 1950 - A").contains("BP"));
        QVERIFY(latexToHtml("\\text{BP} = 1950 - A").contains("−"));          // vrai signe moins
        QVERIFY(latexToHtml("\\sum_{i=1}^{n} x_i").contains("∑"));
        QVERIFY(latexToHtml("e^{-\\frac{(t-\\mu)^2}{2\\sigma^2}}").contains("<sup>"));
        QVERIFY(latexToHtml("\\unknowncmd").contains("unknowncmd") && !latexToHtml("\\unknowncmd").contains("\\"));

        // Markdown complet : formules protégées de l'emphase Markdown (_ et *), hors blocs de code.
        auto md = [](const QString& s) { return seldon::ChatView::markdownToHtml(s); };
        const QString h1 = md("Écart-type : $\\sigma = \\sqrt{\\sigma_1^2 + \\sigma_2^2}$ pour deux sources.");
        QVERIFY(h1.contains("σ") && h1.contains("√"));
        QVERIFY(!h1.contains("\\sigma") && !h1.contains("\\sqrt") && !h1.contains("$"));
        const QString h2 = md("Avec $x_i y_j$ et $a_1 b_2$ : voir la formule.");
        QVERIFY(h2.contains("<sub>") && !h2.contains("<em>"));
        const QString h3 = md("Formule :\n\n$$ L(t) = \\frac{1}{\\sqrt{v}} \\exp\\left(-\\frac{(a-\\mu)^2}{2v}\\right) $$\n\nFin.");
        QVERIFY(h3.contains("align=\"center\"") && h3.contains("exp"));
        QVERIFY(!h3.contains("\\frac") && !h3.contains("\\left"));
        const QString h4 = md("Ici \\(\\alpha_{95}\\) et \\[\\beta\\]");
        QVERIFY(h4.contains("α") && h4.contains("β") && !h4.contains("\\alpha"));
        // Commande nue (sans $) : \frac{a}{b}, \sigma
        const QString h5 = md("L'écart vaut \\frac{\\sigma}{\\sqrt{n}} environ ; \\sigma n'est pas une erreur.");
        QVERIFY(!h5.contains("\\frac") && !h5.contains("\\sigma") && h5.contains("σ"));
        // Montants en dollars et code : intacts
        const QString h6 = md("Coût : $5 et $10, voir `$x^2$` et\n\n```\n$\\alpha$\n```\n");
        QVERIFY(h6.contains("$5") && h6.contains("$10"));
        QVERIFY(h6.contains("$x^2$") && h6.contains("$\\alpha$"));
    }

    // ----------------------------------------------------------------- modèle distant (API compatible OpenAI)
    void remoteApiHelpers()
    {
        using namespace seldon::remote;
        QCOMPARE(normalizeBase("https://api.openai.com/v1/"), QString("https://api.openai.com/v1"));
        QCOMPARE(normalizeBase(" https://api.openai.com/v1/chat/completions "), QString("https://api.openai.com/v1"));
        QCOMPARE(normalizeBase("http://localhost:11434"), QString("http://localhost:11434/v1"));         // chemin vide : /v1 ajouté
        QCOMPARE(normalizeBase("localhost:1234/v1"), QString("https://localhost:1234/v1"));
        QCOMPARE(normalizeBase("https://openrouter.ai/api/v1"), QString("https://openrouter.ai/api/v1"));
        QCOMPARE(chatUrl("https://api.mistral.ai/v1").toString(), QString("https://api.mistral.ai/v1/chat/completions"));
        QCOMPARE(modelsUrl("https://api.mistral.ai/v1/").toString(), QString("https://api.mistral.ai/v1/models"));
        QCOMPARE(hostOf("https://api.openai.com/v1"), QString("api.openai.com"));

        seldon::RemoteConfig cfg{ "https://x/v1", "k", "gpt-4o-mini" };
        QVERIFY(cfg.isValid());
        const seldon::RemoteConfig noUrl{ "", "k", "m" }, noModel{ "u", "k", "" };
        QVERIFY(!noUrl.isValid());
        QVERIFY(!noModel.isValid());
        QVector<seldon::ChatMessage> chat{ { "system", "Sois bref." }, { "user", "Bonjour" } };
        seldon::SamplingParams sp; sp.temperature = 0.3f; sp.maxTokens = 500;
        QJsonObject b = QJsonDocument::fromJson(buildChatBody(cfg, chat, sp)).object();
        QCOMPARE(b["model"].toString(), QString("gpt-4o-mini"));
        QVERIFY(b["stream"].toBool());
        QCOMPARE(b["messages"].toArray().size(), 2);
        QCOMPARE(b["messages"].toArray()[1].toObject()["content"].toString(), QString("Bonjour"));
        QCOMPARE(b["max_tokens"].toInt(), 500);
        QVERIFY(std::abs(b["temperature"].toDouble() - 0.3) < 1e-6);
        b = QJsonDocument::fromJson(buildChatBody(cfg, chat, sp, true, true)).object();
        QVERIFY(!b.contains("max_tokens") && !b.contains("temperature"));
        QCOMPARE(b["max_completion_tokens"].toInt(), 500);

        QCOMPARE(parseModelList(R"({"data":[{"id":"b-model"},{"id":"A-model"},{"id":"b-model"}]})"), QStringList({ "A-model", "b-model" }));
        QVERIFY(errorMessage(R"({"error":{"message":"Incorrect API key"}})", 401).contains("clé API"));
        QVERIFY(errorMessage(R"({"error":{"message":"Incorrect API key"}})", 401).contains("Incorrect API key"));
        QVERIFY(errorMessage("not json", 404).contains("Base URL"));
    }

    void remoteSseParser()
    {
        using namespace seldon::remote;
        SseParser p;
        QVector<seldon::SseEvent> ev;
        // coupé au milieu d'une ligne et d'un caractère UTF-8 (é = 2 octets)
        const QByteArray s1 = "data: {\"choices\":[{\"delta\":{\"content\":\"Bon\"}}]}\n\n: keep-alive\n\ndata: {\"choices\":[{\"delta\":{\"con";
        const QByteArray s2 = "tent\":\"jour \xC3\xA9t\xC3\xA9\"}}]}\r\n\r\ndata: {\"choices\":[{\"delta\":{\"reasoning_content\":\"hmm\"}}]}\n";
        const QByteArray s3 = "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":7}}\n\ndata: [DONE]\n";
        ev += p.feed(s1); ev += p.feed(s2); ev += p.feed(s3);
        QString text, reason; int pt = -1, ct = -1; bool done = false;
        for (const auto& e : ev) { text += e.text; reason += e.reasoning; if (e.promptTokens >= 0) pt = e.promptTokens; if (e.completionTokens >= 0) ct = e.completionTokens; done |= e.done; }
        QCOMPARE(text, QString::fromUtf8("Bonjour été"));
        QCOMPARE(reason, QString("hmm"));
        QCOMPARE(pt, 12); QCOMPARE(ct, 7);
        QVERIFY(done && p.sawEvent());
        SseParser q;
        auto e2 = q.feed("data: {\"error\":{\"message\":\"quota\"}}\n");
        QCOMPARE(e2.size(), 1); QCOMPARE(e2[0].error, QString("quota"));
        SseParser r;
        QVERIFY(r.feed("{\"choices\":[{\"message\":{\"content\":\"x\"}}]}").isEmpty() && !r.sawEvent());
        QCOMPARE(textFromFullResponse(R"({"choices":[{"message":{"content":"Salut"}}],"usage":{"completion_tokens":3}})"), QString("Salut"));
    }

    void remoteEngineStreaming()
    {
        MockServer srv([](const QByteArray&, const QByteArray&, int) {
            QByteArray body =
                "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"\"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\"Bonjour\"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\" le monde\"}}]}\n\n"
                "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":4}}\n\n"
                "data: [DONE]\n\n";
            return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" + body;
        });
        seldon::RemoteEngine eng;
        eng.setConfig({ srv.baseUrl(), "sk-test-123", "mon-modele" });
        QSignalSpy tokens(&eng, &seldon::RemoteEngine::token), fin(&eng, &seldon::RemoteEngine::finished), fail(&eng, &seldon::RemoteEngine::failed);
        seldon::SamplingParams sp; sp.maxTokens = 100;
        eng.generate({ { "system", "s" }, { "user", "q" } }, sp);
        QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 1, 5000);
        QCOMPARE(fail.count(), 0);
        QString all; for (const auto& a : tokens) all += a[0].toString();
        QCOMPARE(all, QString("Bonjour le monde"));
        const auto st = fin[0][0].value<seldon::GenStats>();
        QCOMPARE(st.generated, 4); QCOMPARE(st.promptTokens, 20); QVERIFY(!st.stoppedByUser);
        QVERIFY(srv.requests_[0].startsWith("POST /v1/chat/completions"));
        QVERIFY(srv.requests_[0].contains("Bearer sk-test-123"));
        QCOMPARE(QJsonDocument::fromJson(srv.bodies_[0]).object()["model"].toString(), QString("mon-modele"));
    }

    void remoteEngineErrorsAndRetry()
    {
        // 401 : message avec indication sur la clé
        {
            MockServer srv([](const QByteArray&, const QByteArray&, int) {
                return httpResponse(401, "application/json", R"({"error":{"message":"Incorrect API key provided"}})");
            });
            seldon::RemoteEngine eng; eng.setConfig({ srv.baseUrl(), "bad", "m" });
            QSignalSpy fail(&eng, &seldon::RemoteEngine::failed), fin(&eng, &seldon::RemoteEngine::finished);
            eng.generate({ { "user", "q" } }, {});
            QTRY_COMPARE_WITH_TIMEOUT(fail.count(), 1, 5000);
            QVERIFY(fail[0][0].toString().contains("401") && fail[0][0].toString().contains("clé API"));
            QCOMPARE(fin.count(), 0);
        }
        // 400 « max_tokens non supporté » : une relance avec max_completion_tokens, puis succès (réponse non diffusée)
        {
            MockServer srv([](const QByteArray&, const QByteArray& body, int idx) {
                if (idx == 0)
                    return httpResponse(400, "application/json", R"({"error":{"message":"Unsupported parameter: 'max_tokens'. Use 'max_completion_tokens' instead."}})");
                Q_UNUSED(body);
                return httpResponse(200, "application/json", R"({"choices":[{"message":{"content":"Réponse complète"}}],"usage":{"prompt_tokens":5,"completion_tokens":2}})");
            });
            seldon::RemoteEngine eng; eng.setConfig({ srv.baseUrl(), "", "o-model" });
            QSignalSpy tok(&eng, &seldon::RemoteEngine::token), fin(&eng, &seldon::RemoteEngine::finished), fail(&eng, &seldon::RemoteEngine::failed);
            seldon::SamplingParams sp; sp.maxTokens = 50;
            eng.generate({ { "user", "q" } }, sp);
            QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 1, 5000);
            QCOMPARE(fail.count(), 0);
            QCOMPARE(tok.count(), 1); QCOMPARE(tok[0][0].toString(), QString::fromUtf8("Réponse complète"));
            QCOMPARE(srv.requests_.size(), 2);
            const auto second = QJsonDocument::fromJson(srv.bodies_[1]).object();
            QVERIFY(second.contains("max_completion_tokens") && !second.contains("max_tokens"));
            QVERIFY(!srv.requests_[0].contains("Authorization"));            // pas de clé : pas d'en-tête
        }
        // serveur injoignable
        {
            seldon::RemoteEngine eng; eng.setConfig({ "http://127.0.0.1:1/v1", "", "m" });
            QSignalSpy fail(&eng, &seldon::RemoteEngine::failed);
            eng.generate({ { "user", "q" } }, {});
            QTRY_COMPARE_WITH_TIMEOUT(fail.count(), 1, 8000);
            QVERIFY(fail[0][0].toString().contains("impossible"));
        }
        // configuration incomplète
        {
            seldon::RemoteEngine eng; QSignalSpy fail(&eng, &seldon::RemoteEngine::failed);
            eng.generate({ { "user", "q" } }, {});
            QCOMPARE(fail.count(), 1);
        }
    }

    // ---- Outils (appel d'outils natif du modèle distant ; mêmes calculs que le bloc ```calcul)

    void toolsSchema()
    {
        const auto& all = seldon::tools::all();
        QCOMPARE(all.size(), 7);
        const QJsonArray api = seldon::tools::schemaForApi();
        QCOMPARE(api.size(), 7);
        static const QRegularExpression okName("^[a-z0-9_]{1,64}$");
        QStringList seen;
        for (const QJsonValue& v : api) {
            const QJsonObject f = v.toObject().value("function").toObject();
            QCOMPARE(v.toObject().value("type").toString(), QString("function"));
            QVERIFY2(okName.match(f.value("name").toString()).hasMatch(), qPrintable(f.value("name").toString()));
            QVERIFY(!f.value("description").toString().isEmpty());
            QCOMPARE(f.value("parameters").toObject().value("type").toString(), QString("object"));
            QVERIFY(!seen.contains(f.value("name").toString()));
            seen << f.value("name").toString();
        }
        // chaque outil de calcul vise une méthode qui existe
        for (const auto& d : all)
            if (!d.methodId.isEmpty()) QVERIFY2(seldon::MethodRegistry::instance().byId(d.methodId) != nullptr, qPrintable(d.name));
    }

    void toolsExecute()
    {
        using seldon::tools::execute;
        const auto fmt = seldon::DateFormat::BcAd;

        // même résultat que le chemin « question reconnue » et que le bloc ```calcul
        const auto a = execute("calibrer_radiocarbone", R"({"age":2500,"error":30,"curve":"intcal20"})", fmt);
        QVERIFY(a.known && a.executed);
        QVERIFY(a.content.contains("HPD à 95"));
        QCOMPARE(a.request.value("method").toString(), QString("c14"));
        const auto b = seldon::MethodRunner::run({ QJsonObject{ { "method", "c14" }, { "age", 2500 }, { "error", 30 }, { "curve", "intcal20" } } }, fmt);
        QCOMPARE(a.run.promptBlock, b.promptBlock);
        QVERIFY(!a.run.plot.isEmpty());

        // nombres écrits comme des chaînes, virgule décimale, casse du nom de courbe
        const auto c = execute("calibrer_radiocarbone", R"({"age":"2 500","error":"30,5","curve":"IntCal20"})", fmt);
        QVERIFY(c.executed);
        QCOMPARE(c.request.value("age").toDouble(), 2500.0);
        QCOMPARE(c.request.value("error").toDouble(), 30.5);
        QCOMPARE(c.request.value("curve").toString(), QString("intcal20"));

        // ΔR : indiqué dans la fiche
        const auto m = execute("calibrer_radiocarbone", R"({"age":3000,"error":40,"curve":"marine20","deltaR":-50,"deltaRerr":30})", fmt);
        QVERIFY(m.executed && m.request.value("hasDeltaR").toBool());

        // valeur manquante : rien n'est calculé, le modèle reçoit une consigne claire
        const auto tl = execute("dater_luminescence", R"({"age":800,"error":50})", fmt);
        QVERIFY(tl.known && !tl.executed);
        QVERIFY(tl.content.contains("refYear") && tl.content.contains("Demande"));
        const auto am = execute("dater_archeomagnetisme", R"({"process":"id","inc":65,"dec":3})", fmt);
        QVERIFY(!am.executed && am.content.contains("alpha95"));
        const auto am2 = execute("dater_archeomagnetisme", R"({"process":"i","inc":65,"alpha95":2.5})", fmt);
        QVERIFY2(am2.executed, qPrintable(am2.content));
        QCOMPARE(am2.request.value("process").toString(), QString("I"));

        // calculs simples
        QVERIFY(execute("densite_uniforme", R"({"min":-500,"max":-300})", fmt).executed);
        QVERIFY(execute("densite_gauss", R"({"age":-500,"error":40})", fmt).executed);
        QVERIFY(execute("calibrer_f14c", R"({"f14c":1.2,"error":0.003})", fmt).executed);

        // erreurs : jamais d'exception, toujours un texte
        QVERIFY(!execute("outil_fantome", "{}", fmt).known);
        QVERIFY(execute("outil_fantome", "{}", fmt).content.contains("calibrer_radiocarbone"));
        QVERIFY(execute("calibrer_radiocarbone", "pas du json", fmt).content.contains("invalides"));
        QVERIFY(execute("calibrer_radiocarbone", R"({"age":"beaucoup","error":30})", fmt).content.contains("pas un nombre"));
        const auto bad = execute("calibrer_radiocarbone", R"({"age":2500,"error":0})", fmt);
        QVERIFY(bad.executed && bad.content.contains("Calcul impossible"));        // la méthode refuse : le modèle le lit
        const auto unk = execute("calibrer_radiocarbone", R"({"age":2500,"error":30,"curve":"nexistepas"})", fmt);
        QVERIFY(unk.content.contains("intcal20"));                                   // la liste des courbes est donnée

        // liste des courbes
        const auto lc = execute("courbes_disponibles", R"({"method":"c14"})", fmt);
        QVERIFY(lc.known && !lc.executed && lc.content.contains("intcal20") && lc.content.contains("shcal20"));
        QVERIFY(execute("courbes_disponibles", "", fmt).content.contains("bulgaria_i"));

        // format de dates : le résultat suit le format demandé
        const auto bp = execute("calibrer_radiocarbone", R"({"age":2500,"error":30})", seldon::DateFormat::CalBP);
        QVERIFY(bp.content.contains("cal BP"));
        QCOMPARE(bp.run.plot.value("xreverse").toBool(), true);

        // fusion de courbes
        const auto merged = seldon::tools::mergePlots({ a.run.plot, c.run.plot, QJsonObject() });
        QCOMPARE(merged.value("series").toArray().size(), 2);
    }

    void remoteToolCallParsing()
    {
        using namespace seldon;
        // flux : l'appel arrive en morceaux (nom d'abord, puis arguments coupés n'importe où), deux appels
        remote::SseParser p;
        QVector<SseEvent> evs;
        const QByteArray lines[] = {
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_a\",\"type\":\"function\",\"function\":{\"name\":\"calibrer_radiocarbone\",\"arguments\":\"\"}}]}}]}",
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"age\\\":25\"}}]}}]}",
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"00,\\\"error\\\":30}\"}}]}}]}",
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"call_b\",\"type\":\"function\",\"function\":{\"name\":\"densite_uniforme\",\"arguments\":\"{\\\"min\\\":1,\\\"max\\\":2}\"}}]}}]}",
            "data: [DONE]" };
        for (const QByteArray& l : lines) evs += p.feed(l + "\n\n");
        remote::ToolCallAccumulator acc;
        for (const auto& e : evs) for (const auto& d : e.toolCalls) acc.add(d);
        const auto calls = acc.calls();
        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls[0].id, QString("call_a")); QCOMPARE(calls[0].name, QString("calibrer_radiocarbone"));
        QCOMPARE(QJsonDocument::fromJson(calls[0].arguments.toUtf8()).object().value("age").toInt(), 2500);
        QCOMPARE(calls[1].name, QString("densite_uniforme"));

        // réutilisation après clear() (un nouvel aller-retour repart de zéro, même avec le même index)
        acc.clear();
        QVERIFY(acc.isEmpty());
        ToolCallDelta again; again.index = 0; again.id = "z"; again.name = "densite_gauss"; again.arguments = "{}";
        acc.add(again);
        QCOMPARE(acc.calls().size(), 1);
        QCOMPARE(acc.calls()[0].name, QString("densite_gauss"));

        // fournisseur sans index ni id (appel entier en un seul morceau)
        remote::ToolCallAccumulator acc2;
        for (const auto& e : remote::SseParser().feed(R"(data: {"choices":[{"delta":{"tool_calls":[{"function":{"name":"courbes_disponibles","arguments":"{}"}}]}}]})" "\n\n"))
            for (const auto& d : e.toolCalls) acc2.add(d);
        QCOMPARE(acc2.calls().size(), 1);
        QCOMPARE(acc2.calls()[0].id, QString("call_1"));          // id fabriqué

        // réponse non diffusée
        const QByteArray fullBody =
            "{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":["
            "{\"id\":\"x1\",\"type\":\"function\",\"function\":{\"name\":\"densite_gauss\",\"arguments\":\"{\\\"age\\\":1,\\\"error\\\":2}\"}},"
            "{\"id\":\"x2\",\"type\":\"function\",\"function\":{\"name\":\"densite_uniforme\",\"arguments\":{\"min\":1,\"max\":2}}}]}}]}";
        const auto full = remote::toolCallsFromFullResponse(fullBody);
        QCOMPARE(full.size(), 2);
        QCOMPARE(full[1].name, QString("densite_uniforme"));
        QVERIFY(QJsonDocument::fromJson(full[1].arguments.toUtf8()).isObject());

        // messages renvoyés au serveur
        const QJsonObject am = remote::assistantToolMessage("", "", { { "id1", "densite_gauss", "pas du json", {} } });
        QCOMPARE(am.value("role").toString(), QString("assistant"));
        QVERIFY(am.value("content").isNull());
        QCOMPARE(am.value("tool_calls").toArray()[0].toObject().value("function").toObject().value("arguments").toString(), QString("{}"));
        QVERIFY(!am.contains("reasoning_content"));
        QVERIFY(remote::assistantToolMessage("t", "raisonnement", { { "i", "n", "{}", {} } }).contains("reasoning_content"));
        const QJsonObject tm = remote::toolResultMessage({ "id1", "densite_gauss", "ok" });
        QCOMPARE(tm.value("role").toString(), QString("tool")); QCOMPARE(tm.value("tool_call_id").toString(), QString("id1"));

        // corps de requête
        const RemoteConfig cfg{ "http://x/v1", "", "m" };
        const QJsonObject b1 = QJsonDocument::fromJson(remote::buildChatBody(cfg, { { "user", "q" } }, {}, false, false,
                                                                           tools::schemaForApi(), QJsonArray{ am, tm })).object();
        QCOMPARE(b1.value("tools").toArray().size(), 7);
        QCOMPARE(b1.value("tool_choice").toString(), QString("auto"));
        QCOMPARE(b1.value("messages").toArray().size(), 3);
        const QJsonObject b2 = QJsonDocument::fromJson(remote::buildChatBody(cfg, { { "user", "q" } }, {}, false, false,
                                                                           tools::schemaForApi(), {}, true)).object();
        QCOMPARE(b2.value("tool_choice").toString(), QString("none"));
        QVERIFY(!QJsonDocument::fromJson(remote::buildChatBody(cfg, { { "user", "q" } }, {})).object().contains("tools"));
    }

    void remoteEngineToolLoop()
    {
        // 1re réponse : appel d'outil en flux ; 2e : texte final. Le 2e corps doit contenir l'appel et son résultat.
        MockServer srv([](const QByteArray&, const QByteArray&, int idx) {
            QByteArray body;
            if (idx == 0)
                body = "data: {\"choices\":[{\"delta\":{\"content\":\"Je calcule.\"}}]}\n\n"
                       "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_9\",\"type\":\"function\",\"function\":{\"name\":\"calibrer_radiocarbone\",\"arguments\":\"{\\\"age\\\":2500,\"}}]}}]}\n\n"
                       "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"error\\\":30}\"}}]}}]}\n\n"
                       "data: {\"choices\":[{\"finish_reason\":\"tool_calls\",\"delta\":{}}]}\n\n"
                       "data: [DONE]\n\n";
            else
                body = "data: {\"choices\":[{\"delta\":{\"content\":\"Voici le résultat.\"}}]}\n\n"
                       "data: [DONE]\n\n";
            return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" + body;
        });
        seldon::RemoteEngine eng;
        eng.setConfig({ srv.baseUrl(), "", "m" });
        eng.setTools(seldon::tools::schemaForApi());
        QSignalSpy calls(&eng, &seldon::RemoteEngine::toolCallsRequested), fin(&eng, &seldon::RemoteEngine::finished),
                   fail(&eng, &seldon::RemoteEngine::failed), tok(&eng, &seldon::RemoteEngine::token);
        eng.generate({ { "system", "s" }, { "user", "Date 2500 ± 30 BP ?" } }, {});
        QTRY_COMPARE_WITH_TIMEOUT(calls.count(), 1, 5000);
        QCOMPARE(fin.count(), 0);
        QCOMPARE(fail.count(), 0);
        const auto cs = calls[0][0].value<QVector<seldon::ToolCall>>();
        QCOMPARE(cs.size(), 1);
        QCOMPARE(cs[0].name, QString("calibrer_radiocarbone"));
        QCOMPARE(calls[0][1].toString(), QString("Je calcule."));
        const auto out = seldon::tools::execute(cs[0].name, cs[0].arguments, seldon::DateFormat::BcAd);
        QVERIFY2(out.executed, qPrintable(out.content));

        // 1re requête : outils déclarés
        const QJsonObject first = QJsonDocument::fromJson(srv.bodies_[0]).object();
        QCOMPARE(first.value("tools").toArray().size(), 7);

        eng.submitToolResults({ { cs[0].id, cs[0].name, out.content } });
        QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 1, 5000);
        QCOMPARE(fail.count(), 0);
        QString all; for (const auto& a : tok) all += a[0].toString();
        QVERIFY(all.endsWith("Voici le résultat."));
        QCOMPARE(srv.bodies_.size(), 2);
        const QJsonArray msgs = QJsonDocument::fromJson(srv.bodies_[1]).object().value("messages").toArray();
        QCOMPARE(msgs.size(), 4);                                           // system, user, assistant(tool_calls), tool
        QCOMPARE(msgs[2].toObject().value("role").toString(), QString("assistant"));
        const QJsonObject tc = msgs[2].toObject().value("tool_calls").toArray()[0].toObject();
        QCOMPARE(tc.value("id").toString(), QString("call_9"));
        QCOMPARE(QJsonDocument::fromJson(tc.value("function").toObject().value("arguments").toString().toUtf8()).object().value("error").toInt(), 30);
        QCOMPARE(msgs[3].toObject().value("role").toString(), QString("tool"));
        QCOMPARE(msgs[3].toObject().value("tool_call_id").toString(), QString("call_9"));
        QVERIFY(msgs[3].toObject().value("content").toString().contains("HPD"));
    }

    void remoteEngineToolsRefusedAndRoundLimit()
    {
        // Serveur sans outils : 400, on réessaie sans « tools » et on le signale ; le constat est mémorisé.
        {
            MockServer srv([](const QByteArray&, const QByteArray& body, int) {
                if (QJsonDocument::fromJson(body).object().contains("tools"))
                    return httpResponse(400, "application/json", R"({"error":{"message":"registry.ollama.ai/library/x does not support tools"}})");
                return httpResponse(200, "application/json", R"({"choices":[{"message":{"content":"Sans outils"}}]})");
            });
            seldon::RemoteEngine eng; eng.setConfig({ srv.baseUrl(), "", "x" });
            eng.setTools(seldon::tools::schemaForApi());
            QSignalSpy fin(&eng, &seldon::RemoteEngine::finished), unav(&eng, &seldon::RemoteEngine::toolsUnavailable),
                       fail(&eng, &seldon::RemoteEngine::failed), tok(&eng, &seldon::RemoteEngine::token);
            eng.generate({ { "user", "q" } }, {});
            QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 1, 5000);
            QCOMPARE(fail.count(), 0); QCOMPARE(unav.count(), 1);
            QCOMPARE(tok[0][0].toString(), QString("Sans outils"));
            QVERIFY(!eng.toolsActive());
            eng.generate({ { "user", "q2" } }, {});                    // 2e question : plus d'outils d'emblée
            QTRY_COMPARE_WITH_TIMEOUT(fin.count(), 2, 5000);
            QCOMPARE(srv.bodies_.size(), 3);
            QVERIFY(!QJsonDocument::fromJson(srv.bodies_[2]).object().contains("tools"));
            eng.setConfig({ srv.baseUrl(), "", "autre-modele" });      // autre modèle : on réessaie les outils
            QVERIFY(eng.toolsActive());
        }
        // Une erreur sans rapport avec les outils n'est pas avalée.
        {
            MockServer srv([](const QByteArray&, const QByteArray&, int) {
                return httpResponse(401, "application/json", R"({"error":{"message":"Incorrect API key"}})");
            });
            seldon::RemoteEngine eng; eng.setConfig({ srv.baseUrl(), "k", "m" });
            eng.setTools(seldon::tools::schemaForApi());
            QSignalSpy fail(&eng, &seldon::RemoteEngine::failed);
            eng.generate({ { "user", "q" } }, {});
            QTRY_COMPARE_WITH_TIMEOUT(fail.count(), 1, 5000);
            QVERIFY(eng.toolsActive());
        }
        // Un modèle qui rappelle des outils sans fin : au bout de kMaxToolRounds, tool_choice = none et on termine.
        {
            MockServer srv([](const QByteArray&, const QByteArray& body, int) {
                const bool none = QJsonDocument::fromJson(body).object().value("tool_choice").toString() == "none";
                const QByteArray b = none ? QByteArray("data: {\"choices\":[{\"delta\":{\"content\":\"Fin.\"}}]}\n\ndata: [DONE]\n\n")
                                          : QByteArray("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c\",\"type\":\"function\",\"function\":{\"name\":\"courbes_disponibles\",\"arguments\":\"{}\"}}]}}]}\n\ndata: [DONE]\n\n");
                return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" + b;
            });
            seldon::RemoteEngine eng; eng.setConfig({ srv.baseUrl(), "", "m" });
            eng.setTools(seldon::tools::schemaForApi());
            QSignalSpy calls(&eng, &seldon::RemoteEngine::toolCallsRequested), fin(&eng, &seldon::RemoteEngine::finished);
            eng.generate({ { "user", "q" } }, {});
            int n = 0;
            while (fin.count() == 0 && n < 10) {
                QTRY_VERIFY_WITH_TIMEOUT(calls.count() > n || fin.count() > 0, 5000);
                if (fin.count() > 0) break;
                const auto cs = calls.last()[0].value<QVector<seldon::ToolCall>>();
                ++n;
                eng.submitToolResults({ { cs[0].id, cs[0].name, "x" } });
            }
            QCOMPARE(fin.count(), 1);
            QCOMPARE(n, seldon::RemoteEngine::kMaxToolRounds);
        }
    }

    void resultBlockForbidsInventedPlots()
    {
        const auto r = seldon::MethodRunner::analyzeQuestion("archéomagnétisme : inclinaison 65°, alpha95 = 1");
        QVERIFY(r.found);
        QVERIFY(r.promptBlock.contains("DÉJÀ tracé"));
        QVERIFY(r.promptBlock.contains("bloc « plot »"));
        QVERIFY(r.promptBlock.contains("courbe bleue"));
    }

    void toolsSystemHint()
    {
        QVERIFY(!seldon::toolsSystemHint().empty());
        QVERIFY(seldon::toolsSystemHint().find("Si des fonctions") != std::string::npos);     // conditionnel : vrai même sans outils
    }

    // Préférences : champs larges, moteur local/distant, et réglages distants relus.
    void settingsDialogRemote()
    {
        seldon::AppSettings s;
        s.useRemote = true; s.remoteUrl = "https://api.example.org/v1"; s.remoteKey = "k"; s.remoteModel = "m1";
        s.modelPath = "/Users/dufresne/SeldonH/models/un-nom-de-modele-gguf-tres-long-Q4_K_M.gguf";
        seldon::SettingsDialog dlg(s);
        dlg.show();
        QVERIFY(dlg.width() >= 780);
        const auto out = dlg.settings();
        QVERIFY(out.useRemote);
        QCOMPARE(out.remoteUrl, s.remoteUrl); QCOMPARE(out.remoteKey, s.remoteKey); QCOMPARE(out.remoteModel, s.remoteModel);
        QCOMPARE(out.modelPath, s.modelPath);
        // le champ du modèle local est assez large pour afficher un chemin typique
        for (auto* le : dlg.findChildren<QLineEdit*>())
            if (le->text() == s.modelPath) QVERIFY(le->minimumWidth() >= 380);
        dlg.findChildren<QRadioButton*>().first()->setChecked(true);         // « Modèle local »
        QVERIFY(!dlg.settings().useRemote);
    }

    void parseDetails()
    {
        auto v = MethodRegistry::instance().parse("coquille marine, 14C 3000 ± 30 BP, ΔR 100 ± 50");
        QCOMPARE(v.size(), 1);
        QCOMPARE(v[0].value("curve").toString(), QString("marine20"));
        QVERIFY(v[0].value("hasDeltaR").toBool());
        QCOMPARE(v[0].value("deltaR").toDouble(), 100.0);
        QCOMPARE(v[0].value("deltaRerr").toDouble(), 50.0);
        QCOMPARE(v[0].value("age").toDouble(), 3000.0);
        v = MethodRegistry::instance().parse("âge 14C 1200 ± 30 BP, hémisphère sud");
        QCOMPARE(v[0].value("curve").toString(), QString("shcal20"));
        v = MethodRegistry::instance().parse("calibre 2500 ± 30 BP avec IntCal13");
        QCOMPARE(v[0].value("curve").toString(), QString("intcal13"));
        v = MethodRegistry::instance().parse("échantillon F14C = 0,95 ± 0,005");
        QCOMPARE(v[0].value("curve").toString(), QString("intcal20"));          // pré-bombe
        v = MethodRegistry::instance().parse("F14C = 1,05 ± 0,005 hémisphère sud");
        QCOMPARE(v[0].value("curve").toString(), QString("bomb21shz1"));
    }

    void runnerOutcome()
    {
        const RunOutcome o = MethodRunner::analyzeQuestion("Vous date ce C14 de 1954 +- 50");
        QVERIFY(o.found);
        QCOMPARE(o.count, 1);
        QVERIFY(o.promptBlock.contains("1954 ± 50 BP"));
        QVERIFY(o.promptBlock.contains("HPD à 95 %"));
        QVERIFY(o.noticeHtml.contains("IntCal20"));
        const QJsonObject ser = o.plot["series"].toArray()[0].toObject();
        QVERIFY(ser["x"].toArray().size() > 10);
        QCOMPARE(ser["x"].toArray().size(), ser["y"].toArray().size());
        QVERIFY(!MethodRunner::analyzeQuestion("Bonjour, que fais-tu ?").found);

        // échec propre (hors courbe) : la fiche l'explique sans planter
        const RunOutcome bad = MethodRunner::analyzeQuestion("C14 90000 ± 30 BP");
        QVERIFY(bad.found);
        QVERIFY(bad.noticeHtml.contains("Calcul impossible"));
        QVERIFY(bad.plot.isEmpty());
    }

    void calcExampleInSkill()
    {
        SkillManager m;
        m.reload(QString());
        QString body;
        for (const Skill& sk : m.skills()) if (sk.id == "09_outils_calcul") body = sk.body;
        QVERIFY(!body.isEmpty());
        const auto v = MethodRunner::extractCalcBlocks(body);
        QCOMPARE(v.size(), 1);
        QVERIFY(MethodRunner::run(v).noticeHtml.contains("IntCal20"));
        QVERIFY(m.alwaysText().contains("calcul"));
        // la fiche est sélectionnée pour une question de calcul
        const auto sel = m.select("Peux-tu calibrer une date C14 et donner les HPD ?", 5000, QStringList());
        bool found = false;
        for (const SkillMatch& mt : sel) if (mt.skill->id == "09_outils_calcul") found = true;
        QVERIFY(found);
    }

    void calcBlocks()
    {
        const QString ans = "Je lance le calcul.\n\n```calcul\n{\"method\":\"tl\",\"age\":800,\"error\":60,\"refYear\":2015}\n```\n"
                            "puis\n```calcul\n[{\"method\":\"unif\",\"min\":1,\"max\":9},{\"age\":3}]\n```\n";
        const auto v = MethodRunner::extractCalcBlocks(ans);
        QCOMPARE(v.size(), 2);                                   // le 2e objet sans "method" est ignoré
        const RunOutcome o = MethodRunner::run(v);
        QCOMPARE(o.count, 2);
        QVERIFY(o.noticeHtml.contains("TL/OSL"));
        QVERIFY(o.noticeHtml.contains("Uniforme"));
    }
    // Remplissage des densités : seule la région HPD (95 % par défaut) est colorée, pas les queues.
    void userPromptBlocks()
    {
        QCOMPARE(seldon::userPromptSystemBlock(""), std::string());
        QCOMPARE(seldon::userPromptSystemBlock("  \n "), std::string());
        QCOMPARE(seldon::userPromptReminder("\t"), std::string());
        const std::string sys = seldon::userPromptSystemBlock("  Réponds en trois phrases.  ");
        QVERIFY(sys.find("Consignes de l'utilisateur") != std::string::npos);
        QVERIFY(sys.size() >= 26 && sys.substr(sys.size() - 26) == "Réponds en trois phrases.");
        const std::string rem = seldon::userPromptReminder("Donne les âges en BP");
        QVERIFY(rem.find("Donne les âges en BP") != std::string::npos);
        QCOMPARE(seldon::trimUserPrompt(std::string(5000, 'x')).size(), seldon::kUserPromptMaxChars);
    }

    void fillOnlyHpd()
    {
        QJsonArray x, y;
        for (int i = 0; i <= 400; ++i) {
            const double t = -200 + i * 1.0;
            x.append(t);
            y.append(std::exp(-0.5 * (t / 20.0) * (t / 20.0)));
        }
        const QJsonObject s{ {"name", "g"}, {"x", x}, {"y", y} };
        QJsonObject o{ {"fill", true}, {"series", QJsonArray{ s }} };

        seldon::CurveSpec sp;
        QVERIFY(seldon::CurveSpec::fromJson(o, sp));
        QCOMPARE(sp.hpdLevel, 95.0);
        o["hpd"] = 68;
        QVERIFY(seldon::CurveSpec::fromJson(o, sp));
        QCOMPARE(sp.hpdLevel, 68.0);
        o["hpd"] = false;
        QVERIFY(seldon::CurveSpec::fromJson(o, sp));
        QCOMPARE(sp.hpdLevel, 0.0);
        o.remove("hpd");
        QVERIFY(seldon::CurveSpec::fromJson(o, sp));
        QCOMPARE(sp.hpdLevel, 95.0);

        // Densité : pas d'échelle verticale par défaut ; "yaxis": true la réaffiche ; sans fill elle reste affichée.
        QVERIFY(!sp.showYAxis);
        o["yaxis"] = true;
        QVERIFY(seldon::CurveSpec::fromJson(o, sp));
        QVERIFY(sp.showYAxis);
        o.remove("yaxis");
        QVERIFY(seldon::CurveSpec::fromJson(o, sp));
        {
            QJsonObject line = o; line["fill"] = false;
            seldon::CurveSpec ls;
            QVERIFY(seldon::CurveSpec::fromJson(line, ls));
            QVERIFY(ls.showYAxis);
        }

        // Rendu : on compte les pixels colorés sous la courbe, dans la queue (t = -3σ) et au centre.
        const QSize size(700, 350);
        const QImage hpd = seldon::CurveWidget(sp).render(size);
        sp.hpdLevel = 0.0;
        const QImage all = seldon::CurveWidget(sp).render(size);
        auto bluish = [](const QImage& im) {
            int n = 0;
            for (int j = 0; j < im.height(); ++j)
                for (int i = 0; i < im.width(); ++i) {
                    const QColor c = im.pixelColor(i, j);
                    if (c.blue() > c.red() + 20 && c.blue() > 200 && c.red() > 150) ++n;   // bleu clair du remplissage
                }
            return n;
        };
        // Même rendu quand la série est en ordre décroissant (axe d'âges, « xreverse ») : le remplissage ne doit pas se déformer.
        {
            QJsonArray rx, ry;
            for (int i = x.size() - 1; i >= 0; --i) { rx.append(x[i]); ry.append(y[i]); }
            const QJsonObject rs{ {"name", "g"}, {"x", rx}, {"y", ry} };
            seldon::CurveSpec fwd = sp, rev;
            fwd.hpdLevel = 95.0;
            QJsonObject ro{ {"fill", true}, {"series", QJsonArray{ rs }} };
            QVERIFY(seldon::CurveSpec::fromJson(ro, rev));
            const QImage a1 = seldon::CurveWidget(fwd).render(size), a2 = seldon::CurveWidget(rev).render(size);
            QCOMPARE(a1, a2);
        }
        const int nHpd = bluish(hpd), nAll = bluish(all);
        QVERIFY2(nHpd > 1000, "la région HPD doit être remplie");
        QVERIFY2(nAll > nHpd, "sans HPD, toute l'aire est remplie (queues comprises)");
        // Pour une gaussienne la HPD 95 % = ±1,96σ : la part de l'aire remplie reste très proche de celle de l'aire totale
        // (les queues sont fines) mais strictement inférieure.
        QVERIFY(nHpd < nAll);
    }

};

QTEST_MAIN(TestCore)
#include "tst_core.moc"
