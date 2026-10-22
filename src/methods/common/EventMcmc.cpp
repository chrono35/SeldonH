#include "EventMcmc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <thread>

namespace seldon {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kLogSqrt2Pi = 0.9189385332046727;

// ------------------------------------------------------------------ lois normales

inline double normalCDF(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

// Acklam + un pas de Newton (identique à MetropolisVariable.cpp de ChronoModel).
double invNormalCDF(double p)
{
    if (p <= 0.0) return -kInf;
    if (p >= 1.0) return kInf;
    static const double a[] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                                1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
    static const double b[] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                                6.680131188771972e+01, -1.328068155288572e+01 };
    static const double c[] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                                -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
    static const double d[] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                                3.754408661907416e+00 };
    const double plow = 0.02425, phigh = 1.0 - plow;
    double x;
    if (p < plow) {
        const double q = std::sqrt(-2.0 * std::log(p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    } else if (p <= phigh) {
        const double q = p - 0.5, r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
    } else {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    const double e = 0.5 * std::erfc(-x / std::sqrt(2.0)) - p;
    const double u = e * 2.5066282746310002 * std::exp(x * x / 2.0);
    return x - u / (1.0 + x * u / 2.0);
}

inline double logDnorm(double x, double mu, double s)
{
    const double z = (x - mu) / s;
    return -0.5 * z * z - std::log(s) - kLogSqrt2Pi;
}

inline double dnorm(double x, double mu, double s) { return std::exp(logDnorm(x, mu, s)); }

// log(Φ(b) − Φ(a)), stable dans les queues (utilities/StdUtilities.cpp : log_diff_cdf).
double logDiffCdf(double a, double b)
{
    if (a >= b) return -kInf;
    const double diff = normalCDF(b) - normalCDF(a);
    if (diff > 1e-300) return std::log(diff);
    if (a > 5.0) {
        const double la = -0.5 * a * a - std::log(a) - kLogSqrt2Pi;
        const double lb = -0.5 * b * b - std::log(b) - kLogSqrt2Pi;
        return la + std::log1p(-std::exp(lb - la));
    }
    if (b < -5.0) {
        const double la = -0.5 * a * a - std::log(-a) - kLogSqrt2Pi;
        const double lb = -0.5 * b * b - std::log(-b) - kLogSqrt2Pi;
        return lb + std::log1p(-std::exp(la - lb));
    }
    return -700.0;
}

// ------------------------------------------------------------------ générateur

struct Rng {
    std::mt19937 eng;
    std::uniform_real_distribution<double> u01{ 0.0, 1.0 };
    std::normal_distribution<double> n01{ 0.0, 1.0 };
    explicit Rng(unsigned seed) : eng(seed) {}

    double uniform() { return u01(eng); }
    double uniform(double a, double b) { return a + (b - a) * uniform(); }
    double normal(double mu, double s) { return mu + s * n01(eng); }

    // Normale tronquée sur [lo, hi] : inversion de la fonction de répartition, et algorithme de Robert (1995)
    // dans les queues lointaines.
    double truncatedNormal(double mu, double s, double lo, double hi)
    {
        if (!(s > 0.0) || lo >= hi) return std::clamp(mu, lo, hi);
        const double a = (lo - mu) / s, b = (hi - mu) / s;
        if (a > 5.0 || b < -5.0) {
            const bool right = a > 5.0;
            const double lo2 = right ? a : -b, hi2 = right ? b : -a;
            const double lambda = 0.5 * (lo2 + std::sqrt(lo2 * lo2 + 4.0));
            for (int guard = 0; guard < 10000; ++guard) {
                const double z = lo2 - std::log(1.0 - uniform()) / lambda;
                if (z > hi2) continue;
                if (uniform() <= std::exp(-0.5 * (z - lambda) * (z - lambda))) {
                    const double x = right ? z : -z;
                    return std::clamp(mu + s * x, lo, hi);
                }
            }
            return right ? lo : hi;
        }
        const double pa = normalCDF(a), pb = normalCDF(b);
        const double p = pa + (pb - pa) * uniform();
        return std::clamp(mu + s * invNormalCDF(std::clamp(p, 1e-300, 1.0 - 1e-16)), lo, hi);
    }

    // Generator::shrinkageUniforme : V de densité s0²/(s0²+V)² sur [minV, maxV].
    double shrinkageUniforme(double s02, double minV = 1e-12, double maxV = 1e10)
    {
        const double umin = s02 / (maxV + s02), umax = s02 / (minV + s02);
        const double u = uniform(umin, umax);
        return s02 * ((1.0 - u) / u);
    }

    bool acceptLog(double logRate)
    {
        if (std::isnan(logRate) || logRate == -kInf) return false;
        if (logRate >= 0.0) return true;
        return std::log(uniform()) < logRate;
    }
};

// ------------------------------------------------------------------ densité calibrée sur grille régulière

struct Calib {
    double tmin = 0.0, tmax = 0.0, step = 1.0;
    std::vector<double> v, rep;          // densité ; répartition (0..1)
    double sd = 0.0;

    static Calib from(const Density& d)
    {
        Calib c;
        c.tmin = d.t.front();
        c.tmax = d.t.back();
        c.step = d.t.size() > 1 ? (c.tmax - c.tmin) / double(d.t.size() - 1) : 1.0;
        c.v = d.v;
        c.rep.assign(c.v.size(), 0.0);
        double acc = 0.0;
        for (size_t i = 1; i < c.v.size(); ++i) { acc += 0.5 * (c.v[i - 1] + c.v[i]) * c.step; c.rep[i] = acc; }
        if (acc > 0.0) for (double& r : c.rep) r /= acc;
        c.sd = densityStats(d).sd;
        return c;
    }

    double at(double t) const
    {
        if (!(t >= tmin && t <= tmax) || v.empty()) return 0.0;
        const double x = (t - tmin) / step;
        const size_t i = size_t(x);
        if (i + 1 >= v.size()) return v.back();
        const double f = x - double(i);
        return v[i] * (1.0 - f) + v[i + 1] * f;
    }

    double repAt(double t) const
    {
        if (t <= tmin) return 0.0;
        if (t >= tmax) return 1.0;
        const double x = (t - tmin) / step;
        const size_t i = size_t(x);
        if (i + 1 >= rep.size()) return 1.0;
        const double f = x - double(i);
        return rep[i] * (1.0 - f) + rep[i + 1] * f;
    }

    // Inverse de la répartition.
    double quantile(double u) const
    {
        const auto it = std::lower_bound(rep.begin(), rep.end(), u);
        if (it == rep.begin()) return tmin;
        if (it == rep.end()) return tmax;
        const size_t k = size_t(it - rep.begin());
        const double a = rep[k - 1], b = rep[k];
        const double f = b > a ? (u - a) / (b - a) : 0.0;
        return tmin + (double(k - 1) + f) * step;
    }

    double sample(Rng& r) const { return quantile(r.uniform()); }
};

// Densité de y = t + δ (wiggle) pour l'initialisation de θ (Date::calibrateWiggle).
Calib wiggleCalib(const Calib& c, const Wiggle& w)
{
    if (w.type == Wiggle::None) return c;
    Density d;
    if (w.type == Wiggle::Fixed) {
        for (size_t i = 0; i < c.v.size(); ++i) { d.t.push_back(c.tmin + double(i) * c.step + w.fixed); d.v.push_back(c.v[i]); }
    } else if (w.type == Wiggle::Range) {
        const double lo = c.tmin + w.min, hi = c.tmax + w.max, width = std::max(w.max - w.min, 1e-9);
        const long n = std::max(2L, std::lround((hi - lo) / c.step));
        for (long i = 0; i <= n; ++i) {
            const double y = lo + (hi - lo) * double(i) / double(n);
            d.t.push_back(y);
            d.v.push_back((c.repAt(y - w.min) - c.repAt(y - w.max)) / width);
        }
    } else {
        const double s = std::max(w.error, 1e-9);
        const double lo = c.tmin + w.mean - 4.0 * s, hi = c.tmax + w.mean + 4.0 * s;
        const long n = std::max(2L, std::min(20000L, std::lround((hi - lo) / c.step)));
        for (long i = 0; i <= n; ++i) {
            const double y = lo + (hi - lo) * double(i) / double(n);
            double acc = 0.0;
            for (size_t k = 0; k < c.v.size(); ++k)
                if (c.v[k] > 0.0) acc += c.v[k] * dnorm(y - (c.tmin + double(k) * c.step), w.mean, s) * c.step;
            d.t.push_back(y);
            d.v.push_back(acc);
        }
    }
    double area = 0.0;
    for (size_t i = 1; i < d.v.size(); ++i) area += 0.5 * (d.v[i - 1] + d.v[i]) * (d.t[i] - d.t[i - 1]);
    if (area > 0.0) for (double& x : d.v) x /= area;
    return Calib::from(d);
}

} // namespace

Density wiggleDensity(const Density& calib, const Wiggle& w)
{
    if (w.type == Wiggle::None || !calib.valid()) return calib;
    const Calib c = wiggleCalib(Calib::from(calib), w);
    Density d;
    for (size_t i = 0; i < c.v.size(); ++i) { d.t.push_back(c.tmin + double(i) * c.step); d.v.push_back(c.v[i]); }
    return d;
}

namespace {

// Variance intra-mode minimale (utilities/StdUtilities.cpp : computeIntraModeVarianceMin, seuil 5 %, ≥ 3 points).
bool intraModeVarianceMin(const Calib& c, double& out)
{
    if (c.v.empty()) return false;
    const double thr = 0.05 * *std::max_element(c.v.begin(), c.v.end());
    // Îlot portant moins de 1 % de la masse ignoré (même règle que ChronoModel) : un petit pic isolé donnerait
    // une variance minuscule, donc β(S0²) quasi nul et des σ_i écrasés vers 0.
    constexpr double kMinMassFraction = 0.01;
    double total = 0.0;
    for (const double v : c.v) total += v;
    double minVar = std::numeric_limits<double>::max();
    bool found = false;
    // Repli : îlot de plus grande masse, si aucun n'atteint 1 % (ex. plus de 100 pics identiques).
    double bigMass = -1.0, bigVar = 0.0;
    size_t i = 0;
    while (i < c.v.size()) {
        if (c.v[i] > thr) {
            double wk = 0.0, mean = 0.0, m2 = 0.0;
            size_t count = 0;
            while (i < c.v.size() && c.v[i] > thr) {
                const double w = c.v[i], x = c.tmin + double(i) * c.step;
                wk += w;
                const double dl = x - mean;
                mean += (w / wk) * dl;
                m2 += w * dl * (x - mean);
                ++count; ++i;
            }
            if (wk > 0.0 && count >= 3) {
                const double var = std::max(m2 / wk, 0.0);
                if (wk > bigMass) { bigMass = wk; bigVar = var; }
                if (wk >= kMinMassFraction * total) { minVar = std::min(minVar, var); found = true; }
            }
        } else {
            ++i;
        }
    }
    if (found) { out = minVar; return true; }
    if (bigMass > 0.0) { out = bigVar; return true; }
    return false;
}

// ------------------------------------------------------------------ variable Metropolis-Hastings (MHVariable)

struct MHVar {
    double x = 0.0;
    double sigmaMH = 1.0;
    bool   rw = false;                     // SamplerProposal::eRWAdaptGauss : seul cas où l'acceptation est comptée
    std::deque<bool> window;               // mLastMHAccepts
    size_t windowLen = 100;                // = iter_per_batch (Model::setParametersForChain)
    bool   counting = false;               // acquisition : taux global
    long   accepted = 0, proposed = 0;

    void record(bool a)
    {
        if (!rw) return;
        if (window.size() == windowLen) window.pop_front();
        window.push_back(a);
        if (counting) { ++proposed; if (a) ++accepted; }
    }
    void accept(double v) { x = v; record(true); }
    void reject() { record(false); }
    double rate() const
    {
        if (window.empty()) return 0.0;
        return double(std::count(window.begin(), window.end(), true)) / double(window.size());
    }
    // MHVariable::adapt (c = 0,5, κ = 0,6, t0 = 10, σ ∈ [1e-10, 1e8]) ; vrai = pas d'adaptation nécessaire.
    bool adapt(double cmin, double cmax, int batch)
    {
        const double gamma = 0.5 / std::pow(double(batch) + 10.0, 0.6);
        const double r = rate();
        if (r <= cmin || r >= cmax) {
            sigmaMH *= std::pow(10.0, (r <= cmin ? -1.0 : 1.0) * gamma);
            sigmaMH = std::clamp(sigmaMH, 1e-10, 1e8);
            return false;
        }
        return true;
    }
};

struct DateState {
    const Calib* cal = nullptr;
    Wiggle w;
    MHVar  ti, sigma;
    double delta = 0.0;

    double drawDeltaPrior(Rng& r) const
    {
        switch (w.type) {
        case Wiggle::Range:    return r.uniform(w.min, w.max);
        case Wiggle::Gaussian: return r.normal(w.mean, w.error);
        case Wiggle::Fixed:    return w.fixed;
        default:               return 0.0;
        }
    }
};

// ------------------------------------------------------------------ une chaîne

struct ChainOut {
    std::vector<double> theta, s0;
    std::vector<std::vector<double>> ti, sigma, yi;    // yi = t_i + δ_i (variable « Wiggle » de ChronoModel)
    ChainReport report;
    long thetaAcc = 0, thetaProp = 0;
    std::vector<long> tiAcc, tiProp;
    std::string error;
};

class Chain {
public:
    Chain(const std::vector<Calib>& cals, const std::vector<Calib>& wcals, const std::vector<Wiggle>& wiggles,
          const McmcSettings& s, double tmin, double tmax, unsigned seed,
          std::atomic<long>& progress, std::atomic<bool>& stop)
        : cals_(cals), wcals_(wcals), wig_(wiggles), s_(s), tmin_(tmin), tmax_(tmax), rng_(seed),
          progress_(progress), stop_(stop)
    {
        out.report.seed = seed;
    }

    void run()
    {
        init();
        if (!out.error.empty()) return;
        long total = 0;
        // Burn-in
        for (int i = 0; i < s_.numBurn && !stop_; ++i) { update(false, total); ++total; tick(); }
        // Adaptation
        int batch = 0;
        while (batch < s_.maxBatches && !stop_) {
            for (int i = 0; i < s_.iterPerBatch && !stop_; ++i) { update(false, total); ++total; tick(); }
            ++batch;
            if (adapt(batch)) break;
        }
        out.report.adaptBatches = batch;
        out.report.adapted = batch < s_.maxBatches;
        // Acquisition (l'adaptation continue par lots, comme dans MCMCLoop::run)
        theta_.counting = true;
        for (DateState& d : dates_) d.ti.counting = d.sigma.counting = true;
        int thin = 0, batchIdx = 1, totalBatch = batch;
        for (int i = 0; i < s_.numIter && !stop_; ++i) {
            update(true, total);
            ++total; tick();
            if (++thin >= s_.thinning) { thin = 0; acquire(); }
            if (batchIdx == s_.iterPerBatch) { adapt(totalBatch); batchIdx = 1; ++totalBatch; }
            else ++batchIdx;
        }
        out.thetaAcc = theta_.accepted; out.thetaProp = theta_.proposed;
        for (size_t k = 0; k < dates_.size(); ++k) { out.tiAcc[k] = dates_[k].ti.accepted; out.tiProp[k] = dates_[k].ti.proposed; }
    }

    ChainOut out;

private:
    void tick(long n = 1)
    {
        pending_ += n;
        if (pending_ >= 500) { progress_ += pending_; pending_ = 0; }
    }

    // MCMCLoop::initialize_time() pour un événement sans contrainte (EDM2).
    void init()
    {
        const size_t n = cals_.size();
        out.ti.assign(n, {});
        out.sigma.assign(n, {});
        out.yi.assign(n, {});
        out.tiAcc.assign(n, 0);
        out.tiProp.assign(n, 0);
        dates_.resize(n);
        for (DateState& d : dates_) { d.ti.windowLen = d.sigma.windowLen = size_t(std::max(1, s_.iterPerBatch)); }
        theta_.windowLen = s02_.windowLen = size_t(std::max(1, s_.iterPerBatch));

        // θ initial : tiré dans la calibration de mélange (moyenne des répartitions, generate_mixingCalibration)
        // restreinte à [tmin, tmax].
        {
            double lo = kInf, hi = -kInf, step = kInf;
            for (const Calib& c : wcals_) { lo = std::min(lo, c.tmin); hi = std::max(hi, c.tmax); step = std::min(step, c.step); }
            auto mixRep = [&](double t) { double s = 0.0; for (const Calib& c : wcals_) s += c.repAt(t); return s / double(wcals_.size()); };
            const double rmin = mixRep(tmin_), rmax = mixRep(tmax_);
            if (!(rmax > rmin)) {
                theta_.x = rng_.uniform(tmin_, tmax_);
            } else {
                const double u = rng_.uniform(rmin, rmax);
                double a = std::max(lo, tmin_), b = std::min(hi, tmax_);
                for (int it = 0; it < 80; ++it) { const double m = 0.5 * (a + b); (mixRep(m) < u ? a : b) = m; }
                theta_.x = std::clamp(0.5 * (a + b), tmin_, tmax_);
            }
            (void)step;
        }

        const double sigmaU = (tmax_ - tmin_) / 2.0;
        double s02Sum = 0.0, minVarIntra = kInf;
        for (size_t k = 0; k < n; ++k) {
            DateState& d = dates_[k];
            d.cal = &cals_[k];
            d.w = wig_[k];
            double vi;
            if (intraModeVarianceMin(*d.cal, vi)) minVarIntra = std::min(minVarIntra, vi);
            const double sd = d.cal->sd > 0.0 ? d.cal->sd : d.cal->step;
            // Date::initDelta : uniforme pour « range », N(μ, σ) tronquée à ±5σ pour « gaussian ».
            d.delta = d.w.type == Wiggle::Gaussian
                ? rng_.truncatedNormal(d.w.mean, d.w.error, d.w.mean - 5.0 * d.w.error, d.w.mean + 5.0 * d.w.error)
                : d.drawDeltaPrior(rng_);

            // t_i initial : calibration pondérée par N(θ, σU)
            std::vector<double> rep(d.cal->v.size());
            double acc = 0.0;
            for (size_t i = 0; i < d.cal->v.size(); ++i) {
                acc += dnorm(d.cal->tmin + double(i) * d.cal->step, theta_.x, sigmaU) * d.cal->v[i];
                rep[i] = acc;
            }
            double ti;
            if (acc > 0.0) {
                const double u = rng_.uniform(0.0, acc);
                const auto it = std::lower_bound(rep.begin(), rep.end(), u);
                double idx = double(it - rep.begin());
                if (it != rep.begin() && it != rep.end()) {
                    const size_t j = size_t(it - rep.begin());
                    const double a = rep[j - 1], b = rep[j];
                    idx = double(j - 1) + (b > a ? (u - a) / (b - a) : 0.0);
                }
                ti = std::clamp(d.cal->tmin + idx * d.cal->step, d.cal->tmin, d.cal->tmax);
            } else {
                ti = d.cal->sample(rng_);
            }
            d.ti.sigmaMH = n > 1 ? 2.38 : 2.38 * sd;          // optimum de Roberts
            d.ti.accept(ti);
            d.sigma.x = std::max(std::abs(d.ti.x - (theta_.x - d.delta)), 1.0e-6);
            d.sigma.sigmaMH = 0.1;
            s02Sum += 1.0 / (sd * sd);
        }
        const double s02Harm = double(n) / s02Sum;
        betaS02_ = std::isfinite(minVarIntra) && minVarIntra > 0.0          // aucun mode détecté : repli harmonique
            ? 1.004680139 * (1.0 - std::exp(-0.0000847244 * std::pow(minVarIntra, 1.1867742965)))
            : 1.004680139 * (1.0 - std::exp(-0.0000847244 * std::pow(s02Harm, 1.1867742965)));
        if (!std::isfinite(betaS02_) || !(betaS02_ > 0.0)) { out.error = "β(S0²) invalide"; return; }
        s02_.rw = true;                                        // EDM2 : S0² échantillonné (marche sur log10)
        s02_.sigmaMH = 0.1;
        s02_.accept(betaS02_ / 2.0);                           // mode de la loi inverse gamma
        theta_.sigmaMH = 2.38 * std::sqrt(s02Harm);
    }

    double logHS02(double s02) const                       // Event::log_h_S02 (jacobien compris)
    {
        if (!(s02 > 0.0)) return kNaN;
        double lh = -betaS02_ / s02 - std::log(s02);
        for (const DateState& d : dates_) lh += std::log(s02) - 2.0 * std::log(s02 + d.sigma.x * d.sigma.x);
        return lh;
    }

    // Statistiques pondérées (P = Σ 1/σ², μ, S) des y_i = t_i + δ_i.
    void weightedStats(long double& P, long double& mu, long double& S) const
    {
        P = mu = S = 0.0L;
        for (const DateState& d : dates_) {
            const long double sg = d.sigma.x, y = (long double)d.ti.x + d.delta, w = 1.0L / (sg * sg);
            if (P == 0.0L) { P = w; mu = y; S = 0.0L; }
            else {
                const long double Pn = P + w, dl = y - mu, mun = mu + (w / Pn) * dl;
                S += w * dl * (y - mun);
                P = Pn; mu = mun;
            }
        }
    }

    // MCMCLoopChrono::update_v3_block_simulated_annealing
    void update(bool acquisition, long iteration)
    {
        if (acquisition && s_.annealRecurrence > 0 && iteration > 0 && iteration % s_.annealRecurrence == 0) {
            for (int e = s_.annealTemp; e >= 0; --e) {
                const double T = std::pow(2.0, e);
                const int dwell = std::max(1, int(std::ceil(s_.annealDwell / (1.0 + e))));
                for (int k = 0; k < dwell; ++k) tempering(T);
                tick(dwell);
            }
        }
        samplerBlocDelta();
    }

    double proposalDensity(const DateState& d, double t, double t0) const   // Date::fProposalDensity
    {
        const double s = std::max(tmax_ - tmin_, d.cal->tmax - d.cal->tmin) / 2.0;
        const double qg = dnorm(t, t0, s);
        if (t >= d.cal->tmin && t <= d.cal->tmax) return s_.mixingLevel * d.cal->at(t) + (1.0 - s_.mixingLevel) * qg;
        return (1.0 - s_.mixingLevel) * qg;
    }

    double proposeTiIndependent(const DateState& d)
    {
        if (rng_.uniform() < s_.mixingLevel) return d.cal->sample(rng_);
        const double s = std::max(tmax_ - tmin_, d.cal->tmax - d.cal->tmin) / 2.0;
        return rng_.normal(d.ti.x, s);
    }

    // MCMCLoopChrono::sampler_339_SingleSite_bloc_delta (un événement, θ non fixé, min < max).
    void samplerBlocDelta()
    {
        const double min = tmin_, max = tmax_;
        long double P, mu, S;
        weightedStats(P, mu, S);
        constexpr double mixingKernel = 0.8;

        for (DateState& d : dates_) {
            const double ti0 = d.ti.x, delta0 = d.delta, sig0 = d.sigma.x;
            double tiP = ti0, sigP = sig0, lrL = 0.0, lrQt = 0.0, lrQs = 0.0;
            bool ok = true;

            const long double V1 = (long double)sig0 * sig0, w0 = 1.0L / V1, y0 = (long double)ti0 + delta0;
            const long double Prm = P - w0;
            long double murm = 0.0L, Srm = 0.0L;
            if (Prm > 0.0L) {
                murm = (P * mu - w0 * y0) / Prm;
                Srm = S - w0 * (y0 - mu) * (y0 - murm);
                if (Srm < 0.0L && Srm > -1e-18L) Srm = 0.0L;
            } else {
                murm = y0;
            }
            const double deltaP = d.drawDeltaPrior(rng_);

            if (rng_.uniform() < mixingKernel) {
                // Noyau B.2 : saut non centré (t_i) + marche sur log10(V)
                d.ti.rw = d.sigma.rw = true;
                const double l1 = std::log10(double(V1));
                const double l2 = rng_.normal(l1, d.sigma.sigmaMH);
                if (!std::isfinite(l2) || l2 < -100.0 || l2 > 100.0) {
                    ok = false;
                } else {
                    sigP = std::sqrt(std::pow(10.0, l2));
                    const double sz = d.ti.sigmaMH;
                    if (Prm > 0.0L) {
                        const double ybar = double(murm);
                        const double z0 = (ti0 + delta0 - ybar) / sig0;
                        const double zP = rng_.normal(z0, sz);
                        tiP = ybar + sigP * zP - deltaP;
                        lrQs = 3.0 * std::log(sigP / sig0);
                    } else {
                        tiP = ti0 + rng_.normal(0.0, sz);
                        lrQs = 2.0 * std::log(sigP / sig0);
                    }
                }
            } else {
                // Noyau A.1 : calibration (t_i) + shrinkage exact (σ_i)
                d.ti.rw = d.sigma.rw = false;
                const double S02 = s02_.x;
                constexpr double VMin = 1e-12, VMax = 1e10;
                if (double(V1) < VMin || double(V1) > VMax) ok = false;
                const double V2 = rng_.shrinkageUniforme(S02, VMin, VMax);
                sigP = std::sqrt(V2);
                if (!(sigP > 0.0)) {
                    ok = false;
                } else {
                    tiP = proposeTiIndependent(d);
                    const double qf = proposalDensity(d, tiP, ti0), qr = proposalDensity(d, ti0, tiP);
                    if (!(qf > 0.0) || !(qr > 0.0)) ok = false;
                    else {
                        lrQt = std::log(qr) - std::log(qf);
                        lrQs = -2.0 * (std::log(S02 + double(V1)) - std::log(S02 + V2));
                    }
                }
            }

            if (ok) {
                const double L0 = d.cal->at(ti0), L1 = d.cal->at(tiP);
                if (!(L1 > 0.0) || !(L0 > 0.0)) ok = false;
                else lrL = std::log(L1) - std::log(L0);
            }

            if (!ok) { d.ti.reject(); d.sigma.reject(); continue; }

            const long double V2n = (long double)sigP * sigP, wn = 1.0L / V2n, yn = (long double)tiP + deltaP;
            long double Pn = Prm, mun = murm, Sn = Srm;
            if (Pn == 0.0L) { Pn = wn; mun = yn; Sn = 0.0L; }
            else {
                const long double dl = yn - mun;
                Pn = Prm + wn;
                mun = murm + (wn / Pn) * dl;
                Sn = Srm + wn * dl * (yn - mun);
            }
            if (Sn < 0.0L && Sn > -1e-18L) Sn = 0.0L;

            const long double lsig = std::log((long double)sigP / (long double)sig0);
            const long double sa0 = 1.0L / std::sqrt(P), sa1 = 1.0L / std::sqrt(Pn);
            const double lz0 = logDiffCdf(double((min - mu) / sa0), double((max - mu) / sa0));
            const double lz1 = logDiffCdf(double((min - mun) / sa1), double((max - mun) / sa1));
            if (!std::isfinite(lz0) || !std::isfinite(lz1)) { d.ti.reject(); d.sigma.reject(); continue; }
            const long double lprior = (long double)(lz1 - lz0) + std::log(sa1 / sa0) - lsig - 0.5L * (Sn - S);
            const long double S02 = s02_.x;
            const long double lshrink = 2.0L * (std::log(S02 + V1) - std::log(S02 + V2n));
            const long double total = lprior + lshrink + lrL + lrQt + lrQs;

            if (rng_.acceptLog(double(total))) {
                d.ti.accept(tiP);
                d.sigma.accept(sigP);
                d.delta = deltaP;
                P = Pn; mu = mun; S = Sn;
            } else {
                d.ti.reject();
                d.sigma.reject();
            }
        }

        // Décalage rigide θ + t_i
        if (!dates_.empty()) {
            theta_.rw = true;
            const double dTheta = rng_.normal(0.0, theta_.sigmaMH);
            const double thP = theta_.x + dTheta;
            bool ok = thP >= min && thP <= max;
            long double lr = 0.0L;
            if (ok) {
                for (const DateState& d : dates_) {
                    const double L0 = d.cal->at(d.ti.x), L1 = d.cal->at(d.ti.x + dTheta);
                    if (!(L0 > 0.0) || !(L1 > 0.0)) { ok = false; break; }
                    lr += std::log((long double)L1) - std::log((long double)L0);
                }
            }
            if (ok && rng_.acceptLog(double(lr))) {
                theta_.accept(thP);
                for (DateState& d : dates_) d.ti.x += dTheta;       // sans compter l'acceptation de t_i
                mu += dTheta;
            } else {
                theta_.reject();
            }
        }

        // Gibbs de θ : N(μ, 1/P) tronquée sur [min, max]
        theta_.x = rng_.truncatedNormal(double(mu), 1.0 / std::sqrt(double(P)), min, max);

        // EDM2 : S0² (marche adaptative sur log10)
        updateS02(true);
    }

    void updateS02(bool record)
    {
        const double l2 = rng_.truncatedNormal(std::log10(s02_.x), s02_.sigmaMH, -20.0, 20.0);
        if (std::isnan(l2)) return;
        const double V2 = std::pow(10.0, l2);
        const double h0 = logHS02(s02_.x), h1 = logHS02(V2);
        if (std::isnan(h0) || std::isnan(h1)) return;
        const bool acc = rng_.acceptLog(h1 - h0);
        if (record) { if (acc) s02_.accept(V2); else s02_.reject(); }
        else if (acc) s02_.x = V2;
    }

    // MCMCLoopChrono::tempering_339_ti_marg : θ marginalisé pour les t_i, puis θ tempéré (T), puis δ, σ, S0².
    void tempering(double T)
    {
        const double min = tmin_, max = tmax_;
        long double P, mu, S;
        weightedStats(P, mu, S);
        if (P <= 0.0L) return;
        const long double sAvg = 1.0L / std::sqrt(P);

        for (DateState& d : dates_) {
            const double ti0 = d.ti.x, delta0 = d.delta;
            const long double sg = d.sigma.x, wi = 1.0L / (sg * sg), y0 = (long double)ti0 + delta0;
            const long double Prm = P - wi;
            long double murm = 0.0L, Srm = 0.0L;
            if (Prm > 0.0L) {
                murm = (P * mu - wi * y0) / Prm;
                Srm = S - wi * (y0 - mu) * (y0 - murm);
                if (Srm < 0.0L && Srm > -1e-18L) Srm = 0.0L;
            } else {
                murm = y0;
            }
            const double tiP = proposeTiIndependent(d);
            const double qf = proposalDensity(d, tiP, ti0), qr = proposalDensity(d, ti0, tiP);
            if (!(qf > 0.0) || !(qr > 0.0)) continue;
            const double L0 = d.cal->at(ti0), L1 = d.cal->at(tiP);
            if (!(L1 > 0.0) || !(L0 > 0.0)) continue;
            const long double yn = (long double)tiP + delta0;
            long double mun, Sn;
            if (Prm > 0.0L) {
                const long double dl = yn - murm;
                mun = murm + (wi / P) * dl;
                Sn = Srm + wi * dl * (yn - mun);
            } else {
                mun = yn; Sn = 0.0L;
            }
            if (Sn < 0.0L && Sn > -1e-18L) Sn = 0.0L;
            const double lz0 = logDiffCdf(double((min - mu) / sAvg), double((max - mu) / sAvg));
            const double lz1 = logDiffCdf(double((min - mun) / sAvg), double((max - mun) / sAvg));
            if (!std::isfinite(lz0) || !std::isfinite(lz1)) continue;
            const long double total = (long double)(lz1 - lz0) - 0.5L * (Sn - S)
                                      + (std::log(L1) - std::log(L0)) + (std::log(qr) - std::log(qf));
            if (rng_.acceptLog(double(total))) { d.ti.x = tiP; mu = mun; S = Sn; }
        }

        // θ : proposition normale tronquée élargie (σ_MH · T), cible tempérée N(μ, 1/P)^(1/T)
        const double avg = double(mu), sigma = double(sAvg);
        const double sp = theta_.sigmaMH * T, th0 = theta_.x;
        const double thP = rng_.truncatedNormal(th0, sp, min, max);
        const double num = normalCDF((max - th0) / sp) - normalCDF((min - th0) / sp);
        const double den = normalCDF((max - thP) / sp) - normalCDF((min - thP) / sp);
        if (num > 0.0 && den > 0.0) {
            const double lr = (logDnorm(thP, avg, sigma) - logDnorm(th0, avg, sigma)) / T + std::log(num) - std::log(den);
            if (rng_.acceptLog(lr)) theta_.x = thP;
        }

        for (DateState& d : dates_) {
            // Date::updateDelta_v3 (Gibbs de δ)
            const double lambda = theta_.x - d.ti.x;
            switch (d.w.type) {
            case Wiggle::None:  d.delta = 0.0; break;
            case Wiggle::Fixed: d.delta = d.w.fixed; break;
            case Wiggle::Range: d.delta = rng_.truncatedNormal(lambda, d.sigma.x, d.w.min, d.w.max); break;
            case Wiggle::Gaussian: {
                const double s2 = d.sigma.x * d.sigma.x, e2 = d.w.error * d.w.error;
                const double wsum = 1.0 / s2 + 1.0 / e2;
                const double avgD = (lambda / s2 + d.w.mean / e2) / wsum;
                d.delta = avgD + rng_.normal(0.0, 1.0) / std::sqrt(wsum);
                break;
            }
            }
            // Date::applySigmaShrinkage_K_tempering (T = 1)
            const double m = std::pow(d.ti.x - (theta_.x - d.delta), 2.0) * 0.5;
            const double V1 = d.sigma.x * d.sigma.x;
            const double V2 = rng_.shrinkageUniforme(s02_.x);
            if (1e-6 < V2 && V2 < 1e10) {
                const double lr = -m * (V1 - V2) / (V1 * V2) + 0.5 * (std::log(V1) - std::log(V2));
                if (rng_.acceptLog(lr)) d.sigma.x = std::sqrt(V2);
            }
        }
        updateS02(false);                                     // Event::applyS02Theta_v3
    }

    // MCMCLoopChrono::adapt
    bool adapt(int batch)
    {
        bool done = true;
        const bool many = dates_.size() > 1;
        const double lo = many ? 0.30 : 0.42, hi = many ? 0.40 : 0.46;
        for (DateState& d : dates_) {
            if (d.ti.rw) done &= d.ti.adapt(lo, hi, batch);
            if (d.sigma.rw) done &= d.sigma.adapt(lo, hi, batch);
        }
        if (theta_.rw) done = theta_.adapt(0.42, 0.46, batch) && done;
        if (s02_.rw) done = s02_.adapt(0.42, 0.46, batch) && done;
        return done;
    }

    void acquire()
    {
        out.theta.push_back(theta_.x);
        out.s0.push_back(std::sqrt(s02_.x));
        for (size_t k = 0; k < dates_.size(); ++k) {
            out.ti[k].push_back(dates_[k].ti.x);
            out.sigma[k].push_back(dates_[k].sigma.x);
            out.yi[k].push_back(dates_[k].ti.x + dates_[k].delta);      // Date::updateWiggle
        }
    }

    const std::vector<Calib>& cals_;
    const std::vector<Calib>& wcals_;
    const std::vector<Wiggle>& wig_;
    McmcSettings s_;
    double tmin_, tmax_;
    Rng rng_;
    std::atomic<long>& progress_;
    std::atomic<bool>& stop_;
    long pending_ = 0;

    std::vector<DateState> dates_;
    MHVar theta_, s02_;
    double betaS02_ = 1.0;
};

// ------------------------------------------------------------------ diagnostics (MCMCDiagnostic)

std::vector<double> averageRanks(const std::vector<double>& v)
{
    const int S = int(v.size());
    std::vector<int> idx(S);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int i, int j) { return v[i] < v[j]; });
    std::vector<double> ranks(S);
    int i = 0;
    while (i < S) {
        int j = i;
        while (j + 1 < S && v[idx[j + 1]] == v[idx[i]]) ++j;
        const double avg = 0.5 * ((i + 1) + (j + 1));
        for (int k = i; k <= j; ++k) ranks[idx[k]] = avg;
        i = j + 1;
    }
    return ranks;
}

std::vector<double> rankNormalize(const std::vector<double>& pooled)
{
    const int S = int(pooled.size());
    const std::vector<double> r = averageRanks(pooled);
    std::vector<double> z(S);
    for (int i = 0; i < S; ++i) z[i] = invNormalCDF((r[i] - 3.0 / 8.0) / (double(S) + 1.0 / 4.0));
    return z;
}

struct HalfStats { double W = 0.0, B = 0.0; };

HalfStats halfStats(const std::vector<std::vector<double>>& h)
{
    const int M = int(h.size()), N = int(h.front().size());
    std::vector<double> mean(M), var(M);
    for (int m = 0; m < M; ++m) {
        const double mu = std::accumulate(h[m].begin(), h[m].end(), 0.0) / N;
        double ss = 0.0;
        for (double x : h[m]) ss += (x - mu) * (x - mu);
        mean[m] = mu; var[m] = ss / (N - 1);
    }
    HalfStats s;
    s.W = std::accumulate(var.begin(), var.end(), 0.0) / M;
    const double g = std::accumulate(mean.begin(), mean.end(), 0.0) / M;
    double B = 0.0;
    for (double m : mean) B += (m - g) * (m - g);
    s.B = B * double(N) / (M - 1);
    return s;
}

double rhatFrom(const HalfStats& s, int M, int N)
{
    if (M < 2 || N < 2) return 0.0;
    if (s.W < 1e-15 && s.B < 1e-15) return 1.0;
    if (s.W < 1e-15) return kInf;
    if (s.B < 1e-15 * s.W) return 1.0;
    const double vp = ((N - 1.0) / N) * s.W + s.B / N;
    return std::sqrt(vp / s.W);
}

// Autocovariance biaisée (normalisée par N), calcul direct jusqu'au premier retard utile.
std::vector<double> autocov(const std::vector<double>& x, int maxLag)
{
    const int N = int(x.size());
    const double mu = std::accumulate(x.begin(), x.end(), 0.0) / N;
    std::vector<double> c(size_t(std::min(maxLag, N)), 0.0);
    for (int t = 0; t < int(c.size()); ++t) {
        double s = 0.0;
        for (int i = 0; i + t < N; ++i) s += (x[i] - mu) * (x[i + t] - mu);
        c[size_t(t)] = s / N;
    }
    return c;
}

double essFrom(const std::vector<std::vector<double>>& h, const HalfStats& st)
{
    const int M = int(h.size()), N = int(h.front().size());
    const double vp = ((N - 1.0) / N) * st.W + st.B / N;
    if (M < 2 || N < 2 || !(vp > 0.0) || !std::isfinite(vp)) return kNaN;
    // Les autocorrélations sont calculées par blocs de retards croissants (évite le O(N²) complet).
    int lagCap = std::min(N, 256);
    for (;;) {
        std::vector<std::vector<double>> ac(M);
        for (int m = 0; m < M; ++m) ac[m] = autocov(h[m], lagCap);
        std::vector<double> rho(size_t(lagCap), 0.0);
        rho[0] = 1.0;
        for (int t = 1; t < lagCap; ++t) {
            double mc = 0.0;
            for (int m = 0; m < M; ++m) mc += ac[m][size_t(t)];
            mc /= M;
            rho[size_t(t)] = 1.0 - (st.W - mc) / vp;
        }
        double tau = -1.0, prev = kInf, nextEven = 0.0;
        int t = 0;
        bool stopped = false;
        while (t + 1 < lagCap) {
            const double ps = rho[size_t(t)] + rho[size_t(t + 1)];
            if (ps < 0.0) { nextEven = rho[size_t(t)]; stopped = true; break; }
            const double cap = std::min(ps, prev);
            tau += 2.0 * cap;
            prev = cap;
            t += 2;
        }
        if (!stopped && lagCap < N) { lagCap = std::min(N, lagCap * 4); continue; }
        if (nextEven > 0.0) tau += nextEven;
        const double nTot = double(M) * double(N);
        tau = std::max(tau, 1.0 / std::log10(nTot));
        return nTot / tau;
    }
}

Convergence worse(Convergence a, Convergence b) { return int(a) > int(b) ? a : b; }

} // namespace

