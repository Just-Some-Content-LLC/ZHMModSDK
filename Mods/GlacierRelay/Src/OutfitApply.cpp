#include "OutfitApply.h"

#include "RelayLog.h"

using CommandProtocol::FieldStatus;
using CommandProtocol::ObservedValue;
using CommandProtocol::OutfitTuple;
using CommandProtocol::Result;
using CommandProtocol::Status;
using OutfitObservation::Append;
using OutfitObservation::AppendStatus;
using OutfitObservation::Field;

namespace OutfitApply
{
    const char* InvocationText(Invocation p_Invocation)
    {
        switch (p_Invocation)
        {
            case Invocation::NotCalled: return "not_called";
            case Invocation::Called: return "called";
            case Invocation::Faulted: return "faulted";
        }
        return "not_called";
    }

    const char* EffectText(Effect p_Effect)
    {
        switch (p_Effect)
        {
            case Effect::None: return "none";
            case Effect::Changed: return "changed";
            case Effect::Unchanged: return "unchanged";
            case Effect::Uncertain: return "uncertain";
        }
        return "uncertain";
    }

    namespace
    {
        template <class T>
        Field<T> Known(const std::optional<T>& p_Value, const char* p_Why)
        {
            return p_Value ? Field<T>::Observed(*p_Value) : Field<T>::Of(FieldStatus::Unavailable, p_Why);
        }

        std::optional<OutfitTuple> TupleOf(const OutfitObservation::HeroOutfit& p_Hero)
        {
            if (!p_Hero.outfit_id.observed() || !p_Hero.charset.observed() || !p_Hero.variation.observed())
                return std::nullopt;

            OutfitTuple s_Tuple;
            s_Tuple.outfit_id = *p_Hero.outfit_id.value;
            s_Tuple.charset = static_cast<int32_t>(*p_Hero.charset.value);
            s_Tuple.variation = static_cast<int32_t>(*p_Hero.variation.value);
            return s_Tuple;
        }

        void AppendTuple(OutfitObservation::Entries& p_Out, const char* p_Prefix, const OutfitObservation::HeroOutfit& p_Hero)
        {
            const std::string s_Prefix = p_Prefix;
            Append(p_Out, (s_Prefix + "_outfit_id").c_str(), p_Hero.outfit_id);
            Append(p_Out, (s_Prefix + "_charset").c_str(), p_Hero.charset);
            Append(p_Out, (s_Prefix + "_variation").c_str(), p_Hero.variation);
        }

        void AppendTuple(OutfitObservation::Entries& p_Out, const char* p_Prefix, const OutfitTuple& p_Tuple)
        {
            const std::string s_Prefix = p_Prefix;
            Append(p_Out, (s_Prefix + "_outfit_id").c_str(), Field<std::string>::Observed(p_Tuple.outfit_id));
            Append(p_Out, (s_Prefix + "_charset").c_str(), Field<int64_t>::Observed(p_Tuple.charset));
            Append(p_Out, (s_Prefix + "_variation").c_str(), Field<int64_t>::Observed(p_Tuple.variation));
        }

        void AppendTupleUnavailable(OutfitObservation::Entries& p_Out, const char* p_Prefix, const char* p_Why)
        {
            const std::string s_Prefix = p_Prefix;
            AppendStatus(p_Out, (s_Prefix + "_outfit_id").c_str(), FieldStatus::Unavailable, p_Why);
            AppendStatus(p_Out, (s_Prefix + "_charset").c_str(), FieldStatus::Unavailable, p_Why);
            AppendStatus(p_Out, (s_Prefix + "_variation").c_str(), FieldStatus::Unavailable, p_Why);
        }

        void AppendText(OutfitObservation::Entries& p_Out, const char* p_Name, const char* p_Text)
        {
            Append(p_Out, p_Name, Field<std::string>::Observed(p_Text));
        }

