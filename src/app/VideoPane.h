#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_video/juce_video.h>

#include "Theme.h"

namespace soundsplice
{
/**
    The Video pane: a reference video to work to - dubbing, a podcast's
    video, a score - kept in step with the playhead (REAPER's and
    Audition's video window). The project keeps which file and when it
    starts (Song::video); the app moves it with the transport (sync).

    Picture only: the video's own sound is muted, so what's heard is the
    project. Played through the system's own decoders (juce_video), so it
    opens what the system can play.
*/
class VideoPane final : public juce::Component
{
public:
    std::function<void()>              onLoad;
    std::function<void()>              onRemove;
    std::function<void(double offset)> onOffsetChanged; // seconds into the song the video starts at

    VideoPane()
    {
        addAndMakeVisible(video_);
        loadButton_.onClick   = [this] { if (onLoad) onLoad(); };
        removeButton_.onClick = [this] { if (onRemove) onRemove(); };
        addAndMakeVisible(loadButton_);
        addAndMakeVisible(removeButton_);

        offsetLabel_.setText("Starts at (s):", juce::dontSendNotification);
        addAndMakeVisible(offsetLabel_);
        offset_.setInputRestrictions(10, "-0123456789.");
        offset_.onReturnKey = [this] { commitOffset(); };
        offset_.onFocusLost = [this] { commitOffset(); };
        offset_.setTooltip("Where in the song the video's first frame is");
        addAndMakeVisible(offset_);

        status_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.6f));
        addAndMakeVisible(status_);
        video_.onErrorOccurred = [this](const juce::String& error) { status_.setText("Can't play it: " + error, juce::dontSendNotification); };
        show({}, 0.0);
    }

    /** The project's video (none: an empty file) and where it starts. */
    void show(const juce::File& file, double offsetSeconds)
    {
        offsetSeconds_ = offsetSeconds;
        offset_.setText(juce::String(offsetSeconds, 2), false);
        if (file == loaded_)
            return;
        loaded_ = file;
        video_.closeVideo();
        removeButton_.setEnabled(file != juce::File());
        if (file == juce::File())
        {
            status_.setText("No video: Load Video to work to one", juce::dontSendNotification);
            return;
        }
        playSpeed_        = 1.0; // a newly opened video plays at its own speed
        const auto result = video_.load(file);
        if (result.failed())
        {
            status_.setText("Can't open " + file.getFileName() + ": " + result.getErrorMessage(), juce::dontSendNotification);
            return;
        }
        video_.setAudioVolume(0.0f); // picture only: the project is what's heard
        status_.setText(file.getFileName() + ", " + juce::String(video_.getVideoDuration(), 1) + " s", juce::dontSendNotification);
    }

    /** Called often: the song's playhead, in seconds, whether it's playing,
        and how fast (play-at-speed), so the picture keeps pace rather than
        being jumped back into step over and over. */
    void sync(double songSeconds, bool playing, double speed = 1.0)
    {
        if (! video_.isVideoOpen())
            return;
        if (speed > 0.0 && std::abs(speed - playSpeed_) > 1.0e-6)
        {
            playSpeed_ = speed;
            video_.setPlaySpeed(speed);
        }
        const double target = songSeconds - offsetSeconds_;
        const bool   inside = target >= 0.0 && target < video_.getVideoDuration();
        if (! inside)
        {
            if (video_.isPlaying())
                video_.stop();
            return;
        }
        const double drift = std::abs(video_.getPlayPosition() - target);
        if (playing)
        {
            if (! video_.isPlaying())
            {
                video_.setPlayPosition(target);
                video_.play();
            }
            else if (drift > 0.15) // fallen behind or run ahead: jump back into step
                video_.setPlayPosition(target);
        }
        else
        {
            if (video_.isPlaying())
                video_.stop();
            if (drift > 0.02) // scrubbing: the frame under the playhead
                video_.setPlayPosition(target);
        }
    }

    void paint(juce::Graphics& g) override { g.fillAll(theme::surface(*this, theme::insetId)); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(6);
        auto top  = area.removeFromTop(26);
        loadButton_.setBounds(top.removeFromLeft(100).reduced(0, 1));
        top.removeFromLeft(4);
        removeButton_.setBounds(top.removeFromLeft(80).reduced(0, 1));
        top.removeFromLeft(10);
        offsetLabel_.setBounds(top.removeFromLeft(90));
        offset_.setBounds(top.removeFromLeft(70).reduced(0, 2));
        area.removeFromTop(4);
        status_.setBounds(area.removeFromBottom(20));
        video_.setBounds(area);
    }

    double offsetForTesting() const { return offsetSeconds_; }
    juce::TextEditor& offsetEditorForTesting() { return offset_; }

private:
    void commitOffset()
    {
        const double value = offset_.getText().getDoubleValue();
        if (std::abs(value - offsetSeconds_) < 1e-9)
            return;
        offsetSeconds_ = value;
        if (onOffsetChanged)
            onOffsetChanged(value);
    }

    juce::VideoComponent video_ { false };
    juce::TextButton     loadButton_ { "Load Video..." }, removeButton_ { "Remove" };
    juce::Label          offsetLabel_, status_;
    juce::TextEditor     offset_;
    juce::File           loaded_;
    double               offsetSeconds_ = 0.0;
    double               playSpeed_     = 1.0; // what the video was last told to play at

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoPane)
};

} // namespace soundsplice
