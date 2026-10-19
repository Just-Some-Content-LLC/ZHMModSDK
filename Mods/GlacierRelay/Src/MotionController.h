#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>

#include "CommandProtocol.h"
#include "ITransformApplier.h"
#include "MotionLink.h"
#include "SceneState.h"
#include "TransformApply.h"
#include "TransformObservation.h"

// The native side of the reviewed motion contract (M7-B2; glacier-relay
// docs/design/M7_NETWORK_GHOST.md sections 15.3-15.5 and 17, which governs): the frame thread's
// owner of the one share, the one destination session and the session's retained record.
// Engine-independent: it reads the engine only through the injected read-only inspector and the
// transform applier (the one SetObjectToWorldMatrixFromEditor call site, EngineTransformApplier::
// Invoke), only on the frame callback, re-resolving everything on each frame of use and releasing
// the applier's handles before returning. It never waits on, or writes to, a socket: it reads the
// motion link's values (IMotionLink) and queues lines for its thread.
//
// Time (section 17.1, on the NATIVE clock; BEAM's clock is never read or subtracted): the session's
// reference instant is the opening frame's native monotonic time; the duration deadline is
// opened + duration_ms; the target-silence deadline is the receipt time (stamped by the link thread
// with the same clock) of the last target accepted into the pending slot, or the opening time,
// plus silence_close_ms; every deadline expires AT its instant (>=). A timer is not involved: each
// frame first applies every deadline that has passed at the frame's processing time, before any
// message is processed and before any setter call, so a target received before a deadline and
// processed after it is never applied. Immediately before an invocation — after the potentially
// blocking checked log write — the clock, the cancellation signal, both link bindings and the
// latch are read again. That last check does not make the following engine call atomic, and a
// process fault inside the call is the existing fail-stop.
//
// Application (section 15.5; 17.4): one pending slot (a second target while one is pending is
// refused: BEAM keeps one in flight); at most one setter call per frame (across this controller and
// the M6 commands) and at least min_apply_interval_ms between calls; before each call, on that
// frame: the session open, the mission predicate, scene, generation, transition epoch, frame
// thread and both link bindings as at the open, the latch clear, the compiled candidate resolved
// and qualified (TransformApply::IdentityFailures), the same local hero, a readable clean snapshot
// with the opening parent and EVERY world word except Trans.x / Trans.y equal to the opening
// snapshot's, the current x / y equal to the reference (the last exact postimage, else the opening
// snapshot; an unexplained horizontal change is refused, never overwritten), the sequence beyond
// every earlier one, the target in the area (inclusive) and within max_speed x credit of the
// reference (inclusive; no exemption for the first target, a pause or a reconnect). A target whose
// words equal the reference — and therefore the frame's own read — is skipped_identical with no
// call. The first refusal ends application with no invocation; the first post-call result that is
// not the exact requested words (dirty, unreadable, another entity, generation, parent or hero
// control change) is uncertain and ends application. No interpolation, clamping, extrapolation,
// retry, extra credit, controller override or resumption; holding or closing stops Relay's calls
// and does not freeze the engine.
//
// Autonomous closes (no BEAM needed): duration, target silence, either link's loss, a transition
// (predicate, scene, generation, epoch), any refusal or uncertainty, BEAM's ghost.close, and an
// evidence line that cannot be queued (the link is then dropped). A close acknowledges a pending
// target (refused, session_closed), reads a qualified final snapshot (a dirty or unreadable one is
// reported as null and makes the close unclean) and sends ghost.closed. A ghost.close naming a
// closed session is ignored; a foreign one terminates nothing; nothing reopens a closed session.
//
// Exclusion and budgets (section 15.4, mirrored natively; never reset in the process): an M6
// transform command (v1 / v2, forward or restore) is refused while a session is open or its record
// is retained unreleased; a motion open is refused while a session is open or retained, while an
// M6 forward consumed its budget without a confirmed restoration, or once max_sessions sessions
// opened. A retained record is released only by a restoration that proved every same-frame fact
// of section 17.2, or — when the record closed clean and was never invoked upon by a restore — by
// an observation on an applying frame that the candidate is already at the session's opening
// preimage (no write; this is how BEAM's confirm_baseline, a journal release, meets the native
// record without a release command). An invalidated record (an unclean close, an attempted
// restoration without proof) is never released in the process.
namespace Motion
{
    // What the controller needs on each frame, as plain values and callbacks.
    struct Frame
    {
        uint64_t frame = 0;
        uint64_t now_mono_ms = 0;
        std::function<uint64_t()> clock;  // re-read immediately before an invocation (null: now_mono_ms)
        uint32_t thread_id = 0;
        bool scene_available = false;
        SceneState scene;
        bool mission_playing = false;
        std::optional<std::string> mission_generation; // edge-cached
        uint64_t relay_transition_epoch = 0;
        std::function<uint64_t()> command_link_generation_now; // the command link's current generation (0: none)
        std::function<TransformObservation::Reads()> inspector; // read-only (samples, final snapshots, baseline checks)
        TransformApply::ITransformApplier* applier = nullptr;   // null unless [command] effectful
    };

