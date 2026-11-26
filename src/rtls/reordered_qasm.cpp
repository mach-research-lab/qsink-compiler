#include <lsqecc/rtls/reordered_qasm.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lsqecc {

namespace {

std::string trim_copy(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if(first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(first, last - first + 1)};
}

std::string qasm_statement_text(std::string_view text)
{
    std::string statement = trim_copy(text);
    if(statement.empty())
        throw std::runtime_error{"Cannot render an empty source QASM statement"};
    if(statement.back() != ';')
        statement.push_back(';');
    return statement;
}

std::optional<bool> source_t_is_dagger(std::string_view text)
{
    const std::string statement = trim_copy(text);
    const size_t gate_end = statement.find_first_of(" \t");
    const std::string_view gate = std::string_view{statement}.substr(0, gate_end);
    if(gate == "t")
        return false;
    if(gate == "tdg")
        return true;
    return std::nullopt;
}

bool is_source_s_family(std::string_view text)
{
    const std::string statement = trim_copy(text);
    const size_t gate_end = statement.find_first_of(" \t");
    const std::string_view gate = std::string_view{statement}.substr(0, gate_end);
    return gate == "s" || gate == "sdg";
}

bool is_ignored_source_only_statement(std::string_view text)
{
    const std::string statement = trim_copy(text);
    const size_t gate_end = statement.find_first_of(" \t");
    const std::string_view gate = std::string_view{statement}.substr(0, gate_end);
    return gate == "measure" || gate == "barrier";
}

std::string replace_qasm_gate(std::string_view text, std::string_view replacement)
{
    std::string statement = qasm_statement_text(text);
    const size_t gate_end = statement.find_first_of(" \t");
    if(gate_end == std::string::npos)
        throw std::runtime_error{"Cannot replace a QASM statement without gate operands"};
    statement.replace(0, gate_end, replacement);
    return statement;
}

std::optional<std::string_view> correction_display_gate(RtlsOperationCode operation)
{
    if(operation == RtlsOperationCode::CondS)
        return "S*";
    if(operation == RtlsOperationCode::CondSdg)
        return "Sdg*";
    if(operation == RtlsOperationCode::CondH)
        return "H*";
    return std::nullopt;
}

std::string display_source_name(std::string_view source_name)
{
    std::string name =
            std::filesystem::path{std::string{source_name}}.filename().string();
    if(name.empty())
        name = "QASM input";
    std::replace(name.begin(), name.end(), '\n', ' ');
    std::replace(name.begin(), name.end(), '\r', ' ');
    return name;
}

std::string empty_slice_comment(size_t first, size_t last)
{
    if(first == last)
        return "// Slice " + std::to_string(first) +
               ": no original QASM gate starts.";
    return "// Slices " + std::to_string(first) + "-" + std::to_string(last) +
           ": no original QASM gate starts.";
}

struct ScheduledSourceStatement
{
    size_t source_statement_index = 0;
    std::optional<std::string_view> replacement_gate;
};

void write_scheduled_statement(
        std::ostream& output,
        const QasmSourceDocument& document,
        const ScheduledSourceStatement& scheduled)
{
    const std::string& source =
            document.statements.at(scheduled.source_statement_index).text;
    output << (scheduled.replacement_gate
            ? replace_qasm_gate(source, *scheduled.replacement_gate)
            : qasm_statement_text(source));
}

}

void ReorderedQasmTracker::observe_instruction(const LSInstruction& instruction)
{
    schedule_tracker_->observe_instruction(instruction);
}

void ReorderedQasmTracker::observe_slice(const DenseSlice& slice)
{
    schedule_tracker_->observe_slice(slice);
}

