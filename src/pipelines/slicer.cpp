#include <lsqecc/pipelines/slicer.hpp>

#include <lsqecc/dag/domain_dags.hpp>
#include <lsqecc/gates/parse_gates.hpp>
#include <lsqecc/gates/decompose_rotation_stream.hpp>
#include <lsqecc/ls_instructions/ls_instruction_stream.hpp>
#include <lsqecc/ls_instructions/qsink_instruction_stream.hpp>
#include <lsqecc/ls_instructions/boundary_rotation_injection_stream.hpp>
#include <lsqecc/ls_instructions/teleported_s_gate_injection_stream.hpp>
#include <lsqecc/ls_instructions/catalytic_s_gate_injection_stream.hpp>
#include <lsqecc/layout/ascii_layout_spec.hpp>
#include <lsqecc/layout/router.hpp>
#include <lsqecc/layout/dynamic_layouts/compact_layout.hpp>
#include <lsqecc/layout/dynamic_layouts/edpc_layout.hpp>
#include <lsqecc/patches/slices_to_json.hpp>
#include <lsqecc/patches/slice.hpp>
#include <lsqecc/patches/slice_stats.hpp>
#include <lsqecc/patches/dense_patch_computation.hpp>
#include <lsqecc/patches/slice_variant.hpp>
#include <lsqecc/rtls/evaluation_metrics.hpp>
#include <lsqecc/rtls/rtls.hpp>
#include <lsqecc/rtls/reordered_qasm.hpp>
#include <lsqecc/rtls/streaming_renderer.hpp>
#include <lsqecc/resource_profile.hpp>

#include <lstk/lstk.hpp>

#include <argparse/argparse.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <string_view>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <chrono>
#include <optional>

#include <string>


#define CONSOLE_HELP_NEWLINE_ALIGN "\n                           "

namespace lsqecc
{

    namespace
    {

    bool env_enabled(const char* name)
    {
        const char* value = std::getenv(name);
        if(!value)
            return false;
        const std::string_view text{value};
        return text != "0" && text != "false" && text != "FALSE";
    }

    std::string latency_to_string(const RtlsCorrectionLatency& latency)
    {
        return std::visit(lstk::overloaded{
            [](int value) { return std::to_string(value); },
            [](const std::string& value) { return value; }
        }, latency);
    }

    const char* rtls_kind_to_string(RtlsLogicalOperation::Kind kind)
    {
        switch(kind)
        {
        case RtlsLogicalOperation::Kind::Gate: return "Gate";
        case RtlsLogicalOperation::Kind::TStar: return "TStar";
        case RtlsLogicalOperation::Kind::TStardg: return "TStardg";
        case RtlsLogicalOperation::Kind::Correction: return "Correction";
        case RtlsLogicalOperation::Kind::Mz: return "Mz";
        case RtlsLogicalOperation::Kind::Mx: return "Mx";
        }
        LSTK_UNREACHABLE;
    }

    void maybe_dump_rtls_program(const RtlsProgram& program, std::ostream& err_stream)
    {
        if(!env_enabled("LSQECC_DUMP_RTLS_PROGRAM"))
            return;

        err_stream << "[LSQECC_DUMP_RTLS_PROGRAM] qreg=" << program.qreg.name
                   << " size=" << program.qreg.size
                   << " operations=" << program.operations.size()
                   << " corrections=" << program.corrections.size()
                   << " input_t=" << program.input_t_count
                   << " input_tdg=" << program.input_tdg_count
                   << " gate_count=" << program.optimizer_stats.input_gate_count
                   << " qubit_touches=" << program.optimizer_stats.qubit_touch_count
                   << " successor_links=" << program.optimizer_stats.successor_link_count
                   << " rewrite_probes=" << program.optimizer_stats.rewrite_probe_count
                   << '\n';

        for(size_t index = 0; index < program.operations.size(); index++)
        {
            const RtlsLogicalOperation& operation = program.operations[index];
            const CompactRtlsOperationMetadata& metadata =
                    program.compact_metadata_for(operation);
            err_stream << "[LSQECC_DUMP_RTLS_PROGRAM] op[" << index << "] "
                       << "kind=" << rtls_kind_to_string(operation.kind)
                       << " operation=" << rtls_operation_code_name(metadata.operation)
                       << " id=" << serialize_compact_operation_id(metadata.operation_id)
                       << " target=" << operation.target_qubit;

            if(metadata.conditional_correction_operation_id)
                err_stream << " correction="
                           << serialize_compact_operation_id(
                                   metadata.conditional_correction_operation_id);
            if(operation.split_cnot)
                err_stream << " split=" << operation.split_cnot->split_id
                           << " control=" << operation.split_cnot->control_qubit
                           << " target=" << operation.split_cnot->target_qubit;
            err_stream << '\n';
        }

        for(size_t index = 0; index < program.corrections.size(); index++)
        {
            const RtlsCorrection& correction = program.corrections[index];
            err_stream << "[LSQECC_DUMP_RTLS_PROGRAM] correction[" << index << "] "
                       << "source=" << correction.source_operation_id
                       << " id=" << correction.correction_operation_id
                       << " operation=" << correction.correction_operation
                       << " target=" << correction.target_qubit
                       << " latency=" << latency_to_string(correction.latency)
                       << " anchor=" << correction.anchor_operation_id;
            if(correction.post_anchor_operation_id)
                err_stream << " post_anchor=" << *correction.post_anchor_operation_id;
            err_stream << '\n';
        }
    }

    std::filesystem::path make_temporary_output_path(
            const std::filesystem::path& final_path,
            std::string_view suffix)
    {
        const std::filesystem::path directory = final_path.has_parent_path()
                ? final_path.parent_path()
                : std::filesystem::current_path();
        const std::string base = final_path.filename().string();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();

        for(size_t attempt = 0; attempt < 1000; attempt++)
        {
            std::filesystem::path candidate = directory / (base + "." +
                    std::to_string(stamp) + "." + std::to_string(attempt) +
                    std::string{suffix});
            if(!std::filesystem::exists(candidate))
                return candidate;
        }

        throw std::runtime_error{"Could not create a unique output temporary path"};
    }

    class ScopedTemporaryOutputPath
    {
    public:
        ~ScopedTemporaryOutputPath()
        {
            cleanup();
        }

        void reset(std::filesystem::path path)
        {
            cleanup();
            path_ = std::move(path);
        }

        const std::filesystem::path& path() const
        {
            return path_;
        }

        void release()
        {
            path_.clear();
        }