    class Controller
    {
    public:
        struct Options
        {
            bool effectful = false;     // [command] effectful: the session entry may apply (the applier exists)
            uint32_t max_sessions = 1;  // [motion] max_sessions (1..4): sessions per process, never reset
            uint32_t report_interval_ms = 250;
            size_t closed_history = 16; // closed session ids remembered (a close naming one is harmless)
        };

        struct Stats
        {
            // share
            uint64_t shares_opened = 0;
            uint64_t shares_closed = 0;
            uint64_t samples_taken = 0;       // sampling ticks (a sequence number each)
            uint64_t samples_queued = 0;      // hero.sample lines queued on the link ("sent")
            uint64_t samples_dropped = 0;     // refused by the full sample queue (newest refused)
            uint64_t samples_unavailable = 0; // no readable hero words: no line (a gap at the sink)
            uint64_t samples_dirty = 0;       // queued with dirty: true
            // session
            uint64_t sessions_opened = 0;
            uint64_t sessions_closed = 0;
            uint64_t targets_received = 0;
            uint64_t applied = 0;
            uint64_t skipped = 0;
            uint64_t refused = 0;
            uint64_t uncertain = 0;
            uint64_t invocations = 0;         // setter calls made by the session
            uint64_t reports_sent = 0;
            uint64_t foreign_targets = 0;     // a target naming no open session of this controller
            uint64_t late_targets = 0;        // a target naming a closed session (never applied, never reopening)
            uint64_t stale_inbound = 0;       // a message of an earlier binding (discarded)
            uint64_t closes_for_closed = 0;   // a ghost.close naming a closed session (harmless)
            uint64_t foreign_closes = 0;      // a close naming no session of this controller
            uint64_t control_full = 0;        // a required evidence line that could not be queued (link dropped)
            uint64_t deadline_at_call = 0;    // a target refused by the re-check after the pre-call log
            uint64_t cancelled_at_call = 0;   // a close or link loss seen by that re-check
            uint64_t late_stamped_targets = 0; // a target stamped at or after a deadline (never accepted; verifier F1)
            uint64_t evidence_capacity = 0;   // a call not made because its evidence could not be queued (verifier F5)
            // records, exclusion, restoration
            uint64_t m6_held = 0;             // M6 transform commands refused by the exclusion
            uint64_t released_at_baseline = 0;
            uint64_t restores_dispatched = 0;
            uint64_t restores_invoked = 0;
            uint64_t restores_proven = 0;
            uint64_t last_invocation_frame = 0;
            uint64_t two_writes_prevented = 0; // a pending target deferred because a setter call already happened on this frame
            std::string last_share_close;
            std::string last_session_close;
            bool last_close_clean = false;
        };

        Controller(Options p_Options, IMotionLink* p_Link);

        // ---- commands (the scheduler, on the frame that took them) ----

        // player.pose.share/1 (inert): opens the read-only sampling of the local hero.
        void Share(const TransformApply::Context& p_Context, const std::function<TransformObservation::Reads()>& p_Inspector, CommandProtocol::Result& p_Result);

        // candidate.motion.session/1: open (admission only; no setter call) or restore (the one call
        // through TransformApply::ApplyMotionRestore).
        void Session(const TransformApply::Context& p_Context, TransformApply::ITransformApplier* p_Applier, const TransformApply::Ledger& p_M6, const std::function<TransformObservation::Reads()>& p_Inspector, CommandProtocol::Result& p_Result);

        // The M6 exclusion on this frame: true when a session holds the candidate (open, or retained
        // and not at baseline on this frame's read — a record at baseline is released here, logged).
        bool HoldsTransform(uint64_t p_Frame, const std::function<TransformObservation::Reads()>& p_Inspector);

        // A setter call made on this frame by someone else (the M6 transform path).
        void NoteInvocation(uint64_t p_Frame);
        bool InvokedOnFrame(uint64_t p_Frame) const { return m_Stats.last_invocation_frame == p_Frame && m_InvokedOnce; }

        // ---- every frame (after the command scheduler) ----
        void Step(const Frame& p_Frame);

