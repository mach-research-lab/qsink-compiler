#ifndef LSQECC_RTLS_HPP
#define LSQECC_RTLS_HPP

#include <lsqecc/gates/parse_gates.hpp>
#include <lsqecc/ls_instructions/ls_instruction_stream.hpp>
#include <lsqecc/patches/dense_slice.hpp>
#include <lsqecc/rtls/metadata.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lsqecc {

struct ResourceProfile;

/**
 * Generates globally unique RTLS operation IDs in the `op_000001` form.
 */
class RtlsOperationIdGenerator
{
public:
    std::string next();

private:
    size_t next_id_ = 1;
};

struct RtlsSplitCnot
{
    std::string split_id;
    QubitNum control_qubit = 0;
    QubitNum target_qubit = 0;
    gates::CNOTType cnot_type = gates::CNOTType::ZX_WITH_MBM_CONTROL_FIRST;
    gates::CNOTAncillaPlacement ancilla_placement =
            gates::CNOTAncillaPlacement::ANCILLA_NEXT_TO_CONTROL;
};

enum class RtlsPauliFrameUpdateKind
{
    X,
    Z
};

enum class RtlsPauliFrameUpdateTarget
{
    LogicalQubit,
    SplitCnotAncilla
};

enum class RtlsPauliFrameUpdatePlacement
{
    BeforeOperation,
    AfterMzz,
    AfterMxx,
    AfterFinalAncillaMz
};

/**
 * A qSink-generated conditional Pauli correction implemented entirely as
 * frame metadata. Placement is semantic so lowering can keep the update next
 * to its causal operation without adding a physical correction slice.
 */
struct RtlsPauliFrameUpdate
{
    RtlsPauliFrameUpdateKind kind;
    RtlsPauliFrameUpdateTarget target;
    QubitNum target_qubit = 0;
    RtlsPauliFrameUpdatePlacement placement;
    OperationHandle operation_handle = kInvalidOperationHandle;
};

struct RtlsLogicalOperation
{
    enum class Kind
    {
        Gate,
        TStar,
        TStardg,
        Correction,
        Mz,
        Mx
    };

    Kind kind;
    OperationHandle operation_handle = kInvalidOperationHandle;
    std::optional<gates::Gate> gate;
    std::optional<RtlsSplitCnot> split_cnot;
    QubitNum target_qubit = 0;
    std::vector<RtlsPauliFrameUpdate> pauli_frame_updates;
};

struct RtlsCorrection
{
    std::string source_operation_id;
    std::string correction_operation_id;
    std::string correction_operation;
    QubitNum target_qubit = 0;
    RtlsCorrectionLatency latency = 0;
    std::string anchor_operation_id;
    std::optional<std::string> post_anchor_operation_id;
};

/**
 * Cheap diagnostics for the compact qSink rewrite pass.  These counters make
 * it possible to assert that rewrite matching follows the precomputed
 * per-qubit successor links instead of rescanning the whole gate suffix.
 */
struct QsinkOptimizerStats
{
    size_t input_gate_count = 0;
    size_t qubit_touch_count = 0;
    size_t successor_link_count = 0;
    size_t rewrite_probe_count = 0;
};

struct QsinkOptimizedProgram
{
    Qreg qreg;
    OperationMetadataArena operation_metadata;
    std::vector<RtlsLogicalOperation> operations;
    std::vector<RtlsCorrection> corrections;
    std::optional<QasmSourceDocument> qasm_source_document;
    QasmSourceOperationMap source_operation_map;
    size_t input_t_count = 0;
    size_t input_tdg_count = 0;
    QsinkOptimizerStats optimizer_stats;

    const RtlsOperationMetadata& metadata_for(
            const RtlsLogicalOperation& operation) const
    {
        return operation_metadata.at(operation.operation_handle);
    }

    const CompactRtlsOperationMetadata& compact_metadata_for(
            const RtlsLogicalOperation& operation) const
    {
        return operation_metadata.compact_at(operation.operation_handle);
    }

    const CompactRtlsOperationMetadata& compact_metadata_for(
            const RtlsPauliFrameUpdate& update) const
    {
        return operation_metadata.compact_at(update.operation_handle);
    }

    const RtlsOperationMetadata& metadata_for(
            const RtlsPauliFrameUpdate& update) const
    {
        return operation_metadata.at(update.operation_handle);
    }

    std::vector<RtlsLogicalOperation> take_operations()
    {
        return std::exchange(operations, {});
    }
};

