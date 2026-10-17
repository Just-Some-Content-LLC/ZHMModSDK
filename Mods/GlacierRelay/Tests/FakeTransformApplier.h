#pragma once

#include <string>

#include <Windows.h>

#include "ITransformApplier.h"
#include "TransformObservation.h"

// A FAKE transform applier for the tests and the command probe (M6-F): scripted words only. It
// never constructs an engine pointer, valid or invalid, and never calls an SDK function; every
// value it reports is simulated and is labelled so wherever it is printed. It counts its calls so
// a test can prove the approved call happens exactly once on success and never on a refusal. The
// scripted candidate and hero are the probe's shared FAKE world (FakeCandidate.h / the FAKE entity
// reader: the candidate at the authored spawn position (-40.99882, -24.293657, 0.4925565) with the
// identity axes, the hero feed9a0d70d546f4 at (-0.035, -3.832, -0.413)), so the operator cross-checks
// compare like with like; the M6-D1b observation's real words are exercised by the codec tests.
namespace FakeTransform
{
    using FloatWords::Words12;
    using FloatWords::Words7;
    using OutfitObservation::Field;
    using OutfitObservation::FieldStatus;
    using TransformObservation::Reads;
    using TransformObservation::Snapshot;

    enum class Mode
    {
        ChangeToTarget,      // the fake setter stores the requested words (the parent-relative words untouched)
        Unchanged,           // the fake setter returns and nothing changes
        Elsewhere,           // the fake setter leaves a translation that is neither prior nor requested
        AxesElsewhere,       // the translation reaches the target but an axis word differs (not an all-12 match)
        LocalFollows,        // the world reaches the target and the parent-relative translation follows it
        DirtyPost,           // the fake setter stores the target and sets the dirty flag (indeterminate)
        ReadBackUnavailable, // the fake setter stores the target but the read-back cannot read the candidate
        PostGenerationChange,// the fake registry names another generation after the call
        PostParentChange,    // the parent id differs after the call
        PostEntityChange,    // the post read resolves a different entity
        FaultDuringCall,     // the fake setter "faults" (returns faulted; the real guard would not return)
        ObserveFaults,       // the fake reads "fault" before any call
        ControlChanges,      // the local hero's words change between the pre and post reads
        ControlDrifts,       // the local hero's translation drifts by 1e-5 (within the declared tolerance)
    };

    // What a LATER inspection (the probe's scripted later frame) shows after a fake lift.
    enum class Later
    {
        Held,     // the target persists
        Returned, // the preimage is back
        Near,     // the target plus 1e-5 on z (within the tolerance, not bit-equal)
        Drifted,  // the target plus 0.5 on z
        Dirty,    // the dirty flag set
    };

    constexpr const char* k_Gen1 = "2516103582917844185-a105ec5f-3a75-46a3-8ecd-e72fd15b176c";
    constexpr const char* k_Gen2 = "2516103582027577704-30ccebb9-8138-4c72-9eb9-1a289b4fba33";
    constexpr const char* k_Brick = TransformObservation::k_Brick;
    constexpr const char* k_Entity = TransformObservation::k_Entity;
    constexpr const char* k_HeroId = "feed9a0d70d546f4";
    constexpr const char* k_HeroParent = "0000000000000001";
    constexpr const char* k_LocalOnly = "0:local,1:empty,2:empty,3:empty";
    constexpr const char* k_WithCandidate = "0:local,1:candidate,2:empty,3:empty";
    constexpr const char* k_WithInconclusive = "0:local,1:inconclusive,2:empty,3:empty";

