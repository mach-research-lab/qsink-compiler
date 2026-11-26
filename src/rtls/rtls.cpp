#include <lsqecc/rtls/rtls.hpp>
#include <lsqecc/resource_profile.hpp>

#include <lsqecc/patches/slices_to_json.hpp>
#include <lsqecc/rtls/json_helpers.hpp>

#include <lstk/lstk.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace lsqecc {

namespace {

bool is_op_id(const std::string& operation_id)
{
    if(operation_id.rfind("op_", 0) != 0)
        return false;
    if(operation_id.size() == 3)
        return false;
    return std::all_of(operation_id.begin() + 3, operation_id.end(), [](unsigned char c) {
        return std::isdigit(c);
    });
}

const gates::ControlledGate* as_cnot(const gates::Gate& gate)
{
    const auto* controlled_gate = std::get_if<gates::ControlledGate>(&gate);
    if(!controlled_gate)
        return nullptr;
    const auto* target_gate = std::get_if<gates::BasicSingleQubitGate>(&controlled_gate->target_gate);
    if(!target_gate || target_gate->gate_type != gates::BasicSingleQubitGate::Type::X)
        return nullptr;
    return controlled_gate;
}

QubitNum cnot_target_qubit(const gates::ControlledGate& cnot)
{
    return std::get<gates::BasicSingleQubitGate>(cnot.target_gate).target_qubit;
}

std::optional<gates::BasicSingleQubitGate::Type> single_qubit_gate_type_on(
        const gates::Gate& gate,
        QubitNum qubit)
{
    if(const auto* basic_gate = std::get_if<gates::BasicSingleQubitGate>(&gate))
    {
        if(basic_gate->target_qubit == qubit)
            return basic_gate->gate_type;
    }
    return std::nullopt;
}

bool is_s_or_sdg(gates::BasicSingleQubitGate::Type gate_type)
{
    using Type = gates::BasicSingleQubitGate::Type;
    return gate_type == Type::S || gate_type == Type::SDg;
}

bool is_t_gate(const gates::Gate& gate, bool dagger)
{
    const auto* basic_gate = std::get_if<gates::BasicSingleQubitGate>(&gate);
    if(!basic_gate)
        return false;
    return basic_gate->gate_type == (dagger
            ? gates::BasicSingleQubitGate::Type::TDg
            : gates::BasicSingleQubitGate::Type::T);
}

std::queue<LSInstruction> queue_from_single(LSInstruction instruction)
{
    std::queue<LSInstruction> instructions;
    instructions.push(std::move(instruction));
    return instructions;
}

RtlsOperationMetadata make_metadata(std::string operation, std::string operation_id)
{
    return RtlsOperationMetadata{
            .operation = std::move(operation),
            .operation_id = std::move(operation_id)};
}

RtlsOperationMetadata make_correction_metadata(const RtlsCorrection& correction)
{
    RtlsOperationMetadata metadata =
            make_metadata(correction.correction_operation, correction.correction_operation_id);
    metadata.conditional = true;
    metadata.correction_for_operation_id = correction.source_operation_id;
    metadata.correction_anchor_operation_id = correction.anchor_operation_id;
    metadata.correction_post_anchor_operation_id = correction.post_anchor_operation_id;
    return metadata;
}

std::string pauli_frame_update_name(RtlsPauliFrameUpdateKind kind)
{
    switch(kind)
    {
    case RtlsPauliFrameUpdateKind::X: return "X";
    case RtlsPauliFrameUpdateKind::Z: return "Z";
    }
    LSTK_UNREACHABLE;
}

RtlsOperationMetadata make_pauli_frame_update_metadata(
        RtlsPauliFrameUpdateKind kind,
        std::string operation_id,
        const std::string& source_operation_id,
        const std::string& anchor_operation_id)
{
    RtlsOperationMetadata metadata =
            make_metadata(pauli_frame_update_name(kind), std::move(operation_id));
    metadata.conditional = true;
    metadata.correction_for_operation_id = source_operation_id;
    metadata.correction_anchor_operation_id = anchor_operation_id;
    return metadata;
}

SingleQubitOp::Operator pauli_frame_update_operator(RtlsPauliFrameUpdateKind kind)
{
    switch(kind)
    {
    case RtlsPauliFrameUpdateKind::X: return SingleQubitOp::Operator::X;
    case RtlsPauliFrameUpdateKind::Z: return SingleQubitOp::Operator::Z;
    }
    LSTK_UNREACHABLE;
}

LSInstruction make_pauli_frame_update_instruction(
        const RtlsPauliFrameUpdate& update,
        std::optional<PatchId> split_cnot_ancilla = std::nullopt)
{
    PatchId target = update.target_qubit;
    if(update.target == RtlsPauliFrameUpdateTarget::SplitCnotAncilla)
    {
        if(!split_cnot_ancilla)
            throw std::logic_error{"RTLS Pauli frame update could not resolve split-CNOT ancilla"};
        target = *split_cnot_ancilla;
    }

    LSInstruction instruction{
            .operation = {SingleQubitOp{target, pauli_frame_update_operator(update.kind)}}};
    instruction.rtls_operation = update.operation_handle;
    instruction.wait_for_idle_before_frame_update =
            update.placement == RtlsPauliFrameUpdatePlacement::BeforeOperation;
    return instruction;
}

void append_pauli_frame_updates(
        std::queue<LSInstruction>& instructions,
        const RtlsLogicalOperation& operation,
        RtlsPauliFrameUpdatePlacement placement,
        std::optional<PatchId> split_cnot_ancilla = std::nullopt,
        std::initializer_list<PatchId> dependency_clients = {})
{
    for(const RtlsPauliFrameUpdate& update : operation.pauli_frame_updates)
    {
        if(update.placement == placement)
        {
            LSInstruction instruction =
                    make_pauli_frame_update_instruction(update, split_cnot_ancilla);
            instruction.clients.insert(
                    dependency_clients.begin(),
                    dependency_clients.end());
            instructions.push(std::move(instruction));
        }
    }
}

bool is_mz_s_correction(const CompactRtlsOperationMetadata& metadata)
{
    return (metadata.operation == RtlsOperationCode::CondS ||
            metadata.operation == RtlsOperationCode::CondSdg) &&
           metadata.correction_post_anchor_operation_id != 0;
}

void stamp_instruction(LSInstruction& instruction, OperationHandle operation_handle)
{
    instruction.rtls_operation = operation_handle;
}

std::vector<SourcedGate> drain_gates(GateStream& gate_stream)
{
    std::vector<SourcedGate> gates;
    while(gate_stream.has_next_gate())
        gates.push_back(gate_stream.get_next_sourced_gate());
    return gates;
}

constexpr size_t kNoGateIndex = std::numeric_limits<size_t>::max();

using CompactOperationId = uint32_t;
constexpr CompactOperationId kInvalidCompactOperationId = 0;

class CompactOperationIdGenerator
{
public:
    CompactOperationId next()
    {
        if(next_id_ == std::numeric_limits<CompactOperationId>::max())
            throw std::overflow_error{"qSink compact operation ID space exhausted"};
        return next_id_++;
    }

private:
    CompactOperationId next_id_ = 1;
};

std::string serialize_operation_id(CompactOperationId operation_id)
{
    if(operation_id == kInvalidCompactOperationId)
        throw std::logic_error{"Cannot serialize an invalid qSink operation ID"};
    std::ostringstream out;
    out << "op_";
    out.width(6);
    out.fill('0');
    out << operation_id;
    return out.str();
}

struct GateSuccessors
{
    std::array<QubitNum, 2> qubits{};
    std::array<size_t, 2> next{kNoGateIndex, kNoGateIndex};
    uint8_t qubit_count = 0;

