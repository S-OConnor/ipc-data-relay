// ipc-relay-capture-to-csv: decodes a receiver capture file into one CSV per
// ipc-relay-testpub message type (board_health.csv, mode_status.csv,
// ptp_stats.csv, and unknown.csv for anything else). See csv_export.hpp.
//
// Exit status is 0 on success, 1 if the capture cannot be read, is malformed
// or truncated (the CSVs then hold every record before the error), and 2 on
// a usage error.
#include <cstdio>
#include <filesystem>
#include <string>

#include "csv_export.hpp"

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [options] CAPTURE\n"
        "\n"
        "Decodes a capture file into one CSV per message type.\n"
        "\n"
        "Options:\n"
        "  -o, --output-dir DIR   Directory for the CSV files       [<CAPTURE without extension>_csv]\n"
        "  -h, --help             Show this help\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    std::string capture;
    std::string out_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "-o" || a == "--output-dir") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s: %s needs a value\n", argv[0], a.c_str());
                return 2;
            }
            out_dir = argv[++i];
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "%s: unknown option %s\n", argv[0], a.c_str());
            usage(argv[0]);
            return 2;
        } else if (capture.empty()) {
            capture = a;
        } else {
            std::fprintf(stderr, "%s: only one capture file may be given\n", argv[0]);
            return 2;
        }
    }
    if (capture.empty()) {
        usage(argv[0]);
        return 2;
    }
    std::filesystem::path out = out_dir;
    if (out.empty()) out = std::filesystem::path(capture).replace_extension().string() + "_csv";

    const ipcrelay::csvexport::ExportResult r = ipcrelay::csvexport::export_capture(capture, out);
    for (const auto& [stem, rows] : r.rows) {
        std::printf("%-40s %llu row(s)\n", (out / (stem + ".csv")).c_str(), static_cast<unsigned long long>(rows));
    }
    if (!r.error.empty()) {
        std::fprintf(stderr, "ERROR: %s: %s\n", capture.c_str(), r.error.c_str());
        return 1;
    }
    if (r.rows.empty()) std::printf("no records in %s\n", capture.c_str());
    return 0;
}