        // The result that always fits: the generations, the three tuples, the invocation, the
        // effect and the verdict. Everything else is dropped and the reduction is marked.
        bool KeepWhenReduced(const std::string& p_Name)
        {
            static const char* k_Keep[] = {
                "generation_expected", "generation_at_receipt", "generation_cached", "generation_fresh", "generation_post",
                "previous_outfit_id", "previous_charset", "previous_variation",
                "requested_outfit_id", "requested_charset", "requested_variation",
                "current_outfit_id", "current_charset", "current_variation",
                "setter_called", "apply_log", "invocation", "effect", "precondition_ok", "precondition_reasons",
            };
            for (const char* s_Name : k_Keep)
                if (p_Name == s_Name)
                    return true;
            return false;
        }
    }

    Outcome Apply(const Context& p_Context, IOutfitApplier& p_Applier, Result& p_Result)
    {
        Outcome s_Outcome;
        const auto& s_Pending = *p_Context.pending;
        const auto& s_Command = s_Pending.command;
        auto& s_Out = p_Result.observed;

        // The decoder guarantees both tuples for this entry; a command without them never reaches
        // the queue. Checked anyway: the apply never guesses a target.
        if (!s_Command.outfit_target || !s_Command.outfit_expect_current)
        {
            p_Result.status = Status::Rejected;
            p_Result.reason = "outfit tuples missing";
            return s_Outcome;
        }

        const OutfitTuple& s_Target = *s_Command.outfit_target;
        const OutfitTuple& s_Expect = *s_Command.outfit_expect_current;
        const bool s_Latched = p_Applier.Latched();

        // 1. The frame-local reads, with the candidate resolved (the applier holds the references
        //    for this frame only). A latched applier is not asked to read anything.
        OutfitObservation::EngineReads s_Reads;
        if (!s_Latched)
            s_Reads = p_Applier.Observe(s_Target.outfit_id);

        const uint64_t s_Age = p_Context.now_mono_ms >= s_Pending.received_mono_ms ? p_Context.now_mono_ms - s_Pending.received_mono_ms : 0;
        const uint64_t s_LinkNow = p_Context.link_generation_now ? p_Context.link_generation_now() : p_Context.link_generation;

        PreconditionEvaluator::Input s_Input;
        s_Input.expected_generation = s_Command.mission_generation_precondition;
        s_Input.generation_at_receipt = s_Pending.at_receipt.mission_generation;
        s_Input.generation_cached = p_Context.mission_generation;
        s_Input.generation_fresh = s_Reads.generation_fresh;
        s_Input.mission_playing = p_Context.mission_playing;
        s_Input.previous_frame_playing = p_Context.previous_frame_playing;
        s_Input.frame = p_Context.frame;
        s_Input.rise_frame = p_Context.rise_frame;
        s_Input.now_mono_ms = p_Context.now_mono_ms;
        s_Input.rise_mono_ms = p_Context.rise_mono_ms;
        s_Input.scene_available = p_Context.scene_available;
        s_Input.scene_loaded = p_Context.scene.scene_loaded;
        s_Input.loading_stage = p_Context.scene.loading_stage;
        s_Input.scene_type = p_Context.scene.scene_type;
        s_Input.scene_resource_same_as_rise = p_Context.scene_resource_same_as_rise;
        s_Input.epoch_now = p_Context.relay_transition_epoch;
        s_Input.epoch_at_receipt = s_Pending.at_receipt.relay_transition_epoch;
        s_Input.contract_started_seen = p_Context.contract_started_seen;
        s_Input.contract_ended_seen = p_Context.contract_ended_seen;
        s_Input.player_available = !s_Latched && s_Reads.hero.player_resolved;
        s_Input.hero_outfit_id = s_Reads.hero.outfit_id;
        s_Input.hero_charset = s_Reads.hero.charset;
        s_Input.hero_variation = s_Reads.hero.variation;
        s_Input.hero_heroa_valid = s_Reads.hero.heroa_valid;
        s_Input.candidate = s_Reads.candidate;
        s_Input.set_outfit_available = s_Reads.set_outfit_available;
        s_Input.target = s_Target;
        s_Input.expect_current_tuple = s_Expect;
        s_Input.link_generation_unchanged = s_LinkNow != 0 && s_LinkNow == s_Pending.link_generation && s_LinkNow == p_Context.link_generation;
        s_Input.on_expected_thread = p_Context.expected_thread_id == 0 || p_Context.thread_id == p_Context.expected_thread_id;
        s_Input.effectful_latched = s_Latched;

        const auto s_Verdict = PreconditionEvaluator::Evaluate(s_Input, p_Context.rules);
        s_Outcome.precondition_ok = s_Verdict.ok;

        // 2. The prior tuple, before any call.
        const std::optional<OutfitTuple> s_Previous = TupleOf(s_Reads.hero);

        // 3–6. The call, only when every rule held, after the link generation is re-read once more
        //      and the "applying" line was accepted by the operating system in full (M5 design
        //      section 30, correction B): the write is checked, and the setter is not asked for
        //      when the line could not be written — an engine write without its pre-call record
        //      would leave a fail-stop with no evidence that the engine was asked. A written line
        //      is a submission to the OS file cache, not power-loss durability.
        CallOutcome s_Call;
        OutfitObservation::EngineReads s_Post;
        bool s_PostRead = false;
        bool s_LinkChangedAtCall = false;
        bool s_ApplyLogAttempted = false;
        RelayLog::WriteOutcome s_ApplyLog;

        if (s_Verdict.ok)
        {
            const uint64_t s_LinkAtCall = p_Context.link_generation_now ? p_Context.link_generation_now() : p_Context.link_generation;

            if (s_LinkAtCall != s_Pending.link_generation)
            {
                s_LinkChangedAtCall = true;
            }
            else
            {
                s_ApplyLogAttempted = true;
                s_ApplyLog = RelayLog::InfoChecked(
                    "command: {} applying player.outfit.set/2 on frame {}: previous {}/{}/{} -> requested {}/{}/{}; generations expected {} fresh {}; epoch {}; link generation {}",
                    s_Command.command_id, p_Context.frame,
                    s_Previous ? s_Previous->outfit_id : "?", s_Previous ? s_Previous->charset : -1, s_Previous ? s_Previous->variation : -1,
                    s_Target.outfit_id, s_Target.charset, s_Target.variation,
                    s_Command.mission_generation_precondition ? *s_Command.mission_generation_precondition : "?",
                    s_Reads.generation_fresh.observed() ? *s_Reads.generation_fresh.value : "?",
                    p_Context.relay_transition_epoch, s_LinkAtCall
                );

                if (s_ApplyLog.status == RelayLog::WriteStatus::Written)
                {
                    s_Call = p_Applier.Invoke(s_Target.charset, s_Target.variation);
                    s_Post = p_Applier.ReadBack();
                    s_PostRead = true;
                }
            }
        }

        const bool s_ApplyLogUnwritten = s_ApplyLogAttempted && s_ApplyLog.status != RelayLog::WriteStatus::Written;

        p_Applier.Release();

        // 7. Classification.
        const std::optional<OutfitTuple> s_Current = s_PostRead ? TupleOf(s_Post.hero) : std::nullopt;
        std::string s_Note;

        if (!s_Verdict.ok)
        {
            s_Outcome.invocation = Invocation::NotCalled;
            s_Outcome.effect = Effect::None;
            bool s_Unavailable = false;
            for (const auto s_Reason : s_Verdict.reasons)
                if (PreconditionEvaluator::IsUnavailability(s_Reason))
                    s_Unavailable = true;
            p_Result.status = s_Unavailable ? Status::Unavailable : Status::PreconditionFailed;
            p_Result.reason = (s_Unavailable ? "not applied (unavailable): " : "not applied (precondition failed): ") + PreconditionEvaluator::ReasonsText(s_Verdict);
        }
        else if (s_LinkChangedAtCall)
        {
            s_Outcome.invocation = Invocation::NotCalled;
            s_Outcome.effect = Effect::None;
            p_Result.status = Status::Unavailable;
            p_Result.reason = "not applied (unavailable): link_generation_changed immediately before the call";
        }
        else if (s_ApplyLogUnwritten)
        {
            s_Outcome.invocation = Invocation::NotCalled;
            s_Outcome.effect = Effect::None;
            p_Result.status = Status::Unavailable;
            p_Result.reason = fmt::format(
                "not applied (unavailable): apply_log_unwritten ({}: {} of {} bytes accepted, error {}); the setter was not called",
                RelayLog::WriteStatusText(s_ApplyLog.status), s_ApplyLog.written, s_ApplyLog.requested, s_ApplyLog.error
            );
        }
        else if (s_Call.faulted)
        {
            s_Outcome.invocation = Invocation::Faulted;
            s_Outcome.effect = Effect::Uncertain;
            p_Result.status = Status::Failed;
            p_Result.reason = "fault inside the setter; effect uncertain (possible effect; later observation required)";
        }
        else if (!s_Call.called)
        {
            s_Outcome.invocation = Invocation::NotCalled;
            s_Outcome.effect = Effect::None;
            p_Result.status = Status::Unavailable;
            p_Result.reason = "not applied (unavailable): the setter or its references were not available at the call";
        }
        else
        {
            s_Outcome.invocation = Invocation::Called;
            p_Result.status = Status::Applied;
            const bool s_GenerationHeld = s_Post.generation_fresh.observed() && s_Reads.generation_fresh.observed() && *s_Post.generation_fresh.value == *s_Reads.generation_fresh.value;

            if (!s_Current)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; read-back unavailable (possible effect; later observation required)";
            }
            else if (!s_GenerationHeld)
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the registry generation changed or was unreadable after the call";
            }
            else if (s_Previous && *s_Current == *s_Previous)
            {
                s_Outcome.effect = Effect::Unchanged;
                s_Note = "setter returned; the tuple equals the prior tuple (no confirmed change)";
            }
            else if (*s_Current == s_Target)
            {
                s_Outcome.effect = Effect::Changed;
                s_Note = "setter returned; the tuple equals the requested tuple (native read-back, same frame)";
            }
            else
            {
                s_Outcome.effect = Effect::Uncertain;
                s_Note = "setter returned; the tuple matches neither the prior nor the requested tuple";
            }