    size_t next_on(QubitNum qubit) const
    {
        for(uint8_t index = 0; index < qubit_count; index++)
            if(qubits[index] == qubit)
                return next[index];
        throw std::logic_error{"qSink successor lookup used a qubit not touched by the gate"};
    }
};

std::vector<GateSuccessors> build_per_qubit_successors(
        const std::vector<SourcedGate>& gates,
        QubitNum qubit_count,
        QsinkOptimizerStats& stats)
{
    std::vector<GateSuccessors> successors(gates.size());
    std::vector<size_t> next_gate_by_qubit(qubit_count, kNoGateIndex);

    for(size_t reverse_index = gates.size(); reverse_index > 0; reverse_index--)
    {
        const size_t gate_index = reverse_index - 1;
        GateSuccessors& gate_successors = successors[gate_index];
        for(QubitNum qubit : gates::get_operating_qubits(gates[gate_index].gate))
        {
            if(qubit >= qubit_count)
                throw std::logic_error{"Gate touches a qubit outside the declared qreg"};
            if(gate_successors.qubit_count == gate_successors.qubits.size())
                throw std::logic_error{"qSink optimizer supports at most two operands per gate"};

            const uint8_t operand = gate_successors.qubit_count++;
            gate_successors.qubits[operand] = qubit;
            gate_successors.next[operand] = next_gate_by_qubit[qubit];
            if(gate_successors.next[operand] != kNoGateIndex)
                stats.successor_link_count++;
            next_gate_by_qubit[qubit] = gate_index;
            stats.qubit_touch_count++;
        }
    }
    return successors;
}

struct PendingPauliFrameUpdate
{
    RtlsPauliFrameUpdateKind kind;
    RtlsPauliFrameUpdateTarget target;
    QubitNum target_qubit = 0;
    RtlsPauliFrameUpdatePlacement placement;
    CompactOperationId operation_id = kInvalidCompactOperationId;
    CompactOperationId source_operation_id = kInvalidCompactOperationId;
    CompactOperationId anchor_operation_id = kInvalidCompactOperationId;
};

enum class CompactCorrectionKind : uint8_t
{
    S,
    SDg,
    H
};

enum class CompactCorrectionLatency : uint8_t
{
    Immediate,
    Hs,
    Mz
};

struct PendingCorrection
{
    CompactOperationId source_operation_id = kInvalidCompactOperationId;
    CompactOperationId correction_operation_id = kInvalidCompactOperationId;
    CompactCorrectionKind kind = CompactCorrectionKind::S;
    QubitNum target_qubit = 0;
    CompactCorrectionLatency latency = CompactCorrectionLatency::Immediate;
    CompactOperationId anchor_operation_id = kInvalidCompactOperationId;
    std::optional<CompactOperationId> post_anchor_operation_id;
};

std::string correction_operation_name(CompactCorrectionKind kind)
{
    switch(kind)
    {
    case CompactCorrectionKind::S: return "CondS";
    case CompactCorrectionKind::SDg: return "CondSdg";
    case CompactCorrectionKind::H: return "CondH";
    }
    LSTK_UNREACHABLE;
}

RtlsCorrectionLatency correction_latency(CompactCorrectionLatency latency)
{
    switch(latency)
    {
    case CompactCorrectionLatency::Immediate: return 0;
    case CompactCorrectionLatency::Hs: return std::string{"hs"};
    case CompactCorrectionLatency::Mz: return std::string{"mz"};
    }
    LSTK_UNREACHABLE;
}

RtlsCorrection materialize_correction(const PendingCorrection& correction)
{
    return RtlsCorrection{
            .source_operation_id = serialize_operation_id(correction.source_operation_id),
            .correction_operation_id = serialize_operation_id(
                    correction.correction_operation_id),
            .correction_operation = correction_operation_name(correction.kind),
            .target_qubit = correction.target_qubit,
            .latency = correction_latency(correction.latency),
            .anchor_operation_id = serialize_operation_id(correction.anchor_operation_id),
            .post_anchor_operation_id = correction.post_anchor_operation_id
                    ? std::optional<std::string>{serialize_operation_id(
                            *correction.post_anchor_operation_id)}
                    : std::nullopt};
}

struct SplitCnotRewrite
{
    CompactOperationId mz_operation_id = kInvalidCompactOperationId;
    CompactOperationId mx_operation_id = kInvalidCompactOperationId;
    QubitNum control_qubit = 0;
    QubitNum target_qubit = 0;
    gates::CNOTType cnot_type = gates::CNOTType::ZX_WITH_MBM_CONTROL_FIRST;
    gates::CNOTAncillaPlacement ancilla_placement =
            gates::CNOTAncillaPlacement::ANCILLA_NEXT_TO_CONTROL;
    std::vector<PendingPauliFrameUpdate> mz_frame_updates;
    std::vector<PendingPauliFrameUpdate> mx_frame_updates;
};

RtlsSplitCnot materialize_split_cnot(const SplitCnotRewrite& split)
{
    return RtlsSplitCnot{
            .split_id = serialize_operation_id(split.mz_operation_id),
            .control_qubit = split.control_qubit,
            .target_qubit = split.target_qubit,
            .cnot_type = split.cnot_type,
            .ancilla_placement = split.ancilla_placement};
}

struct GateRewriteRecord
{
    std::optional<CompactOperationId> normal_operation_id;
    std::optional<CompactOperationId> t_source_operation_id;
    std::optional<PendingCorrection> correction;
    std::optional<SplitCnotRewrite> split_cnot;
    std::vector<PendingPauliFrameUpdate> pauli_frame_updates;
};

RtlsPauliFrameUpdate materialize_frame_update(
        RtlsProgram& program,
        PendingPauliFrameUpdate update)
{
    const OperationHandle handle = program.operation_metadata.add(
            make_pauli_frame_update_metadata(
                    update.kind,
                    serialize_operation_id(update.operation_id),
                    serialize_operation_id(update.source_operation_id),
                    serialize_operation_id(update.anchor_operation_id)));
    return RtlsPauliFrameUpdate{
            .kind = update.kind,
            .target = update.target,
            .target_qubit = update.target_qubit,
            .placement = update.placement,
            .operation_handle = handle};
}

std::vector<RtlsPauliFrameUpdate> materialize_frame_updates(
        RtlsProgram& program,
        std::vector<PendingPauliFrameUpdate> updates)
{
    std::vector<RtlsPauliFrameUpdate> result;
    result.reserve(updates.size());
    for(PendingPauliFrameUpdate& update : updates)
        result.push_back(materialize_frame_update(program, std::move(update)));
    return result;
}

struct OperationSliceRange
{
    int first = -1;
    int last = -1;
};

std::unordered_map<std::string, OperationSliceRange> operation_ranges_from_slices(
        const std::vector<DenseSlice>& slices)
{
    std::unordered_map<std::string, OperationSliceRange> ranges;
    for(size_t slice_index = 0; slice_index < slices.size(); slice_index++)
    {
        slices[slice_index].traverse_cells([&](const Cell&, const DensePatch* patch) {
            if(!patch ||
               (patch->activity != PatchActivity::Measurement &&
                patch->activity != PatchActivity::Unitary &&
                patch->activity != PatchActivity::MultiPatchMeasurement &&
                patch->activity != PatchActivity::Rotation) ||
               !patch->rtls_operation)
                return;
            const OperationMetadataArena* arena =
                    slices[slice_index].operation_metadata_arena;
            if(!arena)
                throw std::logic_error{"RTLS patch handle has no metadata arena"};
            const auto& metadata = arena->at(*patch->rtls_operation);
            auto& range = ranges[metadata.operation_id];
            if(range.first < 0)
                range.first = static_cast<int>(slice_index);
            range.last = static_cast<int>(slice_index);
        });
    }
    return ranges;
}

std::unordered_map<std::string, int> first_slice_by_operation_id(const nlohmann::json& schedule)
{
    std::unordered_map<std::string, int> first_slice;
    for(size_t slice_index = 0; slice_index < schedule.size(); slice_index++)
    {
        for(const auto& row : schedule[slice_index])
        {
            for(const auto& cell : row)
            {
                if(!rtls_is_active_cell(cell))
                    continue;
                const auto& activity = cell["activity"];
                if(!activity.contains("operation-id") || !activity["operation-id"].is_string())
                    continue;
                const std::string operation_id = activity["operation-id"];
                if(!first_slice.contains(operation_id))
                    first_slice[operation_id] = static_cast<int>(slice_index);
            }
        }
    }
    return first_slice;
}

void update_source_links(
        nlohmann::json& schedule,
        const std::unordered_map<std::string, int>& correction_slice_by_source)
{
    auto update_activity = [&](nlohmann::json& activity) {
        if(!activity.contains("operation-id") || !activity["operation-id"].is_string())
            return;
        const std::string operation_id = activity["operation-id"];
        auto correction_slice = correction_slice_by_source.find(operation_id);
        if(correction_slice != correction_slice_by_source.end())
            activity["conditional-correction-id"] = correction_slice->second;
    };

    for(auto& slice : schedule)
    {
        for(auto& row : slice)
        {
            for(auto& cell : row)
            {
                if(!cell.is_object() || !cell.contains("activity") || !cell["activity"].is_object())
                    continue;
                auto& activity = cell["activity"];
                if(rtls_is_active_cell(cell))
                    update_activity(activity);
                if(activity.contains("frame-updates") && activity["frame-updates"].is_array())
                    for(auto& frame_update : activity["frame-updates"])
                        if(frame_update.is_object())
                            update_activity(frame_update);
            }
        }
    }
}

void update_correction_source_slices(
        nlohmann::json& schedule,
        const std::unordered_map<std::string, int>& source_slice_by_operation_id)
{
    auto update_activity = [&](nlohmann::json& activity) {
        if(!activity.value("conditional", false))
            return;
        if(!activity.contains("correction-for-operation-id") ||
           !activity["correction-for-operation-id"].is_string())
            throw std::runtime_error{"RTLS correction missing correction-for-operation-id"};
        const std::string source_id = activity["correction-for-operation-id"];
        auto source_slice = source_slice_by_operation_id.find(source_id);
        if(source_slice == source_slice_by_operation_id.end())
            throw std::runtime_error{"RTLS correction source operation does not appear in schedule"};
        activity["condition-source-slice"] = source_slice->second;
    };

    for(auto& slice : schedule)
    {
        for(auto& row : slice)
        {
            for(auto& cell : row)
            {
                if(!cell.is_object() || !cell.contains("activity") || !cell["activity"].is_object())
                    continue;
                auto& activity = cell["activity"];
                if(rtls_is_active_cell(cell))
                    update_activity(activity);
                if(activity.contains("frame-updates") && activity["frame-updates"].is_array())
                    for(auto& frame_update : activity["frame-updates"])
                        if(frame_update.is_object())
                            update_activity(frame_update);
            }
        }
    }
}

bool is_source_t_operation(const std::string& operation)
{
    return operation == "TStar" || operation == "TStardg";
}

bool slice_has_operation(const nlohmann::json& schedule, int slice_index, const std::string& operation)
{
    if(slice_index < 0 || slice_index >= static_cast<int>(schedule.size()))
        return false;
    for(const auto& row : schedule[slice_index])
        for(const auto& cell : row)
            if(rtls_is_active_cell(cell) && cell["activity"].value("operation", "") == operation)
                return true;
    return false;
}

bool later_slice_has_operation(const nlohmann::json& schedule, int first_slice, const std::string& operation)
{
    for(int slice_index = first_slice; slice_index < static_cast<int>(schedule.size()); slice_index++)
        if(slice_has_operation(schedule, slice_index, operation))
            return true;
    return false;
}

void validate_latency(const nlohmann::json& value)
{
    if(value.is_number_integer())
    {
        if(value.get<int>() != 0)
            throw std::runtime_error{"RTLS integer correction latency must be 0"};
        return;
    }
    if(value.is_string())
    {
        const std::string latency = value;
        if(latency != "hs" && latency != "mz")
            throw std::runtime_error{"RTLS string correction latency must be \"hs\" or \"mz\""};
        return;
    }
    throw std::runtime_error{"RTLS correction latency must be 0, \"hs\", or \"mz\""};
}

std::optional<std::string> optional_string_field(
        const nlohmann::json& activity,
        const char* field)
{
    if(!activity.contains(field))
        return std::nullopt;
    if(!activity[field].is_string())
        throw std::runtime_error{lstk::cat("RTLS activity.", field, " must be a string")};
    return activity[field].get<std::string>();
}

} // namespace

