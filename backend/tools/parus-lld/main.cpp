// backend/tools/parus-lld/main.cpp
#include <parus/backend/parlib/Parlib.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

    using parus::backend::parlib::ParlibChunkKind;
    using parus::backend::parlib::ParlibLane;
    using parus::backend::parlib::ParlibNativeDepMode;
    using parus::backend::parlib::ParlibReader;

    struct DriverOptions {
        std::vector<std::string> inputs{};
        std::vector<std::string> passthrough_args{};
        std::string output_path{};
        std::string target_triple{};
        std::string sysroot_path{};
        std::string apple_sdk_root{};
        std::string backend_override{};
        uint64_t expected_toolchain_hash = 0;
        uint64_t expected_target_hash = 0;
        bool verbose = false;
    };

    struct LinkPlan {
        std::vector<std::string> object_inputs{};
        std::vector<std::string> native_args{};
        std::vector<std::filesystem::path> temp_files{};
    };

    struct TempWorkspace {
        std::filesystem::path root{};
        bool keep = false;
        ~TempWorkspace() {
            if (keep || root.empty()) return;
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
    };

    std::string getenv_string_(const char* key) {
        if (key == nullptr) return {};
        const char* p = std::getenv(key);
        if (p == nullptr) return {};
        return std::string(p);
    }

    bool has_suffix_(std::string_view s, std::string_view suffix) {
        if (suffix.size() > s.size()) return false;
        return s.substr(s.size() - suffix.size()) == suffix;
    }

    std::optional<uint64_t> parse_u64_(const std::string& s) {
        if (s.empty()) return std::nullopt;
        try {
            size_t idx = 0;
            const uint64_t v = std::stoull(s, &idx, 0);
            if (idx != s.size()) return std::nullopt;
            return v;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::string trim_(std::string_view s) {
        size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        size_t e = s.size();
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return std::string(s.substr(b, e - b));
    }

    std::vector<std::string> split_ws_(std::string_view s) {
        std::vector<std::string> out;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i >= s.size()) break;
            size_t j = i + 1;
            while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
            out.emplace_back(s.substr(i, j - i));
            i = j;
        }
        return out;
    }

    bool read_text_file_(const std::filesystem::path& p, std::string& out) {
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs.is_open()) return false;
        out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        return true;
    }

    bool write_binary_file_(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        if (!bytes.empty()) {
            ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        return ofs.good();
    }

    std::optional<std::string> parse_json_field_(const std::string& json, std::string_view key) {
        const std::string needle = "\"" + std::string(key) + "\"";
        const size_t at = json.find(needle);
        if (at == std::string::npos) return std::nullopt;
        const size_t colon = json.find(':', at + needle.size());
        if (colon == std::string::npos) return std::nullopt;
        size_t i = colon + 1;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size()) return std::nullopt;

        if (json[i] == '"') {
            ++i;
            size_t j = i;
            while (j < json.size() && json[j] != '"') ++j;
            if (j >= json.size()) return std::nullopt;
            return std::string(json.substr(i, j - i));
        }

        size_t j = i;
        while (j < json.size() && json[j] != ',' && json[j] != '}' && json[j] != '\n') ++j;
        return trim_(std::string_view(json).substr(i, j - i));
    }

    std::optional<uint64_t> parse_json_u64_field_(const std::string& json, std::string_view key) {
        const auto s = parse_json_field_(json, key);
        if (!s.has_value()) return std::nullopt;
        return parse_u64_(*s);
    }

    std::optional<std::string> parse_json_string_field_(const std::string& json, std::string_view key) {
        return parse_json_field_(json, key);
    }

    int run_argv_(const std::vector<std::string>& argv) {
        if (argv.empty()) return 1;
#if defined(_WIN32)
        std::vector<const char*> cargs;
        cargs.reserve(argv.size() + 1);
        for (const auto& a : argv) cargs.push_back(a.c_str());
        cargs.push_back(nullptr);
        const int rc = _spawnvp(_P_WAIT, argv[0].c_str(), cargs.data());
        if (rc < 0) return 1;
        return rc;
#else
        std::vector<char*> cargs;
        cargs.reserve(argv.size() + 1);
        for (const auto& a : argv) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);

        pid_t pid = -1;
        const int sp = posix_spawnp(&pid, argv[0].c_str(), nullptr, nullptr, cargs.data(), environ);
        if (sp != 0) return 1;

        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return 1;
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return 1;
#endif
    }

