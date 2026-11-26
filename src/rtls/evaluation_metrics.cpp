#include <lsqecc/rtls/evaluation_metrics.hpp>

#include <charconv>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace lsqecc {

namespace {

using TimedOperationKind = EvaluationOperationKind;

struct TimedOperation
{
    TimedOperationKind kind = TimedOperationKind::Default;
    std::vector<QubitNum> qubits;
    size_t first_slice = 0;
};

struct ScheduleNode
{
    size_t operation_index = 0;
    std::optional<size_t> predecessor_node_index;
    double finish_time = 0;
};

struct QubitState
{
    double finish_time = 0;
    std::optional<size_t> node_index;
};

bool is_split_t(TimedOperationKind kind)
{
    return kind == TimedOperationKind::SplitT;
}

bool is_any_t(TimedOperationKind kind)
{
    return kind == TimedOperationKind::RegularT ||
           kind == TimedOperationKind::SplitT;
}

TimedOperationKind basic_gate_kind(gates::BasicSingleQubitGate::Type type)
{
    using Type = gates::BasicSingleQubitGate::Type;
    switch(type)
    {
    case Type::Z:
        return TimedOperationKind::Zero;
    case Type::H:
        return TimedOperationKind::H;
    case Type::S:
    case Type::SDg:
        return TimedOperationKind::S;
    case Type::T:
    case Type::TDg:
        return TimedOperationKind::RegularT;
    case Type::X:
    case Type::Y:
        return TimedOperationKind::Default;
    }
    throw std::logic_error{"Unknown basic gate type"};
}

TimedOperationKind correction_kind(std::string_view operation)
{
    if(operation == "CondH" || operation == "HStar" || operation == "H*")
        return TimedOperationKind::H;
    if(operation == "CondS" || operation == "CondSdg" ||
       operation == "SStar" || operation == "SdgStar" ||
       operation == "S*" || operation == "Sdg*")
    {
        return TimedOperationKind::S;
    }
    return TimedOperationKind::Default;
}

EvaluationOperation describe_operation(
        const RtlsProgram& program,
        const RtlsLogicalOperation& operation)
{
    EvaluationOperation compact{
            .operation_handle = operation.operation_handle};
    const RtlsOperationMetadata& metadata = program.metadata_for(operation);
    switch(operation.kind)
    {
    case RtlsLogicalOperation::Kind::TStar:
    case RtlsLogicalOperation::Kind::TStardg:
        compact.kind = TimedOperationKind::SplitT;
        compact.qubits[0] = operation.target_qubit;
        compact.qubit_count = 1;
        break;
    case RtlsLogicalOperation::Kind::Correction:
        compact.kind = correction_kind(metadata.operation);
        compact.qubits[0] = operation.target_qubit;
        compact.qubit_count = 1;
        break;
    case RtlsLogicalOperation::Kind::Mz:
        if(!operation.split_cnot)
            throw std::runtime_error{"RTLS Mz operation is missing split-CNOT metadata"};
        compact.kind = TimedOperationKind::SplitCnot;
        compact.qubits[0] = operation.split_cnot->control_qubit;
        compact.qubit_count = 1;
        break;
    case RtlsLogicalOperation::Kind::Mx:
        if(!operation.split_cnot)
            throw std::runtime_error{"RTLS Mx operation is missing split-CNOT metadata"};
        compact.kind = TimedOperationKind::SplitCnot;
        compact.qubits[0] = operation.split_cnot->target_qubit;
        compact.qubit_count = 1;
        break;
    case RtlsLogicalOperation::Kind::Gate:
        if(!operation.gate)
            throw std::runtime_error{"RTLS gate operation is missing its gate payload"};
        std::visit([&](const auto& gate) {
            using Gate = std::decay_t<decltype(gate)>;
            if constexpr(std::is_same_v<Gate, gates::BasicSingleQubitGate>)
            {
                compact.kind = basic_gate_kind(gate.gate_type);
                compact.qubits[0] = gate.target_qubit;
                compact.qubit_count = 1;
            }
            else if constexpr(std::is_same_v<Gate, gates::RZ>)
            {
                compact.kind = TimedOperationKind::Default;
                compact.qubits[0] = gate.target_qubit;
                compact.qubit_count = 1;
            }
            else if constexpr(std::is_same_v<Gate, gates::ControlledGate>)
            {
                compact.kind = TimedOperationKind::Default;
                compact.qubits[0] = gate.control_qubit;
                compact.qubit_count = 1;
                std::visit([&](const auto& target) {
                    compact.qubits[1] = target.target_qubit;
                    compact.qubit_count = 2;
                    using Target = std::decay_t<decltype(target)>;
                    if constexpr(std::is_same_v<Target, gates::BasicSingleQubitGate>)
                    {
                        if(target.gate_type == gates::BasicSingleQubitGate::Type::X ||
                           target.gate_type == gates::BasicSingleQubitGate::Type::S)
                        {
                            compact.kind = TimedOperationKind::Cnot;
                        }
                    }
                }, gate.target_gate);
            }
            else if constexpr(std::is_same_v<Gate, gates::Reset>)
            {
                compact.kind = TimedOperationKind::Zero;
                compact.qubits[0] = gate.target_qubit;
                compact.qubit_count = 1;
            }
        }, *operation.gate);
        break;
    }

    return compact;
}

TimedOperation adapt_operation(
        const EvaluationOperation& operation,
        const OperationMetadataArena& operation_metadata,
        const EvaluationSchedule& schedule)
{
    TimedOperation timed{
            .kind = operation.kind,
            .qubits = std::vector<QubitNum>(
                    operation.qubits.begin(),
                    operation.qubits.begin() + operation.qubit_count)};

    if(timed.kind == TimedOperationKind::Zero || timed.qubits.empty())
        return timed;

    if(operation.operation_handle >= schedule.first_applied.size())
    {
        throw std::runtime_error{
                "RTLS operation " +
                operation_metadata.at(operation.operation_handle).operation_id +
                " did not appear in any compiler slice"};
    }
    const SliceIndex visible = schedule.first_visible[operation.operation_handle];
    const SliceIndex applied = schedule.first_applied[operation.operation_handle];
    const RtlsOperationMetadata& metadata =
            operation_metadata.at(operation.operation_handle);
    const bool needs_semantic_start =
            metadata.operation == "S" || metadata.operation == "Sdg" ||
            metadata.operation == "CondS" || metadata.operation == "CondSdg";
    SliceIndex first_slice = kUnscheduledSlice;
    if(needs_semantic_start)
    {
        if(operation.operation_handle >= schedule.first_semantic_start.size())
            throw std::runtime_error{
                    "RTLS S-family operation " + metadata.operation_id +
                    " has no semantic-start event"};
        first_slice = schedule.first_semantic_start[operation.operation_handle];
        if(first_slice == kUnscheduledSlice)
            throw std::runtime_error{
                    "RTLS S-family operation " + metadata.operation_id +
                    " has no semantic-start event"};
    }
    else
    {
        first_slice = visible != kUnscheduledSlice ? visible : applied;
    }
    if(first_slice == kUnscheduledSlice)
    {
        throw std::runtime_error{
                "RTLS operation " + metadata.operation_id +
                " did not appear in any compiler slice"};
    }
    if(first_slice >= schedule.slice_count)
    {
        throw std::runtime_error{
                "RTLS operation " + metadata.operation_id +
                " resolves outside the completed compiler slices"};
    }
    timed.first_slice = first_slice;
    return timed;
}

bool correction_compatible(
        const RtlsProgram& program,
        const RtlsLogicalOperation& source,
        const RtlsLogicalOperation& correction)
{
    if(correction.kind != RtlsLogicalOperation::Kind::Correction)
        return false;
    const std::string& correction_operation =
            program.metadata_for(correction).operation;
    if(correction_operation == "CondH")
    {
        return source.kind == RtlsLogicalOperation::Kind::TStar ||
               source.kind == RtlsLogicalOperation::Kind::TStardg;
    }
    if(correction_operation == "CondS")
        return source.kind == RtlsLogicalOperation::Kind::TStar;
    if(correction_operation == "CondSdg")
        return source.kind == RtlsLogicalOperation::Kind::TStardg;
    return false;
}

struct CorrectionPairs
{
    std::unordered_map<size_t, size_t> correction_to_t;
    std::unordered_map<size_t, size_t> t_to_correction;
};

CorrectionPairs pair_corrections(const RtlsProgram& program)
{
    std::unordered_map<std::string, size_t> operation_index_by_id;
    for(size_t index = 0; index < program.operations.size(); index++)
    {
        const std::string& operation_id =
                program.metadata_for(program.operations[index]).operation_id;
        if(operation_id.empty())
            throw std::runtime_error{"RTLS operation has an empty operation ID"};
        if(!operation_index_by_id.emplace(operation_id, index).second)
        {
            throw std::runtime_error{
                    "Duplicate RTLS operation ID in evaluation input: " +
                    operation_id};
        }
    }

    CorrectionPairs pairs;
    for(const RtlsCorrection& correction_link : program.corrections)
    {
        const auto source =
                operation_index_by_id.find(correction_link.source_operation_id);
        const auto correction = operation_index_by_id.find(
                correction_link.correction_operation_id);
        if(source == operation_index_by_id.end())
        {
            throw std::runtime_error{
                    "RTLS correction source operation is missing: " +
                    correction_link.source_operation_id};
        }
        if(correction == operation_index_by_id.end())
        {
            throw std::runtime_error{
                    "RTLS correction operation is missing: " +
                    correction_link.correction_operation_id};
        }
        if(!correction_compatible(
                program,
                program.operations[source->second],
                program.operations[correction->second]))
        {
            throw std::runtime_error{
                    "RTLS correction " + correction_link.correction_operation_id +
                    " is incompatible with source " +
                    correction_link.source_operation_id};
        }
        if(!pairs.t_to_correction.emplace(source->second, correction->second).second)
        {
            throw std::runtime_error{
                    "RTLS T operation has multiple evaluation corrections: " +
                    correction_link.source_operation_id};
        }
        if(!pairs.correction_to_t.emplace(
                correction->second,
                source->second).second)
        {
            throw std::runtime_error{
                    "RTLS correction operation has multiple evaluation sources: " +
                    correction_link.correction_operation_id};
        }
    }
    return pairs;
}

CorrectionPairs pair_corrections(const EvaluationProgram& program)
{
    CorrectionPairs pairs;
    for(size_t operation_index = 0;
        operation_index < program.operations.size();
        operation_index++)
    {
        const uint32_t correction = program.correction_by_t[operation_index];
        if(correction != kNoEvaluationOperationIndex)
            pairs.t_to_correction.emplace(operation_index, correction);

        const uint32_t source_t = program.t_by_correction[operation_index];
        if(source_t != kNoEvaluationOperationIndex)
            pairs.correction_to_t.emplace(operation_index, source_t);
    }
    return pairs;
}

QubitState choose_latest_state(
        const std::vector<QubitState>& states)
{
    if(states.empty())
        return {};
    QubitState latest = states.front();
    for(size_t index = 1; index < states.size(); index++)
    {
        if(states[index].finish_time > latest.finish_time)
            latest = states[index];
    }
    return latest;
}

size_t append_schedule_node(
        std::vector<ScheduleNode>& nodes,
        size_t operation_index,
        std::optional<size_t> predecessor_node_index,
        double finish_time)
{
    const size_t node_index = nodes.size();
    nodes.push_back(ScheduleNode{
            .operation_index = operation_index,
            .predecessor_node_index = predecessor_node_index,
            .finish_time = finish_time});
    return node_index;
}

std::vector<size_t> path_indices_from_nodes(
        const std::vector<ScheduleNode>& nodes,
        std::optional<size_t> best_node_index)
{
    std::vector<size_t> reversed_indices;
    for(std::optional<size_t> current = best_node_index;
        current;
        current = nodes[*current].predecessor_node_index)
    {
        reversed_indices.push_back(nodes[*current].operation_index);
    }

    std::vector<size_t> path_indices;
    for(auto index = reversed_indices.rbegin();
        index != reversed_indices.rend();
        ++index)
    {
        if(path_indices.empty() || path_indices.back() != *index)
            path_indices.push_back(*index);
    }
    return path_indices;
}

double operation_duration(
        TimedOperationKind kind,
        int code_distance,
        double idle_wait)
{
    const double distance = static_cast<double>(code_distance);
    switch(kind)
    {
    case TimedOperationKind::Zero:
    case TimedOperationKind::Default:
        return 0;
    case TimedOperationKind::H:
        return 3 * distance;
    case TimedOperationKind::S:
        return 1.5 * distance;
    case TimedOperationKind::RegularT:
    case TimedOperationKind::SplitT:
        return distance + idle_wait;
    case TimedOperationKind::SplitCnot:
        return distance;
    case TimedOperationKind::Cnot:
        throw std::logic_error{"CNOT duration must use staggered scheduling"};
    }
    throw std::logic_error{"Unknown timed operation kind"};
}

std::string format_number(double value)
{
    if(value == 0)
        return "0";
    if(std::isinf(value))
        return value < 0 ? "-inf" : "inf";
    if(std::isnan(value))
        return "nan";

    std::array<char, 128> buffer{};
    const auto conversion = std::to_chars(
            buffer.data(),
            buffer.data() + buffer.size(),
            value,
            std::chars_format::general);
    if(conversion.ec != std::errc{})
        throw std::runtime_error{"Could not format evaluation metric"};
    return std::string{buffer.data(), conversion.ptr};
}

EvaluationMetrics evaluate_one(
        const std::vector<TimedOperation>& operations,
        const CorrectionPairs& correction_pairs,
        EvaluationInput input,
        EvaluationMethod method,
        double wait_factor)
{
    const double distance = static_cast<double>(input.code_distance);
    const double wait_duration = wait_factor * input.decoder_tw;
    std::unordered_map<QubitNum, QubitState> finish_by_qubit;
    std::vector<ScheduleNode> nodes;
    std::vector<double> idle_wait_by_operation(operations.size(), 0);
    std::vector<double> hidden_wait_by_operation(operations.size(), 0);
    std::optional<size_t> best_node_index;
    double best_finish_time = 0;

    for(size_t operation_index = 0;
        operation_index < operations.size();
        operation_index++)
    {
        const TimedOperation& operation = operations[operation_index];
        if(operation.kind == TimedOperationKind::Zero || operation.qubits.empty())
            continue;

        const double lower_bound_start =
                static_cast<double>(operation.first_slice) * distance;

        if(operation.kind == TimedOperationKind::Cnot &&
           operation.qubits.size() >= 2)
        {
            const QubitNum control = operation.qubits[0];
            const QubitNum target = operation.qubits[1];
            const QubitState control_state = finish_by_qubit.contains(control)
                    ? finish_by_qubit.at(control)
                    : QubitState{};
            const QubitState target_state = finish_by_qubit.contains(target)
                    ? finish_by_qubit.at(target)
                    : QubitState{};
            const QubitState lower_bound_predecessor =
                    choose_latest_state({control_state, target_state});

            double start_time = lower_bound_start;
            std::optional<size_t> predecessor =
                    lower_bound_predecessor.node_index;
            if(control_state.finish_time > start_time)
            {
                start_time = control_state.finish_time;
                predecessor = control_state.node_index;
            }
            if(target_state.finish_time - distance > start_time)
            {
                start_time = target_state.finish_time - distance;
                predecessor = target_state.node_index;
            }
            if(0 > start_time)
            {
                start_time = 0;
                predecessor.reset();
            }

            const double control_finish = start_time + distance;
            const double target_finish = start_time + 2 * distance;
            const size_t control_node = append_schedule_node(
                    nodes,
                    operation_index,
                    predecessor,
                    control_finish);
            const size_t target_node = append_schedule_node(
                    nodes,
                    operation_index,
                    predecessor,
                    target_finish);
            finish_by_qubit[control] = {control_finish, control_node};
            finish_by_qubit[target] = {target_finish, target_node};

            for(const auto& [node_index, finish_time] :
                {std::pair{control_node, control_finish},
                 std::pair{target_node, target_finish}})
            {
                if(finish_time > best_finish_time)
                {
                    best_finish_time = finish_time;
                    best_node_index = node_index;
                }
            }
            continue;
        }

        std::vector<QubitState> predecessor_states;
        predecessor_states.reserve(operation.qubits.size());
        for(QubitNum qubit : operation.qubits)
        {
            const auto state = finish_by_qubit.find(qubit);
            if(state != finish_by_qubit.end())
                predecessor_states.push_back(state->second);
        }
        const QubitState predecessor_state =
                choose_latest_state(predecessor_states);

        double idle_wait = 0;
        double hidden_wait = 0;
        if(is_any_t(operation.kind))
        {
            idle_wait = wait_duration;
            if(is_split_t(operation.kind))
            {
                const auto correction =
                        correction_pairs.t_to_correction.find(operation_index);
                if(correction != correction_pairs.t_to_correction.end())
                {
                    const size_t correction_slice =
                            operations[correction->second].first_slice;
                    const size_t slice_gap = correction_slice > operation.first_slice
                            ? correction_slice - operation.first_slice
                            : 0;
                    hidden_wait = std::min(
                            wait_duration,
                            static_cast<double>(slice_gap) * distance);
                    idle_wait = std::max(0.0, wait_duration - hidden_wait);
                }
            }
        }

        const double duration = operation_duration(
                operation.kind,
                input.code_distance,
                idle_wait);
        double start_time = lower_bound_start;
        std::optional<size_t> predecessor = predecessor_state.node_index;
        if(predecessor_state.finish_time > start_time)
            start_time = predecessor_state.finish_time;
        const double finish_time = start_time + duration;
        const size_t node_index = append_schedule_node(
                nodes,
                operation_index,
                predecessor,
                finish_time);
        for(QubitNum qubit : operation.qubits)
            finish_by_qubit[qubit] = {finish_time, node_index};
        idle_wait_by_operation[operation_index] = idle_wait;
        hidden_wait_by_operation[operation_index] = hidden_wait;

        if(finish_time > best_finish_time)
        {
            best_finish_time = finish_time;
            best_node_index = node_index;
        }
    }

    std::vector<size_t> path_indices =
            path_indices_from_nodes(nodes, best_node_index);
    double path_idle_wait = 0;
    double hidden_decoder_wait = 0;
    size_t path_t_count = 0;
    size_t unmatched_split_t_count = 0;
    for(size_t operation_index : path_indices)
    {
        path_idle_wait += idle_wait_by_operation[operation_index];
        hidden_decoder_wait += hidden_wait_by_operation[operation_index];
        if(is_any_t(operations[operation_index].kind))
            path_t_count++;
        if(is_split_t(operations[operation_index].kind) &&
           !correction_pairs.t_to_correction.contains(operation_index))
        {
            unmatched_split_t_count++;
        }
    }

    double total_idle_wait = 0;
    for(double idle_wait : idle_wait_by_operation)
        total_idle_wait += idle_wait;

    return EvaluationMetrics{
            .input = input,
            .method = method,
            .wait_factor = wait_factor,
            .duration = best_finish_time,
            .path_gate_count = path_indices.size(),
            .path_t_count = path_t_count,
            .path_idle_wait = path_idle_wait,
            .total_idle_wait = total_idle_wait,
            .hidden_decoder_wait = hidden_decoder_wait,
            .split_correction_count =
                    correction_pairs.correction_to_t.size(),
            .unmatched_split_t_count = unmatched_split_t_count,
            .path_operation_indices = std::move(path_indices)};
}

std::string_view trim(std::string_view value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if(first == std::string_view::npos)
        return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowercase(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for(const unsigned char character : value)
    {
        if(character >= 'A' && character <= 'Z')
            result.push_back(static_cast<char>(character - 'A' + 'a'));
        else
            result.push_back(static_cast<char>(character));
    }
    return result;
}

std::vector<std::string_view> split_option_elements(
        std::string_view raw_value,
        std::string_view option)
{
    const std::string_view value = trim(raw_value);
    if(value.empty())
        throw std::invalid_argument{
                std::string{option} + " element 1 is empty"};

    const bool opens_array = value.front() == '[';
    const bool closes_array = value.back() == ']';
    if(opens_array != closes_array)
        throw std::invalid_argument{
                std::string{option} + " has malformed brackets"};

    if(!opens_array)
        return {value};

    const std::string_view contents = value.substr(1, value.size() - 2);
    if(trim(contents).empty())
        throw std::invalid_argument{
                std::string{option} + " array must not be empty"};

    std::vector<std::string_view> elements;
    size_t element_start = 0;
    while(true)
    {
        const size_t comma = contents.find(',', element_start);
        const size_t element_end =
                comma == std::string_view::npos ? contents.size() : comma;
        const std::string_view element =
                trim(contents.substr(element_start, element_end - element_start));
        const size_t element_number = elements.size() + 1;
        if(element.empty())
            throw std::invalid_argument{
                    std::string{option} + " element " +
                    std::to_string(element_number) + " is empty"};
        if(element.find('[') != std::string_view::npos ||
           element.find(']') != std::string_view::npos)
        {
            throw std::invalid_argument{
                    std::string{option} + " element " +
                    std::to_string(element_number) + " has malformed brackets"};
        }
        elements.push_back(element);
        if(comma == std::string_view::npos)
            break;
        element_start = comma + 1;
    }
    return elements;
}

double parse_decoder_tw_element(
        std::string_view value,
        size_t element_index)
{
    double parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto conversion = std::from_chars(begin, end, parsed);
    const std::string element = std::to_string(element_index + 1);
    if(conversion.ec != std::errc{} || conversion.ptr != end)
    {
        throw std::invalid_argument{
                "--tw element " + element + " ('" + std::string{value} +
                "') is not a number"};
    }
    if(!std::isfinite(parsed))
    {
        throw std::invalid_argument{
                "--tw element " + element + " must be finite"};
    }
    if(parsed < 0)
    {
        throw std::invalid_argument{
                "--tw element " + element + " must be nonnegative"};
    }
    return parsed;
}

int parse_code_distance_element(
        std::string_view value,
        size_t element_index)
{
    int64_t parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto conversion = std::from_chars(begin, end, parsed);
    const std::string element = std::to_string(element_index + 1);
    if(conversion.ec == std::errc::result_out_of_range ||
       (conversion.ec == std::errc{} &&
        (parsed < std::numeric_limits<int>::min() ||
         parsed > std::numeric_limits<int>::max())))
    {
        throw std::invalid_argument{
                "--d element " + element + " is out of range"};
    }
    if(conversion.ec != std::errc{} || conversion.ptr != end)
    {
        throw std::invalid_argument{
                "--d element " + element + " ('" + std::string{value} +
                "') is not an integer"};
    }
    if(parsed <= 0)
    {
        throw std::invalid_argument{
                "--d element " + element + " must be positive"};
    }
    if(parsed % 2 == 0)
    {
        throw std::invalid_argument{
                "--d element " + element + " must be odd"};
    }
    return static_cast<int>(parsed);
}

}

EvaluationProgram prepare_evaluation_program(const RtlsProgram& program)
{
    if(program.operations.size() >= kNoEvaluationOperationIndex)
        throw std::overflow_error{"Evaluation operation index space exhausted"};

    EvaluationProgram result;
    result.operations.reserve(program.operations.size());
    for(const RtlsLogicalOperation& operation : program.operations)
        result.operations.push_back(describe_operation(program, operation));

    result.correction_by_t.assign(
            result.operations.size(),
            kNoEvaluationOperationIndex);
    result.t_by_correction.assign(
            result.operations.size(),
            kNoEvaluationOperationIndex);
    const CorrectionPairs pairs = pair_corrections(program);
    for(const auto& [source_t, correction] : pairs.t_to_correction)
    {
        result.correction_by_t[source_t] = static_cast<uint32_t>(correction);
        result.t_by_correction[correction] = static_cast<uint32_t>(source_t);
    }
    result.correction_pair_count = pairs.t_to_correction.size();
    return result;
}

bool parse_print_stat_value(std::string_view value)
{
    const std::string normalized = lowercase(trim(value));
    if(normalized == "true")
        return true;
    if(normalized == "false")
        return false;
    throw std::invalid_argument{
            "--print-stat requires an explicit True or False value"};
}

std::vector<EvaluationInput> parse_evaluation_inputs(
        std::string_view decoder_tw_values,
        std::string_view code_distance_values)
{
    const auto tw_elements = split_option_elements(decoder_tw_values, "--tw");
    const auto distance_elements = split_option_elements(code_distance_values, "--d");
    if(tw_elements.size() != distance_elements.size())
    {
        throw std::invalid_argument{
                "--tw and --d must contain the same number of values after "
                "scalar normalization (got " + std::to_string(tw_elements.size()) +
                " and " + std::to_string(distance_elements.size()) + ")"};
    }

    std::vector<EvaluationInput> inputs;
    inputs.reserve(tw_elements.size());
    for(size_t index = 0; index < tw_elements.size(); index++)
    {
        inputs.push_back(EvaluationInput{
                .decoder_tw = parse_decoder_tw_element(tw_elements[index], index),
                .code_distance =
                        parse_code_distance_element(distance_elements[index], index)});
    }
    return inputs;
}

std::string_view evaluation_method_name(EvaluationMethod method)
{
    switch(method)
    {
    case EvaluationMethod::Qsink:
        return "qsink";
    case EvaluationMethod::QsinkSwiper:
        return "qsink-swiper";
    }
    throw std::logic_error{"Unknown evaluation method"};
}

std::vector<EvaluationMetrics> evaluate_metrics(
        const RtlsProgram& program,
        const EvaluationSchedule& schedule,
        const std::vector<EvaluationInput>& inputs)
{
    return evaluate_metrics(
            prepare_evaluation_program(program),
            program.operation_metadata,
            schedule,
            inputs);
}

std::vector<EvaluationMetrics> evaluate_metrics(
        const EvaluationProgram& program,
        const OperationMetadataArena& operation_metadata,
        const EvaluationSchedule& schedule,
        const std::vector<EvaluationInput>& inputs)
{
    std::vector<TimedOperation> operations;
    operations.reserve(program.operations.size());
    for(const EvaluationOperation& operation : program.operations)
        operations.push_back(adapt_operation(
                operation,
                operation_metadata,
                schedule));
    const CorrectionPairs correction_pairs = pair_corrections(program);

    constexpr std::array<std::pair<EvaluationMethod, double>, 2> methods = {{
            {EvaluationMethod::Qsink, 2.0},
            {EvaluationMethod::QsinkSwiper, 1.1}}};
    std::vector<EvaluationMetrics> results;
    results.reserve(inputs.size() * methods.size());
    for(EvaluationInput input : inputs)
    {
        for(const auto& [method, wait_factor] : methods)
        {
            results.push_back(evaluate_one(
                    operations,
                    correction_pairs,
                    input,
                    method,
                    wait_factor));
        }
    }
    return results;
}

std::string render_evaluation_metrics_table(
        const std::vector<EvaluationMetrics>& metrics)
{
    std::ostringstream output;
    output
            << "Evaluation metrics (s5b timing model)\n"
            << "Tw (us) | D | Method | Wait factor | Duration (us) | "
               "Path gates | Path T | Path idle (us) | Total idle (us) | "
               "Hidden wait (us) | Split corrections | Unmatched split T\n";
    for(const EvaluationMetrics& row : metrics)
    {
        output
                << format_number(row.input.decoder_tw) << " | "
                << row.input.code_distance << " | "
                << evaluation_method_name(row.method) << " | "
                << format_number(row.wait_factor) << " | "
                << format_number(row.duration) << " | "
                << row.path_gate_count << " | "
                << row.path_t_count << " | "
                << format_number(row.path_idle_wait) << " | "
                << format_number(row.total_idle_wait) << " | "
                << format_number(row.hidden_decoder_wait) << " | "
                << row.split_correction_count << " | "
                << row.unmatched_split_t_count << '\n';
    }
    return output.str();
}

std::filesystem::path evaluation_metrics_output_path(
        const std::filesystem::path& final_rtls_path)
{
    constexpr std::string_view rtls_extension = ".rtls";
    const std::string filename = final_rtls_path.filename().string();
    if(filename.size() < rtls_extension.size() ||
       !filename.ends_with(rtls_extension))
    {
        throw std::invalid_argument{
                "Evaluation metrics destination requires a final .rtls path"};
    }

    const std::string stem =
            filename.substr(0, filename.size() - rtls_extension.size());
    if(stem.empty())
        throw std::invalid_argument{
                "Evaluation metrics destination requires a nonempty RTLS stem"};
    return final_rtls_path.parent_path() / (stem + ".stats.txt");
}

}