std::string RtlsOperationIdGenerator::next()
{
    std::ostringstream out;
    out << "op_";
    out.width(6);
    out.fill('0');
    out << next_id_++;
    return out.str();
}

std::string rtls_output_path(std::string_view path)
{
    constexpr std::string_view extension = ".rtls";
    if(path.size() >= extension.size() &&
       path.substr(path.size() - extension.size()) == extension)
        return std::string{path};
    return std::string{path} + std::string{extension};
}

std::string rtls_operation_name(const gates::BasicSingleQubitGate& gate)
{
    using Type = gates::BasicSingleQubitGate::Type;
    switch(gate.gate_type)
    {
    case Type::X: return "X";
    case Type::Y: return "Y";
    case Type::Z: return "Z";
    case Type::S: return "S";
    case Type::T: return "TStar";
    case Type::H: return "H";
    case Type::SDg: return "Sdg";
    case Type::TDg: return "TStardg";
    }
    LSTK_UNREACHABLE;
}

std::string rtls_operation_name(const gates::ControlledGate& gate)
{
    if(const auto* target_gate = std::get_if<gates::BasicSingleQubitGate>(&gate.target_gate))
    {
        if(target_gate->gate_type == gates::BasicSingleQubitGate::Type::X)
            return "BellBasedCNOT";
        if(target_gate->gate_type == gates::BasicSingleQubitGate::Type::Z)
            return "ControlledZ";
    }
    return "ControlledGate";
}

std::string rtls_operation_name(const gates::Reset& reset)
{
    LSTK_UNUSED(reset);
    return "Reset";
}

std::string rtls_operation_name(const gates::Gate& gate)
{
    return std::visit(lstk::overloaded{
        [](const gates::BasicSingleQubitGate& basic_gate) {
            return rtls_operation_name(basic_gate);
        },
        [](const gates::RZ& rz_gate) {
            LSTK_UNUSED(rz_gate);
            return std::string{"Rz"};
        },
        [](const gates::ControlledGate& controlled_gate) {
            return rtls_operation_name(controlled_gate);
        },
        [](const gates::Reset& reset) {
            return rtls_operation_name(reset);
        }
    }, gate);
}

std::string rtls_operation_name(const LSInstruction& instruction)
{
    return std::visit(lstk::overloaded{
        [](const DeclareLogicalQubitPatches&) { return std::string{"DeclareLogicalQubitPatches"}; },
        [](const SinglePatchMeasurement&) { return std::string{"SinglePatchMeasurement"}; },
        [](const MultiPatchMeasurement&) { return std::string{"MultiPatchMeasurement"}; },
        [](const PatchInit&) { return std::string{"PatchInit"}; },
        [](const BellPairInit&) { return std::string{"BellPairInit"}; },
        [](const MagicStateRequest&) { return std::string{"MagicStateRequest"}; },
        [](const YStateRequest&) { return std::string{"YStateRequest"}; },
        [](const SingleQubitOp& op) {
            switch(op.op)
            {
            case SingleQubitOp::Operator::X: return std::string{"X"};
            case SingleQubitOp::Operator::Y: return std::string{"Y"};
            case SingleQubitOp::Operator::Z: return std::string{"Z"};
            case SingleQubitOp::Operator::H: return std::string{"H"};
            case SingleQubitOp::Operator::S: return op.is_dagger ? std::string{"Sdg"} : std::string{"S"};
            }
            LSTK_UNREACHABLE;
        },
        [](const RotateSingleCellPatch&) { return std::string{"RotateSingleCellPatch"}; },
        [](const BusyRegion&) { return std::string{"BusyRegion"}; },
        [](const BellBasedCNOT&) { return std::string{"BellBasedCNOT"}; },
        [](const PatchReset&) { return std::string{"Reset"}; }
    }, instruction.operation);
}