namespace mcmcdiag {

RhatEss computeRhatAndEss(const std::vector<std::vector<double>>& chains)
{
    RhatEss r;
    if (chains.empty()) return r;
    const int N = int(chains.front().size());
    for (const auto& c : chains) if (int(c.size()) != N) return r;
    if (N < 4) return r;
    const int Nh = N / 2;
    std::vector<std::vector<double>> halves;
    for (const auto& c : chains) {
        halves.emplace_back(c.begin(), c.begin() + Nh);
        halves.emplace_back(c.end() - Nh, c.end());
    }
    const int M = int(halves.size()), S = M * Nh;
    std::vector<double> pooled;
    pooled.reserve(size_t(S));
    for (const auto& h : halves) pooled.insert(pooled.end(), h.begin(), h.end());
    std::vector<double> sorted = pooled;
    std::sort(sorted.begin(), sorted.end());
    auto q = [&](double p) {
        const double idx = p * double(S - 1);
        const size_t lo = size_t(std::floor(idx)), hi = size_t(std::ceil(idx));
        const double f = idx - double(lo);
        return sorted[lo] * (1.0 - f) + sorted[hi] * f;
    };
    const double med = q(0.5), q05 = q(0.05), q95 = q(0.95);
    auto split = [&](const std::vector<double>& z) {
        std::vector<std::vector<double>> o(static_cast<size_t>(M), std::vector<double>(static_cast<size_t>(Nh)));
        for (int i = 0, off = 0; i < M; ++i) for (int j = 0; j < Nh; ++j) o[size_t(i)][size_t(j)] = z[size_t(off++)];
        return o;
    };
    const auto bulk = split(rankNormalize(pooled));
    const HalfStats bs = halfStats(bulk);
    const double rBulk = rhatFrom(bs, M, Nh);
    r.bulkEss = essFrom(bulk, bs);
    std::vector<double> folded(static_cast<size_t>(S));
    for (int i = 0; i < S; ++i) folded[size_t(i)] = std::abs(pooled[size_t(i)] - med);
    const auto tail = split(rankNormalize(folded));
    const double rTail = rhatFrom(halfStats(tail), M, Nh);
    std::vector<std::vector<double>> i05(static_cast<size_t>(M), std::vector<double>(static_cast<size_t>(Nh)));
    std::vector<std::vector<double>> i95 = i05;
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < Nh; ++j) {
            i05[size_t(i)][size_t(j)] = halves[size_t(i)][size_t(j)] <= q05 ? 1.0 : 0.0;
            i95[size_t(i)][size_t(j)] = halves[size_t(i)][size_t(j)] <= q95 ? 1.0 : 0.0;
        }
    r.tailEss = std::min(essFrom(i05, halfStats(i05)), essFrom(i95, halfStats(i95)));
    r.rhat = std::max(rBulk, rTail);
    return r;
}

} // namespace mcmcdiag

