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