void ReorderedQasmTracker::render_to(
        std::ostream& output,
        std::string_view source_name,
        const QasmSourceDocument& document,
        const QasmSourceOperationMap& operation_map) const
{
    if(!operation_metadata_arena_)
        throw std::logic_error{"Reordered-QASM rendering has no metadata arena"};
    if(operation_map.operation_handles_by_statement.size() !=
       document.statements.size())
    {
        throw std::runtime_error{
                "Source QASM statement/provenance count mismatch: " +
                std::to_string(document.statements.size()) + " statements and " +
                std::to_string(
                        operation_map.operation_handles_by_statement.size()) +
                " provenance entries"};
    }

    const OperationSchedule& schedule = schedule_tracker_->schedule();
    auto semantic_operation_start = [&](OperationHandle operation_handle, size_t line_number) {
        if(operation_handle >= schedule.first_semantic_start.size())
            throw std::runtime_error{"Semantic operation handle is outside the schedule"};
        const SliceIndex start = schedule.first_semantic_start[operation_handle];
        if(start == kUnscheduledSlice || start >= schedule.slice_count)
            throw std::runtime_error{
                    "Source QASM statement at line " +
                    std::to_string(line_number) +
                    " has no valid semantic/data-application start slice"};
        return static_cast<size_t>(start);
    };
    auto operation_start = [&](OperationHandle operation_handle, size_t line_number) {
        if(operation_handle >= schedule.first_applied.size() ||
           operation_handle >= schedule.first_visible.size())
            throw std::runtime_error{"Scheduled operation handle is outside the schedule"};

        const CompactRtlsOperationMetadata& metadata =
                operation_metadata_arena_->compact_at(operation_handle);
        const SliceIndex visible = schedule.first_visible[operation_handle];
        const SliceIndex applied = schedule.first_applied[operation_handle];
        if(visible == kUnscheduledSlice && applied == kUnscheduledSlice)
        {
            throw std::runtime_error{
                    "Scheduled operation ID " +
                    serialize_compact_operation_id(metadata.operation_id) +
                    " for source QASM statement at line " +
                    std::to_string(line_number) +
                    " did not appear in any compiler slice"};
        }

        const SliceIndex start = visible != kUnscheduledSlice ? visible : applied;
        if(start >= schedule.slice_count)
        {
            throw std::runtime_error{
                    "Scheduled operation ID " +
                    serialize_compact_operation_id(metadata.operation_id) +
                    " resolves outside the completed compiler slices"};
        }
        return static_cast<size_t>(start);
    };
    std::map<size_t, std::vector<ScheduledSourceStatement>> statements_by_slice;
    std::vector<size_t> ignored_source_only_statements;
    for(size_t statement_index = 0;
        statement_index < document.statements.size();
        statement_index++)
    {
        const auto& operation_handles =
                operation_map.operation_handles_by_statement[statement_index];
        const QasmSourceStatement& statement = document.statements[statement_index];
        if(operation_handles.empty())
        {
            if(is_ignored_source_only_statement(statement.text))
            {
                ignored_source_only_statements.push_back(statement_index);
                continue;
            }
            throw std::runtime_error{
                    "Source QASM statement at line " +
                    std::to_string(statement.line_number) +
                    " has no scheduled operation IDs"};
        }
        const std::optional<bool> dagger = source_t_is_dagger(statement.text);
        if(is_source_s_family(statement.text) && operation_handles.size() == 1)
        {
            const OperationHandle handle = operation_handles.front();
            const CompactRtlsOperationMetadata& metadata =
                    operation_metadata_arena_->compact_at(handle);
            if(metadata.operation == RtlsOperationCode::S ||
               metadata.operation == RtlsOperationCode::Sdg)
            {
                const size_t start = semantic_operation_start(
                        handle,
                        statement.line_number);
                statements_by_slice[start].push_back(ScheduledSourceStatement{
                        .source_statement_index = statement_index,
                        .replacement_gate = std::nullopt});
                continue;
            }
        }
        if(dagger && operation_handles.size() == 1)
        {
            const OperationHandle body_handle = operation_handles.front();
            const CompactRtlsOperationMetadata& body_metadata =
                    operation_metadata_arena_->compact_at(body_handle);
            const RtlsOperationCode expected_body = *dagger
                    ? RtlsOperationCode::TStardg
                    : RtlsOperationCode::TStar;
            if(body_metadata.operation == expected_body &&
               body_metadata.conditional_correction_operation_id != 0)
            {
                const uint32_t correction_id =
                        body_metadata.conditional_correction_operation_id;
                const std::optional<OperationHandle> correction_handle =
                        operation_metadata_arena_->find(correction_id);
                if(!correction_handle)
                {
                    throw std::runtime_error{
                            "Scheduled correction operation ID " +
                            serialize_compact_operation_id(correction_id) +
                            " for source QASM statement at line " +
                            std::to_string(statement.line_number) +
                            " did not retain RTLS metadata"};
                }

                const CompactRtlsOperationMetadata& correction_metadata =
                        operation_metadata_arena_->compact_at(*correction_handle);
                const auto correction_gate =
                        correction_display_gate(correction_metadata.operation);
                if(!correction_gate ||
                   correction_metadata.correction_for_operation_id !=
                           body_metadata.operation_id)
                {
                    throw std::runtime_error{
                            "Scheduled T correction metadata is inconsistent for source "
                            "QASM statement at line " +
                            std::to_string(statement.line_number)};
                }

                const size_t body_start = operation_start(
                        body_handle,
                        statement.line_number);
                const bool correction_is_s =
                        correction_metadata.operation == RtlsOperationCode::CondS ||
                        correction_metadata.operation == RtlsOperationCode::CondSdg;
                const size_t correction_start = correction_is_s
                        ? semantic_operation_start(
                                  *correction_handle,
                                  statement.line_number)
                        : operation_start(
                                  *correction_handle,
                                  statement.line_number);
                if(correction_start <= body_start)
                    throw std::runtime_error{
                            "Scheduled T correction " +
                            serialize_compact_operation_id(correction_id) +
                            " must start after source " +
                            serialize_compact_operation_id(body_metadata.operation_id) +
                            " (source slice " + std::to_string(body_start) +
                            ", correction slice " +
                            std::to_string(correction_start) + ")"};

                statements_by_slice[body_start]
                        .push_back(ScheduledSourceStatement{
                                .source_statement_index = statement_index,
                                .replacement_gate = *dagger ? "Tdg*" : "T*"});
                statements_by_slice[correction_start]
                        .push_back(ScheduledSourceStatement{
                                .source_statement_index = statement_index,
                                .replacement_gate = *correction_gate});
                continue;
            }
        }

        std::optional<size_t> statement_start;
        for(OperationHandle operation_handle : operation_handles)
        {
            const size_t resolved_start =
                    operation_start(operation_handle, statement.line_number);
            statement_start = statement_start
                    ? std::min(*statement_start, resolved_start)
                    : resolved_start;
        }

        if(!statement_start)
            throw std::runtime_error{"Source QASM statement has no resolved start slice"};
        statements_by_slice[*statement_start].push_back(ScheduledSourceStatement{
                .source_statement_index = statement_index});
    }

    if(!ignored_source_only_statements.empty())
    {
        const size_t source_only_slice = schedule.slice_count == 0
                ? 0
                : static_cast<size_t>(schedule.slice_count - 1);
        for(size_t statement_index : ignored_source_only_statements)
            statements_by_slice[source_only_slice].push_back(ScheduledSourceStatement{
                    .source_statement_index = statement_index});
    }

    output << "// Reordered execution view generated from "
           << display_source_name(source_name) << ".\n"
           << "// Slice starts are taken directly from the compiler schedule.\n"
           << "//\n"
           << "// This keeps original QASM operations, but groups them by the LSQECC\n"
           << "// slice where their lattice-surgery execution starts.\n\n";

    size_t preamble_end = document.preamble.size();
    while(preamble_end > 0 && trim_copy(document.preamble[preamble_end - 1]).empty())
        preamble_end--;
    for(size_t line_index = 0; line_index < preamble_end; line_index++)
        output << document.preamble[line_index] << '\n';
    if(preamble_end > 0)
        output << '\n';

    std::optional<size_t> final_slice;
    if(schedule.slice_count > 0)
        final_slice = schedule.slice_count - 1;
    if(!statements_by_slice.empty())
    {
        const size_t last_statement_slice = statements_by_slice.rbegin()->first;
        final_slice = final_slice
                ? std::max(*final_slice, last_statement_slice)
                : last_statement_slice;
    }

    std::optional<size_t> empty_span_start;
    bool wrote_schedule_block = false;
    auto begin_schedule_block = [&]() {
        if(wrote_schedule_block)
            output << '\n';
        wrote_schedule_block = true;
    };
    if(final_slice)
    {
        for(size_t slice_index = 0; slice_index <= *final_slice; slice_index++)
        {
            const auto statements_here = statements_by_slice.find(slice_index);
            if(statements_here == statements_by_slice.end())
            {
                if(!empty_span_start)
                    empty_span_start = slice_index;
                continue;
            }

            if(empty_span_start)
            {
                begin_schedule_block();
                output << empty_slice_comment(*empty_span_start, slice_index - 1)
                       << '\n';
                empty_span_start.reset();
            }

            begin_schedule_block();
            output << "// Slice " << slice_index
                   << ": original QASM gates starting here.\n";
            bool first_statement = true;
            for(const ScheduledSourceStatement& statement : statements_here->second)
            {
                if(!first_statement)
                    output << ' ';
                write_scheduled_statement(output, document, statement);
                first_statement = false;
            }
            output << '\n';
        }

        if(empty_span_start)
        {
            begin_schedule_block();
            output << empty_slice_comment(*empty_span_start, *final_slice) << '\n';
        }
    }
}

std::string ReorderedQasmTracker::render(
        std::string_view source_name,
        const QasmSourceDocument& document,
        const QasmSourceOperationMap& operation_map) const
{
    std::ostringstream output;
    render_to(output, source_name, document, operation_map);
    return output.str();
}

size_t ReorderedQasmTracker::slice_count() const noexcept
{
    return schedule_tracker_->slice_count();
}

std::filesystem::path default_reordered_qasm_path(
        const std::filesystem::path& input_path)
{
    if(input_path.filename().empty())
        throw std::invalid_argument{
                "Cannot derive reordered-QASM path from an empty input filename"};
    return input_path.parent_path() /
           (input_path.stem().string() + "_reordered.qasm");
}

}