QsinkOptimizedProgram optimize_qsink_program(
        GateStream& gate_stream,
        bool qsink,
        bool enable_ths_optimization,
        ResourceProfile* resource_profile,
        bool retain_serialized_metadata)
{
    RtlsProgram program;
    program.operation_metadata.set_retain_serialized_metadata(
            retain_serialized_metadata);
    program.qreg = gate_stream.get_qreg();

    const uint64_t parse_start = resource_profile
            ? resource_profile_now_ns()
            : 0;
    std::vector<SourcedGate> sourced_gates = drain_gates(gate_stream);
    if(resource_profile)
        resource_profile->parse_decompose_ns +=
                resource_profile_elapsed_ns(parse_start);
    const uint64_t optimize_start = resource_profile
            ? resource_profile_now_ns()
            : 0;
    program.optimizer_stats.input_gate_count = sourced_gates.size();
    if(std::optional<QasmSourceDocument> source_document =
               gate_stream.take_qasm_source_document())
    {
        program.qasm_source_document = std::move(*source_document);
        program.source_operation_map.operation_handles_by_statement.resize(
                program.qasm_source_document->statements.size());
    }

    auto record_source_operation = [&](size_t gate_index, OperationHandle operation_handle) {
        if(sourced_gates[gate_index].source_statement_index)
            program.source_operation_map.add_operation(
                    *sourced_gates[gate_index].source_statement_index,
                    operation_handle);
    };

    CompactOperationIdGenerator operation_ids;
    std::vector<GateRewriteRecord> rewrites(sourced_gates.size());

    for(size_t gate_index = 0; gate_index < sourced_gates.size(); gate_index++)
    {
        const gates::Gate& gate = sourced_gates[gate_index].gate;
        GateRewriteRecord& rewrite = rewrites[gate_index];
        if(is_t_gate(gate, false) || is_t_gate(gate, true))
        {
            const bool dagger = is_t_gate(gate, true);
            const auto& basic_gate = std::get<gates::BasicSingleQubitGate>(gate);
            const CompactOperationId source_id = operation_ids.next();
            const CompactOperationId correction_id = operation_ids.next();

            rewrite.t_source_operation_id = source_id;
            rewrite.correction = PendingCorrection{
                    .source_operation_id = source_id,
                    .correction_operation_id = correction_id,
                    .kind = dagger
                            ? CompactCorrectionKind::SDg
                            : CompactCorrectionKind::S,
                    .target_qubit = basic_gate.target_qubit,
                    .latency = CompactCorrectionLatency::Immediate,
                    .anchor_operation_id = source_id};

            if(dagger)
                program.input_tdg_count++;
            else
                program.input_t_count++;
            continue;
        }

        rewrite.normal_operation_id = operation_ids.next();
    }

    using Type = gates::BasicSingleQubitGate::Type;
    if(qsink)
    {
        const std::vector<GateSuccessors> successors = build_per_qubit_successors(
                sourced_gates,
                program.qreg.size,
                program.optimizer_stats);
        for(size_t gate_index = 0; gate_index < sourced_gates.size(); gate_index++)
        {
            GateRewriteRecord& source_rewrite = rewrites[gate_index];
            if(!source_rewrite.correction)
                continue;
            const auto& source_gate = std::get<gates::BasicSingleQubitGate>(
                    sourced_gates[gate_index].gate);
            const QubitNum qubit = source_gate.target_qubit;

            // Repeatedly commute a pending conditional S correction past the
            // leading S/Sdg gates in its per-qubit operation sequence.
            std::optional<size_t> last_commuting_s_gate_index;
            std::array<size_t, 2> following_same_qubit_gate_indices{
                    kNoGateIndex,
                    kNoGateIndex};
            size_t following_gate_count = 0;
            size_t next_gate = successors[gate_index].next_on(qubit);
            while(next_gate != kNoGateIndex)
            {
                program.optimizer_stats.rewrite_probe_count++;
                const auto gate_type = single_qubit_gate_type_on(
                        sourced_gates[next_gate].gate,
                        qubit);
                if(following_gate_count == 0 &&
                   gate_type &&
                   is_s_or_sdg(*gate_type))
                {
                    last_commuting_s_gate_index = next_gate;
                    next_gate = successors[next_gate].next_on(qubit);
                    continue;
                }

                following_same_qubit_gate_indices[following_gate_count++] = next_gate;
                if(following_gate_count == following_same_qubit_gate_indices.size())
                    break;
                next_gate = successors[next_gate].next_on(qubit);
            }

            PendingCorrection& correction = *source_rewrite.correction;
            if(last_commuting_s_gate_index)
            {
                const auto& s_operation_id =
                        rewrites[*last_commuting_s_gate_index].normal_operation_id;
                if(!s_operation_id)
                    throw std::logic_error{"qSink S/Sdg correction anchor is missing an operation ID"};
                correction.anchor_operation_id = *s_operation_id;
            }

            if(following_gate_count == 0)
                continue;

            const size_t first_same_qubit_gate = following_same_qubit_gate_indices[0];
            if(const auto* cnot = as_cnot(sourced_gates[first_same_qubit_gate].gate))
            {
                auto& split = rewrites[first_same_qubit_gate].split_cnot;
                if(!split)
                {
                    const CompactOperationId mz_operation_id = operation_ids.next();
                    const CompactOperationId mx_operation_id = operation_ids.next();
                    split = SplitCnotRewrite{
                            .mz_operation_id = mz_operation_id,
                            .mx_operation_id = mx_operation_id,
                            .control_qubit = cnot->control_qubit,
                            .target_qubit = cnot_target_qubit(*cnot),
                            .cnot_type = cnot->cnot_type,
                            .ancilla_placement = cnot->cnot_ancilla_placement};

                    // These byproducts belong only to qSink's default
                    // nonlocal, control-first split protocol.
                    if(cnot->cnot_type == gates::CNOTType::ZX_WITH_MBM_CONTROL_FIRST)
                    {
                        split->mz_frame_updates.push_back(PendingPauliFrameUpdate{
                                .kind = RtlsPauliFrameUpdateKind::X,
                                .target = RtlsPauliFrameUpdateTarget::LogicalQubit,
                                .target_qubit = split->target_qubit,
                                .placement = RtlsPauliFrameUpdatePlacement::AfterMzz,
                                .operation_id = operation_ids.next(),
                                .source_operation_id = mz_operation_id,
                                .anchor_operation_id = mz_operation_id});
                        split->mx_frame_updates.push_back(PendingPauliFrameUpdate{
                                .kind = RtlsPauliFrameUpdateKind::Z,
                                .target = RtlsPauliFrameUpdateTarget::SplitCnotAncilla,
                                .placement = RtlsPauliFrameUpdatePlacement::AfterMxx,
                                .operation_id = operation_ids.next(),
                                .source_operation_id = mx_operation_id,
                                .anchor_operation_id = mx_operation_id});
                        split->mx_frame_updates.push_back(PendingPauliFrameUpdate{
                                .kind = RtlsPauliFrameUpdateKind::X,
                                .target = RtlsPauliFrameUpdateTarget::LogicalQubit,
                                .target_qubit = split->target_qubit,
                                .placement =
                                        RtlsPauliFrameUpdatePlacement::AfterFinalAncillaMz,
                                .operation_id = operation_ids.next(),
                                .source_operation_id = mx_operation_id,
                                .anchor_operation_id = mx_operation_id});
                    }
                }

                correction.latency = CompactCorrectionLatency::Mz;
                correction.anchor_operation_id = split->mz_operation_id;
                correction.post_anchor_operation_id = split->mx_operation_id;
                continue;
            }

            if(following_gate_count != 2)
                continue;
            const auto first_type = single_qubit_gate_type_on(
                    sourced_gates[first_same_qubit_gate].gate,
                    qubit);
            const auto second_type = single_qubit_gate_type_on(
                    sourced_gates[following_same_qubit_gate_indices[1]].gate,
                    qubit);
            if(!enable_ths_optimization ||
               first_type != Type::H ||
               second_type != Type::S)
                continue;

            const auto& s_operation_id =
                    rewrites[following_same_qubit_gate_indices[1]].normal_operation_id;
            if(!s_operation_id)
                continue;

            correction.kind = CompactCorrectionKind::H;
            correction.latency = CompactCorrectionLatency::Hs;
            correction.anchor_operation_id = *s_operation_id;
            // The auxiliary X is tied to this exact H/S match, but the S owns
            // the visible shared slice and is therefore its serialized anchor.
            rewrites[first_same_qubit_gate].pauli_frame_updates.push_back(
                    PendingPauliFrameUpdate{
                            .kind = RtlsPauliFrameUpdateKind::X,
                            .target = RtlsPauliFrameUpdateTarget::LogicalQubit,
                            .target_qubit = qubit,
                            .placement = RtlsPauliFrameUpdatePlacement::BeforeOperation,
                            .operation_id = operation_ids.next(),
                            .source_operation_id = correction.source_operation_id,
                            .anchor_operation_id = *s_operation_id});
        }
    }

    std::unordered_map<CompactOperationId, std::vector<size_t>>
            correction_gate_indices_by_anchor;
    for(size_t gate_index = 0; gate_index < rewrites.size(); gate_index++)
    {
        if(!rewrites[gate_index].correction)
            continue;
        program.corrections.push_back(materialize_correction(
                *rewrites[gate_index].correction));
        correction_gate_indices_by_anchor[rewrites[gate_index].correction->anchor_operation_id]
                .push_back(gate_index);
    }
    for(auto& [anchor_id, correction_gate_indices] : correction_gate_indices_by_anchor)
    {
        LSTK_UNUSED(anchor_id);
        std::sort(correction_gate_indices.begin(), correction_gate_indices.end(),
                [&](size_t lhs, size_t rhs) {
                    return rewrites[lhs].correction->correction_operation_id <
                           rewrites[rhs].correction->correction_operation_id;
                });
    }

    auto append_corrections_for_anchor = [&](CompactOperationId anchor_operation_id) {
        auto corrections_here = correction_gate_indices_by_anchor.find(anchor_operation_id);
        if(corrections_here == correction_gate_indices_by_anchor.end())
            return;

        for(size_t correction_gate_index : corrections_here->second)
        {
            const RtlsCorrection correction = materialize_correction(
                    *rewrites[correction_gate_index].correction);
            program.operations.push_back(RtlsLogicalOperation{
                    .kind = RtlsLogicalOperation::Kind::Correction,
                    .operation_handle = program.operation_metadata.add(
                            make_correction_metadata(correction)),
                    .gate = std::nullopt,
                    .split_cnot = std::nullopt,
                    .target_qubit = correction.target_qubit});
        }
    };

    for(size_t gate_index = 0; gate_index < sourced_gates.size(); gate_index++)
    {
        gates::Gate& gate = sourced_gates[gate_index].gate;
        GateRewriteRecord& rewrite = rewrites[gate_index];
        if(rewrite.correction)
        {
            const bool dagger = is_t_gate(gate, true);
            const auto& basic_gate = std::get<gates::BasicSingleQubitGate>(gate);
            RtlsOperationMetadata source_metadata =
                    make_metadata(
                            dagger ? "TStardg" : "TStar",
                            serialize_operation_id(*rewrite.t_source_operation_id));
            source_metadata.conditional_correction_operation_id =
                    serialize_operation_id(
                            rewrite.correction->correction_operation_id);
            source_metadata.conditional_correction_latency =
                    correction_latency(rewrite.correction->latency);
            source_metadata.conditional_correction_anchor_operation_id =
                    serialize_operation_id(rewrite.correction->anchor_operation_id);
            source_metadata.conditional_correction_post_anchor_operation_id =
                    rewrite.correction->post_anchor_operation_id
                            ? std::optional<std::string>{serialize_operation_id(
                                    *rewrite.correction->post_anchor_operation_id)}
                            : std::nullopt;

            const OperationHandle source_handle =
                    program.operation_metadata.add(std::move(source_metadata));
            program.operations.push_back(RtlsLogicalOperation{
                    .kind = dagger
                            ? RtlsLogicalOperation::Kind::TStardg
                            : RtlsLogicalOperation::Kind::TStar,
                    .operation_handle = source_handle,
                    .gate = std::nullopt,
                    .split_cnot = std::nullopt,
                    .target_qubit = basic_gate.target_qubit});
            record_source_operation(gate_index, source_handle);
            append_corrections_for_anchor(*rewrite.t_source_operation_id);
            continue;
        }

        if(rewrite.split_cnot)
        {
            SplitCnotRewrite& split = *rewrite.split_cnot;
            const RtlsSplitCnot serialized_split = materialize_split_cnot(split);
            const OperationHandle mz_handle = program.operation_metadata.add(
                    make_metadata(
                            "Mz",
                            serialize_operation_id(split.mz_operation_id)));
            program.operations.push_back(RtlsLogicalOperation{
                    .kind = RtlsLogicalOperation::Kind::Mz,
                    .operation_handle = mz_handle,
                    .gate = std::nullopt,
                    .split_cnot = serialized_split,
                    .target_qubit = split.target_qubit,
                    .pauli_frame_updates = materialize_frame_updates(
                            program,
                            std::move(split.mz_frame_updates))});
            record_source_operation(gate_index, mz_handle);
            append_corrections_for_anchor(split.mz_operation_id);
            program.operations.push_back(RtlsLogicalOperation{
                    .kind = RtlsLogicalOperation::Kind::Mx,
                    .operation_handle = program.operation_metadata.add(
                            make_metadata(
                                    "Mx",
                                    serialize_operation_id(split.mx_operation_id))),
                    .gate = std::nullopt,
                    .split_cnot = serialized_split,
                    .target_qubit = split.target_qubit,
                    .pauli_frame_updates = materialize_frame_updates(
                            program,
                            std::move(split.mx_frame_updates))});
            continue;
        }

        const OperationHandle gate_handle = program.operation_metadata.add(make_metadata(
                rtls_operation_name(gate),
                serialize_operation_id(*rewrite.normal_operation_id)));
        program.operations.push_back(RtlsLogicalOperation{
                .kind = RtlsLogicalOperation::Kind::Gate,
                .operation_handle = gate_handle,
                .gate = std::move(gate),
                .split_cnot = std::nullopt,
                .target_qubit = 0,
                .pauli_frame_updates = materialize_frame_updates(
                        program,
                        std::move(rewrite.pauli_frame_updates))});
        record_source_operation(gate_index, gate_handle);
        append_corrections_for_anchor(*rewrite.normal_operation_id);
    }

    if(resource_profile)
    {
        resource_profile->source_gates = program.optimizer_stats.input_gate_count;
        resource_profile->qubit_touches = program.optimizer_stats.qubit_touch_count;
        resource_profile->optimized_operations = program.operations.size();
        resource_profile->corrections = program.corrections.size();
        resource_profile->split_cnots = 0;
        resource_profile->frame_updates = 0;
        for(const RtlsLogicalOperation& operation : program.operations)
        {
            if(operation.kind == RtlsLogicalOperation::Kind::Mz && operation.split_cnot)
                resource_profile->split_cnots++;
            resource_profile->frame_updates += operation.pauli_frame_updates.size();
        }
        if(program.qasm_source_document)
            resource_profile->source_statements =
                    program.qasm_source_document->statements.size();
        for(const auto& handles :
            program.source_operation_map.operation_handles_by_statement)
            resource_profile->provenance_links += handles.size();
        resource_profile->compact_metadata_records =
                program.operation_metadata.size();
        resource_profile->serialized_metadata_records =
                program.operation_metadata.serialized_record_count();
        resource_profile->optimize_materialize_ns +=
                resource_profile_elapsed_ns(optimize_start);
    }
    return program;
}

