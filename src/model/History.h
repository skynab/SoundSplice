#pragma once

#include <string>
#include <utility>
#include <vector>

namespace soundsplice::model
{
/**
    Snapshot-based undo/redo over a copyable value state.

    Each edit records the previous state with a label; undo/redo move between
    snapshots. Simple and correct; a delta-based command history (smaller
    snapshots, the "an AI edit is just a command" seam) is a future refinement.

    **Positions and branches**, for the History pane. The line of states -
    everything undo reaches, the present, everything redo reaches - is
    numbered from 0 (the oldest) and can be jumped along. And an edit made
    after an undo doesn't throw the redo steps away: they're kept as a
    branch off the state they left from, and switchToBranch makes them the
    redo steps again (the ones they replace becoming a branch in their turn),
    so no line of work is lost to a stray edit. A few branches are kept, the
    oldest let go first.
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
        const auto was = presentId_;
        presentId_     = ++lastId_;
        for (auto& branch : branches_) // still leaving from here, nudged or not
            if (branch.fromId == was)
                branch.fromId = presentId_;
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
        branches_.clear();
    }

    /** Commit a new state as an undoable edit. */
    void apply(State next, std::string label = {})
    {
        undo_.push_back({ std::move(label), present_, presentId_ });
        present_   = std::move(next);
        presentId_ = ++lastId_;
        keepRedoAsBranch(undo_.back().id);
    }

    /** Mutate a copy of the current state and commit it. */
    template <typename EditFn>
    void edit(std::string label, EditFn&& fn)
    {
        State next = present_;
        fn(next);
        apply(std::move(next), std::move(label));
    }

    /** Calls @p fn with every state the history holds: the present one, then
        each undo and redo snapshot. For finding what any state the user can
        still get back to refers to, such as an audio file an undo would
        restore. */
    template <typename Fn>
    void forEachState(Fn&& fn) const
    {
        fn(present_);
        for (const auto& entry : undo_)
            fn(entry.state);
        for (const auto& entry : redo_)
            fn(entry.state);
        for (const auto& branch : branches_)
            for (const auto& entry : branch.entries)
                fn(entry.state);
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

    // ---- positions: the line of states, 0 the oldest

    int size() const noexcept { return (int) (undo_.size() + 1 + redo_.size()); }
    int position() const noexcept { return (int) undo_.size(); }

    const State& stateAt(int index) const
    {
        if (index < (int) undo_.size())
            return undo_[(size_t) index].state;
        if (index == (int) undo_.size())
            return present_;
        return redo_[redo_.size() - (size_t) (index - (int) undo_.size())].state;
    }

    /** What made the state at @p index: the edit's label, or empty for the
        first state. */
    std::string labelAt(int index) const
    {
        if (index <= 0 || index > size() - 1)
            return {};
        if (index <= (int) undo_.size())
            return undo_[(size_t) index - 1].label;
        return redo_[redo_.size() - (size_t) (index - (int) undo_.size())].label;
    }

    unsigned long long idAt(int index) const
    {
        if (index < (int) undo_.size())
            return undo_[(size_t) index].id;
        if (index == (int) undo_.size())
            return presentId_;
        return redo_[redo_.size() - (size_t) (index - (int) undo_.size())].id;
    }

    /** Undoes or redoes to the state at @p index. */
    void jumpTo(int index)
    {
        index = index < 0 ? 0 : index > size() - 1 ? size() - 1 : index;
        while (position() > index)
            undo();
        while (position() < index)
            redo();
    }

    // ---- branches

    struct BranchInfo
    {
        std::string firstLabel; // the first edit along it
        int         steps    = 0;
        int         fromIndex = -1; // where on the line it leaves from; -1: not on this line
    };

    std::vector<BranchInfo> branches() const
    {
        std::vector<BranchInfo> out;
        for (const auto& branch : branches_)
            out.push_back({ branch.entries.empty() ? std::string() : branch.entries.back().label,
                            (int) branch.entries.size(), indexOfId(branch.fromId) });
        return out;
    }

    /** Goes to where branch @p which leaves the line, and makes its steps the
        redo steps. False if it doesn't leave from this line. */
    bool switchToBranch(int which)
    {
        if (which < 0 || which >= (int) branches_.size())
            return false;
        const int from = indexOfId(branches_[(size_t) which].fromId);
        if (from < 0)
            return false;

        jumpTo(from);
        auto branch = std::move(branches_[(size_t) which]);
        branches_.erase(branches_.begin() + which);
        keepRedoAsBranch(presentId_);
        redo_ = std::move(branch.entries);
        return true;
    }

    static constexpr int kMaxBranches = 12;

private:
    struct Entry
    {
        std::string        label;
        State              state;
        unsigned long long id = 0;
    };

    struct Branch
    {
        unsigned long long fromId = 0; // the state it leaves from
        std::vector<Entry> entries;    // as redo_ holds them: the next step last
    };

    /** The redo steps, kept as a branch off the state @p fromId rather than
        lost, and cleared. */
    void keepRedoAsBranch(unsigned long long fromId)
    {
        if (redo_.empty())
            return;
        branches_.push_back({ fromId, std::move(redo_) });
        redo_.clear();
        while ((int) branches_.size() > kMaxBranches)
            branches_.erase(branches_.begin());
    }

    int indexOfId(unsigned long long id) const
    {
        for (int i = 0; i < size(); ++i)
            if (idAt(i) == id)
                return i;
        return -1;
    }

    State               present_ {};
    std::vector<Entry>  undo_;
    std::vector<Entry>  redo_;
    std::vector<Branch> branches_;

    // Ids are handed out from a counter that only ever increases, so a state
    // reached by a different route is never mistaken for an earlier one.
    unsigned long long lastId_    = 0;
    unsigned long long presentId_ = 0;
};

} // namespace soundsplice::model
