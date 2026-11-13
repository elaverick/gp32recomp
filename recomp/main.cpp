// main.cpp - command line of gp32recomp.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "recomp.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

void make_dir(const std::string &path) {
    // create every component; errors surface when the first file is written
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/' || path[i] == '\\') MKDIR(path.substr(0, i).c_str());
}

static void copy_bytes(const std::string &to, const unsigned char *data, size_t size) {
    FILE *f = fopen(to.c_str(), "wb");
    if (!f) { fprintf(stderr, "gp32recomp: cannot write %s\n", to.c_str()); exit(1); }
    fwrite(data, 1, size, f);
    fclose(f);
}

void write_runtime(const Options &opt, const std::string &dir) {
    make_dir(dir);
    for (unsigned i = 0; i < runtime_file_count; i++) {
        const RuntimeFile &rf = runtime_files[i];
        std::string to = dir + "/" + rf.name;
        if (!opt.runtime_dir.empty()) {
            std::string from = opt.runtime_dir + "/" + rf.name;
            FILE *f = fopen(from.c_str(), "rb");
            if (!f) { fprintf(stderr, "gp32recomp: cannot read %s\n", from.c_str()); exit(1); }
            std::string buf;
            char tmp[65536];
            size_t n;
            while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf.append(tmp, n);
            fclose(f);
            copy_bytes(to, (const unsigned char *)buf.data(), buf.size());
        } else {
            copy_bytes(to, rf.data, rf.size);
        }
    }
}

static void usage() {
    puts("gp32recomp - translate a Gamepark GP32 program into C for a native build\n"
         "\n"
         "usage: gp32recomp <program.fxe|program.gxb> [options]\n"
         "  -o DIR            output directory (default: <program>_recomp)\n"
         "  --name NAME       name of the generated program (default: from the file name)\n"
         "  --base ADDR       load address (default 0x0C000000)\n"
         "  --entry ADDR      entry point (default: the load address)\n"
         "  --func ADDR       treat ADDR as the start of a function; add 1 for Thumb code.\n"
         "                    May be repeated.\n"
         "  --no-scan         only follow direct calls and branches; do not guess at code\n"
         "                    reached through pointers\n"
         "  --runtime DIR     copy the runtime sources from DIR instead of the built-in copy\n"
         "\n"
         "The output directory holds C sources, the runtime and a CMakeLists.txt:\n"
         "  cmake -S DIR -B DIR/build && cmake --build DIR/build --config Release");
}

int main(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "-o" && v) { opt.outdir = v; i++; }
        else if (a == "--name" && v) { opt.name = v; i++; }
        else if (a == "--base" && v) { opt.base = (uint32_t)strtoul(v, nullptr, 0); i++; }
        else if (a == "--entry" && v) { opt.entry = (uint32_t)strtoul(v, nullptr, 0); i++; }
        else if (a == "--func" && v) { uint32_t f = (uint32_t)strtoul(v, nullptr, 0); opt.extra_funcs.emplace_back(f & ~1u, f & 1); i++; }
        else if (a == "--runtime" && v) { opt.runtime_dir = v; i++; }
        else if (a == "--no-scan") opt.scan = false;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a[0] == '-' || !opt.input.empty()) { fprintf(stderr, "gp32recomp: unexpected argument %s\n", a.c_str()); return 2; }
        else opt.input = a;
    }
    if (opt.input.empty()) { usage(); return 2; }
    size_t slash = opt.input.find_last_of("/\\");
    opt.input_name = slash == std::string::npos ? opt.input : opt.input.substr(slash + 1);
    std::string stem = opt.input_name.substr(0, opt.input_name.find_last_of('.'));
    if (opt.name.empty()) opt.name = stem;
    if (opt.outdir.empty()) opt.outdir = stem + "_recomp";
    if ((opt.base & 3) || opt.base < 0x0C000000u || opt.base >= 0x0C780000u) { fprintf(stderr, "gp32recomp: the load address must lie in RAM (0x0C000000 and up)\n"); return 2; }
    return recompile(opt);
}
