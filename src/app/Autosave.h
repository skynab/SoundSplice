#pragma once

#include <algorithm>
#include <string>

namespace soundsplice::app
{
/**
    The pieces of crash recovery that aren't UI: what an autosave file holds,
    and when one should be written.

    An autosave is the project text exactly as Save would write it, behind a
    two-line header naming the project it belongs to. It is not a project file
    and must not open as one: the header's first line isn't the project
    format's, so model::deserialize refuses it. An autosave picked by accident
    in File > Open can't quietly become someone's project.

    JUCE-free so the format and the timing rule are tested headless; the
    file handling itself lives in MainComponent.
*/
inline constexpr const char* kAutosaveHeader = "SOUNDSPLICE-AUTOSAVE 1";

/** What an autosave file holds. */
struct AutosaveContents
{
    std::string originalPath; // the project file this was autosaved from; empty for an untitled project
    std::string projectText;  // model::serialize output
};

/** @p projectText wrapped as an autosave of the project at @p originalPath. */
inline std::string wrapAutosave(const std::string& projectText, const std::string& originalPath)
{
    // The path is the rest of its line, so it can't hold a line break.
    auto path = originalPath;
    std::replace(path.begin(), path.end(), '\n', ' ');
    std::replace(path.begin(), path.end(), '\r', ' ');

    return std::string(kAutosaveHeader) + "\nPROJECT " + path + "\n" + projectText;
}

/** The inverse of wrapAutosave. False for anything that isn't an autosave,
    including a plain project file, or one cut off before any project text. */
inline bool unwrapAutosave(const std::string& text, AutosaveContents& out)
{
    auto lineAt = [&text](size_t start, size_t& end) -> std::string
    {
        end = text.find('\n', start);
        if (end == std::string::npos)
            return {};

        auto line = text.substr(start, end - start);
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        return line;
    };

    size_t     headerEnd = 0;
    const auto header    = lineAt(0, headerEnd);
    if (headerEnd == std::string::npos || header != kAutosaveHeader)
        return false;

    size_t     projectEnd  = 0;
    const auto projectLine = lineAt(headerEnd + 1, projectEnd);
    if (projectEnd == std::string::npos || projectLine.rfind("PROJECT", 0) != 0)
        return false;

    const std::string prefix = "PROJECT ";
    out.originalPath = projectLine.size() > prefix.size() ? projectLine.substr(prefix.size()) : std::string();
    out.projectText  = text.substr(projectEnd + 1);
    return ! out.projectText.empty();
}

/** Whether to write an autosave now: the document holds changes that aren't
    on disk (@p currentStateId differs from @p savedStateId), the last
    autosave doesn't already hold them (it differs from @p autosavedStateId),
    and at least @p intervalSeconds have passed since the last write — so a
    burst of edits costs one write rather than one per edit. */
inline bool autosaveDue(unsigned long long currentStateId, unsigned long long savedStateId,
                        unsigned long long autosavedStateId, double secondsSinceLastAutosave,
                        double intervalSeconds)
{
    return currentStateId != savedStateId
        && currentStateId != autosavedStateId
        && secondsSinceLastAutosave >= intervalSeconds;
}

} // namespace soundsplice::app
