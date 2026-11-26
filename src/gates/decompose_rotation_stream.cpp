#include <lsqecc/gates/decompose_rotation_stream.hpp>


namespace lsqecc
{

bool DecomposeRotationStream::has_next_gate() const
{
    return !next_gates_.empty() || input_gate_stream_->has_next_gate();
}

SourcedGate DecomposeRotationStream::get_next_sourced_gate()
{
    if(!next_gates_.empty()) return lstk::queue_pop(next_gates_);

    if(!input_gate_stream_->has_next_gate()) LSTK_UNREACHABLE;

    SourcedGate next_gate{input_gate_stream_->get_next_sourced_gate()};
    if(gates::is_decomposed(next_gate.gate)) return next_gate;

    auto decomposed = gates::decompose(next_gate.gate, rz_precision_log_ten_negative_);
    for (auto& gate : decomposed)
        next_gates_.push(SourcedGate{
                .gate = std::move(gate),
                .source_statement_index = next_gate.source_statement_index});
    
    return lstk::queue_pop(next_gates_);
}

gates::Gate DecomposeRotationStream::get_next_gate()
{
    return get_next_sourced_gate().gate;
}

}
