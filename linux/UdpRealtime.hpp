#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef TD_HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#endif

namespace td {
// Elevate only the calling reader thread. The process needs no root privileges;
// RTKit supplies the privilege after checking its own desktop policy.
inline bool enableReaderRealtime() {
    constexpr int desiredPriority = 10;
    auto setTimeout = [](rlim_t maximum) {
        rlimit old{};
        if (getrlimit(RLIMIT_RTTIME, &old) != 0)
            return false;
        rlim_t hard = std::min(old.rlim_max, maximum), soft = std::min(old.rlim_cur, hard);
        if (soft < 10000) {
            errno = EINVAL;
            return false;
        }
        rlimit limit{soft, hard};
        return setrlimit(RLIMIT_RTTIME, &limit) == 0;
    };
    if (!setTimeout(200000)) {
        std::cerr << "[UDPRealtime] RTTIME limit unavailable: " << strerror(errno)
                  << "; using normal event receive\n";
        return false;
    }
    sched_param parameters{};
    parameters.sched_priority = desiredPriority;
    const char *source = "native";
    int result = sched_setscheduler(0, SCHED_RR | SCHED_RESET_ON_FORK, &parameters);
    if (result != 0) {
#ifdef TD_HAVE_SYSTEMD
        struct Bus {
            sd_bus *value = nullptr;

            ~Bus() {
                sd_bus_unref(value);
            }
        } bus;

        struct Error {
            sd_bus_error value = SD_BUS_ERROR_NULL;

            ~Error() {
                sd_bus_error_free(&value);
            }
        } error;

        int r = sd_bus_open_system(&bus.value);
        if (r < 0) {
            std::cerr << "[UDPRealtime] system bus unavailable: " << strerror(-r)
                      << "; using normal event receive\n";
            return false;
        }
        sd_bus_set_method_call_timeout(bus.value, 2000000);
        constexpr const char *service = "org.freedesktop.RealtimeKit1";
        constexpr const char *path = "/org/freedesktop/RealtimeKit1";
        constexpr const char *interface = "org.freedesktop.RealtimeKit1";
        int32_t maximumPriority = 0;
        int64_t timeout = 0;
        r = sd_bus_get_property_trivial(bus.value, service, path, interface, "MaxRealtimePriority",
                                        &error.value, 'i', &maximumPriority);
        if (r >= 0)
            r = sd_bus_get_property_trivial(bus.value, service, path, interface, "RTTimeUSecMax",
                                            &error.value, 'x', &timeout);
        if (r < 0 || maximumPriority < 1 || timeout <= 0) {
            std::cerr << "[UDPRealtime] RTKit limits unavailable: "
                      << (error.value.message ? error.value.message : "invalid limits")
                      << "; using normal event receive\n";
            return false;
        }
        if (!setTimeout(rlim_t(timeout))) {
            std::cerr << "[UDPRealtime] RTKit RTTIME limit failed: " << strerror(errno)
                      << "; using normal event receive\n";
            return false;
        }
        // RESET_ON_FORK may be set on an ordinary thread without elevating it.
        sched_param current{};
        int policy = sched_getscheduler(0);
        if (policy < 0 || sched_getparam(0, &current) != 0 ||
            sched_setscheduler(0, policy | SCHED_RESET_ON_FORK, &current) != 0) {
            std::cerr << "[UDPRealtime] reset-on-fork unavailable: " << strerror(errno)
                      << "; using normal event receive\n";
            return false;
        }
        uint32_t priority = uint32_t(std::min(desiredPriority, int(maximumPriority)));
        r = sd_bus_call_method(bus.value, service, path, interface, "MakeThreadRealtime",
                               &error.value, nullptr, "tu", uint64_t(syscall(SYS_gettid)),
                               priority);
        if (r < 0) {
            std::cerr << "[UDPRealtime] RTKit request denied: "
                      << (error.value.message ? error.value.message : strerror(-r))
                      << "; using normal event receive\n";
            return false;
        }
        source = "rtkit";
#else
        std::cerr
            << "[UDPRealtime] native request denied and RTKit support not built; using normal event receive\n";
        return false;
#endif
    }
    sched_param actual{};
    int policy = sched_getscheduler(0);
    bool enabled = policy >= 0 && (policy & ~SCHED_RESET_ON_FORK) == SCHED_RR &&
                   sched_getparam(0, &actual) == 0 && actual.sched_priority > 0;
    std::cerr << "[UDPRealtime] source=" << source << " enabled=" << enabled << " policy=" << policy
              << " priority=" << actual.sched_priority << '\n';
    return enabled;
}
}
