// -*- C++ -*-
//===-- kt_device_debug.h --------------------------------------------------===//
//
// Copyright (C) UXL Foundation Contributors
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// [DO NOT MERGE] Device side debugging of the cooperative onesweep kernel (kt-device-debug), compiled in only when
// _ONEDPL_KT_DEVICE_DEBUG is defined to a non-zero value. Without the macro this header defines nothing, so the kernel
// code generation is unchanged.
//
// A lookback which spins longer than the spin limit prints a STALL line, then sets an abort flag that makes every
// lookback give up, so that the kernel completes (and its device printf output is flushed) instead of waiting
// forever. The sort result of an aborted launch is invalid. After an abort, each work-group prints its start ticket
// and its last completed tile.
//
// Each onesweep launch gets a device buffer of three words:
//   [0] abort flag
//   [1] number of started work-groups, also the start ticket of each work-group
//   [2] number of stall lines printed, to limit the output
//
// Runtime control (environment variable):
//   ONEDPL_KT_DEVICE_DEBUG_SPIN_LIMIT=N  lookback iterations on one tile before a stall is reported (default 2^24)

#ifndef _ONEDPL_KT_DEVICE_DEBUG_H
#define _ONEDPL_KT_DEVICE_DEBUG_H

#if _ONEDPL_KT_DEVICE_DEBUG

#    include <cstdint>
#    include <cstdlib>

#    include "../../../pstl/hetero/dpcpp/sycl_defs.h"

// Device printf requires its format string in the constant address space
#    ifdef __SYCL_DEVICE_ONLY__
#        define _ONEDPL_KT_DEBUG_CONSTANT_AS __attribute__((opencl_constant))
#    else
#        define _ONEDPL_KT_DEBUG_CONSTANT_AS
#    endif

namespace oneapi::dpl::experimental::kt::gpu::__impl
{

inline constexpr std::uint32_t __kt_debug_abort = 0;
inline constexpr std::uint32_t __kt_debug_started = 1;
inline constexpr std::uint32_t __kt_debug_stall_prints = 2;
inline constexpr std::uint32_t __kt_debug_state_size = 3;
// At most this many STALL lines are printed per launch
inline constexpr std::uint32_t __kt_debug_max_stall_prints = 16;
// The abort flag and the spin limit are checked once per this many lookback iterations
inline constexpr std::uint32_t __kt_debug_check_period = 1024;

inline std::uint32_t
__kt_device_debug_spin_limit()
{
    static const std::uint32_t __limit = [] {
        const char* __value = std::getenv("ONEDPL_KT_DEVICE_DEBUG_SPIN_LIMIT");
        const unsigned long __parsed = __value ? std::strtoul(__value, nullptr, 10) : 0;
        return __parsed > 0 ? static_cast<std::uint32_t>(__parsed) : std::uint32_t{1} << 24;
    }();
    return __limit;
}

inline std::uint32_t*
__kt_device_debug_alloc(sycl::queue& __q)
{
    std::uint32_t* __state = sycl::malloc_device<std::uint32_t>(__kt_debug_state_size, __q);
    __q.memset(__state, 0, __kt_debug_state_size * sizeof(std::uint32_t)).wait();
    return __state;
}

// Waits for the launch so that its device printf output is flushed, then frees the state
inline void
__kt_device_debug_finish(sycl::queue& __q, sycl::event __event, std::uint32_t* __state)
{
    __event.wait_and_throw();
    sycl::free(__state, __q);
}

} // namespace oneapi::dpl::experimental::kt::gpu::__impl

#endif // _ONEDPL_KT_DEVICE_DEBUG

#endif // _ONEDPL_KT_DEVICE_DEBUG_H
