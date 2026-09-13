#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/AudioExport.h"

namespace looper::app
{
/**
    The "what kind of file?" step of an audio export.

    An `AlertWindow` with five combo boxes, following showSpeedPitchDialog's
    pattern rather than building a Component of its own — this is a
    pick-a-few-things-and-go dialog, and a bespoke pane would be more code to
    lay out, parent and audit for no gain.

    The dependent boxes are rebuilt from `engine::AudioExport`'s
    capability queries every time the format changes, rather than from a second
    list kept here. That is the whole reason those queries exist: an export
    that offers an impossible combination doesn't fail loudly, it returns a
    null writer *after* the render has already run.
*/
class ExportAudioDialog
{
public:
    /** Asks for export options. @p onAccepted runs only if the user confirms;
        the dialog owns and deletes itself either way.

        @p defaultSampleRate is the device's rate, so the default export
        matches what is being heard unless the user says otherwise. */
    static void show (juce::Component* parent,
                      double defaultSampleRate,
                      std::function<void (engine::ExportOptions)> onAccepted)
    {
        auto* window = new juce::AlertWindow ("Export Audio", {},
                                              juce::MessageBoxIconType::NoIcon, parent);

        buildControls (*window, defaultSampleRate);

        window->addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        window->enterModalState (true, juce::ModalCallbackFunction::create (
            [window, onAccepted = std::move (onAccepted)] (int result)
            {
                std::unique_ptr<juce::AlertWindow> owned (window);
                if (result != 1 || ! onAccepted)
                    return;

                onAccepted (readOptions (*window));
            }));
    }

    /**
        Adds every control the dialog has and wires them together.

        Public, and the *only* way the boxes get built, so a test drives the
        real dialog rather than a hand-made replica of it. It was a replica
        once: adding the dither box left the copy a box short, and the first
        thing that asked for it dereferenced a null and took the test binary
        down with a segfault.
    */
    static void buildControls (juce::AlertWindow& window, double defaultSampleRate)
    {
        juce::StringArray formatNames;
        for (auto format : engine::allExportFormats())
            formatNames.add (engine::displayNameFor (format));

        juce::StringArray contentsNames;
        for (int i = 0; i < engine::kNumExportContents; ++i)
            contentsNames.add (engine::displayNameFor ((engine::ExportContents) i));

        window.addComboBox ("format", formatNames, "Format:");
        window.addComboBox ("contents", contentsNames, "Contents:");
        window.addComboBox ("rate", {}, "Sample rate:");
        window.addComboBox ("bits", {}, "Bit depth:");
        window.addComboBox ("quality", {}, "Quality:");
        window.addComboBox ("dither", { "On (TPDF)", "Off" }, "Dither:");

        auto* formatBox = window.getComboBoxComponent ("format");
        formatBox->setSelectedItemIndex (0, juce::dontSendNotification);

        refreshDependentBoxes (window, defaultSampleRate);

        formatBox->onChange = [&window, defaultSampleRate]
        {
            refreshDependentBoxes (window, defaultSampleRate);
        };

        // Switching between 24-bit and 32-bit float changes whether anything is
        // quantised at all, so the dither box has to follow the depth as well
        // as the format.
        window.getComboBoxComponent ("bits")->onChange = [&window]
        {
            refreshDitherEnablement (window);
        };
    }

    /** The options currently shown. Public so a test can drive the boxes and
        read back what the dialog would have produced. */
    static engine::ExportOptions readOptions (juce::AlertWindow& window)
    {
        engine::ExportOptions options;

        const int formatIndex = juce::jlimit (0, engine::kNumExportFormats - 1,
                                              window.getComboBoxComponent ("format")->getSelectedItemIndex());
        options.format = engine::allExportFormats()[(size_t) formatIndex];

        const auto rates = engine::possibleSampleRates (options.format);
        if (! rates.isEmpty())
        {
            const int index = juce::jlimit (0, rates.size() - 1,
                                            window.getComboBoxComponent ("rate")->getSelectedItemIndex());
            options.sampleRate = (double) rates[index];
        }

        const auto depths = engine::possibleBitDepths (options.format);
        if (! depths.isEmpty())
        {
            const int index = juce::jlimit (0, depths.size() - 1,
                                            window.getComboBoxComponent ("bits")->getSelectedItemIndex());
            options.bitsPerSample = depths[index];
        }

        const auto qualities = engine::qualityOptionsFor (options.format);
        if (! qualities.isEmpty())
            options.qualityIndex = juce::jlimit (0, qualities.size() - 1,
                                                 window.getComboBoxComponent ("quality")->getSelectedItemIndex());

        if (auto* ditherBox = window.getComboBoxComponent ("dither"))
            options.dither = ditherBox->getSelectedItemIndex() == 0;

        if (auto* contentsBox = window.getComboBoxComponent ("contents"))
            options.contents = (engine::ExportContents)
                                   juce::jlimit (0, engine::kNumExportContents - 1,
                                                 contentsBox->getSelectedItemIndex());

        return options;
    }

