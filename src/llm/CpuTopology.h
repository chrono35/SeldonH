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

// CpuTopology : nombre de cœurs physiques et de cœurs « performance » de la machine (C++ pur, sans Qt).
//
// llama.cpp est limité par la bande passante mémoire pendant la génération : un thread par cœur physique
// est l'optimum, les threads SMT (hyperthreading) n'apportent rien et la synchronisation entre threads
// ralentit tout. Sur les processeurs hybrides (Intel 12e génération et suivantes, Apple Silicon, ARM
// big.LITTLE), les cœurs « efficacité » freinent les cœurs « performance » à chaque barrière.
// Même logique que cpu_get_num_math() de la bibliothèque common de llama.cpp (non liée par SeldonH).
#pragma once

#include <string>

namespace seldon {

struct CpuTopology {
    int logical     = 1;   // threads matériels (std::thread::hardware_concurrency)
    int physical    = 1;   // cœurs physiques (P + E)
    int performance = 1;   // cœurs physiques « performance » (= physical sur un processeur non hybride)
    std::string source;    // méthode de détection (sysctl, sysfs, Windows, repli)
};

// Topologie de la machine (calculée une fois, puis gardée).
const CpuTopology& cpuTopology();

// Lecture de /sys/devices/system/cpu (Linux) ; root est paramétrable pour les tests. Faux si illisible.
bool readLinuxCpuTopology(const std::string& root, CpuTopology& out);

// Threads utilisés en mode « Automatique » :
//   génération (n_threads)        : cœurs performance ;
//   évaluation du prompt (batch)  : tous les cœurs physiques (calcul pur, les cœurs E aident).
struct AutoThreads { int generation = 1; int batch = 1; };
AutoThreads autoThreads(const CpuTopology& t);

} // namespace seldon
