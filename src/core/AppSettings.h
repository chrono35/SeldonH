#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QString>

#include "SecretStore.h"

namespace seldon {

struct AppSettings {
    QString modelPath;               // fichier .gguf ; vide = recherche automatique
    int     nCtx        = 8192;      // contexte (tokens)
    int     nThreads    = 0;         // 0 = automatique
#if defined(__APPLE__) && (defined(__x86_64__) || defined(_M_X64))
    // Macs Intel : le backend Metal produit souvent un texte incohérent (GPU AMD/Intel) -> CPU par défaut.
    int     nGpuLayers  = 0;         // 0 = CPU seul, 999 = tout sur le GPU (Metal)
#else
    // 999 = tout sur le GPU (Metal sur Mac Apple Silicon ; Vulkan ou CUDA sous Windows / Linux si llama.cpp a été
    // compilé avec). Avec une version « cpu » de llama.cpp, aucun GPU n'est trouvé et tout reste sur le processeur.
    int     nGpuLayers  = 999;       // 0 = CPU seul
#endif
    double  temperature = 0.7;
    int     maxTokens   = 1200;
    bool    thinking    = false;     // mode réflexion (plus lent, meilleur sur les raisonnements)
    int     skillChars  = 6000;      // budget de texte des compétences injectées par question (≈ 1 700 tokens)
    QString skillsDir;               // dossier des compétences utilisateur
    QString curvesDir;               // dossier des courbes de référence de l'utilisateur (sous-dossiers 14C, F14C, AM)
    bool    useChronoModelCurves = true;   // lit aussi les courbes installées par ChronoModel (dossier Calib)
    QString dateFormat  = "bcad";    // format d'affichage des dates (comme ChronoModel) : bcad, bcece, calbp, calb2k, datbp, datb2k, ka, ma
    // Modèle distant (API compatible OpenAI). Quand useRemote est vrai, les questions sont envoyées au serveur indiqué.
    bool    useRemote   = false;
    QString remoteUrl   = defaultRemoteUrl();    // Base URL, ex. https://api.openai.com/v1
    QString remoteKey;               // clé API, rangée dans le trousseau du système (SecretStore) ; vide = sans clé
    QString remoteModel = defaultRemoteModel();  // nom du modèle chez le fournisseur
    bool    remoteTools = true;      // appel d'outils natif (les calculs de SeldonH sont appelés par le modèle)
    QString userPrompt;              // consignes personnelles ajoutées à chaque question (ex. « réponses courtes »)
    bool    keepHistory = true;      // enregistre automatiquement les conversations (volet Historique), sur cet ordinateur

    // Serveur distant proposé par défaut (API compatible OpenAI, sans clé)
    static QString defaultRemoteUrl()   { return QStringLiteral("https://oai.endpoints.kepler.ai.cloud.ovh.net/v1"); }
    static QString defaultRemoteModel() { return QStringLiteral("Mistral-Small-3.2-24B-Instruct-2506"); }

