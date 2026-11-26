#include <lsqecc/layout/graph_search/custom_graph_search.hpp>
#include <lsqecc/resource_profile.hpp>

#include <algorithm>
#include <iostream>
#include <lstk/lstk.hpp>


namespace lsqecc {


namespace custom_graph_search {



// Index in the data structure storing the list of vertices
using Vertex = uint32_t;
static constexpr uint32_t kUnvisitedDistance = UINT32_MAX;

using PredecessorData = GraphSearchPredecessorData;

uint32_t distance_for_generation(
        const PredecessorData& predecessor,
        uint32_t generation)
{
    return predecessor.generation == generation
            ? predecessor.distance
            : kUnvisitedDistance;
}

Vertex predecessor_for_generation(
        const std::vector<PredecessorData>& predecessors,
        Vertex vertex,
        uint32_t generation)
{
    return predecessors[vertex].generation == generation
            ? predecessors[vertex].predecessor
            : vertex;
}

void set_predecessor(
        std::vector<PredecessorData>& predecessors,
        Vertex vertex,
        uint32_t distance,
        Vertex predecessor,
        uint32_t generation)
{
    predecessors[vertex] = PredecessorData{
            distance,
            predecessor,
            generation};
}



double euclidean_distance(Cell a, Cell b)
{
    return std::sqrt(std::pow(a.row - b.row, 2) + std::pow(a.col - b.col, 2));
}

template <class SliceSearcher, Heuristic heuristic>
struct Comparator
{
    bool operator()(const Vertex& a, const Vertex& b) const
    {
        if constexpr(heuristic == Heuristic::None)
        {
            return distance_for_generation(predecessor_map[a], generation) >
                   distance_for_generation(predecessor_map[b], generation);
        }
        else if constexpr(heuristic == Heuristic::Euclidean)
        {
            return static_cast<double>(distance_for_generation(predecessor_map[a], generation)) +
                           euclidean_distance(slice_searcher.cell_from_vertex(a), target_cell) >
                   static_cast<double>(distance_for_generation(predecessor_map[b], generation)) +
                           euclidean_distance(slice_searcher.cell_from_vertex(b), target_cell);
        }
        else
        {
            throw std::runtime_error(lstk::cat("Unknown heuristic: ", static_cast<int>(heuristic)));
        }
    }

    Cell target_cell;
    std::vector<PredecessorData>& predecessor_map;
    const SliceSearcher& slice_searcher;
    uint32_t generation;
};


template<bool want_cycle>
class SliceSearchAdaptor
{
public:
    explicit SliceSearchAdaptor(
            const Slice& slice,
            Cell source_cell,
            Cell target_cell,
            PauliOperator source_op,
            PauliOperator target_op)
            : slice_(slice),
              source_cell_(source_cell),
              target_cell_(target_cell),
              source_vertex_(make_vertex(source_cell)),
              target_vertex_(make_vertex(target_cell)),
              source_op_(source_op),
              target_op_(target_op)
    {
        if constexpr(want_cycle)
        {
            simulated_source_ = make_vertex(furthest_cell())+1;
        }
    };

    Cell furthest_cell() const
    {
        return slice_.get_layout().furthest_cell();
    }

    size_t num_vertices_on_lattice() const
    {
        return make_vertex(furthest_cell()) + 1;
    }

    Vertex source_vertex() const
    {
        return source_vertex_;
    }

    Vertex target_vertex() const
    {
        return target_vertex_;
    }

    template<bool _want_cycle = want_cycle>
    typename std::enable_if<_want_cycle, Vertex>::type simulated_source() const
    {
        return simulated_source_;
    }

    bool have_directed_edge(const Cell& a, const Cell& b) const
    {

        // If both cells are free, then they have an edge (it is assumed that this function is called only on neighbors!)
        if(slice_.is_cell_free(a) && slice_.is_cell_free(b)) return true;

        // If they are source and/or target, then the appropriate boundaries need to be matched
        if(a == cell_from_vertex(source_vertex_) && b == cell_from_vertex(target_vertex_))
            return slice_.have_boundary_of_type_with(source_cell_, b, source_op_)
                    && slice_.have_boundary_of_type_with(target_cell_, a, target_op_);


        if(a == cell_from_vertex(source_vertex_) && slice_.is_cell_free(b))
            return slice_.have_boundary_of_type_with(source_cell_, b, source_op_);

        if(slice_.is_cell_free(a) && b == cell_from_vertex(target_vertex_))
            return slice_.have_boundary_of_type_with(target_cell_, a, target_op_);

        return false;
    };

