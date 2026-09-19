// gfnif-export -- Phase 1 diagnostic entry point.
//
// At this stage the executable does one thing: parse a .nif/.kf with niflib and
// dump its block list + block tree, so we can confirm niflib handles the Grand
// Fantasia corpus before building the real exporter on top of it.

#include "nif/NifDumper.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void PrintUsage() {
    std::cout << "gfnif-export (Phase 1 block dumper)\n\n"
              << "Usage:\n"
              << "  gfnif-export <file.nif|file.kf>    dump one file\n"
              << "  gfnif-export <directory>           dump every .nif/.kf found recursively\n"
              << "  gfnif-export <path> --summary      print only the corpus block type tally\n";
}

bool IsNifLike(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".nif" || ext == ".kf";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const fs::path target = argv[1];
    bool summary_only = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--summary") {
            summary_only = true;
        }
    }

    std::error_code ec;
    if (!fs::exists(target, ec)) {
        std::cerr << "error: path not found: " << target.string() << "\n";
        return 1;
    }

    std::vector<fs::path> files;
    if (fs::is_directory(target, ec)) {
        for (auto it = fs::recursive_directory_iterator(target, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) && IsNifLike(it->path())) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(target);
    }

    if (files.empty()) {
        std::cerr << "error: no .nif/.kf files under " << target.string() << "\n";
        return 1;
    }

    // In summary mode the per-file dump is discarded; only the tally is kept.
    std::ostringstream sink;
    gfnif::BlockTypeTally tally;
    int ok = 0;
    std::vector<std::string> failures;

    for (const fs::path& f : files) {
        std::ostream& out = summary_only ? static_cast<std::ostream&>(sink) : std::cout;
        if (gfnif::DumpNifFile(f.string(), out, &tally)) {
            ++ok;
        } else {
            failures.push_back(f.string());
        }
        if (summary_only) {
            sink.str(std::string());
        }
    }

    std::cout << "\n==================================================================\n";
    std::cout << "CORPUS SUMMARY: " << ok << "/" << files.size() << " file(s) parsed\n";
    std::cout << "==================================================================\n";
    std::cout << "Distinct block types: " << tally.size() << "\n";
    for (const auto& kv : tally) {
        std::cout << "  " << kv.second << "\tx " << kv.first << "\n";
    }
    if (!failures.empty()) {
        std::cout << "\nFAILED FILES (" << failures.size() << "):\n";
        for (const std::string& f : failures) {
            std::cout << "  " << f << "\n";
        }
    }

    return failures.empty() ? 0 : 2;
}
