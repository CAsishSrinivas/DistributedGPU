// tests/benchmark_tests/test_telemetry.cpp
// Unit tests for the Telemetry singleton — recording, statistics, and CSV export.

#include "telemetry/telemetry.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace dgpu::telemetry;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(expr)                                                          \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::cerr << "FAIL [" __FILE__ ":" << __LINE__ << "] " #expr << '\n'; \
            ++g_failed;                                                      \
        } else {                                                             \
            ++g_passed;                                                      \
        }                                                                    \
    } while (0)

static void test_record_and_count() {
    Telemetry::instance().clear();
    CHECK(Telemetry::instance().count() == 0);

    FrameRecord fr;
    fr.label   = "test";
    fr.totalUs = 1000;
    Telemetry::instance().record(fr);

    CHECK(Telemetry::instance().count() == 1);
    std::cout << "  record+count: PASS\n";
}

static void test_summarise_mean() {
    Telemetry::instance().clear();
    for (uint64_t i = 1; i <= 4; ++i) {
        FrameRecord fr;
        fr.label   = "bench";
        fr.totalUs = i * 100; // 100, 200, 300, 400
        Telemetry::instance().record(fr);
    }
    auto rep = Telemetry::instance().summarise("bench");
    CHECK(rep.sampleCount == 4);
    CHECK(std::abs(rep.meanUs - 250.0) < 1.0); // (100+200+300+400)/4 = 250
    CHECK(rep.minUs == 100);
    CHECK(rep.maxUs == 400);
    std::cout << "  summarise mean: PASS\n";
}

static void test_csv_export() {
    Telemetry::instance().clear();
    FrameRecord fr;
    fr.frameIndex = 0;
    fr.label      = "csv_test";
    fr.width      = 256;
    fr.height     = 256;
    fr.totalUs    = 5000;
    Telemetry::instance().record(fr);

    auto path = std::filesystem::temp_directory_path() / "dgpu_test.csv";
    CHECK(Telemetry::instance().exportCsv(path));

    std::ifstream f(path);
    CHECK(f.is_open());
    std::string header;
    std::getline(f, header);
    CHECK(header.find("frameIndex") != std::string::npos);

    std::string line;
    std::getline(f, line);
    CHECK(line.find("csv_test") != std::string::npos);
    std::cout << "  CSV export: PASS\n";

    f.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

int main() {
    std::cout << "=== Benchmark / Telemetry Tests ===\n";
    test_record_and_count();
    test_summarise_mean();
    test_csv_export();
    std::cout << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
