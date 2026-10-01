#pragma once

#include <algorithm>
#include <array>

namespace soundsplice::engine
{
/**
    How the mixer's tracks feed each other: each track's output goes to the
    master or to a bus (another track that mixes what it's given), and it can
    send to any number of buses besides, at its own level, before or after its
    fader. Buses can feed buses.

    The graph questions the audio thread asks every block - what order to
    render in, who solo leaves audible, how late each path is - answered
    here, JUCE-free and allocation-free, over a snapshot of the routing. The
    message thread refuses routings that would loop (model/Routing.h); these
    still cope with one, sending whatever is caught in it straight to the
    master, because a loop reaching the audio thread must be silence or
    noise-free, never a hang.
*/
namespace mixrouting
{
    inline constexpr int kMaxNodes = 32;
    inline constexpr int kMaxSends = 8;
    inline constexpr int kMaxKeys  = 4; // keyed effect slots per track

    struct Send
    {
        int   bus       = -1;    // a node index
        float gain      = 0.0f;  // linear
        bool  preFader  = false; // tapped after the effects, before fader and pan
    };

    struct Node
    {
        bool                         active  = false;
        bool                         isBus   = false;
        bool                         solo    = false;
        int                          output  = -1; // a bus's node index, or -1 for the master
        std::array<Send, kMaxSends>  sends {};
        int                          sendCount = 0;
        int                          chainLatency = 0; // its effects', in samples

        // The tracks its keyed effects listen to (sidechains): no audio
        // flows, but each has to render first, so its output is there.
        std::array<int, kMaxKeys>    keySources {};
        int                          keyCount = 0;
    };

    /** @p source if it's an active node other than @p from: a key that can
        be listened to. */
    inline int validKey(const Node* nodes, int count, int from, int source)
    {
        return source >= 0 && source < count && source != from && nodes[source].active ? source : -1;
    }

    /** @p target if it's an active bus other than @p from, else -1. */
    inline int validBus(const Node* nodes, int count, int from, int target)
    {
        return target >= 0 && target < count && target != from && nodes[target].active && nodes[target].isBus ? target : -1;
    }

    /**
        The order to render @p count nodes in: every node before each bus it
        feeds, by output or send, so a bus mixes inputs that are already
        there. Inactive nodes are left out. Nodes caught in a loop come last,
        in index order. Returns how many were written to @p order.
    */
    inline int renderOrder(const Node* nodes, int count, int* order)
    {
        count = std::min(count, kMaxNodes);

        // How many active nodes feed each bus, then Kahn's algorithm.
        std::array<int, kMaxNodes> waiting {};
        for (int i = 0; i < count; ++i)
        {
            if (! nodes[i].active)
                continue;
            if (const int out = validBus(nodes, count, i, nodes[i].output); out >= 0)
                ++waiting[(size_t) out];
            for (int s = 0; s < nodes[i].sendCount; ++s)
                if (const int bus = validBus(nodes, count, i, nodes[i].sends[(size_t) s].bus); bus >= 0)
                    ++waiting[(size_t) bus];
            for (int k = 0; k < nodes[i].keyCount; ++k)
                if (validKey(nodes, count, i, nodes[i].keySources[(size_t) k]) >= 0)
                    ++waiting[(size_t) i]; // waits for each of its keys
        }

        std::array<bool, kMaxNodes> placed {};
        int written = 0;
        bool progress = true;
        while (progress)
        {
            progress = false;
            for (int i = 0; i < count; ++i)
            {
                if (! nodes[i].active || placed[(size_t) i] || waiting[(size_t) i] > 0)
                    continue;

                placed[(size_t) i]  = true;
                order[written++]    = i;
                progress            = true;

                if (const int out = validBus(nodes, count, i, nodes[i].output); out >= 0)
                    --waiting[(size_t) out];
                for (int s = 0; s < nodes[i].sendCount; ++s)
                    if (const int bus = validBus(nodes, count, i, nodes[i].sends[(size_t) s].bus); bus >= 0)
                        --waiting[(size_t) bus];

                // Whoever listens to this one has one key fewer to wait for.
                for (int j = 0; j < count; ++j)
                    if (nodes[j].active)
                        for (int k = 0; k < nodes[j].keyCount; ++k)
                            if (validKey(nodes, count, j, nodes[j].keySources[(size_t) k]) == i)
                                --waiting[(size_t) j];
            }
        }

        for (int i = 0; i < count; ++i)
            if (nodes[i].active && ! placed[(size_t) i])
                order[written++] = i; // in a loop
        return written;
    }

