#ifndef LSQECC_RTLS_REORDERED_QASM_HPP
#define LSQECC_RTLS_REORDERED_QASM_HPP

#include <lsqecc/gates/parse_gates.hpp>
#include <lsqecc/ls_instructions/ls_instructions.hpp>
#include <lsqecc/patches/dense_slice.hpp>
#include <lsqecc/rtls/operation_schedule.hpp>

#include <cstddef>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>

namespace lsqecc {

/**
 * Records the slice in which each source-owned logical operation starts.
 *
 * A physical appearance in a DenseSlice is authoritative.  Successfully
 * applied instructions are retained as a fallback for operations which have
 * no visible lattice geometry, such as Reset.
 */
class ReorderedQasmTracker
{
public:
    explicit ReorderedQasmTracker(
            const OperationMetadataArena* operation_metadata_arena = nullptr,
            size_t core_qubit_count = 0,
            OperationScheduleTracker* shared_schedule_tracker = nullptr,
            ResourceProfile* resource_profile = nullptr)
        : operation_metadata_arena_(operation_metadata_arena),
          owned_schedule_tracker_(operation_metadata_arena, resource_profile),
          schedule_tracker_(shared_schedule_tracker
                  ? shared_schedule_tracker
                  : &owned_schedule_tracker_)
    {
        static_cast<void>(core_qubit_count);
    }

    void observe_instruction(const LSInstruction& instruction);
    void observe_slice(const DenseSlice& slice);

    void render_to(
            std::ostream& output,
            std::string_view source_name,
            const QasmSourceDocument& document,
            const QasmSourceOperationMap& operation_map) const;

    std::string render(
            std::string_view source_name,
            const QasmSourceDocument& document,
            const QasmSourceOperationMap& operation_map) const;

    size_t slice_count() const noexcept;

private:
    const OperationMetadataArena* operation_metadata_arena_ = nullptr;
    OperationScheduleTracker owned_schedule_tracker_;
    OperationScheduleTracker* schedule_tracker_ = nullptr;
};

std::filesystem::path default_reordered_qasm_path(const std::filesystem::path& input_path);

}

#endif
