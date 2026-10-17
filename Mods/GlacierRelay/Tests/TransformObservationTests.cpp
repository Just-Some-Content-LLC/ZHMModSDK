// M6-F tests: the float-word codec (round trips, signed zero, malformed words and counts, non-finite
// values, the seven-digit twin pair), the compiled lift target, the exact-snapshot inspection
// encoding against the actual line bound, the protocol's decoding of the two new entries, and the
// post-call classifiers with the declared tolerance. No engine pointer is constructed.
#include "TestHarness.h"

#include <cmath>
#include <string>

#include "CommandProtocol.h"
#include "CommandScheduler.h"
#include "FakeTransformApplier.h"
#include "FloatWords.h"
#include "TransformApply.h"
#include "TransformObservation.h"

using namespace CommandProtocol;
using FloatWords::Words12;
using FloatWords::Words7;

namespace
{
    bool Contains(const std::string& p_Text, const std::string& p_Needle)
    {
        return p_Text.find(p_Needle) != std::string::npos;
    }

    const ObservedValue* Observed(const Result& p_Result, const char* p_Key)
    {
        for (const auto& s_Entry : p_Result.observed)
            if (s_Entry.first == p_Key)
                return &s_Entry.second;
        return nullptr;
    }

    std::string TextField(const Result& p_Result, const char* p_Key)
    {
        const auto* s_Value = Observed(p_Result, p_Key);
        return s_Value && s_Value->kind == ObservedValue::Kind::Text && s_Value->field_status && *s_Value->field_status == FieldStatus::Observed ? s_Value->text : std::string();
    }

    std::string Line(const std::string& p_Params, const std::string& p_Preconditions, const char* p_Name, int p_Version)
    {
        return std::string("{\"command_protocol_version\":2,\"kind\":\"command\",\"command_id\":\"4e2d1b6a-1e2f-4a5b-8c6d-0f1e2d3c4b5a\",\"link_generation\":1,\"issued_at\":\"t\",\"ttl_ms\":5000,\"name\":\"")
            + p_Name + "\",\"version\":" + std::to_string(p_Version) + ",\"params\":" + p_Params + ",\"preconditions\":" + p_Preconditions + "}";
    }
}