RtlsProgram build_rtls_program(
        GateStream& gate_stream,
        bool qsink,
        bool enable_ths_optimization,
        ResourceProfile* resource_profile,
        bool retain_serialized_metadata)
{
    return optimize_qsink_program(
            gate_stream,
            qsink,
            enable_ths_optimization,
            resource_profile,
            retain_serialized_metadata);
}

RtlsInstructionStream::RtlsInstructionStream(
        const RtlsProgram& program,
        CNOTCorrectionMode cnot_correction_mode,
        IdGenerator& id_generator,
        bool local_instructions)
    : program_(program),
      operations_(program.operations),
      instruction_generator_(id_generator, local_instructions),
      cnot_correction_mode_(cnot_correction_mode),
      local_instructions_(local_instructions),
      id_generator_(id_generator)
{
    for(PatchId id = 0; id < program.qreg.size; id++)
        core_qubits_.insert(id);
    for(const RtlsCorrection& correction : program_.corrections)
    {
        correction_ids_by_anchor_[compact_operation_id(correction.anchor_operation_id)]
                .push_back(compact_operation_id(correction.correction_operation_id));
        if(correction.post_anchor_operation_id)
            correction_ids_by_post_anchor_[compact_operation_id(
                    *correction.post_anchor_operation_id)]
                    .push_back(compact_operation_id(
                            correction.correction_operation_id));
    }
}

RtlsInstructionStream::RtlsInstructionStream(
        std::vector<RtlsLogicalOperation>&& operations,
        const RtlsProgram& retained_program,
        CNOTCorrectionMode cnot_correction_mode,
        IdGenerator& id_generator,
        bool local_instructions)
    : program_(retained_program),
      operations_(std::move(operations)),
      instruction_generator_(id_generator, local_instructions),
      cnot_correction_mode_(cnot_correction_mode),
      local_instructions_(local_instructions),
      id_generator_(id_generator)
{
    for(PatchId id = 0; id < retained_program.qreg.size; id++)
        core_qubits_.insert(id);
    for(const RtlsCorrection& correction : program_.corrections)
    {
        correction_ids_by_anchor_[compact_operation_id(correction.anchor_operation_id)]
                .push_back(compact_operation_id(correction.correction_operation_id));
        if(correction.post_anchor_operation_id)
            correction_ids_by_post_anchor_[compact_operation_id(
                    *correction.post_anchor_operation_id)]
                    .push_back(compact_operation_id(
                            correction.correction_operation_id));
    }
}

LSInstruction RtlsInstructionStream::get_next_instruction()
{
    if(next_instructions_.empty())
    {
        lower_next_operation();
        release_operations_if_drained();
    }
    return lstk::queue_pop(next_instructions_);
}

bool RtlsInstructionStream::has_next_instruction() const
{
    return !next_instructions_.empty() || next_operation_ < operations_.size();
}

const tsl::ordered_set<PatchId>& RtlsInstructionStream::core_qubits() const
{
    return core_qubits_;
}

const OperationMetadataArena* RtlsInstructionStream::operation_metadata_arena() const
{
    return &program_.operation_metadata;
}

size_t RtlsInstructionStream::instruction_count_estimate() const
{
    // Optimized logical operations usually lower to one instruction; split T
    // and CNOT operations need several. Four is a conservative cheap hint.
    return next_instructions_.size() +
           (operations_.size() - next_operation_) * 4;
}

void RtlsInstructionStream::stamp_queue_if_missing(
        std::queue<LSInstruction>& instructions,
        OperationHandle operation_handle)
{
    std::queue<LSInstruction> stamped;
    while(!instructions.empty())
    {
        LSInstruction instruction = lstk::queue_pop(instructions);
        if(!instruction.rtls_operation)
            stamp_instruction(instruction, operation_handle);
        stamped.push(std::move(instruction));
    }
    instructions = std::move(stamped);
}

DependencyTokenId RtlsInstructionStream::dependency_token_for(
        std::unordered_map<uint32_t, DependencyTokenId>& tokens,
        uint32_t correction_operation_id)
{
    auto [token, inserted] = tokens.try_emplace(
            correction_operation_id,
            next_dependency_token_id_);
    if(inserted)
    {
        if(next_dependency_token_id_ == std::numeric_limits<DependencyTokenId>::max())
            throw std::overflow_error{"RTLS dependency-token ID space exhausted"};
        next_dependency_token_id_++;
    }
    return token->second;
}

