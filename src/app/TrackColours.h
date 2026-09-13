#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Track.h"

namespace looper
{
/** One entry in the track-colour menu. */
struct TrackColourOption
{
    const char*  name;
    juce::uint32 argb; // 0 = "leave it alone", i.e. the default lane colour
};

/** The palette offered per track.

    Deliberately a fixed set rather than a colour picker: the point of track
    colours is telling parts apart at a glance, and that works better with a
    handful of distinguishable hues than with the whole spectrum, most of
    which is indistinguishable at the size a clip is drawn.

    Stored on the track as the ARGB value rather than as an index into this
    list, so reordering or extending the palette can't silently recolour
    everyone's existing projects.
*/
inline constexpr TrackColourOption kTrackColours[] = {
    { "Default", 0x00000000 },
    { "Red",     0xffb0413e },
    { "Orange",  0xffb0703a },
    { "Yellow",  0xff9c8f34 },
    { "Green",   0xff3a7d44 },
    { "Teal",    0xff2f7d78 },
    { "Blue",    0xff36618e },
    { "Purple",  0xff6b4a8f },
};

inline constexpr int kNumTrackColours = (int) (sizeof(kTrackColours) / sizeof(kTrackColours[0]));

/** The lane colour a track with no colour of its own gets — the green clips
    were drawn in before any of this existed, so untouched projects look
    exactly as they did. */
inline constexpr juce::uint32 kDefaultTrackColour = 0xff3a7d44;

inline juce::Colour trackColour(juce::uint32 stored)
{
    return juce::Colour(stored == 0 ? kDefaultTrackColour : stored);
}

/** A short tag naming what kind of track this is.

    Track names double as the type indicator until someone renames one — call
    a guitar track "Verse" and nothing on screen says it is a guitar any more.
    This is what keeps that readable, so renaming costs nothing.
*/
inline const char* trackTypeTag(model::TrackType type)
{
    switch (type)
    {
        case model::TrackType::Instrument: return "SYN";
        case model::TrackType::Audio:      return "AUD";
        case model::TrackType::Drum:       return "DRM";
        case model::TrackType::Guitar:     return "GTR";
        case model::TrackType::Bus:        return "BUS";
    }
    return "SYN";
}

/** How tall a track-identity header (see paintTrackHeader) is. A caller
    reserves this much space itself; nothing here lays anything out. */
inline constexpr int kTrackHeaderHeight = 20;

/**
    Draws a track-identity strip: a colour swatch, the track's type tag, and
    its name — the same three things ArrangementView already shows per lane,
    reused here so a pane reached through a dock tab (whose title doesn't
    change per track) can say which track is actually open.

    Switching tracks while parked on a tab titled "Keys"/"Synth"/"Drums"/
    "Guitar" gave no on-screen confirmation of which track's notes, patch,
    kit, or fretboard was showing — this is the fix, applied the same way in
    every pane that needed it rather than once per file.

    A free function rather than a Component: every pane that needs this
    already owns its paint()/resized(), and a real child Component would
    mean each of them re-deriving bounds math a caller can just pass in
    directly.
*/
inline void paintTrackHeader(juce::Graphics& g, juce::Rectangle<int> bounds,
                             const juce::String& trackName, juce::uint32 storedColour,
                             model::TrackType type)
{
    if (bounds.getHeight() <= 0 || bounds.getWidth() <= 0)
        return;

    const auto colour = trackColour(storedColour);

    g.setColour(juce::Colours::white.withAlpha(0.04f));
    g.fillRect(bounds);

    g.setColour(colour);
    g.fillRect(bounds.removeFromLeft(4));
    bounds.removeFromLeft(6);

    const auto tagArea = bounds.removeFromLeft(32).withSizeKeepingCentre(32, 16);
    g.setColour(juce::Colours::white.withAlpha(0.12f));
    g.fillRoundedRectangle(tagArea.toFloat(), 3.0f);
    g.setColour(juce::Colours::white.withAlpha(0.7f));
    g.setFont(juce::FontOptions(10.0f));
    g.drawText(trackTypeTag(type), tagArea, juce::Justification::centred);

    bounds.removeFromLeft(6);
    g.setColour(juce::Colours::white.withAlpha(0.85f));
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(trackName.isEmpty() ? juce::String("(unnamed track)") : trackName,
               bounds, juce::Justification::centredLeft);
}

} // namespace looper
