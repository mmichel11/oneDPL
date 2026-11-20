// -*- C++ -*-
//===-- kt_debug_trace.h ---------------------------------------------------===//
//
// Copyright (C) UXL Foundation Contributors
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// [DO NOT MERGE] Host side tracing (kt-host-trace) used for debugging the cooperative onesweep kernel. It is compiled in only when
// _ONEDPL_KT_TRACE is defined to a non-zero value; otherwise every function below is an empty inline function.
//
// Runtime controls (environment variables):
//   ONEDPL_KT_TRACE=0             silence the trace output
//   ONEDPL_KT_TRACE_SYNC=0        do not wait for each kernel; by default every kernel is waited on and timed, so the
//                                 last "submitted" line without a matching "done" line identifies a kernel which did not complete
//   ONEDPL_KT_DEBUG_MAX_GROUPS=N  cap the number of work-groups of the cooperative onesweep kernel

#ifndef _ONEDPL_KT_DEBUG_TRACE_H
#define _ONEDPL_KT_DEBUG_TRACE_H

#include <cstddef>
#include <cstdint>

#include "../../../pstl/hetero/dpcpp/sycl_defs.h"

#if _ONEDPL_KT_TRACE
#    include <algorithm>
#    include <chrono>
#    include <cstdarg>
#    include <cstdio>
#    include <cstdlib>
#    include <cstring>
#    include <mutex>
#    include <string>
#    include <vector>
#endif

namespace oneapi::dpl::experimental::kt::gpu::__impl
{

#if _ONEDPL_KT_TRACE

inline bool
__kt_env_flag(const char* __name, bool __default_value)
{
    const char* __value = std::getenv(__name);
    return __value ? std::strcmp(__value, "0") != 0 : __default_value;
}

inline bool
__kt_trace_enabled()
{
    static const bool __enabled = __kt_env_flag("ONEDPL_KT_TRACE", true);
    return __enabled;
}

inline bool
__kt_trace_sync()
{
    static const bool __sync = __kt_env_flag("ONEDPL_KT_TRACE_SYNC", true);
    return __sync;
}

inline double
__kt_trace_seconds()
{
    static const auto __start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - __start).count();
}

inline void
__kt_trace(const char* __format, ...) __attribute__((format(printf, 1, 2)));