void RtlsInstructionStream::lower_next_operation()
{
    if(next_operation_ >= operations_.size())
        throw std::logic_error{"RtlsInstructionStream: no more operations"};

    RtlsLogicalOperation operation = std::move(operations_[next_operation_++]);
    const CompactRtlsOperationMetadata& metadata =
            program_.compact_metadata_for(operation);

    switch(operation.kind)
    {
    case RtlsLogicalOperation::Kind::TStar:
    case RtlsLogicalOperation::Kind::TStardg:
    {
        bool dagger = operation.kind == RtlsLogicalOperation::Kind::TStardg;
        next_instructions_ = instruction_generator_.make_t_gate_body_instructions(
                operation.target_qubit,
                dagger);
        if(!next_instructions_.empty())
        {
            const auto* request = std::get_if<MagicStateRequest>(&next_instructions_.front().operation);
            if(request)
                t_source_dependency_patch_[metadata.operation_id] = request->target;

            // The final instruction, rather than the magic-state request,
            // defines completion of the physical T body. A dedicated token
            // lets the scheduler enforce an emitted-slice boundary when the
            // correction becomes ready in the same wave.
            next_instructions_.back().add_dependency_token(
                    dependency_token_for(
                            t_body_done_token_by_source_,
                            metadata.operation_id),
                    DependencyTokenRole::Produced,
                    DependencyTokenKind::RtlsTBodyDone);
        }
        stamp_queue_if_missing(next_instructions_, operation.operation_handle);
        return;
    }
    case RtlsLogicalOperation::Kind::Correction:
    {
        LSInstruction instruction;
        if(metadata.operation == RtlsOperationCode::CondS)
            instruction.operation = SingleQubitOp{operation.target_qubit, SingleQubitOp::Operator::S};
        else if(metadata.operation == RtlsOperationCode::CondSdg)
            instruction.operation = SingleQubitOp{operation.target_qubit, SingleQubitOp::Operator::S, true};
        else if(metadata.operation == RtlsOperationCode::CondH)
            instruction.operation = SingleQubitOp{operation.target_qubit, SingleQubitOp::Operator::H};
        else
            throw std::logic_error{"Unknown RTLS conditional correction operation"};

        if(metadata.operation == RtlsOperationCode::CondS ||
           metadata.operation == RtlsOperationCode::CondSdg)
            instruction.marks_rtls_semantic_start = true;

        if(is_mz_s_correction(metadata))
        {
            instruction.add_dependency_token(
                    dependency_token_for(
                            mz_ready_token_by_correction_,
                            metadata.operation_id),
                    DependencyTokenRole::Consumed,
                    DependencyTokenKind::RtlsMzReady);
            instruction.add_dependency_token(
                    dependency_token_for(
                            correction_done_token_by_correction_,
                            metadata.operation_id),
                    DependencyTokenRole::Produced,
                    DependencyTokenKind::RtlsCorrectionDone);
        }
        if(metadata.correction_for_operation_id != 0)
        {
            instruction.add_dependency_token(
                    dependency_token_for(
                            t_body_done_token_by_source_,
                            metadata.correction_for_operation_id),
                    DependencyTokenRole::Consumed,
                    DependencyTokenKind::RtlsTBodyDone);

            // The insertion anchor may move past commuting gates, but the
            // conditional result is still produced by the source T operation.
            auto t_dependency = t_source_dependency_patch_.find(
                    metadata.correction_for_operation_id);
            if(t_dependency != t_source_dependency_patch_.end())
                instruction.clients.insert(t_dependency->second);
        }

        stamp_instruction(instruction, operation.operation_handle);
        next_instructions_.push(std::move(instruction));
        return;
    }
    case RtlsLogicalOperation::Kind::Mz:
        next_instructions_ = lower_split_cnot_mz(operation);
        stamp_queue_if_missing(next_instructions_, operation.operation_handle);
        return;
    case RtlsLogicalOperation::Kind::Mx:
        next_instructions_ = lower_split_cnot_mx(operation);
        stamp_queue_if_missing(next_instructions_, operation.operation_handle);
        return;
    case RtlsLogicalOperation::Kind::Gate:
        break;
    }

    if(!operation.gate)
        throw std::logic_error{"RTLS gate operation missing gate payload"};

    const gates::Gate& gate = *operation.gate;
    std::queue<LSInstruction> lowered;
    if(const auto* sq_gate = std::get_if<gates::BasicSingleQubitGate>(&gate))
    {
        switch(sq_gate->gate_type)
        {
#define SINGLE_QUBIT_OP_CASE(Op) \
        case gates::BasicSingleQubitGate::Type::Op: \
            lowered.push({.operation={SingleQubitOp{sq_gate->target_qubit, SingleQubitOp::Operator::Op}}}); \
            break;
        SINGLE_QUBIT_OP_CASE(Z)
        SINGLE_QUBIT_OP_CASE(X)
        SINGLE_QUBIT_OP_CASE(Y)
        SINGLE_QUBIT_OP_CASE(H)
        SINGLE_QUBIT_OP_CASE(S)
#undef SINGLE_QUBIT_OP_CASE
        case gates::BasicSingleQubitGate::Type::SDg:
            lowered.push({.operation={SingleQubitOp{sq_gate->target_qubit, SingleQubitOp::Operator::S, true}}});
            break;
        case gates::BasicSingleQubitGate::Type::T:
        case gates::BasicSingleQubitGate::Type::TDg:
            throw std::logic_error{"T gates must be split before RTLS lowering"};
        }
    }
    else if(const auto* rz_gate = std::get_if<gates::RZ>(&gate))
    {
        if(rz_gate->pi_fraction.num == 1 && rz_gate->pi_fraction.den == 1)
            lowered.push({.operation={SingleQubitOp{rz_gate->target_qubit, SingleQubitOp::Operator::Z}}});
        else if(rz_gate->pi_fraction.num == 1 && rz_gate->pi_fraction.den == 2)
            lowered.push({.operation={SingleQubitOp{rz_gate->target_qubit, SingleQubitOp::Operator::S}}});
        else
            throw std::runtime_error(lstk::cat(
                    "RTLS lowering received undecomposed R_z(",
                    rz_gate->pi_fraction.num,
                    "/",
                    rz_gate->pi_fraction.den,
                    ")"));
    }
    else if(const auto* controlled_gate = std::get_if<gates::ControlledGate>(&gate))
    {
        if(const auto* target_gate = std::get_if<gates::BasicSingleQubitGate>(&controlled_gate->target_gate))
        {
            if(target_gate->gate_type == gates::BasicSingleQubitGate::Type::X)
            {
                lowered = instruction_generator_.make_cnot_instructions(
                        controlled_gate->control_qubit,
                        target_gate->target_qubit,
                        controlled_gate->cnot_type,
                        controlled_gate->cnot_ancilla_placement,
                        cnot_correction_mode_);
            }
            else if(target_gate->gate_type == gates::BasicSingleQubitGate::Type::Z)
            {
                lowered.push({.operation={SingleQubitOp{target_gate->target_qubit, SingleQubitOp::Operator::H}}});
                auto cnot = instruction_generator_.make_cnot_instructions(
                        controlled_gate->control_qubit,
                        target_gate->target_qubit,
                        controlled_gate->cnot_type,
                        controlled_gate->cnot_ancilla_placement,
                        cnot_correction_mode_);
                lstk::queue_extend(lowered, cnot);
                lowered.push({.operation={SingleQubitOp{target_gate->target_qubit, SingleQubitOp::Operator::H}}});
            }
            else
            {
                throw std::runtime_error{"RTLS lowering only supports controlled X/Z gates"};
            }
        }
        else
        {
            LSTK_NOT_IMPLEMENTED;
        }
    }
    else if(const auto* reset = std::get_if<gates::Reset>(&gate))
    {
        if(reset->register_name != program_.qreg.name)
            throw std::logic_error{"Invalid 'reset' register name"};
        lowered = queue_from_single({.operation={PatchReset{reset->target_qubit}}});
    }

    std::queue<LSInstruction> with_frame_updates;
    if(metadata.operation == RtlsOperationCode::S ||
       metadata.operation == RtlsOperationCode::Sdg)
    {
        if(lowered.empty())
            throw std::logic_error{"Ordinary S RTLS operation lowered to no instructions"};
        lowered.front().marks_rtls_semantic_start = true;
    }
    append_pauli_frame_updates(
            with_frame_updates,
            operation,
            RtlsPauliFrameUpdatePlacement::BeforeOperation);
    lstk::queue_extend(with_frame_updates, lowered);
    stamp_queue_if_missing(with_frame_updates, operation.operation_handle);
    next_instructions_ = std::move(with_frame_updates);
}

void RtlsInstructionStream::release_operations_if_drained()
{
    if(next_operation_ != operations_.size())
        return;
    std::vector<RtlsLogicalOperation>{}.swap(operations_);
    next_operation_ = 0;
}

std::queue<LSInstruction> RtlsInstructionStream::lower_split_cnot_mz(
        const RtlsLogicalOperation& operation)
{
    if(!operation.split_cnot)
        throw std::logic_error{"RTLS Mz operation missing split-CNOT payload"};

    const RtlsSplitCnot& split = *operation.split_cnot;
    const PatchId ancilla_id = id_generator_.new_id();
    split_cnot_ancilla_[split.split_id] = ancilla_id;

    std::optional<PlaceNexTo> place_ancilla_next_to;
    if(split.ancilla_placement == gates::CNOTAncillaPlacement::ANCILLA_NEXT_TO_CONTROL)
        place_ancilla_next_to = PlaceNexTo{split.control_qubit, PauliOperator::Z};
    else if(split.ancilla_placement == gates::CNOTAncillaPlacement::ANCILLA_NEXT_TO_TARGET)
        place_ancilla_next_to = PlaceNexTo{split.target_qubit, PauliOperator::X};

    std::queue<LSInstruction> instructions;
    instructions.push({
            .operation = {PatchInit{
                    ancilla_id,
                    PatchInit::InitializeableStates::Plus,
                    place_ancilla_next_to}},
            .clients = {split.control_qubit, split.target_qubit}});
    instructions.push({.operation = {MultiPatchMeasurement{.observable = {
            {split.control_qubit, PauliOperator::Z},
            {ancilla_id, PauliOperator::Z}},
            .is_negative = false}}});

    const auto& metadata = program_.compact_metadata_for(operation);
    auto corrections = correction_ids_by_anchor_.find(metadata.operation_id);
    if(corrections != correction_ids_by_anchor_.end())
        for(uint32_t correction_id : corrections->second)
        {
            instructions.back().add_dependency_token(
                    dependency_token_for(
                            mz_ready_token_by_correction_,
                            correction_id),
                    DependencyTokenRole::Produced,
                    DependencyTokenKind::RtlsMzReady);
        }
    if(!local_instructions_ &&
       split.cnot_type == gates::CNOTType::ZX_WITH_MBM_CONTROL_FIRST)
        append_pauli_frame_updates(
                instructions,
                operation,
                RtlsPauliFrameUpdatePlacement::AfterMzz,
                ancilla_id,
                {split.control_qubit, ancilla_id});
    return instructions;
}