Density kernelDensity(const std::vector<double>& samples, double lo, double hi)
{
    Density d;
    const size_t n = samples.size();
    if (n < 2) return d;
    std::vector<double> s = samples;
    std::sort(s.begin(), s.end());
    const double mean = std::accumulate(s.begin(), s.end(), 0.0) / double(n);
    double ss = 0.0;
    for (double x : s) ss += (x - mean) * (x - mean);
    const double sd = std::sqrt(ss / double(n - 1));
    auto quant = [&](double p) { const double i = p * double(n - 1); const size_t a = size_t(i); const size_t b = std::min(n - 1, a + 1); return s[a] + (i - double(a)) * (s[b] - s[a]); };
    const double iqr = quant(0.75) - quant(0.25);
    double h = 0.9 * std::min(sd, iqr / 1.34) * std::pow(double(n), -0.2);         // bw.nrd0 de R
    if (!(h > 0.0)) h = sd > 0.0 ? 0.9 * sd * std::pow(double(n), -0.2) : 1.0;
    const double a = std::max(lo, s.front() - 4.0 * h), b = std::min(hi, s.back() + 4.0 * h);
    if (!(b > a)) return d;
    const int K = 1024;
    const double step = (b - a) / (K - 1);
    // Histogramme fin puis convolution par le noyau gaussien
    std::vector<double> bins(size_t(K), 0.0);
    for (double x : s) {
        const double f = (x - a) / step;
        if (f < 0.0 || f > K - 1) continue;
        const size_t i = size_t(f);
        const double r = f - double(i);
        bins[i] += 1.0 - r;
        if (i + 1 < size_t(K)) bins[i + 1] += r;
    }
    const int half = int(std::ceil(4.0 * h / step));
    std::vector<double> ker(size_t(2 * half + 1));
    for (int k = -half; k <= half; ++k) ker[size_t(k + half)] = dnorm(k * step, 0.0, h);
    d.t.resize(size_t(K));
    d.v.assign(size_t(K), 0.0);
    for (int i = 0; i < K; ++i) {
        d.t[size_t(i)] = a + i * step;
        if (bins[size_t(i)] == 0.0) continue;
        for (int k = -half; k <= half; ++k) {
            const int j = i + k;
            if (j >= 0 && j < K) d.v[size_t(j)] += bins[size_t(i)] * ker[size_t(k + half)];
        }
    }
    d.step = step;
    double area = 0.0;
    for (size_t i = 1; i < d.v.size(); ++i) area += 0.5 * (d.v[i - 1] + d.v[i]) * step;
    if (!(area > 0.0)) return Density();
    for (double& x : d.v) x /= area;
    return d;
}

