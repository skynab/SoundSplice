#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "engine/PluginModule.h"

namespace soundsplice::engine
{
/** One plugin the host knows about, in the plain terms the document and UI
    use — no juce::PluginDescription, for the same reason model::PluginRef
    isn't one: it keeps the boundary in one place. */
struct PluginEntry
{
    std::string format;      // "VST3", "AudioUnit", "LV2"
    std::string identifier;  // what the format needs to find it again
    std::string name;
    bool        isInstrument = false;
    int         numInputs    = 0;
    int         numOutputs   = 0;
    bool        disabled     = false; // turned off in the plugin manager: not offered
};

/**
    Finding and instantiating plugins. Message thread only — instantiating a
    plugin allocates, loads a binary, and runs third-party initialisation, none
    of which may happen on the audio thread. What reaches the audio thread is
    an already-constructed instance inside a PluginNode, handed over by the
    same chain swap everything else uses.

    **Scanning can be crash-safe.** Given a prober (setProber), each plugin is
    probed by it - the app runs a copy of itself to do it (see Main.cpp's
    --probe-plugin), so a plugin that crashes or hangs while being probed
    takes down only that copy, and goes on the blocklist. Without one, as in
    the headless bounce tool, probing is in-process with crash *recovery*: a
    dead man's pedal file records which plugin is being probed, and one that
    brought the process down is blocklisted on the next scan.

    The blocklist and the plugins turned off in the plugin manager are kept
    with the scan cache, so neither is probed or offered again until the
    user says so.

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
        SOUNDSPLICE_ADD_PLUGIN_FORMATS(formatManager_);
    }

    /** Probes one plugin file or identifier somewhere safe, filling
        @p found; false if it crashed, hung or wouldn't load, which
        blocklists it. */
    using Prober = std::function<bool(const std::string& format, const std::string& identifier,
                                      std::vector<juce::PluginDescription>& found)>;

    void setProber(Prober prober) { prober_ = std::move(prober); }

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
        // Whatever was mid-probe when we last died is assumed to be what killed
        // us, and goes on the blocklist.
        if (deadMansPedal.existsAsFile())
        {
            const auto lastProbed = deadMansPedal.loadFileAsString().trim();
            if (lastProbed.isNotEmpty())
                blocked_.insert(keyFor(formatName, lastProbed.toStdString()));
            deadMansPedal.deleteFile();
        }

        auto* format = findFormat(formatName);
        if (format == nullptr)
            return 0;

        const auto identifiers = format->searchPathsForPlugins(format->getDefaultLocationsToSearch(), true, true);

        int probed = 0;
        for (const auto& identifier : identifiers)
        {
            if (probed >= maxToProbe)
                break;
            const auto id = identifier.toStdString();
            if (isBlocked(formatName, id) || isKnown(formatName, id))
                continue;

            // Written *before* probing: if the probe never returns, this is the
            // record of which plugin to blame.
            deadMansPedal.replaceWithText(identifier);

            std::vector<juce::PluginDescription> found;
            if (prober_ ? prober_(formatName, id, found) : probeInProcess(formatName, id, found))
                descriptions_.insert(descriptions_.end(), found.begin(), found.end());
            else
                blocked_.insert(keyFor(formatName, id));

            ++probed;
        }

