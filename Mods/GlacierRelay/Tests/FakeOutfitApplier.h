#pragma once

#include <optional>
#include <string>

#include "IOutfitApplier.h"
#include "OutfitObservation.h"

// A FAKE outfit applier for the tests and the command probe (M5 C-2B): scripted state only. It
// never constructs an engine pointer, valid or invalid, and never calls an SDK function; every
// value it reports is simulated and is labelled so wherever it is printed. It counts its calls so
// a test can prove the approved call happens exactly once on success and never on a refusal.
namespace FakeOutfit
{
    using OutfitObservation::Field;
    using OutfitObservation::FieldStatus;

    enum class Mode
    {
        ChangeToTarget,     // the fake setter changes the fake hero's tuple to the requested one
        Unchanged,          // the fake setter returns and nothing changes
        Elsewhere,          // the fake setter leaves a tuple that is neither prior nor requested
        FaultDuringCall,    // the fake setter "faults" (returns faulted; the real guard would not return)
        ReadBackUnavailable,// the fake setter changes the tuple but the read-back cannot read it
        PostGenerationChange,// the fake registry names another generation after the call
        ObserveFaults,      // the fake reads "fault" before any call
    };

    struct World
    {
        bool player_present = true;
        bool fresh_unavailable = false;
        bool fresh_inconsistent = false;
        std::string fresh_generation = "2516103582917844185-a105ec5f-3a75-46a3-8ecd-e72fd15b176c";
        std::string post_generation = "2516103582917844185-a105ec5f-3a75-46a3-8ecd-e72fd15b176c";
        std::string hero_outfit_id = "874c4c48-0a8b-49e9-883e-49fc5f1fb051"; // the FAKE hero's kit id
        int64_t hero_charset = 0;
        int64_t hero_variation = 0;
        bool hero_tuple_readable = true;
        bool setter_available = true;
        bool kit_map_available = true;
        // The fake kit table: the two fake suits with one charset and one HeroA variation, a fake
        // kit with two charsets whose second has three HeroA variations, a fake kit with no HeroA.
        bool candidate_found(const std::string& p_Id) const { return p_Id == k_FakeSuit || p_Id == k_FakeTarget || p_Id == k_FakeTwoCharsets || p_Id == k_FakeNoHeroA; }
        Mode mode = Mode::ChangeToTarget;

        static constexpr const char* k_FakeSuit = "874c4c48-0a8b-49e9-883e-49fc5f1fb051";       // FAKE table entry (the id the real runs showed)
        static constexpr const char* k_FakeTarget = "4fc9396e-2619-4e66-a51e-2bd366230da7";     // FAKE table entry (the C-1 menu kit id); never resolved against HITMAN here
        static constexpr const char* k_FakeTwoCharsets = "5555db77-3333-4333-8333-333333333333";
        static constexpr const char* k_FakeNoHeroA = "3333db77-2222-4222-8222-222222222222";
    };

    class Applier final : public OutfitApply::IOutfitApplier
    {
    public:
        World world;
        int observes = 0;
        int invokes = 0;
        int readbacks = 0;
        int releases = 0;
        bool latched = false;
        std::optional<std::string> last_candidate;

        OutfitObservation::EngineReads Observe(const std::optional<std::string>& p_CandidateId) override
        {
            ++observes;
            last_candidate = p_CandidateId;

            if (world.mode == Mode::ObserveFaults)
            {
                latched = true;
                return Faulted("fake fault during the apply-frame reads");
            }

            return Reads(p_CandidateId, world.fresh_generation);
        }

        OutfitApply::CallOutcome Invoke(int32_t p_Charset, int32_t p_Variation) override
        {
            ++invokes;
            OutfitApply::CallOutcome s_Outcome;
            s_Outcome.called = true;

            switch (world.mode)
            {
                case Mode::FaultDuringCall:
                    s_Outcome.faulted = true;
                    latched = true;
                    break;
                case Mode::Unchanged:
                    break;
                case Mode::Elsewhere:
                    world.hero_outfit_id = "2018db77-aa8a-4bf9-9afb-56bdaa161156";
                    world.hero_charset = 0;
                    world.hero_variation = 0;
                    break;
                case Mode::ReadBackUnavailable:
                    world.hero_outfit_id = *last_candidate;
                    world.hero_charset = p_Charset;
                    world.hero_variation = p_Variation;
                    world.hero_tuple_readable = false;
                    break;
                case Mode::PostGenerationChange:
                    world.hero_outfit_id = *last_candidate;
                    world.hero_charset = p_Charset;
                    world.hero_variation = p_Variation;
                    world.post_generation = "2516103582027577704-30ccebb9-8138-4c72-9eb9-1a289b4fba33";
                    break;
                default:
                    world.hero_outfit_id = *last_candidate;
                    world.hero_charset = p_Charset;
                    world.hero_variation = p_Variation;
                    break;
            }

            return s_Outcome;
        }

