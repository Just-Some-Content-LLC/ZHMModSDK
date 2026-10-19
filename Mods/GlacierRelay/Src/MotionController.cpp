#include "MotionController.h"

#include <cmath>

#include <fmt/format.h>

#include "PreconditionEvaluator.h"
#include "RelayLog.h"

using CommandProtocol::FieldStatus;
using CommandProtocol::ObservedValue;
using CommandProtocol::Result;
using CommandProtocol::Status;
using MotionProtocol::Outcome;
using OutfitObservation::Append;
using OutfitObservation::Field;
using TransformObservation::Reads;
using TransformObservation::Snapshot;

namespace Motion
{
    namespace
    {
        constexpr size_t k_WordX = 9;
        constexpr size_t k_WordY = 10;

        uint64_t Now(const Frame& p_Frame)
        {
            return p_Frame.clock ? p_Frame.clock() : p_Frame.now_mono_ms;
        }

        Reads ReadOnly(const std::function<Reads()>& p_Inspector)
        {
            return p_Inspector ? p_Inspector() : TransformObservation::AllUnavailable(FieldStatus::Unsupported, "no inspector on this frame source");
        }

        void AppendText(OutfitObservation::Entries& p_Out, const char* p_Name, const std::string& p_Text)
        {
            Append(p_Out, p_Name, Field<std::string>::Observed(p_Text));
        }

        void AppendInt(OutfitObservation::Entries& p_Out, const char* p_Name, int64_t p_Value)
        {
            Append(p_Out, p_Name, Field<int64_t>::Observed(p_Value));
        }

        void AppendBool(OutfitObservation::Entries& p_Out, const char* p_Name, bool p_Value)
        {
            Append(p_Out, p_Name, Field<bool>::Observed(p_Value));
        }

        std::string Join(const std::vector<std::string>& p_Reasons)
        {
            std::string s_Text;
            for (const auto& s_Reason : p_Reasons)
                s_Text += (s_Text.empty() ? "" : ",") + s_Reason;
            return s_Text;
        }

        // The frame's transition facts against the values a share or session opened under.
        std::string TransitionReason(const Frame& p_Frame, const std::string& p_Generation, const std::string& p_Scene, uint64_t p_Epoch)
        {
            if (!p_Frame.mission_playing)
                return "mission_not_playing";
            if (p_Frame.relay_transition_epoch != p_Epoch)
                return "transition_epoch_changed";
            if (!p_Frame.mission_generation || *p_Frame.mission_generation != p_Generation)
                return "generation_changed";
            if (!p_Frame.scene_available || p_Frame.scene.scene_resource != p_Scene)
                return "scene_changed";
            return std::string();
        }

        bool Readable(const Snapshot& p_Snapshot)
        {
            return p_Snapshot.world.observed() && p_Snapshot.local.observed() && p_Snapshot.parent.observed() && p_Snapshot.dirty.observed();
        }
    }

    Controller::Controller(Options p_Options, IMotionLink* p_Link) :
        m_Options(p_Options),
        m_Link(p_Link)
    {
        if (m_Options.max_sessions < 1 || m_Options.max_sessions > 4)
            m_Options.max_sessions = 1;
    }

    bool Controller::KnownSession(const std::string& p_Id) const
    {
        for (const auto& s_Id : m_Closed)
            if (s_Id == p_Id)
                return true;
        return m_Record && m_Record->session_id == p_Id;
    }

    void Controller::NoteInvocation(uint64_t p_Frame)
    {
        m_InvokedOnce = true;
        m_Stats.last_invocation_frame = p_Frame;
    }

    // ---- evidence lines ----------------------------------------------------------------------------

    bool Controller::Control(const std::string& p_Line)
    {
        if (!m_Link || !m_Session)
            return false;

        const auto s_Queued = m_Link->SendControl(m_Session->serial, p_Line);
        if (s_Queued == IMotionLink::Queued::Queued)
            return true;

        // Required evidence cannot be delivered within the bounded policy: no further setter call in
        // this session (it closes at the end of this frame at the latest), and a full queue ends the
        // binding (BEAM then sees the link's loss, never a silent gap).
        m_EvidenceLost = true;
        if (s_Queued == IMotionLink::Queued::Full)
        {
            ++m_Stats.control_full;
            m_Link->Drop(m_Session->serial, "required motion evidence could not be queued (control queue full)");
        }
        return false;
    }

    void Controller::Ack(const Frame& p_Frame, const Pending& p_Target, Outcome p_Outcome, const std::string& p_Reason, std::optional<std::pair<uint32_t, uint32_t>> p_Post, std::optional<bool> p_Exact, uint64_t p_NativeMs)
    {
        SessionState& s_Session = *m_Session;

        switch (p_Outcome)
        {
            case Outcome::Applied: ++s_Session.counts.applied; ++m_Stats.applied; break;
            case Outcome::SkippedIdentical: ++s_Session.counts.skipped; ++m_Stats.skipped; break;
            case Outcome::Refused: ++s_Session.counts.refused; ++m_Stats.refused; break;
            case Outcome::Uncertain: ++s_Session.counts.uncertain; ++m_Stats.uncertain; break;
        }

        MotionProtocol::Applied s_Applied;
        s_Applied.session_id = s_Session.session_id;
        s_Applied.seq = p_Target.seq;
        s_Applied.frame = p_Frame.frame;
        s_Applied.native_mono_ms = p_NativeMs;
        s_Applied.outcome = p_Outcome;
        if (!p_Reason.empty())
            s_Applied.reason = p_Reason;
        if (p_Post)
        {
            s_Applied.post_x = p_Post->first;
            s_Applied.post_y = p_Post->second;
        }
        s_Applied.exact = p_Exact;

        if (p_Outcome != Outcome::Applied && p_Outcome != Outcome::SkippedIdentical)
            RelayLog::Info("motion: session {} target {} {} ({}) on frame {}", s_Session.session_id, p_Target.seq, MotionProtocol::OutcomeText(p_Outcome), p_Reason, p_Frame.frame);

        Control(MotionProtocol::EncodeApplied(s_Applied));
    }

    // ---- the share ---------------------------------------------------------------------------------