inline void
__kt_trace(const char* __format, ...)
{
    if (!__kt_trace_enabled())
        return;
    static std::mutex __mutex;
    std::lock_guard<std::mutex> __lock(__mutex);
    std::fprintf(stderr, "[kt-host-trace %10.3f] ", __kt_trace_seconds());
    va_list __args;
    va_start(__args, __format);
    std::vfprintf(stderr, __format, __args);
    va_end(__args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// Device topology, driver and environment, printed once per process
inline void
__kt_trace_device(const sycl::queue& __q)
{
    static bool __printed = false;
    if (__printed)
        return;
    __printed = true;

    const sycl::device __device = __q.get_device();
    std::string __sub_group_sizes;
    for (std::size_t __size : __device.get_info<sycl::info::device::sub_group_sizes>())
        __sub_group_sizes += std::to_string(__size) + " ";
    __kt_trace("device: name='%s' vendor='%s' driver_version='%s' platform='%s' backend=%d",
               __device.get_info<sycl::info::device::name>().c_str(),
               __device.get_info<sycl::info::device::vendor>().c_str(),
               __device.get_info<sycl::info::device::driver_version>().c_str(),
               __device.get_platform().get_info<sycl::info::platform::name>().c_str(),
               static_cast<int>(__device.get_backend()));
    __kt_trace("device: max_compute_units=%u local_mem_size=%llu max_work_group_size=%zu sub_group_sizes=[%s] "
               "global_mem_size=%llu",
               static_cast<unsigned>(__device.get_info<sycl::info::device::max_compute_units>()),
               static_cast<unsigned long long>(__device.get_info<sycl::info::device::local_mem_size>()),
               __device.get_info<sycl::info::device::max_work_group_size>(), __sub_group_sizes.c_str(),
               static_cast<unsigned long long>(__device.get_info<sycl::info::device::global_mem_size>()));
#    if defined(SYCL_EXT_INTEL_DEVICE_INFO)
    auto __query = [&](sycl::aspect __aspect, auto __info_tag) -> long long {
        using _Info = decltype(__info_tag);
        return __device.has(__aspect) ? static_cast<long long>(__device.get_info<_Info>()) : -1;
    };
    __kt_trace("device (intel ext): slices=%lld subslices_per_slice=%lld eu_count_per_subslice=%lld "
               "hw_threads_per_eu=%lld eu_simd_width=%lld eu_count=%lld (-1: not reported)",
               __query(sycl::aspect::ext_intel_gpu_slices, sycl::ext::intel::info::device::gpu_slices{}),
               __query(sycl::aspect::ext_intel_gpu_subslices_per_slice,
                       sycl::ext::intel::info::device::gpu_subslices_per_slice{}),
               __query(sycl::aspect::ext_intel_gpu_eu_count_per_subslice,
                       sycl::ext::intel::info::device::gpu_eu_count_per_subslice{}),
               __query(sycl::aspect::ext_intel_gpu_hw_threads_per_eu,
                       sycl::ext::intel::info::device::gpu_hw_threads_per_eu{}),
               __query(sycl::aspect::ext_intel_gpu_eu_simd_width, sycl::ext::intel::info::device::gpu_eu_simd_width{}),
               __query(sycl::aspect::ext_intel_gpu_eu_count, sycl::ext::intel::info::device::gpu_eu_count{}));
#    endif
    for (const char* __name : {"ONEAPI_DEVICE_SELECTOR", "ZE_AFFINITY_MASK", "ZE_FLAT_DEVICE_HIERARCHY",
                               "ZEX_NUMBER_OF_CCS", "ONEDPL_KT_DEBUG_MAX_GROUPS", "ONEDPL_KT_TRACE_SYNC"})
    {
        const char* __value = std::getenv(__name);
        __kt_trace("env: %s=%s", __name, __value ? __value : "<unset>");
    }
}

// Properties of the compiled kernel as reported by the runtime. The GRF size is not exposed by SYCL; the shader dump
// scripts extract it, but a reduced max_work_group_size is a sign of a large GRF mode.
inline void
__kt_trace_kernel_info(const sycl::kernel& __kernel, const sycl::queue& __q, const char* __label)
{
    const sycl::device __device = __q.get_device();
    __kt_trace("%s kernel: max_work_group_size=%zu private_mem_size=%llu max_sub_group_size=%u "
               "compile_sub_group_size=%u max_num_sub_groups=%u",
               __label, __kernel.get_info<sycl::info::kernel_device_specific::work_group_size>(__device),
               static_cast<unsigned long long>(
                   __kernel.get_info<sycl::info::kernel_device_specific::private_mem_size>(__device)),
               static_cast<unsigned>(__kernel.get_info<sycl::info::kernel_device_specific::max_sub_group_size>(__device)),
               static_cast<unsigned>(
                   __kernel.get_info<sycl::info::kernel_device_specific::compile_sub_group_size>(__device)),
               static_cast<unsigned>(__kernel.get_info<sycl::info::kernel_device_specific::max_num_sub_groups>(__device)));
}

// Reports a submitted kernel and, unless ONEDPL_KT_TRACE_SYNC=0, waits for it and reports its duration
inline void
__kt_trace_submitted(sycl::event __event, const char* __label, std::uint32_t __stage, std::size_t __group_count,
                     std::size_t __work_group_size)
{
    __kt_trace("%s stage=%u: submitted groups=%zu work_group_size=%zu", __label, __stage, __group_count,
               __work_group_size);
    if (__kt_trace_sync())
    {
        const double __begin = __kt_trace_seconds();
        __event.wait_and_throw();
        __kt_trace("%s stage=%u: done in %.3f s", __label, __stage, __kt_trace_seconds() - __begin);
    }
}

inline std::uint32_t
__kt_debug_max_groups(std::uint32_t __group_count)
{
    const char* __value = std::getenv("ONEDPL_KT_DEBUG_MAX_GROUPS");
    if (!__value)
        return __group_count;
    const unsigned long __cap = std::strtoul(__value, nullptr, 10);
    return __cap > 0 ? std::min<std::uint32_t>(__group_count, static_cast<std::uint32_t>(__cap)) : __group_count;
}

#else

template <typename... _Args>
inline void
__kt_trace(const char*, _Args&&...)
{
}

inline void
__kt_trace_device(const sycl::queue&)
{
}

inline void
__kt_trace_kernel_info(const sycl::kernel&, const sycl::queue&, const char*)
{
}

inline void
__kt_trace_submitted(const sycl::event&, const char*, std::uint32_t, std::size_t, std::size_t)
{
}

inline std::uint32_t
__kt_debug_max_groups(std::uint32_t __group_count)
{
    return __group_count;
}

#endif // _ONEDPL_KT_TRACE

} // namespace oneapi::dpl::experimental::kt::gpu::__impl

#endif // _ONEDPL_KT_DEBUG_TRACE_H
