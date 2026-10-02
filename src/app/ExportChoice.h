#pragma once

#include <juce_core/juce_core.h>

#include "engine/AudioExport.h"

namespace soundsplice::app
{
// Everything Export Audio asks, as data: the dialog fills one in, presets and
// the render queue keep them (ExportChoices.h), and soundsplice-cli makes
// them. JUCE's core only, so the tool can.

/** What an export covers. */
enum class ExportRange
{
    Project,       // everything arranged, with a tail for reverbs to ring out
    TimeSelection, // exactly the time selection
    MarkerRanges,  // one file per marker range, named from a pattern
};

/** Whether the files carry the project's info as tags, and its markers as
    chapters. */
enum class ExportTagging
{
    InfoAndChapters,
    InfoOnly,
    None,
};

/** Everything the dialog asks. */
struct ExportChoice
{
    engine::ExportOptions options;
    ExportRange           range       = ExportRange::Project;
    juce::String          namePattern = "$project - $region";
    ExportTagging         tagging     = ExportTagging::InfoAndChapters;
    bool                  report      = false; // an HTML render report beside each file

    // The time selection's span, kept with a queued export (the queue renders
    // later, when the selection may be something else). 0: the current one.
    double                selectionStartBeats  = 0.0;
    double                selectionLengthBeats = 0.0;
};

/** What the dialog was closed with. */
enum class ExportAction
{
    Export     = 1,
    Queue      = 2, // File > Render Queue renders it later
    SavePreset = 3,
};

/** A named set of choices, for the dialog's Preset box. */
struct NamedExportChoice
{
    juce::String name;
    ExportChoice choice;
};

/** The loudness targets offered, and what each is for. */
struct ExportLoudnessTarget
{
    double      lufs; // 0: leave it
    const char* name;
};

inline constexpr ExportLoudnessTarget kExportLoudnessTargets[] {
    { 0.0, "Leave it" },
    { -14.0, "-14 LUFS (streaming music)" },
    { -16.0, "-16 LUFS (podcasts)" },
    { -18.0, "-18 LUFS (audiobooks, quieter podcasts)" },
    { -23.0, "-23 LUFS (EBU R128 broadcast)" },
    { -24.0, "-24 LUFS (ATSC A/85 broadcast)" },
};

} // namespace soundsplice::app