    void Controller::Share(const TransformApply::Context& p_Context, const std::function<Reads()>& p_Inspector, Result& p_Result)
    {
        const auto& s_Command = p_Context.pending->command;
        if (!s_Command.share)
        {
            p_Result.status = Status::Rejected;
            p_Result.reason = "share request missing";
            return;
        }

        const auto& s_Request = *s_Command.share;
        const uint64_t s_LinkNow = p_Context.link_generation_now ? p_Context.link_generation_now() : p_Context.link_generation;
        const IMotionLink::Binding s_Binding = m_Link ? m_Link->Current() : IMotionLink::Binding{};

        std::vector<std::string> s_Reasons;
        bool s_Unavailable = false;

        if (!s_Binding.bound)
        {
            s_Reasons.push_back("motion_link_not_bound");
            s_Unavailable = true;
        }
        else
        {
            if (s_Binding.motion_generation != s_Request.motion_link_generation)
                s_Reasons.push_back("motion_link_generation_mismatch");
            if (s_Binding.command_generation != s_LinkNow)
                s_Reasons.push_back("motion_link_not_on_this_command_link");
        }
        if (m_Share)
            s_Reasons.push_back("share_open");
        for (const auto& s_Id : m_ClosedShares)
            if (s_Id == s_Request.share_id)
                s_Reasons.push_back("share_id_reused");

        // The local hero's identity on this frame: read-only, the established stored-field reads.
        const Reads s_Reads = ReadOnly(p_Inspector);
        const auto s_Common = TransformApply::CommonGate(p_Context, s_Reads, s_LinkNow, false);
        for (const auto s_Reason : s_Common.reasons)
            if (PreconditionEvaluator::IsUnavailability(s_Reason))
                s_Unavailable = true;

        if (!s_Reads.hero_entity_id.observed())
        {
            s_Reasons.push_back("hero_identity_unavailable");
            s_Unavailable = true;
        }
        else if (*s_Reads.hero_entity_id.value == TransformObservation::k_Entity || (s_Reads.lookup.same_entity.observed() && *s_Reads.lookup.same_entity.value))
        {
            s_Reasons.push_back("hero_is_the_candidate");
        }
        if (!(s_Reads.hero.world.observed() && s_Reads.hero.dirty.observed()))
        {
            s_Reasons.push_back("hero_transform_unreadable");
            s_Unavailable = true;
        }

        const bool s_Ok = s_Common.ok && s_Reasons.empty();
        std::string s_ReasonText = PreconditionEvaluator::ReasonsText(s_Common);
        if (!s_Reasons.empty())
            s_ReasonText += (s_ReasonText.empty() ? "" : ",") + Join(s_Reasons);

        if (s_Ok)
        {
            ShareState s_Share;
            s_Share.share_id = s_Request.share_id;
            s_Share.serial = s_Binding.serial;
            s_Share.motion_generation = s_Binding.motion_generation;
            s_Share.command_generation = s_Binding.command_generation;
            s_Share.interval_ms = s_Request.interval_ms;
            s_Share.duration_ms = s_Request.duration_ms;
            s_Share.opened_ms = p_Context.now_mono_ms;
            s_Share.next_sample_ms = p_Context.now_mono_ms + s_Request.interval_ms;
            s_Share.hero_entity_id = *s_Reads.hero_entity_id.value;
            s_Share.generation = *s_Reads.generation_fresh.value;
            s_Share.scene_resource = p_Context.scene.scene_resource;
            s_Share.epoch = p_Context.relay_transition_epoch;
            m_Share = s_Share;
            ++m_Stats.shares_opened;
            p_Result.status = Status::Applied;
            p_Result.reason = "share open: read-only samples of the local hero every interval_ms for at most duration_ms on the bound motion link; no engine write";
            RelayLog::Info("motion: share {} opened on frame {} (interval {} ms, duration {} ms, motion link generation {} serial {}, hero {})", s_Share.share_id, p_Context.frame, s_Share.interval_ms, s_Share.duration_ms, s_Share.motion_generation, s_Share.serial, s_Share.hero_entity_id);
        }
        else
        {
            p_Result.status = s_Unavailable ? Status::Unavailable : Status::PreconditionFailed;
            p_Result.reason = (s_Unavailable ? "not opened (unavailable): " : "not opened (precondition failed): ") + s_ReasonText;
        }

        auto& s_Out = p_Result.observed;
        AppendText(s_Out, "mode", "open");
        AppendText(s_Out, "share_id", s_Request.share_id);
        AppendBool(s_Out, "share_open", s_Ok);
        AppendInt(s_Out, "interval_ms", s_Request.interval_ms);
        AppendInt(s_Out, "duration_ms", s_Request.duration_ms);
        AppendInt(s_Out, "motion_link_generation", static_cast<int64_t>(s_Binding.motion_generation));
        AppendInt(s_Out, "command_link_generation", static_cast<int64_t>(s_Binding.command_generation));
        Append(s_Out, "hero_entity_id", s_Reads.hero_entity_id);
        Append(s_Out, "generation_fresh", s_Reads.generation_fresh);
        AppendBool(s_Out, "mission_playing", p_Context.mission_playing);
        AppendInt(s_Out, "receipt_to_frame_ms", static_cast<int64_t>(p_Context.now_mono_ms >= p_Context.pending->received_mono_ms ? p_Context.now_mono_ms - p_Context.pending->received_mono_ms : 0));
        AppendBool(s_Out, "precondition_ok", s_Ok);
        Append(s_Out, "precondition_reasons", s_Ok ? Field<std::string>::Of(FieldStatus::Observed, "") : Field<std::string>::Observed(s_ReasonText));
        Append(s_Out, "reads_on_frame", Field<bool>::Observed(s_Reads.thread_id == p_Context.thread_id));
    }

    void Controller::CloseShare(const char* p_Why, const std::string& p_Detail, bool p_Send)
    {
        if (!m_Share)
            return;

        const ShareState s_Share = *m_Share;
        m_Share.reset();
        const std::string s_Reason = MotionProtocol::ReasonText(std::string(p_Why) + p_Detail);

        bool s_Sent = false;
        if (p_Send && m_Link)
        {
            const auto s_Queued = m_Link->SendControl(s_Share.serial, MotionProtocol::EncodeShareClosed(s_Share.share_id, s_Reason, s_Share.last_seq, s_Share.sent));
            s_Sent = s_Queued == IMotionLink::Queued::Queued;
            if (s_Queued == IMotionLink::Queued::Full)
            {
                ++m_Stats.control_full;
                m_Link->Drop(s_Share.serial, "share.closed could not be queued (control queue full)");
            }
        }

        m_ClosedShares.push_back(s_Share.share_id);
        while (m_ClosedShares.size() > m_Options.closed_history)
            m_ClosedShares.pop_front();
        ++m_Stats.shares_closed;
        m_Stats.last_share_close = s_Reason;
        RelayLog::Info("motion: share {} closed ({}); {} sample tick(s), {} sent, last seq {}; share.closed {}", s_Share.share_id, s_Reason, s_Share.seq, s_Share.sent, s_Share.last_seq ? std::to_string(*s_Share.last_seq) : "none", s_Sent ? "queued" : "not sent (no bound link)");
    }