// Source compatibility for RTLS-facing consumers.  The program is the shared
// qSink optimizer result; RTLS is one serialization/output consumer of it.
using RtlsProgram = QsinkOptimizedProgram;

/**
 * Returns the RTLS filename for a requested output path.
 */
std::string rtls_output_path(std::string_view path);

std::string rtls_operation_name(const gates::BasicSingleQubitGate& gate);
std::string rtls_operation_name(const gates::ControlledGate& gate);
std::string rtls_operation_name(const gates::Reset& reset);
std::string rtls_operation_name(const gates::Gate& gate);
std::string rtls_operation_name(const LSInstruction& instruction);

/**
 * Drains a decomposed QASM gate stream into the shared qSink optimizer result.
 * Both sliced and no-slice lowering consume this program. When qSink is
 * enabled, enable_ths_optimization controls only the T/TDg H S rewrite that
 * changes the delayed correction to CondH with "hs" latency.
 */
QsinkOptimizedProgram optimize_qsink_program(
        GateStream& gate_stream,
        bool qsink,
        bool enable_ths_optimization = true,
        ResourceProfile* resource_profile = nullptr,
        bool retain_serialized_metadata = true);

/** Compatibility entry point for callers that still use the former name. */
RtlsProgram build_rtls_program(
        GateStream& gate_stream,
        bool qsink,
        bool enable_ths_optimization = true,
        ResourceProfile* resource_profile = nullptr,
        bool retain_serialized_metadata = true);

class RtlsInstructionStream : public LSInstructionStream
{
public:
    RtlsInstructionStream(
            const RtlsProgram& program,
            CNOTCorrectionMode cnot_correction_mode,
            IdGenerator& id_generator,
            bool local_instructions);
    RtlsInstructionStream(
            std::vector<RtlsLogicalOperation>&& operations,
            const RtlsProgram& retained_program,
            CNOTCorrectionMode cnot_correction_mode,
            IdGenerator& id_generator,
            bool local_instructions);

    LSInstruction get_next_instruction() override;
    bool has_next_instruction() const override;
    const tsl::ordered_set<PatchId>& core_qubits() const override;
    const OperationMetadataArena* operation_metadata_arena() const override;
    size_t instruction_count_estimate() const override;

private:
    const RtlsProgram& program_;
    std::vector<RtlsLogicalOperation> operations_;
    size_t next_operation_ = 0;
    std::queue<LSInstruction> next_instructions_;
    tsl::ordered_set<PatchId> core_qubits_;
    LSIinstructionFromGatesGenerator instruction_generator_;
    CNOTCorrectionMode cnot_correction_mode_;
    bool local_instructions_;
    IdGenerator& id_generator_;
    std::unordered_map<std::string, PatchId> split_cnot_ancilla_;
    std::unordered_map<uint32_t, PatchId> t_source_dependency_patch_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> correction_ids_by_anchor_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> correction_ids_by_post_anchor_;
    DependencyTokenId next_dependency_token_id_ = 1;
    std::unordered_map<uint32_t, DependencyTokenId> mz_ready_token_by_correction_;
    std::unordered_map<uint32_t, DependencyTokenId> correction_done_token_by_correction_;
    std::unordered_map<uint32_t, DependencyTokenId> t_body_done_token_by_source_;

    void lower_next_operation();
    void release_operations_if_drained();
    void stamp_queue_if_missing(
            std::queue<LSInstruction>& instructions,
            OperationHandle operation_handle);
    DependencyTokenId dependency_token_for(
            std::unordered_map<uint32_t, DependencyTokenId>& tokens,
            uint32_t correction_operation_id);
    std::queue<LSInstruction> lower_split_cnot_mz(const RtlsLogicalOperation& operation);
    std::queue<LSInstruction> lower_split_cnot_mx(const RtlsLogicalOperation& operation);
};

struct RtlsValidationContext
{
    size_t input_t_count = 0;
    size_t input_tdg_count = 0;
};

/**
 * Converts buffered dense slices to RTLS JSON, fills final conditional
 * correction cross-links, and validates the contract.
 */
nlohmann::json render_rtls_schedule(
        const std::vector<DenseSlice>& base_slices,
        const std::vector<RtlsCorrection>& corrections,
        RtlsValidationContext validation_context);

void validate_rtls_schedule(
        const nlohmann::json& schedule,
        RtlsValidationContext validation_context);

}

#endif
