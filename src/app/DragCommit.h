#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "model/History.h"

namespace soundsplice
{
/**
    Turns a whole drag into one undo step.

    A drag reports its live values through History::mutableCurrent() as it
    happens, so the audio and the UI follow the pointer in real time without
    pushing an undo step per pixel. Committing the result is then two moves,
    not one: the document is rewound to the value it held before the drag
    started, and only then is the final value applied through a single
    History::edit() call. That is what makes undo land back at the start of
    the drag rather than one step before the end of it.

    The rewind is the part that is easy to leave out and impossible to
    notice — without it the edit still commits, the stack still gets a step,
    and undo simply returns to wherever the fader happened to be partway
    through the drag rather than to where it started. That is why this is a
    function with tests rather than four lines inside a component no test can
    construct.

    Returns false, and leaves history untouched, when the drag ended where it
    started — a grab-and-release shouldn't cost an undo step.
*/
template <typename State, typename Value, typename Write>
bool commitDrag(model::History<State>& history, std::string label,
                 Value from, Value to, Write write, Value tolerance = Value(1.0e-6))
{
    if (! (std::abs(to - from) > tolerance))
        return false;

    write(history.mutableCurrent(), from);
    history.edit(std::move(label), [&write, to](State& state) { write(state, to); });
    return true;
}

/**
    Same technique as commitDrag, for a dragged value with no meaningful
    tolerance to compare within — a whole struct (e.g. one effect slot's
    settings, changed as a unit the way EffectChainPanel already treats a
    slot's parameters), rather than a single continuous number. Equality
    replaces the tolerance check: any change commits, however small.
*/
template <typename State, typename Value, typename Write>
bool commitStructDrag(model::History<State>& history, std::string label,
                      const Value& from, const Value& to, Write write)
{
    if (to == from)
        return false;

    write(history.mutableCurrent(), from);
    history.edit(std::move(label), [&write, to](State& state) { write(state, to); });
    return true;
}

/**
    The other half of a drag: where it started, kept from the control's
    drag-start callback to its drag-end one, for commitDrag to rewind to.

    @p Key says which control is being dragged - a track and fader, a slot -
    so an end that arrives for some other control (the selection moved
    mid-drag, say) is ignored rather than committing the wrong thing.
    std::monostate is the key for a drag there's only ever one of.
*/
template <typename Key, typename Value>
class PendingDrag
{
public:
    void begin(Key key, Value from) { drag_.emplace(Drag { std::move(key), std::move(from) }); }

    /** True while any drag is in progress. */
    bool isActive() const noexcept { return drag_.has_value(); }

    /** Where the drag of @p key started, while it's in progress; nullptr
        when that control isn't the one being dragged. */
    const Value* startOf(const Key& key) const { return drag_ && drag_->key == key ? &drag_->from : nullptr; }

    /** Ends the drag of @p key and hands back where it started. Nothing, and
        the drag left as it was, when @p key isn't the control being dragged. */
    std::optional<Value> end(const Key& key)
    {
        if (startOf(key) == nullptr)
            return std::nullopt;
        auto from = std::move(drag_->from);
        drag_.reset();
        return from;
    }

private:
    struct Drag
    {
        Key   key;
        Value from;
    };
    std::optional<Drag> drag_;
};

} // namespace soundsplice
