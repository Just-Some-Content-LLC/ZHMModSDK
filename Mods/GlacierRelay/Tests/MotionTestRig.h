#pragma once

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "CommandJson.h"
#include "CommandProtocol.h"
#include "CommandQueue.h"
#include "FakeTransformApplier.h"
#include "MotionController.h"
#include "MotionLink.h"
#include "MotionProtocol.h"
#include "RelayLog.h"
#include "TransformApply.h"

// M7-B2 test support: the frame thread's world for the motion controller, without a socket or an
// engine. A FAKE in-memory motion link (IMotionLink: queued BEAM messages in, queued lines out), the
// FAKE transform applier of M6-F (scripted words; the candidate at the probe's shared FAKE shape, the
// hero beside it), a controlled native clock and frame counter, and builders for the two motion
// commands. Every value is scripted here and is never an observation of HITMAN.
namespace MotionTest
{
    using FloatWords::Words12;

    constexpr const char* k_Session = "5e55a0b1-0000-4000-8000-000000000001";
    constexpr const char* k_Session2 = "5e55a0b1-0000-4000-8000-000000000002";
    constexpr const char* k_Foreign = "f0e1d2c3-0000-4000-8000-0000000000ff";
    constexpr const char* k_Share = "5aa2e000-0000-4000-8000-000000000001";
    constexpr const char* k_OpenId = "c0ffee00-0000-4000-8000-000000000001";
    constexpr const char* k_RestoreId = "c0ffee00-0000-4000-8000-000000000002";
    constexpr const char* k_Scene = "assembly:/_PRO/Scenes/Missions/Paris/_Scene_FashionShowHit_01.entity";
    constexpr uint64_t k_MotionGen = 5;
    constexpr uint64_t k_CommandGen = 9;

    class FakeLink final : public IMotionLink
    {
    public:
        Binding binding{1, true, k_MotionGen, k_CommandGen};
        std::vector<Inbound> inbound;
        mutable uint64_t signals = 0;
        std::vector<std::string> control;
        std::vector<std::string> samples;
        size_t control_room = 1000; // lines the control queue still accepts
        size_t sample_room = 1000;
        std::vector<std::string> drops;

        Binding Current() const override { return binding; }
        std::vector<Inbound> Drain() override
        {
            std::vector<Inbound> s_Out;
            s_Out.swap(inbound);
            return s_Out;
        }
        uint64_t Signals() const override { return signals; }
        Queued SendControl(uint64_t p_Serial, std::string p_Line) override
        {
            if (!binding.bound || p_Serial != binding.serial)
                return Queued::NotBound;
            if (control_room == 0)
                return Queued::Full;
            --control_room;
            control.push_back(std::move(p_Line));
            return Queued::Queued;
        }
        Queued SendSample(uint64_t p_Serial, std::string p_Line) override
        {
            if (!binding.bound || p_Serial != binding.serial)
                return Queued::NotBound;
            if (sample_room == 0)
                return Queued::Full;
            --sample_room;
            samples.push_back(std::move(p_Line));
            return Queued::Queued;
        }
        void Drop(uint64_t p_Serial, const char* p_Reason) override
        {
            if (p_Serial == binding.serial)
                drops.push_back(p_Reason);
        }

        void Target(const std::string& p_Session, uint64_t p_Seq, uint32_t p_X, uint32_t p_Y, uint64_t p_At)
        {
            MotionProtocol::Message s_Message;
            s_Message.kind = MotionProtocol::Kind::Target;
            s_Message.id = p_Session;
            s_Message.seq = p_Seq;
            s_Message.x = p_X;
            s_Message.y = p_Y;
            inbound.push_back(Inbound{s_Message, p_At, binding.serial});
        }
        void Close(const std::string& p_Id, const std::string& p_Reason, uint64_t p_At, bool p_Share = false)
        {
            MotionProtocol::Message s_Message;
            s_Message.kind = p_Share ? MotionProtocol::Kind::ShareClose : MotionProtocol::Kind::Close;
            s_Message.id = p_Id;
            s_Message.reason = p_Reason;
            inbound.push_back(Inbound{s_Message, p_At, binding.serial});
            ++signals;
        }
    };

    // A line's field as text ("" when absent or null), for the checks.
    inline std::string Field(const std::string& p_Line, const char* p_Key)
    {
        const auto s_Parsed = CommandJson::Parse(std::string_view(p_Line).substr(0, p_Line.size() - (p_Line.empty() || p_Line.back() != '\n' ? 0 : 1)));
        if (!s_Parsed.ok)
            return "<unparsed>";
        const auto* s_Value = s_Parsed.value.Find(p_Key);
        if (!s_Value)
            return "";
        switch (s_Value->kind)
        {
            case CommandJson::Value::Kind::String: return s_Value->text;
            case CommandJson::Value::Kind::Bool: return s_Value->boolean ? "true" : "false";
            case CommandJson::Value::Kind::Number: return std::to_string(s_Value->integer);
            case CommandJson::Value::Kind::Null: return "";
            default: return "<other>";
        }
    }

