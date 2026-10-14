#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "CommandProtocol.h"

// What the frame-local outfit inspection (M5 C-1, relay.inspect.outfit v1) can report, as plain
// owned values with an explicit per-field status. Nothing here is an engine type, a pointer, an
// entity reference or a resource id; the Glacier-facing reader (SceneObservation::ObserveOutfit)
// fills these from guarded reads on the inspection frame and the result encodes each field as
// {"status": ..., "value": ..., "reason": ...} so a missing value is never a zero, a default kit or
// an assumed generation. A field is one of:
//   observed            the value was read this frame
//   unavailable         a required global, reference or bound was missing (reason says which)
//   unsupported         the reader has no safe way to obtain it on this build
//   inconsistent        two reads of the same engine value disagreed (a possible torn read)
//   precondition_failed the value could not be read because an earlier check failed
namespace OutfitObservation
{
    using FieldStatus = CommandProtocol::FieldStatus;

    template <class T>
    struct Field
    {
        FieldStatus status = FieldStatus::Unavailable;
        std::optional<T> value;
        std::string reason; // bounded on encoding; empty when observed

        static Field Observed(T p_Value)
        {
            Field s_Field;
            s_Field.status = FieldStatus::Observed;
            s_Field.value = std::move(p_Value);
            return s_Field;
        }

        static Field Of(FieldStatus p_Status, std::string p_Reason)
        {
            Field s_Field;
            s_Field.status = p_Status;
            s_Field.reason = std::move(p_Reason);
            return s_Field;
        }

        bool observed() const { return status == FieldStatus::Observed && value.has_value(); }
    };

    // The hero's current outfit tuple (ZHitman5::m_rOutfitKit / m_nOutfitCharset / m_nOutfitVariation,
    // read fresh; the kit's own repository id rendered as the Relay renders disguise ids).
    struct HeroOutfit
    {
        bool player_resolved = false;        // the local hero entity and its interface resolved this frame
        Field<std::string> outfit_id;        // the kit definition's repository id (dashed lowercase)
        Field<int64_t> charset;
        Field<int64_t> variation;
        Field<int64_t> charset_count;        // m_aCharSets.size() of the current kit
        Field<int64_t> heroa_variations;     // HeroA variations of the current charset
        Field<bool> heroa_valid;             // charset within bounds, HeroA collection present, variation within bounds
        Field<bool> is_hitman_suit;
    };

    // The resolution of a candidate kit id against the engine's own kit map (read-only).
    struct KitResolution
    {
        Field<bool> found;                   // the id is a key of the loaded kit map
        Field<int64_t> charsets;             // m_aCharSets.size()
        Field<int64_t> heroa_variations;     // HeroA variations of charset 0
        Field<bool> heroa_usable;            // charset 0 has a HeroA collection with at least one variation
        Field<bool> is_hitman_suit;
        Field<int64_t> kits_scanned;         // how many map entries the bounded scan visited
    };

    struct EngineReads
    {
        Field<std::string> generation_fresh; // registry slot 0 session id read on this frame (twice, equal)
        HeroOutfit hero;
        std::optional<KitResolution> candidate; // present when the command named a candidate id
        Field<bool> set_outfit_available;    // Functions::ZHitman5_SetOutfit->Exists()
        Field<bool> kit_map_available;       // Globals::ContentKitManager non-null
        uint32_t thread_id = 0;              // the thread the reads were made on
    };

    // The shape of a global outfit kit as plain values: its id, the suit flag and, per charset,
    // the HeroA variation count or the reason it could not be read (charset collection null, no
    // HeroA character type, ...). The Glacier-facing reader extracts a shape with bounds-checked
    // reads; the two Describe functions below turn a shape into the hero and candidate fields and
    // are the one place the HeroA validity rules live, so the tests exercise exactly the rules the
    // DLL applies (with fake shapes, never fake engine pointers).
    struct KitShape
    {
        std::string id;
        bool is_hitman_suit = false;
        std::vector<Field<int64_t>> heroa_variations; // index = charset; size = charset count
    };

    // The hero's current tuple against its kit's shape: charset within bounds, HeroA count readable,
    // variation within bounds. A failed bound is "observed: false" with the reason in the count.
    void DescribeHeroOutfit(const KitShape& p_Kit, int64_t p_Charset, int64_t p_Variation, HeroOutfit& p_Out);

    // A candidate kit's shape as a HeroA-usable target (charset 0, at least one HeroA variation).
    void DescribeCandidate(const KitShape& p_Kit, KitResolution& p_Out);

    // Encoding helpers: one observed entry per field, in the result's deterministic order.
    using Entries = std::vector<std::pair<std::string, CommandProtocol::ObservedValue>>;

    void Append(Entries& p_Out, const char* p_Name, const Field<std::string>& p_Field);
    void Append(Entries& p_Out, const char* p_Name, const Field<int64_t>& p_Field);
    void Append(Entries& p_Out, const char* p_Name, const Field<bool>& p_Field);
    void AppendStatus(Entries& p_Out, const char* p_Name, FieldStatus p_Status, const std::string& p_Reason);
}