    void Controller::StepShare(const Frame& p_Frame)
    {
        if (!m_Share)
            return;

        ShareState& s_Share = *m_Share;
        const uint64_t s_Now = p_Frame.now_mono_ms;
        if (s_Now < s_Share.next_sample_ms)
            return;

        // No catch-up after a slow frame: the next sample is one interval after this one.
        s_Share.next_sample_ms = s_Now + s_Share.interval_ms;

        const Reads s_Reads = ReadOnly(p_Frame.inspector);
        if (!(s_Reads.generation_fresh.observed() && *s_Reads.generation_fresh.value == s_Share.generation))
            return CloseShare("generation_changed", std::string(), true);
        if (!(s_Reads.hero_entity_id.observed() && *s_Reads.hero_entity_id.value == s_Share.hero_entity_id))
            return CloseShare("hero_identity_lost", std::string(), true);
        if (*s_Reads.hero_entity_id.value == TransformObservation::k_Entity || (s_Reads.lookup.same_entity.observed() && *s_Reads.lookup.same_entity.value))
            return CloseShare("hero_is_the_candidate", std::string(), true);

        const uint64_t s_Seq = ++s_Share.seq;
        ++m_Stats.samples_taken;

        // A missing read is never a clean zero: no line (the sink sees the gap); a dirty one is sent
        // labelled dirty, as the schema allows.
        if (!(s_Reads.hero.world.observed() && s_Reads.hero.dirty.observed()))
        {
            ++m_Stats.samples_unavailable;
            return;
        }

        const auto& s_World = *s_Reads.hero.world.value;
        const uint32_t s_Pos[3] = {s_World.w[k_WordX], s_World.w[k_WordY], s_World.w[11]};
        const bool s_Dirty = *s_Reads.hero.dirty.value;
        const auto s_Queued = m_Link ? m_Link->SendSample(s_Share.serial, MotionProtocol::EncodeHeroSample(s_Share.share_id, s_Seq, p_Frame.frame, s_Now, s_Pos, s_Dirty)) : IMotionLink::Queued::NotBound;

        if (s_Queued == IMotionLink::Queued::Queued)
        {
            ++s_Share.sent;
            s_Share.last_seq = s_Seq;
            ++m_Stats.samples_queued;
            if (s_Dirty)
                ++m_Stats.samples_dirty;
        }
        else if (s_Queued == IMotionLink::Queued::Full)
        {
            ++m_Stats.samples_dropped; // the newest is refused; nothing older is replaced
        }
        else
        {
            CloseShare("motion_link_lost", std::string(), false);
        }
    }

    // ---- the session: commands ---------------------------------------------------------------------

    void Controller::Session(const TransformApply::Context& p_Context, TransformApply::ITransformApplier* p_Applier, const TransformApply::Ledger& p_M6, const std::function<Reads()>& p_Inspector, Result& p_Result)
    {
        const auto& s_Command = p_Context.pending->command;
        if (!s_Command.motion)
        {
            p_Result.status = Status::Rejected;
            p_Result.reason = "motion session request missing";
            return;
        }

        if (s_Command.motion->mode == CommandProtocol::MotionSessionMode::Open)
            OpenSession(p_Context, p_Applier, p_M6, p_Inspector, p_Result);
        else
            RestoreSession(p_Context, p_Applier, p_Result);
    }

    bool Controller::RecordAtBaseline(const Reads& p_Reads) const
    {
        if (!m_Record)
            return false;
        const Snapshot& s_Candidate = p_Reads.candidate;
        return TransformApply::IdentityFailures(p_Reads).empty() && Readable(s_Candidate) && !*s_Candidate.dirty.value
            && *s_Candidate.world.value == m_Record->opening_world && *s_Candidate.parent.value == m_Record->opening_parent
            && p_Reads.generation_fresh.observed() && *p_Reads.generation_fresh.value == m_Record->mission_generation;
    }

    bool Controller::RecordBlocks(uint64_t p_Frame, const Reads* p_Reads, const char* p_ForWhat)
    {
        if (!m_Record || m_Record->restored)
            return false;
        if (m_Record->invalidated)
            return true;

        // A clean, never-restored record: released only when this frame's read shows the candidate
        // at the session's opening preimage (no write), as BEAM's confirm_baseline does from its
        // journal.
        if (p_Reads && RecordAtBaseline(*p_Reads))
        {
            ++m_Stats.released_at_baseline;
            RelayLog::Info("motion: session {} record released at baseline on frame {} for {}: the candidate's twelve world words equal the opening preimage, clean, same parent and generation (observed; no write)", m_Record->session_id, p_Frame, p_ForWhat);
            m_Record.reset();
            return false;
        }
        return true;
    }

    bool Controller::HoldsTransform(uint64_t p_Frame, const std::function<Reads()>& p_Inspector)
    {
        bool s_Holds = false;
        if (m_Session)
        {
            s_Holds = true;
        }
        else if (m_Record && !m_Record->restored)
        {
            if (m_Record->invalidated)
            {
                s_Holds = true;
            }
            else
            {
                const Reads s_Reads = ReadOnly(p_Inspector);
                s_Holds = RecordBlocks(p_Frame, &s_Reads, "an M6 transform command");
            }
        }

        if (s_Holds)
            ++m_Stats.m6_held;
        return s_Holds;
    }