std::queue<LSInstruction> RtlsInstructionStream::lower_split_cnot_mx(
        const RtlsLogicalOperation& operation)
{
    if(!operation.split_cnot)
        throw std::logic_error{"RTLS Mx operation missing split-CNOT payload"};

    const RtlsSplitCnot& split = *operation.split_cnot;
    auto ancilla = split_cnot_ancilla_.find(split.split_id);
    if(ancilla == split_cnot_ancilla_.end())
        throw std::logic_error{"RTLS Mx operation could not find its split-CNOT ancilla"};

    const PatchId ancilla_id = ancilla->second;
    split_cnot_ancilla_.erase(ancilla);

    std::queue<LSInstruction> instructions;
    instructions.push({.operation = {MultiPatchMeasurement{.observable = {
            {ancilla_id, PauliOperator::X},
            {split.target_qubit, PauliOperator::X}},
            .is_negative = false}}});
    const auto& metadata = program_.compact_metadata_for(operation);
    auto corrections = correction_ids_by_post_anchor_.find(metadata.operation_id);
    if(corrections != correction_ids_by_post_anchor_.end())
        for(uint32_t correction_id : corrections->second)
        {
            instructions.back().add_dependency_token(
                    dependency_token_for(
                            correction_done_token_by_correction_,
                            correction_id),
                    DependencyTokenRole::Consumed,
                    DependencyTokenKind::RtlsCorrectionDone);
        }
    // Only the requested nonlocal, control-first protocol ends in Z. Retain
    // the legacy X basis for local compilation and when qSink splits an
    // explicitly selected target-first or Bell-based CNOT through Mz/Mx.
    const PauliOperator final_observable =
            !local_instructions_ &&
            split.cnot_type == gates::CNOTType::ZX_WITH_MBM_CONTROL_FIRST
                    ? PauliOperator::Z
                    : PauliOperator::X;
    if(final_observable == PauliOperator::Z)
        append_pauli_frame_updates(
                instructions,
                operation,
                RtlsPauliFrameUpdatePlacement::AfterMxx,
                ancilla_id);
    instructions.push({.operation = {SinglePatchMeasurement{
            ancilla_id, final_observable, false}}});
    if(final_observable == PauliOperator::Z)
        append_pauli_frame_updates(
                instructions,
                operation,
                RtlsPauliFrameUpdatePlacement::AfterFinalAncillaMz,
                ancilla_id);
    return instructions;
}

nlohmann::json render_rtls_schedule(
        const std::vector<DenseSlice>& base_slices,
        const std::vector<RtlsCorrection>& corrections,
        RtlsValidationContext validation_context)
{
    const auto operation_ranges = operation_ranges_from_slices(base_slices);
    std::unordered_map<std::string, int> correction_slice_by_source;
    for(const RtlsCorrection& correction : corrections)
    {
        auto source_range = operation_ranges.find(correction.source_operation_id);
        if(source_range == operation_ranges.end() || source_range->second.first < 0)
            throw std::runtime_error{"RTLS correction source operation did not appear in active output"};
        auto correction_range = operation_ranges.find(correction.correction_operation_id);
        if(correction_range == operation_ranges.end() || correction_range->second.first < 0)
            throw std::runtime_error{"RTLS conditional correction operation did not appear in active output"};
        correction_slice_by_source[correction.source_operation_id] = correction_range->second.first;
    }

    nlohmann::json schedule = nlohmann::json::array();

    for(const DenseSlice& base_slice : base_slices)
        schedule.push_back(slice_to_json(base_slice));

    update_source_links(schedule, correction_slice_by_source);
    update_correction_source_slices(schedule, first_slice_by_operation_id(schedule));
    validate_rtls_schedule(schedule, validation_context);
    return schedule;
}

