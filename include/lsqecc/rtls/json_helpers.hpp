#ifndef LSQECC_RTLS_JSON_HELPERS_HPP
#define LSQECC_RTLS_JSON_HELPERS_HPP

#include <lsqecc/rtls/metadata.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace lsqecc {

nlohmann::json rtls_latency_to_json(const RtlsCorrectionLatency& latency);

void add_rtls_metadata_to_activity(
        nlohmann::json& activity,
        const RtlsOperationMetadata& metadata);

bool rtls_is_active_cell(const nlohmann::json& cell);

void clear_rtls_activity_metadata(nlohmann::json& activity);

nlohmann::json idle_rtls_slice_json_from_json(const nlohmann::json& base_slice_json);

nlohmann::json clean_rtls_correction_slice_json_from_json(const nlohmann::json& base_slice_json);

void add_rtls_vertical_support_cell(
        nlohmann::json& slice_json,
        const nlohmann::json& occupancy_slice_json,
        int target_row,
        int target_col);

std::string rtls_active_operation_name_from_activity(const nlohmann::json& activity);

std::string rtls_active_operation_id_from_activity(const nlohmann::json& activity);

bool rtls_is_pauli_operation_name(const std::string& operation);

nlohmann::json rtls_metadata_signature(const nlohmann::json& activity);

}

#endif
