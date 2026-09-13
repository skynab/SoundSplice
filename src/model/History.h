#pragma once

#include <string>
#include <utility>
#include <vector>

namespace looper::model
{
/**
    Snapshot-based undo/redo over a copyable value state.

    Each edit records the previous state with a label; undo/redo move between
    snapshots. Simple and correct; a delta-based command history (smaller
    snapshots, the "an AI edit is just a command" seam) is a future refinement.
*/
template <typename State>
class History
{
public:
    History() = default;
    explicit History(State initial) : present_(std::move(initial)) {}

    const State& current() const noexcept { return present_; }

    /** Non-snapshotting access for live parameter tweaks (e.g. faders) that
        should not each create their own undo step. Still counts as a change:
        the id moves, so a fader nudge marks the document unsaved even though
        it adds no undo step. */
    State& mutableCurrent() noexcept
    {
        presentId_ = ++lastId_;
        return present_;
    }

    /** Identity of the state currently held — distinct for every distinct
        edit, and *restored* by undo/redo rather than advanced.

        Lets a caller ask "is this exactly what I last saved?" without keeping
        a copy of the document to compare against. Undo depth can't answer it
        (undoing an edit and making a different one leaves the depth
        unchanged), and undoing back to the saved state has to read as clean
        again, which a plain modified-flag gets wrong. */
    unsigned long long stateId() const noexcept { return presentId_; }

    /** Replace the whole state, recording the previous one for undo. */
    void reset(State initial)
    {
        present_   = std::move(initial);
        presentId_ = ++lastId_;
        undo_.clear();
        redo_.clear();
    }

    /** Commit a new state as an undoable edit. */
    void apply(State next, std::string label = {})
    {
        undo_.push_back({ std::move(label), present_, presentId_ });
        present_   = std::move(next);
        presentId_ = ++lastId_;
        redo_.clear();
    }

    /** Mutate a copy of the current state and commit it. */
    template <typename EditFn>
    void edit(std::string label, EditFn&& fn)
    {
        State next = present_;
        fn(next);
        apply(std::move(next), std::move(label));
    }

    bool canUndo() const noexcept { return ! undo_.empty(); }
    bool canRedo() const noexcept { return ! redo_.empty(); }

    std::string undoLabel() const { return undo_.empty() ? std::string{} : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? std::string{} : redo_.back().label; }

    void undo()
    {
        if (undo_.empty())
            return;
        redo_.push_back({ undo_.back().label, std::move(present_), presentId_ });
        present_   = std::move(undo_.back().state);
        presentId_ = undo_.back().id; // the state's own id comes back with it
        undo_.pop_back();
    }

    void redo()
    {
        if (redo_.empty())
            return;
        undo_.push_back({ redo_.back().label, std::move(present_), presentId_ });
        present_   = std::move(redo_.back().state);
        presentId_ = redo_.back().id;
        redo_.pop_back();
    }

private:
    struct Entry
    {
        std::string        label;
        State              state;
        unsigned long long id = 0;
    };

    State              present_ {};
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;

    // Ids are handed out from a counter that only ever increases, so a state
    // reached by a different route is never mistaken for an earlier one.
    unsigned long long lastId_    = 0;
    unsigned long long presentId_ = 0;
};

} // namespace looper::model