    /** Rebuilds the rate/depth/quality boxes for whichever format is selected.
        Public for the same reason as readOptions. */
    static void refreshDependentBoxes (juce::AlertWindow& window, double defaultSampleRate)
    {
        const int formatIndex = juce::jlimit (0, engine::kNumExportFormats - 1,
                                              window.getComboBoxComponent ("format")->getSelectedItemIndex());
        const auto format = engine::allExportFormats()[(size_t) formatIndex];

        // Sample rate. Defaults to the device's where the format can do it, so
        // the common case is a straight render with nothing resampled.
        auto* rateBox = window.getComboBoxComponent ("rate");
        const auto rates = engine::possibleSampleRates (format);
        fill (*rateBox, toStrings (rates, " Hz"), indexOfNearest (rates, (int) defaultSampleRate));

        // Bit depth. Empty for the lossy formats, which have none - the box is
        // disabled rather than hidden so the dialog doesn't change height and
        // move the buttons out from under the pointer.
        auto* bitsBox = window.getComboBoxComponent ("bits");
        const auto depths = engine::possibleBitDepths (format);

        juce::StringArray depthNames;
        for (int bits : depths)
            depthNames.add (bits == 32 ? juce::String ("32-bit float")
                                       : juce::String (bits) + "-bit");

        // 24-bit where it exists: the default an export should have, and the
        // one this app produced before there was a choice.
        fill (*bitsBox, depthNames, juce::jmax (0, depths.indexOf (24)));

        auto* qualityBox = window.getComboBoxComponent ("quality");
        const auto qualities = engine::qualityOptionsFor (format);

        // Near the top of the range rather than the middle: someone exporting
        // a finished mix to a lossy format wants it to sound like the mix.
        fill (*qualityBox, qualities, (qualities.size() * 3) / 4);

        // Dither only means anything where something is quantised: not for the
        // lossy formats, and not at 32-bit float. Enabled/disabled rather than
        // removed, so the dialog keeps its shape as the format changes.
        if (auto* ditherBox = window.getComboBoxComponent ("dither"))
            ditherBox->setSelectedItemIndex (0, juce::dontSendNotification); // On

        refreshDitherEnablement (window);
    }

    /** Dither applies only where the export quantises. Split out from
        refreshDependentBoxes so changing the bit depth can call it without
        rebuilding — and resetting — every other box. */
    static void refreshDitherEnablement (juce::AlertWindow& window)
    {
        auto* formatBox = window.getComboBoxComponent ("format");
        auto* bitsBox   = window.getComboBoxComponent ("bits");
        auto* ditherBox = window.getComboBoxComponent ("dither");

        if (formatBox == nullptr || bitsBox == nullptr || ditherBox == nullptr)
            return;

        const int formatIndex = juce::jlimit (0, engine::kNumExportFormats - 1,
                                              formatBox->getSelectedItemIndex());
        const auto format = engine::allExportFormats()[(size_t) formatIndex];
        const auto depths = engine::possibleBitDepths (format);

        const int selectedDepth = depths.isEmpty()
                                    ? 0
                                    : depths[juce::jlimit (0, depths.size() - 1,
                                                           bitsBox->getSelectedItemIndex())];

        ditherBox->setEnabled (! depths.isEmpty() && selectedDepth < 32);
    }

private:
    static juce::StringArray toStrings (const juce::Array<int>& values, const juce::String& suffix)
    {
        juce::StringArray strings;
        for (int value : values)
            strings.add (juce::String (value) + suffix);
        return strings;
    }

    /** An empty list leaves a disabled box showing a dash, rather than an
        empty one that looks broken or a removed one that reflows the dialog. */
    static void fill (juce::ComboBox& box, const juce::StringArray& items, int selectedIndex)
    {
        box.clear (juce::dontSendNotification);

        if (items.isEmpty())
        {
            box.addItem ("-", 1);
            box.setSelectedItemIndex (0, juce::dontSendNotification);
            box.setEnabled (false);
            return;
        }

        box.addItemList (items, 1);
        box.setSelectedItemIndex (juce::jlimit (0, items.size() - 1, selectedIndex),
                                  juce::dontSendNotification);
        box.setEnabled (true);
    }

    static int indexOfNearest (const juce::Array<int>& values, int wanted)
    {
        int best = 0;
        for (int i = 1; i < values.size(); ++i)
            if (std::abs (values[i] - wanted) < std::abs (values[best] - wanted))
                best = i;
        return best;
    }
};

} // namespace looper::app