    /** True if node @p i's audio comes back round to it: it's in a loop. */
    inline bool inLoop(const Node* nodes, int count, int i)
    {
        std::array<bool, kMaxNodes> seen {};
        std::array<int, kMaxNodes>  stack {};
        int top = 0;
        stack[(size_t) top++] = i;
        while (top > 0)
        {
            const int at = stack[(size_t) --top];
            const auto push = [&](int next)
            {
                if (next == i)
                    return true;
                if (next >= 0 && ! seen[(size_t) next])
                {
                    seen[(size_t) next] = true;
                    stack[(size_t) top++] = next;
                }
                return false;
            };
            if (push(validBus(nodes, count, at, nodes[at].output)))
                return true;
            for (int s = 0; s < nodes[at].sendCount; ++s)
                if (push(validBus(nodes, count, at, nodes[at].sends[(size_t) s].bus)))
                    return true;
        }
        return false;
    }

    /** Where node @p i's output goes: its bus, or -1 for the master - also
        when that bus would close a loop. */
    inline int outputOf(const Node* nodes, int count, int i)
    {
        const int out = validBus(nodes, count, i, nodes[i].output);
        return out >= 0 && ! inLoop(nodes, count, i) ? out : -1;
    }

    /**
        Who solo leaves audible, into @p audible: everything when nothing is
        soloed. Otherwise a soloed node, everything that feeds it (soloing a
        bus is hearing what's in it), and everything it feeds (a soloed
        track is heard through its bus and its reverb send, as REAPER's solo
        in place does).
    */
    inline void soloAudible(const Node* nodes, int count, bool* audible)
    {
        count = std::min(count, kMaxNodes);

        bool anySolo = false;
        for (int i = 0; i < count; ++i)
            anySolo = anySolo || (nodes[i].active && nodes[i].solo);

        for (int i = 0; i < count; ++i)
            audible[i] = ! anySolo || (nodes[i].active && nodes[i].solo);
        if (! anySolo)
            return;

        // Downstream: a soloed node's buses, theirs, and so on.
        bool grew = true;
        while (grew)
        {
            grew = false;
            for (int i = 0; i < count; ++i)
            {
                if (! nodes[i].active || ! audible[i])
                    continue;
                const auto open = [&](int bus)
                {
                    if (bus >= 0 && ! audible[bus])
                        audible[bus] = grew = true;
                };
                open(validBus(nodes, count, i, nodes[i].output));
                for (int s = 0; s < nodes[i].sendCount; ++s)
                    open(validBus(nodes, count, i, nodes[i].sends[(size_t) s].bus));
            }
        }

        // Upstream: whatever feeds a soloed node, by output or send.
        std::array<bool, kMaxNodes> soloedOrFed {};
        for (int i = 0; i < count; ++i)
            soloedOrFed[(size_t) i] = nodes[i].active && nodes[i].solo;
        grew = true;
        while (grew)
        {
            grew = false;
            for (int i = 0; i < count; ++i)
            {
                if (! nodes[i].active || soloedOrFed[(size_t) i])
                    continue;
                bool feeds = false;
                if (const int out = validBus(nodes, count, i, nodes[i].output); out >= 0 && soloedOrFed[(size_t) out])
                    feeds = true;
                for (int s = 0; s < nodes[i].sendCount && ! feeds; ++s)
                    if (const int bus = validBus(nodes, count, i, nodes[i].sends[(size_t) s].bus);
                        bus >= 0 && soloedOrFed[(size_t) bus])
                        feeds = true;
                if (feeds)
                    soloedOrFed[(size_t) i] = audible[i] = grew = true;
            }
        }
    }

    /** How late node @p i's audio is by the time it reaches the master: its
        own effects' latency and every bus's on its way out. */
    inline int pathLatency(const Node* nodes, int count, int i)
    {
        int total = 0;
        for (int hops = 0; i >= 0 && hops <= kMaxNodes; ++hops)
        {
            total += nodes[i].chainLatency;
            i = outputOf(nodes, count, i);
        }
        return total;
    }

    /** The latest any source's path makes it: what every track is delayed to
        meet. Buses aren't sources - what they carry is counted through the
        tracks feeding them. */
    inline int latestPath(const Node* nodes, int count)
    {
        int latest = 0;
        for (int i = 0; i < std::min(count, kMaxNodes); ++i)
            if (nodes[i].active && ! nodes[i].isBus)
                latest = std::max(latest, pathLatency(nodes, count, i));
        return latest;
    }

    /** How much node @p i is delayed so it lands with everything else:
        a source, by what its path is short of @p latest; a bus, not at all,
        since what reaches it is already aligned. */
    inline int compensationFor(const Node* nodes, int count, int i, int latest)
    {
        return nodes[i].isBus ? 0 : std::max(0, latest - pathLatency(nodes, count, i));
    }
}

} // namespace soundsplice::engine