EventMcmcResult runEventMcmc(const std::vector<EventDateInput>& dates, const McmcSettings& settingsIn,
                             double tmin, double tmax, const McmcProgress& progress)
{
    EventMcmcResult R;
    R.tmin = tmin; R.tmax = tmax;
    const auto t0 = std::chrono::steady_clock::now();
    if (dates.empty()) { R.error = "aucune datation"; return R; }
    if (!(tmax > tmin)) { R.error = "période d'étude invalide (tmin ≥ tmax)"; return R; }

    McmcSettings s = settingsIn;
    s.numChains = std::clamp(s.numChains, 1, 32);
    s.numBurn = std::max(0, s.numBurn);
    s.maxBatches = std::max(0, s.maxBatches);
    s.iterPerBatch = std::max(1, s.iterPerBatch);
    s.numIter = std::max(10, s.numIter);
    s.thinning = std::clamp(s.thinning, 1, s.numIter);
    s.mixingLevel = std::clamp(s.mixingLevel, 0.0, 1.0);
    s.annealTemp = std::clamp(s.annealTemp, 0, 30);

    std::vector<Calib> cals, wcals;
    std::vector<Wiggle> wig;
    for (const EventDateInput& d : dates) {
        if (!d.calib.valid()) { R.error = "densité calibrée invalide pour « " + d.name + " »"; return R; }
        cals.push_back(Calib::from(d.calib));
        wcals.push_back(wiggleCalib(cals.back(), d.wiggle));
        wig.push_back(d.wiggle);
    }

    // Graines : celles fournies, complétées au hasard.
    std::vector<unsigned> seeds = s.seeds;
    std::random_device rd;
    while (int(seeds.size()) < s.numChains) seeds.push_back(rd() % 1000000u + 1u);
    seeds.resize(size_t(s.numChains));

    // Travail estimé : itérations + balayages de recuit.
    long regen = 0, sweeps = 0;
    if (s.annealRecurrence > 0) {
        regen = s.numIter / s.annealRecurrence;
        for (int e = s.annealTemp; e >= 0; --e) sweeps += std::max(1, int(std::ceil(s.annealDwell / (1.0 + e))));
    }
    const double totalWork = double(s.numChains) * (double(s.numBurn) + double(s.maxBatches) * s.iterPerBatch
                                                    + double(s.numIter) + double(regen) * double(sweeps));

    std::atomic<long> done{ 0 };
    std::atomic<bool> stop{ false };
    std::atomic<int> finished{ 0 };
    std::vector<std::unique_ptr<Chain>> chains;
    for (int c = 0; c < s.numChains; ++c)
        chains.emplace_back(new Chain(cals, wcals, wig, s, tmin, tmax, seeds[size_t(c)], done, stop));
    std::vector<std::thread> threads;
    for (auto& ch : chains) {
        Chain* p = ch.get();
        threads.emplace_back([p, &finished] { p->run(); ++finished; });
    }
    while (finished.load() < s.numChains) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        if (progress && !progress(std::min(0.999, double(done.load()) / std::max(1.0, totalWork)))) stop = true;
    }
    for (std::thread& t : threads) t.join();
    if (stop) { R.error = "calcul interrompu"; return R; }
    for (auto& ch : chains) if (!ch->out.error.empty()) { R.error = ch->out.error; return R; }
    if (progress) progress(1.0);

    // Résultats
    const size_t n = dates.size();
    std::vector<double> allTheta;
    std::vector<std::vector<double>> allTi(n), allSigma(n), allYi(n);
    for (auto& ch : chains) {
        allTheta.insert(allTheta.end(), ch->out.theta.begin(), ch->out.theta.end());
        for (size_t k = 0; k < n; ++k) {
            allTi[k].insert(allTi[k].end(), ch->out.ti[k].begin(), ch->out.ti[k].end());
            allSigma[k].insert(allSigma[k].end(), ch->out.sigma[k].begin(), ch->out.sigma[k].end());
            allYi[k].insert(allYi[k].end(), ch->out.yi[k].begin(), ch->out.yi[k].end());
        }
        R.thetaTraces.push_back(ch->out.theta);
        R.chains.push_back(ch->out.report);
    }
    R.theta = kernelDensity(allTheta, tmin, tmax);
    if (!R.theta.valid()) { R.error = "densité a posteriori de θ vide"; return R; }
    for (size_t k = 0; k < n; ++k) {
        R.ti.push_back(kernelDensity(allTi[k], -kInf, kInf));
        R.yi.push_back(dates[k].wiggle.type == Wiggle::None ? R.ti.back() : kernelDensity(allYi[k], -kInf, kInf));
        std::vector<double> sg = allSigma[k];
        std::nth_element(sg.begin(), sg.begin() + long(sg.size() / 2), sg.end());
        R.sigmaMedian.push_back(sg.empty() ? kNaN : sg[sg.size() / 2]);
    }

    // Diagnostics : θ, S0, t_i, σ_i (Model::generateNumericalResults)
    auto collect = [&](auto get) { std::vector<std::vector<double>> v; for (auto& ch : chains) v.push_back(get(ch->out)); return v; };
    auto addDiag = [&](const std::string& name, const std::vector<std::vector<double>>& cs, double acc) {
        VarDiagnostic v;
        v.name = name;
        const mcmcdiag::RhatEss re = mcmcdiag::computeRhatAndEss(cs);
        v.rhat = re.rhat; v.bulkEss = re.bulkEss; v.tailEss = re.tailEss; v.acceptRate = acc;
        R.diagnostics.push_back(v);
    };
    long ta = 0, tp = 0;
    for (auto& ch : chains) { ta += ch->out.thetaAcc; tp += ch->out.thetaProp; }
    addDiag("θ", collect([](const ChainOut& o) { return o.theta; }), tp > 0 ? double(ta) / tp : -1.0);
    addDiag("S0", collect([](const ChainOut& o) { return o.s0; }), -1.0);
    for (size_t k = 0; k < n; ++k) {
        long a = 0, p = 0;
        for (auto& ch : chains) { a += ch->out.tiAcc[k]; p += ch->out.tiProp[k]; }
        addDiag("t(" + dates[k].name + ")", collect([k](const ChainOut& o) { return o.ti[k]; }), p > 0 ? double(a) / p : -1.0);
        addDiag("σ(" + dates[k].name + ")", collect([k](const ChainOut& o) { return o.sigma[k]; }), -1.0);
    }

    // Synthèse (MCMCDiagnostic::computeConvergenceSummary : tolérance 1 %, au moins 4 variables pour « mauvais »)
    const size_t M = size_t(s.numChains);
    if (M >= 2) {
        size_t nonFinite = 0, above = 0, essNonFinite = 0, essBelow = 0;
        double maxR = 0.0, minE = kInf;
        bool anyBadR = false, anyBadE = false;
        const double essGood = mcmcdiag::kEssGoodPerChain * M, essWarn = mcmcdiag::kEssWarningPerChain * M;
        for (const VarDiagnostic& v : R.diagnostics) {
            if (!std::isfinite(v.rhat)) { ++nonFinite; anyBadR = true; }
            else {
                maxR = std::max(maxR, v.rhat);
                if (v.rhat >= mcmcdiag::kRhatGood) ++above;
                if (v.rhat >= mcmcdiag::kRhatWarning) anyBadR = true;
            }
            const double e = std::min(v.bulkEss, v.tailEss);
            if (!std::isfinite(e)) { ++essNonFinite; anyBadE = true; }
            else {
                minE = std::min(minE, e);
                if (e < essGood) ++essBelow;
                if (e < essWarn) anyBadE = true;
            }
        }
        const double nv = double(R.diagnostics.size());
        auto status = [&](bool bad, bool notGood, size_t count) {
            if (bad) return Convergence::Bad;
            if (notGood) return (count < 4 || double(count) / nv <= 0.01) ? Convergence::Warning : Convergence::Bad;
            return Convergence::Good;
        };
        R.rhatStatus = status(anyBadR, maxR >= mcmcdiag::kRhatGood, above + nonFinite);
        R.essStatus = status(anyBadE, minE < essGood, essBelow + essNonFinite);
        R.status = worse(R.rhatStatus, R.essStatus);
        R.maxRhat = maxR;
        R.minEss = std::isfinite(minE) ? minE : kNaN;
    }
    R.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    R.ok = true;
    return R;
}

} // namespace seldon
