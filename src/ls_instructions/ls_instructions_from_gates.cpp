#include <lsqecc/ls_instructions/ls_instructions_from_gates.hpp>

#include <lstk/lstk.hpp>

namespace lsqecc
{

LSIinstructionFromGatesGenerator::LSIinstructionFromGatesGenerator(IdGenerator& id_generator, bool local_instructions)
: id_generator_(id_generator), local_instructions_(local_instructions)
{}

std::queue<LSInstruction> LSIinstructionFromGatesGenerator::make_t_gate_instructions(PatchId target_id, bool is_dagger)
{
    auto next_instructions = make_t_gate_body_instructions(target_id, is_dagger);
    auto correction_instructions = make_t_gate_correction_instructions(target_id, is_dagger);
    lstk::queue_extend(next_instructions, correction_instructions);
    return next_instructions;
}

std::queue<LSInstruction> LSIinstructionFromGatesGenerator::make_t_gate_body_instructions(PatchId target_id, bool is_dagger)
{
    // Currently we ignore the is_dagger argument, since the procedure in this case is the same.
    LSTK_UNUSED(is_dagger);

    std::queue<LSInstruction> next_instructions;
    PatchId new_magic_state_id = id_generator_.new_id();
    next_instructions.push({.operation={MagicStateRequest{new_magic_state_id, target_id}}, .wait_at_most_for=MagicStateRequest::DEFAULT_WAIT, .clients={target_id}});
    if (local_instructions_)
    {
        auto instructions = make_cnot_instructions(
                            target_id,
                            new_magic_state_id,
                            gates::CNOTType::BELL_BASED,
                            gates::CNOTAncillaPlacement::USE_DEDICATED_CELL,
                            CNOTCorrectionMode::NEVER);
        lstk::queue_extend(next_instructions, instructions);
        next_instructions.push({.operation={SinglePatchMeasurement{new_magic_state_id, PauliOperator::Z, false}}});
    }
    else 
    {
        next_instructions.push({.operation={
                MultiPatchMeasurement{.observable={
                        {target_id,PauliOperator::Z},
                        {new_magic_state_id,PauliOperator::Z},
                },.is_negative=false}}});
        next_instructions.push({.operation={SinglePatchMeasurement{
                new_magic_state_id, PauliOperator::X, false
        }}});
    }
    
    return next_instructions;
}

std::queue<LSInstruction> LSIinstructionFromGatesGenerator::make_t_gate_correction_instructions(PatchId target_id, bool is_dagger)
{
    // Preserve the existing behavior: TDg is currently lowered like T.
    LSTK_UNUSED(is_dagger);

    std::queue<LSInstruction> next_instructions;
    next_instructions.push({.operation={SingleQubitOp{target_id, SingleQubitOp::Operator::S}}});
    return next_instructions;
}

std::queue<LSInstruction> LSIinstructionFromGatesGenerator::make_cnot_instructions(
        PatchId control_id,
        PatchId target_id,
        gates::CNOTType cnot_type,
        gates::CNOTAncillaPlacement cnot_ancilla_placement,
        CNOTCorrectionMode cnot_correction_mode)
{
    std::queue<LSInstruction> next_instructions;

    if ((cnot_type == gates::CNOTType::BELL_BASED) || local_instructions_== true) {
        PatchId id1 = id_generator_.new_id();
        PatchId id2 = id_generator_.new_id();
        next_instructions.push({.operation={BellBasedCNOT{control_id, target_id, id1, id2}}, .clients = {control_id, target_id}});

        if (cnot_correction_mode != CNOTCorrectionMode::NEVER) 
                {throw std::logic_error("Pauli corrections not implemented for Bell Based CNOTs");}
    }
    
    else {

        // Control is green -> smooth -> measures Z otimes Z
        // Target is red -> rough -> measures X otimes X

        std::optional<PlaceNexTo> place_ancilla_next_to;
        if(cnot_ancilla_placement == gates::CNOTAncillaPlacement::ANCILLA_NEXT_TO_CONTROL)
                place_ancilla_next_to = PlaceNexTo{control_id, PauliOperator::Z}; 
        else if(cnot_ancilla_placement == gates::CNOTAncillaPlacement::ANCILLA_NEXT_TO_TARGET)
                place_ancilla_next_to = PlaceNexTo{target_id, PauliOperator::X};

        PatchId ancilla_id = id_generator_.new_id();
        
        next_instructions.push(
        {
                .operation = {PatchInit{ancilla_id, PatchInit::InitializeableStates::Plus, place_ancilla_next_to}},
                .clients = {control_id, target_id}
        });
        
        if(cnot_type == gates::CNOTType::ZX_WITH_MBM_CONTROL_FIRST){
                next_instructions.push({.operation={
                        MultiPatchMeasurement{.observable={
                                {control_id, PauliOperator::Z},
                                {ancilla_id, PauliOperator::Z},
                        },.is_negative=false}}});
                if(cnot_correction_mode == CNOTCorrectionMode::ALWAYS)
                next_instructions.push({.operation={
                        SingleQubitOp{ancilla_id, SingleQubitOp::Operator::X}
                }});
                next_instructions.push({.operation={
                        MultiPatchMeasurement{.observable={
                                {ancilla_id,PauliOperator::X},
                                {target_id,PauliOperator::X},
                        },.is_negative=false}}});
                if(cnot_correction_mode == CNOTCorrectionMode::ALWAYS)
                next_instructions.push({.operation={
                        SingleQubitOp{control_id, SingleQubitOp::Operator::Z}
                }});
                // Complete the |+>-mediated, control-first CNOT by
                // destructively measuring the mediator in Z. The scheduler
                // keeps this immediate measurement in the M_XX slice.
                next_instructions.push({.operation={SinglePatchMeasurement{
                        ancilla_id, PauliOperator::Z, false}}});
        }
        else if(cnot_type == gates::CNOTType::ZX_WITH_MBM_TARGET_FIRST){
                next_instructions.push({.operation={
                        MultiPatchMeasurement{.observable={
                                {ancilla_id,PauliOperator::X},
                                {target_id,PauliOperator::X},
                        },.is_negative=false}}});
                if(cnot_correction_mode == CNOTCorrectionMode::ALWAYS)
                next_instructions.push({.operation={
                        SingleQubitOp{ancilla_id, SingleQubitOp::Operator::Z}
                }});
                next_instructions.push({.operation={
                        MultiPatchMeasurement{.observable={
                                {control_id, PauliOperator::Z},
                                {ancilla_id, PauliOperator::Z},
                        },.is_negative=false}}});
                if(cnot_correction_mode == CNOTCorrectionMode::ALWAYS)
                next_instructions.push({.operation={
                        SingleQubitOp{target_id, SingleQubitOp::Operator::X}
                }});
                // Preserve the distinct target-first protocol, which ends in
                // an X-basis mediator measurement after M_ZZ.
                next_instructions.push({.operation={SinglePatchMeasurement{
                        ancilla_id, PauliOperator::X, false}}});
        }
        else LSTK_UNREACHABLE;

    }


    return next_instructions;
}

}
