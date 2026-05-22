// agentvfs-ctl: thin client for the agentvfs control socket.
// Protocol: one line in, one line out. JSON responses follow the server's
// ad-hoc format; we don't parse JSON, we slice strings the same way the
// server does.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

namespace {

constexpr size_t MAX_RESP = 128 * 1024;

int usage() {
    std::fprintf(stderr,
        "Usage: agentvfs-ctl [--sock <path>] [--json] <subcommand> [args...]\n"
        "\n"
        "Subcommands:\n"
        "  status                                 show daemon status (JSON)\n"
        "  checkpoint [label] [--branch <name>]   create a checkpoint; prints commit hash\n"
        "  rollback <target> [--branch <name>]    roll back to label or hash\n"
        "  branch create <name> [--from <name>]   create a new branch; prints branch_id\n"
        "  branch delete <name>                   delete a branch\n"
        "  branch list                            list all branches (JSON)\n"
        "  branch merge <source> --into <target> [--label <label>]\n"
        "  session register --cgroup <p> --id <n> [--verbosity <v>] [--branch <name>]\n"
        "  session unregister --cgroup <p>\n"
        "  policy install <file|->                install policy rules from file or stdin\n"
        "  raw <line...>                          send a verbatim line to the socket\n"
        "\n"
        "Flags:\n"
        "  --sock <path>    socket path (default: $AGENTVFS_SOCK or /tmp/agentvfs.sock)\n"
        "  --json           emit the raw JSON response instead of a friendlier form\n"
        "\n"
        "Exit codes: 0 ok, 1 server error, 2 usage, 3 socket/IO error.\n");
    return 2;
}

std::string resolve_sock(const std::string& flag) {
    if (!flag.empty()) return flag;
    const char* env = std::getenv("AGENTVFS_SOCK");
    if (env && *env) return env;
#ifdef _WIN32
    return "\\\\.\\pipe\\agentvfs";
#else
    return "/tmp/agentvfs.sock";
#endif
}

std::string slurp_stream(FILE* f) {
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    return s;
}

std::string read_body(const std::string& path_or_dash) {
    if (path_or_dash == "-") return slurp_stream(stdin);
    FILE* f = std::fopen(path_or_dash.c_str(), "r");
    if (!f) { std::fprintf(stderr, "agentvfs-ctl: open %s: %s\n",
                           path_or_dash.c_str(), std::strerror(errno)); std::exit(3); }
    std::string s = slurp_stream(f);
    std::fclose(f);
    return s;
}

std::string extract_str(const std::string& json, const std::string& key) {
    auto p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return {};
    p = json.find(':', p); if (p == std::string::npos) return {};
    p = json.find('"', p); if (p == std::string::npos) return {};
    auto end = json.find('"', p + 1);
    if (end == std::string::npos) return {};
    return json.substr(p + 1, end - p - 1);
}

std::vector<std::string> extract_string_array(const std::string& json, const std::string& key) {
    std::vector<std::string> values;
    auto p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return values;
    p = json.find('[', p);
    if (p == std::string::npos) return values;
    p++;

    while (p < json.size()) {
        while (p < json.size() &&
               (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r' || json[p] == ',')) {
            p++;
        }
        if (p >= json.size() || json[p] == ']') break;
        if (json[p] != '"') break;
        p++;

        std::string value;
        bool closed = false;
        while (p < json.size()) {
            char c = json[p++];
            if (c == '\\' && p < json.size()) {
                char escaped = json[p++];
                if (escaped == 'n') value.push_back('\n');
                else if (escaped == 'r') value.push_back('\r');
                else if (escaped == 't') value.push_back('\t');
                else if (escaped == '"' || escaped == '\\') value.push_back(escaped);
                else value.push_back(escaped);
            } else if (c == '"') {
                closed = true;
                break;
            } else {
                value.push_back(c);
            }
        }
        if (!closed) break;
        values.push_back(std::move(value));
    }
    return values;
}

long extract_int(const std::string& json, const std::string& key) {
    auto p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return -1;
    p = json.find(':', p); if (p == std::string::npos) return -1;
    p++;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    char* end = nullptr;
    long v = std::strtol(json.c_str() + p, &end, 10);
    if (end == json.c_str() + p) return -1;
    return v;
}

bool ok_flag(const std::string& json) {
    auto p = json.find("\"ok\"");
    if (p == std::string::npos) return false;
    p = json.find(':', p); if (p == std::string::npos) return false;
    while (p + 1 < json.size() && (json[p + 1] == ' ' || json[p + 1] == '\t')) p++;
    return json.compare(p + 1, 4, "true") == 0;
}

// JSON-escape a string value for embedding inside a double-quoted field.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Send one line, read up to one line of response.
// Returns empty string on socket failure (errno set); caller maps to exit 3.
#ifdef _WIN32
std::string send_line(const std::string& endpoint, const std::string& line) {
    // Decode the pipe name as UTF-8 to match the daemon's
    // MultiByteToWideChar(CP_UTF8, ...) — a naive byte-cast widening
    // produces mojibake for non-ASCII pipe names and the client then
    // can't open the pipe.
    std::wstring wpipe;
    if (!endpoint.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, endpoint.data(),
                                    (int)endpoint.size(), nullptr, 0);
        if (n > 0) {
            wpipe.assign((size_t)n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, endpoint.data(),
                                (int)endpoint.size(), wpipe.data(), n);
        }
    }
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 20; ++i) {
        h = CreateFileW(wpipe.c_str(), GENERIC_READ | GENERIC_WRITE,
                        0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return {};
        WaitNamedPipeW(wpipe.c_str(), 500);
    }
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string msg = line;
    if (msg.empty() || msg.back() != '\n') msg += '\n';
    DWORD w = 0;
    if (!WriteFile(h, msg.data(), (DWORD)msg.size(), &w, nullptr)) {
        CloseHandle(h); return {};
    }
    std::string resp;
    char ch;
    while (resp.size() < MAX_RESP) {
        DWORD got = 0;
        if (!ReadFile(h, &ch, 1, &got, nullptr) || got == 0) break;
        if (ch == '\n') break;
        resp.push_back(ch);
    }
    CloseHandle(h);
    return resp;
}
#else
std::string send_line(const std::string& sock_path, const std::string& line) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return {};
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd); return {};
    }
    std::string msg = line;
    if (msg.empty() || msg.back() != '\n') msg += '\n';
    ssize_t sent = 0;
    while ((size_t)sent < msg.size()) {
        ssize_t n = write(fd, msg.data() + sent, msg.size() - sent);
        if (n <= 0) { close(fd); return {}; }
        sent += n;
    }
    std::string resp;
    char ch;
    while (resp.size() < MAX_RESP) {
        ssize_t n = read(fd, &ch, 1);
        if (n <= 0) break;
        if (ch == '\n') break;
        resp.push_back(ch);
    }
    close(fd);
    return resp;
}
#endif

