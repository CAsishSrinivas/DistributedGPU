// apps/host/statistical_validator.hpp
// Rigorous statistical testing harness: interleaved round-robin execution,
// paired timing differences, 95% confidence intervals, and hypothesis testing.
#pragma once

#include "protocol/message.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dgpu::host {

struct PairedStats {
    size_t sampleCount{0};
    double meanBaselineMs{0.0};
    double medianBaselineMs{0.0};
    double p95BaselineMs{0.0};
    double stdDevBaselineMs{0.0};

    double meanTargetMs{0.0};
    double medianTargetMs{0.0};
    double p95TargetMs{0.0};
    double stdDevTargetMs{0.0};

    // Paired difference: Delta_i = T_baseline[i] - T_target[i]
    // Positive delta means target is faster than baseline.
    double meanDeltaMs{0.0};
    double stdDevDeltaMs{0.0};
    double standardErrorMs{0.0};
    double ci95LowerMs{0.0};
    double ci95UpperMs{0.0};

    double tStatistic{0.0};
    double pValue{1.0};
    double cohensD{0.0};

    bool isStatisticallySignificant{false}; // p < 0.05 and CI95 lower > 0
    double speedupRatio{1.0};               // meanBaseline / meanTarget
    double mae{0.0};                        // Mean Absolute Error vs baseline
    std::string verdict;
};

struct InterleavedModeRun {
    std::string name;
    std::function<protocol::TaskResult(uint32_t repIndex)> execute;
    std::vector<double> timingsMs;
    protocol::TaskResult lastResult;
};

class StatisticalValidator {
public:
    // Computes paired difference statistics between two vectors of equal length
    [[nodiscard]] static PairedStats computePairedStats(
        const std::vector<double>& baselineTimings,
        const std::vector<double>& targetTimings,
        double mae = 0.0);

    // Approximate two-tailed Student's t critical value for alpha = 0.05
    [[nodiscard]] static double studentTCriticalValue95(size_t degreesOfFreedom) noexcept;

    // Approximate two-tailed p-value for Student's t distribution
    [[nodiscard]] static double studentTPValue(double tStat, size_t degreesOfFreedom) noexcept;

    // Formats a rich statistical report table
    static void printPairedReport(const std::string& title,
                                  const std::string& baselineName,
                                  const std::string& targetName,
                                  const PairedStats& stats);
};

} // namespace dgpu::host