void validate_rtls_schedule(
        const nlohmann::json& schedule,
        RtlsValidationContext validation_context)
{
    if(!schedule.is_array())
        throw std::runtime_error{"RTLS schedule must be a JSON list"};

    std::unordered_map<std::string, std::string> operation_name_by_id;
    std::unordered_map<std::string, nlohmann::json> metadata_by_id;
    std::unordered_map<std::string, nlohmann::json> source_activity_by_id;
    std::unordered_map<std::string, nlohmann::json> correction_activity_by_id;
    std::unordered_map<std::string, nlohmann::json> auxiliary_pauli_activity_by_id;
    std::unordered_map<std::string, int> first_slice_by_id;
    std::unordered_map<std::string, int> last_slice_by_id;
    std::unordered_set<std::string> active_operation_ids;
    std::unordered_set<std::string> frame_update_operation_ids;
    std::unordered_set<std::string> conditional_non_pauli_frame_ids;
    std::unordered_set<std::string> tstar_ids;
    std::unordered_set<std::string> tstardg_ids;

    auto observe_activity = [&](const nlohmann::json& activity,
                                size_t slice_index,
                                bool frame_update) {
        const std::string operation = rtls_active_operation_name_from_activity(activity);
        const std::string operation_id = rtls_active_operation_id_from_activity(activity);

        if(!is_op_id(operation_id))
            throw std::runtime_error{
                    "RTLS operation-id must begin with op_ and contain decimal digits"};
        const bool conditional_frame =
                frame_update && activity.value("conditional", false);
        const bool auxiliary_pauli =
                conditional_frame && rtls_is_pauli_operation_name(operation);
        if(conditional_frame && !auxiliary_pauli)
            conditional_non_pauli_frame_ids.insert(operation_id);
        if(activity.contains("conditional") && !activity["conditional"].is_boolean())
            throw std::runtime_error{"RTLS activity.conditional must be a boolean"};

        auto [name_it, name_inserted] =
                operation_name_by_id.emplace(operation_id, operation);
        if(!name_inserted && name_it->second != operation)
            throw std::runtime_error{
                    "RTLS operation-id reused for different operation names"};

        const nlohmann::json metadata = rtls_metadata_signature(activity);
        auto [metadata_it, metadata_inserted] =
                metadata_by_id.emplace(operation_id, metadata);
        if(!metadata_inserted && metadata_it->second != metadata)
            throw std::runtime_error{"RTLS operation-id has inconsistent metadata"};

        if(auxiliary_pauli)
        {
            if(active_operation_ids.contains(operation_id))
                throw std::runtime_error{
                        "RTLS frame-update operation-id collides with an active operation"};
            frame_update_operation_ids.insert(operation_id);
        }
        else if(!frame_update)
        {
            if(frame_update_operation_ids.contains(operation_id))
                throw std::runtime_error{
                        "RTLS active operation-id collides with a frame update"};
            active_operation_ids.insert(operation_id);
        }

        if(!first_slice_by_id.contains(operation_id))
            first_slice_by_id[operation_id] = static_cast<int>(slice_index);
        last_slice_by_id[operation_id] = static_cast<int>(slice_index);

        if(frame_update)
        {
            if(auxiliary_pauli)
            {
                if(!activity.contains("correction-for-operation-id"))
                    throw std::runtime_error{
                            "RTLS auxiliary correction missing correction-for-operation-id"};
                if(!activity.contains("condition-source-slice"))
                    throw std::runtime_error{
                            "RTLS auxiliary correction missing condition-source-slice"};
                if(!activity.contains("correction-anchor-operation-id"))
                    throw std::runtime_error{
                            "RTLS auxiliary correction missing correction-anchor-operation-id"};
                auxiliary_pauli_activity_by_id[operation_id] = activity;
            }
            return;
        }

        if(operation == "TStar")
            tstar_ids.insert(operation_id);
        if(operation == "TStardg")
            tstardg_ids.insert(operation_id);

        if(is_source_t_operation(operation))
        {
            if(!activity.contains("conditional-correction-id"))
                throw std::runtime_error{
                        "RTLS TStar/TStardg missing conditional-correction-id"};
            if(!activity.contains("conditional-correction-operation-id"))
                throw std::runtime_error{
                        "RTLS TStar/TStardg missing conditional-correction-operation-id"};
            if(!activity.contains("conditional-correction-latency"))
                throw std::runtime_error{
                        "RTLS TStar/TStardg missing conditional-correction-latency"};
            const int correction_slice = activity["conditional-correction-id"];
            if(correction_slice < 0 ||
               correction_slice >= static_cast<int>(schedule.size()))
                throw std::runtime_error{
                        "RTLS conditional-correction-id points outside the schedule"};
            validate_latency(activity["conditional-correction-latency"]);
            source_activity_by_id[operation_id] = activity;
        }

        if(activity.value("conditional", false))
        {
            if(!activity.contains("correction-for-operation-id"))
                throw std::runtime_error{
                        "RTLS correction missing correction-for-operation-id"};
            if(!activity.contains("condition-source-slice"))
                throw std::runtime_error{"RTLS correction missing condition-source-slice"};
            correction_activity_by_id[operation_id] = activity;
        }
    };

    for(size_t slice_index = 0; slice_index < schedule.size(); slice_index++)
    {
        const auto& slice = schedule[slice_index];
        if(!slice.is_array())
            throw std::runtime_error{"Every RTLS schedule element must be a 2D grid"};

        for(const auto& row : slice)
        {
            if(!row.is_array())
                throw std::runtime_error{"Every RTLS schedule element must be a 2D grid"};

            for(const auto& cell : row)
            {
                if(!cell.is_null() && !cell.is_object())
                    throw std::runtime_error{"Every RTLS cell must be null or an object"};
                if(!cell.is_object())
                    continue;
                const auto& activity = cell["activity"];
                if(!activity.is_object())
                    throw std::runtime_error{"Every RTLS cell must contain an activity object"};
                if(rtls_is_active_cell(cell))
                    observe_activity(activity, slice_index, false);
                if(!activity.contains("frame-updates"))
                    continue;
                if(!activity["frame-updates"].is_array())
                    throw std::runtime_error{"RTLS activity.frame-updates must be an array"};
                for(const auto& frame_update : activity["frame-updates"])
                {
                    if(!frame_update.is_object())
                        throw std::runtime_error{"RTLS frame update must be an object"};
                    observe_activity(frame_update, slice_index, true);
                }
            }
        }
    }

    for(const std::string& operation_id : conditional_non_pauli_frame_ids)
        if(!active_operation_ids.contains(operation_id))
            throw std::runtime_error{
                    "RTLS standalone conditional frame update operation must be X, Y, or Z"};

    if(tstar_ids.size() != validation_context.input_t_count)
        throw std::runtime_error{"RTLS TStar count does not match input t count"};
    if(tstardg_ids.size() != validation_context.input_tdg_count)
        throw std::runtime_error{"RTLS TStardg count does not match input tdg count"};

    for(const auto& [source_id, source_activity] : source_activity_by_id)
    {
        const std::string correction_id = source_activity["conditional-correction-operation-id"];
        const int correction_slice = source_activity["conditional-correction-id"];
        bool found_in_referenced_slice = false;

        auto correction_first = first_slice_by_id.find(correction_id);
        auto correction_last = last_slice_by_id.find(correction_id);
        if(correction_first == first_slice_by_id.end() ||
           correction_last == last_slice_by_id.end())
            throw std::runtime_error{"RTLS source correction operation ID does not appear in schedule"};
        if(correction_first->second != correction_slice)
            throw std::runtime_error{"RTLS conditional-correction-id must point to the first correction slice"};
        if(correction_first->second <= first_slice_by_id.at(source_id))
            throw std::runtime_error{
                    "RTLS T correction must start after its source T operation " +
                    source_id + " (source slice " +
                    std::to_string(first_slice_by_id.at(source_id)) +
                    ", correction slice " +
                    std::to_string(correction_first->second) + ")"};

        for(const auto& row : schedule[correction_slice])
        {
            for(const auto& cell : row)
            {
                if(!rtls_is_active_cell(cell))
                    continue;
                const auto& activity = cell["activity"];
                if(activity.contains("operation-id") &&
                   activity["operation-id"].is_string() &&
                   activity["operation-id"] == correction_id)
                    found_in_referenced_slice = true;
            }
        }
        if(!found_in_referenced_slice)
            throw std::runtime_error{"RTLS source correction operation ID not present in referenced slice"};

        auto correction = correction_activity_by_id.find(correction_id);
        if(correction == correction_activity_by_id.end())
            throw std::runtime_error{"RTLS source points to a non-conditional correction operation"};
        if(correction->second["correction-for-operation-id"] != source_id)
            throw std::runtime_error{"RTLS correction does not point back to its source operation ID"};

        const auto source_anchor = optional_string_field(
                source_activity,
                "conditional-correction-anchor-operation-id");
        const auto correction_anchor = optional_string_field(
                correction->second,
                "correction-anchor-operation-id");
        const auto source_post_anchor = optional_string_field(
                source_activity,
                "conditional-correction-post-anchor-operation-id");
        const auto correction_post_anchor = optional_string_field(
                correction->second,
                "correction-post-anchor-operation-id");

        if(source_anchor || correction_anchor)
        {
            if(!source_anchor || !correction_anchor || *source_anchor != *correction_anchor)
                throw std::runtime_error{"RTLS correction anchor operation ID link is inconsistent"};

            auto anchor_name = operation_name_by_id.find(*source_anchor);
            if(anchor_name == operation_name_by_id.end())
                throw std::runtime_error{"RTLS correction anchor operation does not appear in schedule"};
        }

        if(source_post_anchor || correction_post_anchor)
        {
            if(!source_post_anchor || !correction_post_anchor ||
               *source_post_anchor != *correction_post_anchor)
                throw std::runtime_error{"RTLS correction post-anchor operation ID link is inconsistent"};

            auto post_name = operation_name_by_id.find(*source_post_anchor);
            if(post_name == operation_name_by_id.end())
                throw std::runtime_error{"RTLS correction post-anchor operation does not appear in schedule"};
            auto post_first = first_slice_by_id.find(*source_post_anchor);
            if(post_first == first_slice_by_id.end() ||
               !(correction_last->second < post_first->second))
                throw std::runtime_error{"RTLS correction must appear before its post-anchor operation"};
        }

        const auto& latency = source_activity["conditional-correction-latency"];
        if(latency.is_string() && latency.get<std::string>() == "mz")
        {
            const std::string correction_operation = correction->second["operation"];
            if(correction_operation != "CondS" && correction_operation != "CondSdg")
                throw std::runtime_error{"RTLS mz latency must point to a conditional S correction"};
            if(source_anchor || correction_anchor)
            {
                if(!source_anchor || operation_name_by_id[*source_anchor] != "Mz")
                    throw std::runtime_error{"RTLS mz correction anchor must be a visible Mz operation"};
                if(!source_post_anchor || !correction_post_anchor)
                    throw std::runtime_error{"RTLS mz correction missing post-anchor operation ID"};
                if(operation_name_by_id[*source_post_anchor] != "Mx")
                    throw std::runtime_error{"RTLS mz correction post-anchor must be a visible Mx operation"};
                auto anchor_last = last_slice_by_id.find(*source_anchor);
                auto post_first = first_slice_by_id.find(*source_post_anchor);
                if(anchor_last == last_slice_by_id.end() ||
                   !(anchor_last->second <= correction_first->second))
                    throw std::runtime_error{"RTLS mz correction must not start before its Mz anchor operation"};
                if(post_first == first_slice_by_id.end() ||
                   !(correction_last->second < post_first->second))
                    throw std::runtime_error{"RTLS correction must appear before its post-anchor operation"};
            }
            else
            {
                if(!slice_has_operation(schedule, correction_first->second - 1, "Mz"))
                    throw std::runtime_error{"RTLS mz correction must follow a visible Mz operation"};
                if(!later_slice_has_operation(schedule, correction_last->second + 1, "Mx"))
                    throw std::runtime_error{"RTLS mz correction must precede a visible Mx operation"};
            }
        }
    }

    for(const auto& [correction_id, correction_activity] : correction_activity_by_id)
    {
        const std::string source_id = correction_activity["correction-for-operation-id"];
        auto source = source_activity_by_id.find(source_id);
        if(source == source_activity_by_id.end())
            throw std::runtime_error{"RTLS correction source is not a TStar/TStardg operation"};
        if(source->second["conditional-correction-operation-id"] != correction_id)
            throw std::runtime_error{"RTLS source/correction operation-id link is inconsistent"};
        const int condition_source_slice = correction_activity["condition-source-slice"];
        auto actual_source_slice = first_slice_by_id.find(source_id);
        if(actual_source_slice == first_slice_by_id.end() ||
           actual_source_slice->second != condition_source_slice)
            throw std::runtime_error{"RTLS correction condition-source-slice is incorrect"};
    }

    for(const auto& [correction_id, correction_activity] :
        auxiliary_pauli_activity_by_id)
    {
        const auto source_id =
                optional_string_field(correction_activity, "correction-for-operation-id");
        if(!source_id || !active_operation_ids.contains(*source_id))
            throw std::runtime_error{
                    "RTLS auxiliary correction source operation does not appear in schedule"};
        if(!correction_activity["condition-source-slice"].is_number_integer())
            throw std::runtime_error{
                    "RTLS auxiliary correction condition-source-slice must be an integer"};
        const int condition_source_slice = correction_activity["condition-source-slice"];
        if(first_slice_by_id.at(*source_id) != condition_source_slice)
            throw std::runtime_error{
                    "RTLS auxiliary correction condition-source-slice is incorrect"};

        const auto anchor_id =
                optional_string_field(correction_activity, "correction-anchor-operation-id");
        if(!anchor_id || !active_operation_ids.contains(*anchor_id))
            throw std::runtime_error{
                    "RTLS auxiliary correction anchor operation does not appear in schedule"};
        if(first_slice_by_id.at(correction_id) < first_slice_by_id.at(*anchor_id) ||
           last_slice_by_id.at(correction_id) > last_slice_by_id.at(*anchor_id))
            throw std::runtime_error{
                    "RTLS auxiliary correction must share its visible anchor slice"};
    }

}

}