    inline std::vector<std::string> OfKind(const std::vector<std::string>& p_Lines, const char* p_Kind)
    {
        std::vector<std::string> s_Out;
        for (const auto& s_Line : p_Lines)
            if (Field(s_Line, "kind") == p_Kind)
                s_Out.push_back(s_Line);
        return s_Out;
    }

    inline uint32_t Word(float p_Value) { return FloatWords::ToWord(p_Value); }
    inline float Value(uint32_t p_Word) { return FloatWords::ToFloat(p_Word); }

    // The default limits of BEAM's canonical open (design 15.3 / the authority's native limits).
    inline CommandProtocol::MotionLimits Limits()
    {
        CommandProtocol::MotionLimits s_Limits;
        s_Limits.duration_ms = 120000;
        s_Limits.half_extent_mm = 8000;
        s_Limits.max_speed_mmps = 2000;
        s_Limits.credit_ms = 100;
        s_Limits.min_apply_interval_ms = 40;
        s_Limits.silence_close_ms = 1000;
        return s_Limits;
    }

    struct Rig
    {
        FakeTransform::Applier applier;
        FakeLink link;
        Motion::Controller::Options options;
        std::optional<Motion::Controller> controller;
        TransformApply::Ledger m6;
        uint64_t now = 1000000;
        uint64_t frame = 5000;
        uint64_t command_gen = k_CommandGen;
        uint64_t epoch = 3;
        bool playing = true;
        std::string generation = FakeTransform::k_Gen1;
        std::string scene = k_Scene;
        uint32_t thread = 0;
        bool with_inspector = true;
        std::vector<CommandProtocol::Result> results;

        explicit Rig(bool p_Effectful = true, uint32_t p_MaxSessions = 1)
        {
            options.effectful = p_Effectful;
            options.max_sessions = p_MaxSessions;
            controller.emplace(options, &link);
            thread = GetCurrentThreadId();
        }

        Motion::Controller& C() { return *controller; }
        Words12 World() const { return applier.world.world; }
        uint32_t X0() const { return applier.world.world.w[9]; }
        uint32_t Y0() const { return applier.world.world.w[10]; }

        Motion::Frame Frame()
        {
            Motion::Frame s_Frame;
            s_Frame.frame = frame;
            s_Frame.now_mono_ms = now;
            s_Frame.clock = [this] { return now; };
            s_Frame.thread_id = thread;
            s_Frame.scene_available = true;
            s_Frame.scene.available = true;
            s_Frame.scene.scene_loaded = true;
            s_Frame.scene.loading_stage = 8;
            s_Frame.scene.scene_type = "mission";
            s_Frame.scene.scene_resource = scene;
            s_Frame.mission_playing = playing;
            s_Frame.mission_generation = generation;
            s_Frame.relay_transition_epoch = epoch;
            s_Frame.command_link_generation_now = [this] { return command_gen; };
            if (with_inspector)
                s_Frame.inspector = [this] { return applier.Current(false, applier.world.fresh_generation); };
            s_Frame.applier = options.effectful ? &applier : nullptr;
            return s_Frame;
        }

        // One frame: the clock advances by p_Ms first.
        void Step(uint64_t p_Ms = 16)
        {
            now += p_Ms;
            ++frame;
            C().Step(Frame());
        }

        CommandQueue::Pending Pending(CommandProtocol::Command p_Command)
        {
            CommandQueue::Pending s_Pending;
            s_Pending.command = std::move(p_Command);
            s_Pending.received_mono_ms = now;
            s_Pending.link_generation = k_CommandGen;
            s_Pending.at_receipt.mission_generation = generation;
            s_Pending.at_receipt.relay_transition_epoch = epoch;
            return s_Pending;
        }

        TransformApply::Context Context(const CommandQueue::Pending& p_Pending)
        {
            TransformApply::Context s_Context;
            s_Context.pending = &p_Pending;
            s_Context.frame = frame;
            s_Context.now_mono_ms = now;
            s_Context.thread_id = thread;
            s_Context.expected_thread_id = thread;
            s_Context.scene_available = true;
            s_Context.scene.available = true;
            s_Context.scene.scene_loaded = true;
            s_Context.scene.loading_stage = 8;
            s_Context.scene.scene_type = "mission";
            s_Context.scene.scene_resource = scene;
            s_Context.mission_playing = playing;
            s_Context.previous_frame_playing = playing;
            s_Context.mission_generation = generation;
            s_Context.link_generation = command_gen;
            s_Context.link_generation_now = [this] { return command_gen; };
            s_Context.relay_transition_epoch = epoch;
            s_Context.rise_frame = 1;
            s_Context.rise_mono_ms = 1;
            s_Context.scene_resource_same_as_rise = true;
            s_Context.contract_started_seen = generation;
            s_Context.rules = PreconditionEvaluator::Rules{0, 0};
            return s_Context;
        }

