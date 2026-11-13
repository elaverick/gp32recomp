// recomp.h - shared declarations of the gp32recomp tool.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct Options {
    std::string input, input_name, outdir, name;
    std::string runtime_dir;          // copy the runtime from here instead of the built-in copy
    uint32_t base = 0x0C000000u;
    uint32_t entry = 0;
    bool scan = true;                 // look for code reachable only through pointers
    unsigned max_func_blocks = 1500;  // split larger functions
    unsigned ins_per_file = 12000;
    std::vector<std::pair<uint32_t, bool>> extra_funcs;
};

int recompile(const Options &opt);
void make_dir(const std::string &path);
void write_runtime(const Options &opt, const std::string &dir);

struct RuntimeFile {
    const char *name;
    const unsigned char *data;
    unsigned long size;
};
extern const RuntimeFile runtime_files[];
extern const unsigned runtime_file_count;
