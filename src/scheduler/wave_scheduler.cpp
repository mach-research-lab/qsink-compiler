#include <lsqecc/scheduler/wave_scheduler.hpp>
#include <lsqecc/resource_profile.hpp>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>


namespace lsqecc {

namespace
{

bool debug_wave_enabled()
{
	const char* value = std::getenv("LSQECC_DEBUG_WAVE");
	return value && value[0] != '\0';
}

std::string describe_instruction(
		uint32_t id,
		const LSInstruction& instruction,
		const OperationMetadataArena* operation_metadata_arena)
{
	std::ostringstream out;
	out << "#" << id << " " << instruction;
	if(instruction.rtls_operation && operation_metadata_arena)
	{
		const auto& metadata = operation_metadata_arena->compact_at(
				*instruction.rtls_operation);
		out << " rtls=" << rtls_operation_code_name(metadata.operation) << "/"
		    << serialize_compact_operation_id(metadata.operation_id);
	}
	out << " wait=" << instruction.wait_at_most_for;
	return out.str();
}

}


WaveScheduler::WaveScheduler(std::unique_ptr<LSInstructionStream> stream, bool local_instructions, bool allow_twists, bool gen_op_ids, const Layout& layout, std::unique_ptr<Router> router, PipelineMode pipeline_mode, OperationVisibilityVisitor operation_visibility_visitor, ResourceProfile* resource_profile):
	local_instructions_(local_instructions),
	allow_twists_(allow_twists),
	gen_op_ids_(gen_op_ids),
	layout_(layout),
	operation_visibility_visitor_(std::move(operation_visibility_visitor)),
	resource_profile_(resource_profile)
{
	const uint64_t construction_start = resource_profile_
			? resource_profile_now_ns()
			: 0;
	if(!stream)
		throw std::invalid_argument{"WaveScheduler requires an instruction stream"};
	router_ = std::move(router);
	router_->set_resource_profile(resource_profile_);
	operation_metadata_arena_ = stream->operation_metadata_arena();
	const size_t instruction_count_estimate = stream->instruction_count_estimate();
	if(resource_profile_)
	{
		resource_profile_->lowerer_estimate = instruction_count_estimate;
		resource_profile_->record_element_size = sizeof(InstructionRecord);
		resource_profile_->dependency_edge_element_size = sizeof(DependencyEdge);
	}
	if(instruction_count_estimate > 0)
	{
		records_.reserve_index(instruction_count_estimate);
		dependency_counts_.reserve(instruction_count_estimate);
		dependency_edges_.reserve_index(instruction_count_estimate);
	}
	if(resource_profile_)
	{
		resource_profile_->record_size_after_reserve = records_.size();
		resource_profile_->record_capacity_after_reserve = records_.capacity();
		resource_profile_->note_capacity_event(
				"records", "initial_reserve", 0,
				0, 0, records_.size(), records_.capacity(),
				sizeof(InstructionRecord), true, false);
		resource_profile_->note_capacity_event(
				"dependency_counts", "initial_reserve", 0,
				0, 0, dependency_counts_.size(), dependency_counts_.capacity(),
				sizeof(uint8_t), true);
		resource_profile_->note_capacity_event(
				"dependency_edges", "initial_reserve", 0,
				0, 0, dependency_edges_.size(), dependency_edges_.capacity(),
				sizeof(DependencyEdge), true, false);
	}
	if (pipeline_mode == PipelineMode::EDPC)
	{
		router_->set_EDPC();
	}	
	
	std::unordered_map<PatchId, InstructionID> patch_id_to_last_instruction;
	std::unordered_map<DependencyTokenId, InstructionID> token_to_last_producer;
	
	while (stream->has_next_instruction())
	{
		auto instruction = stream->get_next_instruction();
		if(resource_profile_)
		{
			for(const DependencyToken& token : instruction.dependency_tokens)
			{
				resource_profile_->dependency_token_entries++;
				if(token.role == DependencyTokenRole::Produced)
					resource_profile_->dependency_token_producers++;
				else if(token.role == DependencyTokenRole::Consumed)
					resource_profile_->dependency_token_consumers++;
			}
		}
		InstructionID instruction_id = uint32_t(records_.size());
		assert(records_.size() < UINT32_MAX);
		
		auto operating_patches = instruction.get_patch_dependencies();
		assert(operating_patches.size() <= UINT8_MAX);
		
		uint8_t dependency_count = 0;
		
		bool is_head = true;
		
		for (auto patch_id : operating_patches) {
			auto p = patch_id_to_last_instruction.try_emplace(patch_id, 0);
			auto& last_instruction_on_patch = p.first->second;
			
			if (!p.second) // there was a previous instruction on this patch
			{
				++dependency_count;
				add_dependent(last_instruction_on_patch, instruction_id);
				is_head = false;
			}
			
			last_instruction_on_patch = instruction_id;
		}

		for (const DependencyToken& token : instruction.dependency_tokens)
		{
			if(token.role != DependencyTokenRole::Consumed)
				continue;
			auto producer = token_to_last_producer.find(token.id);
			if(producer == token_to_last_producer.end())
				throw std::logic_error{
						"LSInstruction consumed dependency token before producer: " +
						std::to_string(token.id)};

			++dependency_count;
			add_dependent(producer->second, instruction_id);
			is_head = false;
		}

		for (const DependencyToken& token : instruction.dependency_tokens)
			if(token.role == DependencyTokenRole::Produced)
				token_to_last_producer[token.id] = instruction_id;

		const size_t record_size_before = records_.size();
		const size_t record_capacity_before = records_.capacity();
		records_.push_back(InstructionRecord{std::move(instruction)});
		if(resource_profile_)
			resource_profile_->note_capacity_event(
					"records", "initial_drain", 0,
					record_size_before, record_capacity_before,
					records_.size(), records_.capacity(), sizeof(InstructionRecord),
					false, false);
		
		const size_t counts_size_before = dependency_counts_.size();
		const size_t counts_capacity_before = dependency_counts_.capacity();
		dependency_counts_.push_back(dependency_count);
		if(resource_profile_)
			resource_profile_->note_capacity_event(
					"dependency_counts", "initial_drain", 0,
					counts_size_before, counts_capacity_before,
					dependency_counts_.size(), dependency_counts_.capacity(),
					sizeof(uint8_t));
		
		if (is_head)
		{
			current_wave_.heads.push_back(instruction_id);	
		}
	}
	stream.reset();
	if(resource_profile_)
	{
		resource_profile_->initial_unique_nodes = records_.size();
		resource_profile_->lower_dag_construction_ns +=
				resource_profile_elapsed_ns(construction_start);
	}
}

WaveScheduler::~WaveScheduler()
{
	if(!resource_profile_)
		return;
	resource_profile_->record_final_size = records_.size();
	resource_profile_->record_final_capacity = records_.capacity();
	resource_profile_->dependency_count_final_size = dependency_counts_.size();
	resource_profile_->dependency_count_final_capacity = dependency_counts_.capacity();
	resource_profile_->dependency_edge_final_size = dependency_edges_.size();
	resource_profile_->dependency_edge_final_capacity = dependency_edges_.capacity();
	resource_profile_->update_process_max_rss();
}

void WaveScheduler::note_physical_slice_emitted()
{
	if(physical_slice_generation_ == std::numeric_limits<size_t>::max())
		throw std::overflow_error{"Wave scheduler physical-slice generation exhausted"};
	++physical_slice_generation_;
}
	
WaveStats WaveScheduler::schedule_wave(DenseSlice& slice, LSInstructionVisitor instruction_visitor, DensePatchComputationResult& res)
{
	size_t applied_count = 0;
	++wave_number_;
	if(resource_profile_)
		resource_profile_->waves = wave_number_;
	current_wave_instructions_applied_ = 0;
	current_wave_frame_updates_applied_ = 0;
	current_wave_dependency_token_producers_applied_ = 0;
	const bool debug_wave = debug_wave_enabled();

	if(debug_wave)
	{
		std::cerr << "[LSQECC_DEBUG_WAVE] wave=" << wave_number_
		          << " heads=" << current_wave_.heads.size()
		          << " proximate=" << current_wave_.proximate_heads_.size()
		          << " deferred=" << current_wave_.deferred_to_end.size()
		          << std::endl;
	}

	// std::cerr << "PROXIMATE HEADS" << std::endl;
	applied_count += schedule_instructions(current_wave_.proximate_heads_, slice, instruction_visitor, res, true, false);
	// std::cerr << "HEADS" << std::endl;
	applied_count += schedule_instructions(current_wave_.heads, slice, instruction_visitor, res, false, false);

	if (router_->get_EDPC())
	{
		/* After the routing of edge-disjoint paths for all BellBasedCNOTs in a wave, they are deferred because their specific decomposition depends on the others.
		   Once all BellBasedCNOTs in a wave have been seen, they are each decomposed into local instructions and then deferred once more (decomposition algorithm requires slice to have EDPC info retained).
		   Finally, on the third encounter, the first layer of instructions is scheduled, and each instruction reschedules itself.
		*/

		// std::cerr << "DEFERRED ROUND 1" << std::endl;
		applied_count += schedule_instructions(current_wave_.get_deferred(), slice, instruction_visitor, res, false, true);

		// Reset boundaries for data qubits
		for (const auto& bdry : slice.marked_rough_boundaries_EDPC)
		{
			bdry.get().boundary_type = BoundaryType::Rough;
		}
		for (const auto& bdry : slice.marked_smooth_boundaries_EDPC)
		{
			bdry.get().boundary_type = BoundaryType::Smooth;
		}

		slice.marked_rough_boundaries_EDPC.clear();
		slice.marked_smooth_boundaries_EDPC.clear();

		// std::cerr << "DEFERRED ROUND 2" << std::endl;
		applied_count += schedule_instructions(current_wave_.get_deferred(), slice, instruction_visitor, res, false, true);
	}
	
	WaveStats wave_stats = {
			.wave_size = current_wave_.size(),
			.applied_wave_size = current_wave_instructions_applied_,
			.frame_update_wave_size = current_wave_frame_updates_applied_,
			.dependency_token_producer_wave_size = current_wave_dependency_token_producers_applied_};

	if(debug_wave)
	{
		std::cerr << "[LSQECC_DEBUG_WAVE] wave=" << wave_number_
		          << " applied=" << applied_count
		          << " next_heads=" << next_wave_.heads.size()
		          << " next_proximate=" << next_wave_.proximate_heads_.size()
		          << " next_deferred=" << next_wave_.deferred_to_end.size()
		          << std::endl;

		if(applied_count == 0 && current_wave_.size() > 0)
			++no_progress_wave_count_;
		else
			no_progress_wave_count_ = 0;

		if(no_progress_wave_count_ >= 100)
		{
			std::ostringstream error;
			error << "Wave scheduler made no progress for "
			      << no_progress_wave_count_ << " consecutive waves. Current heads:";
			size_t printed = 0;
			for(InstructionID id : current_wave_.proximate_heads_)
			{
				error << "\n  proximate " << describe_instruction(
						id, records_[id].instruction, operation_metadata_arena_);
				if(++printed >= 12)
					break;
			}
			for(InstructionID id : current_wave_.heads)
			{
				if(printed >= 12)
					break;
				error << "\n  head " << describe_instruction(
						id, records_[id].instruction, operation_metadata_arena_);
				++printed;
			}
			throw std::runtime_error{error.str()};
		}
	}
	
	std::swap(current_wave_, next_wave_);
    next_wave_.clear();
    
	// std::cerr << "END OF WAVE" << std::endl;
    return wave_stats;
}

size_t WaveScheduler::schedule_instructions(const std::vector<InstructionID>& instruction_ids, DenseSlice& slice, LSInstructionVisitor instruction_visitor, DensePatchComputationResult& res, bool proximate, bool deferred)
{
	size_t applied_count = 0;
	
	for (auto instruction_id : instruction_ids)
	{
		assert(dependency_counts_[instruction_id] == 0);
		
		auto& instruction = records_[instruction_id].instruction;
		const bool debug_wave = debug_wave_enabled();

		if(debug_wave)
			std::cerr << "[LSQECC_DEBUG_WAVE] consider "
			          << describe_instruction(
						  instruction_id, instruction, operation_metadata_arena_)
			          << " proximate=" << proximate
			          << " deferred=" << deferred << std::endl;

		// A T-body token is a physical-slice barrier, not only a DAG edge.
		// Successive scheduler waves may otherwise share this DenseSlice.
		if(t_body_done_consumer_blocked(instruction))
		{
			if(debug_wave)
				std::cerr << "[LSQECC_DEBUG_WAVE] defer-t-body-consumer "
				          << describe_instruction(
							  instruction_id, instruction, operation_metadata_arena_)
				          << std::endl;
			next_wave_.heads.push_back(instruction_id);
			continue;
		}

		// std::cerr << "Applying instruction: " << instruction << std::endl;
		auto application_result = try_apply_instruction_direct_followup(slice, instruction, local_instructions_, allow_twists_, gen_op_ids_, layout_, *router_);
		propagate_rtls_metadata_to_followups(application_result, instruction);
		if (!application_result.maybe_error)
			stamp_active_patches_with_rtls_metadata(
					slice,
					instruction,
					operation_visibility_visitor_);
		// std::cerr << "Applied instruction: " << instruction << std::endl;

		if (!application_result.maybe_error)
		{   
			if(debug_wave)
				std::cerr << "[LSQECC_DEBUG_WAVE] applied "
				          << describe_instruction(
							  instruction_id, instruction, operation_metadata_arena_)
				          << " followups=" << application_result.followup_instructions.size()
				          << std::endl;
			note_applied_instruction(instruction);
			++applied_count;
		    ++res.ls_instructions_count_;
			if(resource_profile_)
				resource_profile_->successful_applications++;
		    instruction_visitor(instruction);

			if (application_result.followup_instructions.size() == 1 && application_result.followup_instructions[0] == instruction) // instruction has rescheduled itself
			{
				if(resource_profile_)
					resource_profile_->self_reschedules++;
				next_wave_.proximate_heads_.push_back(instruction_id);
			}
			else
			{
				assert(!deferred); // Applied deferred instructions should always fall into the case above
				schedule_dependent_instructions(instruction_id, std::move(application_result.followup_instructions), slice, instruction_visitor, res, proximate);
			}
		}
		else
		{
			// This should only be touched in the case of EDPC compilation where local compilation cannot be performed until the whole EDP set is constructed
			if (application_result.followup_instructions.size() == 1 && application_result.followup_instructions[0] == instruction)
			{
				if(debug_wave)
					std::cerr << "[LSQECC_DEBUG_WAVE] defer-edpc "
					          << describe_instruction(
								  instruction_id, instruction, operation_metadata_arena_)
					          << std::endl;
				assert(router_->get_EDPC());
				if(resource_profile_)
					resource_profile_->deferrals++;
				current_wave_.deferred_to_end.push_back(instruction_id);
				continue;
			}

			// Deferred instructions get decomposed into local instructions and return an error.
			else if (deferred)
			{
				if(debug_wave)
					std::cerr << "[LSQECC_DEBUG_WAVE] deferred-not-applied "
					          << describe_instruction(
								  instruction_id, instruction, operation_metadata_arena_)
					          << " error=\"" << application_result.maybe_error->what()
					          << "\"" << std::endl;
				assert(router_->get_EDPC());
				if(resource_profile_)
					resource_profile_->deferrals++;
				continue;
			}

			if (proximate)
                throw std::runtime_error{lstk::cat(
                    "Could not apply proximate instruction:\n",
                    instruction,"\n",
                    "Caused by:\n",
                    application_result.maybe_error->what())};
            
		    if (instruction.wait_at_most_for == 0)
	            throw std::runtime_error{lstk::cat(
	                "Could not apply instruction after max retries:\n",
	                instruction,"\n",
	                "Caused by:\n",
	                application_result.maybe_error->what())};
		    
		    --instruction.wait_at_most_for;
			if(resource_profile_)
				resource_profile_->retries++;
			if(debug_wave)
				std::cerr << "[LSQECC_DEBUG_WAVE] retry-next-wave "
				          << describe_instruction(
							  instruction_id, instruction, operation_metadata_arena_)
				          << " error=\"" << application_result.maybe_error->what()
				          << "\"" << std::endl;
		    next_wave_.heads.push_back(instruction_id);
		}
	}
	
	return applied_count;
}
	
void WaveScheduler::schedule_dependent_instructions(InstructionID instruction_id, std::vector<LSInstruction>&& followup_instructions, DenseSlice& slice, LSInstructionVisitor instruction_visitor, DensePatchComputationResult& res, bool proximate)
{
	const DependencyEdgeIndex first_dependent = records_[instruction_id].first_dependent;
	const DependencyEdgeIndex last_dependent = records_[instruction_id].last_dependent;
	
	if (followup_instructions.empty())
	{
		for (DependencyEdgeIndex edge = first_dependent;
		     edge != kNoDependencyEdge;
		     edge = dependency_edges_[edge].next)
		{
			const InstructionID dependent = dependency_edges_[edge].dependent;
			assert(dependency_counts_[dependent] > 0);
			--dependency_counts_[dependent];
			if (dependency_counts_[dependent] == 0)
			{
				if (proximate)
				{
					auto& instruction = records_[dependent].instruction;
					if (is_immediate(instruction) && !should_defer_rtls_start(instruction))
						current_wave_.heads.push_back(dependent);
					else
						next_wave_.heads.push_back(dependent);
				}
					
				else
				{
					if (!try_schedule_immediately(dependent, slice, instruction_visitor, res, proximate))
						next_wave_.heads.push_back(dependent);
				}
			}
		}
	}
	else
	{
		auto dependency_delta = followup_instructions.size() - 1;
		if (dependency_delta != 0)
		{
			for (DependencyEdgeIndex edge = first_dependent;
			     edge != kNoDependencyEdge;
			     edge = dependency_edges_[edge].next)
			{
				const InstructionID dependent = dependency_edges_[edge].dependent;
				assert(dependency_counts_[dependent] + dependency_delta <= UINT8_MAX);
				dependency_counts_[dependent] += dependency_delta;
			}
		}
		
		for (LSInstruction& followup : followup_instructions)
		{
			auto followup_id = records_.size();
			assert(records_.size() < UINT32_MAX);
			const size_t record_size_before = records_.size();
			const size_t record_capacity_before = records_.capacity();
			records_.push_back(InstructionRecord{
				std::move(followup),
				first_dependent,
				last_dependent});
			if(resource_profile_)
			{
				resource_profile_->generated_followup_nodes++;
				resource_profile_->note_capacity_event(
						"records", "generated_followup", wave_number_,
						record_size_before, record_capacity_before,
						records_.size(), records_.capacity(), sizeof(InstructionRecord),
						false, false);
			}
			const size_t counts_size_before = dependency_counts_.size();
			const size_t counts_capacity_before = dependency_counts_.capacity();
			dependency_counts_.push_back(0);
			if(resource_profile_)
				resource_profile_->note_capacity_event(
						"dependency_counts", "generated_followup", wave_number_,
						counts_size_before, counts_capacity_before,
						dependency_counts_.size(), dependency_counts_.capacity(),
						sizeof(uint8_t));

			if (!try_schedule_immediately(followup_id, slice, instruction_visitor, res, proximate))
				next_wave_.proximate_heads_.push_back(followup_id);
		}
	}

	release_instruction_payload(instruction_id);
}

void WaveScheduler::add_dependent(
		InstructionID instruction_id,
		InstructionID dependent)
{
	assert(dependency_edges_.size() < UINT32_MAX);
	const DependencyEdgeIndex edge_index =
			static_cast<DependencyEdgeIndex>(dependency_edges_.size());
	const size_t edge_size_before = dependency_edges_.size();
	const size_t edge_capacity_before = dependency_edges_.capacity();
	dependency_edges_.push_back(DependencyEdge{dependent});
	if(resource_profile_)
		resource_profile_->note_capacity_event(
				"dependency_edges", "initial_drain", wave_number_,
				edge_size_before, edge_capacity_before,
				dependency_edges_.size(), dependency_edges_.capacity(),
				sizeof(DependencyEdge), false, false);

	InstructionRecord& record = records_[instruction_id];
	if(record.first_dependent == kNoDependencyEdge)
		record.first_dependent = edge_index;
	else
		dependency_edges_[record.last_dependent].next = edge_index;
	record.last_dependent = edge_index;
}

void WaveScheduler::release_instruction_payload(InstructionID instruction_id)
{
	records_[instruction_id].instruction = LSInstruction{
			.operation = PatchReset{0}};
}
	
bool WaveScheduler::is_immediate(const LSInstruction& instruction)
{
	if (std::get_if<SinglePatchMeasurement>(&instruction.operation))
		return true;
	else if (std::get_if<MultiPatchMeasurement>(&instruction.operation))
		return true;
	else if (std::get_if<PatchInit>(&instruction.operation))
		return true;
	else if (std::get_if<BellPairInit>(&instruction.operation))
		return true;
	else if (std::get_if<MagicStateRequest>(&instruction.operation))
		return true;
	else if (std::get_if<YStateRequest>(&instruction.operation))
		return true;
	else if (auto* op = std::get_if<SingleQubitOp>(&instruction.operation))
	{
		switch(op->op)
		{
			case SingleQubitOp::Operator::X: return true;
			case SingleQubitOp::Operator::Y: return true;
			case SingleQubitOp::Operator::Z: return true;
			case SingleQubitOp::Operator::H: return false;
			case SingleQubitOp::Operator::S: return false;
			default: LSTK_UNREACHABLE;
		}
	}
	else if (std::get_if<RotateSingleCellPatch>(&instruction.operation))
		return true;
	else if (std::get_if<BusyRegion>(&instruction.operation))
		return false;
	else if (std::get_if<PatchReset>(&instruction.operation))
		return true;
	else if (std::get_if<BellBasedCNOT>(&instruction.operation))
		return true;
	else
		LSTK_UNREACHABLE;
}

bool WaveScheduler::is_frame_update(const LSInstruction& instruction) const
{
	const auto* op = std::get_if<SingleQubitOp>(&instruction.operation);
	if(!op)
		return false;

	return op->op == SingleQubitOp::Operator::X ||
	       op->op == SingleQubitOp::Operator::Y ||
	       op->op == SingleQubitOp::Operator::Z;
}

void WaveScheduler::note_applied_instruction(const LSInstruction& instruction)
{
	if(is_frame_update(instruction))
		++current_wave_frame_updates_applied_;
	if(instruction.has_produced_dependency_token())
		++current_wave_dependency_token_producers_applied_;
	++current_wave_instructions_applied_;
	for(const DependencyToken& token : instruction.dependency_tokens)
		if(token.role == DependencyTokenRole::Produced &&
		   token.kind == DependencyTokenKind::RtlsTBodyDone)
			t_body_done_producer_generation_[token.id] =
					physical_slice_generation_;
	note_applied_rtls_operation(instruction);
}

bool WaveScheduler::t_body_done_consumer_blocked(
		const LSInstruction& instruction) const
{
	for(const DependencyToken& token : instruction.dependency_tokens)
	{
		if(token.role != DependencyTokenRole::Consumed ||
		   token.kind != DependencyTokenKind::RtlsTBodyDone)
			continue;
		const auto producer = t_body_done_producer_generation_.find(token.id);
		if(producer != t_body_done_producer_generation_.end() &&
		   producer->second == physical_slice_generation_)
			return true;
	}
	return false;
}
	
bool WaveScheduler::try_schedule_immediately(InstructionID instruction_id, DenseSlice& slice, LSInstructionVisitor instruction_visitor, DensePatchComputationResult& res, bool proximate)
{
	auto& instruction = records_[instruction_id].instruction;
	if (t_body_done_consumer_blocked(instruction))
		return false;
	
	if (!is_immediate(instruction))
		return false;
	if (should_defer_rtls_start(instruction))
		return false;
	
	// std::cerr << "Applying dependent: " << instruction << std::endl;
	auto application_result = try_apply_instruction_direct_followup(slice, instruction, local_instructions_, allow_twists_, gen_op_ids_, layout_, *router_);
	propagate_rtls_metadata_to_followups(application_result, instruction);
	if (!application_result.maybe_error)
		stamp_active_patches_with_rtls_metadata(
				slice,
				instruction,
				operation_visibility_visitor_);
	// std::cerr << "Applied dependent: " << instruction << std::endl;

	if (application_result.maybe_error)
	{
		// This is the case that compilation of an EDP set is deferred to the end
		if (application_result.followup_instructions.size() == 1 && application_result.followup_instructions[0] == instruction)
		{
			assert(router_->get_EDPC());
			if(resource_profile_)
				resource_profile_->deferrals++;
			current_wave_.deferred_to_end.push_back(instruction_id);
			return true;
		}

		// Otherwise, report that instruction could not be placed
		if(resource_profile_)
			resource_profile_->retries++;
		return false;
	}
	else
	{
		note_applied_instruction(instruction);
		++res.ls_instructions_count_;
		if(resource_profile_)
			resource_profile_->successful_applications++;
		instruction_visitor(instruction);
		
		if (application_result.followup_instructions.size() == 1 && application_result.followup_instructions[0] == instruction) // instruction has rescheduled itself
		{
			if(resource_profile_)
				resource_profile_->self_reschedules++;
			next_wave_.proximate_heads_.push_back(instruction_id);
		}
		else
			schedule_dependent_instructions(instruction_id, std::move(application_result.followup_instructions), slice, instruction_visitor, res, proximate);
		
		return true;
	}
}

bool WaveScheduler::should_defer_rtls_mx_start(const LSInstruction& instruction) const
{
	if (!instruction.rtls_operation || !operation_metadata_arena_ ||
	    operation_metadata_arena_->compact_at(
			*instruction.rtls_operation).operation != RtlsOperationCode::Mx)
		return false;
	return !started_rtls_mx_operations_.contains(*instruction.rtls_operation);
}

bool WaveScheduler::should_defer_rtls_start(const LSInstruction& instruction) const
{
	return should_defer_rtls_mx_start(instruction);
}

void WaveScheduler::note_applied_rtls_operation(const LSInstruction& instruction)
{
	if (instruction.rtls_operation && operation_metadata_arena_ &&
	    operation_metadata_arena_->compact_at(
			*instruction.rtls_operation).operation == RtlsOperationCode::Mx)
		started_rtls_mx_operations_.insert(*instruction.rtls_operation);
}


}
