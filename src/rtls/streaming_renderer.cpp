#include <lsqecc/rtls/streaming_renderer.hpp>

#include <lsqecc/patches/slices_to_json.hpp>
#include <lsqecc/rtls/json_helpers.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lsqecc {

namespace {

bool is_rtls_active_patch(const DensePatch* patch)
{
    return patch &&
           (patch->activity == PatchActivity::Measurement ||
            patch->activity == PatchActivity::Unitary ||
            patch->activity == PatchActivity::MultiPatchMeasurement ||
            patch->activity == PatchActivity::Rotation) &&
           patch->rtls_operation;
}

bool env_enabled(const char* name)
{
    const char* value = std::getenv(name);
    if(!value)
        return false;
    const std::string_view text{value};
    return text != "0" && text != "false" && text != "FALSE";
}

bool is_op_id(const std::string& operation_id)
{
    if(operation_id.rfind("op_", 0) != 0)
        return false;
    if(operation_id.size() == 3)
        return false;
    return std::all_of(operation_id.begin() + 3, operation_id.end(), [](unsigned char c) {
        return std::isdigit(c);
    });
}

bool is_source_t_operation(const std::string& operation)
{
    return operation == "TStar" || operation == "TStardg";
}

void validate_latency(const nlohmann::json& value)
{
    if(value.is_number_integer())
    {
        if(value.get<int>() != 0)
            throw std::runtime_error{"RTLS integer correction latency must be 0"};
        return;
    }
    if(value.is_string())
    {
        const std::string latency = value;
        if(latency != "hs" && latency != "mz")
            throw std::runtime_error{"RTLS string correction latency must be \"hs\" or \"mz\""};
        return;
    }
    throw std::runtime_error{"RTLS correction latency must be 0, \"hs\", or \"mz\""};
}

std::optional<std::string> optional_string_field(
        const nlohmann::json& activity,
        const char* field)
{
    if(!activity.contains(field))
        return std::nullopt;
    if(!activity[field].is_string())
        throw std::runtime_error{std::string{"RTLS activity."} + field + " must be a string"};
    return activity[field].get<std::string>();
}

bool latency_is_mz(const RtlsCorrectionLatency& latency)
{
    const auto* text = std::get_if<std::string>(&latency);
    return text && *text == "mz";
}

std::filesystem::path make_temp_path(
        const std::optional<std::filesystem::path>& hint_path,
        const std::string& suffix)
{
    const std::filesystem::path directory = hint_path && hint_path->has_parent_path()
            ? hint_path->parent_path()
            : std::filesystem::temp_directory_path();
    const std::string base = hint_path
            ? hint_path->filename().string()
            : std::string{"lsqecc_rtls"};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();

    for(size_t attempt = 0; attempt < 1000; attempt++)
    {
        auto candidate = directory / (base + "." + std::to_string(stamp) +
                "." + std::to_string(attempt) + suffix);
        if(!std::filesystem::exists(candidate))
            return candidate;
    }

    throw std::runtime_error{"Could not create a unique RTLS temporary path"};
}

class ScopedTemporaryPath
{
public:
    ScopedTemporaryPath() = default;

    explicit ScopedTemporaryPath(std::filesystem::path path)
        : path_(std::move(path))
    {}

    ~ScopedTemporaryPath()
    {
        cleanup();
    }

    ScopedTemporaryPath(const ScopedTemporaryPath&) = delete;
    ScopedTemporaryPath& operator=(const ScopedTemporaryPath&) = delete;

    ScopedTemporaryPath(ScopedTemporaryPath&& other) noexcept
        : path_(std::move(other.path_))
    {
        other.path_.clear();
    }

