#pragma once

#include <functional>
#include <set>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"
#include "model/TextEdit.h"

namespace soundsplice
{
/**
    The Transcript pane: the selected track's words as running text, to edit
    the audio by - select words and Delete cuts them (model/TextEdit.h);
    click a word to put the playhead on it. Pauses longer than a second show
    as their length, to select and take out like words.

    Find Fillers and Find Long Pauses select every "um" and "uh", or every
    long pause, for review rather than cutting them: Ctrl-click lets any one
    go, and nothing changes until Delete. Low-confidence words are drawn
    fainter, so a mishearing is easy to spot.

    Owns no document state: the owner feeds it words and acts on its
    callbacks.
*/
class TranscriptPane final : public juce::Component,
                             private juce::KeyListener
{
public:
    std::function<void(double seconds)>                               onSeek;
    std::function<void(const std::vector<std::pair<double, double>>&)> onDelete; // timeline seconds
    std::function<void()>                                              onTranscribe;

    /** Pauses shorter than this aren't shown; Find Long Pauses keeps this much
        of each one it finds. */
    static constexpr double kPauseSeconds = 1.0, kKeepSeconds = 0.4;

    TranscriptPane()
    {
        text_.owner = this;
        viewport_.setViewedComponent(&text_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);

        transcribeButton_.onClick = [this] { if (onTranscribe) onTranscribe(); };
        fillersButton_.onClick    = [this] { selectWhere([](const Token& t) { return t.filler; }, "filler words"); };
        pausesButton_.onClick     = [this] { selectWhere([](const Token& t) { return t.pause; }, "long pauses"); };
        deleteButton_.onClick     = [this] { deleteSelected(); };
        transcribeButton_.setTooltip("Transcribe the selected track's audio with the model chosen in Preferences > Folders");
        fillersButton_.setTooltip("Select every um, uh and erm, to look over before Delete takes them out");
        pausesButton_.setTooltip("Select every pause over a second; Delete shortens each to under half a second");
        for (auto* button : { &transcribeButton_, &fillersButton_, &pausesButton_, &deleteButton_ })
            addAndMakeVisible(*button);
        status_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.6f));
        addAndMakeVisible(status_);

        text_.setWantsKeyboardFocus(true);
        text_.addKeyListener(this);
        text_.setTitle("Transcript");
        setWords({}, {});
    }

    ~TranscriptPane() override { text_.removeKeyListener(this); }

    /** The words to show, and what to say when there are none. */
    void setWords(const std::vector<model::textedit::TrackWord>& words, const juce::String& emptyMessage)
    {
        tokens_.clear();
        for (size_t i = 0; i < words.size(); ++i)
        {
            if (i > 0 && words[i].start - words[i - 1].end > kPauseSeconds)
            {
                Token pause;
                pause.pause = true;
                pause.start = words[i - 1].end;
                pause.end   = words[i].start;
                pause.text  = "[" + juce::String(pause.end - pause.start, 1) + " s]";
                tokens_.push_back(pause);
            }
            Token word;
            word.text       = juce::String::fromUTF8(words[i].text.c_str());
            word.start      = words[i].start;
            word.end        = words[i].end;
            word.confidence = words[i].confidence;
            word.filler     = model::textedit::isFiller(words[i].text);
            tokens_.push_back(word);
        }
        // A selection made of old tokens means nothing now.
        selected_.clear();
        anchor_ = -1;
        emptyMessage_ = emptyMessage;
        updateStatus();
        layoutText();
    }

    /** Where the playhead is, in timeline seconds: the word under it is marked. */
    void setPlayhead(double seconds)
    {
        int playing = -1;
        for (int i = 0; i < (int) tokens_.size(); ++i)
            if (! tokens_[(size_t) i].pause && seconds >= tokens_[(size_t) i].start && seconds < tokens_[(size_t) i].end)
                playing = i;
        if (playing != playing_)
        {
            playing_ = playing;
            text_.repaint();
        }
    }

    // ---- for tests
    int  tokenCount() const noexcept { return (int) tokens_.size(); }
    void selectTokenForTesting(int index, bool extend, bool toggle) { clickToken(index, extend, toggle); }
    int  selectedCount() const noexcept { return (int) selected_.size(); }
    void deleteForTesting() { deleteSelected(); }
    void findFillersForTesting() { fillersButton_.onClick(); }
    void findPausesForTesting() { pausesButton_.onClick(); }

    void paint(juce::Graphics& g) override { g.fillAll(theme::surface(*this, theme::paneId)); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto top  = area.removeFromTop(28);
        transcribeButton_.setBounds(top.removeFromLeft(100).reduced(0, 2));
        top.removeFromLeft(6);
        fillersButton_.setBounds(top.removeFromLeft(100).reduced(0, 2));
        top.removeFromLeft(6);
        pausesButton_.setBounds(top.removeFromLeft(130).reduced(0, 2));
        top.removeFromLeft(6);
        deleteButton_.setBounds(top.removeFromLeft(80).reduced(0, 2));
        area.removeFromTop(4);
        status_.setBounds(area.removeFromBottom(20));
        viewport_.setBounds(area);
        layoutText();
    }

