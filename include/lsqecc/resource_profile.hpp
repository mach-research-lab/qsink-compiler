#ifndef LSQECC_RESOURCE_PROFILE_HPP
#define LSQECC_RESOURCE_PROFILE_HPP

#include <lsqecc/rtls/metadata.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lsqecc {

struct ResourceCapacityEvent
{
    std::string arena;
    std::string phase;
    uint64_t wave = 0;
    uint64_t size_before = 0;
    uint64_t capacity_before = 0;
    uint64_t size_after = 0;
    uint64_t capacity_after = 0;
    uint64_t element_size = 0;
    uint64_t estimated_relocated_inline_bytes = 0;
    uint64_t max_rss_kib = 0;
};

/**
 * Disabled-by-default Phase V2-0 diagnostic counters.
 *
 * A ResourceProfile is owned by one compiler invocation and passed explicitly
 * to instrumented components. Normal compilation passes nullptr, so no global
 * state survives between in-process compiler calls.
 */
struct ResourceProfile
{
    std::string mode;
    std::string pipeline;

    uint64_t source_gates = 0;
    uint64_t qubit_touches = 0;
    uint64_t optimized_operations = 0;
    uint64_t corrections = 0;
    uint64_t split_cnots = 0;
    uint64_t frame_updates = 0;

    uint64_t lowerer_estimate = 0;
    bool lowerer_upper_bound_known = false;
    uint64_t lowerer_upper_bound = 0;

    uint64_t initial_unique_nodes = 0;
    uint64_t generated_followup_nodes = 0;
    uint64_t successful_applications = 0;
    uint64_t self_reschedules = 0;
    uint64_t retries = 0;
    uint64_t deferrals = 0;
    uint64_t waves = 0;
    uint64_t slices = 0;

    uint64_t record_element_size = 0;
    uint64_t record_size_after_reserve = 0;
    uint64_t record_capacity_after_reserve = 0;
    uint64_t record_final_size = 0;
    uint64_t record_final_capacity = 0;
    uint64_t dependency_count_final_size = 0;
    uint64_t dependency_count_final_capacity = 0;
    uint64_t dependency_edge_element_size = 0;
    uint64_t dependency_edge_final_size = 0;
    uint64_t dependency_edge_final_capacity = 0;
    uint64_t dependency_token_entries = 0;
    uint64_t dependency_token_producers = 0;
    uint64_t dependency_token_consumers = 0;

    uint64_t layout_cells = 0;
    uint64_t live_patches = 0;
    uint64_t live_patch_high_water = 0;
    uint64_t patch_arena_slots = 0;
    uint64_t patch_arena_slot_high_water = 0;

    uint64_t metadata_full_lattice_scans = 0;
    uint64_t metadata_cells_visited = 0;
    uint64_t metadata_patch_arena_scans = 0;
    uint64_t metadata_patches_visited = 0;
    uint64_t pending_frame_update_insertions = 0;
    uint64_t pending_frame_update_releases = 0;
    uint64_t pending_frame_update_past_anchor_insertions = 0;
    uint64_t pending_frame_updates_current = 0;
    uint64_t pending_frame_update_high_water = 0;
    uint64_t pending_frame_updates_final_outstanding = 0;

    uint64_t routing_searches = 0;
    uint64_t indexed_patch_lookups = 0;
    uint64_t full_grid_patch_lookups = 0;
    uint64_t full_grid_patch_lookup_cells = 0;
    uint64_t routing_scratch_allocations = 0;
    uint64_t routing_result_allocations = 0;
    uint64_t routing_predecessor_capacity_high_water = 0;
    uint64_t routing_frontier_capacity_high_water = 0;
    uint64_t routing_path_capacity_high_water = 0;

    uint64_t source_statements = 0;
    uint64_t provenance_links = 0;
    uint64_t compact_metadata_records = 0;
    uint64_t serialized_metadata_records = 0;
    uint64_t tracker_first_applied_entries = 0;
    uint64_t tracker_first_visible_entries = 0;
    uint64_t tracker_instruction_events = 0;
    uint64_t tracker_visibility_events = 0;
    uint64_t output_bytes = 0;

    uint64_t parse_decompose_ns = 0;
    uint64_t optimize_materialize_ns = 0;
    uint64_t lower_dag_construction_ns = 0;
    uint64_t routing_scheduling_ns = 0;
    uint64_t render_publication_ns = 0;

    uint64_t process_max_rss_kib = 0;
    std::vector<ResourceCapacityEvent> capacity_events;

    void note_operation_visible(OperationHandle operation_handle);
    bool operation_was_visible(OperationHandle operation_handle) const;
    void note_pending_frame_update_inserted(bool anchor_was_already_visible);
    void note_pending_frame_updates_released(size_t count);
    void note_patch_added(size_t arena_slots);
    void note_patch_removed();
    void note_capacity_event(
            std::string arena,
            std::string phase,
            uint64_t wave,
            size_t size_before,
            size_t capacity_before,
            size_t size_after,
            size_t capacity_after,
            size_t element_size,
            bool force = false,
            bool relocates_existing = true);
    void update_process_max_rss();
    std::string render_json() const;

private:
    std::vector<bool> operation_ever_visible_;
};

uint64_t resource_profile_max_rss_kib();
uint64_t resource_profile_elapsed_ns(uint64_t start_ns);
uint64_t resource_profile_now_ns();

}

#endif
