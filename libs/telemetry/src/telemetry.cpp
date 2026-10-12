// telemetry/telemetry.cpp
// Frame-recording, statistics, and CSV export.

#include "telemetry/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace dgpu::telemetry {

struct Telemetry::Impl {
    std::vector<FrameRecord> records;
    mutable std::mutex       mu;
};

Telemetry::Telemetry()  : m_impl(std::make_unique<Impl>()) {}
Telemetry::~Telemetry() = default;

Telemetry& Telemetry::instance() {
    static Telemetry inst;
    return inst;
}

void Telemetry::record(FrameRecord rec) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->records.push_back(std::move(rec));
}

BenchmarkReport Telemetry::summarise(const std::string& labelPrefix) const {
    std::vector<uint64_t> samples;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        for (const auto& r : m_impl->records) {
            if (r.label.starts_with(labelPrefix)) {
                samples.push_back(r.totalUs);
            }
        }
    }

    BenchmarkReport rep;
    rep.label       = labelPrefix;
    rep.sampleCount = samples.size();
    if (samples.empty()) return rep;

    std::sort(samples.begin(), samples.end());

    rep.minUs = samples.front();
    rep.maxUs = samples.back();

    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    rep.meanUs = sum / static_cast<double>(samples.size());

    auto percentile = [&](double p) -> double {
        size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(samples.size()))) - 1;
        idx = std::min(idx, samples.size() - 1);
        return static_cast<double>(samples[idx]);
    };

    rep.p50Us = percentile(0.50);
    rep.p95Us = percentile(0.95);
    rep.p99Us = percentile(0.99);

    double variance = 0.0;
    for (auto s : samples) {
        double d = static_cast<double>(s) - rep.meanUs;
        variance += d * d;
    }
    rep.stddevUs = std::sqrt(variance / static_cast<double>(samples.size()));
    rep.avgFps   = (rep.meanUs > 0.0) ? 1'000'000.0 / rep.meanUs : 0.0;

    return rep;
}

bool Telemetry::exportCsv(const std::filesystem::path& path) const {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "frameIndex,label,isRemote,width,height,"
         "cpuSubmitUs,gpuExecUs,readbackUs,uploadUs,"
         "networkSendUs,networkReturnUs,totalUs\n";

    std::lock_guard<std::mutex> lk(m_impl->mu);
    for (const auto& r : m_impl->records) {
        f << r.frameIndex       << ','
          << r.label            << ','
          << (r.isRemote ? 1 : 0) << ','
          << r.width            << ','
          << r.height           << ','
          << r.cpuSubmitUs      << ','
          << r.gpuExecUs        << ','
          << r.readbackUs       << ','
          << r.uploadUs         << ','
          << r.networkSendUs    << ','
          << r.networkReturnUs  << ','
          << r.totalUs          << '\n';
    }
    return true;
}

void Telemetry::clear() {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->records.clear();
}

std::size_t Telemetry::count() const noexcept {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->records.size();
}

} // namespace dgpu::telemetry