            p_Result.reason = s_Note;
        }

        // The read-back, in a fixed order: frame and scene; generations and transition; the prior,
        // requested and current tuples; the candidate; availability; invocation; verdict.
        Append(s_Out, "scene_available", Field<bool>::Observed(p_Context.scene_available));
        Append(s_Out, "scene_loaded", p_Context.scene_available ? Field<bool>::Observed(p_Context.scene.scene_loaded) : Field<bool>::Of(FieldStatus::Unavailable, "scene state unavailable"));
        Append(s_Out, "loading_stage", p_Context.scene_available ? Field<int64_t>::Observed(p_Context.scene.loading_stage) : Field<int64_t>::Of(FieldStatus::Unavailable, "scene state unavailable"));
        Append(s_Out, "scene_type", p_Context.scene_available ? Field<std::string>::Observed(p_Context.scene.scene_type) : Field<std::string>::Of(FieldStatus::Unavailable, "scene state unavailable"));
        Append(s_Out, "mission_playing", Field<bool>::Observed(p_Context.mission_playing));
        Append(s_Out, "prev_frame_playing", Field<bool>::Observed(p_Context.previous_frame_playing));
        Append(s_Out, "frame", Field<int64_t>::Observed(static_cast<int64_t>(p_Context.frame)));
        Append(s_Out, "receipt_to_frame_ms", Field<int64_t>::Observed(static_cast<int64_t>(s_Age)));
        Append(s_Out, "rise_age_ms", p_Context.mission_playing ? Field<int64_t>::Observed(static_cast<int64_t>(s_Verdict.stable_ms)) : Field<int64_t>::Of(FieldStatus::Unavailable, "not playing"));
        Append(s_Out, "rise_age_frames", p_Context.mission_playing ? Field<int64_t>::Observed(static_cast<int64_t>(s_Verdict.stable_frames)) : Field<int64_t>::Of(FieldStatus::Unavailable, "not playing"));
        Append(s_Out, "scene_same_as_rise", p_Context.mission_playing ? Field<bool>::Observed(p_Context.scene_resource_same_as_rise) : Field<bool>::Of(FieldStatus::Unavailable, "not playing"));

        Append(s_Out, "generation_expected", Known(s_Command.mission_generation_precondition, "not supplied by the command"));
        Append(s_Out, "generation_at_receipt", Known(s_Pending.at_receipt.mission_generation, "no open attempt at receipt"));
        Append(s_Out, "generation_cached", Known(p_Context.mission_generation, "no open attempt"));
        Append(s_Out, "generation_fresh", s_Latched ? Field<std::string>::Of(FieldStatus::Unavailable, "effectful path latched") : s_Reads.generation_fresh);
        Append(s_Out, "generation_post", s_PostRead ? s_Post.generation_fresh : Field<std::string>::Of(FieldStatus::Unavailable, "no call made"));
        Append(s_Out, "relay_transition_epoch", Field<int64_t>::Observed(static_cast<int64_t>(p_Context.relay_transition_epoch)));
        Append(s_Out, "epoch_at_receipt", Field<int64_t>::Observed(static_cast<int64_t>(s_Pending.at_receipt.relay_transition_epoch)));
        Append(s_Out, "epoch_post", s_PostRead ? Field<int64_t>::Observed(static_cast<int64_t>(p_Context.relay_transition_epoch)) : Field<int64_t>::Of(FieldStatus::Unavailable, "no call made"));
        Append(s_Out, "contract_started_seen", Known(p_Context.contract_started_seen, "none observed"));
        Append(s_Out, "contract_ended_seen", Known(p_Context.contract_ended_seen, "none observed"));

        if (s_Latched)
            AppendTupleUnavailable(s_Out, "previous", "effectful path latched");
        else
            AppendTuple(s_Out, "previous", s_Reads.hero);
        AppendTuple(s_Out, "requested", s_Target);
        if (s_PostRead)
            AppendTuple(s_Out, "current", s_Post.hero);
        else
            AppendTupleUnavailable(s_Out, "current", "no call made");

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
            AppendStatus(s_Out, "candidate_found", FieldStatus::Unavailable, s_Latched ? "effectful path latched" : "candidate not resolved");
        }

        Append(s_Out, "set_outfit_available", s_Latched ? Field<bool>::Of(FieldStatus::Unavailable, "effectful path latched") : s_Reads.set_outfit_available);
        Append(s_Out, "kit_map", s_Latched ? Field<bool>::Of(FieldStatus::Unavailable, "effectful path latched") : s_Reads.kit_map_available);
        Append(s_Out, "reads_on_frame", s_Latched ? Field<bool>::Of(FieldStatus::Unavailable, "effectful path latched") : Field<bool>::Observed(s_Reads.thread_id == p_Context.thread_id));
        Append(s_Out, "link_generation_at_call", Field<int64_t>::Observed(static_cast<int64_t>(s_LinkNow)));
        Append(s_Out, "setter_called", Field<bool>::Observed(s_Call.called));
        AppendText(s_Out, "apply_log", s_ApplyLogAttempted ? RelayLog::WriteStatusText(s_ApplyLog.status) : "not_attempted");
        AppendText(s_Out, "invocation", InvocationText(s_Outcome.invocation));
        AppendText(s_Out, "effect", EffectText(s_Outcome.effect));
        Append(s_Out, "precondition_ok", Field<bool>::Observed(s_Verdict.ok));
        Append(s_Out, "precondition_reasons", s_Verdict.ok ? Field<std::string>::Of(FieldStatus::Observed, "") : Field<std::string>::Observed(PreconditionEvaluator::ReasonsText(s_Verdict)));

        // The line bound is the protocol's: a result that would not fit keeps the tuples, the
        // generations, the invocation, the effect and the verdict, and says so — the reversal
        // planner must never lose the prior tuple to a bound.
        if (CommandProtocol::EncodeResult(p_Result).size() > CommandProtocol::k_MaxLineBytes)
        {
            s_Outcome.reduced = true;
            OutfitObservation::Entries s_Kept;
            for (auto& s_Entry : s_Out)
                if (KeepWhenReduced(s_Entry.first))
                {
                    s_Entry.second.field_reason.clear();
                    s_Kept.push_back(s_Entry);
                }
            s_Out = std::move(s_Kept);
            s_Out.emplace_back("observed_reduced", [] { ObservedValue v; v.kind = ObservedValue::Kind::Bool; v.boolean = true; return v; }());
            if (p_Result.reason.size() > 120)
                p_Result.reason.resize(120);
        }

        return s_Outcome;
    }
}
