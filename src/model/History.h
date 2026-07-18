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

    /** Replace the whole state, recording the previous one for undo. */
    void reset(State initial)
    {
        present_ = std::move(initial);
        undo_.clear();
        redo_.clear();
    }

    /** Commit a new state as an undoable edit. */
    void apply(State next, std::string label = {})
    {
        undo_.push_back({ std::move(label), present_ });
        present_ = std::move(next);
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
        redo_.push_back({ undo_.back().label, std::move(present_) });
        present_ = std::move(undo_.back().state);
        undo_.pop_back();
    }

    void redo()
    {
        if (redo_.empty())
            return;
        undo_.push_back({ redo_.back().label, std::move(present_) });
        present_ = std::move(redo_.back().state);
        redo_.pop_back();
    }

private:
    struct Entry
    {
        std::string label;
        State       state;
    };

    State              present_ {};
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
};

} // namespace looper::model
