#ifndef LSQECC_RTLS_EVALUATION_METRICS_HPP
#define LSQECC_RTLS_EVALUATION_METRICS_HPP

#include <lsqecc/ls_instructions/ls_instructions.hpp>
#include <lsqecc/patches/dense_slice.hpp>
#include <lsqecc/rtls/operation_schedule.hpp>
#include <lsqecc/rtls/rtls.hpp>

#include <filesystem>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lsqecc {

/**
 * One caller-supplied timing-model evaluation point.
 *
 * Times use the same unit as decoder_tw (microseconds in the public CLI).
 */
struct EvaluationInput
{
    double decoder_tw = 0;
    int code_distance = 0;

    bool operator==(const EvaluationInput&) const = default;
};

enum class EvaluationMethod
{
    Qsink,
    QsinkSwiper
};

/**
 * One qSink-side row from the confirmed s5b timing model.
 */
struct EvaluationMetrics
{
    EvaluationInput input;
    EvaluationMethod method = EvaluationMethod::Qsink;
    double wait_factor = 0;
    double duration = 0;
    size_t path_gate_count = 0;
    size_t path_t_count = 0;
    double path_idle_wait = 0;
    double total_idle_wait = 0;
    double hidden_decoder_wait = 0;
    size_t split_correction_count = 0;
    size_t unmatched_split_t_count = 0;
    std::vector<size_t> path_operation_indices;
};

using EvaluationSchedule = OperationSchedule;

enum class EvaluationOperationKind : uint8_t
{
    Zero,
    Default,
    H,
    S,
    RegularT,
    SplitT,
    Cnot,
    SplitCnot
};

inline constexpr uint32_t kNoEvaluationOperationIndex =
        std::numeric_limits<uint32_t>::max();

struct EvaluationOperation
{
    EvaluationOperationKind kind = EvaluationOperationKind::Default;
    std::array<QubitNum, 2> qubits{};
    uint8_t qubit_count = 0;
    OperationHandle operation_handle = kInvalidOperationHandle;
};

/**
 * Compact evaluator input retained after full logical operations are consumed
 * by lowering. Correction links use dense indices rather than string maps.
 */
struct EvaluationProgram
{
    std::vector<EvaluationOperation> operations;
    std::vector<uint32_t> correction_by_t;
    std::vector<uint32_t> t_by_correction;
    size_t correction_pair_count = 0;
};

EvaluationProgram prepare_evaluation_program(const RtlsProgram& program);

using EvaluationScheduleTracker = OperationScheduleTracker;

/**
 * Parses the explicit value accepted by --print-stat.
 *
 * The public spellings are True and False; matching is case-insensitive.
 */
bool parse_print_stat_value(std::string_view value);

/**
 * Parses scalar or bracketed --tw/--d values and pairs arrays by index.
 *
 * Throws std::invalid_argument with option- and element-specific diagnostics.
 */
std::vector<EvaluationInput> parse_evaluation_inputs(
        std::string_view decoder_tw_values,
        std::string_view code_distance_values);

std::string_view evaluation_method_name(EvaluationMethod method);

/**
 * Evaluates two rows per input in qsink, qsink-swiper order.
 */
std::vector<EvaluationMetrics> evaluate_metrics(
        const RtlsProgram& program,
        const EvaluationSchedule& schedule,
        const std::vector<EvaluationInput>& inputs);

std::vector<EvaluationMetrics> evaluate_metrics(
        const EvaluationProgram& program,
        const OperationMetadataArena& operation_metadata,
        const EvaluationSchedule& schedule,
        const std::vector<EvaluationInput>& inputs);

/**
 * Renders the stable public sidecar table with shortest round-trip numbers.
 */
std::string render_evaluation_metrics_table(
        const std::vector<EvaluationMetrics>& metrics);

/**
 * Returns <final_rtls_stem>.stats.txt beside the final RTLS output.
 */
std::filesystem::path evaluation_metrics_output_path(
        const std::filesystem::path& final_rtls_path);

}

#endif
