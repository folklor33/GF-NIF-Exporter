#pragma once

#include <cstddef>
#include <mutex>
#include <string>

namespace gfnif {

class Logger;

/*! How the run talks to the console. */
enum class ReportMode {
    Verbose,  //!< a block of lines per file (--debug)
    Normal,   //!< one line per file
    Compact   //!< a single repainting progress line (--noverb)
};

/*! Renders run progress, in whichever of the three console modes is active.
 *
 *  Compact mode owns the console's current line and repaints it in place. Any
 *  other output -- a warning, an error, the final summary -- has to erase that
 *  line first or it leaves debris behind, so the Logger is given
 *  ClearLine/Redraw callbacks that route through here. */
class ProgressReporter {
public:
    ProgressReporter(ReportMode mode, std::size_t total, Logger& logger);

    /*! Announces a file is about to be processed. Only prints in Verbose mode,
     *  where processing is sequential and a header keeps the log readable. */
    void BeginFile(const std::string& relativePath, std::size_t index);

    /*! Records one finished file and refreshes the display. */
    void EndFile(const std::string& relativePath, const std::string& summary, bool failed);

    /*! Erases the progress line, if one is currently drawn. */
    void ClearLine();
    /*! Repaints the progress line after other output. */
    void Redraw();

    /*! Leaves the progress line behind for good, before the final summary. */
    void Finish();

    std::size_t Completed() const { return completed_; }
    std::size_t Failed() const { return failed_; }

    /*! Callbacks matching Logger::SetConsoleGuard. */
    static void GuardBefore(void* self);
    static void GuardAfter(void* self);

private:
    void DrawLocked();
    void EraseLocked();

    ReportMode mode_;
    std::size_t total_;
    Logger& logger_;

    std::mutex mutex_;
    std::size_t completed_ = 0;
    std::size_t failed_ = 0;
    std::size_t lastDrawnWidth_ = 0;
    bool lineDrawn_ = false;
    std::string lastPath_;
    bool finished_ = false;
};

} // namespace gfnif