    void Controller::OpenSession(const TransformApply::Context& p_Context, TransformApply::ITransformApplier* p_Applier, const TransformApply::Ledger& p_M6, const std::function<Reads()>& p_Inspector, Result& p_Result)
    {
        (void)p_Inspector;
        const auto& s_Pending = *p_Context.pending;
        const auto& s_Request = *s_Pending.command.motion;
        const uint64_t s_LinkNow = p_Context.link_generation_now ? p_Context.link_generation_now() : p_Context.link_generation;
        const IMotionLink::Binding s_Binding = m_Link ? m_Link->Current() : IMotionLink::Binding{};

        std::vector<std::string> s_Reasons;
        bool s_Unavailable = false;

        if (!p_Applier || !m_Options.effectful)
        {
            s_Reasons.push_back("applier_unavailable");
            s_Unavailable = true;
        }
        if (!s_Binding.bound)
        {
            s_Reasons.push_back("motion_link_not_bound");
            s_Unavailable = true;
        }
        else
        {
            if (!s_Request.motion_link_generation || s_Binding.motion_generation != *s_Request.motion_link_generation)
                s_Reasons.push_back("motion_link_generation_mismatch");
            if (s_Binding.command_generation != s_LinkNow)
                s_Reasons.push_back("motion_link_not_on_this_command_link");
        }
        if (m_Session)
            s_Reasons.push_back("session_open");
        if (KnownSession(s_Request.session_id))
            s_Reasons.push_back("session_id_reused");
        if (m_Stats.sessions_opened >= m_Options.max_sessions)
            s_Reasons.push_back("session_budget_exhausted");
        if (p_M6.forward_budget == 0 && !(p_M6.state == TransformApply::Ledger::State::Restored && p_M6.restore_confirmed))
            s_Reasons.push_back("m6_forward_unrestored");

        // The opening frame's own reads (admission is not a setter call; the handles are released here).
        const bool s_Latched = p_Applier ? p_Applier->Latched() : false;
        const Reads s_Reads = (p_Applier && !s_Latched) ? p_Applier->Observe() : TransformObservation::AllUnavailable(FieldStatus::Unavailable, p_Applier ? "effectful path latched" : "no applier");
        if (p_Applier)
            p_Applier->Release();

        if (RecordBlocks(p_Context.frame, &s_Reads, "a motion open"))
            s_Reasons.push_back("motion_session_retained");

        const auto s_Common = TransformApply::CommonGate(p_Context, s_Reads, s_LinkNow, s_Latched || (p_Applier && p_Applier->Latched()));
        for (const auto s_Reason : s_Common.reasons)
            if (PreconditionEvaluator::IsUnavailability(s_Reason))
                s_Unavailable = true;

        for (const auto s_Reason : TransformApply::IdentityFailures(s_Reads))
        {
            s_Reasons.push_back(TransformApply::ReasonCode(s_Reason));
            if (TransformApply::IsUnavailability(s_Reason))
                s_Unavailable = true;
        }
        if (!s_Reads.hero_entity_id.observed())
        {
            s_Reasons.push_back("hero_identity_unavailable");
            s_Unavailable = true;
        }

        const Snapshot& s_Pre = s_Reads.candidate;
        if (!Readable(s_Pre))
        {
            s_Reasons.push_back("preimage_unavailable");
            s_Unavailable = true;
        }
        else
        {
            if (*s_Pre.dirty.value)
                s_Reasons.push_back("preimage_dirty");
            if (*s_Pre.world.value != s_Request.expect_world)
                s_Reasons.push_back("expect_world_mismatch");
            if (*s_Pre.local.value != s_Request.expect_local)
                s_Reasons.push_back("expect_local_mismatch");
            if (*s_Pre.parent.value != s_Request.expect_parent)
                s_Reasons.push_back("expect_parent_mismatch");
        }

        const bool s_Ok = s_Common.ok && s_Reasons.empty();
        std::string s_ReasonText = PreconditionEvaluator::ReasonsText(s_Common);
        if (!s_Reasons.empty())
            s_ReasonText += (s_ReasonText.empty() ? "" : ",") + Join(s_Reasons);

        if (s_Ok)
        {
            SessionState s_Session;
            s_Session.session_id = s_Request.session_id;
            s_Session.open_command_id = s_Pending.command.command_id;
            s_Session.serial = s_Binding.serial;
            s_Session.motion_generation = s_Binding.motion_generation;
            s_Session.command_generation = s_Binding.command_generation;
            s_Session.limits = s_Request.limits;
            s_Session.opened_ms = p_Context.now_mono_ms;
            s_Session.silence_ref_ms = p_Context.now_mono_ms;
            s_Session.generation = *s_Reads.generation_fresh.value;
            s_Session.scene_resource = p_Context.scene.scene_resource;
            s_Session.epoch = p_Context.relay_transition_epoch;
            s_Session.thread_id = p_Context.thread_id;
            s_Session.hero_entity_id = *s_Reads.hero_entity_id.value;
            s_Session.opening_world = *s_Pre.world.value;
            s_Session.opening_local = *s_Pre.local.value;
            s_Session.opening_parent = *s_Pre.parent.value;
            s_Session.ref_x = s_Session.opening_world.w[k_WordX];
            s_Session.ref_y = s_Session.opening_world.w[k_WordY];
            m_Session = s_Session;
            m_EvidenceLost = false;
            m_Record.reset(); // a record released at baseline above is gone; the new session's comes at its close
            ++m_Stats.sessions_opened;
            p_Result.status = Status::Applied;
            p_Result.reason = "session open: admission only (no setter call); targets apply on later frames under the per-frame checks";
            RelayLog::Info(
                "motion: session {} opened on frame {} by {}: opening world {} local {} parent {}; limits duration {} ms, half extent {} mm, speed {} mm/s, credit {} ms, apply interval {} ms, silence {} ms; motion link generation {} serial {}, command link generation {}; generation {}; sessions {} of {}",
                s_Session.session_id, p_Context.frame, s_Session.open_command_id, FloatWords::Encode(s_Session.opening_world), FloatWords::Encode(s_Session.opening_local), s_Session.opening_parent,
                s_Session.limits.duration_ms, s_Session.limits.half_extent_mm, s_Session.limits.max_speed_mmps, s_Session.limits.credit_ms, s_Session.limits.min_apply_interval_ms, s_Session.limits.silence_close_ms,
                s_Session.motion_generation, s_Session.serial, s_Session.command_generation, s_Session.generation, m_Stats.sessions_opened, m_Options.max_sessions
            );
        }
        else
        {
            p_Result.status = s_Unavailable ? Status::Unavailable : Status::PreconditionFailed;
            p_Result.reason = (s_Unavailable ? "not opened (unavailable): " : "not opened (precondition failed): ") + s_ReasonText;
        }

        auto& s_Out = p_Result.observed;
        AppendText(s_Out, "mode", "open");
        AppendText(s_Out, "session_id", s_Request.session_id);
        AppendBool(s_Out, "session_open", s_Ok);
        if (s_Ok)
        {
            TransformObservation::Append(s_Out, "opening_world", Field<FloatWords::Words12>::Observed(m_Session->opening_world));
            TransformObservation::Append(s_Out, "opening_local", Field<FloatWords::Words7>::Observed(m_Session->opening_local));
            AppendText(s_Out, "opening_parent", m_Session->opening_parent);
        }
        else
        {
            OutfitObservation::AppendStatus(s_Out, "opening_world", FieldStatus::Unavailable, "not opened");
            OutfitObservation::AppendStatus(s_Out, "opening_local", FieldStatus::Unavailable, "not opened");
            OutfitObservation::AppendStatus(s_Out, "opening_parent", FieldStatus::Unavailable, "not opened");
        }
        Append(s_Out, "preimage_dirty", s_Pre.dirty);
        AppendInt(s_Out, "motion_link_generation", static_cast<int64_t>(s_Binding.motion_generation));
        AppendInt(s_Out, "command_link_generation", static_cast<int64_t>(s_Binding.command_generation));
        AppendInt(s_Out, "duration_ms", s_Request.limits.duration_ms);
        AppendInt(s_Out, "half_extent_mm", s_Request.limits.half_extent_mm);
        AppendInt(s_Out, "max_speed_mmps", s_Request.limits.max_speed_mmps);
        AppendInt(s_Out, "credit_ms", s_Request.limits.credit_ms);
        AppendInt(s_Out, "min_apply_interval_ms", s_Request.limits.min_apply_interval_ms);
        AppendInt(s_Out, "silence_close_ms", s_Request.limits.silence_close_ms);
        Append(s_Out, "generation_fresh", s_Reads.generation_fresh);
        AppendText(s_Out, "targets", s_Reads.brick_id + "," + s_Reads.entity_id);
        Append(s_Out, "cand_index_check", s_Reads.lookup.index_check);
        Append(s_Out, "cand_resolved", s_Reads.lookup.resolved);
        Append(s_Out, "cand_spatial_distinct", s_Reads.spatial_distinct);
        Append(s_Out, "registry_slots", s_Reads.registry_slots);
        Append(s_Out, "cand_in_registry", s_Reads.candidate_in_registry);
        Append(s_Out, "hero_entity_id", s_Reads.hero_entity_id);
        AppendInt(s_Out, "sessions_opened", static_cast<int64_t>(m_Stats.sessions_opened));
        AppendInt(s_Out, "max_sessions", static_cast<int64_t>(m_Options.max_sessions));
        AppendBool(s_Out, "mission_playing", p_Context.mission_playing);
        AppendInt(s_Out, "receipt_to_frame_ms", static_cast<int64_t>(p_Context.now_mono_ms >= s_Pending.received_mono_ms ? p_Context.now_mono_ms - s_Pending.received_mono_ms : 0));
        AppendBool(s_Out, "setter_called", false);
        AppendText(s_Out, "invocation", OutfitApply::InvocationText(OutfitApply::Invocation::NotCalled));
        AppendText(s_Out, "effect", OutfitApply::EffectText(OutfitApply::Effect::None));
        AppendBool(s_Out, "precondition_ok", s_Ok);
        Append(s_Out, "precondition_reasons", s_Ok ? Field<std::string>::Of(FieldStatus::Observed, "") : Field<std::string>::Observed(s_ReasonText));
        Append(s_Out, "reads_on_frame", s_Latched ? Field<bool>::Of(FieldStatus::Unavailable, "effectful path latched") : Field<bool>::Observed(s_Reads.thread_id == p_Context.thread_id));
    }

