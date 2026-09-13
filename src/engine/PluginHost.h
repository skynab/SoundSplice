#pragma once

#include <memory>
#include <string>
#include <vector>

#include "engine/PluginModule.h"

namespace looper::engine
{
/** One plugin the host knows about, in the plain terms the document and UI
    use — no juce::PluginDescription, for the same reason model::PluginRef
    isn't one: it keeps the boundary in one place. */
struct PluginEntry
{
    std::string format;      // "VST3", "AudioUnit"
    std::string identifier;  // what the format needs to find it again
    std::string name;
    bool        isInstrument = false;
    int         numInputs    = 0;
    int         numOutputs   = 0;
};

/**
    Finding and instantiating plugins. Message thread only — instantiating a
    plugin allocates, loads a binary, and runs third-party initialisation, none
    of which may happen on the audio thread. What reaches the audio thread is
    an already-constructed instance inside a PluginNode, handed over by the
    same chain swap everything else uses.

    **Scanning is in-process, with crash *recovery* rather than crash
    *isolation*.** A dead man's pedal file records which plugin is being probed
    before probing it, so a plugin that brings the app down is skipped on the
    next run instead of killing it again — but it does bring the app down that
    first time. True isolation needs the probe to happen in a child process;
    §20 lists it and it is still outstanding. Saying so is better than implying
    a safety that isn't there.

    Note this deliberately keeps its own list of descriptions rather than using
    juce::KnownPluginList: that class (and PluginDirectoryScanner) live only in
    the GUI hosting module, and the headless bounce tool has to be able to scan
    and instantiate too — that's what makes it possible to verify hosting
    against a real plugin rather than a mock.
*/
class PluginHost
{
public:
    PluginHost()
    {
        // Adds whichever formats were compiled in — see the JUCE_PLUGINHOST_*
        // defines in the CMake files. JUCE 8 deleted the old
        // addDefaultFormats() member in favour of a free function per module;
        // PluginModule.h picks the one matching this target.
        LOOPER_ADD_PLUGIN_FORMATS(formatManager_);
    }

    /** Formats actually available in this build, for the UI to offer. */
    std::vector<std::string> availableFormats() const
    {
        std::vector<std::string> names;
        for (auto* format : formatManager_.getFormats())
            names.push_back(format->getName().toStdString());
        return names;
    }

    /** Probes plugins of @p formatName, adding what it finds to the known
        list.

        @p maxToProbe caps how many candidates are examined in one call, since
        probing instantiates each plugin and a machine with a large collection
        can take minutes. Returns how many were probed, so a caller can resume.
        @p deadMansPedal records the plugin being probed so a crash doesn't
        repeat it. */
    int scanFormat(const std::string& formatName, const juce::File& deadMansPedal, int maxToProbe = 64)
    {
        auto* format = findFormat(formatName);
        if (format == nullptr)
            return 0;

        // Whatever was mid-probe when we last died is assumed to be what killed
        // us, and is skipped.
        const auto lastProbed = deadMansPedal.existsAsFile() ? deadMansPedal.loadFileAsString().trim()
                                                             : juce::String();

        const auto identifiers = format->searchPathsForPlugins(format->getDefaultLocationsToSearch(), true, true);

        int probed = 0;
        for (const auto& identifier : identifiers)
        {
            if (probed >= maxToProbe)
                break;
            if (identifier == lastProbed || isKnown(formatName, identifier.toStdString()))
                continue;

            // Written *before* probing: if the probe never returns, this is the
            // record of which plugin to blame.
            deadMansPedal.replaceWithText(identifier);

            juce::OwnedArray<juce::PluginDescription> found;
            format->findAllTypesForFile(found, identifier);
            for (auto* description : found)
                descriptions_.push_back(*description);

            ++probed;
        }

        deadMansPedal.deleteFile(); // got through the batch alive
        return probed;
    }

    /** Everything scanned so far. */
    std::vector<PluginEntry> knownPlugins() const
    {
        std::vector<PluginEntry> entries;
        for (const auto& description : descriptions_)
            entries.push_back(toEntry(description));
        return entries;
    }

    /** Restores a previously saved scan so startup doesn't re-probe
        everything, and saves it back out. The blob is JUCE's own XML. */
    void restoreScanCache(const std::string& xml)
    {
        auto parsed = juce::XmlDocument::parse(juce::String(xml));
        if (parsed == nullptr)
            return;

        descriptions_.clear();
        for (auto* child : parsed->getChildIterator())
        {
            juce::PluginDescription description;
            if (description.loadFromXml(*child))
                descriptions_.push_back(description);
        }
    }

    std::string saveScanCache() const
    {
        juce::XmlElement root("KNOWNPLUGINS");
        for (const auto& description : descriptions_)
            root.addChildElement(description.createXml().release());
        return root.toString().toStdString();
    }

    /** Instantiates a plugin previously scanned, prepared for the given rate
        and block size. Returns nullptr (with a reason in @p errorOut) if it
        isn't known or won't load — a project referencing a plugin this machine
        doesn't have must degrade, not crash. */
    std::unique_ptr<juce::AudioPluginInstance> createInstance(const std::string& format,
                                                              const std::string& identifier,
                                                              double sampleRate, int blockSize,
                                                              std::string* errorOut = nullptr)
    {
        const auto* description = findDescription(format, identifier);
        if (description == nullptr)
        {
            if (errorOut != nullptr)
                *errorOut = "plugin not found: " + identifier;
            return nullptr;
        }

        juce::String error;
        auto instance = formatManager_.createPluginInstance(*description, sampleRate, blockSize, error);
        if (instance == nullptr && errorOut != nullptr)
            *errorOut = error.toStdString();
        return instance;
    }

private:
    juce::AudioPluginFormat* findFormat(const std::string& name) const
    {
        for (auto* format : formatManager_.getFormats())
            if (format->getName() == juce::String(name))
                return format;
        return nullptr;
    }

    const juce::PluginDescription* findDescription(const std::string& format,
                                                   const std::string& identifier) const
    {
        for (const auto& description : descriptions_)
            if (description.pluginFormatName == juce::String(format)
                && description.fileOrIdentifier == juce::String(identifier))
                return &description;
        return nullptr;
    }

    bool isKnown(const std::string& format, const std::string& identifier) const
    {
        return findDescription(format, identifier) != nullptr;
    }

    static PluginEntry toEntry(const juce::PluginDescription& description)
    {
        PluginEntry entry;
        entry.format       = description.pluginFormatName.toStdString();
        entry.identifier   = description.fileOrIdentifier.toStdString();
        entry.name         = description.name.toStdString();
        entry.isInstrument = description.isInstrument;
        entry.numInputs    = description.numInputChannels;
        entry.numOutputs   = description.numOutputChannels;
        return entry;
    }

    juce::AudioPluginFormatManager        formatManager_;
    std::vector<juce::PluginDescription>  descriptions_;
};

} // namespace looper::engine
