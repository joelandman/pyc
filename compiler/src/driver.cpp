// The pyc driver (INTERFACES §5) — one binary, no separate `pyc_lower`.
//
// This replaces the former bash `compiler/tools/pycc` script. It does the same
// three-step job, but the middle step (lowering) now happens IN-PROCESS via
// `lower_envelope` instead of launching a `pyc_lower` binary it had to find:
//
//   1. parse  — spawn the TARGET interpreter (`<sysroot>/bin/python3.X
//               -m pyc_parse`) for the AST. Constraint-bound: the compiler
//               links no libpython, so it cannot parse in-process for an
//               arbitrary target (VERSION_TARGETING option (b)).
//   2. lower  — `lower_envelope` + `codegen_llvm` in-process (was pyc_lower).
//   3. link   — spawn the host clang++ to compile the IR + runtime trampoline
//               and link static libpython (Tier 1, -rdynamic, --whole-archive).
//               Constraint-bound: CHARTER pins the toolchain to clang/LLVM 22.
//
// Steps 1 and 3 remain subprocesses on purpose; step 2 is what used to be a
// second binary the user had to install and point PYC_LOWER at. That is gone.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pyc/ir/ir.hpp"
#include "pyc/json.hpp"
#include "pyc/lower_api.hpp"

namespace fs = std::filesystem;

using namespace pyc;  // lower_envelope, codegen_llvm, ir::Module, json::*

namespace {

// ---------------------------------------------------------------------------
// Small process + filesystem helpers
// ---------------------------------------------------------------------------

struct Proc {
    int code = -1;
    std::string out;
    std::string err;
};

// Run a command, capturing stdout and stderr. `env_set` are "K=V" pairs to
// set; `env_unset` are names to clear (the driver always clears
// LD_LIBRARY_PATH for the target interpreter: it is a thin launcher over its
// own libpython, and a caller's LD_LIBRARY_PATH outranks its RUNPATH and would
// silently swap in a DIFFERENT CPython — the parse oracle must be the target).
Proc run_proc(const std::vector<std::string>& argv,
              const std::vector<std::string>& env_set,
              const std::vector<std::string>& env_unset) {
    int outp[2], errp[2];
    if (::pipe(outp) != 0 || ::pipe(errp) != 0) return Proc{};
    pid_t pid = ::fork();
    if (pid < 0) return Proc{};
    if (pid == 0) {
        ::dup2(outp[1], 1);
        ::dup2(errp[1], 2);
        ::close(outp[0]); ::close(outp[1]);
        ::close(errp[0]); ::close(errp[1]);
        for (const auto& u : env_unset) ::unsetenv(u.c_str());
        for (const auto& kv : env_set) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            ::setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
        }
        std::vector<char*> cargs;
        cargs.reserve(argv.size() + 1);
        for (const auto& a : argv) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);
        ::execvp(cargs[0], cargs.data());
        ::_exit(127);
    }
    ::close(outp[1]); ::close(errp[1]);
    Proc r;
    std::thread err_reader([errp, &r]() {
        char t[8192];
        ssize_t n;
        while ((n = ::read(errp[0], t, sizeof t)) > 0) r.err.append(t, (size_t)n);
        ::close(errp[0]);
    });
    {
        char t[8192];
        ssize_t n;
        while ((n = ::read(outp[0], t, sizeof t)) > 0) r.out.append(t, (size_t)n);
    }
    ::close(outp[0]);
    err_reader.join();
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return Proc{};
    }
    if (WIFEXITED(st)) r.code = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) r.code = 128 + WTERMSIG(st);
    return r;
}

std::string exe_dir() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    return fs::path(buf).parent_path().string();
}

bool is_exec(const std::string& p) { return !p.empty() && ::access(p.c_str(), X_OK) == 0; }
bool is_dir(const std::string& p) { std::error_code ec; return fs::is_directory(p, ec); }
bool is_file(const std::string& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
std::string abs_path(const std::string& p) {
    std::error_code ec;
    auto a = fs::absolute(p, ec);
    return ec ? p : a.lexically_normal().string();
}
std::string realpath_of(const std::string& p) {
    char b[4096];
    return ::realpath(p.c_str(), b) ? std::string(b) : p;
}
std::string env_get(const char* k, const std::string& dflt = "") {
    const char* v = ::getenv(k);
    return v ? v : dflt;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string t;
    while (iss >> t) out.push_back(t);
    return out;
}
void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f.write(content.data(), (std::streamsize)content.size());
}
// A C++ string literal (the trampoline embeds these), not JSON: escape for C.
std::string cpp_literal(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '"') o += "\\\"";
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\r') o += "\\r";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\x%02x", c); o += b; }
        else o += (char)c;
    }
    o += "\"";
    return o;
}