#if !defined(_WIN32)
    bool run_argv_capture_stdout_(const std::vector<std::string>& argv, std::string& out, int& exit_code) {
        out.clear();
        exit_code = 1;
        if (argv.empty()) return false;

        int pipefd[2];
        if (pipe(pipefd) != 0) return false;

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        std::vector<char*> cargs;
        cargs.reserve(argv.size() + 1);
        for (const auto& a : argv) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);

        pid_t pid = -1;
        const int sp = posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, cargs.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[1]);
        if (sp != 0) {
            close(pipefd[0]);
            return false;
        }

        std::string buf;
        buf.resize(4096);
        while (true) {
            const ssize_t n = read(pipefd[0], buf.data(), buf.size());
            if (n <= 0) break;
            out.append(buf.data(), static_cast<size_t>(n));
        }
        close(pipefd[0]);

        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return false;
        if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
        else exit_code = 1;
        return true;
    }
#endif

    std::string current_executable_path_(char** argv) {
        if (argv == nullptr || argv[0] == nullptr) return {};
        std::error_code ec;
        const auto p = std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), ec);
        if (!ec && !p.empty()) return p.string();
        return std::filesystem::path(argv[0]).string();
    }

    std::string default_backend_name_() {
#if defined(_WIN32)
        return "lld-link";
#elif defined(__APPLE__)
        return "ld64.lld";
#else
        return "ld.lld";
#endif
    }

    std::string resolve_backend_linker_path_(const DriverOptions& opt, char** argv) {
        namespace fs = std::filesystem;
        if (!opt.backend_override.empty()) return opt.backend_override;

        const std::string name = default_backend_name_();
        const std::string toolchain_root = getenv_string_("PARUS_TOOLCHAIN_ROOT");
        if (!toolchain_root.empty()) {
            const fs::path p = fs::path(toolchain_root) / "bin" / name;
            std::error_code ec;
            if (fs::exists(p, ec) && !ec) return p.string();
        }

        const std::string self = current_executable_path_(argv);
        if (!self.empty()) {
            const fs::path p = fs::path(self).parent_path() / name;
            std::error_code ec;
            if (fs::exists(p, ec) && !ec) return p.string();
        }

        return name;
    }

    std::string resolve_sysroot_(const DriverOptions& opt) {
        if (!opt.sysroot_path.empty()) return opt.sysroot_path;
        return getenv_string_("PARUS_SYSROOT");
    }

    std::string resolve_target_triple_(const DriverOptions& opt, const std::string& sysroot) {
        if (!opt.target_triple.empty()) return opt.target_triple;

        if (!sysroot.empty()) {
            const auto m = std::filesystem::path(sysroot) / "manifest.json";
            std::string text;
            if (read_text_file_(m, text)) {
                if (const auto s = parse_json_string_field_(text, "default_target_triple"); s.has_value()) {
                    return *s;
                }
            }
        }
        return {};
    }

    std::string resolve_apple_sdk_root_(const DriverOptions& opt) {
        if (!opt.apple_sdk_root.empty()) return opt.apple_sdk_root;
        const std::string env_sdk = getenv_string_("SDKROOT");
        if (!env_sdk.empty()) return env_sdk;
#if defined(__APPLE__) && !defined(_WIN32)
        int rc = 1;
        std::string out;
        if (run_argv_capture_stdout_({"xcrun", "--sdk", "macosx", "--show-sdk-path"}, out, rc) && rc == 0) {
            const std::string t = trim_(out);
            if (!t.empty()) return t;
        }
#endif
        return {};
    }

    std::optional<uint64_t> resolve_hash_from_manifest_(
        const std::filesystem::path& path,
        std::string_view key
    ) {
        std::string text;
        if (!read_text_file_(path, text)) return std::nullopt;
        return parse_json_u64_field_(text, key);
    }

    uint64_t resolve_expected_toolchain_hash_(const DriverOptions& opt, const std::string& sysroot) {
        if (opt.expected_toolchain_hash != 0) return opt.expected_toolchain_hash;
        const auto from_env = parse_u64_(getenv_string_("PARUS_EXPECTED_TOOLCHAIN_HASH"));
        if (from_env.has_value()) return *from_env;
        if (sysroot.empty()) return 0;
        const auto manifest = std::filesystem::path(sysroot) / "manifest.json";
        const auto from_manifest = resolve_hash_from_manifest_(manifest, "toolchain_hash");
        return from_manifest.value_or(0);
    }

    uint64_t resolve_expected_target_hash_(
        const DriverOptions& opt,
        const std::string& sysroot,
        const std::string& target
    ) {
        if (opt.expected_target_hash != 0) return opt.expected_target_hash;
        const auto from_env = parse_u64_(getenv_string_("PARUS_EXPECTED_TARGET_HASH"));
        if (from_env.has_value()) return *from_env;
        if (sysroot.empty() || target.empty()) return 0;
        const auto manifest = std::filesystem::path(sysroot) / "targets" / target / "manifest.json";
        const auto from_manifest = resolve_hash_from_manifest_(manifest, "target_hash");
        return from_manifest.value_or(0);
    }

    std::optional<ParlibLane> select_lane_for_object_(const ParlibReader& reader) {
        if (reader.find_chunk(ParlibChunkKind::kObjectArchive, ParlibLane::kPcore, 0).has_value()) {
            return ParlibLane::kPcore;
        }
        if (reader.find_chunk(ParlibChunkKind::kObjectArchive, ParlibLane::kPrt, 0).has_value()) {
            return ParlibLane::kPrt;
        }
        if (reader.find_chunk(ParlibChunkKind::kObjectArchive, ParlibLane::kPstd, 0).has_value()) {
            return ParlibLane::kPstd;
        }
        for (const auto& c : reader.list_chunks()) {
            if (c.kind == ParlibChunkKind::kObjectArchive) return c.lane;
        }
        return std::nullopt;
    }

    bool append_native_dep_reference_args_(
        const parus::backend::parlib::ParlibNativeDepEntry& dep,
        std::vector<std::string>& native_args
    ) {
        using Kind = parus::backend::parlib::ParlibNativeDepKind;
        if (dep.kind == Kind::kFramework) {
            native_args.push_back("-framework");
            native_args.push_back(dep.reference.empty() ? dep.name : dep.reference);
            return true;
        }

        const std::string ref = dep.reference.empty() ? ("-l" + dep.name) : dep.reference;
        const auto toks = split_ws_(ref);
        if (toks.empty()) return false;
        native_args.insert(native_args.end(), toks.begin(), toks.end());
        return true;
    }

    bool plan_inputs_(
        const DriverOptions& opt,
        const std::string& resolved_target,
        uint64_t expected_toolchain_hash,
        uint64_t expected_target_hash,
        TempWorkspace& temp,
        LinkPlan& out_plan
    ) {
        namespace fs = std::filesystem;

        for (const auto& in : opt.inputs) {
            if (!has_suffix_(in, ".parlib")) {
                out_plan.object_inputs.push_back(in);
                continue;
            }

            auto reader_opt = ParlibReader::open(in);
            if (!reader_opt.has_value()) {
                std::cerr << "parus-lld: failed to open parlib: " << in << "\n";
                return false;
            }
            const auto& reader = *reader_opt;
            const auto& header = reader.read_header();

            if (expected_toolchain_hash != 0 && header.compiler_hash != expected_toolchain_hash) {
                std::cerr
                    << "parus-lld: toolchain hash mismatch for parlib '" << in << "'"
                    << " (expected=" << expected_toolchain_hash
                    << ", got=" << header.compiler_hash << ")\n";
                return false;
            }

            if (expected_target_hash != 0 && header.feature_bits != expected_target_hash) {
                std::cerr
                    << "parus-lld: target hash mismatch for parlib '" << in << "'"
                    << " (expected=" << expected_target_hash
                    << ", got=" << header.feature_bits << ")\n";
                return false;
            }

            if (!resolved_target.empty() && !header.target_triple.empty() && header.target_triple != resolved_target) {
                std::cerr
                    << "parus-lld: target triple mismatch for parlib '" << in << "'"
                    << " (expected='" << resolved_target
                    << "', got='" << header.target_triple << "')\n";
                return false;
            }

            const auto lane_opt = select_lane_for_object_(reader);
            if (!lane_opt.has_value()) {
                std::cerr << "parus-lld: ObjectArchive chunk not found in parlib: " << in << "\n";
                return false;
            }

            auto rec = reader.find_chunk(ParlibChunkKind::kObjectArchive, *lane_opt, 0);
            if (!rec.has_value()) {
                for (const auto& c : reader.list_chunks()) {
                    if (c.kind == ParlibChunkKind::kObjectArchive && c.lane == *lane_opt) {
                        rec = c;
                        break;
                    }
                }
            }
            if (!rec.has_value()) {
                std::cerr << "parus-lld: failed to select object chunk in parlib: " << in << "\n";
                return false;
            }

            const auto bytes = reader.read_chunk_slice(*rec, 0, rec->size);
            if (bytes.size() != rec->size) {
                std::cerr << "parus-lld: failed to read object payload from parlib: " << in << "\n";
                return false;
            }

            const fs::path obj_out = temp.root / (fs::path(in).stem().string() + ".from.parlib.o");
            if (!write_binary_file_(obj_out, bytes)) {
                std::cerr << "parus-lld: failed to materialize object from parlib: " << obj_out.string() << "\n";
                return false;
            }
            out_plan.object_inputs.push_back(obj_out.string());
            out_plan.temp_files.push_back(obj_out);

            auto native_deps = reader.read_native_deps();
            std::stable_sort(native_deps.begin(), native_deps.end(), [](const auto& a, const auto& b) {
                return a.link_order < b.link_order;
            });
            for (const auto& dep : native_deps) {
                if (dep.mode == ParlibNativeDepMode::kReference) {
                    if (!append_native_dep_reference_args_(dep, out_plan.native_args)) {
                        if (dep.required) {
                            std::cerr << "parus-lld: invalid NativeDeps reference entry: " << dep.name << "\n";
                            return false;
                        }
                    }
                    continue;
                }

                // embed mode: payload chunk target_id를 link_order로 매핑한다.
                const auto native_chunk = reader.find_chunk(
                    ParlibChunkKind::kNativeArchivePayload,
                    ParlibLane::kGlobal,
                    dep.link_order
                );
                if (!native_chunk.has_value()) {
                    if (dep.required) {
                        std::cerr
                            << "parus-lld: NativeArchivePayload missing for embed dep: "
                            << dep.name << "\n";
                        return false;
                    }
                    continue;
                }

                const auto archive_bytes = reader.read_chunk_slice(*native_chunk, 0, native_chunk->size);
                if (archive_bytes.size() != native_chunk->size) {
                    std::cerr << "parus-lld: failed to read embedded native payload: " << dep.name << "\n";
                    return false;
                }

                const fs::path arc_out = temp.root /
                    ("native_" + std::to_string(dep.link_order) + "_" + dep.name + ".a");
                if (!write_binary_file_(arc_out, archive_bytes)) {
                    std::cerr << "parus-lld: failed to materialize embedded archive: " << arc_out.string() << "\n";
                    return false;
                }
                out_plan.object_inputs.push_back(arc_out.string());
                out_plan.temp_files.push_back(arc_out);
            }
        }

        return true;
    }

    std::string infer_darwin_arch_(const std::string& triple) {
        if (triple.find("aarch64") != std::string::npos || triple.find("arm64") != std::string::npos) {
            return "arm64";
        }
        if (triple.find("x86_64") != std::string::npos || triple.find("amd64") != std::string::npos) {
            return "x86_64";
        }
#if defined(__aarch64__) || defined(__arm64__)
        return "arm64";
#else
        return "x86_64";
#endif
    }

    bool has_system_lib_flag_(const std::vector<std::string>& args) {
        for (const auto& a : args) {
            if (a == "-lSystem") return true;
        }
        return false;
    }

    std::vector<std::string> build_backend_argv_(
        const std::string& backend,
        const DriverOptions& opt,
        const LinkPlan& plan,
        const std::string& sdk_root,
        const std::string& resolved_target
    ) {
        std::vector<std::string> argv;
        argv.push_back(backend);

#if defined(__APPLE__)
        const std::string arch = infer_darwin_arch_(resolved_target);
        const std::string min_ver = getenv_string_("PARUS_DARWIN_MIN_VERSION").empty()
            ? "14.0"
            : getenv_string_("PARUS_DARWIN_MIN_VERSION");
        const std::string sdk_ver = getenv_string_("PARUS_DARWIN_SDK_VERSION").empty()
            ? min_ver
            : getenv_string_("PARUS_DARWIN_SDK_VERSION");

        argv.push_back("-arch");
        argv.push_back(arch);
        argv.push_back("-platform_version");
        argv.push_back("macos");
        argv.push_back(min_ver);
        argv.push_back(sdk_ver);

        if (!sdk_root.empty()) {
            argv.push_back("-syslibroot");
            argv.push_back(sdk_root);
            argv.push_back("-L" + (std::filesystem::path(sdk_root) / "usr/lib").string());
            const auto crt1 = (std::filesystem::path(sdk_root) / "usr/lib/crt1.o");
            std::error_code ec;
            if (std::filesystem::exists(crt1, ec) && !ec) {
                argv.push_back(crt1.string());
            }
        }
#endif

        for (const auto& in : plan.object_inputs) argv.push_back(in);
        for (const auto& a : plan.native_args) argv.push_back(a);
        for (const auto& a : opt.passthrough_args) argv.push_back(a);

#if defined(__APPLE__)
        if (!has_system_lib_flag_(argv)) {
            argv.push_back("-lSystem");
        }
#endif

        argv.push_back("-o");
        argv.push_back(opt.output_path);
        return argv;
    }

    void print_usage_() {
        std::cout
            << "parus-lld [options] <inputs...>\n"
            << "  -o <path>\n"
            << "  --target <triple>\n"
            << "  --sysroot <path>\n"
            << "  --apple-sdk-root <path>\n"
            << "  --toolchain-hash <u64>\n"
            << "  --target-hash <u64>\n"
            << "  --backend <path>\n"
            << "  --verbose\n";
    }

    bool parse_options_(int argc, char** argv, DriverOptions& out) {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto require_next = [&](const char* name) -> std::optional<std::string> {
                if (i + 1 >= argc) {
                    std::cerr << "parus-lld: " << name << " requires a value\n";
                    return std::nullopt;
                }
                ++i;
                return std::string(argv[i]);
            };

            if (a == "-h" || a == "--help") {
                print_usage_();
                return false;
            }
            if (a == "--verbose") {
                out.verbose = true;
                continue;
            }
            if (a == "-o") {
                const auto v = require_next("-o");
                if (!v.has_value()) return false;
                out.output_path = *v;
                continue;
            }
            if (a == "--target") {
                const auto v = require_next("--target");
                if (!v.has_value()) return false;
                out.target_triple = *v;
                continue;
            }
            if (a == "--sysroot") {
                const auto v = require_next("--sysroot");
                if (!v.has_value()) return false;
                out.sysroot_path = *v;
                continue;
            }
            if (a == "--apple-sdk-root") {
                const auto v = require_next("--apple-sdk-root");
                if (!v.has_value()) return false;
                out.apple_sdk_root = *v;
                continue;
            }
            if (a == "--backend") {
                const auto v = require_next("--backend");
                if (!v.has_value()) return false;
                out.backend_override = *v;
                continue;
            }
            if (a == "--toolchain-hash") {
                const auto v = require_next("--toolchain-hash");
                if (!v.has_value()) return false;
                const auto parsed = parse_u64_(*v);
                if (!parsed.has_value()) {
                    std::cerr << "parus-lld: invalid --toolchain-hash value\n";
                    return false;
                }
                out.expected_toolchain_hash = *parsed;
                continue;
            }
            if (a == "--target-hash") {
                const auto v = require_next("--target-hash");
                if (!v.has_value()) return false;
                const auto parsed = parse_u64_(*v);
                if (!parsed.has_value()) {
                    std::cerr << "parus-lld: invalid --target-hash value\n";
                    return false;
                }
                out.expected_target_hash = *parsed;
                continue;
            }

            if (!a.empty() && a[0] == '-') {
                out.passthrough_args.push_back(a);
            } else {
                out.inputs.push_back(a);
            }
        }

        if (out.output_path.empty()) {
            std::cerr << "parus-lld: -o <output> is required\n";
            return false;
        }
        if (out.inputs.empty()) {
            std::cerr << "parus-lld: no inputs were provided\n";
            return false;
        }
        return true;
    }

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            print_usage_();
            return 0;
        }
    }

    DriverOptions opt{};
    if (!parse_options_(argc, argv, opt)) {
        return 1;
    }

    const std::string sysroot = resolve_sysroot_(opt);
    const std::string resolved_target = resolve_target_triple_(opt, sysroot);
    const uint64_t expected_toolchain_hash = resolve_expected_toolchain_hash_(opt, sysroot);
    const uint64_t expected_target_hash = resolve_expected_target_hash_(opt, sysroot, resolved_target);

    TempWorkspace temp{};
    {
        std::error_code ec;
        const auto base = std::filesystem::temp_directory_path(ec);
        if (ec) {
            std::cerr << "parus-lld: failed to resolve temp directory\n";
            return 1;
        }
#if defined(_WIN32)
        const int pid = static_cast<int>(_getpid());
#else
        const int pid = static_cast<int>(getpid());
#endif
        temp.root = base / ("parus-lld-" + std::to_string(pid));
        std::filesystem::create_directories(temp.root, ec);
        if (ec) {
            std::cerr << "parus-lld: failed to create temp workspace: " << temp.root.string() << "\n";
            return 1;
        }
    }

    LinkPlan plan{};
    if (!plan_inputs_(opt, resolved_target, expected_toolchain_hash, expected_target_hash, temp, plan)) {
        return 1;
    }

    const std::string backend = resolve_backend_linker_path_(opt, argv);
    const std::string sdk_root = resolve_apple_sdk_root_(opt);

    auto argv_backend = build_backend_argv_(backend, opt, plan, sdk_root, resolved_target);

    if (opt.verbose) {
        std::cerr << "parus-lld: backend=" << backend << "\n";
        if (!resolved_target.empty()) std::cerr << "parus-lld: target=" << resolved_target << "\n";
        if (!sysroot.empty()) std::cerr << "parus-lld: sysroot=" << sysroot << "\n";
        if (!sdk_root.empty()) std::cerr << "parus-lld: apple-sdk-root=" << sdk_root << "\n";
    }

    const int rc = run_argv_(argv_backend);
    if (rc != 0) {
        std::cerr << "parus-lld: backend linker failed (exit=" << rc << ")\n";
    }
    return rc;
}
