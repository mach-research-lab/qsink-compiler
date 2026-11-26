#ifndef LSQECC_SCHEDULER_CHUNKED_ARENA_HPP
#define LSQECC_SCHEDULER_CHUNKED_ARENA_HPP

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace lsqecc {

/**
 * Index-stable append-only storage with bounded slack.
 *
 * Growing the outer vector moves only small vector descriptors; elements in
 * completed chunks never move.  Instruction IDs therefore remain ordinary
 * dense integer indices without paying for a whole-DAG vector relocation.
 */
template<typename T, size_t ChunkElements = 256>
class ChunkedArena
{
    static_assert(ChunkElements > 0);

public:
    void reserve_index(size_t expected_elements)
    {
        chunks_.reserve((expected_elements + ChunkElements - 1) / ChunkElements);
    }

    template<typename U>
    void push_back(U&& value)
    {
        if(chunks_.empty() || chunks_.back().size() == ChunkElements)
        {
            chunks_.emplace_back();
            chunks_.back().reserve(ChunkElements);
        }
        chunks_.back().push_back(std::forward<U>(value));
        size_++;
    }

    T& operator[](size_t index)
    {
        assert(index < size_);
        return chunks_[index / ChunkElements][index % ChunkElements];
    }

    const T& operator[](size_t index) const
    {
        assert(index < size_);
        return chunks_[index / ChunkElements][index % ChunkElements];
    }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    size_t capacity() const noexcept { return chunks_.size() * ChunkElements; }
    size_t chunk_count() const noexcept { return chunks_.size(); }
    static constexpr size_t chunk_elements() noexcept { return ChunkElements; }

private:
    std::vector<std::vector<T>> chunks_;
    size_t size_ = 0;
};

}

#endif
