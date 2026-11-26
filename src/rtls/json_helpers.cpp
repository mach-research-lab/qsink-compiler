#include <lsqecc/rtls/json_helpers.hpp>

#include <algorithm>
#include <stdexcept>
#include <variant>
#include <vector>

namespace lsqecc {

namespace {

bool is_dead_ancilla_cell(const nlohmann::json& cell)
{
    if(cell.value("patch_type", "") != "Ancilla")
        return false;
    if(!cell.contains("edges") || !cell["edges"].is_object())
        return false;
    return cell["edges"].value("Top", "") == "None" &&
           cell["edges"].value("Bottom", "") == "None" &&
           cell["edges"].value("Left", "") == "None" &&
           cell["edges"].value("Right", "") == "None";
}

bool keep_cell_in_clean_correction_slice(const nlohmann::json& cell)
{
    if(!cell.is_object())
        return false;
    const std::string patch_type = cell.value("patch_type", "");
    return patch_type == "Qubit" ||
           patch_type == "DistillationQubit" ||
           is_dead_ancilla_cell(cell);
}

bool slice_has_cell_at(const nlohmann::json& slice_json, int row, int col)
{
    if(row < 0 || col < 0 || row >= static_cast<int>(slice_json.size()))
        return false;
    if(!slice_json[row].is_array() || col >= static_cast<int>(slice_json[row].size()))
        return false;
    return slice_json[row][col].is_object();
}

nlohmann::json idle_qubit_support_cell()
{
    return {
            {"edges", {
                    {"Top", "Solid"},
                    {"Bottom", "Solid"},
                    {"Left", "Dashed"},
                    {"Right", "Dashed"},
            }},
            {"patch_type", "Qubit"},
            {"text", ""},
            {"activity", {{"activity_type", nullptr}}},
    };
}

} // namespace

nlohmann::json rtls_latency_to_json(const RtlsCorrectionLatency& latency)
{
    return std::visit([](const auto& value) {
        return nlohmann::json(value);
    }, latency);
}

void add_rtls_metadata_to_activity(
        nlohmann::json& activity,
        const RtlsOperationMetadata& metadata)
{
    activity["operation"] = metadata.operation;
    activity["operation-id"] = metadata.operation_id;
    if(metadata.conditional_correction_id)
        activity["conditional-correction-id"] = *metadata.conditional_correction_id;
    if(metadata.conditional_correction_operation_id)
        activity["conditional-correction-operation-id"] = *metadata.conditional_correction_operation_id;
    if(metadata.conditional_correction_latency)
        activity["conditional-correction-latency"] = rtls_latency_to_json(*metadata.conditional_correction_latency);
    if(metadata.conditional_correction_anchor_operation_id)
        activity["conditional-correction-anchor-operation-id"] =
                *metadata.conditional_correction_anchor_operation_id;
    if(metadata.conditional_correction_post_anchor_operation_id)
        activity["conditional-correction-post-anchor-operation-id"] =
                *metadata.conditional_correction_post_anchor_operation_id;
    if(metadata.conditional)
        activity["conditional"] = true;
    if(metadata.correction_for_operation_id)
        activity["correction-for-operation-id"] = *metadata.correction_for_operation_id;
    if(metadata.condition_source_slice)
        activity["condition-source-slice"] = *metadata.condition_source_slice;
    if(metadata.correction_anchor_operation_id)
        activity["correction-anchor-operation-id"] = *metadata.correction_anchor_operation_id;
    if(metadata.correction_post_anchor_operation_id)
        activity["correction-post-anchor-operation-id"] = *metadata.correction_post_anchor_operation_id;
}

bool rtls_is_active_cell(const nlohmann::json& cell)
{
    return cell.is_object() &&
           cell.contains("activity") &&
           cell["activity"].is_object() &&
           cell["activity"].contains("activity_type") &&
           !cell["activity"]["activity_type"].is_null();
}

void clear_rtls_activity_metadata(nlohmann::json& activity)
{
    static const std::vector<std::string> rtls_fields{
            "operation",
            "operation-id",
            "conditional-correction-id",
            "conditional-correction-operation-id",
            "conditional-correction-latency",
            "conditional-correction-anchor-operation-id",
            "conditional-correction-post-anchor-operation-id",
            "conditional",
            "correction-for-operation-id",
            "condition-source-slice",
            "correction-anchor-operation-id",
            "correction-post-anchor-operation-id"};

    for(const auto& field : rtls_fields)
        activity.erase(field);
}

nlohmann::json idle_rtls_slice_json_from_json(const nlohmann::json& base_slice_json)
{
    nlohmann::json out = base_slice_json;
    for(auto& row : out)
    {
        for(auto& cell : row)
        {
            if(!cell.is_object())
                continue;
            if(!cell.contains("activity") || !cell["activity"].is_object())
                cell["activity"] = nlohmann::json::object();
            cell["activity"]["activity_type"] = nullptr;
            clear_rtls_activity_metadata(cell["activity"]);
        }
    }
    return out;
}

nlohmann::json clean_rtls_correction_slice_json_from_json(const nlohmann::json& base_slice_json)
{
    nlohmann::json out = base_slice_json;
    for(auto& row : out)
    {
        for(auto& cell : row)
        {
            if(!keep_cell_in_clean_correction_slice(cell))
            {
                cell = nullptr;
                continue;
            }
            if(!cell.contains("activity") || !cell["activity"].is_object())
                cell["activity"] = nlohmann::json::object();
            cell["activity"]["activity_type"] = nullptr;
            clear_rtls_activity_metadata(cell["activity"]);
        }
    }
    return out;
}

void add_rtls_vertical_support_cell(
        nlohmann::json& slice_json,
        const nlohmann::json& occupancy_slice_json,
        int target_row,
        int target_col)
{
    if(target_row < 0 ||
       target_col < 0 ||
       target_row >= static_cast<int>(slice_json.size()) ||
       !slice_json[target_row].is_array() ||
       target_col >= static_cast<int>(slice_json[target_row].size()))
        throw std::runtime_error{"RTLS correction target cell is outside the slice"};

    int support_row = -1;
    const int below = target_row + 1;
    const int above = target_row - 1;
    if(below < static_cast<int>(slice_json.size()) &&
       !slice_has_cell_at(occupancy_slice_json, below, target_col))
        support_row = below;
    else if(above >= 0)
        support_row = above;
    else
        return;

    if(!slice_json[support_row].is_array() ||
       target_col >= static_cast<int>(slice_json[support_row].size()))
        throw std::runtime_error{"RTLS correction support cell is outside the slice"};

    auto& support_cell = slice_json[support_row][target_col];
    if(support_cell.is_object() && rtls_is_active_cell(support_cell))
        return;
    support_cell = idle_qubit_support_cell();
}

std::string rtls_active_operation_name_from_activity(const nlohmann::json& activity)
{
    if(!activity.contains("operation") || !activity["operation"].is_string())
        throw std::runtime_error{"RTLS active cell missing activity.operation"};
    return activity["operation"];
}

std::string rtls_active_operation_id_from_activity(const nlohmann::json& activity)
{
    if(!activity.contains("operation-id") || !activity["operation-id"].is_string())
        throw std::runtime_error{"RTLS active cell missing activity.operation-id"};
    return activity["operation-id"];
}

bool rtls_is_pauli_operation_name(const std::string& operation)
{
    return operation == "X" || operation == "Y" || operation == "Z";
}

nlohmann::json rtls_metadata_signature(const nlohmann::json& activity)
{
    static const std::vector<std::string> rtls_fields{
            "operation",
            "operation-id",
            "conditional-correction-id",
            "conditional-correction-operation-id",
            "conditional-correction-latency",
            "conditional-correction-anchor-operation-id",
            "conditional-correction-post-anchor-operation-id",
            "conditional",
            "correction-for-operation-id",
            "condition-source-slice",
            "correction-anchor-operation-id",
            "correction-post-anchor-operation-id"};

    nlohmann::json signature = nlohmann::json::object();
    for(const std::string& field : rtls_fields)
        if(activity.contains(field))
            signature[field] = activity[field];
    return signature;
}

}