void RunTransformObservationTests()
{
    // ---- the codec: bit copies, canonical text, exact validation -----------------------------------
    {
        CHECK(FloatWords::ToWord(-1.539356f) == 0xbfc5099eu);
        CHECK(FloatWords::Hex8(FloatWords::ToWord(-1.539356f)) == "bfc5099e");
        CHECK(FloatWords::ToFloat(0xbfc5099eu) == -1.539356f);
        CHECK(FloatWords::ToWord(0.f) == 0x00000000u && FloatWords::ToWord(-0.f) == 0x80000000u);
        CHECK(FloatWords::Finite(0x00000000u) && FloatWords::Finite(0x80000000u));
        CHECK(!FloatWords::Finite(0x7f800000u) && !FloatWords::Finite(0xff800000u) && !FloatWords::Finite(0x7fc00000u));
        CHECK(FloatWords::Finite(0x7f7fffffu)); // FLT_MAX

        // The seven-digit twins: the same rendering, different bits, different states.
        const uint32_t s_A = 0xbfc5099eu, s_B = 0xbfc5099fu;
        CHECK(FloatWords::Number7(FloatWords::ToFloat(s_A)) == "-1.539356" && FloatWords::Number7(FloatWords::ToFloat(s_B)) == "-1.539356");
        CHECK(s_A != s_B && FloatWords::ToFloat(s_A) != FloatWords::ToFloat(s_B));
        CHECK(FloatWords::Number7(0.25f) == "0.25" && FloatWords::Number7(1.0e10f) == "1e+10" && FloatWords::Number7(-0.f) == "-0");

        // Round trips of every word kind through the text form.
        const Words12 s_World = FakeTransform::WorldOf(-83.74377f, 14.69725f, -1.522178f);
        const std::string s_Text = FloatWords::Encode(s_World);
        CHECK(s_Text.size() == FloatWords::k_WorldTextBytes && s_Text.size() <= k_MaxObservedTextBytes);
        CHECK(s_Text == "3f800000,00000000,00000000,00000000,3f800000,00000000,00000000,00000000,3f800000,c2a77ccf,416b27f0,bfc2d6bb");
        Words12 s_Back;
        std::string s_Why;
        CHECK(FloatWords::Decode(s_Text, s_Back, s_Why) && s_Back == s_World);
        const Words7 s_Local = FakeTransform::LocalOf(0.f, -0.f, -1.539356f);
        const std::string s_LocalText = FloatWords::Encode(s_Local);
        CHECK(s_LocalText.size() == FloatWords::k_LocalTextBytes && s_LocalText == "00000000,80000000,bfc5099e,00000000,00000000,00000000,3f800000");
        Words7 s_LocalBack;
        CHECK(FloatWords::Decode(s_LocalText, s_LocalBack, s_Why) && s_LocalBack == s_Local && s_LocalBack.w[1] == 0x80000000u);

        // Malformed: counts, separators, uppercase, non-hex, non-finite, empty.
        Words12 s_Out;
        CHECK(!FloatWords::Decode("", s_Out, s_Why) && Contains(s_Why, "expected 12 words"));
        CHECK(!FloatWords::Decode(s_LocalText, s_Out, s_Why) && Contains(s_Why, "expected 12 words"));
        CHECK(!FloatWords::Decode(s_Text + ",3f800000", s_Out, s_Why));
        std::string s_Upper = s_Text; s_Upper[0] = '3'; s_Upper[1] = 'F';
        CHECK(!FloatWords::Decode(s_Upper, s_Out, s_Why) && Contains(s_Why, "not eight lowercase hex digits"));
        std::string s_Bad = s_Text; s_Bad[8] = ';';
        CHECK(!FloatWords::Decode(s_Bad, s_Out, s_Why) && Contains(s_Why, "not separated by a comma"));
        std::string s_Inf = s_Text; s_Inf.replace(0, 8, "7f800000");
        CHECK(!FloatWords::Decode(s_Inf, s_Out, s_Why) && Contains(s_Why, "word 0 is not finite"));
        std::string s_Nan = s_Text; s_Nan.replace(99, 8, "7fc00001");
        CHECK(!FloatWords::Decode(s_Nan, s_Out, s_Why) && Contains(s_Why, "word 11 is not finite"));
        CHECK(s_Out == Words12()); // untouched on every failure

        // The tolerance: equality within 1e-4 per word, never across it, never for non-finite.
        Words12 s_Near = s_World;
        s_Near.w[11] = FloatWords::ToWord(-1.522178f + 5.0e-5f);
        CHECK(FloatWords::WithinTolerance(s_World, s_Near, TransformObservation::k_DriftToleranceM));
        Words12 s_Far = s_World;
        s_Far.w[11] = FloatWords::ToWord(-1.522178f + 2.0e-4f);
        CHECK(!FloatWords::WithinTolerance(s_World, s_Far, TransformObservation::k_DriftToleranceM));
        Words12 s_NaN = s_World;
        s_NaN.w[0] = 0x7fc00000u;
        CHECK(!FloatWords::WithinTolerance(s_World, s_NaN, 1.f));
        CHECK(TransformObservation::k_DriftToleranceM == 1.0e-4f);
    }

    // ---- the compiled lift target: every other word copied; z + 0.25f in binary32 ------------------
    {
        const Words12 s_Pre = FakeTransform::WorldOf(0.f, 0.f, -1.539356f);
        const Words12 s_Target = TransformObservation::LiftTarget(s_Pre);
        for (size_t i = 0; i < 11; ++i)
            CHECK(s_Target.w[i] == s_Pre.w[i]);
        CHECK(FloatWords::ToFloat(s_Target.w[11]) == -1.539356f + 0.25f);
        CHECK(FloatWords::Number7(FloatWords::ToFloat(s_Target.w[11])) == "-1.289356");
        CHECK(s_Target != s_Pre && TransformObservation::k_LiftZ == 0.25f);
        // A translation so large that the binary32 addition does not move it: the gate refuses it as
        // target_equals_current (tested in TransformApplyTests); the target itself is still finite.
        const Words12 s_Huge = FakeTransform::WorldOf(0.f, 0.f, 3.0e8f);
        CHECK(TransformObservation::LiftTarget(s_Huge) == s_Huge);
    }

    // ---- the inspection encoding: 35 entries with the frame values; the line fits; the texts fit ----
    {
        FakeTransform::Applier s_Fake;
        const TransformObservation::Reads s_Reads = s_Fake.Current(false, FakeTransform::k_Gen1);

        Result s_Result;
        s_Result.protocol_version = 2;
        s_Result.command_id = "4e2d1b6a-1e2f-4a5b-8c6d-0f1e2d3c4b5a";
        s_Result.link_generation = 1;
        s_Result.adapter_instance_id = "aaf540dd-c6df-4429-8fa5-84bc8933a13e";
        s_Result.native_at = "2026-10-17T00:00:00.000000Z";
        s_Result.frame = 51985;
        s_Result.mission_generation = FakeTransform::k_Gen1;
        s_Result.status = Status::Applied;
        TransformObservation::AppendInspection(s_Result.observed, s_Reads);
        CHECK(s_Result.observed.size() == 31);
        const std::string s_Line = EncodeResult(s_Result);
        std::printf("line bound: candidate_transform inspection = %zu bytes of %zu (%zu entries + 4 frame values)\n", s_Line.size(), k_MaxLineBytes, s_Result.observed.size());
        CHECK(s_Line.size() <= k_MaxLineBytes);
        CHECK(TextField(s_Result, "cand_world_words") == FloatWords::Encode(s_Fake.world.world));
        CHECK(TextField(s_Result, "cand_local_words") == FloatWords::Encode(s_Fake.world.local));
        CHECK(TextField(s_Result, "cand_world") == "0,0,-1.539356" && TextField(s_Result, "cand_local") == "0,0,-1.539356|0,0,0,1");
        CHECK(TextField(s_Result, "hero_world_words") == FloatWords::Encode(s_Fake.world.hero_world));
        CHECK(TextField(s_Result, "targets") == std::string(TransformObservation::k_Brick) + "," + TransformObservation::k_Entity);
        CHECK(TextField(s_Result, "registry_slots") == FakeTransform::k_LocalOnly);
        const auto* s_Version = Observed(s_Result, "snapshot_version");
        CHECK(s_Version && s_Version->kind == ObservedValue::Kind::Integer && s_Version->integer == 1);
        for (const auto& s_Entry : s_Result.observed)
            if (s_Entry.second.kind == ObservedValue::Kind::Text)
                CHECK(s_Entry.second.text.size() <= k_MaxObservedTextBytes);

        // The whole thing through the scheduler: the four frame values, reads_on_frame, 35 entries, within the bound.
        CommandQueue s_Queue;
        Result s_Emitted;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Emitted = p_Result; });
        const auto s_Decoded = DecodeCommand(Line("{}", "{}", "relay.inspect.candidate_transform", 1), DecodeOptions{2, false});
        CHECK(s_Decoded.ok && s_Decoded.command.entry == Entry::RelayInspectCandidateTransform1);
        s_Queue.Push(s_Decoded.command, 1, 1000);
        CommandScheduler::FrameObservation s_Frame;
        s_Frame.frame = 7;
        s_Frame.now_mono_ms = 1100;
        s_Frame.thread_id = GetCurrentThreadId();
        s_Frame.link_generation = 1;
        s_Frame.scene_available = true;
        s_Frame.scene.scene_type = "mission";
        s_Frame.mission_playing = true;
        s_Frame.transform_inspector = [&] { return s_Fake.Current(false, FakeTransform::k_Gen1); };
        CHECK(s_Scheduler.Tick(s_Frame));
        CHECK(s_Emitted.status == Status::Applied && s_Emitted.observed.size() == 35 && s_Emitted.observed.size() <= k_MaxObservedEntries);
        CHECK(TextField(s_Emitted, "cand_world_words") == FloatWords::Encode(s_Fake.world.world));
        CHECK(EncodeResult(s_Emitted).size() <= k_MaxLineBytes);
        CHECK(s_Scheduler.GetStats().transforms_inspected == 1 && s_Scheduler.GetStats().reduced == 0);

        // Without an inspector: every engine field unsupported, nothing guessed, still bounded.
        CommandQueue s_Queue2;
        Result s_Emitted2;
        CommandScheduler s_Scheduler2(s_Queue2, [&](const Result& p_Result) { s_Emitted2 = p_Result; });
        s_Queue2.Push(s_Decoded.command, 1, 1000);
        CommandScheduler::FrameObservation s_Bare = s_Frame;
        s_Bare.transform_inspector = nullptr;
        CHECK(s_Scheduler2.Tick(s_Bare));
        CHECK(s_Emitted2.status == Status::Applied && Contains(s_Emitted2.reason, "unsupported"));
        const auto* s_Words = Observed(s_Emitted2, "cand_world_words");
        CHECK(s_Words && s_Words->field_status && *s_Words->field_status == FieldStatus::Unsupported && s_Words->kind == ObservedValue::Kind::Null);
        CHECK(EncodeResult(s_Emitted2).size() <= k_MaxLineBytes);
    }

    // ---- the protocol: the two entries, their parameters and their refusals ----------------------
    {
        CHECK(Lookup("relay.inspect.candidate_transform", 1) == Entry::RelayInspectCandidateTransform1 && !IsEffectful(Entry::RelayInspectCandidateTransform1));
        CHECK(Lookup("candidate.transform.set", 1) == Entry::CandidateTransformSet1 && IsEffectful(Entry::CandidateTransformSet1));
        CHECK(Lookup("candidate.transform.set", 2) == Entry::Unknown);

        DecodeOptions s_V2Eff;
        s_V2Eff.protocol_version = 2;
        s_V2Eff.effectful = true;
        DecodeOptions s_V2Inert;
        s_V2Inert.protocol_version = 2;
        DecodeOptions s_V1;

        const auto s_Inspect = DecodeCommand(Line("{}", "{}", "relay.inspect.candidate_transform", 1), s_V2Inert);
        CHECK(s_Inspect.ok && s_Inspect.command.params.empty());
        const auto s_InspectParam = DecodeCommand(Line("{\"entity_id\":\"feede715906f747f\"}", "{}", "relay.inspect.candidate_transform", 1), s_V2Inert);
        CHECK(!s_InspectParam.ok && Contains(s_InspectParam.reason, "unknown parameter: entity_id"));
        const auto s_InspectV1 = DecodeCommand(Line("{}", "{}", "relay.inspect.candidate_transform", 1).replace(28, 1, "1"), s_V1);
        CHECK(s_InspectV1.ok); // inert on a version-1 link too

        const std::string s_World = FloatWords::Encode(FakeTransform::WorldOf(0.f, 0.f, -1.539356f));
        const std::string s_Local = FloatWords::Encode(FakeTransform::LocalOf(0.f, 0.f, -1.539356f));
        const std::string s_Target = FloatWords::Encode(TransformObservation::LiftTarget(FakeTransform::WorldOf(0.f, 0.f, -1.539356f)));
        const std::string s_Pre = "{\"mission_generation\":\"" + std::string(FakeTransform::k_Gen1) + "\"}";
        const std::string s_Lift = "{\"mode\":\"lift\",\"expect_world\":\"" + s_World + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\"}";
        const std::string s_Restore = "{\"mode\":\"restore\",\"expect_world\":\"" + s_Target + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\",\"target_world\":\"" + s_World + "\",\"restore_of\":\"9c8f3a75-7054-457c-919d-ee87e77f57d9\"}";

        const auto s_Ok = DecodeCommand(Line(s_Lift, s_Pre, "candidate.transform.set", 1), s_V2Eff);
        CHECK(s_Ok.ok && s_Ok.command.entry == Entry::CandidateTransformSet1 && s_Ok.command.transform && s_Ok.command.transform->mode == TransformMode::Lift);
        CHECK(s_Ok.ok && FloatWords::Encode(s_Ok.command.transform->expect_world) == s_World && FloatWords::Encode(s_Ok.command.transform->expect_local) == s_Local && s_Ok.command.transform->expect_parent == "007c57a6ea581f8c");
        CHECK(s_Ok.ok && !s_Ok.command.transform->target_world && !s_Ok.command.transform->restore_of);
        CHECK(s_Ok.ok && s_Ok.command.params.size() == 1 && s_Ok.command.params[0].first == "mode" && s_Ok.command.params[0].second == "lift");

        const auto s_RestoreOk = DecodeCommand(Line(s_Restore, s_Pre, "candidate.transform.set", 1), s_V2Eff);
        CHECK(s_RestoreOk.ok && s_RestoreOk.command.transform->mode == TransformMode::Restore && s_RestoreOk.command.transform->target_world && FloatWords::Encode(*s_RestoreOk.command.transform->target_world) == s_World && *s_RestoreOk.command.transform->restore_of == "9c8f3a75-7054-457c-919d-ee87e77f57d9");

        auto s_Reject = [&](const std::string& p_Params, const std::string& p_Preconditions, const DecodeOptions& p_Options, const char* p_Needle) {
            const auto s_Decoded = DecodeCommand(Line(p_Params, p_Preconditions, "candidate.transform.set", 1), p_Options);
            const bool s_Ok = !s_Decoded.ok && s_Decoded.command_id == "4e2d1b6a-1e2f-4a5b-8c6d-0f1e2d3c4b5a" && Contains(s_Decoded.reason, p_Needle);
            if (!s_Ok)
                std::printf("  transform decode: expected '%s', got ok=%d reason '%s'\n", p_Needle, s_Decoded.ok, s_Decoded.reason.c_str());
            CHECK(s_Ok);
        };
        s_Reject(s_Lift, s_Pre, s_V2Inert, "effectful setting disabled");
        s_Reject(s_Lift, "{}", s_V2Eff, "missing precondition: mission_generation");
        s_Reject("{\"mode\":\"teleport\",\"expect_world\":\"" + s_World + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\"}", s_Pre, s_V2Eff, "invalid parameter: mode");
        s_Reject("{\"expect_world\":\"" + s_World + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\"}", s_Pre, s_V2Eff, "invalid parameter: mode");
        s_Reject("{\"mode\":\"lift\",\"expect_world\":\"" + s_Local + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\"}", s_Pre, s_V2Eff, "invalid parameter: expect_world (expected 12 words");
        s_Reject("{\"mode\":\"lift\",\"expect_world\":\"" + s_World + "\",\"expect_local\":\"" + s_World + "\",\"expect_parent\":\"007c57a6ea581f8c\"}", s_Pre, s_V2Eff, "invalid parameter: expect_local (expected 7 words");
        std::string s_InfWorld = s_World; s_InfWorld.replace(99, 8, "ff800000");
        s_Reject("{\"mode\":\"lift\",\"expect_world\":\"" + s_InfWorld + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\"}", s_Pre, s_V2Eff, "word 11 is not finite");
        s_Reject("{\"mode\":\"lift\",\"expect_world\":\"" + s_World + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007C57A6EA581F8C\"}", s_Pre, s_V2Eff, "invalid parameter: expect_parent");
        s_Reject("{\"mode\":\"lift\",\"expect_world\":\"" + s_World + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\",\"target_world\":\"" + s_Target + "\"}", s_Pre, s_V2Eff, "a lift carries no target_world or restore_of");
        s_Reject("{\"mode\":\"lift\",\"expect_world\":\"" + s_World + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\",\"entity_id\":\"feede715906f747f\"}", s_Pre, s_V2Eff, "unknown parameter: entity_id");
        s_Reject("{\"mode\":\"restore\",\"expect_world\":\"" + s_Target + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\",\"restore_of\":\"9c8f3a75-7054-457c-919d-ee87e77f57d9\"}", s_Pre, s_V2Eff, "invalid parameter: target_world (missing)");
        s_Reject("{\"mode\":\"restore\",\"expect_world\":\"" + s_Target + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\",\"target_world\":\"" + s_World + "\"}", s_Pre, s_V2Eff, "invalid parameter: restore_of");
        s_Reject("{\"mode\":\"restore\",\"expect_world\":\"" + s_World + "\",\"expect_local\":\"" + s_Local + "\",\"expect_parent\":\"007c57a6ea581f8c\",\"target_world\":\"" + s_World + "\",\"restore_of\":\"9c8f3a75-7054-457c-919d-ee87e77f57d9\"}", s_Pre, s_V2Eff, "already at the restore target");
        // The version-1 link never decodes the effectful entry.
        const auto s_V1Line = DecodeCommand(Line(s_Lift, s_Pre, "candidate.transform.set", 1).replace(28, 1, "1"), s_V1);
        CHECK(!s_V1Line.ok && Contains(s_V1Line.reason, "effectful setting disabled"));
    }

    // ---- the post-call classifiers ----------------------------------------------------------------
    {
        using TransformApply::ClassifyControl;
        using TransformApply::ClassifyLocal;
        using TransformApply::ClassifyWorld;
        using TransformApply::Control;
        using TransformApply::LocalMatch;
        using TransformApply::WorldMatch;
        using OutfitObservation::Field;

        const Words12 s_Pre = FakeTransform::WorldOf(0.f, 0.f, -1.539356f);
        const Words12 s_Target = TransformObservation::LiftTarget(s_Pre);
        TransformObservation::Snapshot s_Post;
        s_Post.world = Field<Words12>::Observed(s_Target);
        s_Post.dirty = Field<bool>::Observed(false);
        CHECK(ClassifyWorld(s_Post, s_Pre, s_Target) == WorldMatch::Target);
        s_Post.world = Field<Words12>::Observed(s_Pre);
        CHECK(ClassifyWorld(s_Post, s_Pre, s_Target) == WorldMatch::Preimage);
        Words12 s_Twin = s_Target;
        s_Twin.w[11] = 0xbfa5099fu; // "-1.289356" too, one bit away: neither
        CHECK(FloatWords::Number7(FloatWords::ToFloat(s_Twin.w[11])) == FloatWords::Number7(FloatWords::ToFloat(s_Target.w[11])));
        s_Post.world = Field<Words12>::Observed(s_Twin);
        CHECK(ClassifyWorld(s_Post, s_Pre, s_Target) == WorldMatch::Neither);
        s_Post.world = Field<Words12>::Observed(s_Target);
        s_Post.dirty = Field<bool>::Observed(true);
        CHECK(ClassifyWorld(s_Post, s_Pre, s_Target) == WorldMatch::IndeterminateDirty);
        s_Post.dirty = Field<bool>::Of(FieldStatus::Unavailable, "x");
        CHECK(ClassifyWorld(s_Post, s_Pre, s_Target) == WorldMatch::Unavailable);

        const Words7 s_PreLocal = FakeTransform::LocalOf(0.f, 0.f, -1.539356f);
        s_Post.local = Field<Words7>::Observed(s_PreLocal);
        CHECK(ClassifyLocal(s_Post, s_PreLocal) == LocalMatch::Unchanged);
        Words7 s_Moved = s_PreLocal;
        s_Moved.w[2] = s_Target.w[11];
        s_Post.local = Field<Words7>::Observed(s_Moved);
        CHECK(ClassifyLocal(s_Post, s_PreLocal) == LocalMatch::Changed);
        s_Post.local = Field<Words7>::Of(FieldStatus::Unavailable, "x");
        CHECK(ClassifyLocal(s_Post, s_PreLocal) == LocalMatch::Unavailable);

        TransformObservation::Snapshot s_Before, s_After;
        s_Before.world = Field<Words12>::Observed(FakeTransform::WorldOf(-83.74377f, 14.69725f, -1.522178f));
        s_Before.local = Field<Words7>::Observed(FakeTransform::LocalOf(-83.74377f, 14.69725f, -1.522178f));
        s_Before.parent = Field<std::string>::Observed(FakeTransform::k_HeroParent);
        s_After = s_Before;
        CHECK(ClassifyControl(s_Before, s_After) == Control::Unchanged);
        Words12 s_Drift = *s_Before.world.value;
        s_Drift.w[10] = FloatWords::ToWord(14.69725f + 1.0e-5f);
        s_After.world = Field<Words12>::Observed(s_Drift);
        CHECK(ClassifyControl(s_Before, s_After) == Control::WithinTolerance);
        s_Drift.w[9] = FloatWords::ToWord(-83.0f);
        s_After.world = Field<Words12>::Observed(s_Drift);
        CHECK(ClassifyControl(s_Before, s_After) == Control::Changed);
        s_After = s_Before;
        s_After.parent = Field<std::string>::Observed("none");
        CHECK(ClassifyControl(s_Before, s_After) == Control::Changed);
        s_After.parent = Field<std::string>::Of(FieldStatus::Unavailable, "x");
        CHECK(ClassifyControl(s_Before, s_After) == Control::Unavailable);
    }
}
