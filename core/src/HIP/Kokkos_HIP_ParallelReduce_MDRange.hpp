//@HEADER
// ************************************************************************
//
//                        Kokkos v. 4.0
//       Copyright (2022) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
// Part of Kokkos, under the Apache License v2.0 with LLVM Exceptions.
// See https://kokkos.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//@HEADER

#ifndef KOKKOS_HIP_PARALLEL_REDUCE_MDRANGE_HPP
#define KOKKOS_HIP_PARALLEL_REDUCE_MDRANGE_HPP

#include <Kokkos_Parallel.hpp>

#include <HIP/Kokkos_HIP_BlockSize_Deduction.hpp>
#include <HIP/Kokkos_HIP_KernelLaunch.hpp>
#include <HIP/Kokkos_HIP_ReduceScan.hpp>
#include <KokkosExp_MDRangePolicy.hpp>
#include <impl/KokkosExp_IterateTileGPU.hpp>

namespace Kokkos {
namespace Impl {

// ParallelReduce
template <class CombinedFunctorReducerType, class... Traits>
class ParallelReduce<CombinedFunctorReducerType,
                     Kokkos::MDRangePolicy<Traits...>, HIP> {
 public:
  using Policy      = Kokkos::MDRangePolicy<Traits...>;
  using FunctorType = typename CombinedFunctorReducerType::functor_type;
  using ReducerType = typename CombinedFunctorReducerType::reducer_type;

 private:
  using array_index_type = typename Policy::array_index_type;
  using index_type       = typename Policy::index_type;

  using WorkTag      = typename Policy::work_tag;
  using Member       = typename Policy::member_type;
  using LaunchBounds = typename Policy::launch_bounds;

 public:
  using pointer_type   = typename ReducerType::pointer_type;
  using value_type     = typename ReducerType::value_type;
  using reference_type = typename ReducerType::reference_type;
  using functor_type   = FunctorType;
  using reducer_type   = ReducerType;
  using size_type      = HIP::size_type;

  // Conditionally set word_size_type to int16_t or int8_t if value_type is
  // smaller than int32_t (Kokkos::HIP::size_type)
  // word_size_type is used to determine the word count, shared memory buffer
  // size, and global memory buffer size before the reduction is performed.
  // Within the reduction, the word count is recomputed based on word_size_type
  // and when calculating indexes into the shared/global memory buffers for
  // performing the reduction, word_size_type is used again.
  // For scalars > 4 bytes in size, indexing into shared/global memory relies
  // on the block and grid dimensions to ensure that we index at the correct
  // offset rather than at every 4 byte word; such that, when the join is
  // performed, we have the correct data that was copied over in chunks of 4
  // bytes.
  static_assert(sizeof(size_type) == 4);
  using word_size_type = std::conditional_t<
      sizeof(value_type) < 4,
      std::conditional_t<sizeof(value_type) == 2, int16_t, int8_t>, size_type>;

  // Algorithmic constraints: blockSize is a power of two AND blockDim.y ==
  // blockDim.z == 1

  const CombinedFunctorReducerType m_functor_reducer;
  const Policy m_policy;  // used for workrange and nwork
  const pointer_type m_result_ptr;
  const bool m_result_ptr_device_accessible;
  word_size_type* m_scratch_space;
  size_type* m_scratch_flags;

  using DeviceIteratePattern = typename Kokkos::Impl::Reduce::DeviceIterateTile<
      Policy::rank, Policy, FunctorType, WorkTag, reference_type>;

 public:
  inline __device__ void exec_range(reference_type update) const {
    DeviceIteratePattern(m_policy, m_functor_reducer.get_functor(), update)
        .exec_range();
  }

  inline __device__ void operator()() const {
    const ReducerType& reducer = m_functor_reducer.get_reducer();

    const integral_nonzero_constant<word_size_type,
                                    ReducerType::static_value_size() /
                                        sizeof(word_size_type)>
        word_count(reducer.value_size() / sizeof(word_size_type));

    {
      reference_type value = reducer.init(reinterpret_cast<pointer_type>(
          kokkos_impl_hip_shared_memory<word_size_type>() +
          threadIdx.y * word_count.value));

      // Number of blocks is bounded so that the reduction can be limited to two
      // passes. Each thread block is given an approximately equal amount of
      // work to perform. Accumulate the values for this block. The accumulation
      // ordering does not match the final pass, but is arithmetically
      // equivalent.

      this->exec_range(value);
    }

    // Reduce with final value at blockDim.y - 1 location.
    // Problem: non power-of-two blockDim
    if (::Kokkos::Impl::hip_single_inter_block_reduce_scan<false>(
            reducer, blockIdx.x, gridDim.x,
            kokkos_impl_hip_shared_memory<word_size_type>(), m_scratch_space,
            m_scratch_flags)) {
      // This is the final block with the final result at the final threads'
      // location
      word_size_type* const shared =
          kokkos_impl_hip_shared_memory<word_size_type>() +
          (blockDim.y - 1) * word_count.value;
      word_size_type* const global =
          m_result_ptr_device_accessible
              ? reinterpret_cast<word_size_type*>(m_result_ptr)
              : m_scratch_space;

      if (threadIdx.y == 0) {
        reducer.final(reinterpret_cast<value_type*>(shared));
      }

      if (Impl::HIPTraits::WarpSize < word_count.value) {
        __syncthreads();
      }

      for (unsigned i = threadIdx.y; i < word_count.value; i += blockDim.y) {
        global[i] = shared[i];
      }
    }
  }

  // Determine block size constrained by shared memory:
  // This is copy/paste from Kokkos_HIP_Parallel_Range
  inline unsigned local_block_size(const FunctorType& f) {
    const auto& instance = m_policy.space().impl_internal_space_instance();
    auto shmem_functor   = [&f](unsigned n) {
      return hip_single_inter_block_reduce_scan_shmem<false, WorkTag,
                                                      value_type>(f, n);
    };

    unsigned block_size =
        Kokkos::Impl::hip_get_preferred_blocksize<ParallelReduce, LaunchBounds>(
            instance, shmem_functor);
    if (block_size == 0) {
      Kokkos::Impl::throw_runtime_exception(
          std::string("Kokkos::Impl::ParallelReduce< HIP > could not find a "
                      "valid tile size."));
    }
    return block_size;
  }

  inline void execute() {
    ReducerType reducer = m_functor_reducer.get_reducer();

    const auto nwork = m_policy.m_num_tiles;
    if (nwork) {
      int block_size = m_policy.m_prod_tile_dims;
      // CONSTRAINT: Algorithm requires block_size >= product of tile dimensions
      // Nearest power of two
      int exponent_pow_two = std::ceil(std::log2(block_size));
      block_size           = std::pow(2, exponent_pow_two);
      int suggested_blocksize =
          local_block_size(m_functor_reducer.get_functor());

      block_size = (block_size > suggested_blocksize)
                       ? block_size
                       : suggested_blocksize;  // Note: block_size must be less
                                               // than or equal to 512

      m_scratch_space =
          reinterpret_cast<word_size_type*>(hip_internal_scratch_space(
              m_policy.space(),
              reducer.value_size() *
                  block_size /* block_size == max block_count */));
      m_scratch_flags =
          hip_internal_scratch_flags(m_policy.space(), sizeof(size_type));

      // REQUIRED ( 1 , N , 1 )
      const dim3 block(1, block_size, 1);
      // Required grid.x <= block.y
      const dim3 grid(std::min(static_cast<uint32_t>(block.y),
                               static_cast<uint32_t>(nwork)),
                      1, 1);

      const int shmem =
          ::Kokkos::Impl::hip_single_inter_block_reduce_scan_shmem<
              false, WorkTag, value_type>(m_functor_reducer.get_functor(),
                                          block.y);

      hip_parallel_launch<ParallelReduce, LaunchBounds>(
          *this, grid, block, shmem,
          m_policy.space().impl_internal_space_instance(),
          false);  // copy to device and execute

      if (!m_result_ptr_device_accessible && m_result_ptr) {
        const int size = reducer.value_size();
        DeepCopy<HostSpace, HIPSpace, HIP>(m_policy.space(), m_result_ptr,
                                           m_scratch_space, size);
      }
    } else {
      if (m_result_ptr) {
        reducer.init(m_result_ptr);
      }
    }
  }

  template <class ViewType>
  ParallelReduce(const CombinedFunctorReducerType& arg_functor_reducer,
                 const Policy& arg_policy, const ViewType& arg_result)
      : m_functor_reducer(arg_functor_reducer),
        m_policy(arg_policy),
        m_result_ptr(arg_result.data()),
        m_result_ptr_device_accessible(
            MemorySpaceAccess<HIPSpace,
                              typename ViewType::memory_space>::accessible),
        m_scratch_space(nullptr),
        m_scratch_flags(nullptr) {}

  template <typename Policy, typename Functor>
  static int max_tile_size_product(const Policy&, const Functor&) {
    using closure_type  = ParallelReduce<CombinedFunctorReducerType,
                                        Kokkos::MDRangePolicy<Traits...>, HIP>;
    unsigned block_size = hip_get_max_blocksize<closure_type, LaunchBounds>();
    if (block_size == 0) {
      Kokkos::Impl::throw_runtime_exception(
          std::string("Kokkos::Impl::ParallelReduce< HIP > could not find a "
                      "valid tile size."));
    }
    return block_size;
  }
};
}  // namespace Impl
}  // namespace Kokkos

#endif