    private:
        std::filesystem::path path_;

        void cleanup()
        {
            if(path_.empty())
                return;
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    };

    std::filesystem::path normalized_absolute_path(const std::filesystem::path& path)
    {
        return std::filesystem::absolute(path).lexically_normal();
    }

    void write_file_transactionally(
            const std::filesystem::path& final_path,
            const std::function<void(std::ostream&)>& writer,
            std::string_view temporary_suffix,
            std::string_view description)
    {
        ScopedTemporaryOutputPath temporary;
        temporary.reset(make_temporary_output_path(final_path, temporary_suffix));

        std::ofstream output{temporary.path(), std::ios::binary | std::ios::trunc};
        if(!output)
            throw std::runtime_error{
                    lstk::cat("Could not open ", description, " temporary file: ",
                              temporary.path().string())};

        writer(output);
        output.flush();
        if(!output)
            throw std::runtime_error{
                    lstk::cat("Could not write ", description, " temporary file: ",
                              temporary.path().string())};
        output.close();
        if(!output)
            throw std::runtime_error{
                    lstk::cat("Could not close ", description, " temporary file: ",
                              temporary.path().string())};

        std::error_code rename_error;
        std::filesystem::rename(temporary.path(), final_path, rename_error);
        if(rename_error)
            throw std::runtime_error{
                    lstk::cat("Could not publish ", description, " file ",
                              final_path.string(), ": ", rename_error.message())};
        temporary.release();
    }

    void write_file_transactionally(
            const std::filesystem::path& final_path,
            std::string_view contents,
            std::string_view temporary_suffix,
            std::string_view description)
    {
        write_file_transactionally(
                final_path,
                [contents](std::ostream& output) {
                    output.write(
                            contents.data(),
                            static_cast<std::streamsize>(contents.size()));
                },
                temporary_suffix,
                description);
    }

    }


    const inline double k_default_precision_log_ten_negative = 10;

    std::string file_to_string(std::string fname)
    {
        if(!std::filesystem::exists(fname))
            throw std::logic_error{std::string{"File not found:"}+fname};


        std::ifstream file(fname);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return std::string{buffer.str()};
    }

    enum class OutputFormatMode
    {
        Progress, NoProgress, Machine, Stats
    };

    enum class PrintDagMode {
        None, Input, ProcessedLli
     };

    enum class LLIPrintMode
    {
        None, BeforeSlicing, Sliced
    };

    enum class CompilationMode
    {
        Local, Nonlocal
    };

    enum class AutoLayoutMode // Layouts specified with -L
    {
        Compact, CompactNoClogging, Edpc
    };

    using LayoutMode = std::variant<AutoLayoutMode, std::unique_ptr<LayoutFromSpec>>; // LayoutFromSpec is for -l

    enum class SGateMode
    {
        Catalytic, Twists
    };

    DistillationOptions make_distillation_options(argparse::ArgumentParser& parser)
    {
        DistillationOptions distillation_options;
        if(parser.exists("disttime"))
            distillation_options.distillation_time = parser.get<size_t>("disttime");
        if(parser.exists("nostagger"))
            distillation_options.staggered = false;
        return distillation_options;

    }

