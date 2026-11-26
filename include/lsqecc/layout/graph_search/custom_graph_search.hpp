#ifndef LSQECC_CUSTOM_GRAPH_SEARCH_HPP
#define LSQECC_CUSTOM_GRAPH_SEARCH_HPP

#include <lsqecc/patches/slice.hpp>

#include <cstdint>
#include <vector>

namespace lsqecc {

struct ResourceProfile;

namespace custom_graph_search {


enum class Heuristic
{
        None, // I.e. Djikstra
        Euclidean,
};

struct GraphSearchPredecessorData
{
    uint32_t distance = UINT32_MAX;
    uint32_t predecessor = 0;
    uint32_t generation = 0;
};

struct GraphSearchScratch
{
    std::vector<GraphSearchPredecessorData> predecessors;
    std::vector<uint32_t> frontier;
    std::vector<uint32_t> path_vertices;
    uint32_t generation = 0;
};


std::optional<RoutingRegion> graph_search_route_ancilla(
        const Slice& slice,
        PatchId source,
        PauliOperator source_op,
        PatchId target,
        PauliOperator target_op,
        Heuristic heuristic,
        bool EDPC,
        ResourceProfile* resource_profile = nullptr,
        GraphSearchScratch* scratch = nullptr
);

}

}

#endif //LSQECC_CUSTOM_GRAPH_SEARCH_HPP
