#include <catch2/catch_test_macros.hpp>

#include <app/Screensets.h>

using namespace soundsplice;

TEST_CASE("Screensets are kept by name and survive saving", "[gui][screensets]")
{
    std::vector<screensets::Screenset> list;
    list = screensets::with(list, { "Mixing", "row(0.5, leaf(\"Mixer\"), leaf(\"Tracks\"))" });
    list = screensets::with(list, { "Editing \"tight\" & <small>", "leaf(\"Audio\")" });
    REQUIRE(list.size() == 2);

    // The same name replaces rather than adds.
    list = screensets::with(list, { "Mixing", "leaf(\"Mixer\")" });
    REQUIRE(list.size() == 2);
    REQUIRE(list[0].layout == "leaf(\"Mixer\")");

    // Quotes and markup in names and layouts come back as they went in.
    REQUIRE(screensets::deserialize(screensets::serialize(list)) == list);

    REQUIRE(screensets::deserialize("").empty());
    REQUIRE(screensets::deserialize("<OTHER/>").empty());
    REQUIRE(screensets::deserialize("<SCREENSETS><SCREENSET name=\"\" layout=\"x\"/></SCREENSETS>").empty());
}
