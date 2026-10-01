#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/PluginManagerDialog.h>
#include <engine/PluginHost.h>

using namespace soundsplice;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };

    /** A scan cache holding two made-up plugins, as a scan would save it. */
    std::string cacheWithTwoPlugins()
    {
        std::vector<juce::PluginDescription> found;
        for (const char* name : { "Squash", "Shimmer" })
        {
            juce::PluginDescription description;
            description.name             = name;
            description.pluginFormatName = "VST3";
            description.fileOrIdentifier = juce::String("C:/Plugins/") + name + ".vst3";
            found.push_back(description);
        }

        return engine::PluginHost::descriptionsToXml(found); // a cache reads any root's children
    }
}

TEST_CASE("Turned-off plugins aren't offered, and the plugin manager's lists persist with the scan",
          "[gui][plugins]")
{
    JuceFixture fixture;

    engine::PluginHost host;
    host.restoreScanCache(cacheWithTwoPlugins());
    REQUIRE(host.knownPlugins().size() == 2);
    REQUIRE(host.offeredPlugins().size() == 2);

    host.setDisabled("VST3", "C:/Plugins/Squash.vst3", true);
    REQUIRE(host.offeredPlugins().size() == 1);
    REQUIRE(host.offeredPlugins()[0].name == "Shimmer");
    REQUIRE(host.knownPlugins()[0].disabled);

    engine::PluginHost reloaded;
    reloaded.restoreScanCache(host.saveScanCache());
    REQUIRE(reloaded.knownPlugins().size() == 2);
    REQUIRE(reloaded.offeredPlugins().size() == 1);
    REQUIRE(reloaded.isDisabled("VST3", "C:/Plugins/Squash.vst3"));

    reloaded.forget("VST3", "C:/Plugins/Shimmer.vst3");
    REQUIRE(reloaded.knownPlugins().size() == 1);
}

TEST_CASE("A plugin the prober can't load is blocklisted, kept across a reload, and can be unblocked",
          "[gui][plugins]")
{
    JuceFixture fixture;

    engine::PluginHost host;

    // A dead man's pedal naming a plugin: it was being probed when a scan died.
    juce::TemporaryFile pedal(".tmp");
    pedal.getFile().replaceWithText("C:/Plugins/Crasher.vst3");

    host.setProber([](const std::string&, const std::string&, std::vector<juce::PluginDescription>&) { return false; });
    host.scanFormat("VST3", pedal.getFile(), 0); // probe nothing new; just read the pedal

    REQUIRE(host.isBlocked("VST3", "C:/Plugins/Crasher.vst3"));
    REQUIRE_FALSE(pedal.getFile().existsAsFile());

    engine::PluginHost reloaded;
    reloaded.restoreScanCache(host.saveScanCache());
    REQUIRE(reloaded.blockedPlugins().size() == 1);
    REQUIRE(reloaded.blockedPlugins()[0].second == "C:/Plugins/Crasher.vst3");

    reloaded.unblock("VST3", "C:/Plugins/Crasher.vst3");
    REQUIRE(reloaded.blockedPlugins().empty());
}

TEST_CASE("The plugin manager's buttons act on the selected plugin", "[gui][plugins]")
{
    JuceFixture fixture;

    PluginManagerDialog dialog;
    dialog.setRows({ { "VST3", "a.vst3", "Squash", PluginManagerDialog::Row::State::On },
                     { "LV2", "urn:x", "Crasher", PluginManagerDialog::Row::State::Blocked } });

    std::string toggled, unblocked;
    bool        turnedOn = true;
    dialog.onSetEnabled  = [&](const PluginManagerDialog::Row& row, bool on) { toggled = row.name; turnedOn = on; };
    dialog.onUnblock     = [&](const PluginManagerDialog::Row& row) { unblocked = row.name; };

    // Nothing selected: nothing to act on.
    REQUIRE_FALSE(dialog.toggleButtonForTesting().isEnabled());

    dialog.selectRowForTesting(0);
    REQUIRE(dialog.toggleButtonForTesting().isEnabled());
    REQUIRE_FALSE(dialog.unblockButtonForTesting().isEnabled());
    dialog.toggleButtonForTesting().triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    REQUIRE(toggled == "Squash");
    REQUIRE_FALSE(turnedOn);

    dialog.selectRowForTesting(1);
    REQUIRE_FALSE(dialog.toggleButtonForTesting().isEnabled());
    dialog.unblockButtonForTesting().triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    REQUIRE(unblocked == "Crasher");
}