        // ---- reads for the probe, the tests and the logs ----
        Stats GetStats() const { return m_Stats; }
        bool ShareOpen() const { return m_Share.has_value(); }
        bool SessionOpen() const { return m_Session.has_value(); }
        const std::optional<TransformApply::MotionRecord>& Record() const { return m_Record; }
        uint32_t SessionsRemaining() const { return m_Stats.sessions_opened >= m_Options.max_sessions ? 0 : static_cast<uint32_t>(m_Options.max_sessions - m_Stats.sessions_opened); }

    private:
        struct ShareState
        {
            std::string share_id;
            uint64_t serial = 0;
            uint64_t motion_generation = 0;
            uint64_t command_generation = 0;
            uint32_t interval_ms = 0;
            uint32_t duration_ms = 0;
            uint64_t opened_ms = 0;
            uint64_t next_sample_ms = 0;
            std::string hero_entity_id;
            std::string generation;
            std::string scene_resource;
            uint64_t epoch = 0;
            uint64_t seq = 0;
            uint64_t sent = 0;
            std::optional<uint64_t> last_seq;
        };

        struct Pending
        {
            uint64_t seq = 0;
            uint32_t x = 0;
            uint32_t y = 0;
            uint64_t received_ms = 0;
        };

        struct SessionState
        {
            std::string session_id;
            std::string open_command_id;
            uint64_t serial = 0;
            uint64_t motion_generation = 0;
            uint64_t command_generation = 0;
            CommandProtocol::MotionLimits limits;
            uint64_t opened_ms = 0;
            uint64_t silence_ref_ms = 0;
            uint64_t last_apply_ms = 0;
            bool applied_once = false;
            std::string generation;
            std::string scene_resource;
            uint64_t epoch = 0;
            uint32_t thread_id = 0;
            std::string hero_entity_id;
            FloatWords::Words12 opening_world;
            FloatWords::Words7 opening_local;
            std::string opening_parent;
            uint32_t ref_x = 0; // the reference: the last exact postimage, else the opening snapshot
            uint32_t ref_y = 0;
            uint64_t last_received_seq = 0;
            std::optional<uint64_t> last_applied_seq;
            std::optional<Pending> pending;
            bool first_target_seen = false;
            uint64_t next_report_ms = 0;
            uint64_t report_seq = 0;
            bool uncertain = false;
            MotionProtocol::Counts counts;
        };

        void Dispatch(const Frame& p_Frame, const std::vector<IMotionLink::Inbound>& p_Inbound);
        void StepShare(const Frame& p_Frame);
        void StepSession(const Frame& p_Frame, uint64_t p_SignalsAtDrain);
        void ApplyPending(const Frame& p_Frame, uint64_t p_SignalsAtDrain);
        void MaybeReport(const Frame& p_Frame);
        void CloseShare(const char* p_Why, const std::string& p_Detail, bool p_Send);
        void CloseSession(const Frame& p_Frame, const std::string& p_Reason, bool p_Send);
        void Ack(const Frame& p_Frame, const Pending& p_Target, MotionProtocol::Outcome p_Outcome, const std::string& p_Reason, std::optional<std::pair<uint32_t, uint32_t>> p_Post, std::optional<bool> p_Exact, uint64_t p_NativeMs);
        bool Control(const std::string& p_Line);
        void Condemn(uint64_t p_Serial, const char* p_Why);
        void OpenSession(const TransformApply::Context& p_Context, TransformApply::ITransformApplier* p_Applier, const TransformApply::Ledger& p_M6, const std::function<TransformObservation::Reads()>& p_Inspector, CommandProtocol::Result& p_Result);
        void RestoreSession(const TransformApply::Context& p_Context, TransformApply::ITransformApplier* p_Applier, CommandProtocol::Result& p_Result);
        bool RecordAtBaseline(const TransformObservation::Reads& p_Reads) const;
        bool RecordBlocks(uint64_t p_Frame, const TransformObservation::Reads* p_Reads, const char* p_ForWhat);
        bool KnownSession(const std::string& p_Id) const;

        Options m_Options;
        IMotionLink* m_Link;
        Stats m_Stats;
        bool m_InvokedOnce = false;
        bool m_EvidenceLost = false;     // a required control line of the open session could not be queued
        uint64_t m_CondemnedSerial = 0;  // a binding whose drop this controller requested: nothing more happens on it (verifier F2)
        std::optional<ShareState> m_Share;
        std::optional<SessionState> m_Session;
        std::optional<TransformApply::MotionRecord> m_Record; // the retained record of the last session, until released
        std::deque<std::string> m_Closed;                      // closed session ids (bounded)
        std::deque<std::string> m_ClosedShares;                // closed share ids (bounded)
    };
}
