#pragma once
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace td {
// Use only on an owned pthread, never a libdispatch worker. A successful Mach
// policy change opts that pthread out of QoS permanently; fallback must run on
// a fresh pthread, not merely call pthread_set_qos_class_self_np on this one.
class SenderRealtime {
    mach_port_t port = pthread_mach_thread_np(pthread_self());
    mach_timebase_info_data_t base{};
    int realtimePriority = 0;
    bool changed = false;

    uint64_t ticksToNs(uint32_t ticks) const {
        return uint64_t(ticks) * base.numer / base.denom;
    }

    uint32_t nsToTicks(uint64_t ns) const {
        return uint32_t((__uint128_t(ns) * base.denom) / base.numer);
    }

  public:
    uint64_t periodNs = 0, budgetNs = 0, deadlineNs = 0;

    SenderRealtime() {
        mach_timebase_info(&base);
    }

    bool enable(int fps) {
        periodNs = 1000000000ULL / std::max(1, fps);
        budgetNs = std::min<uint64_t>(8000000, periodNs / 2);
        deadlineNs = std::min(periodNs * 3 / 4, budgetNs * 2);
        thread_time_constraint_policy_data_t requested{nsToTicks(periodNs), nsToTicks(budgetNs),
                                                       nsToTicks(deadlineNs), TRUE};
        kern_return_t result = thread_policy_set(port, THREAD_TIME_CONSTRAINT_POLICY,
                                                 reinterpret_cast<thread_policy_t>(&requested),
                                                 THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        changed = result == KERN_SUCCESS;
        thread_time_constraint_policy_data_t actual{};
        mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
        boolean_t defaults = FALSE;
        kern_return_t query =
            thread_policy_get(port, THREAD_TIME_CONSTRAINT_POLICY,
                              reinterpret_cast<thread_policy_t>(&actual), &count, &defaults);
        thread_extended_info_data_t info{};
        count = THREAD_EXTENDED_INFO_COUNT;
        kern_return_t detail =
            thread_info(port, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &count);
        bool enabled = changed && query == KERN_SUCCESS && !defaults && detail == KERN_SUCCESS &&
                       info.pth_policy == POLICY_RR;
        if (enabled) {
            periodNs = ticksToNs(actual.period);
            budgetNs = ticksToNs(actual.computation);
            deadlineNs = ticksToNs(actual.constraint);
            realtimePriority = info.pth_priority;
        }
        uint64_t tid = 0;
        pthread_threadid_np(nullptr, &tid);
        fprintf(
            stderr,
            "[SenderSchedule] request=mach_realtime enabled=%d set_result=%d query_result=%d info_result=%d default_policy=%d tid=%llu policy=%d base_priority=%d period_ms=%.3f budget_ms=%.3f deadline_ms=%.3f\n",
            enabled, result, query, detail, defaults, (unsigned long long)tid, info.pth_policy,
            info.pth_priority, periodNs / 1e6, budgetNs / 1e6, deadlineNs / 1e6);
        return enabled;
    }

    bool stillRealtime() {
        // TIME_CONSTRAINT_POLICY may return a saved policy after kernel demotion.
        // Check the active scheduling mode and base priority as well.
        thread_extended_info_data_t info{};
        mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
        return thread_info(port, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info),
                           &count) == KERN_SUCCESS &&
               info.pth_policy == POLICY_RR && info.pth_priority >= realtimePriority;
    }

    ~SenderRealtime() {
        if (changed) {
            kern_return_t result = thread_policy_set(port, THREAD_STANDARD_POLICY, nullptr, 0);
            if (result != KERN_SUCCESS)
                fprintf(stderr, "[SenderSchedule] restore_standard_failed=%d\n", result);
        }
    }
};
}