    void Controller::RestoreSession(const TransformApply::Context& p_Context, TransformApply::ITransformApplier* p_Applier, Result& p_Result)
    {
        const auto& s_Request = *p_Context.pending->command.motion;
        ++m_Stats.restores_dispatched;

        if (!p_Applier || !m_Options.effectful)
        {
            p_Result.status = Status::Unavailable;
            p_Result.reason = "not applied (unavailable): applier_unavailable";
            return;
        }

        const TransformApply::MotionRecord* s_Record = (m_Record && m_Record->session_id == s_Request.session_id) ? &*m_Record : nullptr;
        TransformApply::MotionRestoreVerdict s_Verdict;
        TransformApply::ApplyMotionRestore(p_Context, *p_Applier, s_Record, m_Session.has_value(), static_cast<int64_t>(SessionsRemaining()), p_Result, s_Verdict);

        if (s_Verdict.invoked)
        {
            NoteInvocation(p_Context.frame);
            ++m_Stats.restores_invoked;
        }

        if (m_Record && s_Record)
        {
            if (s_Verdict.restored)
            {
                m_Record->restored = true;
                m_Record->restorable = false;
                ++m_Stats.restores_proven;
                RelayLog::Info("motion: session {} restored on frame {}: every same-frame restoration fact held (the record is released)", m_Record->session_id, p_Context.frame);
            }
            else if (s_Verdict.invalidate)
            {
                m_Record->invalidated = true;
                m_Record->restorable = false;
                RelayLog::Warn("motion: session {} restoration attempted on frame {} without proof: the record is invalidated (no second restore, no baseline release in this process)", m_Record->session_id, p_Context.frame);
            }
        }
    }

    // ---- the session: every frame ------------------------------------------------------------------

    void Controller::Step(const Frame& p_Frame)
    {
        // The signal count is read BEFORE the drain: a cancellation queued after the drain changes it.
        const uint64_t s_Signals = m_Link ? m_Link->Signals() : 0;
        const std::vector<IMotionLink::Inbound> s_Inbound = m_Link ? m_Link->Drain() : std::vector<IMotionLink::Inbound>{};
        const uint64_t s_Now = p_Frame.now_mono_ms;
        const IMotionLink::Binding s_Binding = m_Link ? m_Link->Current() : IMotionLink::Binding{};
        const uint64_t s_CommandNow = p_Frame.command_link_generation_now ? p_Frame.command_link_generation_now() : 0;

        // 1. Deadlines first, at this frame's processing time, before any message is processed.
        if (m_Session)
        {
            const SessionState& s_Session = *m_Session;
            if (s_Now >= s_Session.opened_ms + s_Session.limits.duration_ms)
                CloseSession(p_Frame, "duration", true);
            else if (s_Now >= s_Session.silence_ref_ms + s_Session.limits.silence_close_ms)
                CloseSession(p_Frame, "target_silence", true);
        }
        if (m_Share && s_Now >= m_Share->opened_ms + m_Share->duration_ms)
            CloseShare("duration", std::string(), true);

        // 2. The bindings and the frame's transitions.
        if (m_Session)
        {
            const SessionState& s_Session = *m_Session;
            if (!s_Binding.bound || s_Binding.serial != s_Session.serial)
                CloseSession(p_Frame, "motion_link_lost", false);
            else if (s_CommandNow != s_Session.command_generation)
                CloseSession(p_Frame, "command_link_lost", true);
            else if (p_Frame.thread_id != s_Session.thread_id)
                CloseSession(p_Frame, "frame_thread_changed", true);
            else
            {
                const std::string s_Transition = TransitionReason(p_Frame, s_Session.generation, s_Session.scene_resource, s_Session.epoch);
                if (!s_Transition.empty())
                    CloseSession(p_Frame, s_Transition, true);
            }
        }
        if (m_Share)
        {
            const ShareState& s_Share = *m_Share;
            if (!s_Binding.bound || s_Binding.serial != s_Share.serial)
                CloseShare("motion_link_lost", std::string(), false);
            else if (s_CommandNow != s_Share.command_generation)
                CloseShare("command_link_lost", std::string(), true);
            else
            {
                const std::string s_Transition = TransitionReason(p_Frame, s_Share.generation, s_Share.scene_resource, s_Share.epoch);
                if (!s_Transition.empty())
                    CloseShare(s_Transition.c_str(), std::string(), true);
            }
        }

        // 3. BEAM's messages, in arrival order.
        Dispatch(p_Frame, s_Inbound);

        // 4. The share's sample for this frame, if one is due.
        StepShare(p_Frame);

        // 5. The session's pending target, then its report.
        if (m_Session && !m_EvidenceLost)
            ApplyPending(p_Frame, s_Signals);
        if (m_Session && !m_EvidenceLost)
            MaybeReport(p_Frame);

        // 6. Evidence that could not be queued this frame ends the session now (nothing can be sent).
        if (m_Session && m_EvidenceLost)
            CloseSession(p_Frame, "evidence_undeliverable", false);
    }

