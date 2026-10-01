#include <catch2/catch_test_macros.hpp>

#include <engine/MixRouting.h>

#include <algorithm>
#include <vector>

using namespace soundsplice::engine::mixrouting;

namespace
{
    /** Drums (0) and bass (1) into a band bus (3), vocal (2) to the master
        with a send to a reverb bus (4); the band bus also sends to the
        reverb, and the reverb returns to the master. */
    std::vector<Node> band()
    {
        std::vector<Node> nodes(5);
        for (auto& node : nodes)
            node.active = true;
        nodes[3].isBus = nodes[4].isBus = true;
        nodes[0].output = nodes[1].output = 3;
        nodes[2].sends[0] = { 4, 0.5f, false };
        nodes[2].sendCount = 1;
        nodes[3].sends[0] = { 4, 0.25f, true };
        nodes[3].sendCount = 1;
        return nodes;
    }

    int placeOf(const std::vector<int>& order, int node)
    {
        return (int) (std::find(order.begin(), order.end(), node) - order.begin());
    }
}

TEST_CASE("Every track renders before the buses it feeds", "[engine][routing]")
{
    auto nodes = band();
    nodes[1].active = false; // left out altogether

    std::vector<int> order(kMaxNodes);
    order.resize((size_t) renderOrder(nodes.data(), (int) nodes.size(), order.data()));

    REQUIRE(order.size() == 4);
    REQUIRE(placeOf(order, 1) == 4);
    REQUIRE(placeOf(order, 0) < placeOf(order, 3));
    REQUIRE(placeOf(order, 3) < placeOf(order, 4)); // a bus feeding a bus
    REQUIRE(placeOf(order, 2) < placeOf(order, 4));
}

TEST_CASE("A loop is rendered rather than hung on, and goes to the master", "[engine][routing]")
{
    std::vector<Node> nodes(3);
    for (auto& node : nodes)
        node.active = node.isBus = true;
    nodes[0].output = 1;
    nodes[1].output = 0; // 0 and 1 feed each other
    nodes[2].output = 0;

    std::vector<int> order(kMaxNodes);
    REQUIRE(renderOrder(nodes.data(), 3, order.data()) == 3);
    REQUIRE(inLoop(nodes.data(), 3, 0));
    REQUIRE_FALSE(inLoop(nodes.data(), 3, 2));
    REQUIRE(outputOf(nodes.data(), 3, 0) == -1);
    REQUIRE(outputOf(nodes.data(), 3, 2) == 0);

    // Nor can a track feed itself.
    nodes[2].output = 2;
    REQUIRE(outputOf(nodes.data(), 3, 2) == -1);
}

TEST_CASE("Solo keeps what a soloed track feeds and what feeds a soloed bus", "[engine][routing]")
{
    auto nodes = band();
    bool audible[5] {};

    soloAudible(nodes.data(), 5, audible);
    REQUIRE(std::all_of(audible, audible + 5, [](bool a) { return a; }));

    // The vocal: heard through its reverb send; the band isn't.
    nodes[2].solo = true;
    soloAudible(nodes.data(), 5, audible);
    REQUIRE(audible[2]);
    REQUIRE(audible[4]);
    REQUIRE_FALSE(audible[0]);
    REQUIRE_FALSE(audible[3]);

    // The band bus: its drums and bass, and the reverb it sends to.
    nodes[2].solo = false;
    nodes[3].solo = true;
    soloAudible(nodes.data(), 5, audible);
    REQUIRE(audible[0]);
    REQUIRE(audible[1]);
    REQUIRE(audible[3]);
    REQUIRE(audible[4]);
    REQUIRE_FALSE(audible[2]);
}

TEST_CASE("Delay compensation counts the buses on each track's way out", "[engine][routing]")
{
    auto nodes = band();
    nodes[0].chainLatency = 100; // drums' own effects
    nodes[3].chainLatency = 50;  // the band bus's

    REQUIRE(pathLatency(nodes.data(), 5, 0) == 150);
    REQUIRE(pathLatency(nodes.data(), 5, 1) == 50);
    REQUIRE(pathLatency(nodes.data(), 5, 2) == 0);

    const int latest = latestPath(nodes.data(), 5);
    REQUIRE(latest == 150);
    REQUIRE(compensationFor(nodes.data(), 5, 0, latest) == 0);
    REQUIRE(compensationFor(nodes.data(), 5, 1, latest) == 100); // meets the drums at the bus
    REQUIRE(compensationFor(nodes.data(), 5, 2, latest) == 150);
    REQUIRE(compensationFor(nodes.data(), 5, 3, latest) == 0);   // a bus's inputs are already aligned
}
