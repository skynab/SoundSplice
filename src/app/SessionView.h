#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Song.h"

namespace looper
{
/**
    The session grid: tracks across, scenes down, each cell either holding a
    clip or empty. Clicking a clip launches it; it starts at the next bar line
    (the engine decides exactly when — see engine::SessionPlayer) and loops
    until something replaces or stops it.

    Down the right-hand side is a scene column: clicking a scene launches its
    whole row, so a scene is a complete statement of what the grid should be
    playing. Along the bottom of each track column is a stop button.

    Purely a view: it owns no document state, reads a Song snapshot for what to
    draw, and reports intent through the callbacks. Which cells are actually
    *playing* is engine state, pushed back in via setPlayingSlots — the grid
    can't infer it, because a launch doesn't take effect until the next bar.
*/
class SessionView final : public juce::Component
{
public:
    SessionView() = default;

    std::function<void(int trackIndex, int sceneIndex)> onLaunchClip;
    std::function<void(int sceneIndex)>                 onLaunchScene;
    std::function<void(int trackIndex)>                 onStopTrack;
    std::function<void()>                               onStopAll;
    std::function<void(int trackIndex, int sceneIndex)> onClipSelected; // right-click / empty cell
    std::function<void()>                               onAddScene;
    std::function<void(int sceneIndex)>                 onDeleteScene; // right-click a scene button

    void setSong(const model::Song& song)
    {
        song_ = song;
        repaint();
    }

    /** Which slot each track is currently playing (-1 = none), straight from
        the engine. Not inferable here: a launch is pending until the next bar
        line, so the grid would otherwise light up the wrong cell for up to a
        bar. */
    void setPlayingSlots(const std::vector<int>& slots)
    {
        if (playingSlots_ != slots)
        {
            playingSlots_ = slots;
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const auto point = e.position;

        if (isInSceneColumn(point.x))
        {
            const int scene = sceneAtY(point.y);

            // Left-click launches the row; right-click is the only place a
            // scene can be removed, since the button is the only thing on
            // screen that stands for the scene itself.
            if (scene >= 0 && e.mods.isPopupMenu())
            {
                if (onDeleteScene)
                    onDeleteScene(scene);
            }
            else if (scene >= 0 && onLaunchScene)
            {
                onLaunchScene(scene);
            }
            else if (scene < 0 && isInAddSceneRow(point.y) && onAddScene)
            {
                onAddScene();
            }
            return;
        }

        const int track = trackAtX(point.x);
        if (track < 0)
            return;

        if (isInStopRow(point.y))
        {
            if (onStopTrack)
                onStopTrack(track);
            return;
        }

        const int scene = sceneAtY(point.y);
        if (scene < 0)
            return;

        // An empty cell can't be launched, so a click there is a request to
        // put something in it instead — the owner decides what that means.
        if (model::sessionClip(song_, track, scene) != nullptr)
        {
            if (onLaunchClip)
                onLaunchClip(track, scene);
        }
        else if (onClipSelected)
        {
            onClipSelected(track, scene);
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1a1a1e));

        const int trackCount = (int) song_.tracks.size();
        const int sceneCount = (int) song_.scenes.size();

        paintHeaders(g, trackCount);

        for (int scene = 0; scene < sceneCount; ++scene)
        {
            for (int track = 0; track < trackCount; ++track)
                paintCell(g, track, scene);

            paintSceneButton(g, scene);
        }

        paintAddSceneRow(g);
        paintStopRow(g, trackCount);

        if (trackCount == 0 || sceneCount == 0)
        {
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.setFont(juce::FontOptions(13.0f));
            g.drawText(sceneCount == 0 ? "No scenes yet — click + Scene to add one"
                                       : "No tracks yet",
                       getLocalBounds().reduced(12), juce::Justification::centred);
        }
    }

private:
    static constexpr int kHeaderHeight = 24;
    static constexpr int kRowHeight    = 30;
    static constexpr int kTrackWidth   = 110;
    static constexpr int kSceneWidth   = 96;
    static constexpr int kStopRowHeight = 26;

    int  trackAtX(float x) const
    {
        const int index = (int) (x / (float) kTrackWidth);
        return (x >= 0.0f && index < (int) song_.tracks.size()) ? index : -1;
    }

    int sceneAtY(float y) const
    {
        const int index = (int) ((y - (float) kHeaderHeight) / (float) kRowHeight);
        return (y >= (float) kHeaderHeight && index >= 0 && index < (int) song_.scenes.size()) ? index : -1;
    }

    bool isInSceneColumn(float x) const
    {
        return x >= (float) ((int) song_.tracks.size() * kTrackWidth);
    }

    int sceneRowsBottom() const { return kHeaderHeight + (int) song_.scenes.size() * kRowHeight; }