    void Controller::Dispatch(const Frame& p_Frame, const std::vector<IMotionLink::Inbound>& p_Inbound)
    {
        for (const auto& s_Item : p_Inbound)
        {
            const auto& s_Message = s_Item.message;

            if (s_Message.kind == MotionProtocol::Kind::ShareClose)
            {
                if (m_Share && s_Message.id == m_Share->share_id)
                {
                    if (s_Item.serial != m_Share->serial)
                        ++m_Stats.stale_inbound;
                    else
                        CloseShare("beam:", s_Message.reason, true);
                }
                else
                {
                    bool s_Known = false;
                    for (const auto& s_Id : m_ClosedShares)
                        s_Known = s_Known || s_Id == s_Message.id;
                    if (s_Known)
                        ++m_Stats.closes_for_closed;
                    else
                        ++m_Stats.foreign_closes;
                }
                continue;
            }

            const bool s_Ours = m_Session && s_Message.id == m_Session->session_id;
            if (s_Ours && s_Item.serial != m_Session->serial)
            {
                ++m_Stats.stale_inbound;
                continue;
            }

            if (s_Message.kind == MotionProtocol::Kind::Close)
            {
                if (s_Ours)
                    CloseSession(p_Frame, "beam:" + s_Message.reason, true);
                else if (KnownSession(s_Message.id))
                    ++m_Stats.closes_for_closed; // harmless (design 17.4): BEAM may close after a native close it had not processed
                else
                    ++m_Stats.foreign_closes;    // terminates nothing
                continue;
            }

            if (s_Message.kind != MotionProtocol::Kind::Target)
                continue; // the link admits nothing else after the binding

            if (!s_Ours)
            {
                if (KnownSession(s_Message.id))
                    ++m_Stats.late_targets;  // never applied, never reopening
                else
                    ++m_Stats.foreign_targets;
                continue;
            }

            SessionState& s_Session = *m_Session;
            ++s_Session.counts.received;
            ++m_Stats.targets_received;
            const Pending s_Target{s_Message.seq, s_Message.x, s_Message.y, s_Item.received_mono_ms};

            if (s_Message.seq <= s_Session.last_received_seq)
            {
                Ack(p_Frame, s_Target, Outcome::Refused, "duplicate_or_older", std::nullopt, std::nullopt, p_Frame.now_mono_ms);
                CloseSession(p_Frame, "refused:duplicate_or_older", true);
                continue;
            }
            s_Session.last_received_seq = s_Message.seq;

            if (s_Session.pending)
            {
                // One pending slot: BEAM keeps one target in flight, so a second one is refused, never
                // superseding the first (superseded stays 0).
                Ack(p_Frame, s_Target, Outcome::Refused, "pending_slot_occupied", std::nullopt, std::nullopt, p_Frame.now_mono_ms);
                CloseSession(p_Frame, "refused:pending_slot_occupied", true);
                continue;
            }

            // Accepted into the pending slot on a frame before the silence deadline (applied first):
            // only this refreshes the target-silence clock, with the target's receipt time.
            s_Session.pending = s_Target;
            if (s_Item.received_mono_ms > s_Session.silence_ref_ms)
                s_Session.silence_ref_ms = s_Item.received_mono_ms;
            if (!s_Session.first_target_seen)
            {
                s_Session.first_target_seen = true;
                s_Session.next_report_ms = p_Frame.now_mono_ms + m_Options.report_interval_ms;
            }
        }
    }

