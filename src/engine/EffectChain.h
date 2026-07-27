#pragma once

#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/DelayEffect.h"
#include "engine/FilterEffect.h"
#include "engine/ReverbEffect.h"

namespace looper::engine
{
/** What one node of a chain is. Mirrors model::EffectKind, which `engine`
    can't reference: `model` already depends on `engine` (a Clip owns an
    engine::Pattern), so the dependency can't run both ways. The owner
    converts at the boundary, as it does for automation curves. */
enum class EffectNodeKind
{
    Filter = 0,
    Delay  = 1,
    Reverb = 2,
    Plugin = 3
};

/** One effect in a track's chain. Virtual dispatch costs one indirect call
    per node per block, which is nothing against the work inside — and it's
    what lets a node hold only the state its own kind needs, instead of every
    node carrying a delay line it may never use. */
struct EffectProcessor
{
    virtual ~EffectProcessor() = default;

    virtual EffectNodeKind kind() const noexcept = 0;
    virtual void prepare(double sampleRate, int blockSize) = 0;
    virtual void process(juce::AudioBuffer<float>& buffer) = 0;
};

struct FilterNode final : EffectProcessor
{
    FilterEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Filter; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
};

struct DelayNode final : EffectProcessor
{
    DelayEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Delay; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
};

struct ReverbNode final : EffectProcessor
{
    ReverbEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Reverb; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
};

/**
    A track's insert chain: effects in order, each processing in place.

    Built and prepared on the message thread — where allocating a delay line
    is fine — then handed to the audio thread whole, the same pointer-swap the
    rest of the engine uses (Sequencer::ClipList, DrumPadMap, TrackAutomation).
    A swap can't be observed half-applied, which matters more here than
    usual: half a chain is a very different sound from all of it.

    Only *structural* changes rebuild: adding, removing, reordering, or
    changing a node's kind. Parameter changes go straight to the live nodes'
    atomics (see AudioEngine::trackChainFilter and friends), because
    rebuilding on every knob turn would reallocate delay lines and reset every
    tail in the chain — an audible glitch from turning a knob.
*/
class EffectChain
{
public:
    void add(std::unique_ptr<EffectProcessor> node) { nodes_.push_back(std::move(node)); }

    void prepare(double sampleRate, int blockSize)
    {
        for (auto& node : nodes_)
            node->prepare(sampleRate, blockSize);
    }

    /** Audio thread: run every node in order, in place. */
    void process(juce::AudioBuffer<float>& buffer)
    {
        for (auto& node : nodes_)
            node->process(buffer);
    }

    bool   empty() const noexcept { return nodes_.empty(); }
    size_t size() const noexcept  { return nodes_.size(); }

    /** The first node of a given kind, or nullptr. How parameter setters find
        their target while the UI still offers one of each kind; a chain
        editor (§20 stage 3) will address nodes by index instead. */
    template <typename NodeType>
    NodeType* firstOfKind()
    {
        for (auto& node : nodes_)
            if (auto* typed = dynamic_cast<NodeType*>(node.get()))
                return typed;
        return nullptr;
    }

private:
    std::vector<std::unique_ptr<EffectProcessor>> nodes_;
};

} // namespace looper::engine
