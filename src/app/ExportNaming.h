#pragma once

#include <map>

#include <juce_core/juce_core.h>

namespace soundsplice::app::exportnaming
{
/**
    File names from a pattern with wildcards, for an export that writes one
    file per region (REAPER's render wildcards):

        $project  the project's name ("Untitled" before it's saved)
        $region   the marker range's name, or "Region 3" for an unnamed one
        $index    its number, 01, 02, ... in timeline order
        $date     today, 2026-10-02

    An unknown $word is left as it is, so a typo shows in the file name
    rather than silently vanishing. Whatever comes out is made a legal file
    name; an empty result falls back to the index.
*/
inline juce::String expand(const juce::String& pattern, const std::map<juce::String, juce::String>& fields)
{
    juce::String out;
    for (int i = 0; i < pattern.length();)
    {
        if (pattern[i] == '$')
        {
            int end = i + 1;
            while (end < pattern.length() && juce::CharacterFunctions::isLetter(pattern[end]))
                ++end;
            const auto word = pattern.substring(i + 1, end).toLowerCase();
            if (const auto it = fields.find(word); it != fields.end())
            {
                out << it->second;
                i = end;
                continue;
            }
        }
        out << pattern[i];
        ++i;
    }

    out = juce::File::createLegalFileName(out.trim());
    if (out.isEmpty())
        if (const auto it = fields.find("index"); it != fields.end())
            out = it->second;
    return out;
}

/** The fields for region @p index (from 0) of @p count, named @p name. */
inline std::map<juce::String, juce::String> regionFields(const juce::String& project, const juce::String& name, int index,
                                                         int count)
{
    const int digits = juce::jmax(2, juce::String(count).length());
    return {
        { "project", project.isEmpty() ? juce::String("Untitled") : project },
        { "region", name.trim().isEmpty() ? "Region " + juce::String(index + 1) : name.trim() },
        { "index", juce::String(index + 1).paddedLeft('0', digits) },
        { "date", juce::Time::getCurrentTime().formatted("%Y-%m-%d") },
    };
}

/** @p names made distinct: a repeat gets " (2)", " (3)" after it, so two
    regions with one name don't overwrite each other's file. */
inline juce::StringArray distinct(const juce::StringArray& names)
{
    juce::StringArray out;
    for (const auto& name : names)
    {
        auto candidate = name;
        for (int n = 2; out.contains(candidate, true); ++n)
            candidate = name + " (" + juce::String(n) + ")";
        out.add(candidate);
    }
    return out;
}

} // namespace soundsplice::app::exportnaming