    int run_slicer_program(
            int argc, const char* argv[],
            std::istream& in_stream,
            std::ostream& out_stream,
            std::ostream& err_stream)
    {
        std::string prog_name{argv[0]};
        bool output_json_requested = false;
        bool no_ths_opt_requested = false;
        size_t print_qasm_option_count = 0;
        size_t print_stat_option_count = 0;
        size_t decoder_tw_option_count = 0;
        size_t code_distance_option_count = 0;
        std::optional<std::string> print_stat_value;
        std::vector<std::string> normalized_args_storage;
        std::vector<const char*> normalized_args;
        normalized_args_storage.reserve(static_cast<size_t>(argc));
        normalized_args_storage.push_back(prog_name);
        for(int i = 1; i < argc; i++)
        {
            const std::string_view arg{argv[i]};
            if(arg == "--output-json")
            {
                output_json_requested = true;
                continue;
            }
            if(arg == "--no-ths-opt")
            {
                no_ths_opt_requested = true;
                continue;
            }
            if(arg == "--printqasm" || arg.starts_with("--printqasm="))
                print_qasm_option_count++;
            if(arg == "--print-stat")
            {
                print_stat_option_count++;
                if(i + 1 < argc && !std::string_view{argv[i + 1]}.starts_with("-"))
                    print_stat_value = argv[++i];
                else
                    print_stat_value = "";
                continue;
            }
            if(arg.starts_with("--print-stat="))
            {
                print_stat_option_count++;
                print_stat_value = std::string{arg.substr(
                        std::string_view{"--print-stat="}.size())};
                continue;
            }
            if(arg == "--tw" || arg.starts_with("--tw="))
                decoder_tw_option_count++;
            if(arg == "--d" || arg.starts_with("--d="))
                code_distance_option_count++;
            normalized_args_storage.emplace_back(argv[i]);
        }
        normalized_args.reserve(normalized_args_storage.size());
        for(const std::string& arg : normalized_args_storage)
            normalized_args.push_back(arg.c_str());

        argparse::ArgumentParser parser(prog_name, "Slice LS-Instructions");
        parser.add_argument()
                .names({"-i", "--input"})
                .description("File with input. If not provided will read LS Instructions from stdin")
                .required(false);
        parser.add_argument()
                .names({"-q", "--qasm"})
                .description("Parse input as OpenQASM. When not provided, parse input as LLI")
                .required(false);
        parser.add_argument()
                .names({"-l", "--layout"})
                .description("File name of file with layout spec, otherwise the layout is auto-generated (configure with -L)")
                .required(false);
        parser.add_argument()
                .names({"-o", "--output"})
                .description("File name of output. When not provided outputs to stdout")
                .required(false);
        parser.add_argument()
                .names({"-f", "--output-format"})
                .description("Requires -o, STDOUT output format: progress, noprogress, machine, stats")
                .required(false);
        parser.add_argument()
                .names({"-t", "--timeout"})
                .description("Set a timeout in seconds after which stop producing slices")
                .required(false);
        parser.add_argument()
                .names({"-r", "--router"})
                .description("Set a router: graph_search (default), graph_search_cached")
                .required(false);
        parser.add_argument()
                .names({"-P", "--pipeline"})
                .description("pipeline mode: stream (default), wave, edpc, dag (deprecated)")
                .required(false);
        parser.add_argument()
                .names({"-g", "--graph-search"})
                .description("Set a graph search provider: djikstra (default), astar, boost (not always available)")
                .required(false);
        parser.add_argument()
                .names({"--graceful"})
                .description("If there is an error when slicing, print the error and terminate")
                .required(false);
        parser.add_argument()
                .names({"--printlli"})
                .description("Output LLI instead of JSONs. options: before (default), sliced (prints lli on the same slice separated by semicolons)")
                .required(false);
        parser.add_argument()
                .names({"--printdag"})
                .description("Prints a dependency dag of the circuit. Modes: input (default), processedlli")
                .required(false);
        parser.add_argument()
                .names({"--printqasm"})
                .description("Write a slice-ordered QASM execution view. An optional path is exact; without one, use <input_stem>_reordered.qasm")
                .required(false);
        parser.add_argument()
                .names({"--print-stat"})
                .description("Save s5b evaluation metrics beside RTLS output: True or False")
                .required(false);
        parser.add_argument()
                .names({"--tw"})
                .description("Decoder latency in us: one scalar or bracketed array (requires --print-stat True)")
                .required(false);
        parser.add_argument()
                .names({"--d"})
                .description("Positive odd code distance: one scalar or bracketed array (requires --print-stat True)")
                .required(false);
        parser.add_argument()
                .names({"--noslices"})
                .description("Do the slicing but don't write the slices out")
                .required(false);
        parser.add_argument()
                .names({"--cnotcorrections"})
                .description("Add Xs and Zs to correct the the negative outcomes: never (default), always") // TODO add random
                .required(false);
        parser.add_argument()
                .names({"--output-json"})
                .description("For QASM input, emit the legacy JSON output instead of RTLS.")
                .required(false);
        parser.add_argument()
                .names({"--no-ths-opt"})
                .description("Disable the qSink T/TDg H S delayed-correction optimization")
                .required(false);
        parser.add_argument()
                .names({"--op-ids"})
                .description("Generates certain opearion ids. For now, it only adds ids to multi-body-measurement")
                .required(false);
        parser.add_argument()
                .names({"--layoutgenerator","-L"})
                .description(
                    "Automatically generates a layout for the given number of qubits. Incompatible with -l. Options:" CONSOLE_HELP_NEWLINE_ALIGN
                    " - compact (default): Uses Litinski's Game of Surace Code compact layout (https://arxiv.org/abs/1808.02892)" CONSOLE_HELP_NEWLINE_ALIGN
                    " - compact_no_clogging: same as compact, but fewer cells for ancillas and magic state queues" CONSOLE_HELP_NEWLINE_ALIGN
                    " - edpc: Uses a family of layouts based upon the one specified in the EDPC paper by Beverland et. al. (https://arxiv.org/abs/2110.11493)"
                )
                .required(false);
        parser.add_argument()
                .names({"--numlanes"})
                .description("Only compatible with -L edpc. Configures number of free lanes for routing.")
                .required(false);
        parser.add_argument()
                    .names({"--condensed"})
                    .description("Only compatible with -L edpc. Packs logical qubits more compactly.")
                    .required(false);
        parser.add_argument()
                    .names({"--explicitfactories"})
                    .description("Only compatible with -L edpc. Explicitly specifies factories (otherwise, uses tiles reserved for magic state re-spawn).")
                    .required(false);
             
        #ifdef USE_GRIDSYNTH
        parser.add_argument()
                .names({"--rzprecision"})
                .description("Float to define the precision when Gridsynth is compiled. The precision is given by the " CONSOLE_HELP_NEWLINE_ALIGN
                             "negative power of ten of this value (I.e. precision=10^(-rzprecision)). Defaults to 10.")
                .required(false);
        #endif // USE_GRIDSYNTH
        parser.add_argument()
                .names({"--nostagger"})
                .description("Turns off staggered distillation block timing")
                .required(false);
        parser.add_argument()
                .names({"--disttime"})
                .description("Set the distillation time (default 10)")
                .required(false);
        parser.add_argument()
                .names({"--local"})
                .description("Compile gates into a pair-wise local lattice surgery instruction set")
                .required(false);
        parser.add_argument()
                .names({"--notwists"})
                .description("Compile S gates using the catalytic teleportation circuit from Fowler, 2012 instead of using the twist-based Y state initialization and teleportation from Gidney, 2024")
                .required(false);
        parser.enable_help();

        auto err = parser.parse(static_cast<int>(normalized_args.size()), normalized_args.data());
        if (err)
        {
            err_stream << err << std::endl;
            parser.print_help();
            return -1;
        }
        if (parser.exists("help"))
        {
            parser.print_help();
            return 0;
        }

        const bool emit_reordered_qasm = parser.exists("printqasm");
        const bool enable_ths_optimization = !no_ths_opt_requested;
        if(print_stat_option_count > 1)
        {
            err_stream << "--print-stat may be specified only once" << std::endl;
            return -1;
        }
        if(decoder_tw_option_count > 1)
        {
            err_stream << "--tw may be specified only once" << std::endl;
            return -1;
        }
        if(code_distance_option_count > 1)
        {
            err_stream << "--d may be specified only once" << std::endl;
            return -1;
        }

        bool evaluate_statistics = false;
        if(print_stat_option_count > 0)
        {
            try
            {
                evaluate_statistics =
                        parse_print_stat_value(print_stat_value.value_or(""));
            }
            catch(const std::invalid_argument& error)
            {
                err_stream << error.what() << std::endl;
                return -1;
            }
        }

        std::vector<EvaluationInput> evaluation_inputs;
        if(evaluate_statistics)
        {
            if(!parser.exists("tw"))
            {
                err_stream << "--print-stat True requires --tw" << std::endl;
                return -1;
            }
            if(!parser.exists("d"))
            {
                err_stream << "--print-stat True requires --d" << std::endl;
                return -1;
            }
            try
            {
                evaluation_inputs = parse_evaluation_inputs(
                        parser.get<std::string>("tw"),
                        parser.get<std::string>("d"));
            }
            catch(const std::invalid_argument& error)
            {
                err_stream << error.what() << std::endl;
                return -1;
            }

            if(!parser.exists("q"))
            {
                err_stream << "--print-stat True requires OpenQASM input selected with -q/--qasm" << std::endl;
                return -1;
            }
            if(output_json_requested)
            {
                err_stream << "--print-stat True is incompatible with --output-json" << std::endl;
                return -1;
            }
            if(parser.exists("printdag"))
            {
                err_stream << "--print-stat True is incompatible with --printdag" << std::endl;
                return -1;
            }
            if(parser.exists("printlli"))
            {
                err_stream << "--print-stat True is incompatible with --printlli" << std::endl;
                return -1;
            }
            if(!parser.exists("o"))
            {
                err_stream << "--print-stat True requires -o/--output" << std::endl;
                return -1;
            }
        }
        else if(parser.exists("tw") || parser.exists("d"))
        {
            err_stream << "--tw and --d may be used only with --print-stat True" << std::endl;
            return -1;
        }

        if(print_qasm_option_count > 1)
        {
            err_stream << "--printqasm may be specified only once" << std::endl;
            return -1;
        }
        if(emit_reordered_qasm && !parser.exists("q"))
        {
            err_stream << "--printqasm requires OpenQASM input selected with -q/--qasm" << std::endl;
            return -1;
        }
        if(emit_reordered_qasm && parser.exists("printdag"))
        {
            err_stream << "--printqasm is incompatible with --printdag because that mode exits before scheduling" << std::endl;
            return -1;
        }
        if(emit_reordered_qasm && parser.exists("printlli"))
        {
            const std::string print_lli_mode = parser.get<std::string>("printlli");
            if(print_lli_mode.empty() || print_lli_mode == "before")
            {
                err_stream << "--printqasm is incompatible with --printlli before because that mode exits before scheduling" << std::endl;
                return -1;
            }
        }

        // Compilation semantics and requested outputs are deliberately
        // independent. In particular, --noslices suppresses RTLS emission but
        // must not select the legacy, unoptimized instruction stream.
        const bool use_qsink_optimizations =
                parser.exists("q") &&
                !output_json_requested &&
                !parser.exists("printlli") &&
                !parser.exists("printdag");
        const bool emit_rtls =
                use_qsink_optimizations && !parser.exists("noslices");

        std::optional<std::filesystem::path> reordered_qasm_output_path;
        std::string reordered_qasm_source_name = "<stdin>";
        if(emit_reordered_qasm)
        {
            const std::string requested_path = parser.get<std::string>("printqasm");
            if(requested_path.empty())
            {
                if(!parser.exists("i"))
                {
                    err_stream << "--printqasm without a path requires -i/--input to derive <input_stem>_reordered.qasm" << std::endl;
                    return -1;
                }
                reordered_qasm_output_path =
                        default_reordered_qasm_path(parser.get<std::string>("i"));
            }
            else
                reordered_qasm_output_path = requested_path;

            if(parser.exists("i"))
            {
                const std::filesystem::path input_path{parser.get<std::string>("i")};
                reordered_qasm_source_name = input_path.filename().string();
                if(normalized_absolute_path(*reordered_qasm_output_path) ==
                   normalized_absolute_path(input_path))
                {
                    err_stream << "--printqasm destination must not overwrite the input QASM file" << std::endl;
                    return -1;
                }
            }

            if(parser.exists("o") && !parser.exists("noslices"))
            {
                const std::filesystem::path normal_output_path = emit_rtls
                        ? std::filesystem::path{rtls_output_path(parser.get<std::string>("o"))}
                        : std::filesystem::path{parser.get<std::string>("o")};
                if(normalized_absolute_path(*reordered_qasm_output_path) ==
                   normalized_absolute_path(normal_output_path))
                {
                    err_stream << "--printqasm destination must differ from the normal compiler output" << std::endl;
                    return -1;
                }
            }
        }

        std::optional<std::filesystem::path> evaluation_metrics_final_output_path;
        if(evaluate_statistics)
        {
            const std::filesystem::path normal_rtls_output_path{
                    rtls_output_path(parser.get<std::string>("o"))};
            evaluation_metrics_final_output_path =
                    evaluation_metrics_output_path(normal_rtls_output_path);
            const auto normalized_metrics_path =
                    normalized_absolute_path(*evaluation_metrics_final_output_path);

            if(parser.exists("i") &&
               normalized_metrics_path ==
               normalized_absolute_path(parser.get<std::string>("i")))
            {
                err_stream << "Evaluation statistics destination must not overwrite the input QASM file" << std::endl;
                return -1;
            }
            if(normalized_metrics_path ==
               normalized_absolute_path(normal_rtls_output_path))
            {
                err_stream << "Evaluation statistics destination must differ from the normal RTLS output" << std::endl;
                return -1;
            }
            if(reordered_qasm_output_path &&
               normalized_metrics_path ==
               normalized_absolute_path(*reordered_qasm_output_path))
            {
                err_stream << "Evaluation statistics destination must differ from the --printqasm destination" << std::endl;
                return -1;
            }
        }

        DistillationOptions distillation_options = make_distillation_options(parser);

        LayoutMode layout_mode = AutoLayoutMode::Compact;
        if (parser.exists("layoutgenerator"))
        {
            if (parser.get<std::string>("layoutgenerator") == "compact")
            {
                layout_mode = AutoLayoutMode::Compact;
            } else if (parser.get<std::string>("layoutgenerator") == "compact_no_clogging")
            {
                layout_mode = AutoLayoutMode::CompactNoClogging;
            }
            else if (parser.get<std::string>("layoutgenerator") == "edpc") 
            {
                layout_mode = AutoLayoutMode::Edpc;
            }
            else
            {
                err_stream << "Unknown layout generator: " << parser.get<std::string>("layoutgenerator") << std::endl;
                return -1;
            }
        }
        else if(parser.exists("l"))
            layout_mode = std::make_unique<LayoutFromSpec>(file_to_string(parser.get<std::string>("l")), distillation_options);
        else {
            // Default to Litinsiki's compact layout
        }

        gates::ControlledGate::default_ancilla_placement =
                gates::CNOTAncillaPlacement::USE_DEDICATED_CELL;
        if(const AutoLayoutMode* auto_layout_mode = std::get_if<AutoLayoutMode>(&layout_mode);
           auto_layout_mode && *auto_layout_mode == AutoLayoutMode::Edpc)
        {
            // RTLS drains QASM before layout construction; mirror the EDPC
            // no-dedicated-ancilla default that the legacy lazy path sees.
            gates::ControlledGate::default_ancilla_placement =
                    gates::CNOTAncillaPlacement::ANCILLA_NEXT_TO_TARGET;
        }

        std::reference_wrapper<std::ostream> bulk_output_stream = std::ref(out_stream);
        std::optional<std::filesystem::path> rtls_final_output_path;
        ScopedTemporaryOutputPath rtls_temp_output_guard;
        std::unique_ptr<std::ostream> _ofstream_store;
        if(parser.exists("o") && !parser.exists("noslices"))
        {
            const std::string output_path = emit_rtls
                    ? rtls_output_path(parser.get<std::string>("o"))
                    : parser.get<std::string>("o");
            if(emit_rtls)
            {
                rtls_final_output_path = output_path;
                rtls_temp_output_guard.reset(
                        make_temporary_output_path(*rtls_final_output_path, ".rtls-tmp"));
                _ofstream_store = std::make_unique<std::ofstream>(rtls_temp_output_guard.path());
            }
            else
                _ofstream_store = std::make_unique<std::ofstream>(output_path);
            if(!_ofstream_store->good())
            {
                err_stream << "Could not open output file: " << output_path << std::endl;
                return -1;
            }
            bulk_output_stream = std::ref(*_ofstream_store);
        }

        auto output_format_mode = (parser.exists("o") || parser.exists("noslices")) ? OutputFormatMode::Progress : OutputFormatMode::NoProgress;
        if(parser.exists("f"))
        {
            auto mode_arg = parser.get<std::string>("f");
            if (mode_arg=="progress")
                output_format_mode = OutputFormatMode::Progress;
            else if (mode_arg=="noprogres")
                output_format_mode = OutputFormatMode::NoProgress;
            else if (mode_arg=="machine")
                output_format_mode = OutputFormatMode::Machine;
            else if (mode_arg=="stats")
                output_format_mode = OutputFormatMode::Stats;
            else
            {
                err_stream << "Unknown output format mode " << mode_arg << std::endl;
                return -1;
            }

            if(!parser.exists("o") && !parser.exists("noslices"))
            {
                err_stream << "-f requires -o or --noslices" << std::endl;
                return -1;
            }
        }

        PrintDagMode print_dag_mode = PrintDagMode::None;
        if (parser.exists("printdag"))
        {
            auto mode_arg = parser.get<std::string>("printdag");
            if (mode_arg=="input" || mode_arg=="")
                print_dag_mode = PrintDagMode::Input;
            else if (mode_arg=="processedlli")
                print_dag_mode = PrintDagMode::ProcessedLli;
            else
            {
                err_stream << "Unknown print dag mode " << mode_arg << std::endl;
                return -1;
            }
        }
        

        PipelineMode pipeline_mode = PipelineMode::Stream;
        bool pipeline_mode_validity_checker = 1;
        if (parser.exists("pipeline"))
        {
            auto mode_arg = parser.get<std::string>("pipeline");
            if (mode_arg=="stream" || mode_arg=="")
                pipeline_mode = PipelineMode::Stream;
            else if (mode_arg=="dag")
                pipeline_mode = PipelineMode::Dag;
            else if (mode_arg=="wave")
                pipeline_mode = PipelineMode::Wave;
            else if (mode_arg=="edpc")
            {
                pipeline_mode = PipelineMode::EDPC;
                pipeline_mode_validity_checker = 0;
            }
            else
            {
                err_stream << "Unknown pipeline mode " << mode_arg << std::endl;
                return -1;
            }
        }

        if(use_qsink_optimizations &&
           pipeline_mode != PipelineMode::Stream &&
           pipeline_mode != PipelineMode::Wave)
        {
            err_stream << "RTLS output currently supports only --pipeline stream or --pipeline wave" << std::endl;
            return -1;
        }

        std::optional<ResourceProfile> resource_profile_storage;
        std::optional<std::filesystem::path> resource_profile_output_path;
        if(const char* value = std::getenv("LSQECC_RESOURCE_PROFILE");
           value && std::string_view{value} != "0" &&
           std::string_view{value} != "false" &&
           std::string_view{value} != "FALSE")
        {
            resource_profile_storage.emplace();
            ResourceProfile& profile = *resource_profile_storage;
            profile.mode = enable_ths_optimization ? "normal" : "no_ths";
            switch(pipeline_mode)
            {
            case PipelineMode::Stream: profile.pipeline = "stream"; break;
            case PipelineMode::Dag: profile.pipeline = "dag"; break;
            case PipelineMode::Wave: profile.pipeline = "wave"; break;
            case PipelineMode::EDPC: profile.pipeline = "edpc"; break;
            }

            const std::string_view destination{value};
            if(destination != "1" && destination != "true" &&
               destination != "TRUE")
                resource_profile_output_path = value;
        }
        ResourceProfile* resource_profile = resource_profile_storage
                ? &*resource_profile_storage
                : nullptr;


        std::reference_wrapper<std::istream> input_file_stream = std::ref(in_stream);
        std::unique_ptr<std::ifstream> _file_to_read_store;
        if(parser.exists("i"))
        {
            _file_to_read_store = std::make_unique<std::ifstream>(parser.get<std::string>("i"));
            if(!std::filesystem::exists(parser.get<std::string>("i")) || _file_to_read_store->fail()){
                err_stream << "Could not open instruction file: " << parser.get<std::string>("i") <<std::endl;
                return -1;
            }
            input_file_stream = std::ref(*_file_to_read_store);
        }

        IdGenerator id_generator;
        std::unique_ptr<LSInstructionStream> instruction_stream;
        std::unique_ptr<GateStream> gate_stream;
        std::optional<QsinkOptimizedProgram> qsink_program;
        std::optional<EvaluationProgram> evaluation_program;
        std::optional<QasmSourceOperationMap> legacy_qasm_source_operations;
        std::optional<OperationMetadataArena> legacy_operation_metadata;
        
        CompilationMode compile_mode = CompilationMode::Nonlocal;
        if (parser.exists("local"))
            compile_mode = CompilationMode::Local;

        SGateMode sgate_mode = SGateMode::Twists;
        if (parser.exists("notwists"))
            sgate_mode = SGateMode::Catalytic;

        if(!parser.exists("q"))
        {
            instruction_stream = std::make_unique<LSInstructionStreamFromFile>(input_file_stream.get());
            id_generator.set_start(*std::max(instruction_stream->core_qubits().begin(),
                                             instruction_stream->core_qubits().end()));
            if( print_dag_mode == PrintDagMode::Input )
            {
                auto dag = dag::full_dependency_dag_from_instruction_stream(*instruction_stream);
                dag.to_graphviz(out_stream);
                return 0;
            }
        }
        if(parser.exists("q"))
        {
            gate_stream = std::make_unique<GateStreamFromFile>(
                    input_file_stream.get(),
                    emit_reordered_qasm);

            if (print_dag_mode == PrintDagMode::Input)
            {
                auto dag = dag::full_dependency_dag_from_gate_stream(*gate_stream);
                dag.to_graphviz(out_stream);
                return 0;
            }

            gate_stream = std::make_unique<DecomposeRotationStream>(
                std::move(gate_stream),
                parser.exists("rzprecision") ? parser.get<double>("rzprecision") : k_default_precision_log_ten_negative
            );

            CNOTCorrectionMode cnot_correction_mode = CNOTCorrectionMode::NEVER;
            if(parser.exists("cnotcorrections"))
            {
                if(parser.get<std::string>("cnotcorrections") == "always")
                    cnot_correction_mode = CNOTCorrectionMode::ALWAYS;
                else if(parser.get<std::string>("cnotcorrections") == "never")
                    cnot_correction_mode = CNOTCorrectionMode::NEVER;
                else
                {
                    err_stream << "Unknown CNOT correction mode: " << parser.get<std::string>("cnotcorrections") <<std::endl;
                    return -1;
                }
            }

            id_generator.set_start(gate_stream->get_qreg().size);
            if(use_qsink_optimizations)
            {
                qsink_program = optimize_qsink_program(
                        *gate_stream,
                        true,
                        enable_ths_optimization,
                        resource_profile,
                        emit_rtls || evaluate_statistics);
                maybe_dump_rtls_program(*qsink_program, err_stream);
                if(evaluate_statistics)
                    evaluation_program = prepare_evaluation_program(*qsink_program);
                instruction_stream = std::make_unique<RtlsInstructionStream>(
                        qsink_program->take_operations(),
                        *qsink_program,
                        cnot_correction_mode,
                        id_generator,
                        compile_mode == CompilationMode::Local);
                if(!emit_rtls)
                    std::vector<RtlsCorrection>{}.swap(qsink_program->corrections);
                gate_stream.reset();
            }
            else
            {
                legacy_operation_metadata.emplace();
                if(emit_reordered_qasm)
                    legacy_qasm_source_operations.emplace();
                instruction_stream = std::make_unique<QSinkInstructionStreamFromGateStream>(
                        *gate_stream,
                        cnot_correction_mode,
                        id_generator,
                        compile_mode == CompilationMode::Local,
                        legacy_qasm_source_operations
                                ? &*legacy_qasm_source_operations
                                : nullptr,
                        &*legacy_operation_metadata);
            }
        }


        std::unique_ptr<Layout> layout;
        if (AutoLayoutMode* auto_layout_mode = std::get_if<AutoLayoutMode>(&layout_mode))
        {
            if (*auto_layout_mode == AutoLayoutMode::Compact)
            {
                if (sgate_mode == SGateMode::Catalytic) 
                {
                    err_stream << "Catalytic S Gates incompatible with layout: " << parser.get<std::string>("layoutgenerator") <<std::endl;
                    return -1;
                }
                layout = make_compact_layout(instruction_stream->core_qubits().size(), distillation_options);
                instruction_stream = std::make_unique<BoundaryRotationInjectionStream>(std::move(instruction_stream), *layout);
                instruction_stream = std::make_unique<TeleportedSGateInjectionStream>(std::move(instruction_stream), id_generator, false);
            } else if (*auto_layout_mode == AutoLayoutMode::CompactNoClogging)
            {
                if (sgate_mode == SGateMode::Catalytic) 
                {
                    err_stream << "Catalytic S Gates incompatible with layout: " << parser.get<std::string>("layoutgenerator") <<std::endl;
                    return -1;
                }
                layout = make_compact_layout(instruction_stream->core_qubits().size(), distillation_options, true);
                instruction_stream = std::make_unique<BoundaryRotationInjectionStream>(std::move(instruction_stream), *layout);
                instruction_stream = std::make_unique<TeleportedSGateInjectionStream>(std::move(instruction_stream), id_generator, false);
            }
            else if (*auto_layout_mode == AutoLayoutMode::Edpc) 
            {
                size_t num_lanes = 1;
                bool condensed = false;
                bool factories_explicit = false;
                
                if(parser.exists("numlanes"))
                    num_lanes = parser.get<size_t>("numlanes");
                
                if(parser.exists("condensed"))
                    condensed = parser.get<bool>("condensed");
                
                if ((pipeline_mode == PipelineMode::EDPC) &&
                    (compile_mode == CompilationMode::Local) &&
                    (num_lanes == 1))
                {
                    pipeline_mode_validity_checker = 1;
                }

                if (parser.exists("explicitfactories")) 
                    factories_explicit = true;

                if (sgate_mode == SGateMode::Catalytic) 
                {
                    layout = make_edpc_layout(instruction_stream->core_qubits().size(), num_lanes, condensed, factories_explicit, true, distillation_options);
                    instruction_stream = std::make_unique<CatalyticSGateInjectionStream>(std::move(instruction_stream), id_generator, compile_mode == CompilationMode::Local, true);
                }

                else if (sgate_mode == SGateMode::Twists) 
                {
                    layout = make_edpc_layout(instruction_stream->core_qubits().size(), num_lanes, condensed, factories_explicit, false, distillation_options);
                    instruction_stream = std::make_unique<TeleportedSGateInjectionStream>(std::move(instruction_stream), id_generator, true);
                }
            }
            else
            {
                LSTK_NOT_IMPLEMENTED;
            }
        }
        else if(std::unique_ptr<LayoutFromSpec>* custom_layout_path = std::get_if<std::unique_ptr<LayoutFromSpec>>(&layout_mode))
        {
            layout = std::move(*custom_layout_path);
            if ((sgate_mode == SGateMode::Catalytic) && (layout->predistilled_y_states().size() == 0))
            {
                err_stream << "Catalytic S Gates require pre-distilled Y states to be specified by 'Y' in layout ASCII." << std::endl;
                return -1;
            }
        }

        // Override the choice of ancilla placements when no location is provided
        if(layout->ancilla_location().empty())
        {
            gates::ControlledGate::default_ancilla_placement = gates::CNOTAncillaPlacement::ANCILLA_NEXT_TO_TARGET;
        }

        if (!pipeline_mode_validity_checker)
            throw std::runtime_error("PipelineMode::EDPC only valid when paired with (default or 1-lane condensed) EDPC layout and CompilationMode::Local.");

        LLIPrintMode lli_print_mode = LLIPrintMode::None;
        if(parser.exists("printlli"))
        {
            auto mode_arg = parser.get<std::string>("printlli");
            if (mode_arg=="before" || mode_arg=="")
                lli_print_mode = LLIPrintMode::BeforeSlicing;
            else if (mode_arg=="sliced")
                lli_print_mode = LLIPrintMode::Sliced;
            else
            {
                err_stream << "Unknown lli print mode " << mode_arg << std::endl;
                return -1;
            }
        }

        if(lli_print_mode == LLIPrintMode::BeforeSlicing)
        {
            print_all_ls_instructions_to_string(out_stream, std::move(instruction_stream));
            return 0;
        } else if (print_dag_mode == PrintDagMode::ProcessedLli)
        {
            auto dag = dag::full_dependency_dag_from_instruction_stream(*instruction_stream);
            dag.to_graphviz(out_stream);
            return 0;
        }

        auto timeout = parser.exists("t") ?
                       std::make_optional(std::chrono::seconds{parser.get<uint32_t>("t")})
                                          : std::nullopt;


        std::unique_ptr<Router> router = std::make_unique<CustomDPRouter>();
        if(parser.exists("r"))
        {
            auto router_name = parser.get<std::string>("r");
            if(router_name =="graph_search") //TODO change to djikstra
                LSTK_NOOP;// Already set
            else if(router_name=="graph_search_cached")
                router = std::make_unique<CachedRouter>();
            else
            {
                err_stream <<"Unknown router: "<< router_name << std::endl;
                return -1;
            }
        }

        router->set_graph_search_provider(GraphSearchProvider::Djikstra);
        if(parser.exists("g"))
        {
            auto router_name = parser.get<std::string>("g");
            if(router_name =="astar")
                router->set_graph_search_provider(GraphSearchProvider::AStar);
            else if (router_name=="djikstra")
                router->set_graph_search_provider(GraphSearchProvider::Djikstra);
            else if(router_name=="boost")
                router->set_graph_search_provider(GraphSearchProvider::Boost);
            else
            {
                err_stream<<"Unknown router: "<< router_name <<std::endl;
                return -1;
            }
        }


        bool print_slices = !parser.exists("noslices") && lli_print_mode == LLIPrintMode::None;
        const OperationMetadataArena* operation_metadata_arena =
                instruction_stream->operation_metadata_arena();
        std::unique_ptr<OperationScheduleTracker> operation_schedule_tracker;
        if(emit_reordered_qasm || evaluate_statistics)
            operation_schedule_tracker = std::make_unique<OperationScheduleTracker>(
                    operation_metadata_arena,
                    resource_profile);
        std::unique_ptr<ReorderedQasmTracker> reordered_qasm_tracker;
        if(emit_reordered_qasm)
            reordered_qasm_tracker = std::make_unique<ReorderedQasmTracker>(
                    operation_metadata_arena,
                    instruction_stream->core_qubits().size(),
                    operation_schedule_tracker.get(),
                    resource_profile);
        DenseSliceVisitor slice_visitor = [](const DenseSlice& s) -> void {LSTK_UNUSED(s);};
        bool is_first_slice = true;
        std::unique_ptr<RtlsStreamingRenderer> rtls_streaming_renderer;
        if(print_slices && emit_rtls)
        {
            if(!qsink_program)
                throw std::logic_error{"RTLS output selected without an RTLS program"};
            rtls_streaming_renderer = std::make_unique<RtlsStreamingRenderer>(
                    bulk_output_stream.get(),
                    std::move(qsink_program->corrections),
                    RtlsValidationContext{
                            .input_t_count = qsink_program->input_t_count,
                            .input_tdg_count = qsink_program->input_tdg_count},
                    rtls_final_output_path);
            slice_visitor = rtls_streaming_renderer->slice_visitor();
        }
        else if(print_slices)
        {
            const bool include_rtls_metadata = !emit_reordered_qasm || emit_rtls;
            slice_visitor = [
                    &bulk_output_stream,
                    &is_first_slice,
                    include_rtls_metadata](const DenseSlice & s){
                if(is_first_slice)
                {
                    bulk_output_stream.get() << "[\n"
                                             << slice_to_json(s, include_rtls_metadata).dump(3);
                    is_first_slice = false;
                }
                else
                    bulk_output_stream.get() << ",\n"
                                             << slice_to_json(s, include_rtls_metadata).dump(3)
                                             << std::flush;
            };
        }

        size_t slice_counter = 0;

        auto gave_update_at = lstk::now();
        if(output_format_mode == OutputFormatMode::Progress)
        {
            slice_visitor = [&, slice_visitor](const DenseSlice & s)
            {
                slice_visitor(s);
                slice_counter++;
                if(lstk::seconds_since(gave_update_at)>=1)
                {
                    out_stream << "Slice count: " << slice_counter << "\r" << std::flush;
                    gave_update_at = std::chrono::steady_clock::now();
                }
            };
        }


        SliceStats slice_stats;
        if (output_format_mode == OutputFormatMode::Stats)
        {
            slice_visitor = [&, slice_visitor](const DenseSlice & s)
            {
                slice_visitor(s);
                slice_stats.totals += compute_volume_counts(s);
            };
        }

        if(reordered_qasm_tracker || operation_schedule_tracker)
        {
            slice_visitor = [&, slice_visitor](const DenseSlice& slice)
            {
                if(operation_schedule_tracker)
                    operation_schedule_tracker->finish_slice();
                slice_visitor(slice);
            };
        }

        LSInstructionVisitor instruction_visitor{[&](const LSInstruction& i){}};
        if (lli_print_mode == LLIPrintMode::Sliced)
        {
            instruction_visitor = [&](const LSInstruction& i)
            {
                bulk_output_stream.get() << i << ";";
            }; 

            slice_visitor = [&,slice_visitor](const DenseSlice & s)
            {
                slice_visitor(s);
                bulk_output_stream.get() << std::endl;
            };
        }

        if(operation_schedule_tracker)
        {
            instruction_visitor = [&, instruction_visitor](const LSInstruction& instruction)
            {
                operation_schedule_tracker->observe_instruction(instruction);
                instruction_visitor(instruction);
            };
        }

        OperationVisibilityVisitor operation_visibility_visitor;
        if(operation_schedule_tracker)
        {
            operation_visibility_visitor = [tracker =
                    operation_schedule_tracker.get()](
                    OperationHandle operation_handle,
                    PatchActivity activity)
            {
                tracker->observe_visible(
                        operation_handle,
                        activity);
            };
        }


        auto start = lstk::now();

        std::unique_ptr<PatchComputationResult> computation_result = 
            std::make_unique<DensePatchComputationResult>(run_through_dense_slices(
                    std::move(instruction_stream),
                    pipeline_mode,
                    compile_mode == CompilationMode::Local,
                    sgate_mode == SGateMode::Twists,
                    *layout,
                    std::move(router),
                    timeout,
                    slice_visitor,
                    instruction_visitor,
                    parser.exists("graceful"),
                    parser.exists("op-ids"),
                    std::move(operation_visibility_visitor),
                    resource_profile
        ));

        if(resource_profile)
            resource_profile->slices = computation_result->slice_count();

        std::optional<std::string> rendered_evaluation_metrics;
        if(evaluate_statistics)
        {
            if(!qsink_program || !evaluation_program ||
               !operation_schedule_tracker)
                throw std::logic_error{
                        "Evaluation metrics requested without RTLS schedule state"};
            rendered_evaluation_metrics = render_evaluation_metrics_table(
                    evaluate_metrics(
                            *evaluation_program,
                            qsink_program->operation_metadata,
                            operation_schedule_tracker->schedule(),
                            evaluation_inputs));
        }

        if(parser.exists("o") || parser.exists("noslices"))
        {
            if (output_format_mode == OutputFormatMode::Machine)
            {
                out_stream << computation_result->ls_instructions_count() << ","
                           << computation_result->slice_count() << ","
                           << lstk::seconds_since(start) << std::endl;
            } else if (output_format_mode == OutputFormatMode::Progress || output_format_mode == OutputFormatMode::Stats )
            {
                out_stream << "LS Instructions read  " << computation_result->ls_instructions_count() << std::endl;
                out_stream << "Slices " << computation_result->slice_count() << std::endl;
                out_stream << "Made patch computation. Took " << lstk::seconds_since(start) << "s." << std::endl;
            }
            
            if ( output_format_mode == OutputFormatMode::Stats)
                out_stream << slice_stats << std::endl; 
                
        }
        
        if(print_slices && emit_rtls)
        {
            if(!rtls_streaming_renderer)
                throw std::logic_error{"RTLS output selected without a streaming renderer"};
            rtls_streaming_renderer->finish();
            bulk_output_stream.get().flush();

            if(rtls_final_output_path)
            {
                if(_ofstream_store)
                {
                    _ofstream_store->flush();
                    if(!*_ofstream_store)
                        throw std::runtime_error{"Could not finish writing RTLS output"};
                    auto* file_stream = dynamic_cast<std::ofstream*>(_ofstream_store.get());
                    if(file_stream)
                    {
                        file_stream->close();
                        if(!*file_stream)
                            throw std::runtime_error{"Could not close RTLS output"};
                    }
                }

                std::filesystem::rename(rtls_temp_output_guard.path(), *rtls_final_output_path);
                rtls_temp_output_guard.release();
            }
        }
        else if(print_slices)
            bulk_output_stream.get() << "]" <<std::endl;
        if (lli_print_mode == LLIPrintMode::Sliced)
            bulk_output_stream.get() << std::endl;

        if(!(print_slices && emit_rtls && rtls_final_output_path))
        {
            bulk_output_stream.get().flush();
            if(!bulk_output_stream.get())
                throw std::runtime_error{"Could not finish writing normal compiler output"};
        }

        if(reordered_qasm_tracker)
        {
            if(!reordered_qasm_output_path)
                throw std::logic_error{"Reordered QASM requested without an output path"};

            const QasmSourceDocument* source_document = nullptr;
            const QasmSourceOperationMap* source_operations = nullptr;
            if(qsink_program)
            {
                if(qsink_program->qasm_source_document)
                    source_document = &*qsink_program->qasm_source_document;
                source_operations = &qsink_program->source_operation_map;
            }
            else
            {
                if(gate_stream)
                    source_document = gate_stream->qasm_source_document();
                if(legacy_qasm_source_operations)
                    source_operations = &*legacy_qasm_source_operations;
            }

            if(!source_document || !source_operations)
                throw std::logic_error{"Reordered QASM source provenance was not captured"};

            const uint64_t render_started = resource_profile
                    ? resource_profile_now_ns()
                    : 0;
            write_file_transactionally(
                    *reordered_qasm_output_path,
                    [&](std::ostream& output) {
                        reordered_qasm_tracker->render_to(
                                output,
                                reordered_qasm_source_name,
                                *source_document,
                                *source_operations);
                    },
                    ".qasm-tmp",
                    "reordered QASM");
            if(resource_profile)
            {
                resource_profile->render_publication_ns +=
                        resource_profile_elapsed_ns(render_started);
                std::error_code size_error;
                const uintmax_t size = std::filesystem::file_size(
                        *reordered_qasm_output_path,
                        size_error);
                if(!size_error)
                    resource_profile->output_bytes = size;
            }
        }

        if(rendered_evaluation_metrics)
        {
            if(!evaluation_metrics_final_output_path)
                throw std::logic_error{
                        "Evaluation metrics rendered without an output path"};
            write_file_transactionally(
                    *evaluation_metrics_final_output_path,
                    *rendered_evaluation_metrics,
                    ".stats-tmp",
                    "evaluation statistics");
            out_stream << "Saved evaluation metrics to "
                       << evaluation_metrics_final_output_path->string()
                       << std::endl;
        }

        if(resource_profile)
        {
            resource_profile->update_process_max_rss();
            const std::string rendered_profile = resource_profile->render_json();
            if(resource_profile_output_path)
            {
                write_file_transactionally(
                        *resource_profile_output_path,
                        rendered_profile,
                        ".profile-tmp",
                        "resource profile");
            }
            else
                err_stream << rendered_profile;
        }

        return 0;
    }



    std::string run_slicer_program_from_strings(std::string command_line, std::string standard_input)
    {
        std::vector<std::string> args = lstk::split_on_get_strings(command_line, ' ');
        std::vector<const char*> c_args;

        c_args.push_back("slicer_from_strings"); // Placeholder name to occupy the program name's place in argv
        for (const auto &arg : args)
        {
            c_args.push_back(arg.c_str());
        }

        std::istringstream input{standard_input};
        std::ostringstream output;
        std::ostringstream err;

        int exit_code = 100;
        try
        {
            exit_code = run_slicer_program(static_cast<int>(c_args.size()), c_args.data(), input, output, err);
        }
        catch (const std::exception& e)
        {
            err << "Compiler exception: " << e.what() << std::endl;
        }

        nlohmann::json json_res = {
                {"output", output.str()},
                {"err",err.str()},
                {"exit_code", exit_code}
        };

        return json_res.dump(3);
    }




}
