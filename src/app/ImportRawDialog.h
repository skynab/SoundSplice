#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/RawPcm.h"

namespace soundsplice::app
{
/**
    The "what are these bytes?" step of Import Raw Data: encoding, byte order,
    channels, sample rate and a header to skip. An AlertWindow, following
    ExportAudioDialog: a pick-a-few-things-and-go dialog.
*/
class ImportRawDialog
{
public:
    /** Asks how to read @p file. @p onAccepted runs only if the user confirms
        with settings that make sense; the dialog owns and deletes itself. */
    static void show(juce::Component* parent, const juce::File& file,
                     std::function<void(engine::RawPcmFormat)> onAccepted)
    {
        auto* window = new juce::AlertWindow("Import Raw Data",
                                             "How the samples in " + file.getFileName() + " are stored",
                                             juce::MessageBoxIconType::NoIcon, parent);
        buildControls(*window);

        window->addButton("Import", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        window->enterModalState(true, juce::ModalCallbackFunction::create(
            [window, onAccepted = std::move(onAccepted)](int result)
            {
                std::unique_ptr<juce::AlertWindow> owned(window);
                if (result != 1 || ! onAccepted)
                    return;

                const auto format = readFormat(*window);
                if (! format.isValid())
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import Raw Data",
                                                           "Channels must be 1 to 32 and the sample rate above zero.");
                    return;
                }
                onAccepted(format);
            }));
    }

    /** Adds the controls, with the defaults Audacity starts from. Public so a
        test drives the real dialog. */
    static void buildControls(juce::AlertWindow& window)
    {
        juce::StringArray encodings;
        for (int i = 0; i < engine::kNumRawEncodings; ++i)
            encodings.add(engine::displayNameFor((engine::RawEncoding) i));

        window.addComboBox("encoding", encodings, "Encoding:");
        window.addComboBox("order", { "Little-endian", "Big-endian" }, "Byte order:");
        window.addTextEditor("channels", "1", "Channels:");
        window.addTextEditor("rate", "44100", "Sample rate (Hz):");
        window.addTextEditor("header", "0", "Skip bytes at start:");

        window.getComboBoxComponent("encoding")->setSelectedItemIndex((int) engine::RawEncoding::Signed16,
                                                                      juce::dontSendNotification);
        window.getComboBoxComponent("order")->setSelectedItemIndex(0, juce::dontSendNotification);
    }

    static engine::RawPcmFormat readFormat(juce::AlertWindow& window)
    {
        engine::RawPcmFormat format;
        format.encoding    = (engine::RawEncoding) juce::jlimit(0, engine::kNumRawEncodings - 1,
                                                                window.getComboBoxComponent("encoding")->getSelectedItemIndex());
        format.bigEndian   = window.getComboBoxComponent("order")->getSelectedItemIndex() == 1;
        format.channels    = window.getTextEditorContents("channels").getIntValue();
        format.sampleRate  = window.getTextEditorContents("rate").getDoubleValue();
        format.headerBytes = (std::uint64_t) juce::jmax((juce::int64) 0,
                                                        window.getTextEditorContents("header").getLargeIntValue());
        return format;
    }
};

} // namespace soundsplice::app
