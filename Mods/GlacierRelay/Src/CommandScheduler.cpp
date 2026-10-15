#include "CommandScheduler.h"

#include "OutfitApply.h"
#include "PreconditionEvaluator.h"
#include "RelayLog.h"

using CommandProtocol::FieldStatus;
using CommandProtocol::ObservedValue;
using CommandProtocol::Result;
using CommandProtocol::Status;

namespace
{
    ObservedValue Bool(bool p_Value)
    {
        ObservedValue s_Value;
        s_Value.kind = ObservedValue::Kind::Bool;
        s_Value.boolean = p_Value;
        return s_Value;
    }

    ObservedValue Integer(int64_t p_Value)
    {
        ObservedValue s_Value;
        s_Value.kind = ObservedValue::Kind::Integer;
        s_Value.integer = p_Value;
        return s_Value;
    }

    ObservedValue Text(std::string p_Value)
    {
        ObservedValue s_Value;
        s_Value.kind = ObservedValue::Kind::Text;
        s_Value.text = std::move(p_Value);
        return s_Value;
    }

    template <class T>
    OutfitObservation::Field<T> Known(const std::optional<T>& p_Value, const char* p_Why)
    {
        return p_Value ? OutfitObservation::Field<T>::Observed(*p_Value) : OutfitObservation::Field<T>::Of(FieldStatus::Unavailable, p_Why);
    }

    OutfitObservation::Field<std::string> KnownText(const std::optional<std::string>& p_Value, const char* p_Why)
    {
        return Known<std::string>(p_Value, p_Why);
    }
}

Result CommandScheduler::Base(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame) const
{
    Result s_Result;
    s_Result.command_id = p_Pending.command.command_id;
    s_Result.link_generation = p_Pending.link_generation;
    s_Result.adapter_instance_id = p_Frame.adapter_instance_id;
    s_Result.native_at = p_Frame.native_at;
    s_Result.frame = p_Frame.frame;
    s_Result.mission_generation = p_Frame.mission_generation;
    return s_Result;
}

