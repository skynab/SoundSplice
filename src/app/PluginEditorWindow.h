#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/PluginNode.h"

namespace looper
{
/**
    A window hosting one plugin's own editor.

    The plugin supplies the component; this only frames it. Two things it has
    to get right:

    - **The editor must be destroyed before the plugin is.** A chain rebuild
      deletes the PluginNode, so the owner closes any window pointing at it
      first (see MainComponent::closePluginEditors) — otherwise the editor
      outlives the processor it draws.
    - **A plugin with no editor of its own** gets JUCE's generic parameter
      view rather than an empty window, which is the difference between "this
      plugin has no UI" and "this app failed to open it".
*/
class PluginEditorWindow final : public juce::DocumentWindow
{
public:
    std::function<void(PluginEditorWindow*)> onCloseRequested;

    PluginEditorWindow(const juce::String& title, juce::AudioPluginInstance& plugin)
        : juce::DocumentWindow(title, juce::Colours::black, juce::DocumentWindow::closeButton),
          plugin_(plugin)
    {
        setUsingNativeTitleBar(true);

        auto* editor = plugin.hasEditor() ? plugin.createEditorAndMakeActive()
                                          : new juce::GenericAudioProcessorEditor(plugin);
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor(plugin);

        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    ~PluginEditorWindow() override { clearContentComponent(); }

    void closeButtonPressed() override
    {
        if (onCloseRequested)
            onCloseRequested(this);
    }

    const juce::AudioPluginInstance* plugin() const noexcept { return &plugin_; }

private:
    juce::AudioPluginInstance& plugin_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditorWindow)
};

} // namespace looper