    bool have_directed_edge_EDPC(const Cell& a, const Cell& b) const
    {

        // In EDPC we assign dummy patches to cells that have been touched and mark them with PatchActivity::EDPC, and also reserve boundaries
        // Thus here we allow to route through EDPC cells but make sure that boundaries have not yet been reserved
        if ((slice_.is_cell_free_or_activity(a,{PatchActivity::EDPC}) && slice_.is_cell_free_or_activity(b, {PatchActivity::EDPC}))
            && (!slice_.is_boundary_reserved(a, b)))
         return true;

        if(a == cell_from_vertex(source_vertex_) && b == cell_from_vertex(target_vertex_))
            return slice_.have_boundary_of_type_with(source_cell_, b, source_op_)
                    && slice_.have_boundary_of_type_with(target_cell_, a, target_op_);


        if(a == cell_from_vertex(source_vertex_) && slice_.is_cell_free_or_activity(b, {PatchActivity::EDPC}))
            return slice_.have_boundary_of_type_with(source_cell_, b, source_op_);

        if(slice_.is_cell_free_or_activity(a, {PatchActivity::EDPC}) && b == cell_from_vertex(target_vertex_))
            return slice_.have_boundary_of_type_with(target_cell_, a, target_op_);

        return false;
    };

    Vertex make_vertex(const Cell& cell) const
    {
        const uint64_t vertex =
                static_cast<uint64_t>(cell.row) *
                        static_cast<uint64_t>(furthest_cell().col + 1) +
                static_cast<uint64_t>(cell.col);
        if(vertex >= UINT32_MAX)
            throw std::length_error{"Routing lattice exceeds 32-bit vertex IDs"};
        return static_cast<Vertex>(vertex);
    }


    Cell cell_from_vertex(Vertex vertex) const
    {
        if constexpr (want_cycle)
        {
            // In this case we add a simulated source at the end of the list of vertices
            if(vertex == simulated_source_)
                return target_cell_;
        }

        auto v = static_cast<Cell::CoordinateType>(vertex);
        auto col = v%(furthest_cell().col+1);
        return Cell{(v-col)/(furthest_cell().col+1), col};
    };

    template<class Visitor>
    void for_each_neighbour(Vertex v, Visitor&& visitor) const
    {
        for_each_neighbour(cell_from_vertex(v), std::forward<Visitor>(visitor));
    }

    template<class Visitor>
    void for_each_neighbour(Cell cell, Visitor&& visitor) const
    {
        const Cell furthest = furthest_cell();
        if(cell.row > 0)
            visitor(Cell{cell.row - 1, cell.col});
        if(cell.row < furthest.row)
            visitor(Cell{cell.row + 1, cell.col});
        if(cell.col > 0)
            visitor(Cell{cell.row, cell.col - 1});
        if(cell.col < furthest.col)
            visitor(Cell{cell.row, cell.col + 1});
    }

private:

    Vertex simulated_source_ = 0;

