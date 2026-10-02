#include <catch2/catch_test_macros.hpp>

#include <model/Favorites.h>

using namespace soundsplice::model;

TEST_CASE("Favorites round-trip with every setting, and a name saved again replaces it", "[model][favorites]")
{
    auto deEss = makeEffectSlot(EffectKind::DeEsser);
    deEss.enabled = true;
    auto plugin   = makeEffectSlot(EffectKind::Plugin);
    plugin.plugin = { PluginFormat::VST3, "C:/x.vst3", "Shimmer", "c3RhdGU=" };

    std::vector<Favorite> favorites;
    favorites = withFavorite(favorites, { "Podcast voice", { deEss, makeEffectSlot(EffectKind::Compressor) } });
    favorites = withFavorite(favorites, { "Wide", { plugin } });
    REQUIRE(favorites.size() == 2);

    const auto restored = deserializeFavorites(serializeFavorites(favorites));
    REQUIRE(restored == favorites);
    REQUIRE(restored[1].chain[0].plugin.state == "c3RhdGU=");

    favorites = withFavorite(favorites, { "Podcast voice", { deEss } });
    REQUIRE(favorites.size() == 2);
    REQUIRE(favorites[0].chain.size() == 1);

    REQUIRE(deserializeFavorites("").empty());
    REQUIRE(deserializeFavorites("nonsense").empty());
}