// Emit response for caller; returns exit code 0 / 1.
int emit(const std::string& resp, bool raw) {
    if (resp.empty()) {
        std::fprintf(stderr, "agentvfs-ctl: no response from daemon\n");
        return 3;
    }
    bool ok = ok_flag(resp);
    if (raw) {
        std::fwrite(resp.data(), 1, resp.size(), stdout);
        std::fputc('\n', stdout);
    } else {
        if (!ok) {
            std::string err = extract_str(resp, "error");
            std::fprintf(stderr, "agentvfs-ctl: %s\n",
                         err.empty() ? resp.c_str() : err.c_str());
            return 1;
        }
    }
    return ok ? 0 : 1;
}

// Friendly (non-raw) output for specific subcommands.
int emit_field(const std::string& resp, const std::string& key) {
    if (resp.empty()) {
        std::fprintf(stderr, "agentvfs-ctl: no response from daemon\n");
        return 3;
    }
    if (!ok_flag(resp)) {
        std::string err = extract_str(resp, "error");
        std::fprintf(stderr, "agentvfs-ctl: %s\n",
                     err.empty() ? resp.c_str() : err.c_str());
        return 1;
    }
    std::string v = extract_str(resp, key);
    if (v.empty()) {
        // Fall back to raw JSON if the field wasn't there.
        std::fwrite(resp.data(), 1, resp.size(), stdout);
        std::fputc('\n', stdout);
    } else {
        std::fprintf(stdout, "%s\n", v.c_str());
    }
    return 0;
}

