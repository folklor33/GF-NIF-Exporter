#include "util/Logger.hpp"

#include <iostream>

namespace gfnif {

const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
        case LogLevel::Quiet:   return "QUIET";
    }
    return "?";
}

Logger::~Logger() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void Logger::SetConsoleLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    consoleLevel_ = level;
}

bool Logger::OpenFile(const std::string& path, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.open(path, std::ios::out | std::ios::trunc);
    if (!file_) {
        error = "cannot open log file '" + path + "'";
        return false;
    }
    return true;
}

void Logger::SetConsoleGuard(void (*before)(void*), void (*after)(void*), void* context) {
    std::lock_guard<std::mutex> lock(mutex_);
    guardBefore_ = before;
    guardAfter_ = after;
    guardContext_ = context;
}

void Logger::WriteFileLine(LogLevel level, const std::string& message) {
    if (file_.is_open()) {
        file_ << "[" << LogLevelName(level) << "] " << message << "\n";
    }
}

void Logger::Log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    WriteFileLine(level, message);

    if (level < consoleLevel_ || consoleLevel_ == LogLevel::Quiet) {
        return;
    }
    // The progress line, if any, occupies the current console row. Clear it,
    // print a whole line, then let the reporter repaint.
    if (guardBefore_ != nullptr) {
        guardBefore_(guardContext_);
    }
    std::ostream& out = (level >= LogLevel::Error) ? std::cerr : std::cout;
    out << message << "\n";
    if (guardAfter_ != nullptr) {
        guardAfter_(guardContext_);
    }
}

void Logger::FileOnly(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    WriteFileLine(level, message);
}

void Logger::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
    std::cout.flush();
}

} // namespace gfnif
