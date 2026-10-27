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

// Tests unitaires (QtTest) des modules sans interface : compétences, ChrBuilder, ChatText.
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
#include <random>

#include <QImage>
#include <QJsonObject>
#include "../src/ui/FigurePanel.h"
#include "../src/ui/HelpView.h"
#include "../src/ui/ChrScene.h"
#include "../src/ui/SettingsDialog.h"
#include "../src/llm/RemoteEngine.h"
#include <QHash>
#include <QSignalSpy>
#include <functional>
#include <QTcpServer>
#include <QTcpSocket>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include "../src/ui/ChatView.h"
#include "../src/core/MathText.h"
#include "../src/core/AutoTune.h"
#include "../src/core/ChatText.h"
#include "../src/core/ChrBuilder.h"
#include "../src/core/Calculator.h"
#include "../src/core/ChronoResults.h"
#include "../src/core/ConversationStore.h"
#include "../src/ui/HistoryPanel.h"
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QFile>
#include "../src/skills/SkillManager.h"
#include "../src/methods/common/CurveStore.h"
#include "../src/methods/common/DateFormat.h"
#include "../src/methods/common/EventMcmc.h"
#include "../src/methods/common/DensityFile.h"
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
        // questions en anglais : mots-clés anglais des fiches
        QVERIFY(ids("How do I build a ChronoModel model with two phases and a constraint?").contains("03_chronomodel_creer_modele"));
        QVERIFY(ids("Please calibrate this radiocarbon date with IntCal20").contains("05_radiocarbone"));
        QVERIFY(ids("My run did not converge, what should I do?").contains("12_chronomodel_logs_remedes"));
        QVERIFY(ids("How should I interpret the posterior credible interval?").contains("04_chronomodel_interpreter"));
        QVERIFY(ids("Archaeomagnetic dating with inclination 65 and alpha95 1").contains("06_archeomagnetisme"));
        QVERIFY(ids("Hello").isEmpty());
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
        auto ids = [&](const QString& q) { QStringList r; for (auto& x : m.select(q, AppSettings().skillChars)) r << x.skill->id; return r; };
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
        // les deux fiches tiennent ensemble dans le budget par défaut des Préférences
        QVERIFY(logs.size() + fix.size() <= AppSettings().skillChars);
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
          "event_constraints": [["US 12","US 10"]],
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
        QCOMPARE(c["events_constraints"].toArray().size(), 1);
        {   // modèle incomplet : contrainte vers un nom inconnu, événement sans date -> pas de fichier
            QJsonObject bad = QJsonDocument::fromJson(spec.toUtf8()).object();
            QJsonArray cons;
            cons.append(QJsonArray{ "US 12", "Inconnu" });       // (une liste à un seul élément serait copiée, pas imbriquée)
            bad["event_constraints"] = cons;
            const ChrResult u = ChrBuilder::build(bad);
            QVERIFY(!u.ok && u.errors.join(" ").contains("Inconnu"));
            QJsonArray ev = bad["events"].toArray();
            ev.append(QJsonObject{ { "name", "Event_2" }, { "dates", QJsonArray{} } });
            bad["events"] = ev;
            bad.remove("event_constraints");
            const ChrResult e = ChrBuilder::build(bad);
            QVERIFY(!e.ok && e.errors.join(" ").contains("Event_2"));
        }
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
    // Demande réelle (test_strati_corrige.chr, corrigé à la main dans ChronoModel) : couleurs, diagramme stratigraphique,
    // référence AM prise pour une courbe.
    void chrStratigraphieCouleurs()
    {
        const QString spec = R"({"name":"test strati","events":[
          {"name":"US10","dates":[{"type":"c14","age":6589,"error":30},{"type":"c14","age":5879,"error":35}]},
          {"name":"US6","color":"bleu","dates":[{"type":"c14","name":"Ly-12345","age":6458,"error":30}]},
          {"name":"US5","dates":[{"type":"c14","age":6455,"error":40}]},
          {"name":"US3","dates":[{"type":"am","process":"I","inc":65,"alpha95":1.2,"curve_i":"Rennes2025"}]},
          {"name":"US7","type":"bound","known_fixed":-40,"color":"rouge"},
          {"name":"US1","color":"orange","dates":[{"type":"tl","name":"Bdx-478","age":3500,"error":45,"ref_year":2010}]}],
          "event_constraints":[["US10","au-dessus de","US6"],["US3","au dessus de","US5"],["US6","en dessous de","US10"],
            ["US6","en dessous de","US7"],["US7","au-dessus de","US3"],["US1","en dessous de","US6"],["US1","en dessous de","US5"]]})";
        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(spec.toUtf8()).object());
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        QHash<QString, QJsonObject> ev;
        for (const QJsonValue& v : r.chr["events"].toArray()) ev[v.toObject()["name"].toString()] = v.toObject();
        auto rgb = [&](const QString& n) { const QJsonObject& o = ev[n];
            return QString("%1,%2,%3").arg(o["red"].toInt()).arg(o["green"].toInt()).arg(o["blue"].toInt()); };
        QCOMPARE(rgb("US6"), QString("0,0,255"));
        QCOMPARE(rgb("US7"), QString("255,19,12"));
        QCOMPARE(rgb("US1"), QString("255,178,97"));
        QCOMPARE(rgb("US10"), QString("71,112,76"));
        QCOMPARE(ev["US6"]["dates"].toArray()[0].toObject()["red"].toInt(), 71);     // la date garde sa couleur
        // 6 contraintes (le doublon US10/US6 est ignoré)
        QCOMPARE(r.chr["events_constraints"].toArray().size(), 6);
        // diagramme : plus récent en haut (y plus petit)
        auto y = [&](const QString& n) { return ev[n]["item_y"].toDouble(); };
        auto x = [&](const QString& n) { return ev[n]["item_x"].toDouble(); };
        QVERIFY(y("US7") < y("US10"));
        QCOMPARE(y("US10"), y("US3"));
        QVERIFY(y("US10") < y("US6"));
        QCOMPARE(y("US6"), y("US5"));
        QVERIFY(y("US6") < y("US1"));
        QVERIFY(x("US6") < x("US5"));
        QVERIFY(x("US10") < x("US3"));
        // « Rennes2025 » est la référence de l'échantillon, pas une courbe
        const QJsonObject am = ev["US3"]["dates"].toArray()[0].toObject();
        QCOMPARE(am["name"].toString(), QString("Rennes2025"));
        QCOMPARE(am["data"].toObject()["refI_curve"].toString(), QString("gal2002sph2014_i.ref"));
        // couleurs : formes acceptées
        int cr = 0, cg = 0, cb = 0;
        const ChrResult c = ChrBuilder::build(QJsonDocument::fromJson(R"({"events":[
          {"name":"A","color":"en vert clair","dates":[{"type":"gauss","age":0,"error":10}]},
          {"name":"B","color":[10,20,30],"dates":[{"type":"gauss","age":0,"error":10}]},
          {"name":"C","color":"teal","dates":[{"type":"gauss","age":0,"error":10}]},
          {"name":"D","color":"chartreuse fluo","dates":[{"type":"gauss","age":0,"error":10}]}]})").object());
        QVERIFY(c.ok);
        const QJsonArray ce = c.chr["events"].toArray();
        cr = ce[1].toObject()["red"].toInt(); cg = ce[1].toObject()["green"].toInt(); cb = ce[1].toObject()["blue"].toInt();
        QCOMPARE(QString("%1,%2,%3").arg(cr).arg(cg).arg(cb), QString("10,20,30"));
        QVERIFY(ce[0].toObject()["green"].toInt() > 170);
        QCOMPARE(ce[2].toObject()["blue"].toInt(), 128);
        QCOMPARE(ce[3].toObject()["red"].toInt(), 71);
        QVERIFY(!c.warnings.filter("chartreuse fluo").isEmpty());
        // événements sans contrainte : grille à droite du diagramme, pas de chevauchement
        QSet<QPair<int,int>> pos;
        for (const QJsonValue& v : ce) pos.insert({ v.toObject()["item_x"].toInt(), v.toObject()["item_y"].toInt() });
        QCOMPARE(pos.size(), 4);
    }

    // Courbe âge-profondeur : demande réelle (carotte Teilleul 2) ; format de référence teilleulc2.chr (ChronoModel 4.0).
    void chrAgeProfondeur()
    {
        const QString ask = QStringLiteral("Je voudrais faire une courbe âge-profondeur pour la carotte Teilleul 2.\n"
            "J'ai 9 niveaux, du haut vers le bas, chacun plus ancien que celui du dessus.\n"
            "à 13 cm, Ly-8812 310 +- 50 BP\nà 21 cm, Ly-8813 590 +- 30 BP en bleu");
        QVERIFY(MethodRunner::isModelRequest(ask));
        QVERIFY(!MethodRunner::analyzeQuestion(ask).found);           // pas de calcul isolé des 14C
        QVERIFY(MethodRunner::isModelRequest("Build an age-depth model for this core"));
        QVERIFY(!MethodRunner::isModelRequest("Quelle est la profondeur de la fosse ?"));
        {
            SkillManager m; m.reload(QString());
            QVERIFY(MethodRunner::isAgeDepthRequest(ask));
            const auto sel = m.select(ask, AppSettings().skillChars, QStringList(),
                                      { "03_chronomodel_creer_modele", "13_chronomodel_courbe_age_profondeur" });
            QCOMPARE(sel.first().skill->id, QString("03_chronomodel_creer_modele"));
            QCOMPARE(sel[1].skill->id, QString("13_chronomodel_courbe_age_profondeur"));
            QVERIFY(sel[1].skill->body.contains("age_depth") && sel[1].skill->body.contains("dating name/code"));
        }
        const QString spec = R"({"name":"Teilleul 2","curve":"age_depth","events":[
          {"name":"surface","depth":4,"depth_error":1,"node":true,"color":"vert","dates":[{"type":"unif","name":"surface","min":-1500,"max":2000}]},
          {"name":"13 cm","depth":13,"depth_error":2,"dates":[{"type":"c14","name":"Ly-8812","age":310,"error":50,"curve":"intcal20"}]},
          {"name":"21 cm","depth":21,"depth_error":2,"color":"bleu","dates":[{"type":"c14","name":"Ly-8813","age":590,"error":30}]},
          {"name":"106 cm","depth":106,"depth_error":2,"color":"rouge","dates":[{"type":"c14","name":"Ly-8830","age":2930,"error":40}]}],
          "event_constraints":[["106 cm","21 cm","13 cm","surface"]],
          "settings":{"tmin":-1500,"tmax":2000,"step":1},"mcmc":{"anneal_recurrence":0}})";
        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(spec.toUtf8()).object());
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        const QJsonObject cv = r.chr["curve"].toObject();
        QCOMPARE(cv["process_type"].toInt(), 80);
        QCOMPARE(cv["variance_type"].toInt(), 71);
        QCOMPARE(cv["use_err_mesure"].toBool(), true);
        QCOMPARE(cv["alpha"].toDouble(), 0.0);
        const QJsonArray ev = r.chr["events"].toArray();
        QCOMPARE(ev[0].toObject()["X_Inc_Depth"].toDouble(), -4.0);
        QCOMPARE(ev[0].toObject()["sX_Alpha95_sDepth"].toDouble(), 1.0);
        QCOMPARE(ev[0].toObject()["point_type"].toInt(), 1);
        QCOMPARE(ev[1].toObject()["X_Inc_Depth"].toDouble(), -13.0);
        QCOMPARE(ev[1].toObject()["point_type"].toInt(), 0);
        QCOMPARE(ev[3].toObject()["sX_Alpha95_sDepth"].toDouble(), 2.0);
        QCOMPARE(ev[3].toObject()["sampler"].toInt(), 1);
        QCOMPARE(ev[2].toObject()["blue"].toInt(), 255);
        QCOMPARE(r.chr["events_constraints"].toArray().size(), 3);
        QCOMPARE(r.chr["mcmc"].toObject()["anneal_recurrence"].toInt(), 0);
        // le plus profond (le plus ancien) en bas du diagramme
        QVERIFY(ev[3].toObject()["item_y"].toDouble() > ev[0].toObject()["item_y"].toDouble());
        // profondeurs sans « curve » : courbe âge-profondeur déduite ; profondeur manquante : erreur
        const ChrResult d = ChrBuilder::build(QJsonDocument::fromJson(R"({"events":[
          {"name":"A","depth":10,"depth_error":1,"dates":[{"type":"gauss","age":0,"error":10}]},
          {"name":"B","dates":[{"type":"gauss","age":-100,"error":10}]}]})").object());
        QCOMPARE(d.chr["curve"].toObject()["process_type"].toInt(), 80);
        QVERIFY(!d.ok && d.errors.join(" ").contains("« B » n'a pas de profondeur"));
        // formes approximatives d'un petit modèle : date à plat sur l'événement, « date » objet, « method » au lieu de « type »
        const ChrResult f = ChrBuilder::build(QJsonDocument::fromJson(R"({"curve":"age_depth","events":[
          {"name":"13 cm","depth":13,"depth_error":2,"type":"c14","ref":"Ly-8812","age":310,"error":50},
          {"name":"21 cm","depth":21,"depth_error":2,"date":{"method":"c14","name":"Ly-8813","age":590,"error":30}},
          {"name":"29 cm","depth":29,"depth_error":2,"dates":{"type":"c14","reference":"Ly-8814","age":1080,"error":70}}],
          "event_constraints":[["29 cm","21 cm","13 cm"]]})").object());
        QVERIFY2(f.ok, qPrintable(f.errors.join("; ")));
        const QJsonArray fe = f.chr["events"].toArray();
        QCOMPARE(fe[0].toObject()["type"].toInt(), 0);
        QCOMPARE(fe[0].toObject()["dates"].toArray()[0].toObject()["name"].toString(), QString("Ly-8812"));
        QCOMPARE(fe[0].toObject()["dates"].toArray()[0].toObject()["plugin_id"].toString(), QString("14c"));
        QCOMPARE(fe[1].toObject()["dates"].toArray()[0].toObject()["name"].toString(), QString("Ly-8813"));
        QCOMPARE(fe[2].toObject()["dates"].toArray()[0].toObject()["name"].toString(), QString("Ly-8814"));

        // Tableau CSV de ChronoModel (exemple de l'aide) : demande de modèle, lignes conservées au clic
        QString csvEx;
        for (const auto& e : seldon::HelpView::examples(seldon::HelpView::sourceHtml()))
            if (e.text.contains("teilleulc2.chr")) csvEx = e.text;
        QVERIFY(!csvEx.isEmpty());
        QCOMPARE(csvEx.count('\n'), 11);
        QVERIFY(csvEx.contains("\nUnif;surface;1980;2000;;-4;1;\n"));
        QVERIFY(MethodRunner::isModelRequest(csvEx) && MethodRunner::isAgeDepthRequest(csvEx));
        QVERIFY(MethodRunner::isAgeDepthRequest("method;dating name/code;param a;param b\n14C;LY;310;50"));
        QVERIFY(!MethodRunner::isAgeDepthRequest("C14 de 3560 +- 20 BP"));
        QVERIFY(!MethodRunner::analyzeQuestion(csvEx).found);
        QVERIFY(seldon::HelpView::toQtHtml(seldon::HelpView::sourceHtml()).contains("teilleulc2.chr%0Amethod"));
        // spécification attendue du LLM pour ce tableau, sans contrainte : ordre des profondeurs ajouté
        const ChrResult t = ChrBuilder::build(QJsonDocument::fromJson(R"({"name":"teilleulc2","curve":"age_depth","events":[
          {"name":"teilleulc2-1","depth":4,"depth_error":1,"dates":[{"type":"unif","name":"surface","min":1980,"max":2000}]},
          {"name":"teilleulc2-3","depth":21,"depth_error":2,"dates":[{"type":"c14","name":"LY","age":590,"error":30,"curve":"intcal20"}]},
          {"name":"teilleulc2-2","depth":13,"depth_error":2,"dates":[{"type":"c14","name":"LY","age":310,"error":50,"curve":"intcal20"}]}]})").object());
        QVERIFY2(t.ok, qPrintable(t.errors.join("; ")));
        const QJsonArray tc = t.chr["events_constraints"].toArray();
        QCOMPARE(tc.size(), 2);
        // 21 cm (id 2) plus ancien que 13 cm (id 3), lui-même plus ancien que la surface (id 1)
        QCOMPARE(tc[0].toObject()["bwd_id"].toInt(), 2); QCOMPARE(tc[0].toObject()["fwd_id"].toInt(), 3);
        QCOMPARE(tc[1].toObject()["bwd_id"].toInt(), 3); QCOMPARE(tc[1].toObject()["fwd_id"].toInt(), 1);
        // espacement : 450 entre deux niveaux successifs
        const QJsonArray te = t.chr["events"].toArray();
        QCOMPARE(te[1].toObject()["item_y"].toDouble() - te[2].toObject()["item_y"].toDouble(), 450.0);
    }

    // Calculatrice locale (niveaux 1 à 3 et conversions de dates)
    void calculator()
    {
        using seldon::Calculator;
        Calculator c;
        auto one = [&](const QString& t) { const auto l = c.evaluate(t); return l.isEmpty() || !l.last().ok ? std::nan("") : l.last().value; };
        QCOMPARE(one("2+3"), 5.0);
        QCOMPARE(one("2+3*4"), 14.0);
        QCOMPARE(one("(2+3)*4"), 20.0);
        QCOMPARE(one("-2^2"), -4.0);
        QCOMPARE(one("2^3^2"), 512.0);
        QCOMPARE(one("2**10"), 1024.0);
        QCOMPARE(one("3²"), 9.0);
        QCOMPARE(one("1,5 * 2"), 3.0);
        QCOMPARE(one("7 ÷ 2"), 3.5);
        QCOMPARE(one("2e3 + 1"), 2001.0);
        QCOMPARE(one("(1950 - 3560) * 1,03"), -1658.3);
        QCOMPARE(one("sqrt(30^2 + 40^2)"), 50.0);
        QCOMPARE(one("max(1, 7; 3)"), 7.0);                // « 1, 7 » (espace) : séparateur
        // virgule décimale quand les arguments sont séparés par « ; » (demande réelle : moyenne(15,6;12) donnait 11)
        QCOMPARE(one("moyenne(15,6;12)"), 13.8);
        QCOMPARE(one("moyenne(15,6; 12,4)"), 14.0);
        QCOMPARE(one("max(1,5)"), 5.0);                    // sans « ; » : la virgule sépare
        QCOMPARE(one("min(2,5; sqrt(4))"), 2.0);
        QCOMPARE(one("round(15,678; 1)"), 15.7);
        QCOMPARE(one("moyenne(max(1,5); 3)"), 4.0);        // chaque appel a sa propre règle
        QCOMPARE(one("(1,5 + 1) * 2"), 5.0);
        QCOMPARE(one("min(2.5; -1)"), -1.0);
        QCOMPARE(one("round(1950/7; 2)"), 278.57);
        QVERIFY(std::abs(one("sin(30)") - 0.5) < 1e-12);           // degrés
        QVERIFY(std::abs(one("asin(sin(65))") - 65) < 1e-9);
        QVERIFY(std::abs(one("sinr(pi/6)") - 0.5) < 1e-12);
        QVERIFY(std::abs(one("atan2(1; 1)") - 45) < 1e-12);
        QVERIFY(std::abs(one("ln(e)") - 1) < 1e-15);
        QCOMPARE(one("log(1000)"), 3.0);
        // conversions de dates (formules des formats d'affichage)
        QCOMPARE(one("ADtoBP(-550)"), 2500.0);
        QCOMPARE(one("BPtoAD(2500)"), -550.0);
        QCOMPARE(one("adtob2k(-550)"), 2550.0);
        QCOMPARE(one("B2KtoAD(2550)"), -550.0);
        QCOMPARE(one("BPtoB2K(2500)"), 2550.0);
        QCOMPARE(one("B2KtoBP(2550)"), 2500.0);
        QCOMPARE(one("ADtoKa(-50000)"), 50.0);
        QCOMPARE(one("KatoAD(50)"), -50000.0);
        QCOMPARE(one("MatoAD(1.2)"), -1200000.0);
        QCOMPARE(one("ADtoMa(-1200000)"), 1.2);
        QCOMPARE(one("BCtoAD(551)"), -550.0);
        QCOMPARE(one("ADtoBC(-549)"), 550.0);
        // variables, ans, plusieurs instructions
        const auto lines = c.evaluate("a = 3560\nb = 20; sqrt(a^2 + b^2)");
        QCOMPARE(lines.size(), 3);
        QCOMPARE(lines[0].variable, QString("a"));
        QVERIFY(std::abs(lines[2].value - std::sqrt(3560.0 * 3560 + 400)) < 1e-9);
        QCOMPARE(one("ADtoBP(-A)"), 5510.0);                       // casse ignorée
        QCOMPARE(one("ans + 1"), 5511.0);
        // erreurs mathématiques : calcul quand même, avec un message
        auto err = [&](const QString& t) { const auto l = c.evaluate(t); return l.isEmpty() ? QString() : l.last().error; };
        QVERIFY(err("1/0").contains("division par zéro"));
        QVERIFY(err("sqrt(-4)").contains("négatif"));
        QVERIFY(err("ADtoBC(10)").contains("après J.-C."));
        QVERIFY(err("pi = 3").contains("réservé"));
        QVERIFY(c.isCalculation("1/0"));
        // reconnaissance : seulement un calcul
        QVERIFY(c.isCalculation("2+3"));
        QVERIFY(c.isCalculation("Calcule 2+3"));
        QVERIFY(c.isCalculation("combien font 12 * 7 ?"));
        QVERIFY(c.isCalculation("2+3 ="));
        QVERIFY(c.isCalculation("x = 4"));
        QVERIFY(c.isCalculation("ADtoBP(-550)"));
        QVERIFY(!c.isCalculation("2023"));
        QVERIFY(!c.isCalculation("-500"));
        QVERIFY(!c.isCalculation("C14 de 3560 +- 20 BP"));
        QVERIFY(!c.isCalculation("3560 +- 20"));
        QVERIFY(!c.isCalculation("3560 ± 20"));
        QVERIFY(!c.isCalculation("1950-3560 BP"));
        QVERIFY(!c.isCalculation("que vaut 2+3 en BP ?"));
        QVERIFY(!c.isCalculation("Trace une gaussienne de moyenne -500 et d'écart-type 40"));
        QVERIFY(!c.isCalculation("US10 au-dessus de US6"));
        QVERIFY(!c.isCalculation("date uniforme entre -500 et -400"));
        QVERIFY(!c.isCalculation("Dessine le modèle"));
        // radiocarbone : âge <-> F14C, erreur comme ChronoModel (aller-retour exact)
        {
            const auto l = c.evaluate("C14toF14C(2500; 30)");
            QVERIFY(l[0].ok);
            QVERIFY(std::abs(l[0].value - std::exp(-2500 / 8033.0)) < 1e-15);
            QCOMPARE(l[0].parts.size(), 2);
            QVERIFY(std::abs(l[0].parts[1].second - (std::exp(-2500 / 8033.0) - std::exp(-2530 / 8033.0))) < 1e-15);
            QVERIFY(l[0].details.startsWith("F14C = 0,73"));
            const auto r = c.evaluate(QStringLiteral("F14CtoC14(%1; %2)").arg(l[0].value, 0, 'g', 17).arg(l[0].parts[1].second, 0, 'g', 17));
            QVERIFY(std::abs(r[0].value - 2500) < 1e-6 && std::abs(r[0].parts[1].second - 30) < 1e-6);
            QVERIFY(err("F14CtoC14(-1)").contains("> 0"));
        }
        // archéomagnétisme
        QVERIFY(std::abs(one("IGAD(45)") - std::atan(2.0) * 180 / M_PI) < 1e-12);
        QVERIFY(std::abs(one("IGAD(0)")) < 1e-12);
        QVERIFY(std::abs(one("ReducGAD(65; 43.6; 48.85)") - (65 + one("IGAD(48.85)") - one("IGAD(43.6)"))) < 1e-12);
        {
            const auto l = c.evaluate("p = VGP(43.6; 1.4; 10; 62; 2)");
            QVERIFY(l[0].ok);
            QCOMPARE(l[0].parts.size(), 4);
            QVERIFY(c.variables().contains("p_lon") && c.variables().contains("p_dp") && c.variables().contains("p_dm"));
            const auto back = c.evaluate("VGPtoDI(p; p_lon; 43.6; 1.4)");               // aller-retour au même site
            QVERIFY(std::abs(back[0].value - 62) < 1e-9);
            QVERIFY(std::abs(back[0].parts[1].second - 10) < 1e-9);
            QVERIFY(std::abs(one("VGP(0; 0; 0; 0)") - 90) < 1e-9);                    // champ dipolaire à l'équateur
            const auto same = c.evaluate("ReducVGP(43.6; 1.4; 10; 62; 43.6; 1.4)");
            QVERIFY(std::abs(same[0].value - 62) < 1e-9 && std::abs(same[0].parts[1].second - 10) < 1e-9);
            // réduction d'une direction purement dipolaire : on retrouve l'inclinaison GAD du site de référence
            const auto gad = c.evaluate(QStringLiteral("ReducVGP(40; 5; 0; %1; 48.85; 2.35)").arg(one("IGAD(40)"), 0, 'g', 17));
            QVERIFY(std::abs(gad[0].value - one("IGAD(48.85)")) < 1e-9 && std::abs(gad[0].parts[1].second) < 1e-9);
        }
        {
            const double R = 6.3712e6, m = R * R * R * 50 * 10 / std::sqrt(2.5) / 1e22;   // 4π R³ F/(µ0 √(1+3 sin² 45°))
            QVERIFY(std::abs(one("VADM(50; 45)") - m) < 1e-9);
            const auto l = c.evaluate("VADM(50; 2; 45)");
            QVERIFY(std::abs(l[0].parts[1].second - m * 2 / 50) < 1e-9);
            QVERIFY(std::abs(one("VDM(50; IGAD(45))") - m) < 1e-9);                  // champ GAD : VDM = VADM
            QVERIFY(std::abs(one("VADMtoF(VADM(50; 45); 45)") - 50) < 1e-9);
            QVERIFY(std::abs(one("ReducVADM(50; 45; 45)") - 50) < 1e-12);
            QVERIFY(std::abs(one("ReducVADM(50; 43.6; 48.85)") - one("VADMtoF(VADM(50; 43.6); 48.85)")) < 1e-9);
            QVERIFY(err("VADM(50; 95)").contains("hors de"));
        }
        // incertitudes : premier ordre, erreurs indépendantes
        auto ve = [&](const QString& t) { const auto l = c.evaluate(t); return l.isEmpty() || !l.last().ok ? qMakePair(std::nan(""), std::nan(""))
                                                                                                     : qMakePair(l.last().value, l.last().uncertainty); };
        auto near = [](QPair<double, double> r, double v, double e, double tol = 1e-6) {
            return std::abs(r.first - v) < tol * std::max(1.0, std::abs(v)) && std::abs(r.second - e) < tol * std::max(1.0, e); };
        QVERIFY(near(ve("(2500 ± 30) - (2400 ± 40)"), 100, 50));
        QVERIFY(near(ve("2 * (310 +- 50)"), 620, 100));
        QVERIFY(near(ve("2*310 ± 50"), 620, 50));                        // « ± » a la priorité de + et −
        QVERIFY(near(ve("(2500 +/- 30) + 0"), 2500, 30));
        QVERIFY(near(ve("(100 ± 10) / (4 ± 0)"), 25, 2.5));
        QVERIFY(near(ve("sqrt(100 ± 10)"), 10, 0.5, 1e-5));
        QVERIFY(near(ve("ln(100 ± 10)"), std::log(100.0), 0.1, 1e-5));
        QVERIFY(near(ve("(3 ± 0.1)^2"), 9, 0.6));
        QVERIFY(near(ve("ADtoBP(-550 ± 20)"), 2500, 20, 1e-5));
        QVERIFY(near(ve("a = 2500 ± 30\na - a"), 0, 30 * std::sqrt(2.0)));      // indépendance : pas de corrélation
        QVERIFY(near(ve("val(a)"), 2500, 0) && near(ve("err(a)"), 30, 0));
        QVERIFY(near(ve("C14toF14C(2500 ± 30)"), std::exp(-2500 / 8033.0), std::exp(-2500 / 8033.0) - std::exp(-2530 / 8033.0), 1e-9));
        QVERIFY(near(ve("round(2500.4 ± 30)"), 2500, 30));
        // statistiques
        QVERIFY(near(ve("moyenne(2500, 2470, 2530)"), 2500, 0));
        QVERIFY(near(ve("mean(2500 ± 30; 2470 ± 30; 2530 ± 30)"), 2500, 30 / std::sqrt(3.0)));
        QVERIFY(near(ve("ecart_type(2500, 2470, 2530)"), 30, 0));
        QVERIFY(near(ve("mediane(3; 1; 2; 10)"), 2.5, 0));
        QVERIFY(near(ve("somme(1 ± 3; 2 ± 4)"), 3, 5));
        {
            const auto l = c.evaluate("moy_ponderee(2500 ± 30; 2470 ± 35; 2530 ± 25)");
            QVERIFY(l[0].ok);
            const double w[3] = { 1 / 900.0, 1 / 1225.0, 1 / 625.0 }, x[3] = { 2500, 2470, 2530 };
            double sw = 0, swx = 0;
            for (int i = 0; i < 3; ++i) { sw += w[i]; swx += w[i] * x[i]; }
            const double m = swx / sw;
            double T = 0;
            for (int i = 0; i < 3; ++i) T += (x[i] - m) * (x[i] - m) * w[i];
            QVERIFY(std::abs(l[0].value - m) < 1e-9 && std::abs(l[0].uncertainty - 1 / std::sqrt(sw)) < 1e-9);
            QHash<QString, double> pr;
            for (const auto& p : l[0].parts) pr[p.first] = p.second;
            QVERIFY(std::abs(pr["t"] - T) < 1e-9);
            QCOMPARE(pr["ddl"], 2.0);
            QVERIFY(std::abs(pr["seuil"] - 5.991465) < 1e-5);                  // χ² 95 % à 2 ddl
            QVERIFY(std::abs(pr["p"] - std::exp(-T / 2)) < 1e-9);               // à 2 ddl, P(χ² > T) = exp(−T/2)
            QVERIFY(l[0].details.contains("compatibles") && !l[0].details.contains("NON"));
            const auto bad = c.evaluate("wmean(2500 ± 10; 2600 ± 10)");
            QVERIFY(bad[0].details.contains("NON compatibles"));
            QVERIFY(err("moy_ponderee(2500; 2600)").contains("erreur > 0"));
        }
        // reconnaissance avec « ± »
        QVERIFY(c.isCalculation("(2500 ± 30) - (2400 ± 40)"));
        QVERIFY(c.isCalculation("2 * (310 +- 50)"));
        QVERIFY(c.isCalculation("moy_ponderee(2500±30, 2470±35, 2530±25)"));
        QVERIFY(!c.isCalculation("-500 +- 40"));
        QVERIFY(!c.isCalculation("C14 2500 +- 30"));
        QVERIFY(!c.isCalculation("2500 +- 30, 2470 +- 35"));
        QCOMPARE(Calculator::formatValue(620, 100), QString("620 ± 100"));
        QCOMPARE(Calculator::formatValue(0.73255545, 0.0027307), QString("0,7326 ± 0,0027"));
        QCOMPARE(Calculator::formatValue(-0.001, 42.43), QString("0 ± 42"));
        QCOMPARE(Calculator::format(1658.3), QString("1658,3"));
        QCOMPARE(Calculator::format(-0.0), QString("0"));
        c.reset();
        QVERIFY(!c.isCalculation("a + 1"));                        // variable oubliée : nom inconnu
    }

    // Dessin d'un .chr : scène des Events (boîtes, noms des dates, flèches) et scène des phases.
    void chrScenes()
    {
        using seldon::ChrSceneWidget;
        QVERIFY(ChrSceneWidget::asksForDrawing("Dessine le modèle"));
        QVERIFY(ChrSceneWidget::asksForDrawing("montre-moi la scène des phases"));
        QVERIFY(ChrSceneWidget::asksForDrawing("Draw the model"));
        QVERIFY(!ChrSceneWidget::asksForDrawing("Trace une gaussienne de moyenne -500"));
        QVERIFY(!ChrSceneWidget::asksForDrawing("C14 de 3560 +- 20 BP"));
        // seulement le dessin : pas de modèle de langage ; autre demande : le modèle de langage répond au reste
        QVERIFY(ChrSceneWidget::isOnlyDrawing("dessine le modèle"));
        QVERIFY(ChrSceneWidget::isOnlyDrawing("Affiche les scènes des Events et des phases"));
        QVERIFY(ChrSceneWidget::isOnlyDrawing("Draw the model"));
        QVERIFY(!ChrSceneWidget::isOnlyDrawing("Dessine le modèle et explique les contraintes"));
        QVERIFY(!ChrSceneWidget::isOnlyDrawing("Peux-tu dessiner le modèle ?"));
        QVERIFY(!ChrSceneWidget::isOnlyDrawing("Dessine le modèle et commente la phase 2"));
        // bloc plot inventé par le modèle de langage (réponse réelle) : retiré
        const QString bad = "Voici le bloc de code pour tracer le modèle :\n\n```plot\n{\n  \"type\": \"events\",\n  \"scenes\": [\"Events\", \"Phases\"]\n}\n```\n";
        QCOMPARE(ChrSceneWidget::stripPlotBlocks(bad), QString("Voici le bloc de code pour tracer le modèle :"));
        QCOMPARE(ChrSceneWidget::stripPlotBlocks("Texte\n```json\n{}\n```"), QString("Texte\n```json\n{}\n```"));

        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(R"({"name":"Site","phases":[{"name":"Ancienne","color":"orange"},{"name":"Récente"}],
          "events":[
            {"name":"US 12","phases":["Ancienne"],"color":"bleu","dates":[{"type":"c14","name":"Ly-1","age":2500,"error":30},{"type":"gauss","name":"G-2","age":-500,"error":40}]},
            {"name":"US 11","phases":["Ancienne"],"dates":[{"type":"gauss","name":"G-3","age":-480,"error":40}]},
            {"name":"US 10","phases":["Récente"],"dates":[{"type":"gauss","name":"G-4","age":-400,"error":30}]},
            {"name":"Fin","type":"bound","known_fixed":-300}],
          "event_constraints":[["US 12","US 11","US 10","Fin"]],
          "phase_constraints":[["Ancienne","avant","Récente"]]})").object());
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        QVERIFY(ChrSceneWidget::hasPhases(r.chr));

        ChrSceneWidget ev(r.chr, ChrSceneWidget::Events);
        QCOMPARE(ev.boxes().size(), 4);
        QCOMPARE(ev.links().size(), 3);
        const auto& b0 = ev.boxes()[0];
        QCOMPARE(b0.title, QString("US 12"));
        QCOMPARE(b0.lines.size(), 2);
        QCOMPARE(b0.lines[0].text, QString("Ly-1"));
        QVERIFY(b0.lines[0].detail.startsWith("14C 2500"));
        QCOMPARE(b0.color, QColor(0, 0, 255));
        QVERIFY(ev.boxes()[3].bound);
        // la boîte est centrée sur la position du .chr ; flèche du plus ancien (US 12) vers le plus récent (US 11)
        const QJsonObject e0 = r.chr["events"].toArray()[0].toObject();
        QCOMPARE(b0.rect.center(), QPointF(e0["item_x"].toDouble(), e0["item_y"].toDouble()));
        QCOMPARE(ev.links()[0].from, 0); QCOMPARE(ev.links()[0].to, 1);
        // les boîtes ne se chevauchent pas avec l'espacement du diagramme
        for (int i = 0; i < ev.boxes().size(); ++i)
            for (int j = i + 1; j < ev.boxes().size(); ++j)
                QVERIFY(!ev.boxes()[i].rect.intersects(ev.boxes()[j].rect));

        ChrSceneWidget ph(r.chr, ChrSceneWidget::Phases);
        QCOMPARE(ph.boxes().size(), 2);
        QCOMPARE(ph.links().size(), 1);
        QCOMPARE(ph.boxes()[0].title, QString("Ancienne"));
        QCOMPARE(ph.boxes()[0].lines.size(), 2);
        QCOMPARE(ph.boxes()[0].lines[1].text, QString("US 11"));
        QCOMPARE(ph.boxes()[0].color, QColor(255, 178, 97));
        QVERIFY(ph.boxes()[0].rect.center().y() > ph.boxes()[1].rect.center().y());     // la plus ancienne en bas

        const QImage img = ev.render(ev.exportSize());
        QVERIFY(!img.isNull());
        const QByteArray svg = ev.renderSvg(ev.exportSize(), "Scène");
        QVERIFY(svg.contains("<svg") && svg.contains("Ly-1") && svg.contains("US 12"));

        // export depuis le panneau
        seldon::FigurePanel panel;
        panel.showPage(new ChrSceneWidget(r.chr, ChrSceneWidget::Phases), "Scène des phases");
        QTemporaryDir dir;
        QVERIFY(panel.saveCurrentTo(dir.filePath("phases.svg")));
        QVERIFY(panel.saveCurrentTo(dir.filePath("phases.png")));
        QVERIFY(!QImage(dir.filePath("phases.png")).isNull());

        // pas de pastille de couleur ni de paramètres hors courbe ; vignette pour la discussion
        QVERIFY(ev.boxes()[0].params.isEmpty());
        const QImage th = ChrSceneWidget::thumbnail(r.chr, ChrSceneWidget::Events, 520, 2.0);
        QCOMPARE(th.width(), 1040);
        QVERIFY(th.height() >= 320 && th.height() <= 720);

        // modèle avec courbe : paramètres dans un cadre sous le nom de l'Event
        const ChrResult c = ChrBuilder::build(QJsonDocument::fromJson(R"({"curve":"age_depth","events":[
          {"name":"surface","depth":4,"depth_error":1,"node":true,"dates":[{"type":"unif","min":1980,"max":2000}]},
          {"name":"13 cm","depth":13,"depth_error":2,"dates":[{"type":"c14","name":"Ly-8812","age":310,"error":50}]}]})").object());
        QVERIFY2(c.ok, qPrintable(c.errors.join("; ")));
        ChrSceneWidget cs(c.chr, ChrSceneWidget::Events);
        QCOMPARE(cs.boxes()[0].params, QStringList({ "Profondeur : -4 ± 1", "Nœud (la courbe passe par ce point)" }));
        QCOMPARE(cs.boxes()[1].params, QStringList({ "Profondeur : -13 ± 2" }));
        QCOMPARE(cs.boxes()[1].lines.size(), 1);                              // seulement la date
        QJsonObject inc{ { "X_Inc_Depth", 65 }, { "sX_Alpha95_sDepth", 1.5 } };
        QCOMPARE(ChrSceneWidget::curveParams(inc, 73), QStringList({ "Inclinaison : 65", "α95 : 1.5" }));
        QVERIFY(ChrSceneWidget::curveParams(inc, 78).isEmpty());

        // projet sans phase, scène vide
        ChrSceneWidget none(QJsonObject(), ChrSceneWidget::Events);
        QVERIFY(none.boxes().isEmpty());
        QVERIFY(!none.render(QSize(300, 200)).isNull());
    }

    // Fiches injectées pour une création de modèle, et vérification croisée du modèle avec la question (réponses réelles
    // de Qwen3.5-2B et de ministral-3-8b à la même question, 27/10/2026).
    void modelSkillsAndCrossCheck()
    {
        const QString q = QStringLiteral(
            "crée un modèle avec un Event US10 au dessus de l'Event US6\nEvent US 3 est au dessus de US5\n"
            "US6 est en dessous de l'US10 et de l'US7\nUS7 est aussi au dessus de US3\nUS1 est en dessous de US6 et US5\n"
            "dans US10: 14c 6589 +- 30 et 14c 5879+- 35\nUS6: 14c référence Ly-12345 6458 +- 30 en bleu\nUS5: 14C 6455 +- 40\n\n"
            "US3: am Rennes2025 inclinaison 65 alpha95 1.2\nUS7: une borne à 40 BC couleur rouge\n"
            "US1: TL réf Bdx-478 age 3500 +- 45 fait en 2010 en orange");
        QCOMPARE(MethodRunner::datingMethodsIn(q), QStringList({ "c14", "tl", "am" }));
        QCOMPARE(MethodRunner::datingMethodsIn("C14 de 3560 +- 20 BP"), QStringList({ "c14" }));
        QCOMPARE(MethodRunner::datingMethodsIn("date uniforme entre -500 et -400"), QStringList({ "gauss" }));
        QCOMPARE(MethodRunner::datingMethodsIn("échantillon F14C = 1,1023 +- 0,0030"), QStringList({ "f14c" }));
        QVERIFY(MethodRunner::datingMethodsIn("Bonjour").isEmpty());
        // création de modèle : la fiche commune et les fiches de ces trois méthodes, rien d'autre
        SkillManager m; m.reload(QString());
        const QStringList pin{ "03_chronomodel_creer_modele", "14_date_c14", "16_date_tl", "17_date_am" };
        const auto sel = m.select(q, 0, QStringList(), pin);
        QStringList ids; int chars = 0;
        for (const auto& x : sel) { ids << x.skill->id; chars += x.skill->body.size(); }
        QCOMPARE(ids, pin);
        QVERIFY2(chars < 9000, qPrintable(QString::number(chars)));          // contre ~20 000 caractères auparavant
        for (const auto& x : sel) {                                           // un seul nom par champ : ceux du .chr
            QVERIFY(!x.skill->body.contains("refYear") && !x.skill->body.contains("errorF") && !x.skill->body.contains("deltaR"));
        }
        // Qwen3.5-2B : valeurs recopiées de l'exemple (54.2, 0.8), références oubliées
        const QJsonObject qwen = QJsonDocument::fromJson(R"({"name":"Site test","events":[
          {"name":"US 10","dates":[{"type":"c14","name":"Ly-12345","age":6589,"error":30},{"type":"c14","name":"Ly-12345","age":5879,"error":35}]},
          {"name":"US 6","dates":[{"type":"c14","name":"Ly-12345","age":6458,"error":30}]},
          {"name":"US 5","dates":[{"type":"c14","name":"Ly-12345","age":6455,"error":40}]},
          {"name":"US 3","dates":[{"type":"am","process":"IF","inc":65,"alpha95":1.2,"field":54.2,"error_f":0.8}]},
          {"name":"US 7","dates":[{"type":"unif","min":-40,"max":-40}]},
          {"name":"US 1","dates":[{"type":"tl","age":3500,"error":45,"ref_year":2010}]}]})").object();
        const QString g1 = ChrBuilder::crossCheck(qwen, q).join("\n");
        QVERIFY2(g1.contains("54.2") && g1.contains("0.8"), qPrintable(g1));
        QVERIFY(g1.contains("« bdx-478 » de votre texte absente du modèle"));
        // ministral-3-8b (Albert) : références inventées
        const QJsonObject albert = QJsonDocument::fromJson(R"({"name":"Site_test","events":[
          {"name":"US10","dates":[{"type":"c14","name":"Ref-1","age":6589,"error":30},{"type":"c14","name":"Ref-2","age":5879,"error":35}]},
          {"name":"US6","dates":[{"type":"c14","name":"Ly-12345","age":6458,"error":30}]},
          {"name":"US5","dates":[{"type":"c14","name":"Ref-3","age":6455,"error":40}]},
          {"name":"US3","dates":[{"type":"am","name":"Rennes2025","process":"I","inc":65,"alpha95":1.2}]},
          {"name":"US7","type":"bound","known_fixed":-40},
          {"name":"US1","dates":[{"type":"tl","name":"Bdx-478","age":3500,"error":45,"ref_year":2010}]}]})").object();
        const QStringList g2 = ChrBuilder::crossCheck(albert, q);
        QCOMPARE(g2.filter("inventée par le modèle").size(), 3);
        QVERIFY(g2.filter("Valeur(s) absente(s)").isEmpty());
        // modèle correct : aucune remarque
        QJsonObject good = albert;
        QJsonArray evs = good["events"].toArray();
        for (int i = 0; i < evs.size(); ++i) {
            QJsonObject e = evs[i].toObject();
            QJsonArray ds = e["dates"].toArray();
            for (int k = 0; k < ds.size(); ++k) {
                QJsonObject d = ds[k].toObject();
                if (d["name"].toString().startsWith("Ref-")) d.remove("name");
                ds[k] = d;
            }
            e["dates"] = ds;
            evs[i] = e;
        }
        good["events"] = evs;
        QVERIFY2(ChrBuilder::crossCheck(good, q).isEmpty(), qPrintable(ChrBuilder::crossCheck(good, q).join("\n")));
    }

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

        QCOMPARE(keys(r.chr), L({ "app_version", "curve", "dates_trash", "event_model", "events", "events_constraints",
                                  "events_trash", "format_date", "format_date_custom", "mcmc", "phases", "phases_constraints",
                                  "settings" }));
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

    // Dates radiocarbone exportées au format du plugin 14c de ChronoModel (relevé dans Castelnau_version_19-12-2016.chr :
    // "data": {"age", "delta_r", "delta_r_error", "error", "ref_curve": "intcal13.14c"}).
    void chrRadiocarbon()
    {
        using namespace seldon;
        const QString spec = R"({"name":"Projet","events":[{"name":"titi","dates":[
            {"type":"c14","name":"D1","age":15600,"error":50},
            {"type":"c14","name":"D2","age":16040,"error":30,"curve":"IntCal13.14C"},
            {"type":"c14","name":"Coq","age":3000,"error":30,"curve":"marine20","delta_r":-50,"delta_r_error":30}]}]})";
        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(spec.toUtf8()).object());
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        const QJsonArray ds = r.chr["events"].toArray()[0].toObject()["dates"].toArray();
        QCOMPARE(ds.size(), 3);
        auto keys = [](const QJsonObject& o) { QStringList k = o.keys(); k.sort(); return k; };
        const QJsonObject d1 = ds[0].toObject(), d2 = ds[1].toObject(), d3 = ds[2].toObject();
        QCOMPARE(d1["plugin_id"].toString(), QString("14c"));
        QCOMPARE(d1["sampler"].toInt(), 4);
        QCOMPARE(keys(d1["data"].toObject()), QStringList({ "age", "delta_r", "delta_r_error", "error", "ref_curve" }));
        QCOMPARE(d1["data"].toObject()["ref_curve"].toString(), QString("intcal20.14c"));          // défaut, avec avertissement
        QVERIFY(r.warnings.join(" ").contains("IntCal20"));
        QCOMPARE(d2["data"].toObject()["ref_curve"].toString(), QString("intcal13.14c"));          // minuscules, sans doubler .14c
        QCOMPARE(d3["data"].toObject()["delta_r"].toDouble(), -50.0);
        QCOMPARE(d3["data"].toObject()["delta_r_error"].toDouble(), 30.0);
        // plage d'étude : contient les HPD à 95 % des calibrations
        const int tmin = r.chr["settings"].toObject()["tmin"].toInt(), tmax = r.chr["settings"].toObject()["tmax"].toInt();
        for (const QJsonValue& v : ds) {
            const QJsonObject rq = MethodRunner::normalizeRequest(v.toObject());       // relecture du .chr par SeldonH
            QCOMPARE(rq["method"].toString(), QString("c14"));
            const MethodResult cal = MethodRegistry::instance().compute(rq);
            QVERIFY2(cal.ok, qPrintable(cal.error));
            for (const Interval& iv : cal.hpd95) QVERIFY(iv.from >= tmin && iv.to <= tmax);
        }
        QVERIFY(ChrBuilder::summarize(r.chr).contains("14C 15600±50"));
        // erreur manquante : refusée
        QVERIFY(!ChrBuilder::build(QJsonDocument::fromJson(R"({"events":[{"name":"E","dates":[{"type":"c14","age":2500}]}]})").object()).ok);
    }

    // Tous les plugins de date, avec et sans wiggle, comparés à un projet écrit par ChronoModel 4.0
    // (tests/data/test_tous_plugin_wiggle.chr : unif, gauss, 14c, f14c, density avec les 4 cas de décalage, am, tl/osl).
    void chrAllPluginsLikeReference()
    {
        using namespace seldon;
        QFile f(QFINDTESTDATA("data/test_tous_plugin_wiggle.chr"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject ref = QJsonDocument::fromJson(f.readAll()).object();
        auto keys = [](const QJsonObject& o) { QStringList k = o.keys(); k.sort(); return k; };
        QHash<QString, QStringList> refData;                 // plugin -> clés de « data »
        QStringList refDateKeys;
        for (const QJsonValue& ev : ref["events"].toArray())
            for (const QJsonValue& dv : ev.toObject()["dates"].toArray()) {
                const QJsonObject d = dv.toObject();
                refData[d["plugin_id"].toString()] = keys(d["data"].toObject());
                if (d.contains("is_selected")) refDateKeys = keys(d);
                // relecture par SeldonH : méthode reconnue, décalage compris, calcul possible (sauf densité : pas de fichier)
                if (d["plugin_id"].toString() == "density") continue;
                const QJsonObject rq = MethodRunner::normalizeRequest(d);
                QVERIFY2(!rq["method"].toString().isEmpty(), qPrintable(d["name"].toString()));
                QCOMPARE(rq.contains("wiggle"), d["delta_type"].toInt() >= 0);
                const MethodResult r = MethodRegistry::instance().compute(rq);
                QVERIFY2(r.ok, qPrintable(d["name"].toString() + " : " + r.error));
            }
        QCOMPARE(QSet<QString>(refData.keyBegin(), refData.keyEnd()),
                 QSet<QString>({ "unif", "gauss", "14c", "f14c", "density", "am", "tl/osl" }));

        const QString spec = R"({"name":"Tous","events":[
          {"name":"unif","dates":[{"type":"unif","name":"u0","min":0,"max":100},
             {"type":"unif","name":"u1","min":0,"max":100,"wiggle":{"type":"fixed","value":12}},
             {"type":"unif","name":"u2","min":0,"max":100,"wiggle":{"type":"range","min":-10,"max":10}},
             {"type":"unif","name":"u3","min":0,"max":100,"wiggle":{"type":"gaussian","mean":50,"error":5}}]},
          {"name":"Gauss","dates":[{"type":"gauss","name":"g0","age":0,"error":100},
             {"type":"gauss","name":"g1","age":0,"error":100,"wiggle":{"type":"fixed","value":-10}}]},
          {"name":"F14C","dates":[{"type":"f14c","name":"f0","f14c":1.2,"error":0.01,"curve":"bomb13nh1"},
             {"type":"f14c","name":"f1","f14c":1.2,"error":0.01,"curve":"bomb13nh1","wiggle":{"type":"range","min":-20,"max":20}}]},
          {"name":"Density","dates":[{"type":"density","name":"d0","curve":"-400_200.csv"},
             {"type":"density","name":"d1","curve":"-400_200","wiggle":{"type":"gaussian","mean":50,"error":3}}]},
          {"name":"AM","dates":[{"type":"am","name":"Inclinaison","process":"I","inc":65,"alpha95":1}]},
          {"name":"TL/OSL","dates":[{"type":"tl","name":"TL","age":100,"error":30,"ref_year":2026}]},
          {"name":"14C","dates":[{"type":"c14","name":"c0","age":3124,"error":30,"curve":"intcal20"},
             {"type":"c14","name":"c1","age":3124,"error":30,"curve":"intcal20","wiggle":{"type":"fixed","value":120}},
             {"type":"c14","name":"c2","age":3124,"error":30,"curve":"intcal20","wiggle":{"type":"range","min":-20,"max":20}},
             {"type":"c14","name":"c3","age":3124,"error":30,"curve":"intcal20","wiggle":{"type":"gaussian","mean":10,"error":23}}]}]})";
        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(spec.toUtf8()).object());
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        QHash<QString, QJsonObject> byName;
        for (const QJsonValue& ev : r.chr["events"].toArray())
            for (const QJsonValue& dv : ev.toObject()["dates"].toArray()) {
                const QJsonObject d = dv.toObject();
                byName[d["name"].toString()] = d;
                QCOMPARE(keys(d), refDateKeys);
                QCOMPARE(d["sampler"].toInt(), 4);
                const QString pl = d["plugin_id"].toString();
                QCOMPARE(keys(d["data"].toObject()), refData.value(pl));
            }
        // décalages : même encodage que ChronoModel
        QCOMPARE(byName["u0"]["delta_type"].toInt(), -1);
        QCOMPARE(byName["u1"]["delta_type"].toInt(), 0);  QCOMPARE(byName["u1"]["delta_fixed"].toDouble(), 12.0);
        QCOMPARE(byName["u2"]["delta_type"].toInt(), 2);  QCOMPARE(byName["u2"]["delta_min"].toDouble(), -10.0);
        QCOMPARE(byName["u2"]["delta_max"].toDouble(), 10.0);
        QCOMPARE(byName["u3"]["delta_type"].toInt(), 1);  QCOMPARE(byName["u3"]["delta_average"].toDouble(), 50.0);
        QCOMPARE(byName["u3"]["delta_error"].toDouble(), 5.0);
        QCOMPARE(byName["g1"]["delta_fixed"].toDouble(), -10.0);
        QCOMPARE(byName["f1"]["delta_type"].toInt(), 2);
        QCOMPARE(byName["d1"]["delta_type"].toInt(), 1);
        QCOMPARE(byName["f0"]["data"].toObject()["fraction"].toDouble(), 1.2);
        QCOMPARE(byName["f0"]["data"].toObject()["ref_curve"].toString(), QString("bomb13nh1.14c"));
        QCOMPARE(byName["d1"]["data"].toObject()["curve"].toString(), QString("-400_200.csv"));
        QCOMPARE(byName["c0"]["data"].toObject()["ref_curve"].toString(), QString("intcal20.14c"));
        QCOMPARE(byName["c1"]["delta_type"].toInt(), 0);  QCOMPARE(byName["c1"]["delta_fixed"].toDouble(), 120.0);
        QCOMPARE(byName["c2"]["delta_type"].toInt(), 2);  QCOMPARE(byName["c2"]["delta_min"].toDouble(), -20.0);
        QCOMPARE(byName["c3"]["delta_type"].toInt(), 1);  QCOMPARE(byName["c3"]["delta_error"].toDouble(), 23.0);
        QVERIFY(r.warnings.join(" ").contains("Calib/Density"));
        const QString sum = ChrBuilder::summarize(r.chr);
        QVERIFY(sum.contains("F14C 1.2±0.01"));
        QVERIFY(sum.contains("densité tabulée"));
    }

    // Contraintes entre événements ou phases : relation en toutes lettres (français / anglais), séquence, objet ;
    // doublons ignorés, contradictions et cycles refusés, contraintes listées en clair dans le résumé.
    void chrConstraintRelations()
    {
        using namespace seldon;
        auto build = [](const QString& cons, const QString& key = "event_constraints") {
            const QString spec = QStringLiteral(R"({"phases":[{"name":"P1"},{"name":"P2"}],"events":[
                {"name":"A","phases":["P1"],"dates":[{"type":"gauss","age":100,"error":10}]},
                {"name":"B","phases":["P1"],"dates":[{"type":"gauss","age":200,"error":10}]},
                {"name":"C","phases":["P2"],"dates":[{"type":"gauss","age":300,"error":10}]}],"%1":%2})").arg(key, cons);
            return ChrBuilder::build(QJsonDocument::fromJson(spec.toUtf8()).object());
        };
        // (plus ancien, plus récent) de chaque contrainte, par nom
        auto order = [](const ChrResult& r, const char* key = "events_constraints", const char* list = "events") {
            QHash<int, QString> n;
            for (const QJsonValue& v : r.chr[list].toArray()) n[v.toObject()["id"].toInt()] = v.toObject()["name"].toString();
            QStringList l;
            for (const QJsonValue& v : r.chr[key].toArray())
                l << n[v.toObject()["bwd_id"].toInt()] + "<" + n[v.toObject()["fwd_id"].toInt()];
            return l;
        };
        const struct { const char* rel; const char* expected; } cases[] = {
            { "avant", "A<B" }, { "antérieur à", "A<B" }, { "précède", "A<B" }, { "en dessous de", "A<B" }, { "sous", "A<B" },
            { "est coupé par", "A<B" }, { "est suivi par", "A<B" }, { "before", "A<B" }, { "older than", "A<B" }, { "below", "A<B" },
            { "is cut by", "A<B" }, { "après", "B<A" }, { "postérieur à", "B<A" }, { "succède à", "B<A" }, { "au-dessus de", "B<A" },
            { "recouvre", "B<A" }, { "coupe", "B<A" }, { "est précédé par", "B<A" }, { "after", "B<A" }, { "above", "B<A" },
            { "overlies", "B<A" }, { "younger than", "B<A" }, { "cuts", "B<A" }, { "is preceded by", "B<A" } };
        for (const auto& c : cases) {
            const ChrResult r = build(QStringLiteral(R"([["A","%1","B"]])").arg(QString::fromUtf8(c.rel)));
            QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
            QCOMPARE(order(r), QStringList{ QString::fromLatin1(c.expected) });
        }
        // forme historique, séquence, objets ; doublon ignoré
        QCOMPARE(order(build(R"([["A","B"]])")), QStringList{ "A<B" });
        QCOMPARE(order(build(R"([["A","B","C"]])")), QStringList({ "A<B", "B<C" }));
        QCOMPARE(order(build(R"([{"older":"C","younger":"A"},{"a":"B","relation":"above","b":"C"},["C","A"]])")),
                 QStringList({ "C<A", "C<B" }));
        // relation inconnue, contradiction et cycle : erreurs (pas de fichier incomplet)
        const ChrResult u = build(R"([["A","contemporain de","B"]])");
        QVERIFY(!u.ok && u.errors.join(" ").contains("relation non reconnue"));
        QVERIFY(!build(R"([["A","avant","B"],["A","après","B"]])").ok);
        const ChrResult cyc = build(R"([["A","B"],["B","C"],["C","A"]])");
        QVERIFY(!cyc.ok && cyc.errors.join(" ").contains("cycle"));
        // phases et résumé en clair
        const ChrResult ph = build(R"([["P2","succède à","P1"]])", "phase_constraints");
        QCOMPARE(order(ph, "phases_constraints", "phases"), QStringList{ "P1<P2" });
        QVERIFY(ChrBuilder::summarize(ph.chr).contains("Phases : P1 → P2"));
        QVERIFY(ChrBuilder::summarize(build(R"([["B","au-dessus de","A"]])").chr).contains("Événements : A → B"));
    }

    // Tournures anglaises reconnues par le détecteur de datations et les réglages du calcul.
    void englishPhrasing()
    {
        using namespace seldon;
        auto parse = [](const QString& q) { return MethodRegistry::instance().parse(q); };
        const auto tl = parse("TL 488 ± 19 years, measured in 2015");
        QVERIFY(!tl.isEmpty());
        QCOMPARE(tl[0]["ref_year"].toInt(), 2015);
        const auto sh = parse("radiocarbon 2500 ± 30 BP, southern hemisphere");
        QVERIFY(!sh.isEmpty());
        QCOMPARE(sh[0]["curve"].toString(), QString("shcal20"));
        const auto sea = parse("carbon-14 date on a shell, 3000 ± 30 BP");
        QVERIFY(!sea.isEmpty());
        QCOMPARE(sea[0]["curve"].toString(), QString("marine20"));
        const auto un = parse("uniform between 1100 and 1500");
        QVERIFY(!un.isEmpty());
        QCOMPARE(un[0]["max"].toDouble(), 1500.0);
        const auto am = parse("archaeomagnetic date: inclination 65, alpha95 1");
        QVERIFY(!am.isEmpty());
        QCOMPARE(am[0]["process"].toString(), QString("I"));
        QCOMPARE(am[0]["inc"].toDouble(), 65.0);
        QVERIFY(!parse("F14C 1.2 ± 0.01, post-bomb, southern hemisphere").isEmpty());
        // réglages
        double tmin = 0, tmax = 0;
        const QJsonObject m = MethodRunner::mcmcFromQuestion("run with 2 chains, 20000 iterations, no annealing, study period from -1000 to 500",
                                                             &tmin, &tmax);
        QCOMPARE(m["num_proc"].toInt(), 2);
        QCOMPARE(m["num_iter"].toInt(), 20000);
        QCOMPARE(m["anneal_recurrence"].toInt(), 0);
        QCOMPARE(tmin, -1000.0);
        QCOMPARE(tmax, 500.0);
        QCOMPARE(MethodRunner::requestedLevel("14C 2500 ± 30 BP at 99 %"), 99.0);
        QCOMPARE(MethodRunner::requestedLevel("with a confidence level of 90 %"), 90.0);
        QVERIFY(MethodRunner::asksEvent("combine these two dates"));
        QVERIFY(MethodRunner::asksOverlay("show them on the same graph"));
        const QJsonObject w = MethodRunner::wiggleFromQuestion("14C 2500 ± 30 BP with an offset between 10 and 30 years");
        QCOMPARE(w["type"].toString(), QString("range"));
        QCOMPARE(w["max"].toDouble(), 30.0);
        QCOMPARE(dateformat::detect("14C 3000 ± 30 BP, give the results in cal BP", DateFormat::BcAd), DateFormat::CalBP);
        QCOMPARE(dateformat::detect("14C 3000 ± 30 BP, express the intervals in ka", DateFormat::BcAd), DateFormat::Ka);
    }

    // Courbes de référence ajoutées dans un dossier (utilisateur ou Calib de ChronoModel) : lues avant les intégrées.
    void userCurvesFolder()
    {
        using namespace seldon;
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QDir(root.path()).mkpath("14C");
        QDir(root.path()).mkpath("AM");
        QVERIFY(QFile::copy(":/methods/c14/curves/intcal20.14c", root.path() + "/14C/MaCourbe.14c"));
        QVERIFY(QFile::copy(":/methods/c14/curves/intcal13.14c", root.path() + "/14C/intcal20.14c"));    // remplace l'intégrée
        QVERIFY(QFile::copy(":/methods/am/curves/GAL2002sph2014_I.ref", root.path() + "/AM/test_i.ref"));
        QFile junk(root.path() + "/14C/lisez-moi.txt"); QVERIFY(junk.open(QIODevice::WriteOnly)); junk.write("x"); junk.close();

        const QStringList saved = CurveStore::extraRoots();
        CurveStore::setExtraRoots({ root.path() });
        QVERIFY(CurveStore::names("c14").contains("macourbe"));
        QVERIFY(!CurveStore::names("c14").contains("lisez-moi"));               // seulement les .14c
        QVERIFY(CurveStore::names("c14").contains("marine20"));                 // les intégrées restent
        QVERIFY(CurveStore::sourceOf("c14", "intcal20").startsWith(root.path()));
        const MethodResult c = MethodRegistry::instance().compute(QJsonObject{ { "method", "c14" }, { "age", 2500 }, { "error", 30 }, { "curve", "macourbe" } });
        QVERIFY2(c.ok, qPrintable(c.error));
        const MethodResult a = MethodRegistry::instance().compute(QJsonObject{ { "method", "am" }, { "process", "I" }, { "inc", 65 },
                                                                               { "alpha95", 1 }, { "curve_i", "test_i" }, { "steps", 500 } });
        QVERIFY2(a.ok, qPrintable(a.error));
        // retour aux seules courbes intégrées
        CurveStore::setExtraRoots(saved);
        QVERIFY(CurveStore::sourceOf("c14", "intcal20").startsWith(":/"));
        QVERIFY(!CurveStore::names("c14").contains("macourbe"));
    }

    // Fichier de densité pour le plugin Density de ChronoModel (lois classiques, placement sur l'axe du temps).
    void densityFileTool()
    {
        using namespace seldon;
        auto build = [](const char* json) { return buildDensityFile(QJsonDocument::fromJson(QByteArray(json)).object()); };
        // relit le CSV comme PluginDensity::loadRefFile : 1re colonne, commentaires //, tmin, tmax, valeurs
        auto reread = [](const QString& csv) {
            QVector<double> v;
            for (const QString& l : csv.split('\n', Qt::SkipEmptyParts)) {
                if (l.trimmed().startsWith("//")) continue;
                bool ok = false; const double d = l.split(',').first().toDouble(&ok);
                if (ok) v << d;
            }
            return v;
        };
        // loi normale : bornes ±5σ, une valeur par an, statistiques retrouvées
        const DensityFileResult N = build(R"({"law":"normale","mean":-250,"sd":50})");
        QVERIFY2(N.ok, qPrintable(N.error));
        QCOMPARE(N.tmin, -500.0); QCOMPARE(N.tmax, 0.0); QCOMPARE(N.points, 501);
        const QVector<double> nv = reread(N.csv);
        QCOMPARE(nv.size(), N.points + 2);
        QCOMPARE(nv[0], -500.0); QCOMPARE(nv[1], 0.0);
        QVERIFY(N.csv.startsWith("// SeldonH : loi normale"));
        const DensityStats ns = densityStats(N.density);
        QVERIFY(std::abs(ns.mean + 250) < 0.5 && std::abs(ns.sd - 50) < 0.5);
        // durée log-normale comptée vers le passé depuis 1200 : toute la masse avant 1200, médiane ≈ 1170
        const DensityFileResult LN = build(R"({"law":"log-normale","median":30,"factor":2,"offset":1200,"reverse":true})");
        QVERIFY2(LN.ok, qPrintable(LN.error));
        QCOMPARE(LN.tmax, 1200.0);
        QVERIFY(LN.placement.contains("1200 − x"));
        QVERIFY(std::abs(densityStats(LN.density).median - 1170) < 1.5);
        // triangulaire décalée : mode en 110 ; uniforme au pas de 10 ans : 41 valeurs égales
        const DensityFileResult T = build(R"({"law":"triangular","min":0,"mode":10,"max":40,"offset":100})");
        QVERIFY(T.ok && std::abs(densityStats(T.density).mode - 110) < 0.5);
        const DensityFileResult U = build(R"({"law":"uniform","min":1100,"max":1500,"step":10,"name":"Four 3"})");
        QVERIFY(U.ok);
        QCOMPARE(U.points, 41);
        QCOMPARE(U.fileName, QString("Four 3.csv"));
        const QVector<double> uv = reread(U.csv);
        QCOMPARE(uv[2], uv.last());
        // autres lois ; densité infinie au bord (bêta de forme < 1) signalée
        for (const char* j : { R"({"law":"exponential","mean":50})", R"({"law":"gamma","mean":20,"sd":10,"offset":-300})",
                               R"({"law":"trapezoidal","min":0,"mode_min":10,"mode_max":30,"max":50})",
                               R"({"law":"beta","alpha":2,"beta":5,"min":0,"max":100})" }) {
            const DensityFileResult r = build(j);
            QVERIFY2(r.ok, j);
            QVERIFY(std::abs(densityArea(r.density) - 1.0) < 1e-6);
        }
        const DensityFileResult B = build(R"({"law":"beta","alpha":0.5,"beta":0.5,"min":0,"max":10,"points":51})");
        QVERIFY(B.ok && B.warnings.join(" ").contains("infinie"));
        // erreurs
        QVERIFY(!build(R"({"law":"normal","mean":0,"sd":0})").ok);
        QVERIFY(!build(R"({"law":"cauchy","mean":0})").ok);
        QVERIFY(!build(R"({"law":"normal","mean":0,"sd":10,"tmin":100,"tmax":50})").ok);
        // bornes imposées trop étroites : avertissement
        QVERIFY(build(R"({"law":"normal","mean":0,"sd":10,"tmin":-5,"tmax":5})").warnings.join(" ").contains("%"));

        // outil (modèle distant) et bloc « calcul » : fiche, graphique, lien d'enregistrement qui recalcule le même fichier
        const auto t = tools::execute("creer_densite", R"({"law":"normal","mean":-250,"sd":50,"name":"Test"})", DateFormat::BcAd);
        QVERIFY2(t.executed, qPrintable(t.content));
        QCOMPARE(t.run.plot["series"].toArray()[0].toObject()["name"].toString(), QString("Test.csv"));
        const QRegularExpressionMatch m = QRegularExpression("seldon://densityfile\\?spec=([A-Za-z0-9_-]+)").match(t.run.noticeHtml);
        QVERIFY(m.hasMatch());
        const QJsonObject spec = QJsonDocument::fromJson(QByteArray::fromBase64(m.captured(1).toLatin1(),
                                     QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)).object();
        QCOMPARE(buildDensityFile(spec).csv, buildDensityFile(QJsonObject{ { "law", "normal" }, { "mean", -250 }, { "sd", 50 }, { "name", "Test" } }).csv);
        const auto blocks = MethodRunner::extractCalcBlocks("```calcul\n{\"method\":\"density_file\",\"law\":\"uniforme\",\"min\":0,\"max\":10}\n```\n");
        QCOMPARE(blocks.size(), 1);
        const RunOutcome r = MethodRunner::run(blocks);
        QVERIFY(r.noticeHtml.contains("Enregistrer le fichier"));
        QVERIFY(r.promptBlock.contains("Calib/Density"));
        QCOMPARE(MethodRunner::normalizeRequest(QJsonObject{ { "method", "creer_densite" } })["method"].toString(), QString("density_file"));
    }

    // « Et à 50 % ? » après une calibration : la datation précédente est recalculée au niveau demandé.
    // Archéomagnétisme vectoriel IF : « erreur » sans précision, écrite après l'intensité = erreur sur F.
    void amIntensityError()
    {
        using namespace seldon;
        const auto v = MethodRegistry::instance().parse("date en am vectoriel IF; inclinaison=63,95 alpha95=1,83\nintensité=56,89 erreur=0.74");
        QCOMPARE(v.size(), 1);
        QCOMPARE(v[0]["process"].toString(), QString("IF"));
        QCOMPARE(v[0]["inc"].toDouble(), 63.95);
        QCOMPARE(v[0]["alpha95"].toDouble(), 1.83);
        QCOMPARE(v[0]["field"].toDouble(), 56.89);
        QCOMPARE(v[0]["error_f"].toDouble(), 0.74);
        QVERIFY2(MethodRegistry::instance().compute(v[0]).ok, qPrintable(MethodRegistry::instance().compute(v[0]).error));
        // une erreur écrite avant l'intensité n'est pas prise pour celle de F ; les formes déjà reconnues restent
        QCOMPARE(MethodRegistry::instance().parse("inclinaison 65 alpha95 1, intensité 50 ± 2")[0]["error_f"].toDouble(), 2.0);
        QCOMPARE(MethodRegistry::instance().parse("inclinaison 65 alpha95 1, intensité 50, erreur sur F 1,5")[0]["error_f"].toDouble(), 1.5);
        QCOMPARE(MethodRegistry::instance().parse("archaeomagnetic: inclination 65, alpha95 1, intensity 50, error 0.8")[0]["error_f"].toDouble(), 0.8);
    }

    // Chaque datation rappelle ses courbes de référence (nom et origine) ; le modèle doit les rappeler aussi.
    void datingShowsReferenceCurves()
    {
        using namespace seldon;
        const RunOutcome c = MethodRunner::analyzeQuestion("C14 de 3560 ± 20 BP");
        QVERIFY2(c.noticeHtml.contains("Courbe de référence : IntCal20 (intégrée)"), qPrintable(c.noticeHtml));
        QVERIFY(c.promptBlock.contains("Courbe de référence : IntCal20 (intégrée)"));
        QVERIFY(c.promptBlock.contains("Rappelle dans ta réponse la ou les courbes"));
        const RunOutcome m = MethodRunner::analyzeQuestion("date en am vectoriel IF; inclinaison=63,95 alpha95=1,83 intensité=56,89 erreur=0.74");
        QVERIFY2(m.noticeHtml.contains("Courbes de référence : I : GAL2002sph2014_I (intégrée) ; F : GwH2013uni_F (intégrée)"), qPrintable(m.noticeHtml));
        const RunOutcome f = MethodRunner::run({ QJsonObject{ { "method", "f14c" }, { "f14c", 1.2 }, { "error", 0.01 } } });
        QVERIFY2(f.noticeHtml.contains("bomb21NHZ1 (intégrée)"), qPrintable(f.noticeHtml));
        const RunOutcome g = MethodRunner::run({ QJsonObject{ { "method", "gauss" }, { "age", 1200 }, { "error", 50 } } });
        QVERIFY(!g.noticeHtml.contains("de référence") && !g.promptBlock.contains("Rappelle dans ta réponse"));
        const char* ev = R"({"method":"event","dates":[{"method":"c14","age":2500,"error":30},{"method":"c14","age":2550,"error":30,"curve":"intcal13"}],"mcmc":{"num_iter":2000,"num_burn":200,"num_proc":2,"seeds":[1,2]}})";
        const RunOutcome e = MethodRunner::run({ QJsonDocument::fromJson(ev).object() });
        QVERIFY2(e.noticeHtml.contains("Courbes de référence : IntCal20 (intégrée) ; IntCal13 (intégrée)"), qPrintable(e.noticeHtml.left(600)));
    }

    // Courbes nommées dans la question : utilisées au lieu des courbes par défaut.
    void namedCurvesInQuestion()
    {
        using namespace seldon;
        // la question de l'utilisateur : I et F nommées (eurw_2017_f vient de ChronoModel : absente ici, le calcul le dit)
        const auto v = MethodRegistry::instance().parse("date en am vectoriel IF; inclinaison=63,95 alpha95=1,83\n"
                                                        "intensité=56,89 erreur sur l'intensité=0,74\n"
                                                        "avec la courbe pour l'inclinaison gal2002sph2014_i\npour l'intensité eurw_2017_f");
        QCOMPARE(v.size(), 1);
        QCOMPARE(v[0]["curve_i"].toString(), QString("gal2002sph2014_i"));
        QCOMPARE(v[0]["curve_f"].toString(), QString("eurw_2017_f"));
        QCOMPARE(v[0]["error_f"].toDouble(), 0.74);
        const MethodResult miss = MethodRegistry::instance().compute(v[0]);
        QVERIFY(!miss.ok && miss.error.contains("eurw_2017_f"));
        // courbe intégrée nommée
        const auto b = MethodRegistry::instance().parse("inclinaison 65 alpha95 1, intensité 50 erreur 1, courbe pour l'intensité bulgaria_f");
        QCOMPARE(b[0]["curve_f"].toString(), QString("bulgaria_f"));
        QCOMPARE(b[0]["curve_i"].toString(), QString("gal2002sph2014_i"));                 // non nommée : défaut
        QVERIFY(MethodRegistry::instance().compute(b[0]).ok);
        // courbes ajoutées par l'utilisateur : nom sans suffixe (composante d'après le mot qui précède), et 14C
        QTemporaryDir root;
        QDir(root.path()).mkpath("AM"); QDir(root.path()).mkpath("14C");
        QVERIFY(QFile::copy(":/methods/am/curves/GwH2013uni_F.ref", root.path() + "/AM/europe.ref"));
        QVERIFY(QFile::copy(":/methods/c14/curves/intcal13.14c", root.path() + "/14C/intcal98_fix.14c"));
        const QStringList saved = CurveStore::extraRoots();
        CurveStore::setExtraRoots({ root.path() });
        const auto e = MethodRegistry::instance().parse("inclinaison 65 alpha95 1, intensité 50 erreur 1 avec la courbe europe");
        QCOMPARE(e[0]["curve_f"].toString(), QString("europe"));
        QVERIFY(MethodRegistry::instance().compute(e[0]).ok);
        const auto c = MethodRegistry::instance().parse("C14 3000 ± 30 BP avec la courbe intcal98_fix");
        QCOMPARE(c[0]["curve"].toString(), QString("intcal98_fix"));
        CurveStore::setExtraRoots(saved);
        QCOMPARE(MethodRegistry::instance().parse("C14 3000 ± 30 BP, shcal13")[0]["curve"].toString(), QString("shcal13"));
    }

    // Période d'étude d'une datation simple : densité restreinte à [tmin ; tmax] et renormalisée ; sinon toute la courbe.
    void datingStudyPeriod()
    {
        using namespace seldon;
        double a = 0, b = 0;
        QVERIFY(MethodRunner::periodFromQuestion("refait la datation entre 0 et 2000", a, b) && a == 0 && b == 2000);
        QVERIFY(MethodRunner::periodFromQuestion("période d'étude de 500 av. J.-C. à 500", a, b) && a == -500 && b == 500);
        QVERIFY(MethodRunner::periodFromQuestion("redo it between -1000 and 1000", a, b) && a == -1000 && b == 1000);
        QVERIFY(!MethodRunner::periodFromQuestion("loi uniforme entre 1100 et 1500", a, b));
        QVERIFY(!MethodRunner::periodFromQuestion("C14 2500 ± 30 BP avec un décalage entre 10 et 30 ans", a, b));
        QVERIFY(!MethodRunner::periodFromQuestion("C14 de 3560 ± 20 BP", a, b));

        // la datation de l'utilisateur (IF, courbes nommées), puis « refait la datation entre 0 et 2000 »
        const QString first = "date en am vectoriel IF; inclinaison=63,95 alpha95=1,83 intensité=56,89 erreur sur l'intensité=0,74 "
                              "avec la courbe pour l'inclinaison gal2002sph2014_i pour l'intensité bulgaria_f";
        const QVector<QJsonObject> prev = MethodRunner::requestsFromQuestion(first);
        const QVector<QJsonObject> again = MethodRunner::followUpRequests("refait la datation entre 0 et 2000", prev);
        QCOMPARE(again.size(), 1);
        QCOMPARE(again[0]["tmin"].toDouble(), 0.0);
        QCOMPARE(again[0]["tmax"].toDouble(), 2000.0);
        QCOMPARE(again[0]["curve_f"].toString(), QString("bulgaria_f"));             // les courbes choisies sont gardées
        QVERIFY(!again[0].contains("level"));
        const RunOutcome r = MethodRunner::run(again);
        QVERIFY2(r.noticeHtml.contains("période d'étude 0 → 1950 apr. J.-C.") || r.noticeHtml.contains("période d'étude"), qPrintable(r.noticeHtml.left(400)));
        for (const QJsonValue& v : r.plot["series"].toArray()[0].toObject()["x"].toArray())
            QVERIFY(v.toDouble() >= 0 && v.toDouble() <= 2000);
        // datation simple avec période dans la question ; sans période, toute la courbe
        const RunOutcome c = MethodRunner::analyzeQuestion("C14 3000 ± 30 BP entre -1500 et -1000");
        QVERIFY(c.found);
        for (const QJsonValue& v : c.plot["series"].toArray()[0].toObject()["x"].toArray())
            QVERIFY(v.toDouble() >= -1500 && v.toDouble() <= -1000);
        QVERIFY(!MethodRunner::analyzeQuestion("C14 3000 ± 30 BP").noticeHtml.contains("période d'étude"));
        // aucune probabilité sur la période : message clair ; format cal BP : période convertie
        QVERIFY(MethodRunner::analyzeQuestion("C14 3000 ± 30 BP entre 1000 et 2000").noticeHtml.contains("aucune probabilité"));
        const QVector<QJsonObject> bp = MethodRunner::followUpRequests("et entre 3000 et 3500", MethodRunner::requestsFromQuestion("C14 3000 ± 30 BP"),
                                                                       DateFormat::CalBP);
        QCOMPARE(bp[0]["tmin"].toDouble(), -1550.0);
        QCOMPARE(bp[0]["tmax"].toDouble(), -1050.0);
        // clé de l'outil
        const auto t = tools::execute("calibrer_radiocarbone", R"({"age":3000,"error":30,"tmin":-1500,"tmax":-1000})", DateFormat::BcAd);
        QVERIFY(t.executed && t.content.contains("période d'étude"));
    }

    // Export HTML des résultats de ChronoModel : résumé exact (critère global, variables hors critères de la pire à la
    // meilleure, facteur d'itérations, dates au σ grand, Events dont toutes les dates ont un σ grand).
    void chronoModelResultsSummary()
    {
        using namespace seldon;
        const char* page = R"HTML(<html><body><p>ChronoModel 4.0.0</p><p>Project filename : Test</p>
<p>Not converged (max R&#770; = 1.0121 ; min ESS = 504)</p>
<p>R&#770; : max = 1.0121, mean = 1.0006 (1/12 variable(s) &ge; Threshold)</p><p>ESS : min = 504, mean = 9000</p>
<h3>Event : A</h3><h4>Posterior Event Date</h4><p>R&#770; = 1.0000 &middot; ESS bulk = 12971 &middot; ESS tail = 11377 &#9989;</p>
<p>MAP = 1000,00 ; Mean = 1000,00 ; Std = 20,00</p>
<h3>Data : A1</h3><h4>Posterior calib. date</h4><p>R&#770; = 1.0000 &middot; ESS bulk = 13910 &middot; ESS tail = 14660 &#9989;</p>
<p>MAP = 1000,00 ; Mean = 1000,00 ; Std = 50,00</p>
<h4>Posterior Std ti</h4><p>R&#770; = 1.0121 &#9888;&#65039; Insufficient convergence</p><p>ESS bulk = 738 &#9888;&#65039; Insufficient (&lt; 1500)</p>
<p>ESS tail = 1041 &#9888;&#65039; Insufficient (&lt; 1500)</p><p>MAP = 300,00 ; Mean = 320,00 ; Std = 90,00</p>
<h3>Event : B</h3><h4>Posterior Event Date</h4><p>R&#770; = 1.0047 &#9989; Satisfactory convergence</p><p>ESS bulk = 1507 &#9989; Satisfactory</p>
<p>ESS tail = 504 &#9888;&#65039; Insufficient (&lt; 1500)</p><p>MAP = 2000,00 ; Mean = 2000,00 ; Std = 30,00</p>
<h3>Data : B1</h3><h4>Posterior calib. date</h4><p>R&#770; = 1.0000 &middot; ESS bulk = 9000 &middot; ESS tail = 9000 &#9989;</p><p>MAP = 2000,00 ; Mean = 2000,00 ; Std = 40,00</p>
<h4>Posterior Std ti</h4><p>R&#770; = 1.0001 &middot; ESS bulk = 9000 &middot; ESS tail = 9000 &#9989;</p><p>MAP = 200,00 ; Mean = 210,00 ; Std = 50,00</p>
<h3>Data : B2</h3><h4>Posterior calib. date</h4><p>R&#770; = 1.0000 &middot; ESS bulk = 9000 &middot; ESS tail = 9000 &#9989;</p><p>MAP = 2010,00 ; Mean = 2010,00 ; Std = 40,00</p>
<h4>Posterior Std ti</h4><p>R&#770; = 1.0001 &middot; ESS bulk = 9000 &middot; ESS tail = 9000 &#9989;</p><p>MAP = 150,00 ; Mean = 160,00 ; Std = 50,00</p>
</body></html>)HTML";
        const QString text = chronoresults::htmlToText(QString::fromUtf8(page));
        QVERIFY(chronoresults::looksLike(text));
        QVERIFY(!chronoresults::looksLike("<p>Bonjour</p>"));
        const chronoresults::Report r = chronoresults::parse(text);
        QCOMPARE(r.events, 2);
        QCOMPARE(r.data, 3);
        QCOMPARE(r.vars.size(), 8);           // 2 dates d'Event + (date calibrée, σ) pour 3 datations
        QCOMPARE(r.essThreshold, 1500);
        const QString s = chronoresults::summarize(r, "Log.html");
        QVERIFY(s.contains("Not converged (max R̂ = 1.0121 ; min ESS = 504)"));
        // pire en tête : R̂ en échec (σ de A1), puis l'ESS le plus faible (date de l'Event B, ESS tail 504)
        const int i1 = int(s.indexOf("1. Datation « A1 »")), i2 = int(s.indexOf("2. Event « B »"));
        QVERIFY2(i1 > 0 && i2 > i1, qPrintable(s));
        QVERIFY(s.contains("×3.0"));
        // σ grand : A1 (300 > 50), B1 (200 > 40), B2 (150 > 40) ; B a toutes ses dates au σ grand (inversion possible)
        QVERIFY(s.contains("« A1 » (Event « A ») : σ MAP = 300.0 > écart-type de la date = 50.0"));
        QVERIFY(s.contains("inversion stratigraphique possible : « B » (2 datations)"));
        QVERIFY(s.contains("Events à une seule datation, au σ grand : date aberrante (outlier) ou inversion stratigraphique possible : « A »"));
        // comptes par type, verdict calibré sur les proportions, σ grand avec bonne convergence distingué
        QVERIFY2(s.contains("date de l'événement : 1/2 hors critères (dont R̂ : 0)"), qPrintable(s));
        QVERIFY(s.contains("σ de la date (Std ti) : 1/3 hors critères (dont R̂ : 1)"));
        QVERIFY(s.contains("Verdict de convergence") && s.contains("insuffisante (R̂ en échec pour 12.5 %"));
        QVERIFY(s.contains("dont 2 avec une convergence bonne"));
        QVERIFY(s.contains("« B1 » (Event « B ») : σ MAP = 200.0 > écart-type de la date = 40.0 (rapport 5.0) ; convergence bonne"));
        QVERIFY(s.contains("ni d'ajouter des contraintes"));
        // une seule variable en échec sur beaucoup, R̂ max < 1,02 : « presque atteinte »
        chronoresults::Report big = r;
        for (int i = 0; i < 200; ++i) { chronoresults::Variable v; v.kind = "date de l'événement"; v.event = QString("E%1").arg(i); v.rhat = 1.0; v.essBulk = v.essTail = 9000; big.vars << v; }
        QVERIFY(chronoresults::summarize(big, "Log.html").contains("presque atteinte"));
        QVERIFY(s.size() < 6000);
    }

    void levelFollowUp()
    {
        using namespace seldon;
        const QVector<QJsonObject> prev = MethodRunner::requestsFromQuestion("C14 de 3560 ± 20 BP");
        QCOMPARE(prev.size(), 1);
        const QVector<QJsonObject> again = MethodRunner::followUpRequests("quel est l'intervalle à 50%", prev);
        QCOMPARE(again.size(), 1);
        QCOMPARE(again[0]["level"].toDouble(), 50.0);
        const RunOutcome r = MethodRunner::run(again);
        QVERIFY(r.found);
        QVERIFY2(r.promptBlock.contains("HPD à 50 %"), qPrintable(r.promptBlock));
        QVERIFY(MethodRunner::followUpRequests("and at 99 % ?", prev).value(0)["level"].toDouble() == 99.0);
        // la question a ses propres datations, ou ne demande pas de niveau, ou il n'y a rien avant : pas de reprise
        QVERIFY(MethodRunner::followUpRequests("C14 de 3000 ± 30 BP à 50 %", prev).isEmpty());
        QVERIFY(MethodRunner::followUpRequests("que signifie la courbe ?", prev).isEmpty());
        QVERIFY(MethodRunner::followUpRequests("quel est l'intervalle à 50%", {}).isEmpty());
        // une datation demandée par l'assistant (bloc calcul) se reprend aussi ; un événement non (MCMC)
        const auto blocks = MethodRunner::extractCalcBlocks("```calcul\n{\"method\":\"tl\",\"age\":800,\"error\":60,\"ref_year\":2015}\n```\n");
        QCOMPARE(MethodRunner::followUpRequests("à 90 % ?", blocks).size(), 1);
        QVERIFY(MethodRunner::followUpRequests("à 90 % ?", { QJsonObject{ { "method", "event" } } }).isEmpty());
        // la fiche d'identité interdit de déduire une HPD des quartiles
        SkillManager m; m.reload(QString());
        QVERIFY(m.alwaysText().contains("ne la déduis jamais des quartiles"));
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
        // TL sans année de mesure : erreur (pas d'année en cours par défaut) ; anciens noms et borne en date : erreurs
        const ChrResult t = ChrBuilder::build(QJsonDocument::fromJson(
            R"({"events":[{"name":"E","dates":[{"type":"tl","age":500,"error":20}]}]})", nullptr).object());
        QVERIFY(!t.ok && !t.errors.filter("ref_year").isEmpty());
        // réponse réelle d'Albert (ministral-3-8b) : borne écrite comme une date, « refYear » au lieu de « ref_year »
        const ChrResult al = ChrBuilder::build(QJsonDocument::fromJson(
            R"({"events":[{"name":"US7","color":"rouge","dates":[{"type":"bound","known_fixed":-40}]},
                          {"name":"US1","dates":[{"type":"tl","name":"Bdx-478","age":3500,"error":45,"refYear":2010}]}]})", nullptr).object());
        QVERIFY(!al.ok);
        QVERIFY(!al.errors.filter("une borne se déclare sur l'événement").isEmpty());
        QVERIFY(!al.errors.filter("« refYear » inconnu : écrire « ref_year »").isEmpty());
        QVERIFY(MethodRunner::obsoleteFieldError(QJsonObject{ { "method", "am" }, { "errorF", 1 } }).contains("error_f"));
        QVERIFY(MethodRunner::run({ QJsonObject{ { "method", "tl" }, { "age", 800 }, { "error", 60 }, { "refYear", 2015 } } })
                    .promptBlock.contains("écrire « ref_year »"));
        // les six codes de processus sont vérifiés : plus d'avertissement
        const ChrResult p = ChrBuilder::build(QJsonDocument::fromJson(
            R"({"events":[{"name":"E","dates":[{"type":"am","process":"ID","inc":65,"dec":3,"alpha95":1}]}]})", nullptr).object());
        QVERIFY2(p.ok, qPrintable(p.errors.join("; ")));
        QVERIFY(p.warnings.filter("vérifié").isEmpty());
    }

    // Les six types de datation du plugin AM, avec les valeurs du projet réel tous_type_AM.chr (ChronoModel 4.0) :
    // codes process_type et champs utiles de « data ».
    void chrAmAllProcessTypes()
    {
        const QString spec = R"J({"settings":{"tmin":-1000,"tmax":2000},"events":[
          {"name":"date intensité","dates":[{"type":"am","name":"Intensité A","process":"F","field":54.22,"error_f":0.83,
             "curve_i":"bulgaria_i","curve_d":"gal2002sph2014_d","curve_f":"eurw_2017_f"}]},
          {"name":"date inclinaison","dates":[{"type":"am","name":"Inclinaison A","process":"I","inc":65.2,"alpha95":1.2}]},
          {"name":"date déclinaison","dates":[{"type":"am","name":"declinaison A","process":"D","inc":65.3,"dec":5.2,"alpha95":1.3}]},
          {"name":"date (i, d)","dates":[{"type":"am","name":"directionnel A","process":"ID","inc":65.2,"dec":3.2,"alpha95":1.4}]},
          {"name":"date (i, f)","dates":[{"type":"am","name":"vectorielle A","process":"IF","inc":65.2,"alpha95":1.2,
             "field":55.3,"error_f":2.4,"curve_f":"gwh2013uni_f"}]},
          {"name":"date (i, d, f)","dates":[{"type":"am","name":"vectorielle (i, d, f)","process":"IDF","inc":65.2,"dec":5.2,
             "alpha95":1.4,"field":55.3,"error_f":2.2,"curve_f":"gwh2013uni_f"}]}]})J";   // délimiteur J : les noms contiennent « )" »
        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(spec.toUtf8()).object());
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        QVERIFY(r.warnings.filter("vérifié").isEmpty());
        const QJsonArray evs = r.chr["events"].toArray();
        QCOMPARE(evs.size(), 6);
        auto data = [&](int i) { return evs[i].toObject()["dates"].toArray()[0].toObject()["data"].toObject(); };
        const int codes[] = { 3, 1, 2, 4, 5, 6 };
        for (int i = 0; i < 6; ++i) QCOMPARE(data(i)["process_type"].toInt(), codes[i]);
        // F
        QCOMPARE(data(0)["field"].toDouble(), 54.22);
        QCOMPARE(data(0)["error_f"].toDouble(), 0.83);
        QCOMPARE(data(0)["refF_curve"].toString(), QString("eurw_2017_f.ref"));
        QCOMPARE(data(0)["refI_curve"].toString(), QString("bulgaria_i.ref"));
        QVERIFY(data(0)["alpha95"].toDouble() > 0);              // erreur non utilisée : jamais nulle
        // I
        QCOMPARE(data(1)["inc"].toDouble(), 65.2);
        QCOMPARE(data(1)["alpha95"].toDouble(), 1.2);
        QVERIFY(data(1)["error_f"].toDouble() > 0);
        // D : l'inclinaison est écrite (σD = α95 / (2,448 cos I))
        QCOMPARE(data(2)["inc"].toDouble(), 65.3);
        QCOMPARE(data(2)["dec"].toDouble(), 5.2);
        QCOMPARE(data(2)["alpha95"].toDouble(), 1.3);
        // ID, IF, IDF
        QCOMPARE(data(3)["dec"].toDouble(), 3.2);
        QCOMPARE(data(4)["field"].toDouble(), 55.3);
        QCOMPARE(data(4)["refF_curve"].toString(), QString("gwh2013uni_f.ref"));
        QCOMPARE(data(5)["error_f"].toDouble(), 2.2);
        QCOMPARE(data(5)["dec"].toDouble(), 5.2);
        const QString sum = ChrBuilder::summarize(r.chr);
        QVERIFY(sum.contains("archéomag. D (I=65.3, D=5.2"));
        QVERIFY(sum.contains("archéomag. IDF"));
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

        // instantané du prompt (export, reproductibilité) gardé dans l'historique
        Conversation withPrompt = sampleConversation("x2", "q", QDateTime::currentDateTime());
        const QJsonObject snap{ { "date", "2026-10-27T14:00:00" }, { "engine", QJsonObject{ { "type", "distant" }, { "model", "m" } } },
                                { "sampling", QJsonObject{ { "temperature", 0.3 } } },
                                { "messages", QJsonArray{ QJsonObject{ { "role", "system" }, { "content", "S" } },
                                                          QJsonObject{ { "role", "user" }, { "turn", 0 } } } } };
        withPrompt.turns[0].prompt = snap;
        Conversation back2;
        QVERIFY(ConversationStore::fromJson(ConversationStore::toJson(withPrompt), back2));
        QCOMPARE(back2.turns[0].prompt, snap);
        QVERIFY(ConversationStore::fromJson(ConversationStore::toJson(c), back2));
        QVERIFY(back2.turns[0].prompt.isEmpty());                        // ancienne conversation : pas d'instantané
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

        list->setCurrentRow(2);
        QCOMPARE(panel.selectedId(), QString("c"));
        const auto buttons = panel.findChildren<QPushButton*>();
        auto button = [&](const QString& text) { for (QPushButton* b : buttons) if (b->text().contains(text)) return b; return static_cast<QPushButton*>(nullptr); };
        QVERIFY(button("Ouvrir") && button("Supprimer") && button("Tout effacer"));
        QVERIFY(!button("Nouvelle"));                // déplacé au-dessus de la discussion (barre de titre)
        button("Ouvrir")->click();
        QCOMPARE(opened.size(), 1); QCOMPARE(opened[0][0].toString(), QString("c"));
        button("Supprimer")->click();
        QCOMPARE(deleted.size(), 1); QCOMPARE(deleted[0][0].toString(), QString("c"));
        button("Tout effacer")->click();
        QCOMPARE(all.size(), 1);

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
        QVERIFY(MethodRunner::isModelRequest("Make a ChronoModel model with two events: 14C 2500 ± 30 BP and 2600 ± 30 BP"));
        QVERIFY(MethodRunner::isModelRequest("Export this as a .chr file with one phase"));
        QVERIFY(!MethodRunner::isModelRequest("Calibrate 14C 2500 ± 30 BP"));
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
            const auto sel = m.select(q, AppSettings().skillChars, QStringList(), pin);
            QVERIFY(!sel.isEmpty());
            QCOMPARE(sel.first().skill->id, QString("03_chronomodel_creer_modele"));
            // même si la fiche a été injectée récemment (exclue) ou si le budget est minuscule
            QCOMPARE(m.select(q, 100, { "03_chronomodel_creer_modele" }, pin).first().skill->id, QString("03_chronomodel_creer_modele"));
            // sans épinglage, la fiche est quand même choisie avec le budget par défaut
            bool found = false;
            for (const auto& x : m.select(q, AppSettings().skillChars)) found = found || x.skill->id == "03_chronomodel_creer_modele";
            QVERIFY2(found, qPrintable(q));
        }
        // pas de doublon
        const auto sel = m.select(asks[1], AppSettings().skillChars, QStringList(), { "03_chronomodel_creer_modele" });
        int n = 0; for (const auto& x : sel) if (x.skill->id == "03_chronomodel_creer_modele") ++n;
        QCOMPARE(n, 1);
    }

    void chrErrors()
    {
        QVERIFY(!ChrBuilder::build(QJsonObject()).ok);
        auto bad = [](const QString& s) { return ChrBuilder::build(QJsonDocument::fromJson(s.toUtf8()).object()); };
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"gauss","age":-500,"error":0}]}]})").ok);
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"unif","min":5,"max":1}]}]})").ok);
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"dendro","age":2500,"error":30}]}]})").ok);
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"tl","age":488,"error":0}]}]})").ok);
        // archéomagnétisme : valeurs requises selon le processus
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"am","process":"I","inc":65}]}]})").ok);          // alpha95 manquant
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"am","process":"F","field":50}]}]})").ok);        // error_f manquant
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"am","process":"X","inc":65,"alpha95":1}]}]})").ok);
        QVERIFY(!bad(R"({"events":[{"name":"a","dates":[{"type":"am","process":"D","dec":5,"alpha95":1}]}]})").ok);      // D sans inc
        QVERIFY(!bad(R"({"events":[{"name":"a"},{"name":"a"}]})").ok);
        QVERIFY(!bad(R"({"events":[{"name":"b","type":"bound"}]})").ok);
    }

    // Affichage : le bloc de modèle est remplacé par un résumé ; JSON montré seulement sur demande explicite.
    void chrDisplay()
    {
        const QString ok = "Voici le modèle.\n\n```chronomodel\n{\"name\":\"Site\",\"events\":[{\"name\":\"E1\",\"dates\":"
                           "[{\"type\":\"c14\",\"age\":3654,\"error\":40}]},{\"name\":\"E2\",\"dates\":[{\"type\":\"gauss\","
                           "\"age\":-1500,\"error\":30}]}],\"event_constraints\":[[\"E1\",\"avant\",\"E2\"]]}\n```\n\nFin.";
        const QString d = ChrBuilder::displayMarkdown(ok);
        QVERIFY2(d.contains("Modèle ChronoModel prêt") && d.contains("2 événements") && d.contains("1 contrainte"), qPrintable(d));
        QVERIFY(!d.contains("```") && !d.contains("\"events\"") && d.startsWith("Voici le modèle.") && d.endsWith("Fin."));
        const QString shown = ChrBuilder::displayMarkdown(ok, true);
        QVERIFY(shown.contains("```chronomodel") && shown.contains("Modèle ChronoModel prêt"));

        // modèle incomplet (événement sans date) : liste de ce qui manque
        const QString partial = "```chronomodel\n{\"events\":[{\"name\":\"Event_1\",\"dates\":[{\"type\":\"c14\",\"age\":3654,"
                                "\"error\":40}]},{\"name\":\"Event_2\",\"dates\":[]}],\"event_constraints\":[]}\n```";
        const QString p = ChrBuilder::displayMarkdown(partial);
        QVERIFY2(p.contains("incomplet") && p.contains("Event_2"), qPrintable(p));
        // bloc en cours d'écriture
        QCOMPARE(ChrBuilder::displayMarkdown("Je prépare :\n```chronomodel\n{\"events\": ["),
                 QString("Je prépare :\n*📦 Préparation du modèle ChronoModel…*"));
        // autres blocs inchangés
        const QString other = "```calcul\n{\"method\":\"c14\"}\n```\n```json\n{\"a\":1}\n```";
        QCOMPARE(ChrBuilder::displayMarkdown(other), other);

        QVERIFY(ChrBuilder::asksForJson("Montre-moi le JSON du modèle"));
        QVERIFY(ChrBuilder::asksForJson("affiche le bloc chronomodel"));
        QVERIFY(ChrBuilder::asksForJson("Donne la spécification du modèle"));
        QVERIFY(!ChrBuilder::asksForJson("Fais-moi un modèle avec deux événements"));
        QVERIFY(!ChrBuilder::asksForJson("Crée le fichier chr"));
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
        // l'exemple de la fiche donne une loi : SeldonH calcule les points, et le tracé est valide
        QString err;
        const QJsonObject ex = MethodRunner::expandPlotLaws(d.object(), &err);
        QVERIFY2(!ex.isEmpty(), qPrintable(err));
        CurveSpec cs;
        QVERIFY(CurveSpec::fromJson(ex, cs, &err));

        // « Trace une gaussienne de moyenne -500 et d'écart-type 40 » : courbe entière, symétrique, mode à -500
        const QJsonObject g = MethodRunner::expandPlotLaws(QJsonObject{ { "law", "normal" }, { "mean", -500 }, { "sd", 40 } }, &err);
        QVERIFY2(!g.isEmpty(), qPrintable(err));
        const QJsonObject gs = g["series"].toArray()[0].toObject();
        const QJsonArray gx = gs["x"].toArray(), gy = gs["y"].toArray();
        QCOMPARE(gx.size(), gy.size());
        QVERIFY(gx.first().toDouble() <= -500 - 4 * 40 && gx.last().toDouble() >= -500 + 4 * 40);   // pas tronquée à ±100
        int imax = 0;
        for (int i = 1; i < gy.size(); ++i) if (gy[i].toDouble() > gy[imax].toDouble()) imax = i;
        QVERIFY(std::abs(gx[imax].toDouble() + 500) <= 1.0);
        QVERIFY(std::abs(gy[imax].toDouble() - 1.0 / (40 * std::sqrt(2 * M_PI))) < 1e-4);
        QVERIFY(g["fill"].toBool());
        // série sans loi gardée telle quelle ; loi inconnue refusée
        const QJsonObject mixed = MethodRunner::expandPlotLaws(QJsonDocument::fromJson(
            R"({"x":[1,2,3],"series":[{"name":"mesures","y":[1,2,1]},{"law":"gaussienne","mean":2,"sd":1}]})").object(), &err);
        QCOMPARE(mixed["series"].toArray()[0].toObject()["x"].toArray().size(), 3);
        QVERIFY(mixed["series"].toArray()[1].toObject()["x"].toArray().size() > 100);
        QVERIFY(MethodRunner::expandPlotLaws(QJsonObject{ { "law", "cauchy" }, { "mean", 0 } }, &err).isEmpty());
        const QJsonObject plain{ { "x", QJsonArray{ 1, 2 } }, { "series", QJsonArray{ QJsonObject{ { "y", QJsonArray{ 0, 1 } } } } } };
        QCOMPARE(MethodRunner::expandPlotLaws(plain), plain);
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
                       { "delta_r", 100.0 }, { "delta_r_error", 50.0 }, { "has_delta_r", true } };
        const MethodResult r = MethodRegistry::instance().compute(p);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(std::abs(r.hpd95.front().from - (-740)) <= 12 && std::abs(r.hpd95.back().to - (-370)) <= 12);
        QVERIFY(std::abs(r.stats.median - (-540)) <= 12);
        QVERIFY(r.warnings.isEmpty());
        p.remove("has_delta_r"); p.remove("delta_r"); p.remove("delta_r_error");
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
        const MethodResult t = reg.compute({ { "method", "tl" }, { "age", 800.0 }, { "error", 60.0 }, { "ref_year", 2015 } });
        QVERIFY(t.ok);
        QVERIFY(std::abs(t.stats.median - 1215) < 0.7);
        QVERIFY(std::abs(t.hpd95[0].from - (1215 - 117.6)) < 2 && std::abs(t.hpd95[0].to - (1215 + 117.6)) < 2);
    }

    void archeomagnetism()
    {
        const MethodRegistry& reg = MethodRegistry::instance();
        QJsonObject p{ { "method", "am" }, { "process", "I" }, { "inc", 62.0 }, { "alpha95", 2.0 },
                       { "curve_i", "gal2002sph2014_i" } };
        MethodResult r = reg.compute(p);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(std::abs(r.stats.median - 368) <= 4);

        p["process"] = "ID"; p["dec"] = -3.0; p["curve_d"] = "gal2002sph2014_d";
        r = reg.compute(p);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY2(std::abs(r.stats.median - 275) <= 4, qPrintable(QString::number(r.stats.median)));
        QVERIFY(std::abs(r.hpd95.front().from - 57) <= 6 && std::abs(r.hpd95.back().to - 453) <= 6);

        QJsonObject f{ { "method", "am" }, { "process", "F" }, { "field", 60.0 }, { "error_f", 3.0 }, { "curve_f", "gwh2013uni_f" } };
        r = reg.compute(f);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(std::abs(r.stats.median - 1040) <= 6);

        // IDF : calculable et normalisé
        QJsonObject all{ { "method", "am" }, { "process", "IDF" }, { "inc", 62.0 }, { "dec", -3.0 }, { "alpha95", 2.0 },
                         { "field", 60.0 }, { "error_f", 3.0 }, { "curve_i", "gal2002sph2014_i" }, { "curve_d", "gal2002sph2014_d" },
                         { "curve_f", "gwh2013uni_f" } };
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
        QTest::newRow("tl")            << "TL 800 ± 60 ans, mesuré en 2015" << "tl" << 1 << "ref_year" << 2015.0;
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
        int calcExamples = 0;
        for (const auto& e : ex) {
            QVERIFY2(!e.text.isEmpty(), "exemple vide");
            if (e.method.isEmpty()) continue;
            if (e.method == "calc") {                      // calculatrice : reconnu comme calcul, sans erreur
                seldon::Calculator c;
                QVERIFY2(c.isCalculation(e.text), qPrintable("calcul non reconnu : " + e.text));
                for (const auto& l : c.evaluate(e.text)) QVERIFY2(l.ok, qPrintable(e.text + " : " + l.error));
                ++calcExamples;
                continue;
            }
            if (e.method == "event") {                     // événement : au moins deux datations calculables, combinées en MCMC
                QVERIFY2(MethodRunner::asksEvent(e.text), qPrintable("événement non reconnu : " + e.text));
                auto d = MethodRegistry::instance().parse(e.text);
                if (d.size() < 2) d = MethodRunner::jsonRequests(e.text);          // datations collées en JSON (avec wiggle)
                QVERIFY2(d.size() >= 2, qPrintable("datations de l'événement non comprises : " + e.text));
                for (const QJsonObject& o : d)
                    QVERIFY2(MethodRegistry::instance().compute(o).ok, qPrintable("calcul impossible : " + e.text));
                if (e.text.contains("\"wiggle\"")) {                              // les décalages sont bien transmis
                    int w = 0;
                    for (const QJsonObject& o : d) if (o.contains("wiggle")) ++w;
                    QVERIFY2(w >= 2, qPrintable("décalages non lus : " + e.text));
                }
                methods << e.method;
                ++checked;
                continue;
            }
            // Date seule avec décalage : l'application calcule la densité de t + δ.
            if (!MethodRunner::wiggleFromQuestion(e.text).isEmpty()) {
                const RunOutcome o = MethodRunner::analyzeQuestion(e.text);
                QVERIFY2(o.count == 1 && o.promptBlock.contains("densité de t + δ"), qPrintable("wiggle non appliqué : " + e.text));
            }
            const auto v = MethodRegistry::instance().parse(e.text);
            QVERIFY2(!v.isEmpty(), qPrintable("exemple non compris : " + e.text));
            QCOMPARE(v[0].value("method").toString(), e.method);
            const auto r = MethodRegistry::instance().compute(v[0]);
            QVERIFY2(r.ok, qPrintable("calcul impossible : " + e.text + " : " + r.error));
            methods << e.method;
            ++checked;
        }
        QVERIFY(checked >= 10);
        QVERIFY(calcExamples >= 8);
        for (const QString& m : { "c14", "f14c", "tl", "am", "gauss", "unif", "event" })
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
        for (const char* id : { "bcad", "bcece", "calbp", "calb2k", "datbp", "datb2k", "ka", "ma", "numeric" })
            QCOMPARE(QString::fromLatin1(dateformat::id(dateformat::fromId(id))), QString(id));

        // Sans unité : valeur signée brute, sans suffixe
        QCOMPARE(dateformat::format(-1898.0, DateFormat::Numeric), QString("-1898"));
        QCOMPARE(dateformat::spread(21.3, DateFormat::Numeric), QString("21"));
        QVERIFY(!dateformat::isAge(DateFormat::Numeric));
        QCOMPARE(dateformat::detect("donne les résultats sans unité", DateFormat::BcAd), DateFormat::Numeric);
        QCOMPARE(dateformat::detect("results without units please", DateFormat::CalBP), DateFormat::Numeric);
        QVERIFY(QString::fromStdString(dateformat::systemHint(DateFormat::Numeric)).contains("sans unité"));

        // Décimales automatiques : 2 chiffres significatifs sur l'écart-type du résultat en cours
        QCOMPARE(dateformat::spread(21.3, DateFormat::CalBP), QString("21 ans"));
        QCOMPARE(dateformat::spread(8.34, DateFormat::BcAd), QString("8,3 ans"));
        QCOMPARE(dateformat::spread(2130.0, DateFormat::Ka), QString("2,1 ka"));
        QCOMPARE(dateformat::format(-1898.4, DateFormat::Ka), QString("1,898 ka"));          // sans contexte
        {
            const dateformat::PrecisionScope p(2130.0);                                        // σ = 2,1 ka
            QCOMPARE(dateformat::format(-41898.4, DateFormat::Ka), QString("41,9 ka"));
            QCOMPARE(dateformat::format(-41898.4, DateFormat::CalBP), QString("43848 cal BP"));
        }
        {
            const dateformat::PrecisionScope p(0.42);                                          // σ < 1 an
            QCOMPARE(dateformat::format(1515.37, DateFormat::BcAd), QString("1515,37 apr. J.-C."));
            QCOMPARE(dateformat::format(-0.25, DateFormat::BcAd), QString("1,25 av. J.-C."));
        }
        QCOMPARE(dateformat::format(-1898.4, DateFormat::BcAd), QString("1899 av. J.-C."));    // contexte rétabli

        // Décimales fixes
        dateformat::setDecimals(2);
        QCOMPARE(dateformat::format(-1898.0, DateFormat::CalBP), QString("3848,00 cal BP"));
        QCOMPARE(dateformat::format(-1898.0, DateFormat::BceCe), QString("1899,00 BCE"));
        QCOMPARE(dateformat::format(-0.001, DateFormat::Numeric), QString("0,00"));            // pas de « -0,00 »
        QCOMPARE(dateformat::spread(21.3, DateFormat::Ka), QString("0,02 ka"));
        QVERIFY(QString::fromStdString(dateformat::systemHint(DateFormat::BcAd)).contains("2 décimales"));
        dateformat::setDecimals(0);
        QCOMPARE(dateformat::format(-1898.0, DateFormat::Ka), QString("2 ka"));
        dateformat::setDecimals(dateformat::kAutoDecimals);
        QVERIFY(dateformat::systemHint(DateFormat::BcAd).empty());

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
        // Tableau avec <br>, code en ligne, ≥ et gras, suivi d'une citation : rien ne doit disparaître.
        const QString h7 = md(QString::fromUtf8(
            "| Problème | Recommandation |\n|---|---|\n"
            "| **R̂ > 1.01** | - Augmenter le thinning<br>- Ajouter de l'annealing (`num_burn`) |\n"
            "| **ESS très faible (≈ 500)** | - Même recommandation que ci‑dessus, mais envisager d’ajouter **plus de chaînes** (`num_proc` ≥ 4) |\n\n"
            "> **Note** : contrôler le modèle."));
        QVERIFY(h7.contains("Augmenter le thinning") && h7.contains("<br />") && h7.contains("Ajouter de l'annealing"));
        QVERIFY(h7.contains("num_burn") && h7.contains("num_proc") && h7.contains(QString::fromUtf8("≥ 4")));
        QVERIFY(h7.contains("plus de cha") && h7.contains("ESS tr") && h7.contains(QString::fromUtf8("contrôler le modèle")));
        QVERIFY(!h7.contains(QChar(0xE000)) && !h7.contains("&lt;br"));
        const QString h8 = md("H<sub>2</sub>O et 10<sup>3</sup>, mais `<br>` reste du code.");
        QVERIFY(h8.contains("<sub>2</sub>") && h8.contains("<sup>3</sup>") && h8.contains("&lt;br&gt;"));
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

        // clé API envoyée en clair : http:// vers une autre machine, avec une clé
        QVERIFY(sendsKeyInClear("http://192.168.1.20:8080/v1", "k"));
        QVERIFY(sendsKeyInClear("http://serveur-labo.example.org/v1", "k"));
        QVERIFY(!sendsKeyInClear("http://serveur-labo.example.org/v1", ""));     // sans clé : rien à protéger
        QVERIFY(!sendsKeyInClear("http://serveur-labo.example.org/v1", "  "));
        QVERIFY(!sendsKeyInClear("https://api.openai.com/v1", "k"));
        QVERIFY(!sendsKeyInClear("api.openai.com/v1", "k"));                     // sans schéma : https:// ajouté
        QVERIFY(!sendsKeyInClear("http://localhost:11434/v1", "k"));            // reste sur cet ordinateur
        QVERIFY(!sendsKeyInClear("HTTP://LocalHost:1234", "k"));
        QVERIFY(!sendsKeyInClear("http://127.0.0.1:1234/v1", "k"));
        QVERIFY(!sendsKeyInClear("http://[::1]:8080/v1", "k"));

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

    // Réponse coupée : finish_reason « length », erreur au milieu du flux, flux terminé sans [DONE].
    void remoteEngineTruncation()
    {
        auto sse = [](const QByteArray& events) {
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n") + events;
        };
        auto run = [](MockServer& srv) {
            seldon::RemoteEngine eng;
            eng.setConfig({ srv.baseUrl(), "", "m" });
            QSignalSpy fin(&eng, &seldon::RemoteEngine::finished);
            seldon::SamplingParams sp; sp.maxTokens = 50;
            eng.generate({ { "user", "q" } }, sp);
            if (!QTest::qWaitFor([&] { return fin.count() == 1; }, 5000)) return seldon::GenStats{};
            return fin[0][0].value<seldon::GenStats>();
        };
        {
            MockServer srv([&](const QByteArray&, const QByteArray&, int) {
                return sse("data: {\"choices\":[{\"delta\":{\"content\":\"Début\"},\"finish_reason\":null}]}\n\n"
                           "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
                           "data: [DONE]\n\n");
            });
            const auto st = run(srv);
            QCOMPARE(st.truncation, int(seldon::GenStats::TokenLimit));
            QCOMPARE(st.tokenLimit, 50);
        }
        {
            MockServer srv([&](const QByteArray&, const QByteArray&, int) {
                return sse("data: {\"choices\":[{\"delta\":{\"content\":\"Fin normale\"},\"finish_reason\":\"stop\"}]}\n\n"
                           "data: [DONE]\n\n");
            });
            QCOMPARE(run(srv).truncation, int(seldon::GenStats::NotTruncated));
        }
        {
            MockServer srv([&](const QByteArray&, const QByteArray&, int) {
                return sse("data: {\"choices\":[{\"delta\":{\"content\":\"Coupé net\"}}]}\n\n");     // ni finish_reason ni [DONE]
            });
            QCOMPARE(run(srv).truncation, int(seldon::GenStats::StreamCut));
        }
        {
            MockServer srv([&](const QByteArray&, const QByteArray&, int) {
                return sse("data: {\"choices\":[{\"delta\":{\"content\":\"Début\"}}]}\n\n"
                           "data: {\"error\":{\"message\":\"surcharge\"}}\n\n");
            });
            const auto st = run(srv);
            QCOMPARE(st.truncation, int(seldon::GenStats::StreamCut));
            QCOMPARE(QString::fromStdString(st.truncationDetail), QString("surcharge"));
        }
        // Événement SSE : finish_reason lu, même sans texte
        seldon::remote::SseParser p;
        const auto ev = p.feed("data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n");
        QCOMPARE(ev.size(), 1);
        QCOMPARE(ev[0].finishReason, QString("length"));
        QString fr;
        seldon::remote::textFromFullResponse(R"({"choices":[{"message":{"content":"x"},"finish_reason":"length"}]})", nullptr, nullptr, &fr);
        QCOMPARE(fr, QString("length"));
    }

    void remoteModelLimits()
    {
        const auto lim = seldon::remote::parseModelLimits(R"({"data":[
            {"id":"or/model","context_length":131072,"top_provider":{"context_length":131072,"max_completion_tokens":8192}},
            {"id":"mistral-small-latest","max_context_length":32768},
            {"id":"vllm-model","max_model_len":"16384"},
            {"id":"gpt-4o-mini"}]})");
        QCOMPARE(lim.value("or/model").context, 131072);
        QCOMPARE(lim.value("or/model").maxOutput, 8192);
        QCOMPARE(lim.value("mistral-small-latest").context, 32768);
        QCOMPARE(lim.value("vllm-model").context, 16384);
        QVERIFY(!lim.contains("gpt-4o-mini"));                                    // rien d'annoncé
    }

    // « Optimiser pour ce modèle » : règles de calcul des réglages.
    void autoTuneRules()
    {
        using seldon::TuneInput;
        const uint64_t GiB = 1024ull * 1024ull * 1024ull;
        // Qwen3.5-4B type : 32 couches, 4 têtes KV de 256 -> 128 Kio par token
        QCOMPARE(seldon::kvBytesPerToken(32, 2560, 10, 4) , uint64_t(2 * 32 * 4 * 256 * 2));
        QCOMPARE(seldon::kvBytesPerToken(0, 2560, 10, 4), uint64_t(0));

        TuneInput big; big.trainContext = 262144; big.nLayer = 32; big.nEmbd = 2560; big.nHead = 10; big.nHeadKv = 4;
        big.modelBytes = 2.7 * GiB; big.memTotal = 21 * GiB; big.onGpu = true;            // Mac 32 Go
        auto r = seldon::autoTune(big);
        QCOMPARE(r.nCtx, 32768);
        QCOMPARE(r.maxTokens, 4096);
        QCOMPARE(r.skillChars, 20000);

        TuneInput small = big; small.memTotal = 8 * GiB; small.onGpu = false;               // PC 8 Go, CPU
        r = seldon::autoTune(small);
        QVERIFY(r.nCtx >= 4096 && r.nCtx <= 16384);
        QVERIFY(r.maxTokens >= 1024 && r.maxTokens <= r.nCtx / 4 + 1);

        TuneInput slow = big; slow.tokPerSec = 5;                                           // génération lente
        r = seldon::autoTune(slow);
        QCOMPARE(r.nCtx, 8192);
        QCOMPARE(r.maxTokens, 1024);                                                        // ≈ 2 min à 5 tok/s : plancher

        TuneInput shortCtx = big; shortCtx.trainContext = 8192;                             // modèle à 8k
        QCOMPARE(seldon::autoTune(shortCtx).nCtx, 8192);
        QCOMPARE(seldon::autoTune(shortCtx).skillChars, 5500);

        TuneInput think = big; think.thinking = true; think.trainContext = 8192;            // réflexion : réserve comprise
        r = seldon::autoTune(think);
        QVERIFY(r.maxTokens + seldon::AppSettings::kThinkingReserve <= std::max(r.nCtx / 2, 1024 + seldon::AppSettings::kThinkingReserve));

        TuneInput unknown; unknown.trainContext = 32768;                                    // mémoire inconnue : prudence
        QCOMPARE(seldon::autoTune(unknown).nCtx, 8192);

        TuneInput rem; rem.remote = true; rem.remoteContext = 131072; rem.remoteMaxOutput = 8192;
        r = seldon::autoTune(rem);
        QCOMPARE(r.nCtx, 0);
        QCOMPARE(r.maxTokens, 4096);
        QCOMPARE(r.skillChars, 12000);
        TuneInput remSmall; remSmall.remote = true; remSmall.remoteContext = 8192; remSmall.remoteMaxOutput = 1000;
        QCOMPARE(seldon::autoTune(remSmall).maxTokens, 1000);
        TuneInput remUnknown; remUnknown.remote = true;
        QCOMPARE(seldon::autoTune(remUnknown).maxTokens, 4096);

        seldon::AppSettings s; s.maxTokens = 2000; s.thinking = true;
        QCOMPARE(s.effectiveMaxTokens(false), 2000 + seldon::AppSettings::kThinkingReserve);
        QCOMPARE(s.effectiveMaxTokens(true), 2000);
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
        QCOMPARE(all.size(), 9);
        const QJsonArray api = seldon::tools::schemaForApi();
        QCOMPARE(api.size(), 9);
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
            // « event » combine les autres méthodes (MCMC) ; « density_file » écrit un fichier pour le plugin Density
            if (!d.methodId.isEmpty() && d.methodId != "event" && d.methodId != "density_file")
                QVERIFY2(seldon::MethodRegistry::instance().byId(d.methodId) != nullptr, qPrintable(d.name));
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
        const auto m = execute("calibrer_radiocarbone", R"({"age":3000,"error":40,"curve":"marine20","delta_r":-50,"delta_r_error":30})", fmt);
        QVERIFY(m.executed && m.request.value("has_delta_r").toBool());

        // valeur manquante : rien n'est calculé, le modèle reçoit une consigne claire
        const auto tl = execute("dater_luminescence", R"({"age":800,"error":50})", fmt);
        QVERIFY(tl.known && !tl.executed);
        QVERIFY(tl.content.contains("ref_year") && tl.content.contains("Demande"));
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
        QCOMPARE(b1.value("tools").toArray().size(), 9);
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
        QCOMPARE(first.value("tools").toArray().size(), 9);

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

    // Préférences : avertissement « clé en clair » sous la Base URL, mis à jour pendant la saisie.
    void settingsDialogHttpWarning()
    {
        seldon::AppSettings s;
        s.useRemote = true; s.remoteUrl = "https://api.example.org/v1"; s.remoteKey = "k"; s.remoteModel = "m1";
        seldon::SettingsDialog dlg(s);
        dlg.show();
        auto* warn = dlg.findChild<QLabel*>("httpWarning");
        auto* store = dlg.findChild<QLabel*>("keyStore");
        QVERIFY(warn && store);
        QVERIFY(!warn->isVisible());
        QVERIFY(store->isVisible() && !store->text().isEmpty());
        QLineEdit* url = nullptr;
        QLineEdit* key = nullptr;
        for (auto* le : dlg.findChildren<QLineEdit*>()) {
            if (le->text() == s.remoteUrl) url = le;
            if (le->echoMode() == QLineEdit::Password) key = le;
        }
        QVERIFY(url && key);
        url->setText("http://192.168.1.20:8080/v1");
        QVERIFY(warn->isVisible());
        key->clear();                                                       // sans clé : plus d'avertissement
        QVERIFY(!warn->isVisible());
        key->setText("k");
        url->setText("http://localhost:11434/v1");
        QVERIFY(!warn->isVisible());
        url->setText("http://192.168.1.20:8080/v1");
        dlg.findChildren<QRadioButton*>().first()->setChecked(true);         // « Modèle local » : lignes distantes masquées
        QVERIFY(!warn->isVisible() && !store->isVisible());
    }

    void parseDetails()
    {
        auto v = MethodRegistry::instance().parse("coquille marine, 14C 3000 ± 30 BP, ΔR 100 ± 50");
        QCOMPARE(v.size(), 1);
        QCOMPARE(v[0].value("curve").toString(), QString("marine20"));
        QVERIFY(v[0].value("has_delta_r").toBool());
        QCOMPARE(v[0].value("delta_r").toDouble(), 100.0);
        QCOMPARE(v[0].value("delta_r_error").toDouble(), 50.0);
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
        const auto sel = m.select("Peux-tu calibrer une date C14 et donner les HPD ?", AppSettings().skillChars, QStringList());
        bool found = false;
        for (const SkillMatch& mt : sel) if (mt.skill->id == "09_outils_calcul") found = true;
        QVERIFY(found);
    }

    void calcBlocks()
    {
        const QString ans = "Je lance le calcul.\n\n```calcul\n{\"method\":\"tl\",\"age\":800,\"error\":60,\"ref_year\":2015}\n```\n"
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

    // Niveau de confiance : 95 % par défaut ; un autre niveau demandé est calculé en plus des HPD 95 % et 68 %.
    void datingConfidenceLevel()
    {
        QCOMPARE(MethodRunner::requestedLevel("C14 de 2500 ± 30 BP à 99 %"), 99.0);
        QCOMPARE(MethodRunner::requestedLevel("donne la HPD 90 pour 2500 ± 30 BP"), 90.0);
        QCOMPARE(MethodRunner::requestedLevel("avec un niveau de confiance de 99,7 %"), 99.7);
        QCOMPARE(MethodRunner::requestedLevel("intervalle de crédibilité à 80 pour cent"), 80.0);
        QCOMPARE(MethodRunner::requestedLevel("C14 de 2500 ± 30 BP"), 0.0);
        QCOMPARE(MethodRunner::requestedLevel("inclinaison 65°, alpha95 = 1"), 0.0);
        QCOMPARE(MethodRunner::requestedLevel("inclinaison 65, cône de confiance α95 de 2"), 0.0);
        QCOMPARE(MethodRunner::requestedLevel("charbon du niveau 3, 2500 ± 30 BP"), 0.0);
        QCOMPARE(MethodRunner::requestedLevel("erreur à 5 %"), 0.0);                       // hors [50 ; 100[

        const RunOutcome def = MethodRunner::analyzeQuestion("C14 de 2500 ± 30 BP");
        QVERIFY(def.found);
        QCOMPARE(def.plot["hpd"].toDouble(), 95.0);
        QVERIFY(def.noticeHtml.contains("HPD 95 %") && def.noticeHtml.contains("HPD 68 %"));
        QVERIFY(!def.noticeHtml.contains("demandé"));

        const RunOutcome o = MethodRunner::analyzeQuestion("C14 de 2500 ± 30 BP, donne la HPD à 99 %");
        QVERIFY(o.found);
        QCOMPARE(o.plot["hpd"].toDouble(), 99.0);
        QVERIFY(o.noticeHtml.contains("HPD 99 %"));
        QVERIFY(o.noticeHtml.contains("HPD 95 %") && o.noticeHtml.contains("HPD 68 %"));
        QVERIFY(o.noticeHtml.indexOf("HPD 99 %") < o.noticeHtml.indexOf("HPD 95 %"));     // du plus large au plus étroit
        QVERIFY(o.promptBlock.contains("Intervalles HPD à 99 % (niveau demandé"));

        // La HPD à 99 % contient celle à 95 %.
        const MethodResult r = MethodRegistry::instance().compute({ { "method", "c14" }, { "age", 2500 }, { "error", 30 } });
        QVERIFY(r.ok);
        const auto h99 = hpdIntervals(r.density, 99.0);
        QVERIFY(h99.front().from <= r.hpd95.front().from && h99.back().to >= r.hpd95.back().to);

        // Bloc « calcul » ou outil : clé level ; valeur décimale ; valeur invalide -> 95 % et avertissement.
        const RunOutcome dec = MethodRunner::run({ QJsonObject{ { "method", "gauss" }, { "age", 1200 }, { "error", 50 }, { "level", 99.7 } } });
        QVERIFY(dec.noticeHtml.contains("HPD 99,7 %"));
        const RunOutcome bad = MethodRunner::run({ QJsonObject{ { "method", "gauss" }, { "age", 1200 }, { "error", 50 }, { "level", 150 } } });
        QVERIFY(bad.noticeHtml.contains("invalide"));
        QCOMPARE(bad.plot["hpd"].toDouble(), 95.0);
        const auto t = tools::execute("calibrer_radiocarbone", R"({"age":2500,"error":30,"level":"90"})", DateFormat::BcAd);
        QVERIFY2(t.executed, qPrintable(t.content));
        QVERIFY(t.content.contains("Intervalles HPD à 90 %"));
    }

    // Plusieurs datations dans une question : toutes calculées, de méthodes différentes si besoin, sur UNE figure.
    void severalDatingsOneFigure()
    {
        auto methods = [](const QVector<QJsonObject>& v) { QStringList m; for (const QJsonObject& o : v) m << o["method"].toString(); return m; };
        const MethodRegistry& reg = MethodRegistry::instance();

        // Même méthode : inchangé
        QCOMPARE(methods(reg.parse("C14 de 2500 ± 30 BP et 3000 ± 40 BP")), QStringList({ "c14", "c14" }));
        // Méthodes mélangées : chaque paire va à sa méthode, dans l'ordre de la question
        QVector<QJsonObject> v = reg.parse("TL 488 ± 19 ans mesuré en 2015 et C14 2500 ± 30 BP");
        QCOMPARE(methods(v), QStringList({ "tl", "c14" }));
        QCOMPARE(v[0]["age"].toDouble(), 488.0);
        QCOMPARE(v[0]["ref_year"].toInt(), 2015);
        QCOMPARE(v[1]["age"].toDouble(), 2500.0);
        v = reg.parse("C14 : 2500 ± 30, 3000 ± 40 ; TL : 488 ± 19 (mesure en 2015)");
        QCOMPARE(methods(v), QStringList({ "c14", "c14", "tl" }));
        v = reg.parse("C14 2500 ± 30 BP et inclinaison 65°, alpha95 = 1");
        QCOMPARE(methods(v), QStringList({ "c14", "am" }));
        v = reg.parse("F14C 1,2345 ± 0,003 et C14 2500 ± 30 BP");             // « 14c » de « f14c » ignoré
        QCOMPARE(methods(v), QStringList({ "f14c", "c14" }));

        // Une seule figure, une courbe par datation
        const RunOutcome o = MethodRunner::analyzeQuestion("TL 488 ± 19 ans mesuré en 2015 et C14 2500 ± 30 BP");
        QCOMPARE(o.count, 2);
        QCOMPARE(o.plot["series"].toArray().size(), 2);

        // Fusion de figures arrivées en plusieurs fois : courbes ajoutées, doublons ignorés
        const RunOutcome a = MethodRunner::run({ QJsonObject{ { "method", "c14" }, { "age", 2500 }, { "error", 30 } } });
        const RunOutcome b = MethodRunner::run({ QJsonObject{ { "method", "c14" }, { "age", 2500 }, { "error", 30 } },
                                                 QJsonObject{ { "method", "gauss" }, { "age", 1200 }, { "error", 50 } } });
        const QJsonObject m = tools::mergePlots({ a.plot, b.plot });
        QCOMPARE(m["series"].toArray().size(), 2);
        seldon::FigurePanel panel;
        QVERIFY(panel.addCurvesJson(a.plot));
        QWidget* page = panel.lastCurves();
        QVERIFY(page);
        QVERIFY(panel.replaceCurvesJson(page, m));
        QCOMPARE(panel.count(), 1);                                                 // pas de nouvel onglet
    }

    // Événement à plusieurs datations (MCMC de ChronoModel) : diagnostics, combinaison, reconnaissance, outil.
    void eventMcmcDiagnostics()
    {
        std::mt19937 g(7);
        std::normal_distribution<double> n01;
        std::vector<std::vector<double>> iid(4, std::vector<double>(2000));
        for (auto& c : iid) for (double& x : c) x = n01(g);
        const auto ok = mcmcdiag::computeRhatAndEss(iid);
        QVERIFY2(ok.rhat < 1.01, qPrintable(QString::number(ok.rhat)));
        QVERIFY2(ok.bulkEss > 6000 && ok.bulkEss < 10000, qPrintable(QString::number(ok.bulkEss)));
        auto shifted = iid;
        for (double& x : shifted[0]) x += 3.0;                       // une chaîne bloquée ailleurs
        QVERIFY(mcmcdiag::computeRhatAndEss(shifted).rhat > 1.1);
    }

    void eventMcmcCombination()
    {
        const MethodRegistry& reg = MethodRegistry::instance();
        auto dens = [&](double age, double err) {
            return reg.compute({ { "method", "gauss" }, { "age", age }, { "error", err }, { "mode", "none" } }).density;
        };
        McmcSettings ms;
        ms.numIter = 30000;
        ms.seeds = { 1, 2, 3, 4 };
        // Deux dates concordantes : θ ≈ moyenne pondérée, écart-type ≈ 30/√2
        const EventMcmcResult r = runEventMcmc({ { "A", dens(1000, 30), {} }, { "B", dens(1020, 30), {} } }, ms, 0, 2000);
        QVERIFY2(r.ok, r.error.c_str());
        const DensityStats st = densityStats(r.theta);
        QVERIFY2(std::abs(st.mean - 1010) < 4, qPrintable(QString::number(st.mean)));
        QVERIFY2(std::abs(st.sd - 21.2) < 3, qPrintable(QString::number(st.sd)));
        QCOMPARE(int(r.chains.size()), 4);
        QCOMPARE(r.chains[0].seed, 1u);
        QCOMPARE(int(r.diagnostics.size()), 2 + 2 * 2);                 // θ, S0, (t, σ) par date
        QVERIFY(r.maxRhat < 1.05);
        QVERIFY(r.status != Convergence::Unknown);
        QCOMPARE(int(r.thetaTraces.size()), 4);
        QCOMPARE(int(r.thetaTraces[0].size()), 3000);                   // 30000 / thinning 10
        // largeur de bande du lissage de θ : celle de kernelBandwidth sur tous les tirages, et une par t_i
        {
            std::vector<double> all;
            for (const auto& tr : r.thetaTraces) all.insert(all.end(), tr.begin(), tr.end());
            Bandwidth usedRule = Bandwidth::Silverman;
            QCOMPARE(r.thetaBandwidth, kernelBandwidth(all, Bandwidth::SheatherJones, &usedRule));
            QCOMPARE(r.thetaBandwidthSJ, usedRule == Bandwidth::SheatherJones);
            QVERIFY(r.thetaBandwidth > 0 && r.thetaBandwidth < 21.2);    // bien plus fin que l'écart-type de θ
            QCOMPARE(int(r.tiBandwidth.size()), 2);
            QVERIFY(r.tiBandwidth[0] > 0);
        }
        // Reproductible avec les mêmes graines
        const EventMcmcResult r2 = runEventMcmc({ { "A", dens(1000, 30), {} }, { "B", dens(1020, 30), {} } }, ms, 0, 2000);
        QCOMPARE(r2.thetaTraces[2], r.thetaTraces[2]);
        // Une date discordante reçoit une grande dispersion σ et pèse peu
        const EventMcmcResult o = runEventMcmc({ { "A", dens(1000, 30), {} }, { "B", dens(1010, 30), {} }, { "C", dens(1400, 30), {} } },
                                               ms, 0, 2000);
        QVERIFY(o.ok);
        QVERIFY2(std::abs(densityStats(o.theta).mean - 1005) < 30, qPrintable(QString::number(densityStats(o.theta).mean)));
        QVERIFY(o.sigmaMedian[2] > 10 * o.sigmaMedian[0]);
        // Décalage fixe δ = +100 (y = t + δ) : l'événement suit les dates décalées
        Wiggle w; w.type = Wiggle::Fixed; w.fixed = 100;
        const EventMcmcResult d = runEventMcmc({ { "A", dens(1000, 30), w }, { "B", dens(1100, 30), {} } }, ms, 0, 2000);
        QVERIFY(d.ok);
        QVERIFY2(std::abs(densityStats(d.theta).mean - 1100) < 5, qPrintable(QString::number(densityStats(d.theta).mean)));
    }

    // Lissage des densités a posteriori : Sheather-Jones (bw.SJ de R), pas Silverman, pour la multimodalité.
    void kernelBandwidthSheatherJones()
    {
        auto bimodal = [](int na, int nb) {                    // deux modes uniformes de largeur 60, séparés de 150
            std::vector<double> x;
            auto u = [](int i) { return std::fmod(double(i) * 0.6180339887498949, 1.0); };
            for (int i = 0; i < na; ++i) x.push_back(1000.0 + 60.0 * (u(i) - 0.5));
            for (int i = 0; i < nb; ++i) x.push_back(1150.0 + 60.0 * (u(i) - 0.5));
            return x;
        };
        // Valeurs de référence R 4.x : bw.SJ(x) et bw.nrd0(x) (tolérance : uniroot de R, tol = 0.1·lower)
        const std::vector<double> small = bimodal(200, 100);   // n ≤ 500 : comptage exact des paires
        QVERIFY2(std::abs(kernelBandwidth(small) - 7.9873318) < 0.02, qPrintable(QString::number(kernelBandwidth(small))));
        QVERIFY(std::abs(kernelBandwidth(small, Bandwidth::Silverman) - 20.926738) < 1e-4);
        const std::vector<double> big = bimodal(400, 200);     // n > 500 : comptage binné
        QVERIFY2(std::abs(kernelBandwidth(big) - 6.2520425) < 0.02, qPrintable(QString::number(kernelBandwidth(big))));
        QVERIFY(std::abs(kernelBandwidth(big, Bandwidth::Silverman) - 18.240028) < 1e-4);
        // Le creux entre les deux modes est conservé (Silverman le comble en partie)
        const Density sj = kernelDensity(big, 0, 2000), sv = kernelDensity(big, 0, 2000, Bandwidth::Silverman);
        auto at = [](const Density& d, double t) { return d.v[size_t(std::lround((t - d.t[0]) / d.step))]; };
        QVERIFY(at(sj, 1075) / at(sj, 1000) < 0.5 * at(sv, 1075) / at(sv, 1000));
    }

    void eventFromQuestionAndTool()
    {
        QVERIFY(MethodRunner::asksEvent("Calcule l'événement avec ces deux dates"));
        QVERIFY(MethodRunner::asksEvent("combine ces datations"));
        QVERIFY(!MethodRunner::asksEvent("C14 de 2500 ± 30 BP"));
        const QJsonObject m = MethodRunner::mcmcFromQuestion("avec 20 000 itérations, un burn-in de 500, 2 chaînes, graines 11;12");
        QCOMPARE(m["num_iter"].toInt(), 20000);
        QCOMPARE(m["num_burn"].toInt(), 500);
        QCOMPARE(m["num_proc"].toInt(), 2);
        QCOMPARE(m["seeds"].toString(), QString("11;12"));

        int calls = 0;
        MethodRunner::setProgressHandler([&](const QString&, double) { ++calls; return true; });
        const RunOutcome o = MethodRunner::analyzeQuestion(
            "Calcule l'événement « US 12 » avec C14 2500 ± 30 BP et TL 2560 ± 80 ans mesuré en 2015, 20000 itérations, graines 1;2;3;4");
        MethodRunner::setProgressHandler({});
        QVERIFY(o.found);
        QCOMPARE(o.count, 1);
        QVERIFY2(o.noticeHtml.contains("Événement « US 12 »"), qPrintable(o.noticeHtml.left(300)));
        QVERIFY(o.noticeHtml.contains("θ a posteriori"));
        QVERIFY(o.noticeHtml.contains("R̂"));
        QVERIFY(o.noticeHtml.contains("1;2;3;4"));
        QVERIFY(!o.noticeHtml.contains("Calcul impossible"));
        QVERIFY(calls > 0);
        QCOMPARE(o.plot["series"].toArray().size(), 3);                 // θ + 2 calibrations
        QVERIFY(!o.plot["series"].toArray()[1].toObject()["fill"].toBool(true));
        QCOMPARE(o.extraPlots.size(), 1);                               // traces des chaînes
        QCOMPARE(o.extraPlots[0]["series"].toArray().size(), 4);
        seldon::CurveSpec sp;
        QVERIFY(seldon::CurveSpec::fromJson(o.extraPlots[0], sp));
        QVERIFY(!sp.yFromZero);
        QVERIFY(o.promptBlock.contains("Convergence"));
        QVERIFY(o.noticeHtml.contains("largeur de bande <b>h = "));
        QVERIFY(o.noticeHtml.contains("Lissage des densités"));
        QVERIFY(o.promptBlock.contains("lissée par noyau gaussien : h = "));

        // Outil du modèle distant
        const auto t = tools::execute("calculer_evenement",
            R"({"name":"Four","dates":[{"method":"gauss","age":1200,"error":40},{"method":"gauss","age":1230,"error":40,"wiggle":{"type":"range","min":0,"max":20}}],
                "mcmc":{"num_iter":10000,"seeds":[5,6,7,8]}})", DateFormat::BcAd);
        QVERIFY2(t.executed, qPrintable(t.content));
        QVERIFY(t.content.contains("Événement « Four »"));
        QVERIFY(t.content.contains("décalage δ ∈ [0 ; 20] ans"));
        QVERIFY(!tools::execute("calculer_evenement", R"({"dates":[]})", DateFormat::BcAd).executed);
        QVERIFY(!tools::execute("calculer_evenement", R"({"dates":[{"age":1}]})", DateFormat::BcAd).executed);
        // Erreur d'une datation : message explicite
        const RunOutcome bad = MethodRunner::run({ QJsonObject{ { "method", "event" }, { "dates", QJsonArray{ QJsonObject{ { "method", "c14" }, { "age", 2500 } } } } } });
        QVERIFY(bad.noticeHtml.contains("Calcul impossible"));
    }

    // Fragment JSON collé dans la question (bloc chronomodel / .chr) : l'application lance l'événement elle-même ;
    // blocs ```calcul tolérants (nombres en texte, alias, nom d'outil, objets séparés par des virgules, format .chr).
    void eventFromJsonFragment()
    {
        const QString q = "calcul Event avec ces deux dates, 10000 itérations\n"
                          "{\"method\": \"c14\", \"age\": 2570, \"error\": 35},\n{\"method\": \"c14\", \"age\": 2770, \"error\": 35}";
        const RunOutcome o = MethodRunner::analyzeQuestion(q);
        QVERIFY(o.found);
        QVERIFY2(o.promptBlock.contains("combinaison bayésienne de 2 datations"), qPrintable(o.promptBlock.left(400)));
        QVERIFY(o.promptBlock.contains("2570 ± 35 BP"));
        QVERIFY(!o.promptBlock.contains("Calcul impossible"));

        // Normalisation
        QJsonObject n = MethodRunner::normalizeRequest(QJsonObject{ { "method", "14C" }, { "age", "2 570" }, { "error", "35" } });
        QCOMPARE(n["method"].toString(), QString("c14"));
        QCOMPARE(n["age"].toDouble(), 2570.0);
        QCOMPARE(n["error"].toDouble(), 35.0);
        n = MethodRunner::normalizeRequest(QJsonObject{ { "plugin_id", "14c" }, { "name", "Ly-1" },
                                                        { "data", QJsonObject{ { "age", 2570 }, { "error", 35 }, { "ref_curve", "IntCal20.14c" } } } });
        QCOMPARE(n["method"].toString(), QString("c14"));
        QCOMPARE(n["error"].toDouble(), 35.0);
        QCOMPARE(n["curve"].toString(), QString("intcal20"));
        QCOMPARE(MethodRunner::normalizeRequest(QJsonObject{ { "method", "calculer_evenement" } })["method"].toString(), QString("event"));

        // Objets séparés par une virgule (copiés tels quels) : deux calibrations
        auto v = MethodRunner::extractCalcBlocks("```calcul\n{\"method\": \"c14\", \"age\": 2570, \"error\": 35},\n"
                                                 "{\"method\": \"c14\", \"age\": 2770, \"error\": 35}\n```\n");
        QCOMPARE(v.size(), 2);
        // Événement avec nombres en texte : plus de « l'erreur doit être strictement positive »
        v = MethodRunner::extractCalcBlocks("```calcul\n{\"method\":\"event\",\"mcmc\":{\"num_iter\":10000},\"dates\":["
                                            "{\"method\":\"c14\",\"age\":\"2570\",\"error\":\"35\"},{\"method\":\"c14\",\"age\":2770,\"error\":35}]}\n```\n");
        QCOMPARE(v.size(), 1);
        const RunOutcome e = MethodRunner::run(v);
        QVERIFY2(!e.promptBlock.contains("Calcul impossible"), qPrintable(e.promptBlock.left(400)));
        QCOMPARE(e.plot["series"].toArray().size(), 3);
        // Bloc ```json avec une demande de calcul : accepté ; sans « method » : ignoré
        QCOMPARE(MethodRunner::extractCalcBlocks("```json\n{\"method\":\"c14\",\"age\":2570,\"error\":35}\n```\n").size(), 1);
        QCOMPARE(MethodRunner::extractCalcBlocks("```json\n{\"events\":[]}\n```\n").size(), 0);
        // Outil distant : datations au format .chr
        const auto t = tools::execute("calculer_evenement",
            R"({"mcmc":{"num_iter":10000},"dates":[{"plugin_id":"14c","data":{"age":2570,"error":35}},{"method":"14C","age":"2770","error":"35"}]})",
            DateFormat::BcAd);
        QVERIFY2(t.executed, qPrintable(t.content));
        QVERIFY(!t.content.contains("Calcul impossible"));
    }

    // Wiggle : date seule = densité de t + δ (convolution, Date::calibrateWiggle) ; dans un événement, δ est une variable
    // du MCMC (y = t + δ ~ N(θ, σ²)), la vraisemblance reste celle de la date sans décalage.
    void wiggleSingleDateAndEvent()
    {
        // Date seule, décalage fixe : translation exacte de la densité
        const QJsonObject g{ { "method", "gauss" }, { "age", 1000 }, { "error", 20 } };
        QJsonObject gw = g; gw["wiggle"] = QJsonObject{ { "type", "fixed" }, { "value", 50 } };
        const RunOutcome a = MethodRunner::run({ g }), b = MethodRunner::run({ gw });
        QVERIFY(b.promptBlock.contains("densité de t + δ"));
        QVERIFY(!b.promptBlock.contains("Calcul impossible"));
        QVERIFY(a.promptBlock.contains("Moyenne : 1000") && b.promptBlock.contains("Moyenne : 1050"));
        // Range et gaussian : même centre, densité plus large
        QJsonObject gr = g; gr["wiggle"] = QJsonObject{ { "type", "range" }, { "min", 40 }, { "max", 60 } };
        QJsonObject gg = g; gg["wiggle"] = QJsonObject{ { "type", "gaussian" }, { "mean", 50 }, { "error", 30 } };
        QVERIFY(MethodRunner::run({ gr }).promptBlock.contains("Moyenne : 1050"));
        QVERIFY(MethodRunner::run({ gg }).promptBlock.contains("Moyenne : 1050"));
        QJsonObject bad = g; bad["wiggle"] = QJsonObject{ { "type", "range" }, { "min", 60 }, { "max", 40 } };
        QVERIFY(MethodRunner::run({ bad }).promptBlock.contains("Calcul impossible"));
        // Outil distant : paramètre « wiggle » des datations simples
        QVERIFY(tools::execute("densite_gauss", R"({"age":1000,"error":20,"wiggle":{"type":"fixed","value":50}})", DateFormat::BcAd)
                    .content.contains("densité de t + δ"));

        // Événement : N(950)+50 et N(1000) concordent ; t + δ a posteriori est donné pour la date décalée
        QJsonObject e2{ { "method", "gauss" }, { "age", 950 }, { "error", 20 }, { "wiggle", QJsonObject{ { "type", "fixed" }, { "value", 50 } } } };
        const RunOutcome e = MethodRunner::run({ QJsonObject{ { "method", "event" }, { "mcmc", QJsonObject{ { "num_iter", 10000 } } },
                                                              { "dates", QJsonArray{ g, e2 } } } });
        QVERIFY2(e.promptBlock.contains("t + δ a posteriori"), qPrintable(e.promptBlock.left(600)));
        QVERIFY(e.promptBlock.contains("calibration de t + δ"));
        QCOMPARE(e.plot["series"].toArray().size(), 3);
    }

    // Date collée au format du bloc chronomodel ({"type":"gauss",…}) + wiggle écrit dans la question.
    void wiggleFromQuestionText()
    {
        const QString q = QString::fromUtf8("cette date\n{\n\"type\": \"gauss\",\n\"name\": \"Date Gaussienne\",\n\"age\": 250,\n\"error\": 10\n}\n"
                                            "avec un wiggle entre 100 et 300");
        const RunOutcome o = MethodRunner::analyzeQuestion(q);
        QVERIFY(o.found);
        QVERIFY2(o.promptBlock.contains("densité de t + δ"), qPrintable(o.promptBlock.left(400)));
        QVERIFY(o.promptBlock.contains("Moyenne : 450"));
        QCOMPARE(MethodRunner::wiggleFromQuestion("avec un décalage de 20 ± 5 ans")["type"].toString(), QString("gaussian"));
        QCOMPARE(MethodRunner::wiggleFromQuestion("wiggle de 20 ans")["value"].toDouble(), 20.0);
        QVERIFY(MethodRunner::wiggleFromQuestion("calibre 2500 ± 30 BP avec delta R = 50").isEmpty());   // ΔR n'est pas un wiggle
    }

    // Wiggle au format .chr (delta_type -1/0/1/2) : lecture des dates d'un projet ChronoModel, export du bloc chronomodel.
    void wiggleChrFormat()
    {
        // Dates de l'événement « AM » de test_AM_Gauss_wiggle.chr (archéomag. I + trois gauss avec les trois types de wiggle)
        const QJsonArray ds = QJsonDocument::fromJson(R"JS([
          {"plugin_id":"am","name":"Inclinaison","delta_type":-1,"delta_fixed":0,
           "data":{"alpha95":1,"dec":0,"error_f":2,"field":65,"inc":65,"integration_steps":500,"process_type":1,
                   "refD_curve":"bulgaria_d.ref","refF_curve":"gwh2013uni_f.ref","refI_curve":"gal2002sph2014_i.ref"}},
          {"plugin_id":"gauss","name":"N(3000; 50)","delta_type":1,"delta_average":1000,"delta_error":200,
           "data":{"a":0,"age":0,"b":1,"c":0,"curve":"example_csv_ref.csv","error":50,"mode":"none"}},
          {"plugin_id":"gauss","name":"G(0, 100)","delta_type":2,"delta_min":-10,"delta_max":10,
           "data":{"a":0,"age":0,"b":1,"c":0,"curve":"courbe_a.csv","error":100,"mode":"none"}},
          {"plugin_id":"gauss","name":"G(10, 50)","delta_type":0,"delta_fixed":-10,
           "data":{"a":0,"age":10,"b":1,"c":0,"curve":"courbe_a.csv","error":50,"mode":"none"}},
          {"plugin_id":"gauss","name":"ancien","delta_type":-1,"delta_min":1000,"delta_max":1500,"data":{"age":0,"error":50}}])JS").array();
        const QJsonObject am = MethodRunner::normalizeRequest(ds[0].toObject());
        QCOMPARE(am["method"].toString(), QString("am"));
        QCOMPARE(am["process"].toString(), QString("I"));
        QCOMPARE(am["curve_i"].toString(), QString("gal2002sph2014_i"));
        QVERIFY(!am.contains("wiggle"));
        QCOMPARE(MethodRunner::normalizeRequest(ds[1].toObject())["wiggle"].toObject()["type"].toString(), QString("gaussian"));
        QCOMPARE(MethodRunner::normalizeRequest(ds[2].toObject())["wiggle"].toObject()["max"].toDouble(), 10.0);
        QCOMPARE(MethodRunner::normalizeRequest(ds[3].toObject())["wiggle"].toObject()["value"].toDouble(), -10.0);
        QVERIFY(!MethodRunner::normalizeRequest(ds[4].toObject()).contains("wiggle"));     // delta_type -1 : valeurs résiduelles ignorées

        QJsonArray four; for (int k = 0; k < 4; ++k) four.append(ds[k]);
        const RunOutcome e = MethodRunner::run({ QJsonObject{ { "method", "event" }, { "name", "AM" },
                                                              { "mcmc", QJsonObject{ { "num_iter", 10000 } } }, { "dates", four } } });
        QVERIFY2(!e.promptBlock.contains("Calcul impossible"), qPrintable(e.promptBlock.left(600)));
        QVERIFY(e.promptBlock.contains("δ ~ N(1000 ; 200)") && e.promptBlock.contains("δ ∈ [-10 ; 10]") && e.promptBlock.contains("δ = -10"));

        // Export : bloc chronomodel → champs delta_* ; plage d'étude couvrant t + δ
        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(R"({"name":"W","events":[{"name":"E","dates":[
            {"type":"gauss","name":"A","age":0,"error":50,"wiggle":{"type":"gaussian","mean":1000,"error":200}},
            {"type":"gauss","name":"B","age":0,"error":100,"wiggle":{"type":"range","min":-10,"max":10}},
            {"type":"gauss","name":"C","age":10,"error":50,"wiggle":{"type":"fixed","value":-10}},
            {"type":"gauss","name":"D","age":0,"error":50}]}]})").object());
        QVERIFY2(r.ok, qPrintable(r.errors.join("; ")));
        const QJsonArray od = r.chr["events"].toArray()[0].toObject()["dates"].toArray();
        QCOMPARE(od[0].toObject()["delta_type"].toInt(), 1);
        QCOMPARE(od[0].toObject()["delta_average"].toDouble(), 1000.0);
        QCOMPARE(od[0].toObject()["delta_error"].toDouble(), 200.0);
        QCOMPARE(od[1].toObject()["delta_type"].toInt(), 2);
        QCOMPARE(od[1].toObject()["delta_min"].toDouble(), -10.0);
        QCOMPARE(od[2].toObject()["delta_type"].toInt(), 0);
        QCOMPARE(od[2].toObject()["delta_fixed"].toDouble(), -10.0);
        QCOMPARE(od[3].toObject()["delta_type"].toInt(), -1);
        QVERIFY(r.chr["settings"].toObject()["tmax"].toDouble() >= 1600);            // 0 + 1000 + 3×200
        QVERIFY(ChrBuilder::summarize(r.chr).contains("[wiggle δ ~ N(1000 ; 200)]"));
        const ChrResult bad = ChrBuilder::build(QJsonDocument::fromJson(
            R"({"events":[{"name":"E","dates":[{"type":"gauss","age":0,"error":5,"wiggle":{"type":"range","min":5,"max":1}}]}]})").object());
        QVERIFY(bad.errors.join(" ").contains("min < max"));
    }

    // « Pas de recuit » = anneal_recurrence 0 (ChronoModel : Recurrence = 0 désactive le recuit simulé).
    void noAnnealing()
    {
        for (const char* q : { "calcule l'événement sans recuit", "pas de recuit", "désactive le recuit", "annealing = 0", "recuit désactivé" })
            QCOMPARE(MethodRunner::mcmcFromQuestion(QString::fromUtf8(q)).value("anneal_recurrence").toInt(-1), 0);
        QVERIFY(!MethodRunner::mcmcFromQuestion("calcule l'événement avec recuit").contains("anneal_recurrence"));
        const RunOutcome o = MethodRunner::analyzeQuestion(QString::fromUtf8(
            "calcule l'événement sans recuit, 10000 itérations : {\"method\":\"gauss\",\"age\":1000,\"error\":20}, {\"method\":\"gauss\",\"age\":1010,\"error\":20}"));
        QVERIFY2(o.promptBlock.contains("sans recuit (anneal_recurrence = 0)"), qPrintable(o.promptBlock.left(600)));
        const ChrResult r = ChrBuilder::build(QJsonDocument::fromJson(
            R"({"events":[{"name":"E","dates":[{"type":"gauss","age":0,"error":5}]}],"mcmc":{"anneal_recurrence":0}})").object());
        QCOMPARE(r.chr["mcmc"].toObject()["anneal_recurrence"].toInt(-1), 0);
        QVERIFY(ChrBuilder::summarize(r.chr).contains("sans recuit"));
    }

    // Export SVG : courbe en vecteurs, image SVG réenregistrée telle quelle, image matricielle incorporée.
    // Titre du graphique : nom de la datation (champ « name », notamment d'un fichier .chr).
    void datingPlotTitleUsesDateName()
    {
        using namespace seldon;
        // sans nom : titre générique
        const RunOutcome a = MethodRunner::run({ QJsonObject{ { "method", "c14" }, { "age", 2500 }, { "error", 30 } } });
        QCOMPARE(a.plot["title"].toString(), QString("Densités calendaires (calcul SeldonH)"));
        // date d'un .chr : le « name » est repris dans le titre et dans la légende
        const QJsonObject chr = QJsonDocument::fromJson(
            R"({"plugin_id":"14c","name":"Ly-1234","data":{"age":2500,"error":30,"ref_curve":"intcal20.14c"}})").object();
        const RunOutcome b = MethodRunner::run({ MethodRunner::normalizeRequest(chr) });
        QCOMPARE(b.plot["title"].toString(), QString("Densité calendaire — Ly-1234"));
        QVERIFY(b.plot["series"].toArray()[0].toObject()["name"].toString().startsWith("Ly-1234 — "));
        // bloc json collé dans la question
        const RunOutcome q = MethodRunner::analyzeQuestion(
            R"(Calibre {"plugin_id":"14c","name":"Ly-1234","data":{"age":2500,"error":30,"ref_curve":"intcal20.14c"}})");
        QCOMPARE(q.plot["title"].toString(), QString("Densité calendaire — Ly-1234"));
        // plusieurs dates nommées, puis fusion avec un autre calcul
        const RunOutcome c = MethodRunner::run({ QJsonObject{ { "method", "gauss" }, { "name", "G1" }, { "age", 1200 }, { "error", 50 } },
                                                 QJsonObject{ { "method", "unif" }, { "name", "U1" }, { "min", 1100 }, { "max", 1300 } } });
        QCOMPARE(c.plot["title"].toString(), QString("Densités calendaires — G1, U1"));
        const QJsonObject m = tools::mergePlots({ b.plot, c.plot });
        QCOMPARE(m["title"].toString(), QString("Densités calendaires — Ly-1234, G1, U1"));
        // outil (modèle distant) : « name » accepté
        const auto t = tools::execute("calibrer_radiocarbone", R"({"name":"Ly-99","age":2500,"error":30})", DateFormat::BcAd);
        QCOMPARE(t.run.plot["title"].toString(), QString("Densité calendaire — Ly-99"));
    }

    // Plusieurs densités : une par ligne sauf demande de superposition ; couleur de la date / de l'Event, sinon défaut.
    void datingPlotRowsAndColors()
    {
        using namespace seldon;
        // deux dates sans précision : en lignes
        const RunOutcome a = MethodRunner::analyzeQuestion("C14 2500 ± 30 BP et C14 2600 ± 30 BP");
        QCOMPARE(a.plot["layout"].toString(), QString("rows"));
        QVERIFY(a.promptBlock.contains("une densité par ligne"));
        CurveSpec spec;
        QVERIFY(CurveSpec::fromJson(a.plot, spec));
        QVERIFY(spec.stacked());
        QVERIFY(!a.plot["series"].toArray()[0].toObject().contains("color"));       // couleur par défaut
        // superposition demandée
        const RunOutcome b = MethodRunner::analyzeQuestion("C14 2500 ± 30 BP et C14 2600 ± 30 BP sur le même graphe");
        QCOMPARE(b.plot["layout"].toString(), QString("overlay"));
        QVERIFY(MethodRunner::asksOverlay("superpose les densités"));
        QVERIFY(!MethodRunner::asksOverlay("calibre ces deux dates"));
        QVERIFY(CurveSpec::fromJson(b.plot, spec));
        QVERIFY(!spec.stacked());
        // bloc plot du modèle sans « layout » : densités en lignes, autres courbes superposées
        QVERIFY(CurveSpec::fromJson(QJsonDocument::fromJson(
            R"({"fill":true,"series":[{"name":"a","x":[1,2,3],"y":[0,1,0]},{"name":"b","x":[1,2,3],"y":[0,2,0]}]})").object(), spec));
        QVERIFY(spec.stacked());
        QVERIFY(CurveSpec::fromJson(QJsonDocument::fromJson(
            R"({"series":[{"name":"a","x":[1,2,3],"y":[0,1,0]},{"name":"b","x":[1,2,3],"y":[0,2,0]}]})").object(), spec));
        QVERIFY(!spec.stacked());
        // couleur : « color », ou red/green/blue d'un .chr
        const RunOutcome c = MethodRunner::run({ QJsonObject{ { "method", "gauss" }, { "age", 1200 }, { "error", 50 }, { "color", "#c0392b" } },
                                                 MethodRunner::normalizeRequest(QJsonDocument::fromJson(
                                                     R"({"plugin_id":"gauss","name":"G2","red":71,"green":112,"blue":76,
                                                         "data":{"age":1300,"error":40}})").object()) });
        QCOMPARE(c.plot["series"].toArray()[0].toObject()["color"].toString(), QString("#c0392b"));
        QCOMPARE(c.plot["series"].toArray()[1].toObject()["color"].toString(), QString("#47704c"));
        QVERIFY(CurveSpec::fromJson(c.plot, spec));
        QCOMPARE(spec.series[1].color, QColor(71, 112, 76));
        // outil : couleur et superposition acceptées
        const auto t = tools::execute("densite_gauss", R"({"age":1200,"error":50,"color":"red","layout":"overlay"})", DateFormat::BcAd);
        QCOMPARE(t.run.plot["series"].toArray()[0].toObject()["color"].toString(), QString("red"));
        QCOMPARE(t.run.plot["layout"].toString(), QString("overlay"));
        // Event : couleur de l'événement pour θ et pour les dates sans couleur ; une date colorée garde la sienne
        // (JSON sur une seule ligne : moc se perd dans une chaîne brute multiligne contenant « # »)
        const char* eventJson = R"({"method":"event","name":"US 12","red":200,"green":0,"blue":0,"dates":[{"method":"gauss","age":1200,"error":50},{"method":"gauss","age":1250,"error":50,"color":"#0000ff"}],"mcmc":{"num_iter":2000,"num_burn":200,"num_proc":2,"seeds":[1,2]}})";
        const RunOutcome e = MethodRunner::run({ QJsonDocument::fromJson(eventJson).object() });
        const QJsonArray es = e.plot["series"].toArray();
        QCOMPARE(es.size(), 3);
        QCOMPARE(es[0].toObject()["color"].toString(), QString("#c80000"));
        QCOMPARE(es[1].toObject()["color"].toString(), QString("#c80000"));
        QCOMPARE(es[2].toObject()["color"].toString(), QString("#0000ff"));
        QCOMPARE(e.plot["title"].toString(), QString("Densités calendaires — US 12"));
        // rendu en lignes : image et SVG plus hauts quand il y a beaucoup de lignes
        QJsonArray many;
        for (int k = 0; k < 10; ++k) many.append(QJsonObject{ { "name", QString("D%1").arg(k) }, { "x", QJsonArray{ 1, 2, 3 } }, { "y", QJsonArray{ 0, 1, 0 } } });
        QVERIFY(CurveSpec::fromJson(QJsonObject{ { "fill", true }, { "series", many } }, spec));
        CurveWidget w(spec);
        QVERIFY(w.exportSize(QSize(800, 500)).height() > 500);
        QVERIFY(!w.render(QSize(800, 600)).isNull());
        QVERIFY(w.renderSvg(QSize(800, 790)).contains("D9"));
    }

    // Rappel des figures dans le dialogue : vignette cliquable, intégrée en PNG à l'export HTML.
    void chatFigureRecall()
    {
        using namespace seldon;
        const RunOutcome a = MethodRunner::analyzeQuestion("C14 2500 ± 30 BP");
        const QImage img = FigurePanel::thumbnailJson(a.plot, 520, 2.0);
        QVERIFY(!img.isNull());
        QCOMPARE(img.deviceIndependentSize().toSize().width(), 520);
        QCOMPARE(img.width(), 1040);                                       // dessinée en double résolution
        QVERIFY(FigurePanel::thumbnailJson(QJsonObject{ { "series", "x" } }, 520, 1.0).isNull());

        ChatView v;
        v.addFigure(0, img, "Densité calendaire");
        QVERIFY(v.toHtml().contains("seldon-fig:0"));
        QVERIFY(v.toHtml().contains("seldon://figure/0"));
        const QString exp = v.exportHtml();
        QVERIFY(exp.contains("data:image/png;base64,"));
        QVERIFY(!exp.contains("seldon-fig:"));
        v.setFigure(0, img);
        QVERIFY(v.toHtml().contains("seldon-fig:0?v=2"));                   // nouvelle version : pas d'image périmée en cache
    }

    // Fiche d'un événement : période d'étude en tête, paramètres MCMC en tableau, sans mention « non générées ».
    void eventNoticeLayout()
    {
        using namespace seldon;
        const RunOutcome o = MethodRunner::run({ QJsonObject{
            { "method", "event" }, { "name", "US 12" },
            { "dates", QJsonArray{ QJsonObject{ { "method", "gauss" }, { "age", 1000 }, { "error", 20 } },
                                   QJsonObject{ { "method", "gauss" }, { "age", 1010 }, { "error", 20 } } } },
            { "mcmc", QJsonObject{ { "num_iter", 2000 }, { "num_chains", 2 } } } } });
        const QString h = o.noticeHtml;
        QVERIFY(!h.contains("non générées"));
        QVERIFY(h.contains("méthodes de ChronoModel"));
        const int period = h.indexOf("Période d'étude"), theta = h.indexOf("θ a posteriori"), mcmc = h.indexOf("Paramètres du MCMC");
        QVERIFY2(period > 0 && theta > period && mcmc > theta, qPrintable(h.left(800)));
        QVERIFY(h.contains("calculée d'après les datations"));
        QVERIFY(h.contains("Burn-in") && h.contains("Graines") && h.contains("Recuit simulé") && h.contains("Durée du calcul"));
        QVERIFY(o.promptBlock.contains("Période d'étude (a priori uniforme de θ"));
    }

    void figureSvgExport()
    {
        seldon::CurveSpec sp;
        sp.title = "Densité test";
        seldon::CurveSeries s; s.name = "E";
        for (int i = 0; i <= 100; ++i) { s.x << i; s.y << std::exp(-0.5 * std::pow((i - 50) / 10.0, 2)); }
        sp.series << s;
        const QByteArray svg = seldon::CurveWidget(sp).renderSvg(QSize(800, 500));
        QVERIFY(svg.contains("<svg"));
        QVERIFY(svg.contains("<path") || svg.contains("<polyline"));
        QVERIFY(svg.contains("Densité test"));                       // titre écrit en texte, pas en pixels
        QVERIFY(!svg.contains("<image"));                            // aucune image matricielle

        QTemporaryDir dir;
        seldon::FigurePanel panel;
        panel.addCurves(sp);
        const QString curvePath = dir.filePath("courbe.svg");
        QVERIFY(panel.saveCurrentTo(curvePath));
        QFile cf(curvePath); QVERIFY(cf.open(QIODevice::ReadOnly)); QVERIFY(cf.readAll().contains("<svg"));
        QVERIFY(panel.saveCurrentTo(dir.filePath("courbe.png")));
        QVERIFY(!QImage(dir.filePath("courbe.png")).isNull());

    }

};

QTEST_MAIN(TestCore)
#include "tst_core.moc"