    static QString dataDir()
    {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    static QString defaultSkillsDir() { return dataDir() + "/skills"; }
    static QString defaultCurvesDir() { return dataDir() + "/curves"; }
    // Dossier des courbes de ChronoModel (PluginAbstract::AppPluginLibrary() + "/Calib") :
    // GenericDataLocation/CNRS/ChronoModel/Calib, soit sur macOS ~/Library/Application Support/CNRS/ChronoModel/Calib.
    static QString chronoModelCalibDir()
    {
        const QStringList l = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
        return l.isEmpty() ? QString() : l.first() + QStringLiteral("/CNRS/ChronoModel/Calib");
    }
    // Dossiers de courbes à lire, par priorité : utilisateur, puis ChronoModel (s'il existe et si l'option est cochée).
    QStringList curveRoots() const
    {
        QStringList r{ curvesDir };
        const QString cm = chronoModelCalibDir();
        if (useChronoModelCurves && !cm.isEmpty() && QDir(cm).exists()) r << cm;
        return r;
    }
    static QString defaultModelsDir() { return dataDir() + "/models"; }

    // Dossier "models" du projet (défini à la compilation par SeldonH.pro) : pratique en développement.
    static QString projectModelsDir()
    {
#ifdef SELDON_PROJECT_DIR
        return QStringLiteral(SELDON_PROJECT_DIR) + "/models";
#else
        return {};
#endif
    }

    // Dossiers où chercher un modèle, par ordre de priorité.
    static QStringList modelSearchDirs()
    {
        const QString app = QCoreApplication::applicationDirPath();
        QStringList d;
        if (!projectModelsDir().isEmpty()) d << projectModelsDir();
        d << app + "/../Resources/models";         // bundle macOS : SeldonH.app/Contents/Resources/models
#ifdef Q_OS_MACOS
        // macOS installé par QtIFW : /Applications/SeldonH/models, à côté de SeldonH.app
        // (hors du bundle, pour ne pas invalider sa signature)
        d << QDir::cleanPath(app + "/../../../models");
#endif
        d << app + "/models"                       // à côté de l'exécutable (Linux / Windows)
          << defaultModelsDir();
        return d;
    }

    // Dossier où l'utilisateur dépose ses modèles (celui du projet s'il existe, sinon le dossier de données).
    static QString modelsDropDir()
    {
        if (!projectModelsDir().isEmpty() && QDir(projectModelsDir()).exists()) return projectModelsDir();
        return defaultModelsDir();
    }

    static QString findEmbeddedModel()
    {
        for (const QString& d : modelSearchDirs()) {
            const QStringList f = QDir(d).entryList({ "*.gguf" }, QDir::Files, QDir::Name);
            if (!f.isEmpty()) return QDir(d).absoluteFilePath(f.first());
        }
        return {};
    }

    static AppSettings load()
    {
        QSettings s;
        AppSettings a;
        a.modelPath   = s.value("model/path").toString();
        a.nCtx        = s.value("model/nCtx", a.nCtx).toInt();
        a.nThreads    = s.value("model/nThreads", a.nThreads).toInt();
        a.nGpuLayers  = s.value("model/nGpuLayers", a.nGpuLayers).toInt();
        a.temperature = s.value("gen/temperature", a.temperature).toDouble();
        a.maxTokens   = s.value("gen/maxTokens", a.maxTokens).toInt();
        a.thinking    = s.value("gen/thinking", a.thinking).toBool();
        a.skillChars  = s.value("skills/chars", a.skillChars).toInt();
        a.skillsDir   = s.value("skills/dir", defaultSkillsDir()).toString();
        a.curvesDir   = s.value("curves/dir", defaultCurvesDir()).toString();
        a.useChronoModelCurves = s.value("curves/chronomodel", true).toBool();
        a.userPrompt  = s.value("gen/userPrompt").toString();
        a.useRemote   = s.value("remote/enabled", false).toBool();
        a.remoteUrl   = s.value("remote/url").toString().trimmed();
        if (a.remoteUrl.isEmpty()) a.remoteUrl = defaultRemoteUrl();          // champ vide = serveur par défaut
        a.remoteKey   = secrets::read(QStringLiteral("remote/key"));        // trousseau (repli : réglages, en clair)
        a.remoteModel = s.value("remote/model").toString().trimmed();
        if (a.remoteModel.isEmpty()) a.remoteModel = defaultRemoteModel();
        a.remoteTools = s.value("remote/tools", true).toBool();
        a.dateFormat  = s.value("display/dateFormat", a.dateFormat).toString();
        a.keepHistory = s.value("history/keep", true).toBool();
        return a;
    }

    void save() const
    {
        QSettings s;
        s.setValue("model/path", modelPath);
        s.setValue("model/nCtx", nCtx);
        s.setValue("model/nThreads", nThreads);
        s.setValue("model/nGpuLayers", nGpuLayers);
        s.setValue("gen/temperature", temperature);
        s.setValue("gen/maxTokens", maxTokens);
        s.setValue("gen/thinking", thinking);
        s.setValue("skills/chars", skillChars);
        s.setValue("skills/dir", skillsDir);
        s.setValue("curves/dir", curvesDir);
        s.setValue("curves/chronomodel", useChronoModelCurves);
        s.setValue("gen/userPrompt", userPrompt);
        s.setValue("remote/enabled", useRemote);
        s.setValue("remote/url", remoteUrl);
        secrets::write(QStringLiteral("remote/key"), remoteKey);           // sans effet si la clé n'a pas changé
        s.setValue("remote/model", remoteModel);
        s.setValue("remote/tools", remoteTools);
        s.setValue("display/dateFormat", dateFormat);
        s.setValue("history/keep", keepHistory);
    }
};

} // namespace seldon
