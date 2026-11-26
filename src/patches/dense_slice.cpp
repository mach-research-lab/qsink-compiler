#include <lsqecc/patches/dense_slice.hpp>
#include <lsqecc/patches/patches.hpp>
#include <lsqecc/resource_profile.hpp>

#include <algorithm>
#include <sstream>

namespace lsqecc
{




const Layout& DenseSlice::get_layout() const
{
    return layout.get();
}


void DenseSlice::traverse_cells_mut(const DenseSlice::CellTraversalFunctor& f)
{
    for(size_t index = 0; index < occupancy.size(); ++index)
    {
        const Cell cell{
                static_cast<Cell::CoordinateType>(index / column_count_),
                static_cast<Cell::CoordinateType>(index % column_count_)};
        const PatchIndex patch_index = occupancy[index];
        f(cell, patch_index == kEmptyCell ? nullptr : &patches_[patch_index].value());
    }
}

void DenseSlice::traverse_cells(const CellTraversalConstFunctor& f) const
{
    for(size_t index = 0; index < occupancy.size(); ++index)
    {
        const Cell cell{
                static_cast<Cell::CoordinateType>(index / column_count_),
                static_cast<Cell::CoordinateType>(index % column_count_)};
        const PatchIndex patch_index = occupancy[index];
        f(cell, patch_index == kEmptyCell ? nullptr : &patches_[patch_index].value());
    }
}

void DenseSlice::traverse_patches_mut(const PatchTraversalFunctor& f)
{
    for(std::optional<DensePatch>& patch : patches_)
        if(patch)
            f(*patch);
}

void DenseSlice::traverse_patches(const PatchTraversalConstFunctor& f) const
{
    for(const std::optional<DensePatch>& patch : patches_)
        if(patch)
            f(*patch);
}

void DenseSlice::note_rtls_operation_visible(OperationHandle operation_handle)
{
    const size_t required = static_cast<size_t>(operation_handle) + 1;
    if(rtls_operations_ever_visible_.size() < required)
        rtls_operations_ever_visible_.resize(required, false);
    rtls_operations_ever_visible_[operation_handle] = true;
}

bool DenseSlice::rtls_operation_was_visible(
        OperationHandle operation_handle) const
{
    return operation_handle < rtls_operations_ever_visible_.size() &&
           rtls_operations_ever_visible_[operation_handle];
}

void DenseSlice::queue_pending_frame_update(
        OperationHandle anchor,
        Cell target_cell,
        OperationHandle update)
{
    pending_frame_updates_by_anchor_[anchor].push_back({target_cell, update});
}

std::vector<DenseSlice::PendingFrameUpdate>
DenseSlice::take_pending_frame_updates(OperationHandle anchor)
{
    auto pending = pending_frame_updates_by_anchor_.find(anchor);
    if(pending == pending_frame_updates_by_anchor_.end())
        return {};
    std::vector<PendingFrameUpdate> result = std::move(pending->second);
    pending_frame_updates_by_anchor_.erase(pending);
    return result;
}

std::optional<std::reference_wrapper<DensePatch>> DenseSlice::get_patch_by_id(PatchId id)
{
    if(resource_profile_)
        resource_profile_->indexed_patch_lookups++;
    const auto location = patch_cells_by_id_.find(id);
    if(location == patch_cells_by_id_.end())
        return std::nullopt;
    if(location->second.empty())
        throw std::logic_error{"DenseSlice patch index contains an empty cell list"};
    DensePatch* const patch = patch_at(location->second.back());
    if(!patch || patch->id != id)
        throw std::logic_error{"DenseSlice patch index is inconsistent"};
    return std::ref(*patch);
}


std::optional<std::reference_wrapper<DensePatch const>> DenseSlice::get_patch_by_id(PatchId id) const
{
    auto maybe_patch = const_cast<DenseSlice*>(this)->get_patch_by_id(id);
    return maybe_patch ? std::make_optional(std::cref(maybe_patch->get())) : std::nullopt;
}

std::optional<SparsePatch> DenseSlice::get_sparse_patch_by_id(lsqecc::PatchId id) const
{
    auto dense_patch = get_patch_by_id(id);
    if(!dense_patch) return std::nullopt;
    const DensePatch& dense_patch_ref = dense_patch->get();

    return SparsePatch{
            {static_cast<Patch>(dense_patch_ref)},
            SingleCellOccupiedByPatch{
                    {dense_patch_ref.boundaries},
                    get_cell_by_id(id).value()
            },
    };
}

std::optional<Cell> DenseSlice::get_cell_by_id(PatchId id) const
{
    if(resource_profile_)
        resource_profile_->indexed_patch_lookups++;
    const auto location = patch_cells_by_id_.find(id);
    if(location == patch_cells_by_id_.end())
        return std::nullopt;
    if(location->second.empty())
        throw std::logic_error{"DenseSlice patch index contains an empty cell list"};
    const Cell canonical_cell = location->second.back();
    const DensePatch* const patch = patch_at(canonical_cell);
    if(!patch || patch->id != id)
        throw std::logic_error{"DenseSlice patch index is inconsistent"};
    return canonical_cell;
}

size_t DenseSlice::cell_index(const Cell& cell) const
{
    if(cell.row < 0 || cell.col < 0 ||
       static_cast<size_t>(cell.col) >= column_count_)
        throw std::out_of_range{"DenseSlice cell outside layout"};
    const size_t index = static_cast<size_t>(cell.row) * column_count_ +
                         static_cast<size_t>(cell.col);
    if(index >= occupancy.size())
        throw std::out_of_range{"DenseSlice cell outside layout"};
    return index;
}

DensePatch* DenseSlice::patch_at(const Cell& cell)
{
    const PatchIndex patch_index = occupancy.at(cell_index(cell));
    return patch_index == kEmptyCell ? nullptr : &patches_[patch_index].value();
}

const DensePatch* DenseSlice::patch_at(const Cell& cell) const
{
    const PatchIndex patch_index = occupancy.at(cell_index(cell));
    return patch_index == kEmptyCell ? nullptr : &patches_[patch_index].value();
}

void DenseSlice::set_patch(const Cell& cell, DensePatch patch)
{
    PatchIndex& patch_index = occupancy.at(cell_index(cell));
    if(patch_index != kEmptyCell)
    {
        unindex_patch_id(patches_[patch_index].value(), cell);
        patches_[patch_index] = std::move(patch);
        index_patch_id(patches_[patch_index].value(), cell);
        return;
    }

    if(free_patch_indices_.empty())
    {
        assert(patches_.size() < kEmptyCell);
        patch_index = static_cast<PatchIndex>(patches_.size());
        patches_.emplace_back(std::move(patch));
    }
    else
    {
        patch_index = free_patch_indices_.back();
        free_patch_indices_.pop_back();
        patches_[patch_index].emplace(std::move(patch));
    }
    index_patch_id(patches_[patch_index].value(), cell);
    if(resource_profile_)
        resource_profile_->note_patch_added(patches_.size());
}

void DenseSlice::set_patch_id(const Cell& cell, std::optional<PatchId> id)
{
    DensePatch* const patch = patch_at(cell);
    if(!patch)
        throw std::logic_error{"Cannot assign an ID to an empty DenseSlice cell"};
    if(patch->id == id)
        return;
    unindex_patch_id(*patch, cell);
    patch->id = id;
    index_patch_id(*patch, cell);
}

void DenseSlice::clear_patch(const Cell& cell)
{
    PatchIndex& patch_index = occupancy.at(cell_index(cell));
    if(patch_index == kEmptyCell)
        return;
    unindex_patch_id(patches_[patch_index].value(), cell);
    patches_[patch_index].reset();
    free_patch_indices_.push_back(patch_index);
    patch_index = kEmptyCell;
    if(resource_profile_)
        resource_profile_->note_patch_removed();
}

void DenseSlice::delete_patch_by_id(PatchId id)
{
    if(resource_profile_)
        resource_profile_->indexed_patch_lookups++;
    const auto location = patch_cells_by_id_.find(id);
    if(location != patch_cells_by_id_.end())
    {
        const std::vector<Cell> cells = location->second;
        for(const Cell& cell : cells)
            clear_patch(cell);
    }
}

void DenseSlice::index_patch_id(const DensePatch& patch, const Cell& cell)
{
    if(!patch.id)
        return;
    std::vector<Cell>& cells = patch_cells_by_id_[*patch.id];
    const size_t index = cell_index(cell);
    const auto position = std::lower_bound(
            cells.begin(),
            cells.end(),
            index,
            [&](const Cell& candidate, size_t target_index) {
                return cell_index(candidate) < target_index;
            });
    if(position == cells.end() || *position != cell)
        cells.insert(position, cell);
}

void DenseSlice::unindex_patch_id(const DensePatch& patch, const Cell& cell)
{
    if(!patch.id)
        return;
    const auto existing = patch_cells_by_id_.find(*patch.id);
    if(existing == patch_cells_by_id_.end())
        return;
    std::vector<Cell>& cells = existing->second;
    cells.erase(std::remove(cells.begin(), cells.end(), cell), cells.end());
    if(cells.empty())
        patch_cells_by_id_.erase(existing);
}

bool DenseSlice::patch_index_is_consistent() const
{
    std::unordered_map<PatchId, std::vector<Cell>> reconstructed;
    traverse_cells([&](const Cell& cell, const DensePatch* patch) {
        if(!patch || !patch->id)
            return;
        reconstructed[*patch->id].push_back(cell);
    });
    if(reconstructed.size() != patch_cells_by_id_.size())
        return false;
    for(const auto& [id, cells] : reconstructed)
    {
        const auto indexed = patch_cells_by_id_.find(id);
        if(indexed == patch_cells_by_id_.end() || indexed->second != cells)
            return false;
    }
    return true;
}

std::optional<Cell> DenseSlice::get_directional_neighbor_within_slice(const Cell& cell, CellDirection dir) const
{
    return cell.get_directional_neighbor({0,0}, get_layout().furthest_cell(), dir);
}

std::vector<Cell> DenseSlice::get_neigbours_within_slice(const Cell& cell) const
{
    return cell.get_neigbours_within_bounding_box_inclusive({0,0}, get_layout().furthest_cell());
}

DenseSlice::DenseSlice(
        const Layout& layout,
        const OperationMetadataArena* metadata_arena,
        ResourceProfile* resource_profile)
: occupancy(
        static_cast<size_t>(layout.furthest_cell().row + 1) *
                static_cast<size_t>(layout.furthest_cell().col + 1),
        kEmptyCell),
  layout(std::cref(layout)),
  operation_metadata_arena(metadata_arena),
  column_count_(static_cast<size_t>(layout.furthest_cell().col + 1)),
  resource_profile_(resource_profile)
{
    if(resource_profile_)
        resource_profile_->layout_cells = occupancy.size();
}

DenseSlice::DenseSlice(
        const lsqecc::Layout &layout,
        const tsl::ordered_set<PatchId> &core_qubit_ids,
        const OperationMetadataArena* metadata_arena,
        ResourceProfile* resource_profile)
 : DenseSlice(layout, metadata_arena, resource_profile)
{
    if (layout.core_patches().size()<core_qubit_ids.size())
        throw std::runtime_error("Not enough Init patches for all ids");

    auto core_qubit_ids_itr = core_qubit_ids.begin();
    for (const SparsePatch& p : layout.core_patches())
    {
        if(core_qubit_ids_itr == core_qubit_ids.end()) break;
        Cell cell = place_single_cell_sparse_patch(p,false);
        set_patch_id(cell, *core_qubit_ids_itr++);
    }

    for (const Cell& cell: layout.predistilled_y_states())
    {
        SparsePatch p = LayoutHelpers::basic_square_patch(cell);
        place_single_cell_sparse_patch(p,false);
        predistilled_ystates_available++;
    }

    for(const MultipleCellsOccupiedByPatch& distillation_region: layout.distillation_regions())
    {
        for (const SingleCellOccupiedByPatch& cell: distillation_region.sub_cells)
        {
            set_patch(cell.cell, DensePatch{
                    Patch{PatchType::Distillation,PatchActivity::Distillation,std::nullopt},
                    static_cast<CellBoundaries>(cell)});
        }
    }

    for (const Cell& cell: layout.reserved_for_magic_states()) 
    {
        set_patch(cell, DensePatch{
                Patch{PatchType::Distillation,PatchActivity::Distillation,std::nullopt},
                CellBoundaries{Boundary{BoundaryType::Connected, false},Boundary{BoundaryType::Connected, false},
                    Boundary{BoundaryType::Connected, false},Boundary{BoundaryType::Connected, false}}});
    }

    // Reserved tiles are themselves 'distillation regions'
    for(auto t : layout.distillation_times())
        time_to_next_magic_state_by_distillation_region.push_back(t);


    for(const Cell& cell: layout.dead_location())
    {
        set_patch(cell, DensePatch{
            Patch{PatchType::Dead,PatchActivity::Dead,std::nullopt},
            CellBoundaries{Boundary{BoundaryType::Connected, false},Boundary{BoundaryType::Connected, false},
                Boundary{BoundaryType::Connected, false},Boundary{BoundaryType::Connected, false}}});
    }

}

void DenseSlice::record_final_resource_state() const
{
    if(!resource_profile_)
        return;
    size_t pending = 0;
    size_t live = 0;
    for(const std::optional<DensePatch>& patch : patches_)
    {
        if(!patch)
            continue;
        live++;
        pending += patch->rtls_pending_frame_updates.size();
    }
    for(const auto& [anchor, updates] : pending_frame_updates_by_anchor_)
    {
        static_cast<void>(anchor);
        pending += updates.size();
    }
    resource_profile_->live_patches = live;
    resource_profile_->patch_arena_slots = patches_.size();
    resource_profile_->pending_frame_updates_final_outstanding = pending;
}

bool DenseSlice::is_cell_free(const Cell& cell) const
{
    return patch_at(cell) == nullptr;
}

bool DenseSlice::is_cell_free_or_activity(
        const Cell& cell,
        std::initializer_list<PatchActivity> activities) const
{
    const DensePatch* patch = patch_at(cell);

    if (!patch)
    {
        return true;
    }

    else
    {
        for (auto activity : activities)
        {
            if (patch->activity == activity)
                return true;
        }

        return false;
    }
}

Cell DenseSlice::place_single_cell_sparse_patch(const SparsePatch& sparse_patch, bool distillation)
{
    auto* occupied_cell = std::get_if<SingleCellOccupiedByPatch>(&sparse_patch.cells);
    if(!occupied_cell)
        throw std::logic_error("Placing Multi Patch cell not yet supported");
    else {
        if(!is_cell_free(occupied_cell->cell) && !distillation)
            throw std::logic_error(lstk::cat("Double patch occupation at ",occupied_cell->cell, "\n",
                    "Found patch: ", patch_at(occupied_cell->cell)->id.value_or(-1)));
    }

    set_patch(occupied_cell->cell, DensePatch::from_sparse_patch(sparse_patch));
    return occupied_cell->cell;
}
void DenseSlice::place_sparse_patch(const SparsePatch& sparse_patch, bool distillation)
{
    auto* occupied_cells = std::get_if<MultipleCellsOccupiedByPatch>(&sparse_patch.cells);
    if (!occupied_cells) 
        place_single_cell_sparse_patch(sparse_patch, distillation);
    else 
    {
        for (const SingleCellOccupiedByPatch& patch : occupied_cells->sub_cells)
            place_single_cell_sparse_patch(SparsePatch{.cells=patch}, distillation);
    }
}

bool DenseSlice::has_patch(PatchId id) const
{
    return (bool) get_patch_by_id(id);
}

std::optional<std::reference_wrapper<Boundary>> DenseSlice::get_boundary_between(
        const Cell& target, const Cell& neighbour)
{
    DensePatch* target_patch = patch_at(target);
    if(!target_patch) return std::nullopt;

    if(neighbour == Cell{target.row-1, target.col})   return target_patch->boundaries.top;
    if(neighbour == Cell{target.row+1, target.col})   return target_patch->boundaries.bottom;
    if(neighbour == Cell{target.row,   target.col-1}) return target_patch->boundaries.left;
    if(neighbour == Cell{target.row,   target.col+1}) return target_patch->boundaries.right;

    return std::nullopt;

}

std::reference_wrapper<Boundary> DenseSlice::get_boundary_between_or_fail(const Cell& target, const Cell& neighbour)
{
    return lstk::get_or_throw(get_boundary_between(target, neighbour), 
        std::logic_error{lstk::cat("No boundary between cells ", target, "and ", neighbour)});
}


std::optional<std::reference_wrapper<const Boundary>> DenseSlice::get_boundary_between(
        const Cell& target, const Cell& neighbour) const
{
    std::optional<std::reference_wrapper<Boundary>> r
        = const_cast<DenseSlice*>(this)->get_boundary_between(target,neighbour);
    return r ? std::make_optional(std::cref(r->get())) : std::nullopt;
}

bool DenseSlice::have_boundary_of_type_with(const Cell& target, const Cell& neighbour, PauliOperator op) const
{
    const auto b = get_boundary_between(target, neighbour);
    return b ? b->get().boundary_type== boundary_for_operator(op) : false;
}

bool DenseSlice::is_boundary_reserved(const Cell& target, const Cell& neighbor) const 
{
    const auto b = get_boundary_between(target, neighbor);
    return b ? ((b->get().boundary_type == BoundaryType::Reserved_Label1) || (b->get().boundary_type == BoundaryType::Reserved_Label2)) : false;
}


SurfaceCodeTimestep DenseSlice::time_to_next_magic_state(size_t distillation_region_id) const
{
    return time_to_next_magic_state_by_distillation_region[distillation_region_id];
}

// Recursively flip all boundaries for an input crossing cell in EDPC. 'dir' is the direction that we came from.
void DenseSlice::flip_crossing_chain(const Cell& crossing_cell, CellDirection dir)
{
    if (!EDPC_crossing_vertices.count(crossing_cell))
        throw std::invalid_argument("DenseSlice::flip_crossing_chain: input cell must be a crossing cell.");

    // Get associated patch
    DensePatch* patch = patch_at(crossing_cell);
    if (patch)
    {  
        
        // Get boundaries and neighbors
        std::vector<Boundary*> boundary_ptrs = {&patch->boundaries.top, &patch->boundaries.bottom, &patch->boundaries.left, &patch->boundaries.right};
        std::vector<std::optional<Cell>> neighbors;
        std::vector<CellDirection> cell_directions = {CellDirection::top, CellDirection::bottom, CellDirection::left, CellDirection::right};
        for (CellDirection dir_ : cell_directions)
            neighbors.push_back(get_directional_neighbor_within_slice(crossing_cell, dir_));

        // Loop over boundaries
        size_t counter = 0;
        for (Boundary* boundary : boundary_ptrs)
        {
            if (neighbors[counter].has_value())
            {
                if ((boundary->boundary_type == BoundaryType::Reserved_Label1) || (boundary->boundary_type == BoundaryType::Reserved_Label2))   
                {

                    // Flip boundary and conjugate boundary except those corresp. to the neighbor we came from (those should have already been flipped)
                    if (cell_directions[counter] != (!dir))
                    {
                        boundary->boundary_type = !boundary->boundary_type;
                        auto conjugate_bdry = get_boundary_between(neighbors[counter].value(), crossing_cell);
                        if (conjugate_bdry.has_value()) {
                            conjugate_bdry.value().get().boundary_type = !conjugate_bdry.value().get().boundary_type;
                        } 
                    }

                } 

                else
                {
                    throw std::logic_error("DenseSlice::flip_crossing_chain: input crossing cell must have all reserved boundaries.");
                } 
            }

            else
            {
                throw std::logic_error("DenseSlice::flip_crossing_chain: input crossing cell must be interior and have four neighbors.");
            }

            counter++;
        }
    }

    else
    {
        throw std::logic_error("DenseSlice::flip_crossing_chain: input crossing cell must have an associated dense patch on the slice.");
    }

    // Assuming consistency conditions from mark_boundaries_for_crossing_cell have been met and remain unchanged for every crossing cell, we only need to propagate linearly and flip all boundaries
    std::optional<Cell> next_crossing_cell = get_directional_neighbor_within_slice(crossing_cell, dir);
    if (next_crossing_cell.has_value() && EDPC_crossing_vertices.count(next_crossing_cell.value()))
        flip_crossing_chain(next_crossing_cell.value(), dir);
}

BoundaryType DenseSlice::mark_boundaries_for_crossing_cell(DensePatch& dp, const SingleCellOccupiedByPatch& p, const Cell& prev) 
{

    // Get pointers to boundary labels (so that we can loop over them)
    std::vector<Boundary*> boundary_ptrs = {&dp.boundaries.top, &dp.boundaries.bottom, &dp.boundaries.left, &dp.boundaries.right};

    // Loop over boundary pointers to get the appropriate neighbor
    std::vector<std::optional<Cell>> neighbors;
    std::vector<CellDirection> cell_directions = {CellDirection::top, CellDirection::bottom, CellDirection::left, CellDirection::right};
    for (CellDirection dir : cell_directions)
        neighbors.push_back(get_directional_neighbor_within_slice(p.cell, dir));
    
    // Check if the previous cell routed through was a crossing cell
    bool prev_cell_was_crossing_vertex = EDPC_crossing_vertices.count(prev);

    // Create tracker for number of crossing neighbors
    size_t num_crossing_neighbors = 0;
    if (prev_cell_was_crossing_vertex)
        num_crossing_neighbors++;

    // Get boundary type with the previous cell
    auto prev_bdry = get_boundary_between(p.cell, prev);

    // Consistency check: prev_boundary must exist
    if (!prev_bdry.has_value())
        throw std::invalid_argument("DenseSlice::mark_boundaries_for_crossing_cell: input patch's cell does not share a boundary with input previously-routed cell");

    // Consistency check: prev_boundary should already be set if prev was a crossing cell
    if ((prev_cell_was_crossing_vertex) && (prev_bdry->get().boundary_type == BoundaryType::None))
        throw std::logic_error("DenseSlice::mark_boundaries_for_crossing_cell: previously routed cell is a crossing vertex but boundary is not already reserved.");

    // Loop over boundary pointers
    size_t counter = 0;
    std::vector<size_t> crossing_indices;
    Boundary* tmp = nullptr;
    size_t num_reserved_edges_found = 0;
    for (Boundary* boundary : boundary_ptrs) 
    {
        // Check whether there is a corresponding neighbor
        if (neighbors[counter].has_value())
        {

            // Filter for only previously-reserved boundaries
            if ((boundary->boundary_type == BoundaryType::Reserved_Label1) || (boundary->boundary_type == BoundaryType::Reserved_Label2))  
            {
                // Track # found reserved edges for later consistency checks
                num_reserved_edges_found++;

                // Ignore the previously-routed cell but track its index
                if (neighbors[counter].value() == prev)
                {
                    if (prev_cell_was_crossing_vertex) 
                        crossing_indices.push_back(counter);
                    else
                        throw std::logic_error("DenseSlice::mark_boundaries_for_crossing_cell: previously-routed cell not in crossing vertices but its corresponding boundary already reserved.");

                    counter++;
                    continue;                   
                }

                // Otherwise, if this is the first previously-reserved boundary we have seen, enforce consistency of boundary labels
                else if (tmp == nullptr)
                {
                    // We store the tmp boundary and its index for later
                    tmp = boundary;

                    // If the present boundary is the same type as prev_bdry, they must be made opposite
                    // (This will only be triggered if prev was a crossing vertex since it is only in that case that prev_bdry would have already reserved)
                    if (tmp->boundary_type == prev_bdry->get().boundary_type)
                    {

                        // Flip the present boundary and its conjugate
                        tmp->boundary_type = !tmp->boundary_type;
                        auto conjugate_bdry = get_boundary_between(neighbors[counter].value(), p.cell);
                        if (conjugate_bdry.has_value()) {
                            conjugate_bdry.value().get().boundary_type = !conjugate_bdry.value().get().boundary_type;
                        } 

                        // If the corresp. neighbor is also a crossing cell then we (generically) need to propagate the boundary flip through a connected chain of crossings
                        if (EDPC_crossing_vertices.count(neighbors[counter].value()))
                        {
                            crossing_indices.push_back(counter);
                            num_crossing_neighbors++;
                            flip_crossing_chain(neighbors[counter].value(), cell_directions[counter]);
                        }

                    }

                    // If they are already opposite, then we already have consistency and do nothing

                    // Or, if prev_boundary has not been reserved, then we will set it opposite to tmp later

                }


                // Since prev_bdry has already been made consistent with the first seen previously-reserved boundary,
                // the second previously-reserved boundary type must simply be made to match the first
                else if (boundary->boundary_type != tmp->boundary_type)
                {

                    // Flip the boundary and its conjugate in advance
                    boundary->boundary_type = !boundary->boundary_type;
                    auto conjugate_bdry = get_boundary_between(neighbors[counter].value(), p.cell);
                    if (conjugate_bdry.has_value()) {
                        conjugate_bdry.value().get().boundary_type = boundary->boundary_type;     
                    } 

                    // If the corresp. neighbor is also a crossing cell then we (generically) need to propagate the boundary flip through a connected chain of crossings
                    if (EDPC_crossing_vertices.count(neighbors[counter].value()))
                    {
                        crossing_indices.push_back(counter);
                        num_crossing_neighbors++;
                        flip_crossing_chain(neighbors[counter].value(), cell_directions[counter]);
                    }

                }

                // If they are already equal, then we already have consistency and do nothing

            }

            else if (boundary->boundary_type != BoundaryType::None) 
            {
                throw std::runtime_error("Invalid BoundaryType for crossing cell in EDPC.");
            }

        }

        else
        {
            throw std::logic_error("DenseSlice::mark_boundaries_for_crossing_cell: input crossing cell must be interior and have four neighbors.");
        }

        counter++;

    } 

    // Consistency check: this crossing vertex can only be adjacent to up to two other crossing vertices
    if (num_crossing_neighbors > 2)
        throw std::logic_error("DenseSlice::mark_boundaries_for_crossing_cell: a given crossing cell can only have two neighbors that are themselves crossing cells.");

    // Consistency check: two neighboring crossing cells to a crossing cell must be across from each other
    if (num_crossing_neighbors == 2)
    {
        assert(crossing_indices.size() == 2);
        if (cell_directions[crossing_indices[0]] != (!cell_directions[crossing_indices[1]]))
            throw std::logic_error("DenseSlice::mark_boundaries_for_crossing_cell: two neighboring crossing cells to a crossing cell must be across from one another.");
    }

    // Consistency checks on number of reserved edges found
    if (num_reserved_edges_found == 4)
        throw std::logic_error("DenseSlice::mark_boundaries_for_crossing_cell: too many reserved edges found.");

    else if ((num_reserved_edges_found == 3) && !prev_cell_was_crossing_vertex)
        throw std::logic_error("DenseSlice::mark_boundaries_for_crossing_cell: found three reserved edges and the previously-routed cell was not a crossing vertex.");

    else if ((num_reserved_edges_found == 2) && prev_cell_was_crossing_vertex)
        throw std::logic_error("DenseSlice::mark_boundaries_for_crossing_cell: too few reserved edges found (2 incl. bdry set by prev crossing cell).");

    else if (num_reserved_edges_found < 2)
        throw std::logic_error("DenseSlice::mark_boundaries_for_crossing_cell: too few reserved edges found (< 2).");

    // The label that we place is always opposite the one present
    BoundaryType label_to_place = !tmp->boundary_type;

    // Set remaining boundaries (and their conjugates) to label_to_place
    counter = 0;
    for (Boundary* boundary : boundary_ptrs)
    {
        if (neighbors[counter].has_value())
        {
            if (boundary->boundary_type == BoundaryType::None)
            {
                boundary->boundary_type = label_to_place;
                boundary->is_active = true;
                auto conjugate_bdry = get_boundary_between(neighbors[counter].value(), p.cell);
                if (conjugate_bdry.has_value())
                {
                    conjugate_bdry.value().get().boundary_type = label_to_place;     
                } 
            }
        }
        counter++;
    }

    return label_to_place;
}

}
