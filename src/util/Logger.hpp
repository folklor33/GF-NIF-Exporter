#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace gfnif {

enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3, Quiet = 4 };

/*! Thread-safe log sink: an optional console stream plus an optional file.
 *
 *  Two properties matter for this pipeline:
 *
 *  - Workers log concurrently, so every emission takes the mutex and writes one
 *    whole line at a time. Interleaved half-lines would make a 2825-file run
 *    unreadable.
 *  - In --noverb the console is owned by a repainting progress line. The logger
 *    must therefore be able to erase that line before printing and let the
 *    reporter redraw it, which is what the ConsoleGuard callback is for. The
 *    log FILE keeps receiving everything regardless of console mode, so
 *    --log-file is always complete. */
class Logger {
public:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /*! Minimum level echoed to the console. The file always gets everything
     *  from Debug up, so a --noverb run still yields a full log. */
    void SetConsoleLevel(LogLevel level);

    /*! Opens `path` for the detailed log. Returns false and fills `error` if it
     *  cannot be opened; the caller decides whether that is fatal. */
    bool OpenFile(const std::string& path, std::string& error);

    /*! Installs callbacks the logger uses to clear and restore a progress line
     *  that shares the console. Both may be null. */
    void SetConsoleGuard(void (*before)(void*), void (*after)(void*), void* context);

    void Log(LogLevel level, const std::string& message);

    void Debug(const std::string& m) { Log(LogLevel::Debug, m); }
    void Info(const std::string& m) { Log(LogLevel::Info, m); }
    void Warning(const std::string& m) { Log(LogLevel::Warning, m); }
    void Error(const std::string& m) { Log(LogLevel::Error, m); }

    /*! Writes to the log file only, never the console. Used for per-file detail
     *  that would drown the console in --noverb but must still be recorded. */
    void FileOnly(LogLevel level, const std::string& message);

    void Flush();

private:
    void WriteFileLine(LogLevel level, const std::string& message);

    std::mutex mutex_;
    std::ofstream file_;
    LogLevel consoleLevel_ = LogLevel::Info;
    void (*guardBefore_)(void*) = nullptr;
    void (*guardAfter_)(void*) = nullptr;
    void* guardContext_ = nullptr;
};

/*! "DEBUG" / "INFO" / ... for log lines. */
const char* LogLevelName(LogLevel level);

} // namespace gfnif