        CommandProtocol::Command SessionCommand(CommandProtocol::MotionSessionMode p_Mode, const std::string& p_Id, const std::string& p_SessionId)
        {
            CommandProtocol::Command s_Command;
            s_Command.command_id = p_Id;
            s_Command.link_generation = k_CommandGen;
            s_Command.ttl_ms = 5000;
            s_Command.name = "candidate.motion.session";
            s_Command.version = 1;
            s_Command.entry = CommandProtocol::Entry::CandidateMotionSession1;
            s_Command.mission_generation_precondition = generation;
            CommandProtocol::MotionSessionRequest s_Request;
            s_Request.mode = p_Mode;
            s_Request.session_id = p_SessionId;
            s_Request.expect_world = applier.world.world;
            s_Request.expect_local = applier.world.local;
            s_Request.expect_parent = applier.world.parent;
            if (p_Mode == CommandProtocol::MotionSessionMode::Open)
            {
                s_Request.motion_link_generation = k_MotionGen;
                s_Request.limits = Limits();
            }
            s_Command.motion = s_Request;
            return s_Command;
        }

        CommandProtocol::Result Run(const CommandProtocol::Command& p_Command)
        {
            const auto s_Pending = Pending(p_Command);
            const auto s_Context = Context(s_Pending);
            CommandProtocol::Result s_Result;
            s_Result.command_id = p_Command.command_id;
            s_Result.link_generation = k_CommandGen;
            s_Result.frame = frame;
            if (p_Command.entry == CommandProtocol::Entry::PlayerPoseShare1)
                C().Share(s_Context, [this] { return applier.Current(false, applier.world.fresh_generation); }, s_Result);
            else
                C().Session(s_Context, options.effectful ? &applier : nullptr, m6, [this] { return applier.Current(false, applier.world.fresh_generation); }, s_Result);
            results.push_back(s_Result);
            return s_Result;
        }

        CommandProtocol::Result Open(std::function<void(CommandProtocol::Command&)> p_Edit = nullptr, const std::string& p_SessionId = k_Session, const std::string& p_CommandId = k_OpenId)
        {
            auto s_Command = SessionCommand(CommandProtocol::MotionSessionMode::Open, p_CommandId, p_SessionId);
            if (p_Edit)
                p_Edit(s_Command);
            return Run(s_Command);
        }

        CommandProtocol::Result Restore(std::function<void(CommandProtocol::Command&)> p_Edit = nullptr, const std::string& p_SessionId = k_Session, const std::string& p_CommandId = k_RestoreId, const std::string& p_RestoreOf = k_OpenId)
        {
            auto s_Command = SessionCommand(CommandProtocol::MotionSessionMode::Restore, p_CommandId, p_SessionId);
            const auto& s_Record = C().Record();
            s_Command.motion->target_world = s_Record ? s_Record->opening_world : applier.world.world;
            s_Command.motion->restore_of = p_RestoreOf;
            if (p_Edit)
                p_Edit(s_Command);
            return Run(s_Command);
        }

        CommandProtocol::Result OpenShare(uint32_t p_Interval = 50, uint32_t p_Duration = 1000, const std::string& p_ShareId = k_Share)
        {
            CommandProtocol::Command s_Command;
            s_Command.command_id = "c0ffee00-0000-4000-8000-0000000000a1";
            s_Command.link_generation = k_CommandGen;
            s_Command.ttl_ms = 5000;
            s_Command.name = "player.pose.share";
            s_Command.version = 1;
            s_Command.entry = CommandProtocol::Entry::PlayerPoseShare1;
            s_Command.mission_generation_precondition = generation;
            s_Command.share = CommandProtocol::MotionShareRequest{p_ShareId, k_MotionGen, p_Duration, p_Interval};
            return Run(s_Command);
        }

        // The target words at a displacement (dx, dy) metres from the opening translation, as binary32.
        std::pair<uint32_t, uint32_t> At(double p_Dx, double p_Dy) const
        {
            return {Word(static_cast<float>(static_cast<double>(Value(X0Opening())) + p_Dx)), Word(static_cast<float>(static_cast<double>(Value(Y0Opening())) + p_Dy))};
        }

