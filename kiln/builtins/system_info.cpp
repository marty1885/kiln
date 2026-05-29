#include "registry.hpp"
#include "../interperter.hpp"
#include "../command_parser.hpp"
#include "../container_utils.hpp"
#include "../parse_number.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <set>
#include <unistd.h>
#include <sys/utsname.h>

namespace kiln {

namespace {

// Read entire file into string, empty on failure
std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Parse /proc/cpuinfo into a map of first-occurrence fields
std::unordered_map<std::string, std::string> parse_cpuinfo() {
    std::unordered_map<std::string, std::string> info;
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // trim
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        if (!info.contains(key)) { info[key] = val; }
    }
    return info;
}

// Parse /proc/meminfo values (in kB) into a map
std::unordered_map<std::string, long long> parse_meminfo() {
    std::unordered_map<std::string, long long> info;
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // trim and parse number
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        auto v = parse_number<long long>(val);
        if (v) info[key] = *v; // value is in kB
    }
    return info;
}

// Parse /etc/os-release into key=value map
std::unordered_map<std::string, std::string> parse_os_release() {
    std::unordered_map<std::string, std::string> info;
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Remove quotes
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') { val = val.substr(1, val.size() - 2); }
        info[key] = val;
    }
    return info;
}

// Count physical cores by counting unique "core id" entries per "physical id"
int count_physical_cores() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    std::string phys_id;
    std::set<std::string> seen;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        if (key == "physical id") {
            phys_id = val;
        } else if (key == "core id") {
            seen.insert(phys_id + ":" + val);
        }
    }
    return seen.empty() ? 1 : static_cast<int>(seen.size());
}

bool has_cpu_flag(const std::string& flags, const std::string& flag) {
    // flags is space-separated
    std::istringstream ss(flags);
    std::string f;
    while (ss >> f) {
        if (f == flag) return true;
    }
    return false;
}

std::string query_system_info(const std::string& key) {
    // CPU info
    if (key == "NUMBER_OF_LOGICAL_CORES") {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        return n > 0 ? std::to_string(n) : "1";
    }
    if (key == "NUMBER_OF_PHYSICAL_CORES") { return std::to_string(count_physical_cores()); }
    if (key == "IS_64BIT") { return sizeof(void*) == 8 ? "1" : "0"; }
    if (key == "PROCESSOR_NAME" || key == "PROCESSOR_DESCRIPTION") {
        auto info = parse_cpuinfo();
        if (auto it = info.find("model name"); it != info.end()) return it->second;
        return "";
    }
    if (key == "PROCESSOR_SERIAL_NUMBER") return "";
    if (key == "HAS_SERIAL_NUMBER") return "0";

    // Network
    if (key == "HOSTNAME") {
        char buf[256];
        if (gethostname(buf, sizeof(buf)) == 0) return buf;
        return "";
    }
    if (key == "FQDN") {
        char buf[256];
        if (gethostname(buf, sizeof(buf)) == 0) return buf;
        return "";
    }

    // Memory (CMake reports in MiB)
    if (key == "TOTAL_PHYSICAL_MEMORY") {
        auto info = parse_meminfo();
        if (auto it = info.find("MemTotal"); it != info.end()) return std::to_string(it->second / 1024);
        return "0";
    }
    if (key == "AVAILABLE_PHYSICAL_MEMORY") {
        auto info = parse_meminfo();
        if (auto it = info.find("MemAvailable"); it != info.end()) return std::to_string(it->second / 1024);
        return "0";
    }
    if (key == "TOTAL_VIRTUAL_MEMORY") {
        auto info = parse_meminfo();
        long long total = 0;
        if (auto it = info.find("MemTotal"); it != info.end()) total += it->second;
        if (auto it = info.find("SwapTotal"); it != info.end()) total += it->second;
        return std::to_string(total / 1024);
    }
    if (key == "AVAILABLE_VIRTUAL_MEMORY") {
        auto info = parse_meminfo();
        long long avail = 0;
        if (auto it = info.find("MemAvailable"); it != info.end()) avail += it->second;
        if (auto it = info.find("SwapFree"); it != info.end()) avail += it->second;
        return std::to_string(avail / 1024);
    }

    // OS info via uname
    if (key == "OS_NAME") {
        struct utsname u;
        if (uname(&u) == 0) return u.sysname;
        return "Linux";
    }
    if (key == "OS_RELEASE") {
        struct utsname u;
        if (uname(&u) == 0) return u.release;
        return "";
    }
    if (key == "OS_VERSION") {
        struct utsname u;
        if (uname(&u) == 0) return u.version;
        return "";
    }
    if (key == "OS_PLATFORM") {
        struct utsname u;
        if (uname(&u) == 0) return u.machine;
        return "";
    }

    // Linux distro info
    if (key == "DISTRIB_INFO") {
        auto info = parse_os_release();
        // Return semicolon-separated key=value pairs
        return join(info, ";", [](const auto& p) { return p.first + "=" + p.second; });
    }
    // DISTRIB_<name> queries specific os-release keys (e.g., DISTRIB_ID → ID)
    if (key.starts_with("DISTRIB_")) {
        auto os_key = key.substr(8); // strip "DISTRIB_"
        auto info = parse_os_release();
        if (auto it = info.find(os_key); it != info.end()) return it->second;
        return "";
    }

    // CPU feature flags
    if (key.starts_with("HAS_")) {
        auto info = parse_cpuinfo();
        std::string flags;
        if (auto it = info.find("flags"); it != info.end()) flags = it->second;

        // Map CMake key names to /proc/cpuinfo flag names
        static const std::unordered_map<std::string, std::string> flag_map = {
            {"HAS_FPU", "fpu"},       {"HAS_MMX", "mmx"},         {"HAS_MMX_PLUS", "mmxext"},
            {"HAS_SSE", "sse"},       {"HAS_SSE2", "sse2"},       {"HAS_SSE_FP", "sse"},
            {"HAS_SSE_MMX", "sse"},   {"HAS_AMD_3DNOW", "3dnow"}, {"HAS_AMD_3DNOW_PLUS", "3dnowext"},
            {"HAS_IA64", "ia64"},     {"HAS_SSE3", "pni"},        {"HAS_SSSE3", "ssse3"},
            {"HAS_SSE4_1", "sse4_1"}, {"HAS_SSE4_2", "sse4_2"},
        };

        if (auto it = flag_map.find(key); it != flag_map.end()) { return has_cpu_flag(flags, it->second) ? "1" : "0"; }
        // Unknown HAS_ key, default to 0
        return "0";
    }

    return "";
}

} // anonymous namespace

void register_system_info_builtins(Interpreter& interp) {
    interp.add_builtin("cmake_host_system_information", [](Interpreter& interp, const std::vector<std::string>& args) {
        CommandParser parser("cmake_host_system_information");
        std::string result_var;
        std::vector<std::string> queries;

        parser.value("RESULT", result_var);
        parser.list("QUERY", queries);

        PARSE_OR_RETURN(parser, interp, args);

        if (result_var.empty()) {
            interp.set_fatal_error("cmake_host_system_information() requires RESULT");
            return;
        }
        if (queries.empty()) {
            interp.set_fatal_error("cmake_host_system_information() requires QUERY");
            return;
        }

        interp.set_variable(result_var, join(queries, ";", [](const auto& q) { return query_system_info(q); }));
    });
}

} // namespace kiln
