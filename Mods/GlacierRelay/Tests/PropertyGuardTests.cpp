// M6-D1a review, correction B: the property-read guard (CandidateObservation::PropertyGuard), the
// engine-independent decision the Relay's property-inspection path makes BEFORE it enters the SDK's
// property walkers and BEFORE it enters the SDK's property read. Pure facts in, a verdict out: a
// missing table, an empty table, a valid table, a table at the limit, a table over the limit,
// inconsistent metadata (one null end, a backwards span, a misaligned span, a zero element size),
// a property absent from a valid table, a getter-backed property, a wrong type by name and by size,
// an unterminated and an empty type name, an unregistered type, a missing allocator and missing type
// functions, and the allowed read. Every rejected input ends at a named stage before the SDK read;
// every refusal's reason fits the encoder's reason bound and a candidate read-back carrying every
// refusal reason encodes within the line bound. No engine pointer is constructed; no memory is
// written; no setter exists on this path.
#include "TestHarness.h"

#include <cstdio>
#include <optional>
#include <string>

#include <Windows.h>

#include "CandidateObservation.h"
#include "CommandScheduler.h"
#include "TestListener.h"

using namespace CommandProtocol;
using OutfitObservation::Field;
using namespace CandidateObservation::PropertyGuard;

namespace
{
    constexpr size_t k_Element = 40; // sizeof the declared SPropertyData on x64 (ptr, int64, 2 × uint32, uint64, int32, padding)
    constexpr uintptr_t k_Base = 0x7ff6'0000'1000;

    TableFacts Table(size_t p_Count, uintptr_t p_Base = k_Base)
    {
        TableFacts s_Facts;
        s_Facts.present = true;
        s_Facts.begin = p_Base;
        s_Facts.end = p_Base + p_Count * k_Element;
        s_Facts.element_size = k_Element;
        return s_Facts;
    }

    EntryFacts GoodEntry()
    {
        EntryFacts s_Facts;
        s_Facts.info_present = true;
        s_Facts.type_present = true;
        s_Facts.has_getter = false;
        s_Facts.type_info_present = true;
        s_Facts.name_present = true;
        s_Facts.name_length = 4; // "bool"
        s_Facts.name_crc = 0x1234abcd;
        s_Facts.expected_crc = 0x1234abcd;
        s_Facts.type_size = 1;
        s_Facts.expected_size = 1;
        s_Facts.type_registered = true;
        s_Facts.allocator_present = true;
        s_Facts.copy_present = true;
        s_Facts.destruct_present = true;
        return s_Facts;
    }

    bool Rejected(const Decision& p_Decision, Stage p_Stage, FieldStatus p_Status, const char* p_Reason)
    {
        return p_Decision.stage == p_Stage && p_Decision.status == p_Status && p_Decision.reason == p_Reason && p_Decision.reason.size() <= k_MaxObservedReasonBytes;
    }
}