        deadMansPedal.deleteFile(); // got through the batch alive
        return probed;
    }

    /** Probes one plugin here, in this process: what a prober running in a
        child process does, and the fallback without one. */
    bool probeInProcess(const std::string& formatName, const std::string& identifier,
                        std::vector<juce::PluginDescription>& found)
    {
        auto* format = findFormat(formatName);
        if (format == nullptr)
            return false;

        juce::OwnedArray<juce::PluginDescription> descriptions;
        format->findAllTypesForFile(descriptions, juce::String(identifier));
        for (auto* description : descriptions)
            found.push_back(*description);
        return ! found.empty();
    }

    /** Descriptions as XML, and back: how a probe in a child process reports
        what it found. */
    static std::string descriptionsToXml(const std::vector<juce::PluginDescription>& descriptions)
    {
        juce::XmlElement root("FOUND");
        for (const auto& description : descriptions)
            root.addChildElement(description.createXml().release());
        return root.toString().toStdString();
    }

    static std::vector<juce::PluginDescription> descriptionsFromXml(const std::string& xml)
    {
        std::vector<juce::PluginDescription> descriptions;
        if (auto parsed = juce::XmlDocument::parse(juce::String(xml)))
            for (auto* child : parsed->getChildIterator())
            {
                juce::PluginDescription description;
                if (description.loadFromXml(*child))
                    descriptions.push_back(description);
            }
        return descriptions;
    }

    /** Everything scanned so far, the turned-off ones marked. */
    std::vector<PluginEntry> knownPlugins() const
    {
        std::vector<PluginEntry> entries;
        for (const auto& description : descriptions_)
            entries.push_back(toEntry(description));
        return entries;
    }

    /** What to offer for adding: everything scanned and not turned off. */
    std::vector<PluginEntry> offeredPlugins() const
    {
        auto entries = knownPlugins();
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const PluginEntry& e) { return e.disabled; }),
                      entries.end());
        return entries;
    }

    /** Turns a plugin off (not offered for adding; a project that already
        uses it still loads it) or back on. */
    void setDisabled(const std::string& format, const std::string& identifier, bool disabled)
    {
        if (disabled)
            disabled_.insert(keyFor(format, identifier));
        else
            disabled_.erase(keyFor(format, identifier));
    }

    bool isDisabled(const std::string& format, const std::string& identifier) const
    {
        return disabled_.count(keyFor(format, identifier)) > 0;
    }

    /** The blocklist: plugins that crashed, hung or wouldn't load while
        being probed, as (format, identifier). Never probed again until
        unblocked. */
    std::vector<std::pair<std::string, std::string>> blockedPlugins() const
    {
        std::vector<std::pair<std::string, std::string>> entries;
        for (const auto& key : blocked_)
            entries.push_back(splitKey(key));
        return entries;
    }

    bool isBlocked(const std::string& format, const std::string& identifier) const
    {
        return blocked_.count(keyFor(format, identifier)) > 0;
    }

    /** Takes a plugin off the blocklist, so the next scan probes it again. */
    void unblock(const std::string& format, const std::string& identifier) { blocked_.erase(keyFor(format, identifier)); }

    /** Forgets what was found for a plugin, so the next scan probes it again. */
    void forget(const std::string& format, const std::string& identifier)
    {
        descriptions_.erase(std::remove_if(descriptions_.begin(), descriptions_.end(),
                                           [&](const juce::PluginDescription& d)
                                           {
                                               return d.pluginFormatName == juce::String(format)
                                                   && d.fileOrIdentifier == juce::String(identifier);
                                           }),
                            descriptions_.end());
    }

    /** Restores a previously saved scan so startup doesn't re-probe
        everything, and saves it back out. The blob is JUCE's own XML. */
    void restoreScanCache(const std::string& xml)
    {
        auto parsed = juce::XmlDocument::parse(juce::String(xml));
        if (parsed == nullptr)
            return;

        descriptions_.clear();
        blocked_.clear();
        disabled_.clear();
        for (auto* child : parsed->getChildIterator())
        {
            // The plugin manager's lists ride along with the scan.
            if (child->hasTagName("BLOCKED"))
            {
                blocked_.insert(child->getStringAttribute("key").toStdString());
                continue;
            }
            if (child->hasTagName("DISABLED"))
            {
                disabled_.insert(child->getStringAttribute("key").toStdString());
                continue;
            }

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
        for (const auto& key : blocked_)
            root.createNewChildElement("BLOCKED")->setAttribute("key", juce::String(key));
        for (const auto& key : disabled_)
            root.createNewChildElement("DISABLED")->setAttribute("key", juce::String(key));
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

    /** One string per plugin for the lists, format first: a format name
        never holds the separator, an identifier (a path) may. */
    static std::string keyFor(const std::string& format, const std::string& identifier)
    {
        return format + "|" + identifier;
    }

    static std::pair<std::string, std::string> splitKey(const std::string& key)
    {
        const auto bar = key.find('|');
        return bar == std::string::npos ? std::pair<std::string, std::string> { {}, key }
                                        : std::pair<std::string, std::string> { key.substr(0, bar), key.substr(bar + 1) };
    }

    PluginEntry toEntry(const juce::PluginDescription& description) const
    {
        PluginEntry entry;
        entry.format       = description.pluginFormatName.toStdString();
        entry.identifier   = description.fileOrIdentifier.toStdString();
        entry.name         = description.name.toStdString();
        entry.isInstrument = description.isInstrument;
        entry.numInputs    = description.numInputChannels;
        entry.numOutputs   = description.numOutputChannels;
        entry.disabled     = isDisabled(entry.format, entry.identifier);
        return entry;
    }

    juce::AudioPluginFormatManager        formatManager_;
    std::vector<juce::PluginDescription>  descriptions_;
    std::set<std::string>                 blocked_;  // keyFor(format, identifier)
    std::set<std::string>                 disabled_;
    Prober                                prober_;
};

} // namespace soundsplice::engine