// relay.inspect.outfit v1 (M5 C-1): no engine write; the read-back is the frame-local inspection
// with a per-field status, plus the hypothetical precondition verdict. The inspection itself is
// "applied" whenever it ran, whether or not the hypothetical precondition holds: inspecting a
// failing frame is the point.
void CommandScheduler::Inspect(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, Result& p_Result)
{
    using OutfitObservation::Append;
    using OutfitObservation::AppendStatus;
    using OutfitObservation::Field;

    std::optional<std::string> s_CandidateId;
    for (const auto& s_Param : p_Pending.command.params)
        if (s_Param.first == "disguise_repository_id")
            s_CandidateId = s_Param.second;

    // The engine reads, on this frame, through the plugin's inspector; without one (tests, the
    // probe) every engine field is unsupported and says so.
    OutfitObservation::EngineReads s_Reads;
    bool s_HaveInspector = static_cast<bool>(p_Frame.inspector);

    if (s_HaveInspector)
    {
        s_Reads = p_Frame.inspector(s_CandidateId);
    }
    else
    {
        const auto s_Unsupported = [](const char* p_Why) { return Field<std::string>::Of(FieldStatus::Unsupported, p_Why); };
        s_Reads.generation_fresh = s_Unsupported("no inspector on this frame source");
        s_Reads.hero.outfit_id = s_Unsupported("no inspector on this frame source");
        s_Reads.set_outfit_available = Field<bool>::Of(FieldStatus::Unsupported, "no inspector on this frame source");
        s_Reads.kit_map_available = Field<bool>::Of(FieldStatus::Unsupported, "no inspector on this frame source");
        if (s_CandidateId)
        {
            OutfitObservation::KitResolution s_None;
            s_None.found = Field<bool>::Of(FieldStatus::Unsupported, "no inspector on this frame source");
            s_Reads.candidate = s_None;
        }
    }

    const uint64_t s_Age = p_Frame.now_mono_ms >= p_Pending.received_mono_ms ? p_Frame.now_mono_ms - p_Pending.received_mono_ms : 0;
    const bool s_Playing = p_Frame.mission_playing;

    // The hypothetical precondition over the copied observation (pure; no action).
    PreconditionEvaluator::Input s_Input;
    s_Input.expected_generation = p_Pending.command.mission_generation_precondition;
    s_Input.generation_at_receipt = p_Pending.at_receipt.mission_generation;
    s_Input.generation_cached = p_Frame.mission_generation;
    s_Input.generation_fresh = s_Reads.generation_fresh;
    s_Input.mission_playing = s_Playing;
    s_Input.previous_frame_playing = p_Frame.previous_frame_playing;
    s_Input.frame = p_Frame.frame;
    s_Input.rise_frame = p_Frame.rise_frame;
    s_Input.now_mono_ms = p_Frame.now_mono_ms;
    s_Input.rise_mono_ms = p_Frame.rise_mono_ms;
    s_Input.scene_available = p_Frame.scene_available;
    s_Input.scene_loaded = p_Frame.scene.scene_loaded;
    s_Input.loading_stage = p_Frame.scene.loading_stage;
    s_Input.scene_type = p_Frame.scene.scene_type;
    s_Input.scene_resource_same_as_rise = p_Frame.scene_resource_same_as_rise;
    s_Input.epoch_now = p_Frame.relay_transition_epoch;
    s_Input.epoch_at_receipt = p_Pending.at_receipt.relay_transition_epoch;
    s_Input.contract_started_seen = p_Frame.contract_started_seen;
    s_Input.contract_ended_seen = p_Frame.contract_ended_seen;
    s_Input.player_available = s_HaveInspector && s_Reads.hero.player_resolved;
    s_Input.hero_outfit_id = s_Reads.hero.outfit_id;
    s_Input.hero_heroa_valid = s_Reads.hero.heroa_valid;
    s_Input.candidate = s_Reads.candidate;
    s_Input.set_outfit_available = s_Reads.set_outfit_available;

    const auto s_Verdict = PreconditionEvaluator::Evaluate(s_Input, m_Rules);

    // The read-back, in a fixed order: frame and scene, generations and transition, hero, candidate,
    // availability, verdict. Plain frame values are "observed" by construction; the engine reads
    // carry the status the reader gave them.
    auto& s_Out = p_Result.observed;
    Append(s_Out, "scene_available", Field<bool>::Observed(p_Frame.scene_available));
    Append(s_Out, "scene_loaded", p_Frame.scene_available ? Field<bool>::Observed(p_Frame.scene.scene_loaded) : Field<bool>::Of(FieldStatus::Unavailable, "scene state unavailable"));
    Append(s_Out, "loading_stage", p_Frame.scene_available ? Field<int64_t>::Observed(p_Frame.scene.loading_stage) : Field<int64_t>::Of(FieldStatus::Unavailable, "scene state unavailable"));
    Append(s_Out, "scene_type", p_Frame.scene_available ? Field<std::string>::Observed(p_Frame.scene.scene_type) : Field<std::string>::Of(FieldStatus::Unavailable, "scene state unavailable"));
    Append(s_Out, "mission_playing", Field<bool>::Observed(s_Playing));
    Append(s_Out, "prev_frame_playing", Field<bool>::Observed(p_Frame.previous_frame_playing));
    Append(s_Out, "frame", Field<int64_t>::Observed(static_cast<int64_t>(p_Frame.frame)));
    Append(s_Out, "receipt_to_frame_ms", Field<int64_t>::Observed(static_cast<int64_t>(s_Age)));
    Append(s_Out, "rise_age_ms", s_Playing ? Field<int64_t>::Observed(static_cast<int64_t>(s_Verdict.stable_ms)) : Field<int64_t>::Of(FieldStatus::Unavailable, "not playing"));
    Append(s_Out, "rise_age_frames", s_Playing ? Field<int64_t>::Observed(static_cast<int64_t>(s_Verdict.stable_frames)) : Field<int64_t>::Of(FieldStatus::Unavailable, "not playing"));
    Append(s_Out, "scene_same_as_rise", s_Playing ? Field<bool>::Observed(p_Frame.scene_resource_same_as_rise) : Field<bool>::Of(FieldStatus::Unavailable, "not playing"));

    Append(s_Out, "generation_expected", KnownText(p_Pending.command.mission_generation_precondition, "not supplied by the command"));
    Append(s_Out, "generation_at_receipt", KnownText(p_Pending.at_receipt.mission_generation, "no open attempt at receipt"));
    Append(s_Out, "generation_cached", KnownText(p_Frame.mission_generation, "no open attempt"));
    Append(s_Out, "generation_fresh", s_Reads.generation_fresh);
    Append(s_Out, "relay_transition_epoch", Field<int64_t>::Observed(static_cast<int64_t>(p_Frame.relay_transition_epoch)));
    Append(s_Out, "epoch_at_receipt", Field<int64_t>::Observed(static_cast<int64_t>(p_Pending.at_receipt.relay_transition_epoch)));
    Append(s_Out, "contract_started_seen", KnownText(p_Frame.contract_started_seen, "none observed"));
    Append(s_Out, "contract_ended_seen", KnownText(p_Frame.contract_ended_seen, "none observed"));

    Append(s_Out, "hero_outfit_id", s_Reads.hero.outfit_id);
    Append(s_Out, "hero_charset", s_Reads.hero.charset);
    Append(s_Out, "hero_variation", s_Reads.hero.variation);
    Append(s_Out, "hero_charset_count", s_Reads.hero.charset_count);
    Append(s_Out, "hero_heroa_vars", s_Reads.hero.heroa_variations);
    Append(s_Out, "hero_heroa_valid", s_Reads.hero.heroa_valid);
    Append(s_Out, "hero_suit", s_Reads.hero.is_hitman_suit);

    if (s_CandidateId)
    {
        Append(s_Out, "candidate_id", Field<std::string>::Observed(*s_CandidateId));
        if (s_Reads.candidate)
        {
            Append(s_Out, "candidate_found", s_Reads.candidate->found);
            Append(s_Out, "candidate_charsets", s_Reads.candidate->charsets);
            Append(s_Out, "candidate_heroa_vars", s_Reads.candidate->heroa_variations);
            Append(s_Out, "candidate_heroa_usable", s_Reads.candidate->heroa_usable);
            Append(s_Out, "candidate_suit", s_Reads.candidate->is_hitman_suit);
            Append(s_Out, "kits_scanned", s_Reads.candidate->kits_scanned);
        }
        else
        {
            AppendStatus(s_Out, "candidate_found", FieldStatus::Unavailable, "candidate not resolved");
        }
    }

    Append(s_Out, "set_outfit_available", s_Reads.set_outfit_available);
    Append(s_Out, "kit_map", s_Reads.kit_map_available);
    Append(s_Out, "reads_on_frame", s_HaveInspector ? Field<bool>::Observed(s_Reads.thread_id == p_Frame.thread_id) : Field<bool>::Of(FieldStatus::Unsupported, "no inspector"));

    Append(s_Out, "precondition_ok", Field<bool>::Observed(s_Verdict.ok));
    Append(s_Out, "precondition_reasons", s_Verdict.ok ? Field<std::string>::Of(FieldStatus::Observed, "") : Field<std::string>::Observed(PreconditionEvaluator::ReasonsText(s_Verdict)));

    p_Result.status = Status::Applied;
    if (!s_HaveInspector)
        p_Result.reason = "engine reads unsupported on this frame source; frame values only";
}