    void Controller::ApplyPending(const Frame& p_Frame, uint64_t p_SignalsAtDrain)
    {
        SessionState& s_Session = *m_Session;
        if (!s_Session.pending)
            return;

        // At most one setter call per frame (any path) and at least min_apply_interval_ms apart.
        if (InvokedOnFrame(p_Frame.frame))
        {
            ++m_Stats.two_writes_prevented;
            return;
        }
        const uint64_t s_Now = p_Frame.now_mono_ms;
        if (s_Session.applied_once && s_Now < s_Session.last_apply_ms + s_Session.limits.min_apply_interval_ms)
            return;

        const Pending s_Target = *s_Session.pending;
        TransformApply::ITransformApplier* s_Applier = p_Frame.applier;

        // A refusal: no invocation; the target is acknowledged refused and application ends (the close
        // reason names the refusal, or the deadline that the re-check before the call found passed).
        auto s_Refuse = [&](const std::string& p_Why, const std::string& p_CloseReason = std::string()) {
            if (s_Applier)
                s_Applier->Release();
            m_Session->pending.reset();
            Ack(p_Frame, s_Target, Outcome::Refused, p_Why, std::nullopt, std::nullopt, Now(p_Frame));
            CloseSession(p_Frame, p_CloseReason.empty() ? "refused:" + p_Why : p_CloseReason, true);
        };

        if (!s_Applier)
            return s_Refuse("applier_unavailable");
        if (s_Applier->Latched())
            return s_Refuse("effectful_latched");

        // The frame's own reads: everything re-resolved on this frame.
        const Reads s_Reads = s_Applier->Observe();
        if (s_Applier->Latched())
            return s_Refuse("fault_during_reads");
        if (!(s_Reads.generation_fresh.observed() && *s_Reads.generation_fresh.value == s_Session.generation))
            return s_Refuse("generation_changed");
        const auto s_Identity = TransformApply::IdentityFailures(s_Reads);
        if (!s_Identity.empty())
            return s_Refuse(std::string("identity:") + TransformApply::ReasonCode(s_Identity.front()));
        if (!(s_Reads.hero_entity_id.observed() && *s_Reads.hero_entity_id.value == s_Session.hero_entity_id))
            return s_Refuse("local_hero_changed");

        const Snapshot& s_Pre = s_Reads.candidate;
        if (!Readable(s_Pre))
            return s_Refuse("snapshot_unavailable");
        if (*s_Pre.dirty.value)
            return s_Refuse("snapshot_dirty");
        if (*s_Pre.parent.value != s_Session.opening_parent)
            return s_Refuse("parent_changed");
        const auto& s_World = *s_Pre.world.value;
        for (size_t i = 0; i < FloatWords::k_WorldWords; ++i)
            if (i != k_WordX && i != k_WordY && s_World.w[i] != s_Session.opening_world.w[i])
                return s_Refuse("untouched_word_changed");
        if (s_World.w[k_WordX] != s_Session.ref_x || s_World.w[k_WordY] != s_Session.ref_y)
            return s_Refuse("position_changed");
        if (s_Session.last_applied_seq && s_Target.seq <= *s_Session.last_applied_seq)
            return s_Refuse("duplicate_or_older");

        // The area (inclusive) and the displacement from the reference (inclusive), in double over
        // the exact binary32 values; no exemption for the first target, a pause or a reconnect.
        const double s_Tx = FloatWords::ToFloat(s_Target.x);
        const double s_Ty = FloatWords::ToFloat(s_Target.y);
        const double s_Ax = FloatWords::ToFloat(s_Session.opening_world.w[k_WordX]);
        const double s_Ay = FloatWords::ToFloat(s_Session.opening_world.w[k_WordY]);
        const double s_Half = static_cast<double>(s_Session.limits.half_extent_mm) / 1000.0;
        if (std::fabs(s_Tx - s_Ax) > s_Half || std::fabs(s_Ty - s_Ay) > s_Half)
            return s_Refuse("outside_area");
        const double s_Rx = FloatWords::ToFloat(s_Session.ref_x);
        const double s_Ry = FloatWords::ToFloat(s_Session.ref_y);
        const double s_Cap = static_cast<double>(s_Session.limits.max_speed_mmps) * static_cast<double>(s_Session.limits.credit_ms) / 1000000.0;
        if (std::sqrt((s_Tx - s_Rx) * (s_Tx - s_Rx) + (s_Ty - s_Ry) * (s_Ty - s_Ry)) > s_Cap)
            return s_Refuse("displacement");

        // The same words as the reference — and so as this frame's own read: no call.
        if (s_Target.x == s_Session.ref_x && s_Target.y == s_Session.ref_y)
        {
            s_Applier->Release();
            m_Session->pending.reset();
            Ack(p_Frame, s_Target, Outcome::SkippedIdentical, std::string(), std::make_pair(s_World.w[k_WordX], s_World.w[k_WordY]), std::nullopt, s_Now);
            return;
        }

        FloatWords::Words12 s_Requested = s_Session.opening_world;
        s_Requested.w[k_WordX] = s_Target.x;
        s_Requested.w[k_WordY] = s_Target.y;

        // The checked pre-call line: no call unless the operating system accepted it in full.
        const auto s_Log = RelayLog::InfoChecked(
            "motion: session {} applying target {} on frame {}: candidate {} in {}; x,y {} -> {}; reference seq {}; motion link generation {} serial {}, command link generation {}; generation {}",
            s_Session.session_id, s_Target.seq, p_Frame.frame, s_Reads.entity_id, s_Reads.brick_id,
            FloatWords::Encode(&s_World.w[k_WordX], 2), FloatWords::Encode(&s_Requested.w[k_WordX], 2),
            s_Session.last_applied_seq ? std::to_string(*s_Session.last_applied_seq) : "opening", s_Session.motion_generation, s_Session.serial,
            s_Session.command_generation, s_Session.generation
        );
        if (s_Log.status != RelayLog::WriteStatus::Written)
            return s_Refuse(std::string("apply_log_unwritten:") + RelayLog::WriteStatusText(s_Log.status));

        // Again, after that potentially blocking write and immediately before the call: the clock,
        // the cancellations, both bindings and the latch.
        const uint64_t s_AtCall = Now(p_Frame);
        if (s_AtCall >= s_Session.opened_ms + s_Session.limits.duration_ms)
        {
            ++m_Stats.deadline_at_call;
            return s_Refuse("deadline_at_call:duration", "duration");
        }
        if (s_AtCall >= s_Session.silence_ref_ms + s_Session.limits.silence_close_ms)
        {
            ++m_Stats.deadline_at_call;
            return s_Refuse("deadline_at_call:target_silence", "target_silence");
        }
        if (m_Link && m_Link->Signals() != p_SignalsAtDrain)
        {
            // A close or a binding end arrived since this frame's drain: handle it first; the target
            // stays pending for a later frame if the session survives.
            ++m_Stats.cancelled_at_call;
            s_Applier->Release();
            Dispatch(p_Frame, m_Link->Drain());
            const IMotionLink::Binding s_Binding = m_Link->Current();
            if (m_Session && (!s_Binding.bound || s_Binding.serial != m_Session->serial))
                CloseSession(p_Frame, "motion_link_lost", false);
            return;
        }
        const IMotionLink::Binding s_Binding = m_Link ? m_Link->Current() : IMotionLink::Binding{};
        if (!s_Binding.bound || s_Binding.serial != s_Session.serial)
        {
            ++m_Stats.cancelled_at_call;
            s_Applier->Release();
            CloseSession(p_Frame, "motion_link_lost", false);
            return;
        }
        if ((p_Frame.command_link_generation_now ? p_Frame.command_link_generation_now() : 0) != s_Session.command_generation)
        {
            ++m_Stats.cancelled_at_call;
            return s_Refuse("command_link_lost_at_call", "command_link_lost");
        }
        if (s_Applier->Latched())
            return s_Refuse("effectful_latched");

        // The one call (EngineTransformApplier::Invoke: the one SetObjectToWorldMatrixFromEditor site).
        const auto s_Call = s_Applier->Invoke(s_Requested);
        NoteInvocation(p_Frame.frame);
        ++m_Stats.invocations;
        s_Session.last_apply_ms = s_AtCall;
        s_Session.applied_once = true;
        const Reads s_Post = s_Applier->ReadBack();
        s_Applier->Release();
        s_Session.pending.reset();

        if (s_Call.faulted)
        {
            s_Session.uncertain = true;
            Ack(p_Frame, s_Target, Outcome::Uncertain, "setter_fault", std::nullopt, false, s_AtCall);
            return CloseSession(p_Frame, "uncertain:setter_fault", true);
        }
        if (!s_Call.called)
        {
            // Proven non-application (the applier made no call): a refusal, never uncertain.
            Ack(p_Frame, s_Target, Outcome::Refused, "setter_unavailable", std::nullopt, std::nullopt, s_AtCall);
            return CloseSession(p_Frame, "refused:setter_unavailable", true);
        }

        const Snapshot& s_After = s_Post.candidate;
        const bool s_Readable = s_After.world.observed() && s_After.dirty.observed() && s_After.parent.observed();
        std::optional<std::pair<uint32_t, uint32_t>> s_PostXY;
        if (s_After.world.observed())
            s_PostXY = std::make_pair(s_After.world.value->w[k_WordX], s_After.world.value->w[k_WordY]);

        std::string s_Why;
        if (!s_Readable)
            s_Why = "read_back_unavailable";
        else if (*s_After.dirty.value)
            s_Why = "post_dirty";
        else if (!(s_Post.same_entity_as_before.observed() && *s_Post.same_entity_as_before.value))
            s_Why = "post_entity_changed";
        else if (!(s_Post.generation_fresh.observed() && *s_Post.generation_fresh.value == s_Session.generation))
            s_Why = "generation_changed_after_call";
        else if (*s_After.parent.value != s_Session.opening_parent)
            s_Why = "parent_changed_after_call";
        else if (const auto s_Control = TransformApply::ClassifyControl(s_Reads.hero, s_Post.hero); s_Control != TransformApply::Control::Unchanged)
            s_Why = std::string("hero_control_") + TransformApply::ControlText(s_Control);
        else if (*s_After.world.value != s_Requested)
            s_Why = "postimage_mismatch";

        if (s_Why.empty())
        {
            s_Session.ref_x = s_Target.x;
            s_Session.ref_y = s_Target.y;
            s_Session.last_applied_seq = s_Target.seq;
            Ack(p_Frame, s_Target, Outcome::Applied, std::string(), s_PostXY, true, s_AtCall);
            return;
        }

        // The first post-call result that is not the exact request: uncertain (possible effect).
        s_Session.uncertain = true;
        Ack(p_Frame, s_Target, Outcome::Uncertain, s_Why, s_PostXY, false, s_AtCall);
        CloseSession(p_Frame, "uncertain:" + s_Why, true);
    }

