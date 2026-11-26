#ifndef LSQECC_RTLS_OPERATION_SCHEDULE_HPP
#define LSQECC_RTLS_OPERATION_SCHEDULE_HPP

#include <lsqecc/ls_instructions/ls_instructions.hpp>
#include <lsqecc/patches/dense_slice.hpp>
#include <lsqecc/rtls/metadata.hpp>

#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace lsqecc {

struct ResourceProfile;

using SliceIndex = uint32_t;
inline constexpr SliceIndex kUnscheduledSlice =
        std::numeric_limits<SliceIndex>::max();

struct OperationSchedule
{
    std::vector<SliceIndex> first_applied;
    std::vector<SliceIndex> first_visible;
    std::vector<SliceIndex> first_semantic_start;
    SliceIndex slice_count = 0;
};

/**
 * Dense, handle-indexed operation-start collector shared by reordered QASM and
 * evaluation metrics. The scheduler-facing event method avoids string maps and
 * repeated full-grid scans in those consumers.
 */
class OperationScheduleTracker
{
public:
    explicit OperationScheduleTracker(
            const OperationMetadataArena* operation_metadata_arena = nullptr,
            ResourceProfile* resource_profile = nullptr);

    void observe_instruction(const LSInstruction& instruction);
    void observe_visible(OperationHandle operation_handle, PatchActivity activity);
    void finish_slice();

    // Compatibility/testing path. Production scheduling emits visibility
    // events directly and calls finish_slice once per completed slice.
    void observe_slice(const DenseSlice& slice);

    size_t first_slice_for(std::string_view operation_id) const;
    size_t semantic_start_for(std::string_view operation_id) const;
    size_t slice_count() const noexcept;
    const OperationSchedule& schedule() const noexcept;

private:
    const OperationMetadataArena* operation_metadata_arena_ = nullptr;
    OperationSchedule schedule_;
    ResourceProfile* resource_profile_ = nullptr;

    void ensure_handle(OperationHandle operation_handle);
    static void record_first(
            std::vector<SliceIndex>& starts,
            OperationHandle operation_handle,
            SliceIndex slice);
};

}

#endif