// M6-B1: the plain frame values both inspections share, in a fixed order, before the reads.
namespace
{
    void AppendFrameValues(CommandProtocol::Result& p_Result, const CommandScheduler::FrameObservation& p_Frame, uint64_t p_Age)
    {
        using OutfitObservation::Append;
        using OutfitObservation::Field;
        auto& s_Out = p_Result.observed;
        Append(s_Out, "mission_playing", Field<bool>::Observed(p_Frame.mission_playing));
        Append(s_Out, "scene_type", p_Frame.scene_available ? Field<std::string>::Observed(p_Frame.scene.scene_type) : Field<std::string>::Of(FieldStatus::Unavailable, "scene state unavailable"));
        Append(s_Out, "receipt_to_frame_ms", Field<int64_t>::Observed(static_cast<int64_t>(p_Age)));
    }
}

// Two stages. First the per-field reasons go (every field keeps its status and value; a
// "reasons_dropped" marker says so), which is what an all-unavailable registry needs; only if the
// line still does not fit are entries dropped from the end, with an "observed_reduced" marker. The
// result's own reason is extended, never replaced.
bool CommandScheduler::ReduceToFit(Result& p_Result)
{
    if (CommandProtocol::EncodeResult(p_Result).size() <= CommandProtocol::k_MaxLineBytes)
        return false;

    const auto s_Extend = [&](const char* p_Note) {
        if (p_Result.reason.empty())
            p_Result.reason = p_Note;
        else
            p_Result.reason += std::string("; ") + p_Note;
    };

    for (auto& s_Entry : p_Result.observed)
        s_Entry.second.field_reason.clear();

    if (p_Result.observed.size() < CommandProtocol::k_MaxObservedEntries)
        p_Result.observed.emplace_back("reasons_dropped", Bool(true));

    if (CommandProtocol::EncodeResult(p_Result).size() <= CommandProtocol::k_MaxLineBytes)
    {
        s_Extend("field reasons dropped to fit the result line bound");
        return true;
    }

    if (!p_Result.observed.empty() && p_Result.observed.back().first == "reasons_dropped")
        p_Result.observed.pop_back();

    while (!p_Result.observed.empty() && CommandProtocol::EncodeResult(p_Result).size() + 32 > CommandProtocol::k_MaxLineBytes)
        p_Result.observed.pop_back();

    p_Result.observed.emplace_back("observed_reduced", Bool(true));
    s_Extend("inspection reduced to fit the result line bound");
    return true;
}