struct TmpGuard {
    std::string path;
    bool keep;
    explicit TmpGuard(bool keep) : keep(keep) {
        char tmpl[] = "/tmp/pyc-driver.XXXXXX";
        if (char* p = ::mkdtemp(tmpl)) path = p;
    }
    ~TmpGuard() {
        if (!keep && !path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
};

// Read a manifest key from <root>/pyc-sysroot.json (""); parse failures are "".
std::string manifest_key(const std::string& root, const std::string& key) {
    std::string text;
    {
        std::ifstream f(root + "/pyc-sysroot.json");
        if (!f) return "";
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    json::Doc doc;
    json::ParseError err;
    if (!json::parse(text, doc, err)) return "";
    if (const json::Value* v = doc.find(doc.root(), key))
        if (v->kind == json::Kind::String) return std::string(doc.str_of(*v));
    return "";
}

// ---------------------------------------------------------------------------
// Sysroot resolution (ported from the bash driver)
// ---------------------------------------------------------------------------

bool xy_le(const std::string& a_in, const std::string& b_in) {
    auto strip = [](std::string s) { return s.rfind("py", 0) == 0 ? s.substr(2) : s; };
    auto parts = [](const std::string& s, int& maj, int& min) {
        maj = min = 0;
        std::istringstream iss(s);
        char dot;
        iss >> maj >> dot >> min;
    };
    int a0, a1, b0, b1;
    parts(strip(a_in), a0, a1);
    parts(strip(b_in), b0, b1);
    return a0 < b0 || (a0 == b0 && a1 <= b1);
}

std::string find_interpreter(const std::string& root, const std::string& want) {
    std::string interp = manifest_key(root, "interpreter");
    if (!interp.empty() && is_exec(interp)) return interp;
    if (!want.empty()) {
        std::string p = root + "/bin/python" + want;
        if (is_exec(p)) return p;
    }
    std::string p3 = root + "/bin/python3";
    if (is_exec(p3)) return p3;
    // Highest bin/python3.N (not -config).
    struct Cand { int n; std::string path; };
    std::vector<Cand> cands;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root + "/bin", ec)) {
        std::string name = e.path().filename().string();
        if (name.rfind("python3.", 0) != 0) continue;
        if (name.find("config") != std::string::npos) continue;
        std::string digits = name.substr(8);
        if (digits.empty() || std::any_of(digits.begin(), digits.end(),
                                          [](unsigned char c) { return !std::isdigit(c); }))
            continue;
        cands.push_back(Cand{std::stoi(digits), e.path().string()});
    }
    if (!cands.empty()) {
        std::sort(cands.begin(), cands.end(),
                  [](const Cand& a, const Cand& b) { return a.n < b.n; });
        const std::string& best = cands.back().path;
        if (is_exec(best)) return best;
    }
    return "";
}

bool is_sysroot(const std::string& d) {
    if (!is_dir(d)) return false;
    if (is_file(d + "/pyc-sysroot.json")) return true;
    if (is_exec(d + "/bin/python3")) return true;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(d + "/bin", ec)) {
        std::string name = e.path().filename().string();
        if (name.rfind("python3.", 0) == 0) return true;
    }
    return false;
}

std::string sysroot_xy(const std::string& root) {
    std::string xy = manifest_key(root, "xy");
    if (xy.empty()) {
        std::string p = find_interpreter(root, "");
        if (!p.empty()) {
            Proc r = run_proc({p, "-c",
                               "import sys; print('%d.%d'%sys.version_info[:2])"},
                              {}, {"LD_LIBRARY_PATH"});
            xy = trim(r.out);
        }
    }
    return xy;
}

