#include <lsqecc/ls_instructions/catalytic_s_gate_injection_stream.hpp>


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

void stamp_queue(std::queue<LSInstruction>& instructions, const LSInstruction& source)
{
    std::queue<LSInstruction> stamped;
    while(!instructions.empty())
    {
        LSInstruction instruction = lstk::queue_pop(instructions);
        instruction.rtls_operation = source.rtls_operation;
        stamped.push(std::move(instruction));
    }
    instructions = std::move(stamped);
}

}

CatalyticSGateInjectionStream::CatalyticSGateInjectionStream(std::unique_ptr<LSInstructionStream> &&source,
                                                               IdGenerator& id_generator,
                                                               bool local_instruction,
                                                               bool always_rotate)
 : source_(std::move(source)), id_generator_(id_generator), instruction_generator_(id_generator, local_instruction), always_rotate_(always_rotate)
{}


bool CatalyticSGateInjectionStream::has_next_instruction() const
{
    return !next_instructions_.empty() || source_->has_next_instruction();
}


const tsl::ordered_set<PatchId> &CatalyticSGateInjectionStream::core_qubits() const
{
    return source_->core_qubits();
}

const OperationMetadataArena*
CatalyticSGateInjectionStream::operation_metadata_arena() const
{
    return source_->operation_metadata_arena();
}

size_t CatalyticSGateInjectionStream::instruction_count_estimate() const
{
    return next_instructions_.size() + source_->instruction_count_estimate();
}

LSInstruction CatalyticSGateInjectionStream::get_next_instruction()
{
    if(!next_instructions_.empty()) return lstk::queue_pop(next_instructions_);

    if(!source_->has_next_instruction())
        throw std::logic_error{"CatalyticSGateInjectionStream: No more instructions from source"};

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
    
        // Add gates
        auto instructions = instruction_generator_.make_cnot_instructions(
                                    gate->target,
                                    ystate_id,
                                    gates::CNOTType::ZX_WITH_MBM_CONTROL_FIRST,
                                    gates::CNOTAncillaPlacement::ANCILLA_NEXT_TO_CONTROL,
                                    CNOTCorrectionMode::NEVER);
        stamp_queue(instructions, new_instruction);
        lstk::queue_extend(next_instructions_, instructions);

        next_instructions_.push(with_rtls_metadata({.operation={
                SingleQubitOp{
                    .target = ystate_id,
                    .op = SingleQubitOp::Operator::H}}}, new_instruction));

        next_instructions_.push(with_rtls_metadata({.operation={RotateSingleCellPatch{ystate_id}}}, new_instruction));

        instructions = instruction_generator_.make_cnot_instructions(
                                    gate->target,
                                    ystate_id,
                                    gates::CNOTType::ZX_WITH_MBM_CONTROL_FIRST,
                                    gates::CNOTAncillaPlacement::ANCILLA_NEXT_TO_CONTROL,
                                    CNOTCorrectionMode::NEVER);
        stamp_queue(instructions, new_instruction);
        lstk::queue_extend(next_instructions_, instructions);

        next_instructions_.push(with_rtls_metadata({.operation={
                SingleQubitOp{
                    .target = ystate_id,
                    .op = SingleQubitOp::Operator::H}}}, new_instruction));

        next_instructions_.push(with_rtls_metadata({.operation={RotateSingleCellPatch{ystate_id}}}, new_instruction));

        // Unbind Y state
        LSInstruction unbind = with_rtls_metadata({
                .operation={YStateRequest{ystate_id, gate->target}},
                .wait_at_most_for=YStateRequest::DEFAULT_WAIT,
                .marks_rtls_semantic_start = new_instruction.marks_rtls_semantic_start},
                new_instruction);
        copy_completion_tokens(unbind, new_instruction);
        next_instructions_.push(std::move(unbind));
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