// relay.inspect.players v1 (M6-B1): no engine write; the registry's four declared slots, the three
// words at 0x390 classified, and slot 0 through M5's local-hero path, each field with its status.
void CommandScheduler::InspectPlayers(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, Result& p_Result)
{
    using OutfitObservation::Append;
    using OutfitObservation::Field;

    const uint64_t s_Age = p_Frame.now_mono_ms >= p_Pending.received_mono_ms ? p_Frame.now_mono_ms - p_Pending.received_mono_ms : 0;
    const bool s_HaveInspector = static_cast<bool>(p_Frame.players_inspector);

    PlayerObservation::RegistryReads s_Reads = s_HaveInspector
        ? p_Frame.players_inspector()
        : PlayerObservation::AllRegistry(FieldStatus::Unsupported, "no inspector on this frame source");

    AppendFrameValues(p_Result, p_Frame, s_Age);
    PlayerObservation::AppendRegistry(p_Result.observed, s_Reads);
    Append(p_Result.observed, "reads_on_frame", s_HaveInspector ? Field<bool>::Observed(s_Reads.thread_id == p_Frame.thread_id) : Field<bool>::Of(FieldStatus::Unsupported, "no inspector"));

    p_Result.status = Status::Applied;
    if (!s_HaveInspector)
        p_Result.reason = "engine reads unsupported on this frame source; frame values only";
}

// relay.inspect.entity v1 (M6-B1): no engine write; the loaded-brick inventory, the local hero's
// authored identity and raw transform, the blueprint-factory round trip on the hero and the
// candidate (the historical hitmen.brick / Agent48 by default; expected absent without content).
void CommandScheduler::InspectEntity(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, Result& p_Result)
{
    using OutfitObservation::Append;
    using OutfitObservation::Field;

    const uint64_t s_Age = p_Frame.now_mono_ms >= p_Pending.received_mono_ms ? p_Frame.now_mono_ms - p_Pending.received_mono_ms : 0;
    const bool s_HaveInspector = static_cast<bool>(p_Frame.entity_inspector);

    PlayerObservation::EntityQuery s_Query;
    for (const auto& s_Param : p_Pending.command.params)
    {
        if (s_Param.first == "brick")
            s_Query.brick = s_Param.second;
        else if (s_Param.first == "entity_id")
            s_Query.entity_id = s_Param.second;
    }

    PlayerObservation::EntityReads s_Reads = s_HaveInspector
        ? p_Frame.entity_inspector(s_Query)
        : PlayerObservation::AllEntity(FieldStatus::Unsupported, "no inspector on this frame source", s_Query);

    AppendFrameValues(p_Result, p_Frame, s_Age);
    PlayerObservation::AppendEntity(p_Result.observed, s_Reads);
    Append(p_Result.observed, "reads_on_frame", s_HaveInspector ? Field<bool>::Observed(s_Reads.thread_id == p_Frame.thread_id) : Field<bool>::Of(FieldStatus::Unsupported, "no inspector"));

    p_Result.status = Status::Applied;
    if (!s_HaveInspector)
        p_Result.reason = "engine reads unsupported on this frame source; frame values only";
}