int emit_int_field(const std::string& resp, const std::string& key) {
    if (resp.empty()) {
        std::fprintf(stderr, "agentvfs-ctl: no response from daemon\n");
        return 3;
    }
    if (!ok_flag(resp)) {
        std::string err = extract_str(resp, "error");
        std::fprintf(stderr, "agentvfs-ctl: %s\n",
                     err.empty() ? resp.c_str() : err.c_str());
        return 1;
    }
    long v = extract_int(resp, key);
    if (v < 0) {
        // Fall back to raw JSON if the field wasn't there.
        std::fwrite(resp.data(), 1, resp.size(), stdout);
        std::fputc('\n', stdout);
    } else {
        std::fprintf(stdout, "%ld\n", v);
    }
    return 0;
}

int emit_merge(const std::string& resp) {
    if (resp.empty()) {
        std::fprintf(stderr, "agentvfs-ctl: no response from daemon\n");
        return 3;
    }
    if (!ok_flag(resp)) {
        std::string err = extract_str(resp, "error");
        std::fprintf(stderr, "agentvfs-ctl: %s\n",
                     err.empty() ? resp.c_str() : err.c_str());
        if (err == "merge conflicts") {
            for (const auto& path : extract_string_array(resp, "conflicts")) {
                std::fprintf(stderr, "  %s\n", path.c_str());
            }
        }
        return 1;
    }
    return emit_field(resp, "commit");
}

int emit_ok_or_error(const std::string& resp) {
    if (resp.empty()) {
        std::fprintf(stderr, "agentvfs-ctl: no response from daemon\n");
        return 3;
    }
    if (!ok_flag(resp)) {
        std::string err = extract_str(resp, "error");
        std::fprintf(stderr, "agentvfs-ctl: %s\n",
                     err.empty() ? resp.c_str() : err.c_str());
        return 1;
    }
    std::fprintf(stdout, "ok\n");
    return 0;
}

// Parse --flag value pairs out of argv[start..], appending remaining positional
// args to `positional`. Returns 0 on success, 2 on malformed flags. Rejects
// duplicate --flag occurrences: passing the same flag twice is almost always
// a user mistake and the previous behavior silently kept only the first copy.
int split_flags(int argc, char** argv, int start,
                std::vector<std::pair<std::string, std::string>>& flags,
                std::vector<std::string>& positional) {
    for (int i = start; i < argc; i++) {
        std::string a = argv[i];
        if (a.size() > 2 && a.compare(0, 2, "--") == 0) {
            if (i + 1 >= argc) return 2;
            std::string key = a.substr(2);
            for (const auto& f : flags) {
                if (f.first == key) {
                    std::fprintf(stderr,
                        "agentvfs-ctl: --%s specified more than once\n",
                        key.c_str());
                    return 2;
                }
            }
            flags.emplace_back(std::move(key), argv[++i]);
        } else {
            positional.push_back(a);
        }
    }
    return 0;
}

std::string find_flag(const std::vector<std::pair<std::string, std::string>>& flags,
                      const std::string& name,
                      const std::string& dflt = {}) {
    for (const auto& f : flags) if (f.first == name) return f.second;
    return dflt;
}

} // namespace

