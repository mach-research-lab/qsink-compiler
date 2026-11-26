#ifndef LSQECC_QSINK_INSTRUCTION_STREAM_HPP
#define LSQECC_QSINK_INSTRUCTION_STREAM_HPP

#include <lsqecc/ls_instructions/ls_instruction_stream.hpp>
#include <lsqecc/rtls/rtls.hpp>

#include <cstddef>
#include <deque>
#include <optional>
#include <queue>
#include <vector>

namespace lsqecc
{

enum class PendingSKind
{
    None,
    S,
    SDg
};

struct PendingConditionalS
{
    QubitNum qubit;
    PendingSKind kind;
    bool source_t_is_dagger = false;
};

struct QSinkStats
{
    size_t pending_corrections_created = 0;
    size_t pending_corrections_materialized = 0;
    size_t pending_corrections_delayed = 0;
    size_t pattern_t_h_matches = 0;
    size_t pattern_t_h_s_matches = 0;
    size_t pattern_t_h_s_t_matches = 0;
};

class QSinkInstructionStreamFromGateStream : public LSInstructionStream
{
public:
    QSinkInstructionStreamFromGateStream(
            GateStream& gate_stream,
            CNOTCorrectionMode cnot_correction_mode,
            IdGenerator& id_generator,
            bool local_instructions,
            QasmSourceOperationMap* source_operation_map = nullptr,
            OperationMetadataArena* operation_metadata_arena = nullptr
            );

    LSInstruction get_next_instruction() override;
    bool has_next_instruction() const override;
    const tsl::ordered_set<PatchId>& core_qubits() const override;
    const OperationMetadataArena* operation_metadata_arena() const override;
    const QSinkStats& stats() const;

private:
    GateStream& gate_stream_;
    std::deque<SourcedGate> input_buffer_;
    std::queue<LSInstruction> next_instructions_;
    tsl::ordered_set<PatchId> core_qubits_;
    LSIinstructionFromGatesGenerator instruction_generator_;
    CNOTCorrectionMode cnot_correction_mode_;
    QasmSourceOperationMap* source_operation_map_;
    RtlsOperationIdGenerator source_operation_ids_;
    OperationMetadataArena owned_operation_metadata_;
    OperationMetadataArena* operation_metadata_;
    std::vector<std::optional<PendingConditionalS>> pending_s_;
    QSinkStats stats_;

    void refill_next_instructions();
    void lower_gate(const SourcedGate& sourced_gate);
    bool has_pending_s() const;
    void materialize_next_pending_s();
    void set_pending_s(QubitNum qubit, bool source_t_is_dagger);
    bool has_input_gate() const;
    SourcedGate get_next_input_gate();
    const SourcedGate* peek_input_gate(size_t offset);
    void record_pending_patterns();
    void record_patterns_for_pending(const PendingConditionalS& pending_s);
};

}

#endif // LSQECC_QSINK_INSTRUCTION_STREAM_HPP