bool CommandScheduler::Tick(const FrameObservation& p_Frame)
{
    ++m_Stats.frames;

    if (m_Stats.first_thread_id == 0)
        m_Stats.first_thread_id = p_Frame.thread_id;
    else if (p_Frame.thread_id != m_Stats.first_thread_id)
        m_Stats.thread_changed = true;

    m_Stats.last_thread_id = p_Frame.thread_id;

    CommandQueue::FrameSnapshot s_Snapshot;
    s_Snapshot.mission_generation = p_Frame.mission_generation;
    s_Snapshot.relay_transition_epoch = p_Frame.relay_transition_epoch;
    const auto s_Pending = m_Queue.Pop(s_Snapshot);

    if (!s_Pending)
        return false;

    const auto& s_Command = s_Pending->command;

    // A command from a link that no longer exists: its issuer has already been told the link
    // was lost (and marked it unknown); there is nobody to answer and nothing to apply.
    if (p_Frame.link_generation == 0 || s_Pending->link_generation != p_Frame.link_generation)
    {
        ++m_Stats.stale_generation;
        m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Expired);
        RelayLog::Warn("command scheduler: dropped {} from link generation {} (current {})", s_Command.command_id, s_Pending->link_generation, p_Frame.link_generation);
        return true;
    }

    Result s_Result = Base(*s_Pending, p_Frame);
    const uint64_t s_Age = p_Frame.now_mono_ms >= s_Pending->received_mono_ms ? p_Frame.now_mono_ms - s_Pending->received_mono_ms : 0;

    if (s_Age > s_Command.ttl_ms)
    {
        ++m_Stats.expired;
        s_Result.status = Status::Expired;
        s_Result.reason = "ttl elapsed before the scheduled frame (" + std::to_string(s_Age) + " ms on the native clock)";
        m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Expired);
        m_Emit(s_Result);
        return true;
    }

    switch (s_Command.entry)
    {
        case CommandProtocol::Entry::RelayProbe1:
        {
            // No engine write. The read-back is what this frame observed, as plain values.
            ++m_Stats.executed;
            m_Stats.last_executed_frame = p_Frame.frame;
            m_Stats.last_latency_ms = s_Age;
            if (s_Age > m_Stats.max_latency_ms)
                m_Stats.max_latency_ms = s_Age;

            s_Result.status = Status::Applied;
            s_Result.observed.emplace_back("scene_available", Bool(p_Frame.scene_available));
            s_Result.observed.emplace_back("mission_playing", Bool(p_Frame.mission_playing));
            s_Result.observed.emplace_back("frame", Integer(static_cast<int64_t>(p_Frame.frame)));
            s_Result.observed.emplace_back("receipt_to_frame_ms", Integer(static_cast<int64_t>(s_Age)));

            if (p_Frame.scene_available)
            {
                s_Result.observed.emplace_back("scene_loaded", Bool(p_Frame.scene.scene_loaded));
                s_Result.observed.emplace_back("loading_stage", Integer(p_Frame.scene.loading_stage));
                s_Result.observed.emplace_back("scene_type", Text(p_Frame.scene.scene_type));
                s_Result.observed.emplace_back("codename_hint", Text(p_Frame.scene.codename_hint));
            }
            else
            {
                // The observation could not be obtained this frame: said so, never guessed.
                s_Result.reason = "scene state unavailable this frame";
            }

            m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Applied);
            m_Emit(s_Result);
            return true;
        }

        case CommandProtocol::Entry::RelayInspectOutfit1:
        {
            // No engine write. The reads happen here, on this frame, through the inspector.
            ++m_Stats.inspected;
            m_Stats.last_executed_frame = p_Frame.frame;
            m_Stats.last_latency_ms = s_Age;
            if (s_Age > m_Stats.max_latency_ms)
                m_Stats.max_latency_ms = s_Age;

            Inspect(*s_Pending, p_Frame, s_Result);

            // The line bound is the protocol's, not something to grow: an inspection that would not
            // fit is reduced to its first entries (frame, generations, transition, hero) and says so.
            if (CommandProtocol::EncodeResult(s_Result).size() > CommandProtocol::k_MaxLineBytes)
            {
                ++m_Stats.reduced;
                constexpr size_t k_Keep = 19; // through contract_ended_seen
                if (s_Result.observed.size() > k_Keep)
                    s_Result.observed.resize(k_Keep);
                for (auto& s_Entry : s_Result.observed)
                    s_Entry.second.field_reason.clear();
                s_Result.observed.emplace_back("observed_reduced", Bool(true));
                s_Result.reason = "inspection reduced to fit the result line bound";
            }

            m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Applied);
            m_Emit(s_Result);
            return true;
        }

        case CommandProtocol::Entry::RelayInspectPlayers1:
        case CommandProtocol::Entry::RelayInspectEntity1:
        {
            // M6-B1: no engine write. The reads happen here, on this frame, through the inspector.
            const bool s_Players = s_Command.entry == CommandProtocol::Entry::RelayInspectPlayers1;
            if (s_Players)
                ++m_Stats.players_inspected;
            else
                ++m_Stats.entities_inspected;
            m_Stats.last_executed_frame = p_Frame.frame;
            m_Stats.last_latency_ms = s_Age;
            if (s_Age > m_Stats.max_latency_ms)
                m_Stats.max_latency_ms = s_Age;

            if (s_Players)
                InspectPlayers(*s_Pending, p_Frame, s_Result);
            else
                InspectEntity(*s_Pending, p_Frame, s_Result);

            if (ReduceToFit(s_Result))
                ++m_Stats.reduced;
            m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Applied);
            m_Emit(s_Result);
            return true;
        }

        case CommandProtocol::Entry::PlayerOutfitSet2:
        {
            // M5 C-2B: the one effectful entry. Without an applier on this frame source (tests
            // without one, the probe's inert modes) it is rejected, never applied.
            ++m_Stats.outfit_dispatched;
            m_Stats.last_executed_frame = p_Frame.frame;
            m_Stats.last_latency_ms = s_Age;
            if (s_Age > m_Stats.max_latency_ms)
                m_Stats.max_latency_ms = s_Age;

            if (!p_Frame.applier)
            {
                ++m_Stats.rejected;
                s_Result.status = Status::Rejected;
                s_Result.reason = "effectful entry not executable on this frame source (no outfit applier)";
                m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Rejected);
                m_Emit(s_Result);
                return true;
            }

            ApplyOutfit(*s_Pending, p_Frame, s_Result);
            m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, s_Result.status);
            m_Emit(s_Result);
            return true;
        }

        case CommandProtocol::Entry::PlayerOutfitSet1Reserved:
        case CommandProtocol::Entry::Unknown:
        default:
            // The link never queues these; a defensive rejection keeps the invariant visible.
            ++m_Stats.rejected;
            s_Result.status = Status::Rejected;
            s_Result.reason = "not executable";
            m_Queue.MarkDone(s_Command.command_id, s_Pending->link_generation, Status::Rejected);
            m_Emit(s_Result);
            return true;
    }
}

