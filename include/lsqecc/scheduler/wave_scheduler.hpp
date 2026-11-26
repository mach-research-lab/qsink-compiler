#ifndef LSQECC_WAVE_SCHEDULER
#define LSQECC_WAVE_SCHEDULER


#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include <lstk/lstk.hpp>

#include <lsqecc/layout/layout.hpp>
#include <lsqecc/layout/router.hpp>
#include <lsqecc/ls_instructions/ls_instructions.hpp>
#include <lsqecc/ls_instructions/ls_instruction_stream.hpp>
#include <lsqecc/patches/dense_patch_computation.hpp>
#include <lsqecc/scheduler/chunked_arena.hpp>

namespace lsqecc {

struct ResourceProfile;

struct WaveStats
{
	size_t wave_size; // number of instructions in wave
	size_t applied_wave_size; // number of instructions in wave that were actually applied
	size_t frame_update_wave_size; // applied X/Y/Z frame updates
	size_t dependency_token_producer_wave_size; // applied instructions that release token dependents
};

class WaveScheduler
{
public:
	
	WaveScheduler(std::unique_ptr<LSInstructionStream> stream, bool local_instructions, bool allow_twists, bool gen_op_ids, const Layout& layout, std::unique_ptr<Router> router, PipelineMode pipeline_mode, OperationVisibilityVisitor operation_visibility_visitor = {}, ResourceProfile* resource_profile = nullptr);
	~WaveScheduler();
	
	bool done() const { return current_wave_.proximate_heads_.empty() && current_wave_.heads.empty(); }
	WaveStats schedule_wave(DenseSlice& slice, LSInstructionVisitor instruction_visitor, DensePatchComputationResult& res);
	void note_physical_slice_emitted();
	
private:
	
	using InstructionID = uint32_t;
	using DependencyEdgeIndex = uint32_t;
	static constexpr DependencyEdgeIndex kNoDependencyEdge = UINT32_MAX;

	struct DependencyEdge
	{
		InstructionID dependent;
		DependencyEdgeIndex next = kNoDependencyEdge;
	};
	
	struct InstructionRecord
	{
		LSInstruction instruction;
		DependencyEdgeIndex first_dependent = kNoDependencyEdge;
		DependencyEdgeIndex last_dependent = kNoDependencyEdge;
	};

	struct Wave
	{
		std::vector<InstructionID> heads;
		std::vector<InstructionID> proximate_heads_;
		std::vector<InstructionID> deferred_to_end;
				
		void clear()
		{
			heads.clear();
			proximate_heads_.clear();
			deferred_to_end.clear();
		}

		std::vector<InstructionID> get_deferred() const
		{
			return deferred_to_end;
		}
		
		size_t size() const { return heads.size() + proximate_heads_.size(); }
	};
	
	// returns number of instruction_ids that were applied
	size_t schedule_instructions(const std::vector<InstructionID>& instruction_ids, DenseSlice& slice, LSInstructionVisitor instruction_visitor, DensePatchComputationResult& res, bool proximate, bool deferred);
	void schedule_dependent_instructions(InstructionID instruction_id, std::vector<LSInstruction>&& followup_instructions, DenseSlice& slice, LSInstructionVisitor instruction_visitor, DensePatchComputationResult& res, bool proximate);
	void add_dependent(InstructionID instruction_id, InstructionID dependent);
	void release_instruction_payload(InstructionID instruction_id);
	
	bool is_immediate(const LSInstruction& instruction);
	bool try_schedule_immediately(InstructionID instruction_id, DenseSlice& slice, LSInstructionVisitor instruction_visitor, DensePatchComputationResult& res, bool proximate);
	bool is_frame_update(const LSInstruction& instruction) const;
	void note_applied_instruction(const LSInstruction& instruction);
	bool t_body_done_consumer_blocked(const LSInstruction& instruction) const;
	bool should_defer_rtls_mx_start(const LSInstruction& instruction) const;
	bool should_defer_rtls_start(const LSInstruction& instruction) const;
	void note_applied_rtls_operation(const LSInstruction& instruction);
	
	bool local_instructions_;
	bool allow_twists_;
	bool gen_op_ids_;
	const Layout& layout_;
	std::unique_ptr<Router> router_;
	OperationVisibilityVisitor operation_visibility_visitor_;
	ResourceProfile* resource_profile_ = nullptr;
	
	ChunkedArena<InstructionRecord> records_;
	ChunkedArena<DependencyEdge> dependency_edges_;
	std::vector<uint8_t> dependency_counts_;
	const OperationMetadataArena* operation_metadata_arena_ = nullptr;
	std::unordered_set<OperationHandle> started_rtls_mx_operations_;
	size_t current_wave_instructions_applied_ = 0;
	size_t current_wave_frame_updates_applied_ = 0;
	size_t current_wave_dependency_token_producers_applied_ = 0;
	size_t wave_number_ = 0;
	size_t no_progress_wave_count_ = 0;
	size_t physical_slice_generation_ = 0;
	std::unordered_map<DependencyTokenId, size_t>
			t_body_done_producer_generation_;
	
	Wave current_wave_, next_wave_;
};

}


#endif //LSQECC_WAVE_SCHEDULER
