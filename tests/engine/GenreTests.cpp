#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <utility>

#include <engine/Genre.h>

using namespace looper::engine;

namespace
{
    constexpr Genre kAllGenres[] = { Genre::House, Genre::Techno, Genre::HipHop,
                                      Genre::Trap, Genre::Ambient, Genre::LoFi,
                                      Genre::Synthwave };
}

TEST_CASE("Every genre's rhythm profile is in range", "[engine][genre]")
{
    for (auto genre : kAllGenres)
    {
        const auto profile = rhythmProfileForGenre(genre);
        INFO("genre " << genreName(genre));
        REQUIRE(profile.density >= 0.0);
        REQUIRE(profile.density <= 1.0);
        REQUIRE(profile.swing >= 0.0);
        REQUIRE(profile.swing < 0.9);
    }
}

TEST_CASE("Genres don't all share one rhythm profile", "[engine][genre]")
{
    std::set<std::pair<double, double>> profiles;
    for (auto genre : kAllGenres)
    {
        const auto p = rhythmProfileForGenre(genre);
        profiles.insert({ p.density, p.swing });
    }
    REQUIRE(profiles.size() == std::size(kAllGenres));
}

TEST_CASE("Every genre has a distinct, non-empty display name", "[engine][genre]")
{
    std::set<std::string> names;
    for (auto genre : kAllGenres)
    {
        const std::string name = genreName(genre);
        REQUIRE_FALSE(name.empty());
        names.insert(name);
    }
    REQUIRE(names.size() == std::size(kAllGenres));
}
