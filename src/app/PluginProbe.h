#pragma once

#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "engine/PluginHost.h"

namespace soundsplice
{
/**
    Crash-safe plugin scanning: each plugin is probed by a copy of the app
    run with --probe-plugin, so a plugin that crashes or hangs while being
    loaded takes down only that copy. The parent waits for it, reads what it
    found from a file, and blocklists the plugin if it died, timed out or
    found nothing (see engine::PluginHost::setProber).
*/
inline constexpr const char* kProbePluginFlag = "--probe-plugin";

/** How long one plugin gets to load before it's taken to have hung. */
inline constexpr int kProbeTimeoutMs = 60000;

/** The child's side: given "--probe-plugin <format> <identifier> <out
    file>", probes the plugin and writes what it found to the file as XML.
    Returns the process's exit code: 0 if it found something. */
inline int runPluginProbe(const juce::StringArray& args)
{
    const int at = args.indexOf(kProbePluginFlag);
    if (at < 0 || at + 3 >= args.size())
        return 2;

    engine::PluginHost                   host;
    std::vector<juce::PluginDescription> found;
    if (! host.probeInProcess(args[at + 1].unquoted().toStdString(), args[at + 2].unquoted().toStdString(), found))
        return 1;

    const juce::File out(args[at + 3].unquoted());
    return out.replaceWithText(juce::String(engine::PluginHost::descriptionsToXml(found))) ? 0 : 3;
}

/** The parent's side, as an engine::PluginHost::Prober. If this executable
    can't be run as a child at all, probes in-process instead rather than
    blocklisting everything. */
inline bool probePluginInChildProcess(const std::string& format, const std::string& identifier,
                                      std::vector<juce::PluginDescription>& found)
{
    const juce::TemporaryFile out(".xml");
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);

    juce::ChildProcess child;
    if (! child.start(juce::StringArray { exe.getFullPathName(), kProbePluginFlag, juce::String(format),
                                          juce::String(identifier), out.getFile().getFullPathName() },
                      0))
    {
        engine::PluginHost host;
        return host.probeInProcess(format, identifier, found);
    }

    if (! child.waitForProcessToFinish(kProbeTimeoutMs))
    {
        child.kill();
        return false; // hung
    }
    if (child.getExitCode() != 0)
        return false; // crashed, or nothing in it loaded

    found = engine::PluginHost::descriptionsFromXml(out.getFile().loadFileAsString().toStdString());
    return ! found.empty();
}

} // namespace soundsplice
