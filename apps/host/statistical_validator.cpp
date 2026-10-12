// apps/host/statistical_validator.cpp
// Rigorous statistical testing harness implementation.

#include "statistical_validator.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <numeric>

namespace dgpu::host {

// Standard normal cumulative distribution function
static double normalCdf(double z) noexcept {
    return 0.5 * std::erfc(-z * 0.70710678118654752440);
}

double StatisticalValidator::studentTCriticalValue95(size_t degreesOfFreedom) noexcept {
    if (degreesOfFreedom == 0) return 1.96;
    // Exact lookup for small degrees of freedom
    static constexpr double kSmallDfTable[] = {
        0.0,    // 0
        12.706, // 1
        4.303,  // 2
        3.182,  // 3
        2.776,  // 4
        2.571,  // 5
        2.447,  // 6
        2.365,  // 7
        2.306,  // 8
        2.262,  // 9
        2.228,  // 10
    };
    if (degreesOfFreedom < sizeof(kSmallDfTable) / sizeof(kSmallDfTable[0])) {
        return kSmallDfTable[degreesOfFreedom];
    }
    // Cornish-Fisher high-precision expansion for df >= 10
    double df = static_cast<double>(degreesOfFreedom);
    return 1.959964 + (2.37227 / df) + (2.8225 / (df * df)) + (2.55585 / (df * df * df));
}

double StatisticalValidator::studentTPValue(double tStat, size_t degreesOfFreedom) noexcept {
    if (degreesOfFreedom == 0) return 1.0;
    double t = std::abs(tStat);
    if (t < 1e-9) return 1.0;

    double df = static_cast<double>(degreesOfFreedom);
    // Hill's transformation to standard normal variate
    double w = std::log1p((t * t) / df);
    double z = std::sqrt(df * w);
    // Correction term for finite df
    z += (z * z * z + 3.0 * z) / (4.0 * df);
    double p = 2.0 * (1.0 - normalCdf(z));
    return std::clamp(p, 0.0, 1.0);
}

static double computeMedian(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n % 2 == 1) return v[n / 2];
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static double computeP95(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(std::ceil(0.95 * v.size())) - 1;
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

PairedStats StatisticalValidator::computePairedStats(
    const std::vector<double>& baselineTimings,
    const std::vector<double>& targetTimings,
    double mae) {

    PairedStats s;
    size_t n = std::min(baselineTimings.size(), targetTimings.size());
    s.sampleCount = n;
    s.mae = mae;
    if (n < 2) return s;

    // Baseline metrics
    double sumBase = std::accumulate(baselineTimings.begin(), baselineTimings.begin() + n, 0.0);
    s.meanBaselineMs = sumBase / n;
    s.medianBaselineMs = computeMedian(std::vector<double>(baselineTimings.begin(), baselineTimings.begin() + n));
    s.p95BaselineMs = computeP95(std::vector<double>(baselineTimings.begin(), baselineTimings.begin() + n));

    double varBase = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double diff = baselineTimings[i] - s.meanBaselineMs;
        varBase += diff * diff;
    }
    s.stdDevBaselineMs = std::sqrt(varBase / (n - 1));

    // Target metrics
    double sumTarget = std::accumulate(targetTimings.begin(), targetTimings.begin() + n, 0.0);
    s.meanTargetMs = sumTarget / n;
    s.medianTargetMs = computeMedian(std::vector<double>(targetTimings.begin(), targetTimings.begin() + n));
    s.p95TargetMs = computeP95(std::vector<double>(targetTimings.begin(), targetTimings.begin() + n));

    double varTarget = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double diff = targetTimings[i] - s.meanTargetMs;
        varTarget += diff * diff;
    }
    s.stdDevTargetMs = std::sqrt(varTarget / (n - 1));

    // Paired differences: Delta_i = baseline[i] - target[i]
    std::vector<double> deltas(n);
    double sumDelta = 0.0;
    for (size_t i = 0; i < n; ++i) {
        deltas[i] = baselineTimings[i] - targetTimings[i];
        sumDelta += deltas[i];
    }
    s.meanDeltaMs = sumDelta / n;