void RunPropertyGuardTests()
{
    // ---- the table: missing, empty (both forms), valid, at the limit, over the limit --------------
    {
        TableFacts s_Missing;
        s_Missing.present = false;
        const auto s_M = CheckTable(s_Missing);
        CHECK(s_M.kind == TableKind::Missing && s_M.status == FieldStatus::Unavailable && s_M.reason == "property table null" && s_M.count == 0);

        TableFacts s_EmptyNull;
        s_EmptyNull.present = true;
        s_EmptyNull.element_size = k_Element;
        const auto s_E0 = CheckTable(s_EmptyNull);
        CHECK(s_E0.kind == TableKind::Empty && s_E0.status == FieldStatus::Unavailable && s_E0.reason == "property table empty" && s_E0.count == 0);

        const auto s_E1 = CheckTable(Table(0)); // begin == end, both non-null
        CHECK(s_E1.kind == TableKind::Empty && s_E1.reason == "property table empty");

        const auto s_V = CheckTable(Table(37));
        CHECK(s_V.kind == TableKind::Ok && s_V.count == 37 && s_V.status == FieldStatus::Observed && s_V.reason.empty());

        const auto s_One = CheckTable(Table(1));
        CHECK(s_One.kind == TableKind::Ok && s_One.count == 1);

        const auto s_At = CheckTable(Table(k_MaxPropertyCount));
        CHECK(s_At.kind == TableKind::Ok && s_At.count == k_MaxPropertyCount);

        const auto s_Over = CheckTable(Table(k_MaxPropertyCount + 1));
        CHECK(s_Over.kind == TableKind::OverLimit && s_Over.count == k_MaxPropertyCount + 1 && s_Over.status == FieldStatus::Unsupported && s_Over.reason == "property table over bound (4096)");
        CHECK(s_Over.reason.size() <= k_MaxObservedReasonBytes);

        const auto s_Huge = CheckTable(Table(1u << 20));
        CHECK(s_Huge.kind == TableKind::OverLimit); // refused whole, never searched in part
        CHECK(k_MaxPropertyCount == 4096);
    }

    // ---- inconsistent metadata: one null end, a backwards span, a misaligned span, no element --
    {
        TableFacts s_NullBegin = Table(3);
        s_NullBegin.begin = 0;
        const auto s_NB = CheckTable(s_NullBegin);
        CHECK(s_NB.kind == TableKind::Inconsistent && s_NB.status == FieldStatus::Inconsistent && s_NB.reason == "property table span inconsistent");

        TableFacts s_NullEnd = Table(3);
        s_NullEnd.end = 0;
        const auto s_NE = CheckTable(s_NullEnd);
        CHECK(s_NE.kind == TableKind::Inconsistent && s_NE.reason == "property table span inconsistent");

        TableFacts s_Backwards = Table(3);
        s_Backwards.end = s_Backwards.begin - k_Element;
        const auto s_B = CheckTable(s_Backwards);
        CHECK(s_B.kind == TableKind::Inconsistent && s_B.reason == "property table span inconsistent");

        TableFacts s_Misaligned = Table(3);
        s_Misaligned.end += 8;
        const auto s_Mis = CheckTable(s_Misaligned);
        CHECK(s_Mis.kind == TableKind::Inconsistent && s_Mis.status == FieldStatus::Inconsistent && s_Mis.reason == "property table span misaligned");

        TableFacts s_NoElement = Table(3);
        s_NoElement.element_size = 0;
        const auto s_NoE = CheckTable(s_NoElement);
        CHECK(s_NoE.kind == TableKind::Inconsistent && s_NoE.reason == "property table element size zero");

        // an inconsistent table is refused before any lookup, whatever the entry would have said
        const Decision s_D = Decide(s_Misaligned, GoodEntry());
        CHECK(Rejected(s_D, Stage::RejectTable, FieldStatus::Inconsistent, "property table span misaligned"));
    }

    // ---- the decision stages: table rejected, not declared, entry rejected, read ------------------
    {
        TableFacts s_Missing;
        s_Missing.present = false;
        CHECK(Rejected(Decide(s_Missing, GoodEntry()), Stage::RejectTable, FieldStatus::Unavailable, "property table null"));
        CHECK(Rejected(Decide(Table(0), GoodEntry()), Stage::RejectTable, FieldStatus::Unavailable, "property table empty"));
        CHECK(Rejected(Decide(Table(k_MaxPropertyCount + 1), GoodEntry()), Stage::RejectTable, FieldStatus::Unsupported, "property table over bound (4096)"));

        // a valid table, the property absent from it: the lookup ran (bounded) and found nothing
        CHECK(Rejected(Decide(Table(12), std::nullopt), Stage::NotDeclared, FieldStatus::Unavailable, "property not declared on this type"));

        const Decision s_Read = Decide(Table(12), GoodEntry());
        CHECK(s_Read.stage == Stage::Read && s_Read.status == FieldStatus::Observed && s_Read.reason.empty() && s_Read.table_count == 12);

        const Decision s_AtLimit = Decide(Table(k_MaxPropertyCount), GoodEntry());
        CHECK(s_AtLimit.stage == Stage::Read && s_AtLimit.table_count == k_MaxPropertyCount);
    }

    // ---- the entry: getter, wrong type by name, wrong type by size, unterminated / empty name,
    //      unregistered type, missing allocator, missing type functions, missing info ----------------
    {
        EntryFacts s_Getter = GoodEntry();
        s_Getter.has_getter = true;
        const auto s_G = CheckEntry(s_Getter);
        CHECK(!s_G.allowed && s_G.status == FieldStatus::Unsupported && s_G.reason == "property has an engine getter");
        CHECK(Rejected(Decide(Table(5), s_Getter), Stage::RejectEntry, FieldStatus::Unsupported, "property has an engine getter"));

        EntryFacts s_WrongName = GoodEntry();
        s_WrongName.name_crc = 0x0badf00d;
        const auto s_WN = CheckEntry(s_WrongName);
        CHECK(!s_WN.allowed && s_WN.status == FieldStatus::Unsupported && s_WN.reason == "property type differs from the expected");

        EntryFacts s_WrongSize = GoodEntry();
        s_WrongSize.type_size = 4;
        const auto s_WS = CheckEntry(s_WrongSize);
        CHECK(!s_WS.allowed && s_WS.reason == "property type differs from the expected");

        EntryFacts s_Unterminated = GoodEntry();
        s_Unterminated.name_length = k_MaxTypeNameBytes; // the bounded scan found no terminator
        s_Unterminated.name_crc = s_Unterminated.expected_crc; // even an equal prefix hash is refused
        const auto s_U = CheckEntry(s_Unterminated);
        CHECK(!s_U.allowed && s_U.status == FieldStatus::Unsupported && s_U.reason == "property type name unterminated");

        EntryFacts s_Long = GoodEntry();
        s_Long.name_length = k_MaxTypeNameBytes - 1; // terminated on the last byte: compared
        CHECK(CheckEntry(s_Long).allowed);

        EntryFacts s_Empty = GoodEntry();
        s_Empty.name_length = 0;
        const auto s_Em = CheckEntry(s_Empty);
        CHECK(!s_Em.allowed && s_Em.status == FieldStatus::Unavailable && s_Em.reason == "property type name empty");

        EntryFacts s_Unregistered = GoodEntry();
        s_Unregistered.type_registered = false;
        const auto s_Un = CheckEntry(s_Unregistered);
        CHECK(!s_Un.allowed && s_Un.status == FieldStatus::Unsupported && s_Un.reason == "type not registered");

        EntryFacts s_NoAllocator = GoodEntry();
        s_NoAllocator.allocator_present = false;
        const auto s_NA = CheckEntry(s_NoAllocator);
        CHECK(!s_NA.allowed && s_NA.status == FieldStatus::Unavailable && s_NA.reason == "normal allocator null");

        EntryFacts s_NoCopy = GoodEntry();
        s_NoCopy.copy_present = false;
        CHECK(!CheckEntry(s_NoCopy).allowed && CheckEntry(s_NoCopy).reason == "property type functions null");
        EntryFacts s_NoDestruct = GoodEntry();
        s_NoDestruct.destruct_present = false;
        CHECK(!CheckEntry(s_NoDestruct).allowed && CheckEntry(s_NoDestruct).reason == "property type functions null");

        EntryFacts s_NoInfo = GoodEntry();
        s_NoInfo.info_present = false;
        CHECK(!CheckEntry(s_NoInfo).allowed && CheckEntry(s_NoInfo).reason == "property info null");
        EntryFacts s_NoType = GoodEntry();
        s_NoType.type_present = false;
        CHECK(!CheckEntry(s_NoType).allowed && CheckEntry(s_NoType).reason == "property info null");
        EntryFacts s_NoTypeInfo = GoodEntry();
        s_NoTypeInfo.type_info_present = false;
        CHECK(!CheckEntry(s_NoTypeInfo).allowed && CheckEntry(s_NoTypeInfo).reason == "property type info null");
        EntryFacts s_NoName = GoodEntry();
        s_NoName.name_present = false;
        CHECK(!CheckEntry(s_NoName).allowed && CheckEntry(s_NoName).reason == "property type info null");

        // the getter is refused before the type is compared: a getter-backed property of the right type is still not read
        EntryFacts s_GetterRightType = GoodEntry();
        s_GetterRightType.has_getter = true;
        CHECK(CheckEntry(s_GetterRightType).reason == "property has an engine getter");

        const auto s_Good = CheckEntry(GoodEntry());
        CHECK(s_Good.allowed && s_Good.status == FieldStatus::Observed && s_Good.reason.empty());
    }

    // ---- every refusal reason fits the encoder's reason bound; a read-back carrying them fits the line
    {
        const char* s_Reasons[] = {
            "property table null", "property table empty", "property table span inconsistent", "property table span misaligned",
            "property table element size zero", "property table over bound (4096)", "property not declared on this type",
            "property info null", "property has an engine getter", "property type info null", "property type name empty",
            "property type name unterminated", "property type differs from the expected", "type not registered",
            "normal allocator null", "property type functions null", "property variant type mismatch",
        };
        for (const char* s_Reason : s_Reasons)
            CHECK(std::string(s_Reason).size() <= k_MaxObservedReasonBytes);

        // a candidate read-back whose six property fields carry the longest refusal reason, with every
        // other field observed at its realistic value, encodes within the bound (the scheduler adds
        // the frame values): the refusal texts never force a reduction
        CommandQueue s_Queue;
        std::vector<Result> s_Results;
        CommandScheduler s_Scheduler(s_Queue, [&](const Result& p_Result) { s_Results.push_back(p_Result); });
        Command s_Command;
        s_Command.command_id = CommandTest::Uuid(21);
        s_Command.link_generation = 1;
        s_Command.ttl_ms = k_MaxTtlMs;
        s_Command.name = "relay.inspect.candidate";
        s_Command.version = 1;
        s_Command.entry = Entry::RelayInspectCandidate1;
        s_Queue.Push(s_Command, 1, 0);
        CommandScheduler::FrameObservation s_Frame;
        s_Frame.frame = 40000;
        s_Frame.now_mono_ms = 10;
        s_Frame.native_at = "2026-10-16T18:00:00.000Z";
        s_Frame.thread_id = GetCurrentThreadId();
        s_Frame.scene_available = true;
        s_Frame.scene.available = true;
        s_Frame.scene.scene_type = "mission";
        s_Frame.mission_playing = true;
        s_Frame.mission_generation = "2516103582917844185-a105ec5f-3a75-46a3-8ecd-e72fd15b176c";
        s_Frame.link_generation = 1;
        s_Frame.adapter_instance_id = CommandTest::Uuid(99);
        s_Frame.candidate_inspector = [&](const CandidateObservation::CandidateQuery& p_Query) {
            CandidateObservation::CandidateReads s_Reads = CandidateObservation::AllCandidate(FieldStatus::Unavailable, "candidate reads faulted", p_Query);
            s_Reads.thread_id = GetCurrentThreadId();
            const std::string s_Longest = "property type differs from the expected";
            s_Reads.hero.active = Field<bool>::Of(FieldStatus::Unsupported, s_Longest);
            s_Reads.candidate.active = Field<bool>::Of(FieldStatus::Unsupported, s_Longest);
            s_Reads.spawn.player_id = Field<int64_t>::Of(FieldStatus::Unsupported, s_Longest);
            s_Reads.spawn.default_spawn = Field<bool>::Of(FieldStatus::Unsupported, s_Longest);
            s_Reads.spawn.preferred_spawn = Field<bool>::Of(FieldStatus::Unsupported, s_Longest);
            s_Reads.spawn.hitman_ref = Field<std::string>::Of(FieldStatus::Unsupported, s_Longest);
            s_Reads.spawn.ref_is_candidate = Field<bool>::Of(FieldStatus::Unsupported, s_Longest);
            return s_Reads;
        };
        CHECK(s_Scheduler.Tick(s_Frame) && s_Results.size() == 1);
        if (!s_Results.empty())
        {
            const std::string s_Line = EncodeResult(s_Results[0]);
            std::printf("line bound: candidate read-back with every property refusal at the longest reason = %zu bytes of %zu\n", s_Line.size(), k_MaxLineBytes);
            CHECK(s_Line.size() <= k_MaxLineBytes && s_Results[0].status == Status::Applied);
            bool s_Marker = false;
            for (const auto& s_Entry : s_Results[0].observed)
                if (s_Entry.first == "reasons_dropped" || s_Entry.first == "observed_reduced")
                    s_Marker = true;
            CHECK(!s_Marker);
        }
    }
}
