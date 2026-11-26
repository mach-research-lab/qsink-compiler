#ifndef LSQECC_RTLS_METADATA_HPP
#define LSQECC_RTLS_METADATA_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include <charconv>
#include <cstdio>

namespace lsqecc {

using RtlsCorrectionLatency = std::variant<int, std::string>;

/**
 * Metadata emitted inside RTLS activity objects for active operation cells.
 *
 * Field names intentionally use normal C++ identifiers here. The RTLS JSON
 * writer converts them to the contract field names, such as `operation-id`.
 */
struct RtlsOperationMetadata
{
    std::string operation;
    std::string operation_id;
    std::optional<int> conditional_correction_id;
    std::optional<std::string> conditional_correction_operation_id;
    std::optional<RtlsCorrectionLatency> conditional_correction_latency;
    std::optional<std::string> conditional_correction_anchor_operation_id;
    std::optional<std::string> conditional_correction_post_anchor_operation_id;
    bool conditional = false;
    std::optional<std::string> correction_for_operation_id;
    std::optional<int> condition_source_slice;
    std::optional<std::string> correction_anchor_operation_id;
    std::optional<std::string> correction_post_anchor_operation_id;

    bool operator==(const RtlsOperationMetadata&) const = default;
};

using OperationHandle = uint32_t;
inline constexpr OperationHandle kInvalidOperationHandle =
        std::numeric_limits<OperationHandle>::max();

enum class RtlsOperationCode : uint8_t
{
    Unknown,
    X, Y, Z, H, S, Sdg, Rz, Reset, CNOT,
    TStar, TStardg, CondS, CondSdg, CondH, Mz, Mx
};

inline RtlsOperationCode rtls_operation_code(std::string_view operation)
{
    if(operation == "X") return RtlsOperationCode::X;
    if(operation == "Y") return RtlsOperationCode::Y;
    if(operation == "Z") return RtlsOperationCode::Z;
    if(operation == "H") return RtlsOperationCode::H;
    if(operation == "S") return RtlsOperationCode::S;
    if(operation == "Sdg") return RtlsOperationCode::Sdg;
    if(operation == "Rz") return RtlsOperationCode::Rz;
    if(operation == "Reset") return RtlsOperationCode::Reset;
    if(operation == "CNOT") return RtlsOperationCode::CNOT;
    if(operation == "TStar") return RtlsOperationCode::TStar;
    if(operation == "TStardg") return RtlsOperationCode::TStardg;
    if(operation == "CondS") return RtlsOperationCode::CondS;
    if(operation == "CondSdg") return RtlsOperationCode::CondSdg;
    if(operation == "CondH") return RtlsOperationCode::CondH;
    if(operation == "Mz") return RtlsOperationCode::Mz;
    if(operation == "Mx") return RtlsOperationCode::Mx;
    return RtlsOperationCode::Unknown;
}

inline std::string_view rtls_operation_code_name(RtlsOperationCode operation)
{
    switch(operation)
    {
    case RtlsOperationCode::X: return "X";
    case RtlsOperationCode::Y: return "Y";
    case RtlsOperationCode::Z: return "Z";
    case RtlsOperationCode::H: return "H";
    case RtlsOperationCode::S: return "S";
    case RtlsOperationCode::Sdg: return "Sdg";
    case RtlsOperationCode::Rz: return "Rz";
    case RtlsOperationCode::Reset: return "Reset";
    case RtlsOperationCode::CNOT: return "CNOT";
    case RtlsOperationCode::TStar: return "TStar";
    case RtlsOperationCode::TStardg: return "TStardg";
    case RtlsOperationCode::CondS: return "CondS";
    case RtlsOperationCode::CondSdg: return "CondSdg";
    case RtlsOperationCode::CondH: return "CondH";
    case RtlsOperationCode::Mz: return "Mz";
    case RtlsOperationCode::Mx: return "Mx";
    case RtlsOperationCode::Unknown: return "Unknown";
    }
    return "Unknown";
}

inline uint32_t compact_operation_id(std::string_view operation_id)
{
    if(!operation_id.starts_with("op_") || operation_id.size() == 3)
        throw std::invalid_argument{"Invalid RTLS operation ID"};
    uint32_t result = 0;
    const char* first = operation_id.data() + 3;
    const char* last = operation_id.data() + operation_id.size();
    const auto parsed = std::from_chars(first, last, result);
    if(parsed.ec != std::errc{} || parsed.ptr != last || result == 0)
        throw std::invalid_argument{"Invalid RTLS operation ID"};
    return result;
}

inline uint32_t compact_operation_id_or_hash(std::string_view operation_id)
{
    try { return compact_operation_id(operation_id); }
    catch(const std::invalid_argument&)
    {
        uint32_t hash = 2166136261u;
        for(unsigned char byte : operation_id)
        {
            hash ^= byte;
            hash *= 16777619u;
        }
        return (hash | 0x80000000u) == 0xffffffffu
                ? 0xfffffffeu
                : hash | 0x80000000u;
    }
}

inline std::string serialize_compact_operation_id(uint32_t operation_id)
{
    if(operation_id == 0)
        throw std::invalid_argument{"Cannot serialize an invalid RTLS operation ID"};
    char buffer[32];
    const int written = std::snprintf(
            buffer, sizeof(buffer), "op_%06u", operation_id);
    if(written < 0 || static_cast<size_t>(written) >= sizeof(buffer))
        throw std::overflow_error{"RTLS operation ID serialization overflow"};
    return std::string{buffer, static_cast<size_t>(written)};
}

struct CompactRtlsOperationMetadata
{
    RtlsOperationCode operation = RtlsOperationCode::Unknown;
    uint32_t operation_id = 0;
    uint32_t conditional_correction_operation_id = 0;
    uint32_t conditional_correction_anchor_operation_id = 0;
    uint32_t conditional_correction_post_anchor_operation_id = 0;
    uint32_t correction_for_operation_id = 0;
    uint32_t correction_anchor_operation_id = 0;
    uint32_t correction_post_anchor_operation_id = 0;
    bool conditional = false;
};

/**
 * Compilation-local storage for string-bearing RTLS operation metadata.
 *
 * Scheduler instructions and lattice patches carry only an OperationHandle.
 * Metadata is resolved only by observers and output renderers.
 */
class OperationMetadataArena
{
public:
    void set_retain_serialized_metadata(bool retain)
    {
        if(!records_.empty())
            throw std::logic_error{
                    "RTLS metadata retention mode must be selected before insertion"};
        retain_serialized_metadata_ = retain;
    }