    bool isInAddSceneRow(float y) const
    {
        return y >= (float) sceneRowsBottom() && y < (float) (sceneRowsBottom() + kRowHeight);
    }

    bool isInStopRow(float y) const
    {
        return y >= (float) sceneRowsBottom() && y < (float) (sceneRowsBottom() + kStopRowHeight);
    }

    juce::Rectangle<int> cellBounds(int track, int scene) const
    {
        return { track * kTrackWidth, kHeaderHeight + scene * kRowHeight, kTrackWidth, kRowHeight };
    }

    bool isPlaying(int track, int scene) const
    {
        return track >= 0 && track < (int) playingSlots_.size() && playingSlots_[(size_t) track] == scene;
    }

    void paintHeaders(juce::Graphics& g, int trackCount) const
    {
        g.setFont(juce::FontOptions(12.0f));
        for (int track = 0; track < trackCount; ++track)
        {
            const juce::Rectangle<int> header(track * kTrackWidth, 0, kTrackWidth, kHeaderHeight);
            g.setColour(juce::Colour(0xff2a2a2e));
            g.fillRect(header.reduced(1));

            const auto& t = song_.tracks[(size_t) track];
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawText(t.name.empty() ? ("Track " + juce::String(track + 1)) : juce::String(t.name),
                       header.reduced(6, 0), juce::Justification::centredLeft);
        }
    }

    void paintCell(juce::Graphics& g, int track, int scene) const
    {
        const auto  bounds = cellBounds(track, scene).reduced(2);
        const auto* clip   = model::sessionClip(song_, track, scene);

        if (clip == nullptr)
        {
            // An empty cell still reads as a cell, so the grid stays legible.
            g.setColour(juce::Colours::white.withAlpha(0.04f));
            g.fillRoundedRectangle(bounds.toFloat(), 3.0f);
            return;
        }

        const bool playing = isPlaying(track, scene);
        g.setColour(playing ? juce::Colour(0xff5aad64) : juce::Colour(0xff3a7d44));
        g.fillRoundedRectangle(bounds.toFloat(), 3.0f);

        if (playing)
        {
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 3.0f, 1.5f);
        }

        // A play triangle for a clip that's sounding, a square for one that's
        // merely loaded — the same shorthand every session grid uses.
        auto marker = bounds.reduced(6).removeFromLeft(10).withSizeKeepingCentre(8, 8).toFloat();
        g.setColour(juce::Colours::white.withAlpha(playing ? 0.95f : 0.6f));
        if (playing)
        {
            juce::Path triangle;
            triangle.addTriangle(marker.getX(), marker.getY(),
                                 marker.getX(), marker.getBottom(),
                                 marker.getRight(), marker.getCentreY());
            g.fillPath(triangle);
        }
        else
        {
            g.fillRect(marker);
        }

        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(juce::String(clip->pattern.notes.size()) + " notes",
                   bounds.reduced(22, 0), juce::Justification::centredLeft);
    }

    void paintSceneButton(juce::Graphics& g, int scene) const
    {
        const juce::Rectangle<int> bounds((int) song_.tracks.size() * kTrackWidth,
                                          kHeaderHeight + scene * kRowHeight,
                                          kSceneWidth, kRowHeight);

        g.setColour(juce::Colour(0xff33333a));
        g.fillRect(bounds.reduced(2));
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::FontOptions(11.0f));

        const auto& name = song_.scenes[(size_t) scene].name;
        g.drawText(name.empty() ? ("Scene " + juce::String(scene + 1)) : juce::String(name),
                   bounds.reduced(8, 0), juce::Justification::centredLeft);
    }

    void paintAddSceneRow(juce::Graphics& g) const
    {
        const juce::Rectangle<int> bounds((int) song_.tracks.size() * kTrackWidth,
                                          sceneRowsBottom(), kSceneWidth, kRowHeight);
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.fillRect(bounds.reduced(2));
        g.setColour(juce::Colours::white.withAlpha(0.65f));
        g.setFont(juce::FontOptions(11.0f));
        g.drawText("+ Scene", bounds.reduced(8, 0), juce::Justification::centredLeft);
    }

    void paintStopRow(juce::Graphics& g, int trackCount) const
    {
        for (int track = 0; track < trackCount; ++track)
        {
            const juce::Rectangle<int> bounds(track * kTrackWidth, sceneRowsBottom(),
                                              kTrackWidth, kStopRowHeight);
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillRect(bounds.reduced(2));

            g.setColour(juce::Colours::white.withAlpha(0.7f));
            const auto square = bounds.withSizeKeepingCentre(8, 8).toFloat();
            g.fillRect(square);
        }
    }

    model::Song      song_;
    std::vector<int> playingSlots_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SessionView)
};

} // namespace looper
