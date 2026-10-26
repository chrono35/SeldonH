// SecretStore : secrets de SeldonH (clé API du modèle distant) rangés dans le trousseau du système, via QtKeychain :
//   macOS   : Trousseau (Security.framework)
//   Windows : Gestionnaire d'identifiants
//   Linux   : Secret Service (GNOME Keyring, KWallet, KeePassXC…) ou KWallet directement
// Repli : réglages QSettings, en clair, quand SeldonH est compilé sans QtKeychain (voir qtkeychain.pri) ou
// qu'aucun trousseau ne répond (Linux sans service de secrets : session minimale, SSH…). L'interface le signale.
#pragma once

#include <QString>

namespace seldon {
namespace secrets {

// Valeur enregistrée pour `key` (vide si absente). Le résultat est gardé en mémoire : un seul accès au trousseau.
QString read(const QString& key);

// Enregistre `value` (vide = suppression). Rien n'est fait si la valeur n'a pas changé depuis la dernière lecture ou
// écriture : AppSettings::save() peut être appelé souvent sans solliciter le trousseau. Si l'utilisateur refuse l'accès
// au trousseau, la valeur n'est PAS écrite en clair à la place (elle reste valable jusqu'à la fermeture de SeldonH).
void write(const QString& key, const QString& value);

// Vrai si les secrets sont (ou seront) rangés dans un trousseau ; faux = repli en clair.
bool isSecure();

// Phrase pour l'interface : où la clé est conservée.
QString storageDescription();

} // namespace secrets
} // namespace seldon
