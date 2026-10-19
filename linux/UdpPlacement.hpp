#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace td {
inline std::string readPlacementFile(const std::filesystem::path &path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

inline void addPlacementCPUs(const std::string &text, cpu_set_t &set) {
    std::istringstream list(text);
    std::string part;
    while (std::getline(list, part, ',')) {
        std::istringstream item(part);
        int first = -1, last = -1;
        if (!(item >> first) || first < 0 || first >= CPU_SETSIZE)
            continue;
        last = first;
        char separator;
        if (item >> separator) {
            if (separator != '-' || !(item >> last))
                continue;
        }
        for (int cpu = first; cpu <= last && cpu < CPU_SETSIZE; cpu++)
            CPU_SET(cpu, &set);
    }
}

// Keep a fast socket reader off the Thunderbolt controller's IRQ cores,
// including their SMT siblings. No IRQ, system-wide affinity or scheduling
// policy is changed. Other interfaces retain the normal scheduler placement.
inline int placeThunderboltReader(const sockaddr_in &peer, std::string &interface) {
    int route = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (route < 0)
        return -1;
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    bool found = ::connect(route, reinterpret_cast<const sockaddr *>(&peer), sizeof(peer)) == 0 &&
                 getsockname(route, reinterpret_cast<sockaddr *>(&local), &length) == 0;
    close(route);
    if (!found)
        return -1;
    ifaddrs *addresses = nullptr;
    if (getifaddrs(&addresses) != 0)
        return -1;
    for (auto *a = addresses; a; a = a->ifa_next)
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET &&
            reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr.s_addr ==
                local.sin_addr.s_addr) {
            interface = a->ifa_name;
            break;
        }
    freeifaddrs(addresses);
    if (interface.empty())
        return -1;
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path net = fs::path("/sys/class/net") / interface;
    auto driver = fs::canonical(net / "device/driver", error);
    if (error || driver.filename() != "thunderbolt-net")
        return -1;
    auto parent = fs::canonical(net / "device", error);
    if (error)
        return -1;
    fs::path irqs;
    while (!parent.empty() && parent != parent.root_path()) {
        if (fs::is_directory(parent / "msi_irqs", error)) {
            irqs = parent / "msi_irqs";
            break;
        }
        error.clear();
        parent = parent.parent_path();
    }
    if (irqs.empty())
        return -1;
    cpu_set_t avoid, allowed;
    CPU_ZERO(&avoid);
    CPU_ZERO(&allowed);
    if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) != 0)
        return -1;
    std::set<std::string> activeIRQs;
    std::ifstream interrupts("/proc/interrupts");
    std::string line;
    while (std::getline(interrupts, line)) {
        std::istringstream row(line);
        std::string number;
        if (row >> number && !number.empty() && number.back() == ':') {
            number.pop_back();
            activeIRQs.insert(number);
        }
    }
    for (fs::directory_iterator it(irqs, error), end; !error && it != end; it.increment(error)) {
        if (!activeIRQs.count(it->path().filename().string()))
            continue;
        fs::path proc = fs::path("/proc/irq") / it->path().filename();
        addPlacementCPUs(readPlacementFile(proc / "effective_affinity_list"), avoid);
    }
    if (!CPU_COUNT(&avoid))
        return -1;
    cpu_set_t siblings = avoid;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &avoid))
            addPlacementCPUs(readPlacementFile(fs::path("/sys/devices/system/cpu") /
                                               ("cpu" + std::to_string(cpu)) /
                                               "topology/thread_siblings_list"),
                             siblings);
    int selected = -1;
    uint64_t bestFrequency = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &allowed) && !CPU_ISSET(cpu, &siblings)) {
            uint64_t frequency = 0;
            std::istringstream(readPlacementFile(fs::path("/sys/devices/system/cpu") /
                                                 ("cpu" + std::to_string(cpu)) /
                                                 "cpufreq/cpuinfo_max_freq")) >>
                frequency;
            if (selected < 0 || frequency > bestFrequency) {
                selected = cpu;
                bestFrequency = frequency;
            }
        }
    if (selected < 0)
        return -1;
    cpu_set_t target;
    CPU_ZERO(&target);
    CPU_SET(selected, &target);
    return pthread_setaffinity_np(pthread_self(), sizeof(target), &target) == 0 ? selected : -1;
}
}