private:
    struct Token
    {
        juce::String           text;
        double                 start = 0.0, end = 0.0;
        float                  confidence = 1.0f;
        bool                   filler = false, pause = false;
        juce::Rectangle<float> box; // where it's drawn
    };

    /** The words themselves: flowed like text, clickable. */
    struct Text final : juce::Component
    {
        TranscriptPane* owner = nullptr;

        void paint(juce::Graphics& g) override
        {
            if (owner->tokens_.empty())
            {
                g.setColour(juce::Colours::white.withAlpha(0.5f));
                g.setFont(juce::FontOptions(14.0f));
                g.drawFittedText(owner->emptyMessage_, getLocalBounds().reduced(8), juce::Justification::topLeft, 4);
                return;
            }
            g.setFont(owner->font());
            for (int i = 0; i < (int) owner->tokens_.size(); ++i)
            {
                const auto& t        = owner->tokens_[(size_t) i];
                const bool  selected = owner->selected_.count(i) > 0;
                if (selected)
                {
                    g.setColour(juce::Colours::steelblue.withAlpha(0.55f));
                    g.fillRoundedRectangle(t.box.expanded(1.0f, 0.0f), 3.0f);
                }
                else if (i == owner->playing_)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.15f));
                    g.fillRoundedRectangle(t.box.expanded(1.0f, 0.0f), 3.0f);
                }
                auto colour = juce::Colours::white;
                if (t.pause)
                    colour = juce::Colour(0xff8fb6d9);
                else if (t.filler)
                    colour = juce::Colour(0xffffb36b);
                g.setColour(colour.withAlpha(t.pause ? 0.8f : juce::jlimit(0.45f, 1.0f, 0.35f + t.confidence)));
                g.drawText(t.text, t.box, juce::Justification::centredLeft, false);
            }
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            grabKeyboardFocus();
            for (int i = 0; i < (int) owner->tokens_.size(); ++i)
                if (owner->tokens_[(size_t) i].box.contains(e.position))
                {
                    owner->clickToken(i, e.mods.isShiftDown(), e.mods.isCommandDown());
                    return;
                }
            owner->selected_.clear();
            owner->updateStatus();
            repaint();
        }
    };

    juce::Font font() const { return juce::Font(juce::FontOptions(17.0f)); }

    void layoutText()
    {
        const int   width = juce::jmax(100, viewport_.getMaximumVisibleWidth() - 4);
        const float line  = 26.0f, gap = 6.0f;
        float x = 4.0f, y = 4.0f;
        const auto f = font();
        for (auto& t : tokens_)
        {
            const float w = juce::GlyphArrangement::getStringWidth(f, t.text) + 2.0f;
            if (x + w > (float) width && x > 4.0f)
            {
                x = 4.0f;
                y += line;
            }
            t.box = { x, y, w, line - 4.0f };
            x += w + gap;
        }
        text_.setSize(width, (int) (y + line + 8.0f));
        text_.repaint();
    }

    void clickToken(int index, bool extend, bool toggle)
    {
        if (index < 0 || index >= (int) tokens_.size())
            return;
        if (toggle)
        {
            if (! selected_.erase(index))
                selected_.insert(index);
        }
        else if (extend && anchor_ >= 0)
        {
            selected_.clear();
            for (int i = juce::jmin(anchor_, index); i <= juce::jmax(anchor_, index); ++i)
                selected_.insert(i);
        }
        else
        {
            selected_ = { index };
            anchor_   = index;
            if (onSeek)
                onSeek(tokens_[(size_t) index].start);
        }
        updateStatus();
        text_.repaint();
    }

    template <typename Predicate>
    void selectWhere(Predicate want, const char* what)
    {
        selected_.clear();
        for (int i = 0; i < (int) tokens_.size(); ++i)
            if (want(tokens_[(size_t) i]))
                selected_.insert(i);
        updateStatus(selected_.empty() ? juce::String("No ") + what
                                       : juce::String((int) selected_.size()) + " " + what
                                             + " selected - Ctrl-click any to keep it, Delete to take them out");
        text_.repaint();
        text_.grabKeyboardFocus();
    }

    /** The selection as stretches of timeline to cut: a run of selected words
        one stretch (the pauses inside it too), a selected pause shortened
        rather than closed up, so the words either side still breathe. */
    std::vector<std::pair<double, double>> selectedRanges() const
    {
        std::vector<std::pair<double, double>> ranges;
        int i = 0;
        while (i < (int) tokens_.size())
        {
            if (selected_.count(i) == 0)
            {
                ++i;
                continue;
            }
            int j = i;
            while (j + 1 < (int) tokens_.size() && selected_.count(j + 1) > 0)
                ++j;
            if (i == j && tokens_[(size_t) i].pause)
            {
                const auto cut = model::textedit::shortened(tokens_[(size_t) i].start, tokens_[(size_t) i].end, kKeepSeconds);
                if (cut.second > cut.first)
                    ranges.push_back(cut);
            }
            else
            {
                // From the first selected word to the last, and the gap
                // before the next word: a deleted sentence takes its breath
                // with it, but never the next word's start.
                double start = tokens_[(size_t) i].start, end = tokens_[(size_t) j].end;
                if (j + 1 < (int) tokens_.size() && ! tokens_[(size_t) j + 1].pause)
                    end = juce::jmin(tokens_[(size_t) j + 1].start, end + 0.15);
                ranges.emplace_back(start, end);
            }
            i = j + 1;
        }
        return ranges;
    }

    void deleteSelected()
    {
        const auto ranges = selectedRanges();
        if (ranges.empty() || ! onDelete)
            return;
        onDelete(ranges);
    }

    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            deleteSelected();
            return true; // the arrangement's own Delete mustn't also act
        }
        if (key == juce::KeyPress::escapeKey && ! selected_.empty())
        {
            selected_.clear();
            updateStatus();
            text_.repaint();
            return true;
        }
        return false;
    }

    void updateStatus(const juce::String& message = {})
    {
        if (message.isNotEmpty())
            status_.setText(message, juce::dontSendNotification);
        else if (tokens_.empty())
            status_.setText({}, juce::dontSendNotification);
        else
        {
            int words = 0;
            for (const auto& t : tokens_)
                words += t.pause ? 0 : 1;
            status_.setText(juce::String(words) + " words" + (selected_.empty() ? juce::String()
                                                                              : ", " + juce::String((int) selected_.size()) + " selected"),
                            juce::dontSendNotification);
        }
        deleteButton_.setEnabled(! selected_.empty());
        fillersButton_.setEnabled(! tokens_.empty());
        pausesButton_.setEnabled(! tokens_.empty());
    }

    std::vector<Token> tokens_;
    std::set<int>      selected_;
    int                anchor_  = -1;
    int                playing_ = -1;
    juce::String       emptyMessage_;
    Text               text_;
    juce::Viewport     viewport_;
    juce::TextButton   transcribeButton_ { "Transcribe" }, fillersButton_ { "Find Fillers" },
                       pausesButton_ { "Find Long Pauses" }, deleteButton_ { "Delete" };
    juce::Label        status_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TranscriptPane)
};

} // namespace soundsplice
