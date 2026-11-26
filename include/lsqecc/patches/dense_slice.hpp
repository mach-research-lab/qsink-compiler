#ifndef LSQECC_DENSE_SLICE_HPP
#define LSQECC_DENSE_SLICE_HPP

#include <lsqecc/patches/patches.hpp>
#include <lsqecc/layout/layout.hpp>
#include <lsqecc/patches/slice.hpp>

#include <functional>
#include <deque>
#include <limits>
#include <unordered_map>
#include <set>
#include <tsl/ordered_set.h>

namespace lsqecc
{

struct ResourceProfile;


struct DenseSlice : public Slice
{
    using Slice::Slice;

    using PatchIndex = uint32_t;
    static constexpr PatchIndex kEmptyCell = std::numeric_limits<PatchIndex>::max();

    // Empty cells carry only a compact arena index. The segmented arena keeps
    // references to patch boundaries stable while routing inserts patches.
    std::vector<PatchIndex> occupancy;
    std::set<Cell> magic_states;
    DistillationTimeMap time_to_next_magic_state_by_distillation_region;
    std::reference_wrapper<const Layout> layout;
    const OperationMetadataArena* operation_metadata_arena = nullptr;
    unsigned int predistilled_ystates_available = 0;
    std::set<Cell> EDPC_crossing_vertices;
    // std::vector<std::pair<Cell, Cell>> marked_rough_boundaries_EDPC;
    // std::vector<std::pair<Cell, Cell>> marked_smooth_boundaries_EDPC;
    std::vector<std::reference_wrapper<Boundary>> marked_rough_boundaries_EDPC;
    std::vector<std::reference_wrapper<Boundary>> marked_smooth_boundaries_EDPC;


    explicit DenseSlice(
            const Layout& layout,
            const OperationMetadataArena* operation_metadata_arena = nullptr,
            ResourceProfile* resource_profile = nullptr);
    DenseSlice(
            const Layout& layout,
            const tsl::ordered_set<PatchId>& core_qubit_ids,
            const OperationMetadataArena* operation_metadata_arena = nullptr,
            ResourceProfile* resource_profile = nullptr);

    virtual const Layout& get_layout() const override;

    using CellTraversalFunctor
        = std::function<void(const Cell&, DensePatch*)>;
    using CellTraversalConstFunctor
        = std::function<void(const Cell&, const DensePatch*)>;
    using PatchTraversalFunctor = std::function<void(DensePatch&)>;
    using PatchTraversalConstFunctor = std::function<void(const DensePatch&)>;
    void traverse_cells_mut(const CellTraversalFunctor& f);
    void traverse_cells(const CellTraversalConstFunctor& f) const;
    void traverse_patches_mut(const PatchTraversalFunctor& f);
    void traverse_patches(const PatchTraversalConstFunctor& f) const;

    std::optional<std::reference_wrapper<DensePatch>> get_patch_by_id(PatchId id);
    std::optional<std::reference_wrapper<const DensePatch>> get_patch_by_id(PatchId id) const;
    std::optional<SparsePatch> get_sparse_patch_by_id(PatchId id) const;
    std::optional<Cell> get_cell_by_id(PatchId id) const override;
    bool has_patch(PatchId id) const override;
    DensePatch* patch_at(const Cell& cell);
    const DensePatch* patch_at(const Cell& cell) const;
    void set_patch(const Cell& cell, DensePatch patch);
    void set_patch_id(const Cell& cell, std::optional<PatchId> id);
    void clear_patch(const Cell& cell);
    bool patch_index_is_consistent() const;
    size_t cell_count() const noexcept { return occupancy.size(); }
    ResourceProfile* resource_profile() const noexcept { return resource_profile_; }
    void record_final_resource_state() const;
    void note_rtls_operation_visible(OperationHandle operation_handle);
    bool rtls_operation_was_visible(OperationHandle operation_handle) const;

    struct PendingFrameUpdate
    {
        Cell target_cell;
        OperationHandle update;
    };
    void queue_pending_frame_update(
            OperationHandle anchor,
            Cell target_cell,
            OperationHandle update);
    std::vector<PendingFrameUpdate> take_pending_frame_updates(
            OperationHandle anchor);

    std::optional<std::reference_wrapper<Boundary>> get_boundary_between(const Cell& target, const Cell& neighbour);
    std::reference_wrapper<Boundary> get_boundary_between_or_fail(const Cell& target, const Cell& neighbour);
    std::optional<std::reference_wrapper<const Boundary>> get_boundary_between(const Cell& target, const Cell& neighbour) const;
    bool have_boundary_of_type_with(const Cell& target, const Cell& neighbour, PauliOperator op) const override;
    bool is_boundary_reserved(const Cell& target, const Cell& neighbour) const override;

    // Return a cell of the placed patch
    Cell place_single_cell_sparse_patch(const SparsePatch& sparse_patch, bool distillation);
    void place_sparse_patch(const SparsePatch& sparse_patch, bool distillation);

    void delete_patch_by_id(PatchId id);

    bool is_cell_free(const Cell& cell) const override;
    bool is_cell_free_or_activity(
            const Cell& cell,
            std::initializer_list<PatchActivity> activities) const override;

    std::optional<Cell> get_directional_neighbor_within_slice(const Cell& cell, CellDirection dir) const override;
    std::vector<Cell> get_neigbours_within_slice(const Cell& cell) const override;

    SurfaceCodeTimestep time_to_next_magic_state(size_t distillation_region_id) const override;

    void flip_crossing_chain(const Cell& crossing_cell, CellDirection dir);

    BoundaryType mark_boundaries_for_crossing_cell(DensePatch& dp, const SingleCellOccupiedByPatch& p, const Cell& prev);

private:
    size_t column_count_ = 0;
    std::deque<std::optional<DensePatch>> patches_;
    std::vector<PatchIndex> free_patch_indices_;
    std::unordered_map<PatchId, std::vector<Cell>> patch_cells_by_id_;
    ResourceProfile* resource_profile_ = nullptr;
    std::vector<bool> rtls_operations_ever_visible_;
    std::unordered_map<OperationHandle, std::vector<PendingFrameUpdate>>
            pending_frame_updates_by_anchor_;

    size_t cell_index(const Cell& cell) const;
    void index_patch_id(const DensePatch& patch, const Cell& cell);
    void unindex_patch_id(const DensePatch& patch, const Cell& cell);
};

}

#endif //LSQECC_DENSE_SLICE_HPP
