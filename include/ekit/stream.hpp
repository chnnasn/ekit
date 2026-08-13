#pragma once
// ekit - stream.hpp
//
// ScratchSoa<Ts...> is an aligned, growable structure-of-arrays staging buffer
// for the "collect then batch" (stream processing) pattern:
//
//   Phase 1 (collect, scalar): gather scattered / randomly-accessed data and
//            append one record at a time into contiguous SoA columns.
//   Phase 2 (batch, SIMD):     consume the collected rows with ForEachBatch,
//            which hands out raw, aligned column pointers for vectorization.
//
// This turns N repeated random gathers into one collect pass plus N cheap
// batch passes over contiguous memory.

#include "core.hpp"
#include "parallel.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <vector>

namespace ekit {

template<typename... Ts>
class ScratchSoa {
    static_assert(sizeof...(Ts) > 0, "ekit::ScratchSoa requires at least one column type.");
    static_assert((std::is_trivially_copyable_v<Ts> && ...),
                  "ekit::ScratchSoa columns must be trivially copyable.");

public:
    ScratchSoa() = default;

    // Number of collected records.
    std::size_t Size() const {
        return size_;
    }

    // Drops all records but keeps the allocated capacity.
    void Clear() {
        size_ = 0;
    }

    // Reserves storage for at least `rows` records.
    void Reserve(std::size_t rows) {
        ReserveImpl(rows, std::index_sequence_for<Ts...>{});
    }

    // Appends one record (one value per column). The argument count must match
    // the number of columns, and each argument must be convertible to its
    // column type.
    template<typename... Args>
    void Append(const Args&... values) {
        static_assert(sizeof...(Args) == sizeof...(Ts),
                      "ekit::ScratchSoa::Append argument count must match the number of columns.");
        AppendImpl(std::index_sequence_for<Ts...>{}, std::tie(values...));
        ++size_;
    }

    // Batch-consume all collected rows with raw SoA column pointers + count:
    //   fn(Ts*..., std::size_t count)
    template<typename F>
    void ForEachBatch(F&& fn) {
        BatchImpl(fn, std::index_sequence_for<Ts...>{});
    }

    // Parallel batch consumption: collected rows are processed in fixed
    // batches across the supplied thread pool.
    template<typename F>
    void ForEachBatchParallel(ThreadPool& pool, F&& fn, std::size_t batch_size = 256) {
        const std::size_t n = size_;
        if (n == 0) {
            return;
        }
        const std::size_t num_batches = (n + batch_size - 1) / batch_size;
        detail::ParallelFor(pool, num_batches, [&](std::size_t b0, std::size_t b1) {
            for (std::size_t bi = b0; bi < b1; ++bi) {
                const std::size_t base = bi * batch_size;
                const std::size_t count = (std::min)(batch_size, n - base);
                BatchRangeImpl(fn, base, count, std::index_sequence_for<Ts...>{});
            }
        });
    }

private:
    template<std::size_t... I>
    void ReserveImpl(std::size_t rows, std::index_sequence<I...>) {
        ((columns_[I].reserve(rows * sizeof(Ts))), ...);
    }

    template<std::size_t... I, typename Tuple>
    void AppendImpl(std::index_sequence<I...>, Tuple tup) {
        ((Write<I>(size_, std::get<I>(tup))), ...);
    }

    template<std::size_t I, typename A>
    void Write(std::size_t row, const A& value) {
        using T = std::tuple_element_t<I, std::tuple<Ts...>>;
        auto& col = columns_[I];
        const std::size_t offset = row * sizeof(T);
        if (offset + sizeof(T) > col.size()) {
            col.resize(offset + sizeof(T));
        }
        const T tmp = static_cast<T>(value);
        std::memcpy(col.data() + offset, &tmp, sizeof(T));
    }

    template<typename F, std::size_t... I>
    void BatchImpl(F& fn, std::index_sequence<I...>) {
        fn(reinterpret_cast<Ts*>(columns_[I].data())..., size_);
    }

    template<typename F, std::size_t... I>
    void BatchRangeImpl(F& fn, std::size_t base, std::size_t count, std::index_sequence<I...>) {
        fn((reinterpret_cast<Ts*>(columns_[I].data()) + base)..., count);
    }

    std::array<std::vector<std::byte>, sizeof...(Ts)> columns_;
    std::size_t size_ = 0;
};

} // namespace ekit
