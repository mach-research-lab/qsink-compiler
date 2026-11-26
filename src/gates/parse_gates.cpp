#include <lsqecc/gates/parse_gates.hpp>

#include <lstk/lstk.hpp>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <iostream>

namespace lsqecc {

struct Line
{
    // The parser assumes a line is formed by the following components
    // instruction_name <space> comma_separated_args [; // %comma_separated_annotations]
    // NOTE the two spaces in the "; // %" group
    // Otherwise, what follows a semicolon is treated as a comment

    std::string_view instruction;
    std::vector<std::string_view> args;
    std::vector<std::string_view> annotations;
};


// TODO specialize for the arbitrary precision case
template<class Integer>
Integer try_parse_int(std::string_view sv)
{
    std::string s{sv};

    errno = 0;
    char* endptr;
    Integer res = std::strtol(s.c_str(), &endptr, 10);

    if (errno != 0)
        throw GateParseException(lstk::cat("Could not parse ", s, "as string, strtol gave errno: ",
                                                static_cast<int>(errno)));
    if (endptr == s.c_str())
        throw GateParseException(lstk::cat("Could not parse ", s, ". No digits were found"));
    if (*endptr != '\0')
        throw GateParseException(lstk::cat("Could not parse ", s, ". Further characters after number"));

//    if(std::to_string(res) != s)
//        throw GateParseException(std::string{"Could not parse "} + s + "as string, atol gave "+std::to_string(res));
    return res;
}

Line split_instruction_and_args(std::string_view gate_str)
{
    if(!lstk::contains(gate_str, ' '))
        return {gate_str, {}};
    
    auto semicolon_split = lstk::split_on(gate_str, ';');
    gate_str = semicolon_split.at(0);
    auto annotation_line = (semicolon_split.size() > 1) ? semicolon_split.at(1) : std::string_view();
    
    auto instruction_with_details = lstk::split_on_first(gate_str, ' ');
    auto instruction = instruction_with_details.at(0);
    auto details = instruction_with_details.at(1);
    
    auto args = lstk::split_on(details, ',');
    // trim starting spaces
    for (auto& arg : args)
    {
        while (arg.starts_with(' '))
            arg.remove_prefix(1);
    }
    
    std::vector<std::string_view> annotations;
    if (annotation_line.starts_with(" // %"))
            annotations = lstk::split_on(annotation_line.substr(5),',');

    return Line{instruction, args, annotations};
}

QubitNum get_index_arg(std::string_view s)
{
    return try_parse_int<QubitNum>(
            lstk::split_on(
                    lstk::split_on(s,'[').at(1),
                    ']'
            ).at(0));
}


std::string_view get_arg_in_brackets(std::string_view s)
{
    return lstk::split_on(
            lstk::split_on(s,'(').at(1),
            ')'
    ).at(0);
}

gates::CNOTType determine_cnot_type(const std::vector<std::string_view>& annotations)
{
    for(const auto& annotation : annotations)
    {
        auto t = gates::CNOTType_fromString(annotation);
        if(t) return * t;
    }
    return gates::ControlledGate::default_cnot_type;
}

gates::CNOTAncillaPlacement determine_cnot_ancilla_placement(const std::vector<std::string_view>& annotations)
{
    for(const auto& annotation : annotations)
    {
        auto t = gates::CNOTAncillaPlacement_fromString(annotation);
        if(t) return * t;
    }
    return gates::ControlledGate::default_ancilla_placement;
}


Fraction parse_angle(std::string_view s)
{
    bool is_negative = false;
    // check if negative
    if(s.starts_with("-")) {
        is_negative = true;
        s = s.substr(1);
    }

    if(s.starts_with("pi/"))
        return Fraction{1,try_parse_int<ArbitraryPrecisionInteger>(s.substr(3)), is_negative};

    // use split on with *pi/ as delimiter
    auto split = lstk::split_on(s,"*pi/");
    if(split.size() != 2) {
        //Is this an angle of the form 3*pi?
        // not sure why but maybe we need something like
        std::string ns = std::string(s) + "/1";

        split = lstk::split_on(ns,"*pi/");
        if(split.size() != 2)
            throw GateParseException{lstk::cat("Could not parse angle ", s, " as n*pi/m")};
    }

    ArbitraryPrecisionInteger num = try_parse_int<ArbitraryPrecisionInteger>(split.at(0));
    ArbitraryPrecisionInteger den = try_parse_int<ArbitraryPrecisionInteger>(split.at(1));
    return Fraction{num, den, is_negative};
}

gates::Reset parse_reset(const std::vector<std::string_view>& args)
{
    if(args.size() != 1)
        throw GateParseException(lstk::cat("Encountered reset with ", args.size(), "args"));
    
    if (args[0].find('[') == std::string_view::npos)
        throw GateParseException("Resetting a whole register is not implemented");
        
    auto parts = lstk::split_on(args[0], '[');
    QubitNum i = get_index_arg(args[0]);
    return { std::string{parts[0]}, i };
}

gates::Gate parse_qasm_gate(const Line& line)
{
    if(line.instruction == "x") return gates::X(get_index_arg(line.args.at(0)));
    if(line.instruction == "y") return gates::Y(get_index_arg(line.args.at(0))); 
    if(line.instruction == "z") return gates::Z(get_index_arg(line.args.at(0)));
    if(line.instruction == "s") return gates::S(get_index_arg(line.args.at(0)));
    if(line.instruction == "t") return gates::T(get_index_arg(line.args.at(0)));
    if(line.instruction == "h") return gates::H(get_index_arg(line.args.at(0)));
    if(line.instruction == "sdg") return gates::SDg(get_index_arg(line.args.at(0)));
    if(line.instruction == "tdg") return gates::TDg(get_index_arg(line.args.at(0)));

    if(line.instruction == "cx")
    {
        if(line.args.size() != 2) throw GateParseException{lstk::cat("cx gate must have 2 args")};
        return gates::CNOT(
                get_index_arg(line.args.at(1)),
                get_index_arg(line.args.at(0)),
                determine_cnot_type(line.annotations),
                determine_cnot_ancilla_placement(line.annotations));
    }

    if(line.instruction == "cz")
    {
        if(line.args.size() != 2) throw GateParseException{lstk::cat("cz gate must have 2 args")};
        return gates::CZ(
                get_index_arg(line.args.at(1)),
                get_index_arg(line.args.at(0)),
                determine_cnot_type(line.annotations),
                determine_cnot_ancilla_placement(line.annotations));
    }

    if(line.instruction.substr(0,2) == "rz")
    {
        if (get_arg_in_brackets(line.instruction) == "pi*-1")
            return gates::Z(get_index_arg(line.args.at(0)));

        Fraction fraction = parse_angle(get_arg_in_brackets(line.instruction));

        if(fraction.den == 1)
        {
            // Multiples of PI are Z with a global phase
            return gates::Z(get_index_arg(line.args.at(0)));
        }

        return gates::RZ{
            get_index_arg(line.args[0]),
            fraction
            };
    }
    if(line.instruction.substr(0,3) == "crz")
    {
        if(line.instruction.substr(3,4) == "(pi/")
        {
            return gates::CRZ(
                get_index_arg(line.args[1]),
                get_index_arg(line.args[0]),
                parse_angle(get_arg_in_brackets(line.instruction)),
                determine_cnot_type(line.annotations),
                determine_cnot_ancilla_placement(line.annotations));
        }
        else {
            throw GateParseException{lstk::cat(
                    "Can only parse pi/n for n power of 2 angles as crz args, got ",line.instruction)};
        }
    }
    
    if (line.instruction == "reset")
        return parse_reset(line.args);

    throw GateParseException{lstk::cat("Instruction not implemented ",line.instruction)};
}

bool is_ignored_instruction(std::string_view instr)
{
    if( instr == "OPENQASM" ||
        instr == "include" ||
        instr == "creg" ||
        instr == "barrier" ||
        instr == "//" )
        return true;
    return false;
}



Qreg parse_qreg(std::vector<std::string_view>& args)
{
    if(args.size() != 1)
        throw GateParseException(lstk::cat("Encountered qreg with ", args.size(), "args"));

    auto parts = lstk::split_on(args[0], '[');
    QubitNum num = get_index_arg(args[0]);
    return {
        .name=std::string{parts[0]},
        .size=num
    };
}


ParseGateResult parse_gate(std::string_view str_line)
{
    // trim
    std::string sline {str_line};
    lstk::ltrim(sline);
    lstk::rtrim(sline);
    if(sline.empty())
        return IgnoredInstruction{};
    str_line = std::string_view { sline };

    // check for comment at beginning of line
    if (str_line.starts_with("//"))
    {
        return IgnoredInstruction{};
    }

    Line line = split_instruction_and_args(str_line);

    if (!is_ignored_instruction(line.instruction))
    {
        if (line.instruction == "qreg")
            return parse_qreg(line.args);
        else
            return parse_qasm_gate(line);
    }
    return IgnoredInstruction{};
}

void QasmSourceOperationMap::add_operation(
        size_t source_statement_index,
        OperationHandle operation_handle)
{
    if(operation_handles_by_statement.size() <= source_statement_index)
        operation_handles_by_statement.resize(source_statement_index + 1);
    operation_handles_by_statement[source_statement_index].push_back(operation_handle);
}


namespace
{

std::string qasm_source_statement_text(const std::string& line)
{
    std::string statement = line;
    lstk::ltrim(statement);
    const size_t semicolon = statement.find(';');
    if(semicolon != std::string::npos)
        statement.erase(semicolon + 1);
    lstk::rtrim(statement);
    return statement;
}

bool is_source_only_qasm_statement(const std::string& line)
{
    std::string statement = line;
    lstk::ltrim(statement);
    return statement.starts_with("measure ") ||
           statement.starts_with("barrier ");
}

std::string qasm_gate_without_classical_condition(const std::string& line)
{
    std::string statement = line;
    lstk::ltrim(statement);
    if(!statement.starts_with("if"))
        return line;
    const size_t open = statement.find('(');
    const size_t close = open == std::string::npos
            ? std::string::npos
            : statement.find(')', open + 1);
    if(close == std::string::npos)
        return line;
    const size_t gate_start = statement.find_first_not_of(" \t", close + 1);
    return gate_start == std::string::npos
            ? line
            : statement.substr(gate_start);
}

}


void GateStreamFromFile::advance_gate()
{
    std::string line;
    while(std::getline(gate_file_, line))
    {
        line_number_++;

        try
        {
            if(is_source_only_qasm_statement(line))
            {
                if(source_document_)
                    source_document_->statements.push_back(QasmSourceStatement{
                            .line_number = line_number_,
                            .text = qasm_source_statement_text(line)});
                seen_executable_gate_ = true;
                continue;
            }
            ParseGateResult maybe_gate = parse_gate(
                    std::string_view{qasm_gate_without_classical_condition(line)});

            if(auto* qreg = std::get_if<Qreg>(&maybe_gate))
            {
                if(!qreg_)
                    qreg_ = *qreg;
                else
                    throw GateParseException{
                            lstk::cat("Can only handle one qreg, found second at line ", line_number_)};
            }

            if(auto* gate = std::get_if<gates::Gate>(&maybe_gate))
            {
                std::optional<size_t> source_statement_index;
                if(source_document_)
                {
                    source_statement_index = source_document_->statements.size();
                    source_document_->statements.push_back(QasmSourceStatement{
                            .line_number = line_number_,
                            .text = qasm_source_statement_text(line)});
                }
                seen_executable_gate_ = true;
                next_gate_ = SourcedGate{
                        .gate = *gate,
                        .source_statement_index = source_statement_index};
                return;
            }

            if(source_document_ && !seen_executable_gate_)
                source_document_->preamble.push_back(line);
        }
        catch (const GateParseException& e)
        {
            throw std::runtime_error{
                    lstk::cat("Encountered gate parsing exception at line ", line_number_, ":\n", e.what())};
        }
    }

    next_gate_ = std::nullopt;
}


GateStreamFromFile::GateStreamFromFile(
        std::istream& gate_file,
        bool capture_qasm_source)
        : gate_file_(gate_file),
          source_document_(capture_qasm_source
                  ? std::make_optional<QasmSourceDocument>()
                  : std::nullopt)
{
    advance_gate();

    if(!qreg_)
        throw std::runtime_error{"qreg not found. A single qreg must precede instructions"};
}

SourcedGate GateStreamFromFile::get_next_sourced_gate()
{
    SourcedGate gate = std::move(*next_gate_);
    advance_gate();
    return gate;
}

gates::Gate GateStreamFromFile::get_next_gate()
{
    return get_next_sourced_gate().gate;
}

}