    void Controller::MaybeReport(const Frame& p_Frame)
    {
        SessionState& s_Session = *m_Session;
        if (!s_Session.first_target_seen || p_Frame.now_mono_ms < s_Session.next_report_ms)
            return;

        MotionProtocol::Report s_Report;
        s_Report.session_id = s_Session.session_id;
        s_Report.report_seq = ++s_Session.report_seq;
        s_Report.native_mono_ms = p_Frame.now_mono_ms;
        s_Report.frame = p_Frame.frame;
        s_Report.counts = s_Session.counts;
        s_Report.last_applied_seq = s_Session.last_applied_seq;

        // One report per frame at most: no catch-up burst after a slow frame.
        while (s_Session.next_report_ms <= p_Frame.now_mono_ms)
            s_Session.next_report_ms += m_Options.report_interval_ms;

        if (Control(MotionProtocol::EncodeReport(s_Report)))
            ++m_Stats.reports_sent;
        else
            CloseSession(p_Frame, "evidence_undeliverable", false);
    }

    void Controller::CloseSession(const Frame& p_Frame, const std::string& p_Reason, bool p_Send)
    {
        if (!m_Session)
            return;

        // A pending target is accounted before the close.
        if (m_Session->pending)
        {
            const Pending s_Pending = *m_Session->pending;
            m_Session->pending.reset();
            Ack(p_Frame, s_Pending, Outcome::Refused, "session_closed", std::nullopt, std::nullopt, p_Frame.now_mono_ms);
        }

        // The qualified final snapshot (read-only, this frame): a dirty, unreadable or unqualified one
        // is null and makes the close unclean — never a clean restoration boundary.
        const Reads s_Final = ReadOnly(p_Frame.inspector);
        std::optional<FloatWords::Words12> s_FinalWorld;
        if (TransformApply::IdentityFailures(s_Final).empty() && Readable(s_Final.candidate) && !*s_Final.candidate.dirty.value)
            s_FinalWorld = *s_Final.candidate.world.value;

        SessionState& s_Session = *m_Session;
        const bool s_Clean = !s_Session.uncertain && s_FinalWorld.has_value();

        bool s_Sent = false;
        if (p_Send)
        {
            MotionProtocol::Closed s_Closed;
            s_Closed.session_id = s_Session.session_id;
            s_Closed.reason = p_Reason;
            s_Closed.native_mono_ms = p_Frame.now_mono_ms;
            s_Closed.clean = s_Clean;
            if (s_FinalWorld)
                s_Closed.final_world = FloatWords::Encode(*s_FinalWorld);
            s_Closed.last_applied_seq = s_Session.last_applied_seq;
            s_Closed.applied = s_Session.counts.applied;
            s_Sent = Control(MotionProtocol::EncodeClosed(s_Closed));
        }

        TransformApply::MotionRecord s_Record;
        s_Record.session_id = s_Session.session_id;
        s_Record.open_command_id = s_Session.open_command_id;
        s_Record.command_link_generation = s_Session.command_generation;
        s_Record.mission_generation = s_Session.generation;
        s_Record.opening_world = s_Session.opening_world;
        s_Record.opening_local = s_Session.opening_local;
        s_Record.opening_parent = s_Session.opening_parent;
        if (s_FinalWorld)
            s_Record.final_world = *s_FinalWorld;
        s_Record.restorable = s_Clean;
        s_Record.invalidated = !s_Clean;

        RelayLog::Info(
            "motion: session {} closed on frame {} ({}); clean {}; final world {}; received {} applied {} skipped {} refused {} uncertain {}; last applied {}; ghost.closed {}",
            s_Session.session_id, p_Frame.frame, p_Reason, s_Clean, s_FinalWorld ? FloatWords::Encode(*s_FinalWorld) : "unqualified",
            s_Session.counts.received, s_Session.counts.applied, s_Session.counts.skipped, s_Session.counts.refused, s_Session.counts.uncertain,
            s_Session.last_applied_seq ? std::to_string(*s_Session.last_applied_seq) : "none", s_Sent ? "queued" : (p_Send ? "NOT queued" : "not sendable (link lost)")
        );

        m_Closed.push_back(s_Session.session_id);
        while (m_Closed.size() > m_Options.closed_history)
            m_Closed.pop_front();
        m_Record = s_Record;
        ++m_Stats.sessions_closed;
        m_Stats.last_session_close = p_Reason;
        m_Stats.last_close_clean = s_Clean;
        m_Session.reset();
    }
}
