#include <lsqecc/ls_instructions/qsink_instruction_stream.hpp>

#include <lstk/lstk.hpp>

#include <cassert>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lsqecc
{

namespace
{

tsl::ordered_set<PatchId> core_qubits_from_qsink_gate_stream(GateStream& gate_stream)
{
    tsl::ordered_set<PatchId> core_qubits;
    for(PatchId id = 0; id < gate_stream.get_qreg().size; id++)
        core_qubits.insert(id);
    assert(core_qubits.size());
    return core_qubits;
}

bool is_pauli_gate(gates::BasicSingleQubitGate::Type gate_type)
{
    return gate_type == gates::BasicSingleQubitGate::Type::X ||
           gate_type == gates::BasicSingleQubitGate::Type::Y ||
           gate_type == gates::BasicSingleQubitGate::Type::Z;
}

bool gate_touches_qubit(const gates::Gate& gate, QubitNum qubit)
{
    return std::visit(lstk::overloaded{
        [qubit](const gates::BasicSingleQubitGate& basic_gate) {
            return basic_gate.target_qubit == qubit;
        },
        [qubit](const gates::RZ& rz_gate) {
            return rz_gate.target_qubit == qubit;
        },
        [qubit](const gates::ControlledGate& controlled_gate) {
            if(controlled_gate.control_qubit == qubit)
                return true;

            return std::visit(lstk::overloaded{
                [qubit](const gates::BasicSingleQubitGate& target_gate) {
                    return target_gate.target_qubit == qubit;
                },
                [qubit](const gates::RZ& target_gate) {
                    return target_gate.target_qubit == qubit;
                }
            }, controlled_gate.target_gate);
        },
        [qubit](const gates::Reset& reset) {
            return reset.target_qubit == qubit;
        }
    }, gate);
}

std::optional<gates::BasicSingleQubitGate::Type> supported_single_qubit_pattern_gate(
        const gates::Gate& gate,
        QubitNum qubit)
{
    if(const auto* basic_gate = std::get_if<gates::BasicSingleQubitGate>(&gate))
    {
        if(basic_gate->target_qubit == qubit)
            return basic_gate->gate_type;
        return std::nullopt;
    }

    if(const auto* rz_gate = std::get_if<gates::RZ>(&gate))
    {
        if(rz_gate->target_qubit != qubit)
            return std::nullopt;
        if(rz_gate->pi_fraction.num == 1 && rz_gate->pi_fraction.den == 1)
            return gates::BasicSingleQubitGate::Type::Z;
        if(rz_gate->pi_fraction.num == 1 && rz_gate->pi_fraction.den == 2)
            return gates::BasicSingleQubitGate::Type::S;
        if(rz_gate->pi_fraction.num == 1 && rz_gate->pi_fraction.den == 4)
            return gates::BasicSingleQubitGate::Type::T;
    }

    return std::nullopt;
}

void stamp_instructions(
        std::queue<LSInstruction>& instructions,
        OperationHandle operation_handle)
{
    std::queue<LSInstruction> stamped;
    while(!instructions.empty())
    {
        LSInstruction instruction = lstk::queue_pop(instructions);
        instruction.rtls_operation = operation_handle;
        stamped.push(std::move(instruction));
    }
    instructions.swap(stamped);
}

}

QSinkInstructionStreamFromGateStream::QSinkInstructionStreamFromGateStream(
        GateStream& gate_stream,
        CNOTCorrectionMode cnot_correction_mode,
        IdGenerator& id_generator,
        bool local_instructions,
        QasmSourceOperationMap* source_operation_map,
        OperationMetadataArena* operation_metadata_arena)
    : gate_stream_(gate_stream),
      core_qubits_(core_qubits_from_qsink_gate_stream(gate_stream)),
      instruction_generator_(id_generator, local_instructions),
      cnot_correction_mode_(cnot_correction_mode),
      source_operation_map_(source_operation_map),
      operation_metadata_(operation_metadata_arena
              ? operation_metadata_arena
              : &owned_operation_metadata_),
      pending_s_(gate_stream.get_qreg().size)
{}

LSInstruction QSinkInstructionStreamFromGateStream::get_next_instruction()
{
    if(next_instructions_.empty())
        refill_next_instructions();

    return lstk::queue_pop(next_instructions_);
}

bool QSinkInstructionStreamFromGateStream::has_next_instruction() const
{
    return !next_instructions_.empty() || has_pending_s() || has_input_gate();
}

const tsl::ordered_set<PatchId>& QSinkInstructionStreamFromGateStream::core_qubits() const
{
    return core_qubits_;
}

const OperationMetadataArena*
QSinkInstructionStreamFromGateStream::operation_metadata_arena() const
{
    return operation_metadata_;
}

const QSinkStats& QSinkInstructionStreamFromGateStream::stats() const
{
    return stats_;
}

void QSinkInstructionStreamFromGateStream::refill_next_instructions()
{
    if(has_pending_s())
    {
        record_pending_patterns();
        materialize_next_pending_s();
        return;
    }

    if(!has_input_gate())
        throw std::logic_error{"QSinkInstructionStreamFromGateStream: No more gates but requesting instructions"};

    lower_gate(get_next_input_gate());
}

void QSinkInstructionStreamFromGateStream::lower_gate(const SourcedGate& sourced_gate)
{
    const gates::Gate& gate = sourced_gate.gate;

    if(const auto* sq_gate = std::get_if<gates::BasicSingleQubitGate>(&gate))
    {
        switch(sq_gate->gate_type)
        {
#define SINGLE_QUBIT_OP_CASE(Op)\
            case gates::BasicSingleQubitGate::Type::Op: \
                next_instructions_.push({.operation={SingleQubitOp{sq_gate->target_qubit,SingleQubitOp::Operator::Op}}});\
                break;
            SINGLE_QUBIT_OP_CASE(Z)
            SINGLE_QUBIT_OP_CASE(X)
            SINGLE_QUBIT_OP_CASE(Y)
            SINGLE_QUBIT_OP_CASE(H)
            SINGLE_QUBIT_OP_CASE(S)
#undef SINGLE_QUBIT_OP_CASE

            case gates::BasicSingleQubitGate::Type::SDg:
                next_instructions_.push({.operation={SingleQubitOp{sq_gate->target_qubit,SingleQubitOp::Operator::S, true}}});
                break;

            case gates::BasicSingleQubitGate::Type::TDg:
            case gates::BasicSingleQubitGate::Type::T:
            {
                bool is_dagger = sq_gate->gate_type == gates::BasicSingleQubitGate::Type::TDg;
                auto instructions = instruction_generator_.make_t_gate_body_instructions(sq_gate->target_qubit, is_dagger);
                lstk::queue_extend(next_instructions_, instructions);
                set_pending_s(sq_gate->target_qubit, is_dagger);
                break;
            }
        }
    }
    else if(const auto* rz_gate = std::get_if<gates::RZ>(&gate))
    {
        if(rz_gate->pi_fraction.num == 1 && rz_gate->pi_fraction.den == 1)
            next_instructions_.push({.operation={SingleQubitOp{rz_gate->target_qubit,SingleQubitOp::Operator::Z}}});
        else if(rz_gate->pi_fraction.num == 1 && rz_gate->pi_fraction.den == 2)
            next_instructions_.push({.operation={SingleQubitOp{rz_gate->target_qubit,SingleQubitOp::Operator::S}}});
        else if(rz_gate->pi_fraction.num == 1 && rz_gate->pi_fraction.den == 4)
        {
            auto instructions = instruction_generator_.make_t_gate_body_instructions(rz_gate->target_qubit);
            lstk::queue_extend(next_instructions_, instructions);
            set_pending_s(rz_gate->target_qubit, false);
        }

        else
            throw std::runtime_error(lstk::cat(
                    "Cannot approximate R_z(",rz_gate->pi_fraction.num,"/",rz_gate->pi_fraction.den,")"));
    }
    else if(const auto* controlled_gate = std::get_if<gates::ControlledGate>(&gate))
    {
        if(const auto* target_gate = std::get_if<gates::BasicSingleQubitGate>(&controlled_gate->target_gate))
        {
            if(target_gate->gate_type == gates::BasicSingleQubitGate::Type::X)
            {
                auto instructions = instruction_generator_.make_cnot_instructions(
                        controlled_gate->control_qubit,
                        target_gate->target_qubit,
                        controlled_gate->cnot_type,
                        controlled_gate->cnot_ancilla_placement,
                        cnot_correction_mode_);
                lstk::queue_extend(next_instructions_, instructions);
            }
            else if (target_gate->gate_type == gates::BasicSingleQubitGate::Type::Z)
            {
                next_instructions_.push({.operation={SingleQubitOp{target_gate->target_qubit,SingleQubitOp::Operator::H}}});
                auto instructions = instruction_generator_.make_cnot_instructions(
                        controlled_gate->control_qubit,
                        target_gate->target_qubit,
                        controlled_gate->cnot_type,
                        controlled_gate->cnot_ancilla_placement,
                        cnot_correction_mode_);
                lstk::queue_extend(next_instructions_, instructions);
                next_instructions_.push({.operation={SingleQubitOp{target_gate->target_qubit,SingleQubitOp::Operator::H}}});
            }
            else
                throw std::runtime_error{lstk::cat(
                        "Gate type of index ", static_cast<size_t>(target_gate->gate_type), "Not supported for control")};
        }
        else if(const auto* target_gate = std::get_if<gates::RZ>(&controlled_gate->target_gate))
        {
            LSTK_UNUSED(target_gate);
            LSTK_NOT_IMPLEMENTED;
        }
    }
    else if(const auto* reset = std::get_if<gates::Reset>(&gate))
    {
        if (reset->register_name != gate_stream_.get_qreg().name)
            throw std::logic_error{"Invalid 'reset' register name"};

        next_instructions_.push({.operation={PatchReset{reset->target_qubit}}});
    }
    else
    {
        LSTK_UNREACHABLE;
    }

    if(source_operation_map_ && sourced_gate.source_statement_index)
    {
        RtlsOperationMetadata metadata{
                .operation = rtls_operation_name(gate),
                .operation_id = source_operation_ids_.next()};
        const OperationHandle operation_handle = operation_metadata_->add(metadata);
        stamp_instructions(next_instructions_, operation_handle);
        source_operation_map_->add_operation(
                *sourced_gate.source_statement_index,
                operation_handle);
    }
}

bool QSinkInstructionStreamFromGateStream::has_pending_s() const
{
    for(const auto& pending_s : pending_s_)
        if(pending_s.has_value())
            return true;
    return false;
}

void QSinkInstructionStreamFromGateStream::materialize_next_pending_s()
{
    for(auto& pending_s : pending_s_)
    {
        if(!pending_s)
            continue;

        auto instructions = instruction_generator_.make_t_gate_correction_instructions(
                pending_s->qubit,
                pending_s->source_t_is_dagger);
        lstk::queue_extend(next_instructions_, instructions);
        stats_.pending_corrections_materialized++;
        pending_s.reset();
        return;
    }
}

void QSinkInstructionStreamFromGateStream::set_pending_s(QubitNum qubit, bool source_t_is_dagger)
{
    if(qubit >= pending_s_.size())
        throw std::logic_error{"Cannot create pending qSink correction for qubit outside qreg"};

    if(pending_s_[qubit])
        throw std::logic_error{"qSink pending correction already exists for qubit"};

    pending_s_[qubit] = PendingConditionalS{
            .qubit = qubit,
            .kind = PendingSKind::S,
            .source_t_is_dagger = source_t_is_dagger
    };
    stats_.pending_corrections_created++;
}

bool QSinkInstructionStreamFromGateStream::has_input_gate() const
{
    return !input_buffer_.empty() || gate_stream_.has_next_gate();
}

SourcedGate QSinkInstructionStreamFromGateStream::get_next_input_gate()
{
    if(!input_buffer_.empty())
    {
        SourcedGate gate = std::move(input_buffer_.front());
        input_buffer_.pop_front();
        return gate;
    }

    return gate_stream_.get_next_sourced_gate();
}

const SourcedGate* QSinkInstructionStreamFromGateStream::peek_input_gate(size_t offset)
{
    while(input_buffer_.size() <= offset && gate_stream_.has_next_gate())
        input_buffer_.push_back(gate_stream_.get_next_sourced_gate());

    if(input_buffer_.size() <= offset)
        return nullptr;

    return &input_buffer_.at(offset);
}

void QSinkInstructionStreamFromGateStream::record_pending_patterns()
{
    for(const auto& pending_s : pending_s_)
        if(pending_s)
            record_patterns_for_pending(*pending_s);
}

void QSinkInstructionStreamFromGateStream::record_patterns_for_pending(const PendingConditionalS& pending_s)
{
    std::vector<gates::BasicSingleQubitGate::Type> same_qubit_gates;

    for(size_t offset = 0; same_qubit_gates.size() < 3; offset++)
    {
        const SourcedGate* sourced_gate = peek_input_gate(offset);
        if(!sourced_gate)
            break;
        const gates::Gate& gate = sourced_gate->gate;

        if(!gate_touches_qubit(gate, pending_s.qubit))
            continue;

        auto gate_type = supported_single_qubit_pattern_gate(gate, pending_s.qubit);
        if(!gate_type)
            break;

        if(is_pauli_gate(*gate_type))
            continue;

        same_qubit_gates.push_back(*gate_type);
    }

    if(same_qubit_gates.empty())
        return;

    using Type = gates::BasicSingleQubitGate::Type;
    if(same_qubit_gates[0] != Type::H)
        return;

    stats_.pattern_t_h_matches++;

    if(same_qubit_gates.size() < 2 || same_qubit_gates[1] != Type::S)
        return;

    stats_.pattern_t_h_s_matches++;

    if(same_qubit_gates.size() >= 3 && same_qubit_gates[2] == Type::T)
        stats_.pattern_t_h_s_t_matches++;
}

}
