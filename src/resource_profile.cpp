#include <lsqecc/resource_profile.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <sys/resource.h>

namespace lsqecc {

namespace {

using Json = nlohmann::json;

}

uint64_t resource_profile_max_rss_kib()
{
    rusage usage{};
    if(getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
    return static_cast<uint64_t>(usage.ru_maxrss);
}

uint64_t resource_profile_now_ns()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t resource_profile_elapsed_ns(uint64_t start_ns)
{
    return resource_profile_now_ns() - start_ns;
}

void ResourceProfile::note_operation_visible(OperationHandle operation_handle)
{
    const size_t required = static_cast<size_t>(operation_handle) + 1;
    if(operation_ever_visible_.size() < required)
        operation_ever_visible_.resize(required, false);
    operation_ever_visible_[operation_handle] = true;
}

bool ResourceProfile::operation_was_visible(OperationHandle operation_handle) const
{
    return operation_handle < operation_ever_visible_.size() &&
           operation_ever_visible_[operation_handle];
}

void ResourceProfile::note_pending_frame_update_inserted(
        bool anchor_was_already_visible)
{
    pending_frame_update_insertions++;
    pending_frame_updates_current++;
    pending_frame_update_high_water = std::max(
            pending_frame_update_high_water,
            pending_frame_updates_current);
    if(anchor_was_already_visible)
        pending_frame_update_past_anchor_insertions++;
}

void ResourceProfile::note_pending_frame_updates_released(size_t count)
{
    pending_frame_update_releases += count;
    pending_frame_updates_current = count > pending_frame_updates_current
            ? 0
            : pending_frame_updates_current - count;
}

void ResourceProfile::note_patch_added(size_t arena_slots)
{
    live_patches++;
    live_patch_high_water = std::max(live_patch_high_water, live_patches);
    patch_arena_slots = arena_slots;
    patch_arena_slot_high_water = std::max(
            patch_arena_slot_high_water,
            patch_arena_slots);
}

void ResourceProfile::note_patch_removed()
{
    if(live_patches > 0)
        live_patches--;
}

void ResourceProfile::note_capacity_event(
        std::string arena,
        std::string phase,
        uint64_t wave,
        size_t size_before,
        size_t capacity_before,
        size_t size_after,
        size_t capacity_after,
        size_t element_size,
        bool force,
        bool relocates_existing)
{
    if(!force && capacity_before == capacity_after)
        return;
    const uint64_t relocated = capacity_before == capacity_after ||
                               !relocates_existing
            ? 0
            : static_cast<uint64_t>(size_before) * element_size;
    const uint64_t rss = resource_profile_max_rss_kib();
    process_max_rss_kib = std::max(process_max_rss_kib, rss);
    capacity_events.push_back(ResourceCapacityEvent{
            .arena = std::move(arena),
            .phase = std::move(phase),
            .wave = wave,
            .size_before = size_before,
            .capacity_before = capacity_before,
            .size_after = size_after,
            .capacity_after = capacity_after,
            .element_size = element_size,
            .estimated_relocated_inline_bytes = relocated,
            .max_rss_kib = rss});
}

void ResourceProfile::update_process_max_rss()
{
    process_max_rss_kib = std::max(
            process_max_rss_kib,
            resource_profile_max_rss_kib());
}

std::string ResourceProfile::render_json() const
{
    Json events = Json::array();
    for(const ResourceCapacityEvent& event : capacity_events)
    {
        events.push_back({
                {"arena", event.arena},
                {"phase", event.phase},
                {"wave", event.wave},
                {"size_before", event.size_before},
                {"capacity_before", event.capacity_before},
                {"size_after", event.size_after},
                {"capacity_after", event.capacity_after},
                {"element_size", event.element_size},
                {"estimated_relocated_inline_bytes", event.estimated_relocated_inline_bytes},
                {"max_rss_kib", event.max_rss_kib}});
    }

    Json output{
            {"schema", "qsink-resource-profile-v2-0"},
            {"mode", mode},
            {"pipeline", pipeline},
            {"optimizer", {
                    {"source_gates", source_gates},
                    {"qubit_touches", qubit_touches},
                    {"optimized_operations", optimized_operations},
                    {"corrections", corrections},
                    {"split_cnots", split_cnots},
                    {"frame_updates", frame_updates}}},
            {"lowering", {
                    {"estimate", lowerer_estimate},
                    {"upper_bound_known", lowerer_upper_bound_known},
                    {"upper_bound", lowerer_upper_bound}}},
            {"scheduler", {
                    {"initial_unique_nodes", initial_unique_nodes},
                    {"generated_followup_nodes", generated_followup_nodes},
                    {"successful_applications", successful_applications},
                    {"self_reschedules", self_reschedules},
                    {"retries", retries},
                    {"deferrals", deferrals},
                    {"waves", waves},
                    {"slices", slices},
                    {"record_element_size", record_element_size},
                    {"record_size_after_reserve", record_size_after_reserve},
                    {"record_capacity_after_reserve", record_capacity_after_reserve},
                    {"record_final_size", record_final_size},
                    {"record_final_capacity", record_final_capacity},
                    {"dependency_count_final_size", dependency_count_final_size},
                    {"dependency_count_final_capacity", dependency_count_final_capacity},
                    {"dependency_edge_element_size", dependency_edge_element_size},
                    {"dependency_edge_final_size", dependency_edge_final_size},
                    {"dependency_edge_final_capacity", dependency_edge_final_capacity},
                    {"dependency_token_entries", dependency_token_entries},
                    {"dependency_token_producers", dependency_token_producers},
                    {"dependency_token_consumers", dependency_token_consumers}}},
            {"lattice", {
                    {"layout_cells", layout_cells},
                    {"live_patches", live_patches},
                    {"live_patch_high_water", live_patch_high_water},
                    {"patch_arena_slots", patch_arena_slots},
                    {"patch_arena_slot_high_water", patch_arena_slot_high_water}}},
            {"metadata", {
                    {"full_lattice_scans", metadata_full_lattice_scans},
                    {"cells_visited", metadata_cells_visited},
                    {"patch_arena_scans", metadata_patch_arena_scans},
                    {"patches_visited", metadata_patches_visited},
                    {"pending_insertions", pending_frame_update_insertions},
                    {"pending_releases", pending_frame_update_releases},
                    {"past_anchor_insertions", pending_frame_update_past_anchor_insertions},
                    {"pending_high_water", pending_frame_update_high_water},
                    {"pending_final_outstanding", pending_frame_updates_final_outstanding}}},
            {"routing", {
                    {"searches", routing_searches},
                    {"indexed_patch_lookups", indexed_patch_lookups},
                    {"full_grid_patch_lookups", full_grid_patch_lookups},
                    {"full_grid_patch_lookup_cells", full_grid_patch_lookup_cells},
                    {"scratch_allocations", routing_scratch_allocations},
                    {"result_allocations", routing_result_allocations},
                    {"predecessor_capacity_high_water", routing_predecessor_capacity_high_water},
                    {"frontier_capacity_high_water", routing_frontier_capacity_high_water},
                    {"path_capacity_high_water", routing_path_capacity_high_water}}},
            {"provenance_and_output", {
                    {"source_statements", source_statements},
                    {"provenance_links", provenance_links},
                    {"compact_metadata_records", compact_metadata_records},
                    {"serialized_metadata_records", serialized_metadata_records},
                    {"tracker_first_applied_entries", tracker_first_applied_entries},
                    {"tracker_first_visible_entries", tracker_first_visible_entries},
                    {"tracker_instruction_events", tracker_instruction_events},
                    {"tracker_visibility_events", tracker_visibility_events},
                    {"output_bytes", output_bytes}}},
            {"timing_ns", {
                    {"parse_decompose", parse_decompose_ns},
                    {"optimize_materialize", optimize_materialize_ns},
                    {"lower_dag_construction", lower_dag_construction_ns},
                    {"routing_scheduling", routing_scheduling_ns},
                    {"render_publication", render_publication_ns}}},
            {"process_max_rss_kib", process_max_rss_kib},
            {"capacity_events", std::move(events)}};
    return output.dump(2) + "\n";
}

}