        uint32_t X0Opening() const { return opening_x; }
        uint32_t Y0Opening() const { return opening_y; }
        void Remember() { opening_x = X0(); opening_y = Y0(); }

        void Target(uint64_t p_Seq, std::pair<uint32_t, uint32_t> p_At, const std::string& p_Session = k_Session)
        {
            link.Target(p_Session, p_Seq, p_At.first, p_At.second, now);
        }

        std::vector<std::string> Acks() const { return OfKind(link.control, "ghost.applied"); }
        std::vector<std::string> Closeds() const { return OfKind(link.control, "ghost.closed"); }
        std::vector<std::string> Reports() const { return OfKind(link.control, "ghost.report"); }

        uint32_t opening_x = 0;
        uint32_t opening_y = 0;
    };

    // The RelayLog raw-writer seam (tests only; RelayLog::SetRawWriterForTests): every line is
    // accepted without a file, and a line containing the marker first runs the hook — to move the
    // controlled clock or queue a close DURING the checked pre-call write — and may report a failed
    // write. The hook runs under RelayLog's lock: it must not log.
    struct LogHook
    {
        static inline std::string marker;
        static inline std::function<void()> hook;
        static inline bool fail = false;
        static inline int hits = 0;
        static inline std::vector<std::string> lines;

        static RelayLog::RawWrite Write(const char* p_Data, uint32_t p_Size)
        {
            const std::string_view s_Line(p_Data, p_Size);
            RelayLog::RawWrite s_Write;
            s_Write.file_open = true;
            s_Write.succeeded = true;
            s_Write.written = p_Size;
            if (!marker.empty() && s_Line.find(marker) != std::string_view::npos)
            {
                ++hits;
                lines.emplace_back(s_Line);
                if (hook)
                    hook();
                if (fail)
                {
                    s_Write.succeeded = false;
                    s_Write.written = 0;
                    s_Write.error = 5;
                }
            }
            return s_Write;
        }

        struct Scope
        {
            RelayLog::RawWriter previous;
            Scope(const std::string& p_Marker, std::function<void()> p_Hook, bool p_Fail = false)
            {
                marker = p_Marker;
                hook = std::move(p_Hook);
                fail = p_Fail;
                hits = 0;
                lines.clear();
                previous = RelayLog::SetRawWriterForTests(&Write);
            }
            ~Scope()
            {
                RelayLog::SetRawWriterForTests(previous);
                marker.clear();
                hook = nullptr;
                fail = false;
            }
        };
    };

    inline std::string Observed(const CommandProtocol::Result& p_Result, const char* p_Name)
    {
        for (const auto& s_Entry : p_Result.observed)
            if (s_Entry.first == p_Name)
            {
                const auto& s_Value = s_Entry.second;
                if (s_Value.field_status && *s_Value.field_status != CommandProtocol::FieldStatus::Observed)
                    return std::string("<") + CommandProtocol::FieldStatusText(*s_Value.field_status) + ">";
                switch (s_Value.kind)
                {
                    case CommandProtocol::ObservedValue::Kind::Bool: return s_Value.boolean ? "true" : "false";
                    case CommandProtocol::ObservedValue::Kind::Integer: return std::to_string(s_Value.integer);
                    case CommandProtocol::ObservedValue::Kind::Text: return s_Value.text;
                    default: return "<null>";
                }
            }
        return "<missing>";
    }

    // The largest binary32 x above p_From whose distance from p_From (in double) is <= p_Limit, and
    // the next one, which exceeds it: the exact boundary pair for an inclusive rule.
    inline std::pair<float, float> Boundary(float p_From, double p_Limit)
    {
        float s_X = static_cast<float>(static_cast<double>(p_From) + p_Limit);
        while (static_cast<double>(s_X) - static_cast<double>(p_From) > p_Limit)
            s_X = std::nextafter(s_X, -INFINITY);
        while (static_cast<double>(std::nextafter(s_X, INFINITY)) - static_cast<double>(p_From) <= p_Limit)
            s_X = std::nextafter(s_X, INFINITY);
        return {s_X, std::nextafter(s_X, INFINITY)};
    }

    // The same pair below p_From (the smallest binary32 within p_Limit, and the next one down).
    inline std::pair<float, float> BoundaryDown(float p_From, double p_Limit)
    {
        float s_X = static_cast<float>(static_cast<double>(p_From) - p_Limit);
        while (static_cast<double>(p_From) - static_cast<double>(s_X) > p_Limit)
            s_X = std::nextafter(s_X, INFINITY);
        while (static_cast<double>(p_From) - static_cast<double>(std::nextafter(s_X, -INFINITY)) <= p_Limit)
            s_X = std::nextafter(s_X, -INFINITY);
        return {s_X, std::nextafter(s_X, -INFINITY)};
    }
}
