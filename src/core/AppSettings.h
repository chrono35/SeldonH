#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QString>

namespace seldon {

struct AppSettings {
    QString modelPath;               // fichier .gguf ; vide = recherche automatique
    int     nCtx        = 8192;      // contexte (tokens)
    int     nThreads    = 0;         // 0 = automatique
#if defined(__x86_64__) || defined(_M_X64)
    // Macs Intel : le backend Metal produit souvent un texte incohérent (GPU AMD/Intel) -> CPU par défaut.
    int     nGpuLayers  = 0;         // 0 = CPU seul, 999 = tout sur le GPU (Metal)
#else
    int     nGpuLayers  = 999;       // 0 = CPU seul, 999 = tout sur le GPU (Metal)
#endif
    double  temperature = 0.7;
    int     maxTokens   = 1200;
    bool    thinking    = false;     // mode réflexion (plus lent, meilleur sur les raisonnements)
    int     skillChars  = 5000;      // budget de texte des compétences injectées par question
    QString skillsDir;               // dossier des compétences utilisateur
    QString dateFormat  = "bcad";    // format d'affichage des dates (comme ChronoModel) : bcad, bcece, calbp, calb2k, datbp, datb2k, ka, ma
    // Modèle distant (API compatible OpenAI). Quand useRemote est vrai, les questions sont envoyées au serveur indiqué.
    bool    useRemote   = false;
    QString remoteUrl;               // Base URL, ex. https://api.openai.com/v1
    QString remoteKey;               // clé API (stockée en clair dans les réglages de l'utilisateur)
    QString remoteModel;             // nom du modèle chez le fournisseur
    bool    remoteTools = true;      // appel d'outils natif (les calculs de SeldonH sont appelés par le modèle)
    QString userPrompt;              // consignes personnelles ajoutées à chaque question (ex. « réponses courtes »)
    bool    keepHistory = true;      // enregistre automatiquement les conversations (volet Historique), sur cet ordinateur

    static QString dataDir()
    {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    static QString defaultSkillsDir() { return dataDir() + "/skills"; }
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
        d << app + "/../Resources/models"          // bundle macOS : SeldonH.app/Contents/Resources/models
          << app + "/models"                       // à côté de l'exécutable (Linux / Windows)
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
        a.userPrompt  = s.value("gen/userPrompt").toString();
        a.useRemote   = s.value("remote/enabled", false).toBool();
        a.remoteUrl   = s.value("remote/url").toString();
        a.remoteKey   = s.value("remote/key").toString();
        a.remoteModel = s.value("remote/model").toString();
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
        s.setValue("gen/userPrompt", userPrompt);
        s.setValue("remote/enabled", useRemote);
        s.setValue("remote/url", remoteUrl);
        s.setValue("remote/key", remoteKey);
        s.setValue("remote/model", remoteModel);
        s.setValue("remote/tools", remoteTools);
        s.setValue("display/dateFormat", dateFormat);
        s.setValue("history/keep", keepHistory);
    }
};

} // namespace seldon
