#include <lsqecc/rtls/operation_schedule.hpp>
#include <lsqecc/resource_profile.hpp>

#include <optional>
#include <stdexcept>
#include <string>

namespace lsqecc {

namespace {

bool is_visible_rtls_activity(PatchActivity activity)
{
    return activity == PatchActivity::Measurement ||
           activity == PatchActivity::Unitary ||
           activity == PatchActivity::MultiPatchMeasurement ||
           activity == PatchActivity::Rotation;
}

}

OperationScheduleTracker::OperationScheduleTracker(
        const OperationMetadataArena* operation_metadata_arena,
        ResourceProfile* resource_profile)
    : operation_metadata_arena_(operation_metadata_arena),
      resource_profile_(resource_profile)
{
    const size_t operation_count = operation_metadata_arena_
            ? operation_metadata_arena_->size()
            : 0;
    schedule_.first_applied.assign(operation_count, kUnscheduledSlice);
    schedule_.first_visible.assign(operation_count, kUnscheduledSlice);
    schedule_.first_semantic_start.assign(operation_count, kUnscheduledSlice);
    if(resource_profile_)
    {
        resource_profile_->tracker_first_applied_entries = operation_count;
        resource_profile_->tracker_first_visible_entries = operation_count;
    }
}

void OperationScheduleTracker::ensure_handle(OperationHandle operation_handle)
{
    if(operation_metadata_arena_ && operation_handle >= operation_metadata_arena_->size())
        throw std::out_of_range{"Operation schedule handle is outside its metadata arena"};
    const size_t required = static_cast<size_t>(operation_handle) + 1;
    auto grow = [required](std::vector<SliceIndex>& starts) {
        if(starts.size() < required)
            starts.resize(required, kUnscheduledSlice);
    };
    grow(schedule_.first_applied);
    grow(schedule_.first_visible);
    grow(schedule_.first_semantic_start);
    if(resource_profile_)
    {
        resource_profile_->tracker_first_applied_entries =
                schedule_.first_applied.size();
        resource_profile_->tracker_first_visible_entries =
                schedule_.first_visible.size();
    }
}

void OperationScheduleTracker::record_first(
        std::vector<SliceIndex>& starts,
        OperationHandle operation_handle,
        SliceIndex slice)
{
    SliceIndex& first = starts[operation_handle];
    if(first == kUnscheduledSlice)
        first = slice;
}

void OperationScheduleTracker::observe_instruction(const LSInstruction& instruction)
{
    if(!instruction.rtls_operation)
        return;
    if(resource_profile_)
        resource_profile_->tracker_instruction_events++;
    ensure_handle(*instruction.rtls_operation);
    record_first(
            schedule_.first_applied,
            *instruction.rtls_operation,
            schedule_.slice_count);
    if(instruction.marks_rtls_semantic_start)
        record_first(
                schedule_.first_semantic_start,
                *instruction.rtls_operation,
                schedule_.slice_count);
}

void OperationScheduleTracker::observe_visible(
        OperationHandle operation_handle,
        PatchActivity activity)
{
    if(resource_profile_)
        resource_profile_->tracker_visibility_events++;
    ensure_handle(operation_handle);
    record_first(
            schedule_.first_visible,
            operation_handle,
            schedule_.slice_count);
    static_cast<void>(activity);
}

void OperationScheduleTracker::finish_slice()
{
    if(schedule_.slice_count == kUnscheduledSlice - 1)
        throw std::overflow_error{"Operation schedule slice index space exhausted"};
    schedule_.slice_count++;
}

void OperationScheduleTracker::observe_slice(const DenseSlice& slice)
{
    if(resource_profile_)
    {
        resource_profile_->metadata_full_lattice_scans++;
        resource_profile_->metadata_cells_visited += slice.cell_count();
    }
    slice.traverse_cells([&](const Cell&, const DensePatch* patch) {
        if(!patch)
            return;
        if(patch->rtls_operation && is_visible_rtls_activity(patch->activity))
            observe_visible(*patch->rtls_operation, patch->activity);
        for(OperationHandle operation_handle : patch->rtls_frame_updates)
            observe_visible(operation_handle, PatchActivity::None);
    });
    finish_slice();
}

size_t OperationScheduleTracker::first_slice_for(
        std::string_view operation_id) const
{
    if(!operation_metadata_arena_)
        throw std::logic_error{"Operation schedule has no metadata arena"};
    const std::optional<OperationHandle> operation_handle =
            operation_metadata_arena_->find(operation_id);
    if(!operation_handle || *operation_handle >= schedule_.first_applied.size())
        throw std::runtime_error{
                "RTLS operation " + std::string{operation_id} +
                " did not appear in any compiler slice"};

    const SliceIndex visible = schedule_.first_visible[*operation_handle];
    const SliceIndex applied = schedule_.first_applied[*operation_handle];
    const SliceIndex first = visible != kUnscheduledSlice ? visible : applied;
    if(first == kUnscheduledSlice)
        throw std::runtime_error{
                "RTLS operation " + std::string{operation_id} +
                " did not appear in any compiler slice"};
    if(first >= schedule_.slice_count)
        throw std::runtime_error{
                "RTLS operation " + std::string{operation_id} +
                " resolves outside the completed compiler slices"};
    return first;
}

size_t OperationScheduleTracker::semantic_start_for(
        std::string_view operation_id) const
{
    if(!operation_metadata_arena_)
        throw std::logic_error{"Operation schedule has no metadata arena"};
    const std::optional<OperationHandle> operation_handle =
            operation_metadata_arena_->find(operation_id);
    if(!operation_handle || *operation_handle >= schedule_.first_semantic_start.size())
        throw std::runtime_error{
                "RTLS operation " + std::string{operation_id} +
                " has no semantic-start event"};
    const SliceIndex start = schedule_.first_semantic_start[*operation_handle];
    if(start == kUnscheduledSlice || start >= schedule_.slice_count)
        throw std::runtime_error{
                "RTLS operation " + std::string{operation_id} +
                " has no semantic-start event"};
    return start;
}

size_t OperationScheduleTracker::slice_count() const noexcept
{
    return schedule_.slice_count;
}

const OperationSchedule& OperationScheduleTracker::schedule() const noexcept
{
    return schedule_;
}

}