    OperationHandle add(RtlsOperationMetadata metadata)
    {
        if(records_.size() >= kInvalidOperationHandle)
            throw std::overflow_error{"RTLS operation metadata arena exhausted"};
        const OperationHandle handle = static_cast<OperationHandle>(records_.size());
        uint32_t compact_id = compact_operation_id_or_hash(metadata.operation_id);
        if(!retain_serialized_metadata_)
            compact_id = compact_operation_id(metadata.operation_id);
        CompactRtlsOperationMetadata compact{
                .operation = rtls_operation_code(metadata.operation),
                .operation_id = compact_id,
                .conditional_correction_operation_id =
                        metadata.conditional_correction_operation_id
                                ? compact_operation_id_or_hash(*metadata.conditional_correction_operation_id)
                                : 0,
                .conditional_correction_anchor_operation_id =
                        metadata.conditional_correction_anchor_operation_id
                                ? compact_operation_id_or_hash(*metadata.conditional_correction_anchor_operation_id)
                                : 0,
                .conditional_correction_post_anchor_operation_id =
                        metadata.conditional_correction_post_anchor_operation_id
                                ? compact_operation_id_or_hash(*metadata.conditional_correction_post_anchor_operation_id)
                                : 0,
                .correction_for_operation_id = metadata.correction_for_operation_id
                        ? compact_operation_id_or_hash(*metadata.correction_for_operation_id)
                        : 0,
                .correction_anchor_operation_id = metadata.correction_anchor_operation_id
                        ? compact_operation_id_or_hash(*metadata.correction_anchor_operation_id)
                        : 0,
                .correction_post_anchor_operation_id = metadata.correction_post_anchor_operation_id
                        ? compact_operation_id_or_hash(*metadata.correction_post_anchor_operation_id)
                        : 0,
                .conditional = metadata.conditional};
        records_.push_back(compact);
        if(retain_serialized_metadata_)
        {
            handles_by_serialized_operation_id_.try_emplace(
                    metadata.operation_id, handle);
            serialized_records_.push_back(std::move(metadata));
        }
        handles_by_operation_id_.try_emplace(compact.operation_id, handle);
        return handle;
    }

    const RtlsOperationMetadata& at(OperationHandle handle) const
    {
        if(!retain_serialized_metadata_)
            throw std::logic_error{"String RTLS metadata was not retained"};
        return serialized_records_.at(handle);
    }

    RtlsOperationMetadata& at(OperationHandle handle)
    {
        if(!retain_serialized_metadata_)
            throw std::logic_error{"String RTLS metadata was not retained"};
        return serialized_records_.at(handle);
    }

    const CompactRtlsOperationMetadata& compact_at(OperationHandle handle) const
    {
        return records_.at(handle);
    }

    size_t size() const noexcept
    {
        return records_.size();
    }

    std::optional<OperationHandle> find(std::string_view operation_id) const
    {
        if(retain_serialized_metadata_)
        {
            const auto serialized = handles_by_serialized_operation_id_.find(
                    std::string{operation_id});
            if(serialized != handles_by_serialized_operation_id_.end())
                return serialized->second;
        }
        uint32_t compact_id = 0;
        try { compact_id = compact_operation_id(operation_id); }
        catch(const std::invalid_argument&) { return std::nullopt; }
        return find(compact_id);
    }

    std::optional<OperationHandle> find(uint32_t operation_id) const
    {
        const auto found = handles_by_operation_id_.find(operation_id);
        if(found == handles_by_operation_id_.end())
            return std::nullopt;
        return found->second;
    }

    size_t serialized_record_count() const noexcept
    {
        return serialized_records_.size();
    }

private:
    std::vector<CompactRtlsOperationMetadata> records_;
    std::vector<RtlsOperationMetadata> serialized_records_;
    std::unordered_map<uint32_t, OperationHandle> handles_by_operation_id_;
    std::unordered_map<std::string, OperationHandle>
            handles_by_serialized_operation_id_;
    bool retain_serialized_metadata_ = true;
};

}

#endif