    ScopedTemporaryPath& operator=(ScopedTemporaryPath&& other) noexcept
    {
        if(this != &other)
        {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

    void release()
    {
        path_.clear();
    }

private:
    std::filesystem::path path_;

    void cleanup()
    {
        if(path_.empty() || std::getenv("LSQECC_KEEP_RTLS_SPOOL"))
            return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
};

struct OperationRange
{
    int first = -1;
    int last = -1;
};

class RtlsStreamingValidator
{
public:
    void observe_slice(const nlohmann::json& slice, int slice_index)
    {
        if(!slice.is_array())
            throw std::runtime_error{"Every RTLS schedule element must be a 2D grid"};

        for(const auto& row : slice)
        {
            if(!row.is_array())
                throw std::runtime_error{"Every RTLS schedule element must be a 2D grid"};

            for(const auto& cell : row)
            {
                if(!cell.is_null() && !cell.is_object())
                    throw std::runtime_error{"Every RTLS cell must be null or an object"};
                if(!cell.is_object())
                    continue;
                const auto& activity = cell["activity"];
                if(!activity.is_object())
                    throw std::runtime_error{"Every RTLS cell must contain an activity object"};
                if(rtls_is_active_cell(cell))
                    observe_activity(activity, slice_index, false);
                if(!activity.contains("frame-updates"))
                    continue;
                if(!activity["frame-updates"].is_array())
                    throw std::runtime_error{"RTLS activity.frame-updates must be an array"};
                for(const auto& frame_update : activity["frame-updates"])
                {
                    if(!frame_update.is_object())
                        throw std::runtime_error{"RTLS frame update must be an object"};
                    observe_activity(frame_update, slice_index, true);
                }
            }
        }
    }

    void finish(RtlsValidationContext validation_context, int final_slice_count) const
    {
        if(tstar_ids_.size() != validation_context.input_t_count)
            throw std::runtime_error{"RTLS TStar count does not match input t count"};
        if(tstardg_ids_.size() != validation_context.input_tdg_count)
            throw std::runtime_error{"RTLS TStardg count does not match input tdg count"};
        for(const std::string& operation_id : conditional_non_pauli_frame_ids_)
            if(!active_operation_ids_.contains(operation_id))
                throw std::runtime_error{
                        "RTLS standalone conditional frame update operation must be X, Y, or Z"};

        for(const auto& [source_id, source_activity] : source_activity_by_id_)
        {
            const std::string correction_id = source_activity["conditional-correction-operation-id"];
            const int correction_slice = source_activity["conditional-correction-id"];
            if(correction_slice < 0 || correction_slice >= final_slice_count)
                throw std::runtime_error{"RTLS conditional-correction-id points outside the schedule"};

            auto correction_first = first_slice_by_id_.find(correction_id);
            auto correction_last = last_slice_by_id_.find(correction_id);
            if(correction_first == first_slice_by_id_.end() ||
               correction_last == last_slice_by_id_.end())
                throw std::runtime_error{"RTLS source correction operation ID does not appear in schedule"};
            if(correction_first->second != correction_slice)
                throw std::runtime_error{"RTLS conditional-correction-id must point to the first correction slice"};
            auto source_first = first_slice_by_id_.find(source_id);
            if(source_first == first_slice_by_id_.end() ||
               correction_first->second <= source_first->second)
                throw std::runtime_error{
                        "RTLS T correction must start after its source T operation " +
                        source_id + " (source slice " +
                        std::to_string(source_first == first_slice_by_id_.end()
                                ? -1 : source_first->second) +
                        ", correction slice " +
                        std::to_string(correction_first->second) + ")"};

            auto ids_in_slice = correction_ids_by_slice_.find(correction_slice);
            if(ids_in_slice == correction_ids_by_slice_.end() ||
               !ids_in_slice->second.contains(correction_id))
                throw std::runtime_error{"RTLS source correction operation ID not present in referenced slice"};

            auto correction = correction_activity_by_id_.find(correction_id);
            if(correction == correction_activity_by_id_.end())
                throw std::runtime_error{"RTLS source points to a non-conditional correction operation"};
            if(correction->second["correction-for-operation-id"] != source_id)
                throw std::runtime_error{"RTLS correction does not point back to its source operation ID"};

            const auto source_anchor = optional_string_field(
                    source_activity,
                    "conditional-correction-anchor-operation-id");
            const auto correction_anchor = optional_string_field(
                    correction->second,
                    "correction-anchor-operation-id");
            const auto source_post_anchor = optional_string_field(
                    source_activity,
                    "conditional-correction-post-anchor-operation-id");
            const auto correction_post_anchor = optional_string_field(
                    correction->second,
                    "correction-post-anchor-operation-id");

            if(source_anchor || correction_anchor)
            {
                if(!source_anchor || !correction_anchor || *source_anchor != *correction_anchor)
                    throw std::runtime_error{"RTLS correction anchor operation ID link is inconsistent"};

                auto anchor_name = operation_name_by_id_.find(*source_anchor);
                if(anchor_name == operation_name_by_id_.end())
                    throw std::runtime_error{"RTLS correction anchor operation does not appear in schedule"};
            }

            if(source_post_anchor || correction_post_anchor)
            {
                if(!source_post_anchor || !correction_post_anchor ||
                   *source_post_anchor != *correction_post_anchor)
                    throw std::runtime_error{"RTLS correction post-anchor operation ID link is inconsistent"};

                auto post_name = operation_name_by_id_.find(*source_post_anchor);
                if(post_name == operation_name_by_id_.end())
                    throw std::runtime_error{"RTLS correction post-anchor operation does not appear in schedule"};
                auto post_first = first_slice_by_id_.find(*source_post_anchor);
                if(post_first == first_slice_by_id_.end() ||
                   !(correction_last->second < post_first->second))
                    throw std::runtime_error{"RTLS correction must appear before its post-anchor operation"};
            }

            const auto& latency = source_activity["conditional-correction-latency"];
            if(latency.is_string() && latency.get<std::string>() == "mz")
            {
                const std::string correction_operation = correction->second["operation"];
                if(correction_operation != "CondS" && correction_operation != "CondSdg")
                    throw std::runtime_error{"RTLS mz latency must point to a conditional S correction"};
                if(source_anchor || correction_anchor)
                {
                    if(!source_anchor || operation_name_by_id_.at(*source_anchor) != "Mz")
                        throw std::runtime_error{"RTLS mz correction anchor must be a visible Mz operation"};
                    if(!source_post_anchor || !correction_post_anchor)
                        throw std::runtime_error{"RTLS mz correction missing post-anchor operation ID"};
                    if(operation_name_by_id_.at(*source_post_anchor) != "Mx")
                        throw std::runtime_error{"RTLS mz correction post-anchor must be a visible Mx operation"};
                    auto anchor_last = last_slice_by_id_.find(*source_anchor);
                    auto post_first = first_slice_by_id_.find(*source_post_anchor);
                    if(anchor_last == last_slice_by_id_.end() ||
                       !(anchor_last->second <= correction_first->second))
                        throw std::runtime_error{"RTLS mz correction must not start before its Mz anchor operation"};
                    if(post_first == first_slice_by_id_.end() ||
                       !(correction_last->second < post_first->second))
                        throw std::runtime_error{"RTLS correction must appear before its post-anchor operation"};
                }
            }
        }

        for(const auto& [correction_id, correction_activity] : correction_activity_by_id_)
        {
            const std::string source_id = correction_activity["correction-for-operation-id"];
            auto source = source_activity_by_id_.find(source_id);
            if(source == source_activity_by_id_.end())
                throw std::runtime_error{"RTLS correction source is not a TStar/TStardg operation"};
            if(source->second["conditional-correction-operation-id"] != correction_id)
                throw std::runtime_error{"RTLS source/correction operation-id link is inconsistent"};
            const int condition_source_slice = correction_activity["condition-source-slice"];
            auto actual_source_slice = first_slice_by_id_.find(source_id);
            if(actual_source_slice == first_slice_by_id_.end() ||
               actual_source_slice->second != condition_source_slice)
                throw std::runtime_error{"RTLS correction condition-source-slice is incorrect"};
        }

        for(const auto& [correction_id, correction_activity] :
            auxiliary_pauli_activity_by_id_)
        {
            const auto source_id =
                    optional_string_field(correction_activity, "correction-for-operation-id");
            if(!source_id || !active_operation_ids_.contains(*source_id))
                throw std::runtime_error{
                        "RTLS auxiliary correction source operation does not appear in schedule"};
            if(!correction_activity["condition-source-slice"].is_number_integer())
                throw std::runtime_error{
                        "RTLS auxiliary correction condition-source-slice must be an integer"};
            const int condition_source_slice = correction_activity["condition-source-slice"];
            if(first_slice_by_id_.at(*source_id) != condition_source_slice)
                throw std::runtime_error{
                        "RTLS auxiliary correction condition-source-slice is incorrect"};

            const auto anchor_id =
                    optional_string_field(correction_activity, "correction-anchor-operation-id");
            if(!anchor_id || !active_operation_ids_.contains(*anchor_id))
                throw std::runtime_error{
                        "RTLS auxiliary correction anchor operation does not appear in schedule"};
            if(first_slice_by_id_.at(correction_id) < first_slice_by_id_.at(*anchor_id) ||
               last_slice_by_id_.at(correction_id) > last_slice_by_id_.at(*anchor_id))
                throw std::runtime_error{
                        "RTLS auxiliary correction must share its visible anchor slice"};
        }
    }

private:
    std::unordered_map<std::string, std::string> operation_name_by_id_;
    std::unordered_map<std::string, nlohmann::json> metadata_by_id_;
    std::unordered_map<std::string, nlohmann::json> source_activity_by_id_;
    std::unordered_map<std::string, nlohmann::json> correction_activity_by_id_;
    std::unordered_map<std::string, nlohmann::json> auxiliary_pauli_activity_by_id_;
    std::unordered_map<std::string, int> first_slice_by_id_;
    std::unordered_map<std::string, int> last_slice_by_id_;
    std::unordered_map<int, std::unordered_set<std::string>> correction_ids_by_slice_;
    std::unordered_set<std::string> active_operation_ids_;
    std::unordered_set<std::string> frame_update_operation_ids_;
    std::unordered_set<std::string> conditional_non_pauli_frame_ids_;
    std::unordered_set<std::string> tstar_ids_;
    std::unordered_set<std::string> tstardg_ids_;

    void observe_activity(
            const nlohmann::json& activity,
            int slice_index,
            bool frame_update)
    {
        const std::string operation = rtls_active_operation_name_from_activity(activity);
        const std::string operation_id = rtls_active_operation_id_from_activity(activity);

        if(!is_op_id(operation_id))
            throw std::runtime_error{
                    "RTLS operation-id must begin with op_ and contain decimal digits"};
        const bool conditional_frame =
                frame_update && activity.value("conditional", false);
        const bool auxiliary_pauli =
                conditional_frame && rtls_is_pauli_operation_name(operation);
        if(conditional_frame && !auxiliary_pauli)
            conditional_non_pauli_frame_ids_.insert(operation_id);
        if(activity.contains("conditional") && !activity["conditional"].is_boolean())
            throw std::runtime_error{"RTLS activity.conditional must be a boolean"};

        auto [name_it, name_inserted] =
                operation_name_by_id_.emplace(operation_id, operation);
        if(!name_inserted && name_it->second != operation)
            throw std::runtime_error{
                    "RTLS operation-id reused for different operation names"};

        const nlohmann::json metadata = rtls_metadata_signature(activity);
        auto [metadata_it, metadata_inserted] =
                metadata_by_id_.emplace(operation_id, metadata);
        if(!metadata_inserted && metadata_it->second != metadata)
            throw std::runtime_error{"RTLS operation-id has inconsistent metadata"};

        if(auxiliary_pauli)
        {
            if(active_operation_ids_.contains(operation_id))
                throw std::runtime_error{
                        "RTLS frame-update operation-id collides with an active operation"};
            frame_update_operation_ids_.insert(operation_id);
        }
        else if(!frame_update)
        {
            if(frame_update_operation_ids_.contains(operation_id))
                throw std::runtime_error{
                        "RTLS active operation-id collides with a frame update"};
            active_operation_ids_.insert(operation_id);
        }

        if(!first_slice_by_id_.contains(operation_id))
            first_slice_by_id_[operation_id] = slice_index;
        last_slice_by_id_[operation_id] = slice_index;

        if(frame_update)
        {
            if(auxiliary_pauli)
            {
                if(!activity.contains("correction-for-operation-id"))
                    throw std::runtime_error{
                            "RTLS auxiliary correction missing correction-for-operation-id"};
                if(!activity.contains("condition-source-slice"))
                    throw std::runtime_error{
                            "RTLS auxiliary correction missing condition-source-slice"};
                if(!activity.contains("correction-anchor-operation-id"))
                    throw std::runtime_error{
                            "RTLS auxiliary correction missing correction-anchor-operation-id"};
                auxiliary_pauli_activity_by_id_[operation_id] = activity;
            }
            return;
        }

        if(operation == "TStar")
            tstar_ids_.insert(operation_id);
        if(operation == "TStardg")
            tstardg_ids_.insert(operation_id);

        const bool conditional = activity.value("conditional", false);
        if(is_source_t_operation(operation))
        {
            if(!activity.contains("conditional-correction-id"))
                throw std::runtime_error{
                        "RTLS TStar/TStardg missing conditional-correction-id"};
            if(!activity.contains("conditional-correction-operation-id"))
                throw std::runtime_error{
                        "RTLS TStar/TStardg missing conditional-correction-operation-id"};
            if(!activity.contains("conditional-correction-latency"))
                throw std::runtime_error{
                        "RTLS TStar/TStardg missing conditional-correction-latency"};
            if(!activity["conditional-correction-id"].is_number_integer())
                throw std::runtime_error{
                        "RTLS conditional-correction-id must be an integer"};
            validate_latency(activity["conditional-correction-latency"]);
            source_activity_by_id_[operation_id] = activity;
        }

        if(conditional)
        {
            if(!activity.contains("correction-for-operation-id"))
                throw std::runtime_error{
                        "RTLS correction missing correction-for-operation-id"};
            if(!activity.contains("condition-source-slice"))
                throw std::runtime_error{"RTLS correction missing condition-source-slice"};
            correction_activity_by_id_[operation_id] = activity;
            correction_ids_by_slice_[slice_index].insert(operation_id);
        }
    }
};

} // namespace

class RtlsStreamingRenderer::Impl
{
public:
    Impl(
            std::ostream& output,
            std::vector<RtlsCorrection> corrections,
            RtlsValidationContext validation_context,
            std::optional<std::filesystem::path> spool_hint_path)
        : output_(output),
          corrections_(std::move(corrections)),
          validation_context_(validation_context),
          spool_path_(make_temp_path(spool_hint_path, ".rtls-spool"))
    {
        spool_output_.open(spool_path_.path(), std::ios::binary | std::ios::trunc);
        if(!spool_output_)
            throw std::runtime_error{"Could not open RTLS spool file for writing"};
    }

    DenseSliceVisitor slice_visitor()
    {
        return [this](const DenseSlice& slice) {
            consume_slice(slice);
        };
    }

    void finish()
    {
        if(finished_)
            return;
        finished_ = true;

        spool_output_.close();
        if(!spool_output_)
            throw std::runtime_error{"Could not finish writing RTLS spool file"};

        const auto final_indices = compute_final_indices();
        if(env_enabled("LSQECC_DEBUG_RTLS_RENDERER"))
        {
            std::error_code ignored;
            const auto spool_bytes = std::filesystem::file_size(spool_path_.path(), ignored);
            std::cerr << "[LSQECC_DEBUG_RTLS_RENDERER] base_slices=" << base_slice_count_
                      << " corrections=" << corrections_.size()
                      << " final_slices=" << final_indices.final_slice_count
                      << " spool_path=" << spool_path_.path()
                      << " spool_bytes=" << (ignored ? 0 : spool_bytes)
                      << '\n';
        }
        replay_spool(final_indices);
    }

private:
    struct FinalIndexMaps
    {
        std::unordered_map<std::string, int> source_final_slice_by_operation_id;
        std::unordered_map<std::string, int> correction_final_slice_by_source_id;
        int final_slice_count = 0;
    };

    std::ostream& output_;
    std::vector<RtlsCorrection> corrections_;
    RtlsValidationContext validation_context_;
    ScopedTemporaryPath spool_path_;
    std::ofstream spool_output_;
    size_t base_slice_count_ = 0;
    bool finished_ = false;
    std::unordered_map<std::string, OperationRange> operation_ranges_;
    std::unordered_map<std::string, std::string> operation_names_by_id_;

    void consume_slice(const DenseSlice& slice)
    {
        nlohmann::json slice_json = slice_to_json(slice);
        const std::string bytes = slice_json.dump();
        const uint64_t length = static_cast<uint64_t>(bytes.size());
        spool_output_.write(reinterpret_cast<const char*>(&length), sizeof(length));
        spool_output_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if(!spool_output_)
            throw std::runtime_error{"Could not write RTLS slice to spool"};

        const int base_slice_index = static_cast<int>(base_slice_count_);
        slice.traverse_cells([&](const Cell&, const DensePatch* patch) {
            if(!is_rtls_active_patch(patch))
                return;

            if(!slice.operation_metadata_arena)
                throw std::logic_error{"RTLS patch handle has no metadata arena"};
            const auto& metadata = slice.operation_metadata_arena->at(
                    *patch->rtls_operation);
            auto [name_it, inserted] = operation_names_by_id_.emplace(
                    metadata.operation_id,
                    metadata.operation);
            if(!inserted && name_it->second != metadata.operation)
                throw std::runtime_error{"RTLS operation-id reused for different operation names"};

            auto& range = operation_ranges_[metadata.operation_id];
            if(range.first < 0)
                range.first = base_slice_index;
            range.last = base_slice_index;
        });

        base_slice_count_++;
    }

    FinalIndexMaps compute_final_indices() const
    {
        FinalIndexMaps maps;
        maps.final_slice_count = static_cast<int>(base_slice_count_);

        for(const auto& [operation_id, range] : operation_ranges_)
        {
            if(range.first >= 0)
                maps.source_final_slice_by_operation_id[operation_id] = range.first;
        }

        for(const RtlsCorrection& correction : corrections_)
        {
            auto source_range = operation_ranges_.find(correction.source_operation_id);
            if(source_range == operation_ranges_.end() || source_range->second.first < 0)
                throw std::runtime_error{"RTLS correction source operation does not appear in schedule"};

            auto correction_range = operation_ranges_.find(correction.correction_operation_id);
            if(correction_range == operation_ranges_.end() || correction_range->second.first < 0)
                throw std::runtime_error{"RTLS conditional correction operation does not appear in schedule"};

            maps.correction_final_slice_by_source_id[correction.source_operation_id] =
                    correction_range->second.first;

            if(latency_is_mz(correction.latency))
            {
                auto anchor_range = operation_ranges_.find(correction.anchor_operation_id);
                if(anchor_range == operation_ranges_.end() || anchor_range->second.last < 0)
                    throw std::runtime_error{"RTLS mz correction anchor operation does not appear in schedule"};
                if(!correction.post_anchor_operation_id)
                    throw std::runtime_error{"RTLS mz correction missing post-anchor operation ID"};
                auto post_range = operation_ranges_.find(*correction.post_anchor_operation_id);
                if(post_range == operation_ranges_.end() || post_range->second.first < 0)
                    throw std::runtime_error{"RTLS mz correction post-anchor operation does not appear in schedule"};

                if(!(anchor_range->second.last <= correction_range->second.first &&
                     correction_range->second.last < post_range->second.first))
                {
                    std::ostringstream err;
                    err << "RTLS mz correction must not start before its Mz anchor and must finish before its Mx operation"
                        << ": source=" << correction.source_operation_id
                        << " correction=" << correction.correction_operation_id
                        << " anchor=" << correction.anchor_operation_id
                        << " anchor_range=[" << anchor_range->second.first << ","
                        << anchor_range->second.last << "]"
                        << " correction_range=[" << correction_range->second.first << ","
                        << correction_range->second.last << "]"
                        << " post_anchor=" << *correction.post_anchor_operation_id
                        << " post_range=[" << post_range->second.first << ","
                        << post_range->second.last << "]";
                    throw std::runtime_error{err.str()};
                }
            }
        }

        return maps;
    }

    nlohmann::json read_spooled_slice(std::ifstream& spool_input) const
    {
        uint64_t length = 0;
        spool_input.read(reinterpret_cast<char*>(&length), sizeof(length));
        if(!spool_input)
            throw std::runtime_error{"Could not read RTLS spool slice length"};

        std::string bytes(length, '\0');
        spool_input.read(bytes.data(), static_cast<std::streamsize>(length));
        if(!spool_input)
            throw std::runtime_error{"Could not read RTLS spool slice payload"};

        return nlohmann::json::parse(bytes);
    }

    void patch_source_links(
            nlohmann::json& slice_json,
            const FinalIndexMaps& final_indices) const
    {
        auto patch_activity = [&](nlohmann::json& activity) {
            if(!activity.contains("operation-id") || !activity["operation-id"].is_string())
                return;
            const std::string operation_id = activity["operation-id"];
            auto correction_slice =
                    final_indices.correction_final_slice_by_source_id.find(operation_id);
            if(correction_slice != final_indices.correction_final_slice_by_source_id.end())
                activity["conditional-correction-id"] = correction_slice->second;
        };

        for(auto& row : slice_json)
        {
            for(auto& cell : row)
            {
                if(!cell.is_object() || !cell.contains("activity") || !cell["activity"].is_object())
                    continue;
                auto& activity = cell["activity"];
                if(rtls_is_active_cell(cell))
                    patch_activity(activity);
                if(activity.contains("frame-updates") && activity["frame-updates"].is_array())
                    for(auto& frame_update : activity["frame-updates"])
                        if(frame_update.is_object())
                            patch_activity(frame_update);
            }
        }
    }

    void patch_correction_links(
            nlohmann::json& slice_json,
            const FinalIndexMaps& final_indices) const
    {
        auto patch_activity = [&](nlohmann::json& activity) {
            if(!activity.value("conditional", false))
                return;
            if(!activity.contains("correction-for-operation-id") ||
               !activity["correction-for-operation-id"].is_string())
                return;
            const std::string source_id = activity["correction-for-operation-id"];
            auto source_slice = final_indices.source_final_slice_by_operation_id.find(source_id);
            if(source_slice == final_indices.source_final_slice_by_operation_id.end())
                throw std::runtime_error{"RTLS correction source operation does not appear in schedule"};
            activity["condition-source-slice"] = source_slice->second;
        };

        for(auto& row : slice_json)
        {
            for(auto& cell : row)
            {
                if(!cell.is_object() || !cell.contains("activity") || !cell["activity"].is_object())
                    continue;
                auto& activity = cell["activity"];
                if(rtls_is_active_cell(cell))
                    patch_activity(activity);
                if(activity.contains("frame-updates") && activity["frame-updates"].is_array())
                    for(auto& frame_update : activity["frame-updates"])
                        if(frame_update.is_object())
                            patch_activity(frame_update);
            }
        }
    }

    void write_output_slice(
            const nlohmann::json& slice_json,
            bool& first_output_slice) const
    {
        if(first_output_slice)
        {
            output_ << "\n" << slice_json.dump(3);
            first_output_slice = false;
        }
        else
            output_ << ",\n" << slice_json.dump(3);
    }

    void replay_spool(const FinalIndexMaps& final_indices)
    {
        std::ifstream spool_input{spool_path_.path(), std::ios::binary};
        if(!spool_input)
            throw std::runtime_error{"Could not open RTLS spool file for reading"};

        RtlsStreamingValidator validator;
        bool first_output_slice = true;
        int final_slice_index = 0;
        output_ << "[";

        for(size_t base_index = 0; base_index < base_slice_count_; base_index++)
        {
            const nlohmann::json raw_base_slice_json = read_spooled_slice(spool_input);
            nlohmann::json base_slice_json = raw_base_slice_json;
            patch_source_links(base_slice_json, final_indices);
            patch_correction_links(base_slice_json, final_indices);

            write_output_slice(base_slice_json, first_output_slice);
            validator.observe_slice(base_slice_json, final_slice_index++);
        }

        if(first_output_slice)
            output_ << "]\n";
        else
            output_ << "\n]\n";

        if(final_slice_index != final_indices.final_slice_count)
            throw std::runtime_error{"RTLS emitted slice count does not match computed final slice count"};

        validator.finish(validation_context_, final_slice_index);
    }
};

RtlsStreamingRenderer::RtlsStreamingRenderer(
        std::ostream& output,
        std::vector<RtlsCorrection> corrections,
        RtlsValidationContext validation_context,
        std::optional<std::filesystem::path> spool_hint_path)
    : impl_(std::make_unique<Impl>(
            output,
            std::move(corrections),
            validation_context,
            std::move(spool_hint_path)))
{}

RtlsStreamingRenderer::~RtlsStreamingRenderer() = default;

DenseSliceVisitor RtlsStreamingRenderer::slice_visitor()
{
    return impl_->slice_visitor();
}

void RtlsStreamingRenderer::finish()
{
    impl_->finish();
}

}