std::vector<std::string> collect_sysroot_dirs(const std::string& exe) {
    std::vector<std::string> out;
    auto push_dir = [&](const std::string& p) {
        if (is_dir(p)) out.push_back(abs_path(p));
    };
    push_dir(exe + "/sysroot");
    push_dir(exe + "/../sysroot");
    std::string sr = exe + "/../sysroots";
    std::error_code ec;
    if (is_dir(sr))
        for (auto& e : fs::directory_iterator(sr, ec)) {
            std::string name = e.path().filename().string();
            if (name.rfind("cp", 0) == 0 && name.find("-tier") != std::string::npos)
                push_dir(e.path().string());
        }
    std::string home = env_get("HOME", "");
    std::string hsr = home + "/opt/py-sysroots";
    if (is_dir(hsr))
        for (auto& e : fs::directory_iterator(hsr, ec))
            if (e.is_directory(ec)) push_dir(e.path().string());
    return out;
}

}  // namespace

int pyc_driver_main(int argc, char** argv) {
    const std::string exe = exe_dir();
    // Repo root, the way the old bash driver found it (the driver lives at
    // compiler/tools/pyc). Only a last-resort fallback: in-tree and in an
    // installed prefix the exe-relative candidates (../pyc_parse, ../lib/pyc,
    // ../../tools) resolve first.
    const std::string repo = abs_path(exe + "/../..");

    // ---- argument parsing (mirrors the former bash driver's diagnostics) ----
    std::string flag_sysroot, out = "a.out", src;
    int fetch_sysroot = 0, keep = 0, emit_llvm = 0, list_targets = 0;
    std::string opt = "-O1";
    std::string python_ver = env_get("PYC_PYTHON", "");
    std::string std_ver, python_abi;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const std::string& k) -> std::string {
            if (a == k) { ++i; return i < argc ? argv[i] : ""; }
            if (a.rfind(k + "=", 0) == 0) return a.substr(k.size() + 1);
            return std::string();
        };
        if (a == "-o") { if (++i < argc) out = argv[i]; }
        else if ((a == "--sysroot" || a == "--python-sysroot")) {
            std::string v = val("--sysroot"), w = val("--python-sysroot");
            flag_sysroot = !v.empty() ? v : w;
        }
        else if (a == "--python") python_ver = val("--python");
        else if (a == "--python-abi") python_abi = val("--python-abi");
        else if (a == "-std") std_ver = val("-std");
        else if (a == "--fetch-sysroot") fetch_sysroot = 1;
        else if (a == "--list-python-targets") list_targets = 1;
        else if (a == "--keep") keep = 1;
        else if (a == "--emit-llvm") emit_llvm = 1;
        else if (a == "-O0" || a == "-O1" || a == "-O2" || a == "-O3") opt = a;
        else if (a == "--") { if (++i < argc) { src = argv[i]; } }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "pyc: unknown option '%s'\n", a.c_str());
            return 2;
        } else {
            if (!src.empty()) {
                std::fprintf(stderr,
                             "pyc: more than one input file ('%s', '%s')\n",
                             src.c_str(), a.c_str());
                return 2;
            }
            src = a;
        }
    }

    // ---- sysroot discovery ----
    std::string sysroot;
    std::vector<std::string> tried;

    auto resolve_sysroot = [&]() -> bool {
        if (!flag_sysroot.empty()) {
            std::string c = abs_path(flag_sysroot);
            if (!is_sysroot(c)) {
                std::fprintf(stderr, "pyc: --python-sysroot is not a sysroot: %s\n",
                             c.c_str());
                std::fprintf(stderr, "  need pyc-sysroot.json or bin/python3*\n");
                return false;
            }
            sysroot = c;
            return true;
        }
        std::string env_sr = env_get("PYC_SYSROOT", "");
        if (!env_sr.empty()) {
            std::string c = abs_path(env_sr);
            if (!is_sysroot(c)) {
                std::fprintf(stderr, "pyc: PYC_SYSROOT is not a sysroot: %s\n", c.c_str());
                std::fprintf(stderr, "  need pyc-sysroot.json or bin/python3*\n");
                return false;
            }
            sysroot = c;
            return true;
        }
        std::string want = python_ver;
        if (want.rfind("py", 0) == 0) want = want.substr(2);
        for (const std::string& c : collect_sysroot_dirs(exe)) {
            if (!is_sysroot(c)) continue;
            if (!want.empty() && sysroot_xy(c) != want) continue;
            sysroot = c;
            return true;
        }
        tried = collect_sysroot_dirs(exe);
        return false;
    };

    auto print_sysroot_miss = [&]() {
        std::fprintf(stderr, "pyc: no sysroot (need pyc-sysroot.json or bin/python3*)\n");
        std::fprintf(stderr, "  tried:\n");
        for (const std::string& t : tried) std::fprintf(stderr, "    %s\n", t.c_str());
        std::fprintf(stderr, "  pyc --fetch-sysroot\n");
        std::fprintf(stderr, "  # or: PYC_FETCH=1 pyc file.py\n");
        std::fprintf(stderr, "  # or: ./tools/setup-machine.sh --beside\n");
        std::fprintf(stderr, "  # or: --python-sysroot DIR / PYC_SYSROOT\n");
    };

    auto find_install_sysroot = [&]() -> std::string {
        std::vector<std::string> c = {
            exe + "/install-sysroot.sh",
            exe + "/../lib/pyc/install-sysroot.sh",
            exe + "/../../tools/install-sysroot.sh",
            repo + "/tools/install-sysroot.sh"};
        for (const std::string& p : c)
            if (is_file(p)) return abs_path(p);
        return "";
    };

    auto fetch_sysroot_fn = [&]() -> int {
        std::string dest = exe + "/sysroot";
        if (fs::path(exe).filename().string() == "bin")
            dest = exe + "/../sysroots/cp314-3.14.7-tier1";
        std::string tag = env_get("PYC_SYSROOT_TAG", "sysroot-cp314-linux-x86_64");
        std::string asset = env_get("PYC_SYSROOT_ASSET", "cp314-3.14.7-tier1-x86_64.tar.xz");
        std::string gh = env_get("PYC_GITHUB_REPO", "joelandman/pyc");
        std::string inst = find_install_sysroot();
        if (inst.empty()) {
            std::fprintf(stderr, "pyc: --fetch-sysroot needs tools/install-sysroot.sh\n");
            return 2;
        }
        if (is_sysroot(dest)) {
            std::fprintf(stderr, "pyc: sysroot already at %s\n", dest.c_str());
            run_proc({inst, "--prefix", dest, "--verify-only"}, {}, {});
            return 0;
        }
        {
            // `command -v curl`
            std::string which = trim(run_proc({"command", "-v", "curl"}, {}, {}).out);
            if (which.empty()) {
                std::fprintf(stderr, "pyc: curl required for --fetch-sysroot\n");
                return 2;
            }
        }
        char tmpl[] = "/tmp/pyc-sysroot.XXXXXX.tar.xz";
        if (!::mkdtemp(tmpl)) return 2;
        std::string archive = tmpl;
        std::string url = "https://github.com/" + gh + "/releases/download/" + tag + "/" + asset;
        std::fprintf(stderr, "pyc: downloading %s\n", url.c_str());
        Proc curl = run_proc({"curl", "-fL", "--retry", "3", "-o", archive, url}, {}, {});
        if (curl.code != 0) {
            std::remove(archive.c_str());
            std::fprintf(stderr, "pyc: download failed\n  %s\n", url.c_str());
            return 2;
        }
        run_proc({inst, "--archive", archive, "--prefix", dest}, {}, {});
        std::remove(archive.c_str());
        std::fprintf(stderr, "pyc: sysroot installed at %s\n", dest.c_str());
        return 0;
    };

    if (fetch_sysroot) {
        int rc = fetch_sysroot_fn();
        if (rc != 0) return rc;
        if (src.empty() && !list_targets) return 0;
    }

    if (!resolve_sysroot()) {
        if (env_get("PYC_FETCH", "0") == "1" && !fetch_sysroot && flag_sysroot.empty() &&
            env_get("PYC_SYSROOT", "").empty()) {
            std::fprintf(stderr, "pyc: PYC_FETCH=1, fetching sysroot\n");
            fetch_sysroot_fn();
            resolve_sysroot();
        }
    }
    if (sysroot.empty()) {
        std::string want = python_ver;
        if (want.rfind("py", 0) == 0) want = want.substr(2);
        if (!want.empty()) {
            std::fprintf(stderr, "pyc: no sysroot for --python=%s\n", want.c_str());
            std::fprintf(stderr, "  installed:\n");
            for (const std::string& d : collect_sysroot_dirs(exe)) {
                if (!is_sysroot(d)) continue;
                std::string xy = sysroot_xy(d);
                std::fprintf(stderr, "    %s  %s\n", xy.empty() ? "?" : xy.c_str(), d.c_str());
            }
            std::fprintf(stderr,
                         "  --python=X.Y selects a sysroot artifact, not a second compiler\n");
            return 2;
        }
        print_sysroot_miss();
        return 2;
    }

    // ---- target interpreter, version, ABI ----
    std::string want = python_ver;
    if (want.rfind("py", 0) == 0) want = want.substr(2);
    std::string py = find_interpreter(sysroot, want);
    if (py.empty()) {
        std::fprintf(stderr, "pyc: no interpreter in sysroot %s\n", sysroot.c_str());
        return 2;
    }
    std::string xy = trim(run_proc(
        {py, "-c", "import sys; print('%d.%d' % sys.version_info[:2])"}, {},
        {"LD_LIBRARY_PATH"}).out);
    std::string abi = trim(run_proc(
        {py, "-c",
         "import sys,sysconfig\n"
         "a='cp%d%d'%sys.version_info[:2]\n"
         "if sysconfig.get_config_var('Py_GIL_DISABLED'): a+='t'\n"
         "if hasattr(sys,'gettotalrefcount'): a+='d'\n"
         "print(a)"},
        {}, {"LD_LIBRARY_PATH"}).out);

    if (list_targets) {
        std::printf("selected %s  abi=%s\n", xy.c_str(), abi.c_str());
        std::printf("  sysroot      %s\n", sysroot.c_str());
        std::printf("  interpreter  %s\n", py.c_str());
        std::printf("installed:\n");
        for (const std::string& d : collect_sysroot_dirs(exe)) {
            if (!is_sysroot(d)) continue;
            std::string xyd = sysroot_xy(d);
            std::string ab = manifest_key(d, "abi");
            std::printf("  %s  abi=%s  %s\n", xyd.empty() ? "?" : xyd.c_str(),
                        ab.empty() ? "?" : ab.c_str(), d.c_str());
        }
        return 0;
    }

    if (!python_ver.empty()) {
        std::string pv = python_ver;
        if (pv.rfind("py", 0) == 0) pv = pv.substr(2);
        if (xy != pv) {
            std::fprintf(stderr, "pyc: sysroot is Python %s, not %s\n", xy.c_str(),
                         pv.c_str());
            std::fprintf(stderr, "  --python=%s\n  sysroot %s\n", pv.c_str(),
                         sysroot.c_str());
            return 2;
        }
    }
    if (!python_abi.empty() && python_abi != abi) {
        std::fprintf(stderr, "pyc: sysroot ABI is %s, not %s\n", abi.c_str(),
                     python_abi.c_str());
        std::fprintf(stderr, "  --python-abi=%s\n  sysroot %s\n", python_abi.c_str(),
                     sysroot.c_str());
        return 2;
    }
    std::string std_xy;
    if (!std_ver.empty()) {
        std_xy = std_ver;
        if (std_xy.rfind("py", 0) == 0) std_xy = std_xy.substr(2);
        if (!xy_le(std_xy, xy)) {
            std::fprintf(stderr,
                         "pyc: -std=py%s requires a runtime of %s or newer; --python=%s\n",
                         std_xy.c_str(), std_xy.c_str(), xy.c_str());
            return 2;
        }
    }

    if (src.empty()) {
        std::fprintf(stderr,
                     "usage: pyc FILE [-o OUT] [--python=X.Y] [-std=pyX.Y] "
                     "[--python-sysroot DIR]\n");
        return 2;
    }

    // ---- parse: the TARGET interpreter produces the AST (constraint-bound) ----
    std::string parse_home;
    if (is_dir(exe + "/pyc_parse")) parse_home = exe;
    else if (is_dir(exe + "/../lib/pyc/pyc_parse")) parse_home = abs_path(exe + "/../lib/pyc");
    else if (is_dir(exe + "/../pyc_parse")) parse_home = abs_path(exe + "/..");
    else if (is_dir(repo + "/compiler/pyc_parse")) parse_home = repo + "/compiler";
    if (parse_home.empty()) {
        std::fprintf(stderr,
                     "pyc: no pyc_parse next to pyc, in lib/pyc, or in compiler/\n");
        return 2;
    }
    std::vector<std::string> parse_args = {py, "-m", "pyc_parse", realpath_of(src)};
    if (!std_xy.empty()) parse_args.insert(parse_args.end(),
                                           {"--feature-version", std_xy});
    std::string pp = env_get("PYTHONPATH", "");
    std::string pythonpath = parse_home + (pp.empty() ? "" : (":" + pp));
    Proc parse = run_proc(parse_args, {"PYTHONPATH=" + pythonpath}, {"LD_LIBRARY_PATH"});
    if (parse.code != 0) {
        if (!parse.err.empty()) std::fputs(parse.err.c_str(), stderr);
        if (!parse.out.empty()) {
            // Render the structured error envelope (a SyntaxError) the way the
            // lower stage does, so the user sees file:line:col, not a bare exit.
            ir::Module dummy;
            lower_envelope(parse.out, "<input>", dummy);
        }
        return 1;
    }

    // ---- lower in-process (this used to be the separate pyc_lower binary) ----
    ir::Module m;
    if (lower_envelope(parse.out, realpath_of(src), m) != 0) return 1;
    std::string llvm = codegen_llvm(m);

    if (emit_llvm) {
        write_file(out, llvm);
        return 0;
    }

    // ---- link: the host clang++ compiles IR + trampoline + static libpython ----
    TmpGuard tmp(keep == 1);
    if (tmp.path.empty()) {
        std::fprintf(stderr, "pyc: cannot create temp dir\n");
        return 2;
    }

    std::string cfg;
    if (is_exec(py + "-config")) cfg = py + "-config";
    else if (is_exec(sysroot + "/bin/python3-config")) cfg = sysroot + "/bin/python3-config";
    else {
        std::fprintf(stderr, "pyc: no python-config next to %s\n", py.c_str());
        return 2;
    }
    std::string inc, lib;
    if (is_file(sysroot + "/pyc-sysroot.json")) {
        std::string inc_dir = manifest_key(sysroot, "include");
        if (is_dir(inc_dir)) inc = "-I" + inc_dir;
        std::string lib_static = manifest_key(sysroot, "libpython_static");
        if (is_file(lib_static)) lib = lib_static;
    }
    if (inc.empty()) inc = trim(run_proc({cfg, "--includes"}, {}, {}).out);
    if (lib.empty()) {
        std::string libpl = trim(run_proc(
            {py, "-c", "import sysconfig;print(sysconfig.get_config_var('LIBPL') or '')"}, {},
            {"LD_LIBRARY_PATH"}).out);
        std::string libname = trim(run_proc(
            {py, "-c", "import sysconfig;print(sysconfig.get_config_var('LIBRARY') or '')"}, {},
            {"LD_LIBRARY_PATH"}).out);
        if (!libpl.empty() && !libname.empty() && is_file(libpl + "/" + libname))
            lib = libpl + "/" + libname;
        else {
            std::string glob_dir;
            std::error_code ec;
            std::string base = sysroot + "/lib/python" + xy + "/";
            for (auto& e : fs::directory_iterator(base, ec)) {
                std::string name = e.path().filename().string();
                if (name.rfind("config-", 0) != 0) continue;
                std::error_code ec2;
                for (auto& l : fs::directory_iterator(e.path(), ec2)) {
                    std::string ln = l.path().filename().string();
                    if (ln.rfind("libpython", 0) == 0 && ln.find(".a") != std::string::npos) {
                        lib = l.path().string();
                        break;
                    }
                }
                if (!lib.empty()) break;
            }
        }
    }
    if (!is_file(lib)) {
        std::fprintf(stderr, "pyc: no libpython.a under %s\n", sysroot.c_str());
        return 2;
    }

    std::string file_c = cpp_literal(realpath_of(src));
    std::string home_c = cpp_literal(realpath_of(sysroot));
    write_file(tmp.path + "/main.cpp",
               "#include \"pyc/rt/entry.hpp\"\n"
               "#include \"pyc/rt/support.hpp\"\n"
               "extern \"C\" int __pyc_main(void*);\n"
               "static int body(void) { return __pyc_main(nullptr); }\n"
               "int main(int argc, char** argv) {\n"
               "  pyc_rt_set_source_file(" + file_c + ");\n"
               "  pyc_rt_set_python_home(" + home_c + ");\n"
               "  return pyc_rt_main(argc, argv, body);\n"
               "}\n");
    write_file(tmp.path + "/mod.ll", llvm);

    // LTO policy: in the gate's -O0 builds LTO is pure cost, so auto drops it
    // at -O0; -O1 and up keep it. A silent fallback is deliberate (stderr is
    // compared by the harness), so CI asserts the fast path is live instead.
    std::string lto;
    std::string lto_mode = env_get("PYC_LTO", "auto");
    std::string cxx = env_get("PYC_CXX", "clang++");
    if (lto_mode == "off") lto.clear();
    else if (lto_mode == "auto") {
        if (opt != "-O0") {
            std::string lld =
                trim(run_proc({cxx, "-print-prog-name=ld.lld"}, {}, {}).out);
            if (is_exec(lld)) lto = "-flto=thin -fuse-ld=lld";
        }
    } else lto = lto_mode;

    // Runtime sources (the trampoline is compiled on every link; runtime
    // objects are always -O2, the generated IR keeps $OPT).
    std::string rt_home;
    auto has_rt = [](const std::string& d) {
        return is_dir(d + "/include/pyc/rt") && is_file(d + "/src/rt/entry.cpp");
    };
    if (has_rt(exe)) rt_home = exe;
    else if (has_rt(exe + "/../lib/pyc")) rt_home = abs_path(exe + "/../lib/pyc");
    else if (has_rt(exe + "/..")) rt_home = abs_path(exe + "/..");
    else if (is_dir(repo + "/compiler/include/pyc/rt")) rt_home = repo + "/compiler";
    if (rt_home.empty()) {
        std::fprintf(stderr,
                     "pyc: no runtime sources (include/pyc/rt + src/rt) next to "
                     "pyc, in lib/pyc, or in compiler/\n");
        return 2;
    }
    std::string inc_all = inc + " -I " + rt_home + "/include";

    for (const char* rt : {"entry", "support", "frame"}) {
        std::vector<std::string> c = {cxx, "-std=c++20", "-O2"};
        for (auto& t : split_ws(lto)) c.push_back(t);  // e.g. -flto=thin -fuse-ld=lld (two args)
        for (auto& t : split_ws(inc_all)) c.push_back(t);
        c.push_back("-c");
        c.push_back(rt_home + "/src/rt/" + rt + ".cpp");
        c.push_back("-o");
        c.push_back(tmp.path + "/" + rt + ".o");
        Proc r = run_proc(c, {}, {});
        if (r.code != 0) {
            if (!r.err.empty()) std::fputs(r.err.c_str(), stderr);
            return 1;
        }
    }

    {
        std::vector<std::string> c = {cxx, "-std=c++20", opt, "-rdynamic"};
        for (auto& t : split_ws(lto)) c.push_back(t);  // e.g. -flto=thin -fuse-ld=lld (two args)
        for (auto& t : split_ws(inc_all)) c.push_back(t);
        c.push_back(tmp.path + "/mod.ll");
        c.push_back(tmp.path + "/main.cpp");
        c.push_back(tmp.path + "/entry.o");
        c.push_back(tmp.path + "/support.o");
        c.push_back(tmp.path + "/frame.o");
        c.push_back("-Wl,--whole-archive");
        c.push_back(lib);
        c.push_back("-Wl,--no-whole-archive");
        c.push_back("-lm");
        c.push_back("-lpthread");
        c.push_back("-ldl");
        c.push_back("-lutil");
        c.push_back("-lz");
        c.push_back("-o");
        c.push_back(out);
        Proc r = run_proc(c, {}, {});
        if (r.code != 0) {
            if (!r.err.empty()) std::fputs(r.err.c_str(), stderr);
            return 1;
        }
    }
    return 0;
}
