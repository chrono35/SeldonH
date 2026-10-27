/* ---------------------------------------------------------------------

Copyright © CNRS	2014 - 2026

Authors :
	Philippe LANOS
	Helori LANOS
	Philippe DUFRESNE

This software is a computer program whose purpose is to help
archaeologists and geologists date samples, compute events, build
ChronoModel models and interpret their results, with a local or
remote language model.

This file contains dating methods derived from ChronoModel
(https://chronomodel.com), distributed under the same license.

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

// EventMcmc : estimation bayésienne d'un Événement (Event) à partir de plusieurs datations, éventuellement de
// méthodes différentes (14C, TL/OSL, archéomagnétisme, Gauss…), par MCMC.
//
// Port du moteur de ChronoModel 4.0 (CNRS, GPL-3.0-or-later) pour UN événement sans contrainte :
//   - modèle d'événement EDM2 (défaut de ChronoModel) : y_i = t_i + δ_i ~ N(θ, σ_i²), θ uniforme sur la période
//     d'étude, σ_i² de loi a priori « shrinkage » de paramètre S0², S0² ~ Inv-Gamma(1, β) ;
//   - initialisation : MCMCLoop::initialize_time() ;
//   - échantillonneur : MCMCLoopChrono::sampler_339_SingleSite_bloc_delta() (bloc (t_i, σ_i, δ_i) par date, noyaux
//     A.1 « calibration + shrinkage » / B.2 « non centré + marche sur log10 V », décalage rigide θ + t_i, Gibbs de θ) ;
//   - régénération par recuit (tempering) pendant l'acquisition : MCMCLoopChrono::tempering_339_ti_marg(),
//     T = 2^e pour e = anneal_temp … 0, tous les anneal_recurrence itérations ;
//   - adaptation des pas (MHVariable::adapt, fenêtre glissante, Robbins-Monro) ;
//   - diagnostics de convergence : split-R̂ rang-normalisé et replié, ESS bulk et tail (MCMCDiagnostic,
//     Vehtari, Gelman, Simpson, Carpenter & Bürkner 2021).
// Sans dépendance Qt (testable seul).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Density.h"

namespace seldon {

// Décalage δ entre la date mesurée et l'événement (wiggle-matching) : y = t + δ.
struct Wiggle {
    enum Type { None, Fixed, Range, Gaussian } type = None;
    double fixed = 0.0;              // Fixed
    double min = 0.0, max = 0.0;     // Range
    double mean = 0.0, error = 0.0;  // Gaussian
};

// Densité de y = t + δ pour UNE date (Date::calibrateWiggle de ChronoModel) : convolution de la densité calibrée
// par la loi de δ — translation (fixed), porte uniforme [min ; max] (range), noyau N(mean, error) (gaussian). Aire 1.
Density wiggleDensity(const Density& calib, const Wiggle& w);

// Paramètres MCMC : valeurs par défaut de ChronoModel (mcmc/MCMCSettings.h).
struct McmcSettings {
    int    numChains        = 4;        // MCMC_NUM_CHAINS_DEFAULT
    int    numBurn          = 1000;     // MCMC_NUM_BURN_DEFAULT
    int    maxBatches       = 200;      // MCMC_MAX_ADAPT_BATCHES_DEFAULT
    int    iterPerBatch     = 100;      // MCMC_ITER_PER_BATCH_DEFAULT
    int    numIter          = 100000;   // MCMC_NUM_RUN_DEFAULT (acquisition)
    int    thinning         = 10;       // MCMC_THINNING_INTERVAL_DEFAULT
    double mixingLevel      = 0.99;     // MCMC_MIXING_DEFAULT
    int    annealTemp       = 10;       // MCMC_ANNEAL_TEMP (T max = 2^10)
    int    annealRecurrence = 1000;     // MCMC_ANNEAL_RECURRENCE (0 = pas de recuit)
    int    annealDwell      = 1000;     // MCMC_ANNEAL_DWELL
    std::vector<unsigned> seeds;        // une graine par chaîne ; tirées au hasard si absentes
};

struct EventDateInput {
    std::string name;
    Density     calib;                  // densité calibrée de la date (aire 1)
    Wiggle      wiggle;
};

struct VarDiagnostic {
    std::string name;                   // « θ », « t(Ech-1) », « σ(Ech-1) »
    double rhat = 0.0;                  // max(bulk-R̂, tail-R̂) ; 0 si une seule chaîne
    double bulkEss = 0.0, tailEss = 0.0;
    double acceptRate = -1.0;           // taux d'acceptation en acquisition (−1 : sans objet, Gibbs)
};

struct ChainReport {
    unsigned seed = 0;
    int      adaptBatches = 0;          // nombre de lots d'adaptation effectués
    bool     adapted = false;           // adaptation terminée avant maxBatches
};

enum class Convergence { Good, Warning, Bad, Unknown };

struct EventMcmcResult {
    bool        ok = false;
    std::string error;
    double      tmin = 0.0, tmax = 0.0; // période d'étude (loi a priori de θ)
    Density     theta;                  // densité a posteriori de θ (toutes chaînes)
    std::vector<Density> ti;            // densités a posteriori des t_i (date de l'échantillon)
    std::vector<Density> yi;            // densités a posteriori de t_i + δ_i (« Wiggle » de ChronoModel) ; = ti sans décalage
    std::vector<double>  sigmaMedian;   // médiane a posteriori de σ_i (dispersion de chaque date autour de θ)
    double      thetaBandwidth = 0.0;   // largeur de bande h (années) du noyau gaussien qui lisse la densité de θ
    bool        thetaBandwidthSJ = true; // h de Sheather-Jones (faux : repli sur la règle de Silverman)
    std::vector<double>  tiBandwidth;   // h des densités des t_i
    std::vector<std::vector<double>> thetaTraces;   // traces de θ en acquisition (après thinning), une par chaîne
    std::vector<VarDiagnostic> diagnostics;
    std::vector<ChainReport>   chains;
    Convergence status = Convergence::Unknown;
    Convergence rhatStatus = Convergence::Unknown, essStatus = Convergence::Unknown;
    double      maxRhat = 0.0, minEss = 0.0;
    double      elapsedMs = 0.0;
};

// Avancement (0..1). Retourner false interrompt le calcul.
using McmcProgress = std::function<bool(double)>;

// Lance les chaînes (en parallèle) et renvoie θ a posteriori et les diagnostics. [tmin, tmax] : période d'étude.
EventMcmcResult runEventMcmc(const std::vector<EventDateInput>& dates, const McmcSettings& settings,
                             double tmin, double tmax, const McmcProgress& progress = {});

namespace mcmcdiag {

// Seuils de ChronoModel (MCMCDiagnostic::Threshold).
constexpr double kRhatGood = 1.01, kRhatWarning = 1.05;
constexpr double kEssGoodPerChain = 500.0, kEssWarningPerChain = 100.0;

struct RhatEss { double rhat = 0.0, bulkEss = 0.0, tailEss = 0.0; };
// M chaînes de même longueur N ≥ 4.
RhatEss computeRhatAndEss(const std::vector<std::vector<double>>& chains);

} // namespace mcmcdiag

// Règle de largeur de bande du noyau gaussien.
//   SheatherJones : Sheather & Jones (1991) « solve-the-equation » (bw.SJ de R) — défaut ; respecte la multimodalité
//                   (résultats comparables à ChronoModel). Repli sur Silverman si l'équation n'a pas de solution.
//   Silverman     : règle « nrd0 » de R, 0.9·min(sd, IQR/1.34)·n^(−1/5) ; sur-lisse les densités multimodales.
enum class Bandwidth { SheatherJones, Silverman };

// Largeur de bande h pour ces échantillons (n ≥ 2 ; 0 sinon). `used` : règle réellement appliquée (repli possible).
double kernelBandwidth(const std::vector<double>& samples, Bandwidth rule = Bandwidth::SheatherJones, Bandwidth* used = nullptr);

// Densité par noyau gaussien, sur ~1024 points, restreinte à [lo, hi]. `bandwidth`, `used` : h et règle utilisés.
Density kernelDensity(const std::vector<double>& samples, double lo, double hi,
                      Bandwidth rule = Bandwidth::SheatherJones, double* bandwidth = nullptr, Bandwidth* used = nullptr);

} // namespace seldon
