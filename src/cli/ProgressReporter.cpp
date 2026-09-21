#include "cli/ProgressReporter.hpp"

#include "util/Logger.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace gfnif {
namespace {

constexpr int kBarWidth = 24;
/*! Keep the whole line inside a default 80-column console so the repaint never
 *  wraps -- a wrapped line cannot be erased with a single carriage return and
 *  would smear progress across the screen. */
constexpr std::size_t kMaxLineWidth = 78;

std::string Ellipsize(const std::string& s, std::size_t max) {
    if (s.size() <= max) {
        return s;
    }
    if (max <= 3) {
        return s.substr(0, max);
    }
    // Keep the tail: the filename identifies the model, the leading directories
    // repeat across thousands of lines.
    return "..." + s.substr(s.size() - (max - 3));
}

} // namespace

ProgressReporter::ProgressReporter(ReportMode mode, std::size_t total, Logger& logger)
    : mode_(mode), total_(total), logger_(logger) {}

void ProgressReporter::GuardBefore(void* self) {
    static_cast<ProgressReporter*>(self)->ClearLine();
}

void ProgressReporter::GuardAfter(void* self) {
    static_cast<ProgressReporter*>(self)->Redraw();
}

void ProgressReporter::BeginFile(const std::string& relativePath, std::size_t index) {
    if (mode_ != ReportMode::Verbose) {
        return;
    }
    std::ostringstream os;
    os << "\n[" << (index + 1) << "/" << total_ << "] " << relativePath;
    logger_.Info(os.str());
}

void ProgressReporter::EndFile(const std::string& relativePath, const std::string& summary,
                              bool failed) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++completed_;
        if (failed) {
            ++failed_;
        }
        lastPath_ = relativePath;
    }

    if (mode_ == ReportMode::Compact) {
        std::lock_guard<std::mutex> lock(mutex_);
        DrawLocked();
        return;
    }

    // Verbose and Normal both print a real line per file; the Logger handles
    // the locking and the log file.
    if (!summary.empty()) {
        logger_.Info(summary);
    }
}

void ProgressReporter::DrawLocked() {
    if (finished_) {
        return;
    }
    std::ostringstream os;

    const double frac = total_ == 0 ? 1.0 : static_cast<double>(completed_) /
                                                static_cast<double>(total_);
    const int filled = static_cast<int>(frac * kBarWidth);
    os << "[";
    for (int i = 0; i < kBarWidth; ++i) {
        os << (i < filled ? '=' : ' ');
    }
    os << "] " << completed_ << "/" << total_ << " ";
    if (failed_ > 0) {
        os << "(" << failed_ << " err) ";
    }

    std::string line = os.str();
    // Whatever budget is left goes to the filename.
    const std::size_t room = line.size() < kMaxLineWidth ? kMaxLineWidth - line.size() : 0;
    line += Ellipsize(lastPath_, room);

    // Pad to the previous width so leftovers from a longer path are wiped.
    std::string padded = line;
    if (padded.size() < lastDrawnWidth_) {
        padded.append(lastDrawnWidth_ - padded.size(), ' ');
    }
    std::cout << '\r' << padded << std::flush;

    lastDrawnWidth_ = line.size();
    lineDrawn_ = true;
}

void ProgressReporter::EraseLocked() {
    if (!lineDrawn_) {
        return;
    }
    std::cout << '\r' << std::string(lastDrawnWidth_, ' ') << '\r' << std::flush;
    lineDrawn_ = false;
}

void ProgressReporter::ClearLine() {
    if (mode_ != ReportMode::Compact) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    EraseLocked();
}

void ProgressReporter::Redraw() {
    if (mode_ != ReportMode::Compact) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    DrawLocked();
}

void ProgressReporter::Finish() {
    if (mode_ != ReportMode::Compact) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (lineDrawn_) {
        // Keep the completed bar on screen and move past it.
        std::cout << "\n" << std::flush;
        lineDrawn_ = false;
    }
    finished_ = true;
}

} // namespace gfnif
