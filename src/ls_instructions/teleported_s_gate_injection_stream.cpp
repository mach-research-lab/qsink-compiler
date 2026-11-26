//
// Created by george on 26/09/22.
//

#include <lsqecc/ls_instructions/teleported_s_gate_injection_stream.hpp>


namespace lsqecc
{

namespace
{

LSInstruction with_rtls_metadata(LSInstruction instruction, const LSInstruction& source)
{
    instruction.rtls_operation = source.rtls_operation;
    return instruction;
}

void copy_start_dependencies(LSInstruction& instruction, const LSInstruction& source)
{
    for(PatchId client : source.clients)
        instruction.clients.insert(client);
    for(const DependencyToken& token : source.dependency_tokens)
        if(token.role == DependencyTokenRole::Consumed)
            instruction.add_dependency_token(token.id, token.role, token.kind);
}

void copy_completion_tokens(LSInstruction& instruction, const LSInstruction& source)
{
    for(const DependencyToken& token : source.dependency_tokens)
        if(token.role == DependencyTokenRole::Produced)
            instruction.add_dependency_token(token.id, token.role, token.kind);
}

}

TeleportedSGateInjectionStream::TeleportedSGateInjectionStream(std::unique_ptr<LSInstructionStream> &&source,
                                                               IdGenerator& id_generator,
                                                               bool always_rotate)
 : source_(std::move(source)), id_generator_(id_generator), always_rotate_(always_rotate)
{}


bool TeleportedSGateInjectionStream::has_next_instruction() const
{
    return !next_instructions_.empty() || source_->has_next_instruction();
}


const tsl::ordered_set<PatchId> &TeleportedSGateInjectionStream::core_qubits() const
{
    return source_->core_qubits();
}

const OperationMetadataArena*
TeleportedSGateInjectionStream::operation_metadata_arena() const
{
    return source_->operation_metadata_arena();
}

size_t TeleportedSGateInjectionStream::instruction_count_estimate() const
{
    return next_instructions_.size() + source_->instruction_count_estimate();
}

LSInstruction TeleportedSGateInjectionStream::get_next_instruction()
{
    if(!next_instructions_.empty()) return lstk::queue_pop(next_instructions_);

    if(!source_->has_next_instruction())
        throw std::logic_error{"TeleportedSGateInjectionStream: No more instructions from source"};

    LSInstruction new_instruction = source_->get_next_instruction();
    const auto* gate = std::get_if<SingleQubitOp>(&new_instruction.operation);
    if( gate && gate->op == SingleQubitOp::Operator::S && core_qubits().contains(gate->target))
    {
        const PatchId ystate_id = id_generator_.new_id();

        // Request Y state
        LSInstruction request = with_rtls_metadata({
                .operation={YStateRequest{ystate_id, gate->target}},
                .wait_at_most_for=YStateRequest::DEFAULT_WAIT,
                .clients={gate->target}}, new_instruction);
        copy_start_dependencies(request, new_instruction);
        next_instructions_.push(std::move(request));

        // Teleport the state
        next_instructions_.push(with_rtls_metadata({.operation={
                MultiPatchMeasurement{.observable={
                        {ystate_id, PauliOperator::Z},
                        {gate->target, PauliOperator::Z},
                },.is_negative=false}}}, new_instruction));
        next_instructions_.push(with_rtls_metadata({.operation={SinglePatchMeasurement{ystate_id, PauliOperator::X, false}}}, new_instruction));

        // This Pauli Z is applied probabilistically according to the XOR of the two measurement outcomes.
        LSInstruction corrective_z = with_rtls_metadata({
                .operation={SingleQubitOp{gate->target,SingleQubitOp::Operator::Z}},
                .marks_rtls_semantic_start = new_instruction.marks_rtls_semantic_start},
                new_instruction);
        if(new_instruction.has_produced_dependency_token())
            corrective_z.clients.insert(ystate_id);
        copy_completion_tokens(corrective_z, new_instruction);
        next_instructions_.push(std::move(corrective_z));
    }
    else if (always_rotate_ && gate 
        && gate->op == SingleQubitOp::Operator::H && core_qubits().contains(gate->target))
    {
        next_instructions_.push(new_instruction);
        next_instructions_.push(with_rtls_metadata({.operation={RotateSingleCellPatch{gate->target}}}, new_instruction));
    }
    else
    {
        next_instructions_.push(new_instruction);
    }

    return lstk::queue_pop(next_instructions_);
}

}