    double varDelta = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double diff = deltas[i] - s.meanDeltaMs;
        varDelta += diff * diff;
    }
    s.stdDevDeltaMs = std::sqrt(varDelta / (n - 1));
    s.standardErrorMs = s.stdDevDeltaMs / std::sqrt(static_cast<double>(n));

    // Confidence Interval & t-statistic
    size_t df = n - 1;
    double tCrit = studentTCriticalValue95(df);
    s.ci95LowerMs = s.meanDeltaMs - tCrit * s.standardErrorMs;
    s.ci95UpperMs = s.meanDeltaMs + tCrit * s.standardErrorMs;

    s.tStatistic = (s.standardErrorMs > 1e-9) ? (s.meanDeltaMs / s.standardErrorMs) : 0.0;
    s.pValue = studentTPValue(s.tStatistic, df);
    s.cohensD = (s.stdDevDeltaMs > 1e-9) ? (s.meanDeltaMs / s.stdDevDeltaMs) : 0.0;
    s.speedupRatio = (s.meanTargetMs > 1e-9) ? (s.meanBaselineMs / s.meanTargetMs) : 1.0;

    // Scientific statistical verdict
    if (s.ci95LowerMs > 0.0 && s.pValue < 0.05) {
        s.isStatisticallySignificant = true;
        s.verdict = std::format("GENUINE SPEEDUP CONFIRMED ({:.2f}x, 95% CI: [{:+.2f}, {:+.2f}] ms, p = {:.4f})",
                                s.speedupRatio, s.ci95LowerMs, s.ci95UpperMs, s.pValue);
    } else if (s.ci95UpperMs < 0.0 && s.pValue < 0.05) {
        s.isStatisticallySignificant = false;
        s.verdict = std::format("STATISTICALLY SLOWER ({:.2f}x, 95% CI: [{:+.2f}, {:+.2f}] ms, p = {:.4f})",
                                s.speedupRatio, s.ci95LowerMs, s.ci95UpperMs, s.pValue);
    } else {
        s.isStatisticallySignificant = false;
        s.verdict = std::format("INCONCLUSIVE / WITHIN MEASUREMENT NOISE (CI: [{:+.2f}, {:+.2f}] ms spans zero, p = {:.4f})",
                                s.ci95LowerMs, s.ci95UpperMs, s.pValue);
    }

    return s;
}

void StatisticalValidator::printPairedReport(
    const std::string& title,
    const std::string& baselineName,
    const std::string& targetName,
    const PairedStats& stats) {

    std::cout << "\n  ┌────────────────────────────────────────────────────────────────────────┐\n";
    std::cout << std::format("  │ Statistical Hypothesis Test: {:<41} │\n", title);
    std::cout << "  ├────────────────────────────────────────────────────────────────────────┤\n";
    std::cout << std::format("  │ Paired Repetitions (N):        {:>10} runs (alternating interleaved) │\n", stats.sampleCount);
    std::cout << std::format("  │ Baseline ({}):  {:>10.2f} ms mean (median: {:.2f} ms)     │\n",
                             baselineName.substr(0, 18), stats.meanBaselineMs, stats.medianBaselineMs);
    std::cout << std::format("  │ Target   ({}):  {:>10.2f} ms mean (median: {:.2f} ms)     │\n",
                             targetName.substr(0, 18), stats.meanTargetMs, stats.medianTargetMs);
    std::cout << "  ├────────────────────────────────────────────────────────────────────────┤\n";
    std::cout << std::format("  │ Paired Timing Delta (Delta = T_base - T_target): {:>+8.2f} ms              │\n", stats.meanDeltaMs);
    std::cout << std::format("  │ Delta Std Deviation (s_delta):                   {:>8.2f} ms              │\n", stats.stdDevDeltaMs);
    std::cout << std::format("  │ Standard Error (SE = s / sqrt(N)):               {:>8.2f} ms              │\n", stats.standardErrorMs);
    std::cout << std::format("  │ 95% Confidence Interval:              [{:>+7.2f}, {:>+7.2f}] ms              │\n",
                             stats.ci95LowerMs, stats.ci95UpperMs);
    std::cout << std::format("  │ Student's t Statistic:                           {:>8.2f}                 │\n", stats.tStatistic);
    std::cout << std::format("  │ Two-Tailed p-value:                              {:>8.4f}                 │\n", stats.pValue);
    std::cout << std::format("  │ Effect Size (Cohen's d):                         {:>8.2f}                 │\n", stats.cohensD);
    std::cout << std::format("  │ Visual Equivalence (MAE):                        {:>8.2f}                 │\n", stats.mae);
    std::cout << "  ├────────────────────────────────────────────────────────────────────────┤\n";
    std::cout << std::format("  │ Verdict: {:<61} │\n", stats.verdict.substr(0, 61));
    std::cout << "  └────────────────────────────────────────────────────────────────────────┘\n";
}

} // namespace dgpu::host
