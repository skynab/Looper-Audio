#pragma once

#include <memory>
#include <string>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "engine/DelayEffect.h"
#include "engine/DriveEffect.h"
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
    Plugin = 3,
    Drive  = 4
};

/** What a chain slot should be. Carries plugin identity as plain strings —
    the engine can't reference model::PluginRef, and this is the same boundary
    the rest of the engine keeps. */
struct EffectSlotSpec
{
    EffectNodeKind kind = EffectNodeKind::Filter;
    std::string    pluginFormat;
    std::string    pluginIdentifier;
    std::string    pluginState; // base64, applied after instantiation

    /** Only identity matters for deciding whether to rebuild — a changed
        preset is restored onto the existing instance, not a new chain. */
    bool sameShapeAs(const EffectSlotSpec& other) const
    {
        return kind == other.kind
            && pluginFormat == other.pluginFormat
            && pluginIdentifier == other.pluginIdentifier;
    }
};

/** Every built-in's parameters for one slot, pushed by index. All of them
    travel together because only the ones matching the slot's kind are read —
    the same trade model::EffectSlot makes, so switching a slot's kind doesn't
    lose the settings of the others. */
struct EffectSlotParams
{
    bool  enabled = false;

    int   filterMode      = 0;
    float filterCutoff    = 1000.0f;
    float filterResonance = 0.707f;

    float delayTimeMs   = 300.0f;
    float delayFeedback = 0.35f;
    float delayMix      = 0.3f;

    float reverbRoomSize = 0.5f;
    float reverbDamping  = 0.5f;
    float reverbMix      = 0.3f;

    float driveAmount   = 4.0f;
    float driveTone     = 0.5f;
    float driveLevel    = 0.7f;
    bool  driveHardClip = false;
    bool  driveCabinet  = true;
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

    /** Bypass. Means the same thing for a hosted plugin as for a built-in: the
        node stays in the chain and passes audio through untouched. */
    virtual void setEnabled(bool enabled) = 0;
};

struct FilterNode final : EffectProcessor
{
    FilterEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Filter; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct DelayNode final : EffectProcessor
{
    DelayEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Delay; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct DriveNode final : EffectProcessor
{
    DriveEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Drive; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
};

struct ReverbNode final : EffectProcessor
{
    ReverbEffect effect;

    EffectNodeKind kind() const noexcept override { return EffectNodeKind::Reverb; }
    void prepare(double sampleRate, int blockSize) override { effect.prepare(sampleRate, blockSize); }
    void process(juce::AudioBuffer<float>& buffer) override { effect.process(buffer); }
    void setEnabled(bool enabled) override { effect.setEnabled(enabled); }
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

    /** Applies one slot's parameters, addressed by *position*. By index rather
        than by kind because a chain may hold two filters, and "the filter"
        stops meaning anything the moment it does. An out-of-range index is
        ignored: the live chain can be one rebuild behind the document. */
    void applyParams(size_t index, const EffectSlotParams& params)
    {
        if (index >= nodes_.size())
            return;

        auto& node = *nodes_[index];
        node.setEnabled(params.enabled);

        if (auto* filter = dynamic_cast<FilterNode*>(&node))
        {
            filter->effect.setMode(params.filterMode);
            filter->effect.setCutoff(params.filterCutoff);
            filter->effect.setResonance(params.filterResonance);
        }
        else if (auto* delay = dynamic_cast<DelayNode*>(&node))
        {
            delay->effect.setTimeMs(params.delayTimeMs);
            delay->effect.setFeedback(params.delayFeedback);
            delay->effect.setMix(params.delayMix);
        }
        else if (auto* reverb = dynamic_cast<ReverbNode*>(&node))
        {
            reverb->effect.setRoomSize(params.reverbRoomSize);
            reverb->effect.setDamping(params.reverbDamping);
            reverb->effect.setMix(params.reverbMix);
        }
        else if (auto* drive = dynamic_cast<DriveNode*>(&node))
        {
            drive->effect.setDrive(params.driveAmount);
            drive->effect.setTone(params.driveTone);
            drive->effect.setLevel(params.driveLevel);
            drive->effect.setHardClip(params.driveHardClip);
            drive->effect.setCabinet(params.driveCabinet);
        }
    }

    EffectProcessor* nodeAt(size_t index) { return index < nodes_.size() ? nodes_[index].get() : nullptr; }

private:
    std::vector<std::unique_ptr<EffectProcessor>> nodes_;
};

} // namespace looper::engine
