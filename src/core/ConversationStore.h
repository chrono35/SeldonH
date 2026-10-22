// ConversationStore : historique des conversations, un fichier JSON par conversation.
//
// Une conversation contient deux choses :
//   - `turns`   : ce qu'il faut pour REPRENDRE le dialogue (question, texte vu par le modèle, réponse, appels d'outils) ;
//   - `display` : ce qu'il faut pour REDESSINER la zone de réponses (bulles, fiches de calcul, courbes), dans l'ordre.
// Le fichier ne contient ni clé API ni réglage : seulement le contenu des échanges, en clair, sur cet ordinateur.
#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace seldon {

struct StoredTurn {
    QString     question;
    QString     modelUser;
    QString     answer;
    QString     toolBlocks;
    QStringList skillIds;
    bool        followUp = false;   // pas de calcul à relancer pour ce tour
    bool        synthetic = false;  // tour créé par l'application (résultat d'un calcul), pas par une question de l'utilisateur
};

struct DisplayItem {
    enum Kind { User, Assistant, Notice, Plot };
    Kind        kind = Notice;
    QString     text;         // User : texte affiché ; Assistant : Markdown affiché ; Notice : HTML
    QString     thinking;     // Assistant : réflexion du modèle (facultative)
    int         turn = -1;    // Assistant : indice du tour
    QJsonObject plot;         // Plot : JSON de courbes pour le panneau de graphiques
};

struct Conversation {
    QString              id;
    QString              title;
    QDateTime            created;
    QDateTime            updated;
    QString              backend;       // « local » ou « distant : modèle » (information)
    QVector<StoredTurn>  turns;
    QVector<DisplayItem> display;

    bool hasContent() const;                                   // au moins une vraie question
    int  exchanges() const;                                    // nombre de questions de l'utilisateur (hors tours de suite)
    static QString newId();                                    // identifiant unique, sûr comme nom de fichier
    static QString makeTitle(const QString& firstQuestion);    // première ligne, ~60 caractères
};

struct ConversationInfo {
    QString   id;
    QString   title;
    QDateTime updated;
    int       exchanges = 0;
    QString   backend;
};

class ConversationStore {
public:
    explicit ConversationStore(const QString& dir) : dir_(dir) {}
    static QString defaultDir();                   // <dossier de données>/history

    const QString& dir() const { return dir_; }

    // Écriture atomique (fichier temporaire puis renommage) : un arrêt brutal ne laisse pas un fichier tronqué.
    bool save(const Conversation& c, QString* err = nullptr) const;
    bool load(const QString& id, Conversation& out, QString* err = nullptr) const;
    bool remove(const QString& id) const;
    int  removeAll() const;                        // retourne le nombre de conversations effacées

    // Les plus récentes d'abord. Les fichiers illisibles sont ignorés.
    QVector<ConversationInfo> list() const;

    static bool validId(const QString& id);
    static QJsonObject toJson(const Conversation& c);
    static bool fromJson(const QJsonObject& o, Conversation& out, QString* err = nullptr);

private:
    QString pathFor(const QString& id) const;

    QString dir_;
};

} // namespace seldon