        OutfitObservation::EngineReads ReadBack() override
        {
            ++readbacks;
            return Reads(std::nullopt, world.post_generation);
        }

        bool Latched() const override { return latched; }
        void Release() override { ++releases; }

    private:
        static OutfitObservation::EngineReads Faulted(const char* p_Why)
        {
            OutfitObservation::EngineReads s_Reads;
            s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, p_Why);
            s_Reads.hero.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, p_Why);
            s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, p_Why);
            s_Reads.set_outfit_available = Field<bool>::Of(FieldStatus::Unavailable, p_Why);
            s_Reads.kit_map_available = Field<bool>::Of(FieldStatus::Unavailable, p_Why);
            return s_Reads;
        }

        OutfitObservation::KitShape Shape(const std::string& p_Id) const
        {
            OutfitObservation::KitShape s_Shape;
            s_Shape.id = p_Id;
            if (p_Id == World::k_FakeSuit || p_Id == World::k_FakeTarget)
            {
                s_Shape.is_hitman_suit = true;
                s_Shape.heroa_variations = {Field<int64_t>::Observed(1)};
            }
            else if (p_Id == World::k_FakeTwoCharsets)
            {
                s_Shape.heroa_variations = {Field<int64_t>::Observed(1), Field<int64_t>::Observed(3)};
            }
            else if (p_Id == World::k_FakeNoHeroA)
            {
                s_Shape.heroa_variations = {Field<int64_t>::Of(FieldStatus::Unavailable, "no HeroA character type in charset")};
            }
            return s_Shape;
        }

        OutfitObservation::EngineReads Reads(const std::optional<std::string>& p_CandidateId, const std::string& p_Fresh) const
        {
            OutfitObservation::EngineReads s_Reads;

            if (world.fresh_inconsistent)
                s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Inconsistent, "fake registry reads differ");
            else if (world.fresh_unavailable || p_Fresh.empty())
                s_Reads.generation_fresh = Field<std::string>::Of(FieldStatus::Unavailable, "fake registry session id empty");
            else
                s_Reads.generation_fresh = Field<std::string>::Observed(p_Fresh);

            if (world.player_present)
            {
                s_Reads.hero.player_resolved = true;
                if (world.hero_tuple_readable)
                {
                    OutfitObservation::DescribeHeroOutfit(Shape(world.hero_outfit_id), world.hero_charset, world.hero_variation, s_Reads.hero);
                }
                else
                {
                    s_Reads.hero.outfit_id = Field<std::string>::Observed(world.hero_outfit_id);
                    s_Reads.hero.charset = Field<int64_t>::Of(FieldStatus::Unavailable, "fake charset unreadable");
                    s_Reads.hero.variation = Field<int64_t>::Of(FieldStatus::Unavailable, "fake variation unreadable");
                    s_Reads.hero.heroa_valid = Field<bool>::Observed(true);
                }
            }
            else
            {
                s_Reads.hero.outfit_id = Field<std::string>::Of(FieldStatus::Unavailable, "fake hero entity reference null");
                s_Reads.hero.heroa_valid = Field<bool>::Of(FieldStatus::Unavailable, "fake hero entity reference null");
            }

            s_Reads.kit_map_available = Field<bool>::Observed(world.kit_map_available);

            if (p_CandidateId)
            {
                OutfitObservation::KitResolution s_Kit;
                s_Kit.kits_scanned = Field<int64_t>::Observed(4);
                if (world.kit_map_available && world.candidate_found(*p_CandidateId))
                    OutfitObservation::DescribeCandidate(Shape(*p_CandidateId), s_Kit);
                else
                    s_Kit.found = Field<bool>::Observed(false);
                s_Reads.candidate = s_Kit;
            }

            s_Reads.set_outfit_available = Field<bool>::Observed(world.setter_available);
            return s_Reads;
        }
    };
}
