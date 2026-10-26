#include "SecretStore.h"

#include <QCoreApplication>
#include <QHash>
#include <QSettings>
#include <QtGlobal>

#ifdef SELDON_HAS_QTKEYCHAIN
#include <QEventLoop>
#include <QTimer>
#include <memory>
#include <qt6keychain/keychain.h>
#endif

namespace seldon {
namespace secrets {
namespace {

QHash<QString, QString>& cache()
{
    static QHash<QString, QString> c;
    return c;
}

QString tr(const char* s) { return QCoreApplication::translate("SecretStore", s); }

QString plainRead(const QString& key) { return QSettings().value(key).toString(); }

void plainWrite(const QString& key, const QString& value)
{
    QSettings s;
    if (value.isEmpty()) s.remove(key);
    else s.setValue(key, value);
}

#ifdef SELDON_HAS_QTKEYCHAIN
bool& noBackend()           // aucun trousseau ne répond (constaté lors d'un accès) : repli en clair pour la session
{
    static bool f = false;
    return f;
}

bool keychainUsable()
{
    static const bool available = QKeychain::isAvailable();   // Linux : interroge D-Bus une seule fois
    return available && !noBackend();
}

QString service()
{
    const QString n = QCoreApplication::applicationName();
    return n.isEmpty() ? QStringLiteral("SeldonH") : n;
}

struct Outcome {
    bool             done  = false;
    QKeychain::Error error = QKeychain::OtherError;
    QString          text;
};

// Exécute un job QtKeychain de façon synchrone (AppSettings lit et écrit tout d'un bloc). Le job se supprime
// lui-même : s'il ne répond pas dans le délai (fenêtre de déverrouillage laissée ouverte…), on continue sans lui.
Outcome run(QKeychain::Job* job)
{
    auto out = std::make_shared<Outcome>();
    QEventLoop loop;
    job->setAutoDelete(true);
    QObject::connect(job, &QKeychain::Job::finished, &loop, [out, &loop](QKeychain::Job* j) {
        out->done  = true;
        out->error = j->error();
        if (auto* r = qobject_cast<QKeychain::ReadPasswordJob*>(j)) out->text = r->textData();
        loop.quit();
    });
    QTimer::singleShot(60000, &loop, &QEventLoop::quit);
    job->start();                                       // démarrage différé (file d'événements) : la boucle est prête
    if (!out->done) loop.exec(QEventLoop::ExcludeUserInputEvents);
    return *out;
}

bool isNoBackend(const Outcome& o)
{
    return o.done && (o.error == QKeychain::NoBackendAvailable || o.error == QKeychain::NotImplemented);
}
#endif

} // namespace

QString read(const QString& key)
{
    auto& c = cache();
    const auto it = c.constFind(key);
    if (it != c.constEnd()) return it.value();

    QString v;
#ifdef SELDON_HAS_QTKEYCHAIN
    if (keychainUsable()) {
        auto* job = new QKeychain::ReadPasswordJob(service());
        job->setKey(key);
        const Outcome o = run(job);
        if (o.done && o.error == QKeychain::NoError) {
            v = o.text;
        } else if (o.done && o.error == QKeychain::EntryNotFound) {
            v = plainRead(key);                         // éventuelle valeur laissée en clair par un repli précédent
        } else if (isNoBackend(o)) {
            noBackend() = true;
            v = plainRead(key);
        } else {
            qWarning("SeldonH : lecture du trousseau impossible (%s)",
                     o.done ? qPrintable(QString::number(int(o.error))) : "délai dépassé");
        }
        c.insert(key, v);
        return v;
    }
#endif
    v = plainRead(key);
    c.insert(key, v);
    return v;
}

void write(const QString& key, const QString& value)
{
    auto& c = cache();
    const auto it = c.constFind(key);
    if (it != c.constEnd() && it.value() == value) return;

#ifdef SELDON_HAS_QTKEYCHAIN
    if (keychainUsable()) {
        Outcome o;
        bool ok = false;
        if (value.isEmpty()) {
            auto* job = new QKeychain::DeletePasswordJob(service());
            job->setKey(key);
            o  = run(job);
            ok = o.done && (o.error == QKeychain::NoError || o.error == QKeychain::EntryNotFound);
        } else {
            auto* job = new QKeychain::WritePasswordJob(service());
            job->setKey(key);
            job->setTextData(value);
            o  = run(job);
            ok = o.done && o.error == QKeychain::NoError;
        }
        if (ok) {
            plainWrite(key, QString());                 // jamais de copie en clair à côté du trousseau
            c.insert(key, value);
            return;
        }
        if (!isNoBackend(o)) {
            // Accès refusé ou délai dépassé : on n'écrit pas la clé en clair à la place.
            qWarning("SeldonH : écriture dans le trousseau impossible (%s)",
                     o.done ? qPrintable(QString::number(int(o.error))) : "délai dépassé");
            c.insert(key, value);                       // pas de nouvelle demande à chaque enregistrement des réglages
            return;
        }
        noBackend() = true;                             // pas de trousseau : repli en clair (signalé dans les réglages)
    }
#endif
    plainWrite(key, value);
    c.insert(key, value);
}

bool isSecure()
{
#ifdef SELDON_HAS_QTKEYCHAIN
    return keychainUsable();
#else
    return false;
#endif
}

QString storageDescription()
{
#ifdef SELDON_HAS_QTKEYCHAIN
    if (keychainUsable()) {
#if defined(Q_OS_MACOS)
        return tr("Clé conservée dans le trousseau de macOS.");
#elif defined(Q_OS_WIN)
        return tr("Clé conservée dans le Gestionnaire d'identifiants de Windows.");
#else
        return tr("Clé conservée dans le trousseau du bureau (GNOME Keyring, KWallet…).");
#endif
    }
    return tr("Aucun trousseau disponible sur cet ordinateur : la clé est enregistrée en clair dans les réglages de SeldonH.");
#else
    return tr("SeldonH a été compilé sans QtKeychain : la clé est enregistrée en clair dans les réglages de SeldonH.");
#endif
}

} // namespace secrets
} // namespace seldon