// M5 C-2B: the apply context is this frame's observation as plain values plus the scheduler's
// own thread expectation; OutfitApply drives the applier and classifies the outcome.
void CommandScheduler::ApplyOutfit(const CommandQueue::Pending& p_Pending, const FrameObservation& p_Frame, Result& p_Result)
{
    OutfitApply::Context s_Context;
    s_Context.pending = &p_Pending;
    s_Context.frame = p_Frame.frame;
    s_Context.now_mono_ms = p_Frame.now_mono_ms;
    s_Context.thread_id = p_Frame.thread_id;
    s_Context.expected_thread_id = m_Stats.first_thread_id;
    s_Context.scene_available = p_Frame.scene_available;
    s_Context.scene = p_Frame.scene;
    s_Context.mission_playing = p_Frame.mission_playing;
    s_Context.previous_frame_playing = p_Frame.previous_frame_playing;
    s_Context.mission_generation = p_Frame.mission_generation;
    s_Context.link_generation = p_Frame.link_generation;
    s_Context.link_generation_now = p_Frame.link_generation_now;
    s_Context.relay_transition_epoch = p_Frame.relay_transition_epoch;
    s_Context.rise_frame = p_Frame.rise_frame;
    s_Context.rise_mono_ms = p_Frame.rise_mono_ms;
    s_Context.scene_resource_same_as_rise = p_Frame.scene_resource_same_as_rise;
    s_Context.contract_started_seen = p_Frame.contract_started_seen;
    s_Context.contract_ended_seen = p_Frame.contract_ended_seen;
    s_Context.rules = m_Rules;

    const auto s_Outcome = OutfitApply::Apply(s_Context, *p_Frame.applier, p_Result);

    if (s_Outcome.invocation != OutfitApply::Invocation::NotCalled)
        ++m_Stats.outfit_called;
    if (s_Outcome.effect == OutfitApply::Effect::Changed)
        ++m_Stats.outfit_changed;
    if (s_Outcome.effect == OutfitApply::Effect::Uncertain)
        ++m_Stats.outfit_uncertain;
    if (p_Result.status == Status::PreconditionFailed || p_Result.status == Status::Unavailable)
        ++m_Stats.outfit_not_applied;
    if (s_Outcome.reduced)
        ++m_Stats.reduced;
}