    inline Words12 WorldOf(float p_X, float p_Y, float p_Z)
    {
        Words12 s_Words;
        const float s_Floats[12] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, p_X, p_Y, p_Z};
        for (size_t i = 0; i < 12; ++i)
            s_Words.w[i] = FloatWords::ToWord(s_Floats[i]);
        return s_Words;
    }

    inline Words7 LocalOf(float p_X, float p_Y, float p_Z)
    {
        Words7 s_Words;
        const float s_Floats[7] = {p_X, p_Y, p_Z, 0.f, 0.f, 0.f, 1.f};
        for (size_t i = 0; i < 7; ++i)
            s_Words.w[i] = FloatWords::ToWord(s_Floats[i]);
        return s_Words;
    }

    struct World
    {
        // the candidate's identity block
        bool brick_present = true;
        bool scan_complete = true;    // false: absence not established (the scan bound)
        bool resolved = true;
        bool index_in_range = true;
        bool hitman5 = true;
        bool id_match = true;
        bool owner_match = true;
        bool distinct = true;         // false: the lookup returned the local hero
        bool spatial = true;
        bool spatial_distinct = true;
        bool registry_readable = true;
        bool in_registry = false;     // slot 1 = candidate
        bool inconclusive_slot = false; // slot 1 = inconclusive
        bool hero_readable = true;
        bool dirty = false;
        bool fresh_unavailable = false;
        std::string fresh_generation = k_Gen1;
        std::string post_generation = k_Gen1;

        // the candidate's words (the M6-D1b shape) and the hero's
        Words12 world = WorldOf(-40.99882f, -24.293657f, 0.4925565f);
        Words7 local = LocalOf(-40.99882f, -24.293657f, 0.4925565f);
        std::string parent = k_Brick;
        Words12 hero_world = WorldOf(-0.035f, -3.832f, -0.413f);
        Words7 hero_local = LocalOf(-0.035f, -3.832f, -0.413f);
        std::string hero_parent = k_HeroParent;

        Mode mode = Mode::ChangeToTarget;
        Later later = Later::Held;
    };

    class Applier final : public TransformApply::ITransformApplier
    {
    public:
        World world;
        int observes = 0;
        int invokes = 0;
        int readbacks = 0;
        int releases = 0;
        bool latched = false;
        bool later_pending = false; // the later-frame script applies once, on the first Observe after an Invoke (the same-frame ReadBack is not a later frame)
        Words12 last_target;

        // The snapshot an inspection or a read of the scripted world produces (shared with the probe's inspector).
        Reads Current(bool p_SameEntity, const std::string& p_Generation) const
        {
            Reads s_Reads;
            s_Reads.thread_id = GetCurrentThreadId();
            s_Reads.generation_fresh = world.fresh_unavailable ? Field<std::string>::Of(FieldStatus::Unavailable, "fake registry session id empty") : Field<std::string>::Observed(p_Generation);
            s_Reads.brick_id = k_Brick;
            s_Reads.entity_id = k_Entity;

            if (world.hero_readable)
            {
                s_Reads.hero_entity_id = Field<std::string>::Observed(k_HeroId);
                s_Reads.hero_flags = Field<int64_t>::Observed(7);
                s_Reads.hero.world = Field<Words12>::Observed(world.hero_world);
                s_Reads.hero.local = Field<Words7>::Observed(world.hero_local);
                s_Reads.hero.dirty = Field<bool>::Observed(false);
                s_Reads.hero.parent = Field<std::string>::Observed(world.hero_parent);
                s_Reads.hero.flags = Field<CandidateObservation::SpatialFlags>::Observed(Flags(false));
            }
            else
            {
                s_Reads.hero_entity_id = Field<std::string>::Of(FieldStatus::Unavailable, "fake hero entity reference null");
                s_Reads.hero_flags = Field<int64_t>::Of(FieldStatus::Unavailable, "fake hero entity reference null");
                TransformObservation::MarkSnapshot(s_Reads.hero, FieldStatus::Unavailable, "fake hero entity reference null");
            }

            if (!world.brick_present)
            {
                s_Reads.brick_present = world.scan_complete ? Field<bool>::Observed(false) : Field<bool>::Of(FieldStatus::Unavailable, "scan bound: 64/70 bricks scanned");
                const char* s_Why = world.scan_complete ? "brick not loaded" : "brick scan incomplete";
                PlayerObservation::MarkLookup(s_Reads.lookup, FieldStatus::Unavailable, s_Why);
                s_Reads.hitman5 = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
                s_Reads.cand_flags = Field<int64_t>::Of(FieldStatus::Unavailable, s_Why);
                s_Reads.id_match = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
                s_Reads.cand_brick_owner = Field<std::string>::Of(FieldStatus::Unavailable, s_Why);
                s_Reads.spatial = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
                s_Reads.spatial_distinct = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
                TransformObservation::MarkSnapshot(s_Reads.candidate, FieldStatus::Unavailable, s_Why);
            }
            else
            {
                s_Reads.brick_present = Field<bool>::Observed(true);
                s_Reads.lookup.owner_found = Field<bool>::Observed(true);
                s_Reads.lookup.factory_available = Field<bool>::Observed(true);
                s_Reads.lookup.factory_template = Field<bool>::Observed(true);
                s_Reads.lookup.subentity_count = Field<int64_t>::Observed(3);
                if (!world.index_in_range)
                {
                    s_Reads.lookup.index = Field<int64_t>::Observed(3);
                    s_Reads.lookup.index_check = Field<std::string>::Observed("out_of_range");
                    s_Reads.lookup.resolved = Field<bool>::Of(FieldStatus::Inconsistent, "index outside the sub-entity count");
                    s_Reads.lookup.same_entity = Field<bool>::Of(FieldStatus::Inconsistent, "index outside the sub-entity count");
                }
                else if (!world.resolved)
                {
                    s_Reads.lookup.index = Field<int64_t>::Observed(-1);
                    s_Reads.lookup.index_check = Field<std::string>::Observed("negative");
                    s_Reads.lookup.resolved = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
                    s_Reads.lookup.same_entity = Field<bool>::Of(FieldStatus::Unavailable, "not a sub-entity of that factory");
                }
                else
                {
                    s_Reads.lookup.index = Field<int64_t>::Observed(1);
                    s_Reads.lookup.index_check = Field<std::string>::Observed("in_range");
                    s_Reads.lookup.resolved = Field<bool>::Observed(true);
                    s_Reads.lookup.same_entity = Field<bool>::Observed(!world.distinct);
                }

                if (world.resolved && world.index_in_range)
                {
                    s_Reads.hitman5 = Field<bool>::Observed(world.hitman5);
                    s_Reads.cand_flags = Field<int64_t>::Observed(3);
                    s_Reads.id_match = Field<bool>::Observed(world.id_match);
                    s_Reads.cand_brick_owner = Field<std::string>::Observed(world.owner_match ? k_Brick : "0012e5a7b1c9d3ee");
                    s_Reads.spatial = Field<bool>::Observed(world.spatial);
                    if (!world.spatial)
                    {
                        s_Reads.spatial_distinct = Field<bool>::Of(FieldStatus::Unavailable, "spatial interface not resolved");
                        TransformObservation::MarkSnapshot(s_Reads.candidate, FieldStatus::Unavailable, "spatial interface not resolved");
                    }
                    else
                    {
                        s_Reads.spatial_distinct = Field<bool>::Observed(world.spatial_distinct);
                        s_Reads.candidate.world = Field<Words12>::Observed(world.world);
                        s_Reads.candidate.local = Field<Words7>::Observed(world.local);
                        s_Reads.candidate.dirty = Field<bool>::Observed(world.dirty);
                        s_Reads.candidate.parent = Field<std::string>::Observed(world.parent);
                        s_Reads.candidate.flags = Field<CandidateObservation::SpatialFlags>::Observed(Flags(world.dirty));
                    }
                }
                else
                {
                    const char* s_Why = "candidate entity not resolved";
                    s_Reads.hitman5 = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
                    s_Reads.cand_flags = Field<int64_t>::Of(FieldStatus::Unavailable, s_Why);
                    s_Reads.id_match = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
                    s_Reads.cand_brick_owner = Field<std::string>::Of(FieldStatus::Unavailable, s_Why);
                    s_Reads.spatial = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
                    s_Reads.spatial_distinct = Field<bool>::Of(FieldStatus::Unavailable, s_Why);
                    TransformObservation::MarkSnapshot(s_Reads.candidate, FieldStatus::Unavailable, s_Why);
                }
            }

            if (!world.registry_readable)
            {
                s_Reads.registry_slots = Field<std::string>::Of(FieldStatus::Unavailable, "fake registry global null");
                s_Reads.candidate_in_registry = Field<bool>::Of(FieldStatus::Unavailable, "fake registry global null");
            }
            else
            {
                s_Reads.registry_slots = Field<std::string>::Observed(world.in_registry ? k_WithCandidate : world.inconclusive_slot ? k_WithInconclusive : k_LocalOnly);
                s_Reads.candidate_in_registry = Field<bool>::Observed(world.in_registry || world.inconclusive_slot);
            }

            s_Reads.same_entity_as_before = Field<bool>::Observed(p_SameEntity);
            return s_Reads;
        }

        Reads Observe() override
        {
            ++observes;

            if (world.mode == Mode::ObserveFaults)
            {
                latched = true;
                auto s_Reads = TransformObservation::AllUnavailable(FieldStatus::Unavailable, "fake fault during the apply-frame reads");
                s_Reads.thread_id = GetCurrentThreadId();
                return s_Reads;
            }

            // The scripted later frame, once, on the first read after a fake lift.
            if (later_pending)
            {
                later_pending = false;
                switch (world.later)
                {
                    case Later::Held: break;
                    case Later::Returned: world.world = preimage; world.local = preimage_local; break;
                    case Later::Near: world.world.w[11] = FloatWords::ToWord(FloatWords::ToFloat(world.world.w[11]) + 1.0e-5f); break;
                    case Later::Drifted: world.world.w[11] = FloatWords::ToWord(FloatWords::ToFloat(world.world.w[11]) + 0.5f); break;
                    case Later::Dirty: world.dirty = true; break;
                }
            }

            Reads s_Reads = Current(false, world.fresh_generation);
            s_Reads.same_entity_as_before = Field<bool>::Of(FieldStatus::Unavailable, "no previous read on this frame");
            return s_Reads;
        }

        TransformApply::CallOutcome Invoke(const Words12& p_World) override
        {
            ++invokes;
            last_target = p_World;
            TransformApply::CallOutcome s_Outcome;
            s_Outcome.called = true;
            preimage = world.world;
            preimage_local = world.local;

            switch (world.mode)
            {
                case Mode::FaultDuringCall:
                    s_Outcome.faulted = true;
                    latched = true;
                    break;
                case Mode::Unchanged:
                    break;
                case Mode::Elsewhere:
                    world.world = p_World;
                    world.world.w[11] = FloatWords::ToWord(FloatWords::ToFloat(p_World.w[11]) + 0.125f);
                    break;
                case Mode::AxesElsewhere:
                    world.world = p_World;
                    world.world.w[0] = FloatWords::ToWord(0.9999999f);
                    break;
                case Mode::LocalFollows:
                    world.world = p_World;
                    world.local.w[2] = p_World.w[11];
                    break;
                case Mode::DirtyPost:
                    world.world = p_World;
                    world.dirty = true;
                    break;
                case Mode::ReadBackUnavailable:
                    world.world = p_World;
                    world.resolved = false;
                    break;
                case Mode::PostGenerationChange:
                    world.world = p_World;
                    world.post_generation = k_Gen2;
                    break;
                case Mode::PostParentChange:
                    world.world = p_World;
                    world.parent = "feed000000000bad";
                    break;
                case Mode::ControlChanges:
                    world.world = p_World;
                    world.hero_world = WorldOf(-1.0f, -3.832f, -0.413f);
                    break;
                case Mode::ControlDrifts:
                    world.world = p_World;
                    world.hero_world.w[10] = FloatWords::ToWord(-3.832f + 1.0e-5f);
                    break;
                default:
                    world.world = p_World;
                    break;
            }

            later_pending = true;
            return s_Outcome;
        }

        Reads ReadBack() override
        {
            ++readbacks;
            return Current(world.mode != Mode::PostEntityChange, world.post_generation);
        }

        bool Latched() const override { return latched; }
        void Release() override { ++releases; }

        Words12 preimage;
        Words7 preimage_local;

    private:
        static CandidateObservation::SpatialFlags Flags(bool p_Dirty)
        {
            CandidateObservation::SpatialFlags s_Flags;
            s_Flags.visible = true;
            s_Flags.dynamic = true;
            s_Flags.scene_child = true;
            s_Flags.dirty = p_Dirty;
            s_Flags.room_behaviour = 1;
            s_Flags.transform_parent = true;
            s_Flags.transform_children = true;
            return s_Flags;
        }
    };
}
