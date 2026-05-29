#include "module_scanner.hpp"
#include <fstream>
#include <sstream>
#include <glaze/glaze.hpp>

namespace kiln {

std::string generate_module_mapper_content(const std::vector<ModuleMapEntry>& entries) {
    // GCC module mapper format (one entry per line):
    // ModuleName BMIPath
    // The mapper is used with -fmodule-mapper=<file>
    std::ostringstream oss;
    for (const auto& entry : entries) { oss << entry.module_name << " " << entry.bmi_path << "\n"; }
    return oss.str();
}

std::expected<std::vector<ModuleMapEntry>, std::string> parse_module_mapper_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) { return std::unexpected("Failed to open module mapper file: " + path); }

    std::vector<ModuleMapEntry> entries;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        ModuleMapEntry entry;
        if (iss >> entry.module_name >> entry.bmi_path) { entries.push_back(entry); }
    }

    return entries;
}

std::string get_bmi_path(const std::string& binary_dir, const std::string& module_name) {
    // Replace : with - for partition names (ModuleName:Partition -> ModuleName-Partition.gcm)
    std::string safe_name = module_name;
    std::replace(safe_name.begin(), safe_name.end(), ':', '-');

    std::filesystem::path path = std::filesystem::path(binary_dir) / "bmis" / (safe_name + ".gcm");
    return path.lexically_normal().string();
}

std::string get_header_unit_bmi_path(const std::string& binary_dir, const std::string& source_path) {
    // Mangle the absolute source path into a flat filename. Replace path
    // separators and other path-meaningful punctuation with '_' so the BMI
    // sits at a single, deterministic location under bmis/header_units/.
    std::string mangled;
    mangled.reserve(source_path.size());
    for (char c : source_path) {
        if (c == '/' || c == '\\' || c == ':')
            mangled.push_back('_');
        else
            mangled.push_back(c);
    }
    std::filesystem::path path = std::filesystem::path(binary_dir) / "bmis" / "header_units" / (mangled + ".gcm");
    return path.lexically_normal().string();
}

std::string get_ddi_path(const std::string& binary_dir, const std::string& source_path) {
    std::filesystem::path src(source_path);
    std::filesystem::path ddi_suffix;

    if (src.is_absolute()) {
        ddi_suffix = src.filename();
    } else {
        ddi_suffix = src;
    }

    std::filesystem::path ddi = std::filesystem::path(binary_dir) / "ddi" / ddi_suffix;
    ddi += ".ddi";
    return ddi.lexically_normal().string();
}

} // namespace kiln

template <> struct glz::meta<kiln::P1689Provide> {
    using T = kiln::P1689Provide;
    static constexpr auto value = glz::object("logical-name", &T::logical_name, "source-path", &T::source_path, "compiled-module-path",
                                              &T::compiled_module_path, "is-interface", &T::is_interface);
};

template <> struct glz::meta<kiln::P1689Require> {
    using T = kiln::P1689Require;
    static constexpr auto value =
        glz::object("logical-name", &T::logical_name, "lookup-method", &T::lookup_method, "source-path", &T::source_path);
};

template <> struct glz::meta<kiln::ModuleManifestEntry> {
    using T = kiln::ModuleManifestEntry;
    static constexpr auto value = glz::object("logical-name", &T::logical_name, "bmi-path", &T::bmi_path, "primary-output",
                                              &T::primary_output, "source-path", &T::source_path, "visibility", &T::visibility);
};

template <> struct glz::meta<kiln::ModuleManifest> {
    using T = kiln::ModuleManifest;
    static constexpr auto value = glz::object("entries", &T::entries);
};

template <> struct glz::meta<kiln::LibstdcxxModuleEntry> {
    using T = kiln::LibstdcxxModuleEntry;
    static constexpr auto value =
        glz::object("logical-name", &T::logical_name, "source-path", &T::source_path, "is-std-library", &T::is_std_library);
};

template <> struct glz::meta<kiln::LibstdcxxModulesJson> {
    using T = kiln::LibstdcxxModulesJson;
    static constexpr auto value = glz::object("version", &T::version, "revision", &T::revision, "modules", &T::modules);
};

template <> struct glz::meta<kiln::P1689Rule> {
    using T = kiln::P1689Rule;
    static constexpr auto value = glz::object("primary-output", &T::primary_output, "provides", &T::provides, "requires", &T::requires_);
};

namespace kiln {

std::expected<P1689File, std::string> parse_p1689_string(const std::string& json) {
    P1689File file;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(file, json);
    if (ec) { return std::unexpected("Failed to parse P1689 JSON: " + glz::format_error(ec, json)); }
    return file;
}

std::expected<P1689File, std::string> parse_p1689_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::unexpected("Failed to open P1689 file: " + path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto result = parse_p1689_string(content);
    if (!result) return std::unexpected(result.error() + " (file: " + path + ")");
    return result;
}

std::expected<LibstdcxxModulesJson, std::string> parse_libstdcxx_modules_json_string(const std::string& json) {
    LibstdcxxModulesJson j;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(j, json);
    if (ec) { return std::unexpected("Failed to parse libstdc++.modules.json: " + glz::format_error(ec, json)); }
    return j;
}

std::expected<LibstdcxxModulesJson, std::string> parse_libstdcxx_modules_json_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::unexpected("Failed to open libstdc++.modules.json: " + path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto r = parse_libstdcxx_modules_json_string(content);
    if (!r) return std::unexpected(r.error() + " (file: " + path + ")");
    return r;
}

std::expected<ModuleManifest, std::string> parse_module_manifest_string(const std::string& json) {
    ModuleManifest m;
    auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(m, json);
    if (ec) { return std::unexpected("Failed to parse module manifest: " + glz::format_error(ec, json)); }
    return m;
}

std::expected<ModuleManifest, std::string> read_module_manifest(const std::string& path) {
    std::ifstream f(path);
    if (!f) return std::unexpected("Failed to open module manifest: " + path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto r = parse_module_manifest_string(content);
    if (!r) return std::unexpected(r.error() + " (file: " + path + ")");
    return r;
}

std::expected<void, std::string> write_module_manifest(const std::string& path, const ModuleManifest& manifest) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) return std::unexpected("Failed to create directory for manifest: " + ec.message());

    std::string json;
    auto write_ec = glz::write<glz::opts{.prettify = true}>(manifest, json);
    if (write_ec) return std::unexpected("Failed to serialize manifest: " + glz::format_error(write_ec));

    std::ofstream f(path);
    if (!f) return std::unexpected("Failed to open manifest for writing: " + path);
    f << json;
    if (!f) return std::unexpected("Failed to write manifest: " + path);
    return {};
}

} // namespace kiln
