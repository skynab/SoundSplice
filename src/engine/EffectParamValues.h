#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/EffectKind.h"

namespace soundsplice::engine
{
/**
    One chain slot's settings, addressed by parameter id: what a built-in
    node reads its setters from.

    The ids are the ones model::EffectDescriptor gives each parameter, and
    model::effectParamValues fills this from a slot through those
    descriptors. Addressing by name rather than through a struct with a field
    for every effect is what keeps a new effect from touching the engine's
    plumbing, and it is how automation will reach a single parameter.

    JUCE-free, so the model can build one and the headless tests can check
    that every node reads exactly the parameters its descriptor names: each
    read is recorded, and a read of an id that isn't here is too.
*/
class EffectParamValues
{
public:
    EffectKind kind    = EffectKind::Filter; // whose parameters these are
    bool       enabled = false;

    void set(std::string id, double value)
    {
        for (auto& entry : numbers_)
            if (entry.id == id)
            {
                entry.value = value;
                return;
            }
        numbers_.push_back({ std::move(id), value });
    }

    void setText(std::string id, std::string value)
    {
        for (auto& entry : texts_)
            if (entry.id == id)
            {
                entry.value = std::move(value);
                return;
            }
        texts_.push_back({ std::move(id), std::move(value) });
    }

    /** The value of @p id, or 0 if there is no such parameter. */
    double get(std::string_view id) const
    {
        for (const auto& entry : numbers_)
            if (entry.id == id)
            {
                entry.read = true;
                return entry.value;
            }
        missing_.emplace_back(id);
        return 0.0;
    }

    /** The value of @p id, or nothing if there is no such parameter. */
    std::optional<double> find(std::string_view id) const
    {
        for (const auto& entry : numbers_)
            if (entry.id == id)
            {
                entry.read = true;
                return entry.value;
            }
        missing_.emplace_back(id);
        return std::nullopt;
    }

    float getFloat(std::string_view id) const { return (float) get(id); }
    int   getInt(std::string_view id) const   { return (int) std::lround(get(id)); }
    bool  getBool(std::string_view id) const  { return get(id) >= 0.5; }

    /** A text parameter (a file path), or empty if there is none. */
    const std::string& text(std::string_view id) const
    {
        for (const auto& entry : texts_)
            if (entry.id == id)
            {
                entry.read = true;
                return entry.value;
            }
        missing_.emplace_back(id);
        static const std::string none;
        return none;
    }

    /** Ids present here that nothing has read: a parameter the engine ignores. */
    std::vector<std::string> unreadIds() const
    {
        std::vector<std::string> ids;
        for (const auto& entry : numbers_)
            if (! entry.read)
                ids.push_back(entry.id);
        for (const auto& entry : texts_)
            if (! entry.read)
                ids.push_back(entry.id);
        return ids;
    }

    /** Ids read that aren't here: a node asking for a parameter by the wrong name. */
    const std::vector<std::string>& missingIds() const { return missing_; }

private:
    template <typename T>
    struct Entry
    {
        std::string  id;
        T            value {};
        mutable bool read = false;
    };

    std::vector<Entry<double>>       numbers_;
    std::vector<Entry<std::string>>  texts_;
    mutable std::vector<std::string> missing_;
};

} // namespace soundsplice::engine