    const Slice& slice_;
    const Cell source_cell_;
    const Cell target_cell_;
    const Vertex source_vertex_;
    const Vertex target_vertex_;
    const PauliOperator source_op_;
    const PauliOperator target_op_;
};



template<bool want_cycle, Heuristic heuristic>
std::optional<RoutingRegion> do_graph_search_route_ancilla(
        const Slice& slice,
        PatchId source,
        PauliOperator source_op,
        PatchId target,
        PauliOperator target_op,
        bool EDPC,
        ResourceProfile* resource_profile,
        GraphSearchScratch& scratch
)
{

    const Cell source_cell = slice.get_cell_by_id(source).value();
    const Cell target_cell = slice.get_cell_by_id(target).value();

    const SliceSearchAdaptor<want_cycle> slice_searcher(slice, source_cell, target_cell, source_op, target_op);

    const size_t num_vertices_on_lattice = slice_searcher.num_vertices_on_lattice();
    if(num_vertices_on_lattice >= UINT32_MAX)
        throw std::length_error{"Routing lattice exceeds 32-bit vertex IDs"};

    scratch.generation++;
    if(scratch.generation == 0)
    {
        for(PredecessorData& predecessor : scratch.predecessors)
            predecessor.generation = 0;
        scratch.generation = 1;
    }
    const uint32_t generation = scratch.generation;
    const size_t predecessor_count = num_vertices_on_lattice + (want_cycle ? 1 : 0);
    const size_t predecessor_capacity_before = scratch.predecessors.capacity();
    if(scratch.predecessors.size() < predecessor_count)
        scratch.predecessors.resize(predecessor_count);
    if(resource_profile)
    {
        if(scratch.predecessors.capacity() != predecessor_capacity_before)
            resource_profile->routing_scratch_allocations++;
        resource_profile->routing_predecessor_capacity_high_water = std::max(
                resource_profile->routing_predecessor_capacity_high_water,
                static_cast<uint64_t>(scratch.predecessors.capacity()));
    }

    std::vector<PredecessorData>& predecessor_map = scratch.predecessors;
    std::vector<Vertex>& frontier = scratch.frontier;
    frontier.clear();
    scratch.path_vertices.clear();
    Comparator<decltype(slice_searcher), heuristic> cmp{
            target_cell,
            predecessor_map,
            slice_searcher,
            generation};
    auto push_frontier = [&](Vertex vertex) {
        const size_t capacity_before = frontier.capacity();
        frontier.push_back(vertex);
        std::push_heap(frontier.begin(), frontier.end(), cmp);
        if(resource_profile)
        {
            if(frontier.capacity() != capacity_before)
                resource_profile->routing_scratch_allocations++;
            resource_profile->routing_frontier_capacity_high_water = std::max(
                    resource_profile->routing_frontier_capacity_high_water,
                    static_cast<uint64_t>(frontier.capacity()));
        }
    };

    if constexpr (!want_cycle)
    {
        set_predecessor(
                predecessor_map,
                slice_searcher.source_vertex(),
                0,
                slice_searcher.source_vertex(),
                generation);
        push_frontier(slice_searcher.source_vertex());
    }
    else // Simulated double source to force a cycle case
    {
        Vertex simulated_source = slice_searcher.simulated_source();
        set_predecessor(
                predecessor_map,
                simulated_source,
                0,
                simulated_source,
                generation);

        slice_searcher.for_each_neighbour(source_cell, [&](const Cell& neighbour_cell) {
            if((!EDPC && slice_searcher.have_directed_edge(source_cell, neighbour_cell)) ||
                (EDPC && slice_searcher.have_directed_edge_EDPC(source_cell, neighbour_cell)))
            {
                Vertex neighbour = slice_searcher.make_vertex(neighbour_cell);
                set_predecessor(
                        predecessor_map,
                        neighbour,
                        1,
                        simulated_source,
                        generation);
                push_frontier(neighbour);
            }
        });

    }

    while(!frontier.empty())
    {
        std::pop_heap(frontier.begin(), frontier.end(), cmp);
        const Vertex curr = frontier.back();
        frontier.pop_back();
        const uint32_t distance_to_curr =
                distance_for_generation(predecessor_map[curr], generation);

        if constexpr (heuristic != Heuristic::None)
            if(curr == slice_searcher.target_vertex()) break;

        slice_searcher.for_each_neighbour(curr, [&](const Cell& neighbour_cell) {
            if((!EDPC && !slice_searcher.have_directed_edge(slice_searcher.cell_from_vertex(curr), neighbour_cell)) ||
                (EDPC && !slice_searcher.have_directed_edge_EDPC(slice_searcher.cell_from_vertex(curr), neighbour_cell)))
                    return;

            Vertex neighbour = slice_searcher.make_vertex(neighbour_cell);
            const uint32_t neighbour_distance =
                    distance_for_generation(predecessor_map[neighbour], generation);
            if(neighbour_distance == kUnvisitedDistance)
            {
                set_predecessor(
                        predecessor_map,
                        neighbour,
                        distance_to_curr + 1,
                        curr,
                        generation);
                push_frontier(neighbour);
            }
            else if(neighbour_distance > distance_to_curr + 1)
            {
                set_predecessor(
                        predecessor_map,
                        neighbour,
                        distance_to_curr + 1,
                        curr,
                        generation);
            }
        });
    }

#if false

    std::cout<< source_vertex << " to " << target_vertex<<std::endl;

    for (size_t i = 0; i<predecessor_map.size(); ++i)
        std::cout << i << "->" << predecessor_map[i].predecessor
                  << " (" << (predecessor_map[i].distance ? std::to_string(*predecessor_map[i].distance) : "N/A") << ")" << std::endl;
#endif


    // TODO refactor this to be shared with the boost implementation
    Vertex curr = predecessor_for_generation(
            predecessor_map,
            slice_searcher.target_vertex(),
            generation);
    Vertex next = predecessor_for_generation(predecessor_map, curr, generation);

    while (curr!=next)
    {
        const size_t path_capacity_before = scratch.path_vertices.capacity();
        scratch.path_vertices.push_back(curr);
        if(resource_profile)
        {
            if(scratch.path_vertices.capacity() != path_capacity_before)
                resource_profile->routing_scratch_allocations++;
            resource_profile->routing_path_capacity_high_water = std::max(
                    resource_profile->routing_path_capacity_high_water,
                    static_cast<uint64_t>(scratch.path_vertices.capacity()));
        }
        curr = next;
        next = predecessor_for_generation(predecessor_map, next, generation);
    }

    bool reached_source = curr == slice_searcher.source_vertex();
    if constexpr (want_cycle)
        reached_source = curr == slice_searcher.simulated_source();
    if(!reached_source)
        return std::nullopt;

    RoutingRegion ret;
    if(!scratch.path_vertices.empty())
    {
        ret.cells.reserve(scratch.path_vertices.size());
        if(resource_profile)
            resource_profile->routing_result_allocations++;
    }
    Vertex prec = slice_searcher.target_vertex();
    for(const Vertex path_vertex : scratch.path_vertices)
    {
        curr = path_vertex;
        next = predecessor_for_generation(predecessor_map, curr, generation);
        const Cell prec_cell = slice_searcher.cell_from_vertex(prec);
        const Cell curr_cell = slice_searcher.cell_from_vertex(curr);
        const Cell next_cell = slice_searcher.cell_from_vertex(next);

        ret.cells.push_back(SingleCellOccupiedByPatch{
                {.top=   {BoundaryType::None, false},
                 .bottom={BoundaryType::None, false},
                 .left=  {BoundaryType::None, false},
                 .right= {BoundaryType::None, false}},
                curr_cell
        });

        slice_searcher.for_each_neighbour(curr_cell, [&](const Cell& neighbour) {
            if (prec_cell==neighbour || next_cell==neighbour)
            {
                auto boundary = ret.cells.back().get_mut_boundary_with(neighbour);
                if (boundary) {
                    EDPC ? boundary->get() = {.boundary_type=BoundaryType::Reserved_Label1, .is_active=true} : boundary->get() = {.boundary_type=BoundaryType::Connected, .is_active=true};
                }
            }
        });

        prec = curr;
    }
    return ret;
}


template<Heuristic heuristic>
std::optional<RoutingRegion> graph_search_route_ancilla_dispatc_heuristic(
        const Slice& slice,
        PatchId source,
        PauliOperator source_op,
        PatchId target,
        PauliOperator target_op,
        bool EDPC,
        ResourceProfile* resource_profile,
        GraphSearchScratch& scratch
)
{
    return source == target ?
        do_graph_search_route_ancilla<true, heuristic>(slice, source, source_op, target, target_op, EDPC, resource_profile, scratch):
        do_graph_search_route_ancilla<false, heuristic>(slice, source, source_op, target, target_op, EDPC, resource_profile, scratch);
}


std::optional<RoutingRegion> graph_search_route_ancilla(
        const Slice& slice,
        PatchId source,
        PauliOperator source_op,
        PatchId target,
        PauliOperator target_op,
        Heuristic heuristic,
        bool EDPC,
        ResourceProfile* resource_profile,
        GraphSearchScratch* scratch
)
{
    GraphSearchScratch local_scratch;
    GraphSearchScratch& active_scratch = scratch ? *scratch : local_scratch;
    if(heuristic == Heuristic::None)
        return graph_search_route_ancilla_dispatc_heuristic<Heuristic::None>(slice, source, source_op, target, target_op, EDPC, resource_profile, active_scratch);
    else if(heuristic == Heuristic::Euclidean)
        return graph_search_route_ancilla_dispatc_heuristic<Heuristic::Euclidean>(slice, source, source_op, target, target_op, EDPC, resource_profile, active_scratch);
    else
        throw std::runtime_error(lstk::cat("Unknown heuristic: ", static_cast<int>(heuristic)));
}


}
}