int main(int argc, char** argv) {
    std::string sock_flag;
    bool json = false;
    int i = 1;
    while (i < argc) {
        std::string a = argv[i];
        if (a == "--sock" && i + 1 < argc) { sock_flag = argv[++i]; i++; }
        else if (a == "--json") { json = true; i++; }
        else if (a == "-h" || a == "--help") { return usage(); }
        else break;
    }
    if (i >= argc) return usage();
    std::string sock = resolve_sock(sock_flag);
    std::string cmd = argv[i++];

    if (cmd == "status") {
        std::string resp = send_line(sock, "status");
        if (resp.empty()) {
            std::fprintf(stderr, "agentvfs-ctl: socket %s: %s\n",
                         sock.c_str(), std::strerror(errno));
            return 3;
        }
        // status is always useful as JSON; emit verbatim whether or not --json.
        std::fwrite(resp.data(), 1, resp.size(), stdout);
        std::fputc('\n', stdout);
        return ok_flag(resp) ? 0 : 1;
    }

    if (cmd == "checkpoint") {
        std::vector<std::pair<std::string, std::string>> flags;
        std::vector<std::string> positional;
        if (split_flags(argc, argv, i, flags, positional) != 0) return usage();
        std::string branch_name = find_flag(flags, "branch");
        std::string label = positional.empty() ? "" : positional[0];

        std::string line = "checkpoint";
        if (!label.empty()) line += " " + label;
        if (!branch_name.empty()) line += " branch=" + branch_name;

        std::string resp = send_line(sock, line);
        if (resp.empty()) {
            std::fprintf(stderr, "agentvfs-ctl: socket %s: %s\n",
                         sock.c_str(), std::strerror(errno));
            return 3;
        }
        if (json) return emit(resp, true);
        return emit_field(resp, "commit");
    }

    if (cmd == "rollback") {
        std::vector<std::pair<std::string, std::string>> flags;
        std::vector<std::string> positional;
        if (split_flags(argc, argv, i, flags, positional) != 0) return usage();
        if (positional.empty()) {
            std::fprintf(stderr, "agentvfs-ctl: rollback requires <target>\n"); return 2;
        }
        std::string target = positional[0];
        std::string branch_name = find_flag(flags, "branch");

        std::string line = "rollback " + target;
        if (!branch_name.empty()) line += " branch=" + branch_name;

        std::string resp = send_line(sock, line);
        if (resp.empty()) {
            std::fprintf(stderr, "agentvfs-ctl: socket %s: %s\n",
                         sock.c_str(), std::strerror(errno));
            return 3;
        }
        if (json) return emit(resp, true);
        return emit_field(resp, "rolled_back_to");
    }

    if (cmd == "session") {
        if (i >= argc) { std::fprintf(stderr, "agentvfs-ctl: session requires register|unregister\n"); return 2; }
        std::string sub = argv[i++];
        std::vector<std::pair<std::string, std::string>> flags;
        std::vector<std::string> positional;
        if (split_flags(argc, argv, i, flags, positional) != 0) return usage();
        std::string cg = find_flag(flags, "cgroup");
        if (cg.empty()) { std::fprintf(stderr, "agentvfs-ctl: session %s requires --cgroup\n", sub.c_str()); return 2; }

        std::string line;
        if (sub == "register") {
            std::string id = find_flag(flags, "id");
            std::string v  = find_flag(flags, "verbosity", "1");
            std::string branch_name = find_flag(flags, "branch");
            if (id.empty()) { std::fprintf(stderr, "agentvfs-ctl: session register requires --id\n"); return 2; }
            line = "session.register {\"cgroup_path\":\"" + json_escape(cg)
                 + "\",\"session_id\":" + id
                 + ",\"telemetry_verbosity\":" + v;
            if (!branch_name.empty()) line += ",\"branch\":\"" + json_escape(branch_name) + "\"";
            line += "}";
        } else if (sub == "unregister") {
            line = "session.unregister {\"cgroup_path\":\"" + json_escape(cg) + "\"}";
        } else {
            return usage();
        }
        std::string resp = send_line(sock, line);
        if (resp.empty()) {
            std::fprintf(stderr, "agentvfs-ctl: socket %s: %s\n",
                         sock.c_str(), std::strerror(errno));
            return 3;
        }
        if (json) return emit(resp, true);
        return emit_ok_or_error(resp);
    }

    if (cmd == "policy") {
        if (i >= argc) { std::fprintf(stderr, "agentvfs-ctl: policy requires install\n"); return 2; }
        std::string sub = argv[i++];
        if (sub != "install") return usage();
        if (i >= argc) { std::fprintf(stderr, "agentvfs-ctl: policy install requires <file|->\n"); return 2; }
        std::string body = read_body(argv[i++]);
        // Server parses one line; strip newlines inside the body.
        for (char& c : body) if (c == '\n' || c == '\r') c = ' ';
        std::string resp = send_line(sock, "policy.install " + body);
        if (resp.empty()) {
            std::fprintf(stderr, "agentvfs-ctl: socket %s: %s\n",
                         sock.c_str(), std::strerror(errno));
            return 3;
        }
        if (json) return emit(resp, true);
        if (!ok_flag(resp)) {
            std::string err = extract_str(resp, "error");
            std::fprintf(stderr, "agentvfs-ctl: %s\n",
                         err.empty() ? resp.c_str() : err.c_str());
            return 1;
        }
        long installed = extract_int(resp, "entries_installed");
        long version   = extract_int(resp, "policy_version");
        std::fprintf(stdout, "installed=%ld policy_version=%ld\n", installed, version);
        return 0;
    }

    if (cmd == "branch") {
        if (i >= argc) { std::fprintf(stderr, "agentvfs-ctl: branch requires create|delete|list|merge\n"); return 2; }
        std::string sub = argv[i++];
        std::string line;
        if (sub == "create") {
            std::vector<std::pair<std::string, std::string>> flags;
            std::vector<std::string> positional;
            if (split_flags(argc, argv, i, flags, positional) != 0) return usage();
            if (positional.empty()) {
                std::fprintf(stderr, "agentvfs-ctl: branch create requires <name>\n"); return 2;
            }
            std::string name = positional[0];
            std::string from = find_flag(flags, "from", "main");
            line = "branch.create {\"name\":\"" + json_escape(name)
                 + "\",\"from\":\"" + json_escape(from) + "\"}";
        } else if (sub == "delete") {
            if (i >= argc) { std::fprintf(stderr, "agentvfs-ctl: branch delete requires <name>\n"); return 2; }
            std::string name = argv[i++];
            line = "branch.delete {\"name\":\"" + json_escape(name) + "\"}";
        } else if (sub == "list") {
            line = "branch.list";
        } else if (sub == "merge") {
            std::vector<std::pair<std::string, std::string>> flags;
            std::vector<std::string> positional;
            if (split_flags(argc, argv, i, flags, positional) != 0) return usage();
            if (positional.empty()) {
                std::fprintf(stderr, "agentvfs-ctl: branch merge requires <source>\n"); return 2;
            }
            if (positional.size() != 1) {
                std::fprintf(stderr, "agentvfs-ctl: branch merge takes exactly one <source>\n"); return 2;
            }
            for (const auto& flag : flags) {
                if (flag.first != "into" && flag.first != "label") {
                    std::fprintf(stderr, "agentvfs-ctl: branch merge unknown option --%s\n",
                                 flag.first.c_str());
                    return 2;
                }
            }
            std::string source = positional[0];
            std::string target = find_flag(flags, "into");
            std::string label = find_flag(flags, "label");
            if (target.empty()) {
                std::fprintf(stderr, "agentvfs-ctl: branch merge requires --into <target>\n"); return 2;
            }
            line = "branch.merge {\"source\":\"" + json_escape(source)
                 + "\",\"target\":\"" + json_escape(target) + "\"";
            if (!label.empty()) line += ",\"label\":\"" + json_escape(label) + "\"";
            line += "}";
        } else {
            return usage();
        }
        std::string resp = send_line(sock, line);
        if (resp.empty()) {
            std::fprintf(stderr, "agentvfs-ctl: socket %s: %s\n",
                         sock.c_str(), std::strerror(errno));
            return 3;
        }
        if (json || sub == "list") return emit(resp, true);
        if (sub == "create") return emit_int_field(resp, "branch_id");
        if (sub == "merge") return emit_merge(resp);
        return emit_ok_or_error(resp);
    }

    if (cmd == "raw") {
        if (i >= argc) { std::fprintf(stderr, "agentvfs-ctl: raw requires <line...>\n"); return 2; }
        std::string line = argv[i++];
        while (i < argc) { line.push_back(' '); line += argv[i++]; }
        std::string resp = send_line(sock, line);
        if (resp.empty()) {
            std::fprintf(stderr, "agentvfs-ctl: socket %s: %s\n",
                         sock.c_str(), std::strerror(errno));
            return 3;
        }
        std::fwrite(resp.data(), 1, resp.size(), stdout);
        std::fputc('\n', stdout);
        return ok_flag(resp) ? 0 : 1;
    }

    std::fprintf(stderr, "agentvfs-ctl: unknown subcommand '%s'\n", cmd.c_str());
    return usage();
}
