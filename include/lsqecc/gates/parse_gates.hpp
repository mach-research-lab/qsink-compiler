#ifndef LSQECC_PARSE_GATES_HPP
#define LSQECC_PARSE_GATES_HPP

#include <lsqecc/gates/gates.hpp>
#include <lsqecc/rtls/metadata.hpp>

#include <string_view>
#include <string>
#include <istream>
#include <vector>
#include <optional>
#include <iterator>
#include <cstddef>
#include <utility>

namespace lsqecc {


struct GateParseException : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};


// indicate that the instruction is something like an OPENQASM or include standard gates directive
struct IgnoredInstruction{};

struct Qreg
{
    std::string name;
    QubitNum size;
};


using ParseGateResult = std::variant<gates::Gate, IgnoredInstruction, Qreg>;
ParseGateResult parse_gate(std::string_view str_line);


struct QasmSourceStatement
{
    size_t line_number;
    std::string text;
};


struct QasmSourceDocument
{
    std::vector<std::string> preamble;
    std::vector<QasmSourceStatement> statements;
};


struct QasmSourceOperationMap
{
    std::vector<std::vector<OperationHandle>> operation_handles_by_statement;

    void add_operation(size_t source_statement_index, OperationHandle operation_handle);
};


struct SourcedGate
{
    gates::Gate gate;
    std::optional<size_t> source_statement_index;
};


class GateStream
{
public:
    virtual gates::Gate get_next_gate() = 0;
    virtual SourcedGate get_next_sourced_gate()
    {
        return SourcedGate{.gate = get_next_gate(), .source_statement_index = std::nullopt};
    }
    virtual bool has_next_gate() const = 0;
    virtual const Qreg& get_qreg() const = 0;
    virtual const QasmSourceDocument* qasm_source_document() const { return nullptr; }
    virtual std::optional<QasmSourceDocument> take_qasm_source_document()
    {
        const QasmSourceDocument* document = qasm_source_document();
        return document
                ? std::optional<QasmSourceDocument>{*document}
                : std::nullopt;
    }

    virtual ~GateStream() {};
};



class GateStreamFromFile : public GateStream
{
public:
    explicit GateStreamFromFile(std::istream& gate_file, bool capture_qasm_source = false);

    gates::Gate get_next_gate() override;
    SourcedGate get_next_sourced_gate() override;
    bool has_next_gate() const override {return next_gate_.has_value();};
    const Qreg& get_qreg() const override {return qreg_.value();};
    const QasmSourceDocument* qasm_source_document() const override
    {
        return source_document_ ? &*source_document_ : nullptr;
    }
    std::optional<QasmSourceDocument> take_qasm_source_document() override
    {
        return std::exchange(source_document_, std::nullopt);
    }
private:

    std::istream& gate_file_;
    std::optional<SourcedGate> next_gate_;
    std::optional<Qreg> qreg_;
    std::optional<QasmSourceDocument> source_document_;
    bool seen_executable_gate_ = false;
    size_t line_number_ = 0;

    void advance_gate();
};


}



#endif //LSQECC_PARSE_GATES_HPP
