#ifndef LSQECC_RTLS_STREAMING_RENDERER_HPP
#define LSQECC_RTLS_STREAMING_RENDERER_HPP

#include <lsqecc/patches/dense_patch_computation.hpp>
#include <lsqecc/rtls/rtls.hpp>

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <vector>

namespace lsqecc {

/**
 * Streams RTLS output through a compact per-slice spool.
 *
 * The scheduler still produces ordinary DenseSlice objects, but the renderer
 * consumes each one immediately, keeps only operation summaries, and writes the
 * final RTLS JSON array with patched source/correction links during replay.
 */
class RtlsStreamingRenderer
{
public:
    RtlsStreamingRenderer(
            std::ostream& output,
            std::vector<RtlsCorrection> corrections,
            RtlsValidationContext validation_context,
            std::optional<std::filesystem::path> spool_hint_path = std::nullopt);

    ~RtlsStreamingRenderer();

    RtlsStreamingRenderer(const RtlsStreamingRenderer&) = delete;
    RtlsStreamingRenderer& operator=(const RtlsStreamingRenderer&) = delete;

    DenseSliceVisitor slice_visitor();
    void finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
