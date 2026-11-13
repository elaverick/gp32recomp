// gp32recomp - static recompiler for Gamepark GP32 executables.
//
// Reads a GXB or FXE program image, finds its code, and writes C source in
// which every ARM/Thumb instruction is a call to one of the handlers in
// runtime/gp32_ops.h with the instruction word as a constant.  Compiling that
// source together with the runtime gives a native program.
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../runtime/gp32_ops.h"
}
#include "recomp.h"

namespace {

// ---------------------------------------------------------------------------
// Program image
// ---------------------------------------------------------------------------
struct Image {
    std::vector<uint8_t> data;
    uint32_t base = GP32_RAM_BASE;
    uint32_t entry = GP32_RAM_BASE;
    std::string title;

    bool has(uint32_t a, uint32_t n = 1) const { return a >= base && a - base + n <= data.size(); }
    uint32_t rd32(uint32_t a) const {
        const uint8_t *p = &data[a - base];
        return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    uint32_t rd16(uint32_t a) const {
        const uint8_t *p = &data[a - base];
        return p[0] | (p[1] << 8);
    }
};

[[noreturn]] void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "gp32recomp: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

std::vector<uint8_t> read_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) die("cannot open %s", path.c_str());
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    return d;
}

uint32_t le32(const std::vector<uint8_t> &d, size_t o) {
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}

// FXE: "fxe ", total size, info size, title[32], author[32], reserved[16],
// 32x32 icon, payload size, key size, key, payload XORed with the inverted key.
void load_image(const std::string &path, Image &img) {
    std::vector<uint8_t> d = read_file(path);
    const size_t hdr = 12 + 32 + 32 + 16 + 1024;
    if (d.size() > hdr + 8 && !memcmp(d.data(), "fxe ", 4)) {
        uint32_t size = le32(d, hdr), klen = le32(d, hdr + 4);
        size_t off = hdr + 8;
        if (!klen || off + klen + size > d.size()) die("%s: damaged FXE header", path.c_str());
        img.title.assign((const char *)&d[12], strnlen((const char *)&d[12], 32));
        img.data.resize(size);
        for (uint32_t i = 0; i < size; i++) img.data[i] = d[off + klen + i] ^ d[off + i % klen] ^ 0xFF;
    } else {
        img.data = d;
    }
    if (img.data.size() < 8) die("%s: file is too small to be a GP32 program", path.c_str());
    while (img.data.size() & 3) img.data.push_back(0);
    uint32_t w0 = le32(img.data, 0);
    if ((w0 & 0xFF000000u) != 0xEA000000u)
        fprintf(stderr, "gp32recomp: warning: the image does not start with a branch (%08X).\n"
                        "            Commercial GXE files are encrypted and cannot be translated.\n", w0);
    if (img.data.size() > GP32_RAM_SIZE - 0x80000) die("image is larger than the GP32's memory");
}

// ---------------------------------------------------------------------------
// Code discovery
// ---------------------------------------------------------------------------
enum Flow : uint8_t {
    F_NONE,     // ordinary instruction
    F_BRANCH,   // direct branch to `target`
    F_CALL,     // direct call to `target`
    F_RET,      // writes pc; treated as a return
    F_JUMP,     // writes pc; target looked up within the function first
    F_TAIL,     // writes pc; leaves the function (BX)
    F_INDCALL,  // writes pc after "mov lr, pc"
    F_SWI,
    F_INTERP,   // left to the interpreter
};

struct Ins {
    uint32_t raw = 0;
    uint8_t size = 4;
    bool thumb = false;
    uint8_t cond = 14;
    Flow flow = F_NONE;
    uint8_t cls = 0;
    uint32_t target = 0;
};

enum : uint8_t { C_FREE, C_CODE, C_TAIL, C_DATA };

struct Analysis {
    const Image &img;
    Options opt;
    std::map<uint32_t, Ins> code;
    std::vector<uint8_t> cover;                    // per byte: C_*
    std::set<uint32_t> leaders;                    // block starts
    std::set<uint32_t> funcs;                      // function entries
    std::vector<std::pair<uint32_t, bool>> work;   // (address, thumb)
    std::set<std::pair<uint32_t, bool>> strong;    // pointers seen in code
    std::map<uint32_t, std::vector<uint32_t>> tables; // jump instruction -> targets
    explicit Analysis(const Image &i, const Options &o) : img(i), opt(o), cover(i.data.size(), C_FREE) {}

    uint8_t &cov(uint32_t a) { return cover[a - img.base]; }
    void add_leader(uint32_t a, bool thumb) {
        leaders.insert(a);
        work.emplace_back(a, thumb);
    }
    void add_func(uint32_t a, bool thumb) {
        funcs.insert(a);
        add_leader(a, thumb);
    }
    void mark_data(uint32_t a, uint32_t n) {
        for (uint32_t i = 0; i < n && img.has(a + i); i++)
            if (cov(a + i) == C_FREE) cov(a + i) = C_DATA;
    }
    bool mode_near(uint32_t a) const { // is the nearest known code below `a` Thumb?
        auto it = code.upper_bound(a);
        if (it == code.begin()) return false;
        --it;
        return a - it->first < 0x1000 && it->second.thumb;
    }

    Ins decode_arm(uint32_t a) const;
    Ins decode_thumb(uint32_t a) const;
    bool plausible(uint32_t a, bool thumb) const;
    void explore(uint32_t a, bool thumb);
    void drain() {
        while (!work.empty()) {
            auto w = work.back();
            work.pop_back();
            explore(w.first, w.second);
        }
    }
    void run();
};

uint32_t arm_imm(uint32_t raw) {
    unsigned rot = (raw >> 7) & 0x1E;
    uint32_t imm = raw & 0xFF;
    return rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
}

Ins Analysis::decode_arm(uint32_t a) const {
    Ins in;
    in.raw = img.rd32(a);
    in.cond = in.raw >> 28;
    in.cls = (uint8_t)arm_classify(in.raw);
    uint32_t raw = in.raw;
    if (in.cond == 15) { in.cls = ARM_UNDEF; return in; } // "never": a no-op on ARMv4
    switch (in.cls) {
    case ARM_B: in.flow = F_BRANCH; in.target = a + 8 + (uint32_t)(((int32_t)(raw << 8) >> 8) * 4); break;
    case ARM_BL: in.flow = F_CALL; in.target = a + 8 + (uint32_t)(((int32_t)(raw << 8) >> 8) * 4); break;
    case ARM_BX: in.flow = (raw & 15) == 14 ? F_RET : F_TAIL; break;
    case ARM_SWI: in.flow = F_SWI; break;
    case ARM_UNDEF: in.flow = F_INTERP; break;
    case ARM_DP: {
        unsigned op = (raw >> 21) & 15;
        if (((raw >> 12) & 15) != 15 || (op >= 8 && op <= 11)) break;
        if ((raw & 0x00100000u) || (raw & 0x0FFFFFFFu) == 0x01A0F00Eu) in.flow = F_RET; // movs pc,.. / mov pc, lr
        else in.flow = F_JUMP;
        break;
    }
    case ARM_LDRSTR:
        if ((raw & 0x00100000u) && ((raw >> 12) & 15) == 15)
            in.flow = ((raw >> 16) & 15) == 13 ? F_RET : F_JUMP; // ldr pc, [sp], #4
        break;
    case ARM_LDRSTRH:
        if ((raw & 0x00100000u) && ((raw >> 12) & 15) == 15) in.flow = F_JUMP;
        break;
    case ARM_LDM:
        if (raw & 0x8000u) in.flow = F_RET;
        break;
    default: break;
    }
    return in;
}

Ins Analysis::decode_thumb(uint32_t a) const {
    Ins in;
    in.thumb = true;
    in.size = 2;
    in.raw = img.rd16(a);
    in.cls = (uint8_t)thumb_classify(in.raw);
    uint32_t raw = in.raw;
    switch (in.cls) {
    case THUMB_BCOND:
        in.flow = F_BRANCH;
        in.cond = (raw >> 8) & 15;
        in.target = a + 4 + (uint32_t)((int32_t)(int8_t)raw * 2);
        break;
    case THUMB_B: in.flow = F_BRANCH; in.target = a + 4 + (uint32_t)(((int32_t)(raw << 21) >> 21) * 2); break;
    case THUMB_BL_HI:
        if (img.has(a + 2, 2) && thumb_classify(img.rd16(a + 2)) == THUMB_BL_LO) {
            uint32_t lo = img.rd16(a + 2);
            in.size = 4;
            in.flow = F_CALL;
            in.target = a + 4 + ((uint32_t)((int32_t)(raw << 21) >> 21) << 12) + ((lo & 0x7FF) << 1);
            in.raw = raw | (lo << 16);
        } else {
            in.flow = F_INTERP;
        }
        break;
    case THUMB_BL_LO: case THUMB_UNDEF: in.flow = F_INTERP; break;
    case THUMB_SWI: in.flow = F_SWI; break;
    default:
        if ((raw & 0xFF00u) == 0x4700u) in.flow = ((raw >> 3) & 15) == 14 ? F_RET : F_TAIL;         // bx
        else if ((raw & 0xFF87u) == 0x4687u) in.flow = ((raw >> 3) & 15) == 14 ? F_RET : F_JUMP;    // mov pc, r
        else if ((raw & 0xFF87u) == 0x4487u) in.flow = F_JUMP;                                      // add pc, r
        else if ((raw & 0xFF00u) == 0xBD00u) in.flow = F_RET;                                       // pop {.., pc}
        break;
    }
    return in;
}

// Registers an ARM instruction may modify, as a bit mask (conservative).
uint32_t arm_writes(const Ins &in) {
    uint32_t raw = in.raw, m = 0;
    switch (in.cls) {
    case ARM_DP: { unsigned op = (raw >> 21) & 15; if (op < 8 || op > 11) m |= 1u << ((raw >> 12) & 15); break; }
    case ARM_MUL: m |= 1u << ((raw >> 16) & 15); break;
    case ARM_MULL: m |= (1u << ((raw >> 16) & 15)) | (1u << ((raw >> 12) & 15)); break;
    case ARM_SWP: case ARM_MRS: m |= 1u << ((raw >> 12) & 15); break;
    case ARM_LDRSTR: case ARM_LDRSTRH:
        if (raw & 0x00100000u) m |= 1u << ((raw >> 12) & 15);
        if (!(raw & 0x01000000u) || (raw & 0x00200000u)) m |= 1u << ((raw >> 16) & 15);
        break;
    case ARM_LDM: m |= raw & 0xFFFF; // fall through
    case ARM_STM: if (raw & 0x00200000u) m |= 1u << ((raw >> 16) & 15); break;
    case ARM_COP: m |= 1u << ((raw >> 12) & 15); break;
    default: m = 0xFFFF; break;
    }
    return m;
}

uint32_t thumb_writes(const Ins &in) {
    uint32_t raw = in.raw;
    unsigned top = (raw >> 11) & 31;
    if (in.flow == F_CALL || in.flow == F_SWI || in.flow == F_INTERP) return 0xFFFF;
    if (top <= 3 || (top >= 10 && top <= 17)) return 1u << (raw & 7);
    if (top <= 7 || (top >= 18 && top <= 21)) return 1u << ((raw >> 8) & 7);
    if (top == 8) return (raw & 0x0400u) ? 1u << ((raw & 7) | ((raw >> 4) & 8)) : 1u << (raw & 7);
    if (top == 22 || top == 23) return raw & 0xFF;
    if (top == 24 || top == 25) return (raw & 0xFF) | (1u << ((raw >> 8) & 7));
    return 0;
}

bool Analysis::plausible(uint32_t a, bool thumb) const {
    // Would decoding from `a` give something that looks like code?
    unsigned step = thumb ? 2 : 4, n;
    if (a & (step - 1)) return false;
    for (n = 0; n < 24; n++, a += step) {
        if (!img.has(a, step)) return false;
        uint8_t cv = cover[a - img.base];
        if (cv == C_CODE) {
            auto it = code.find(a);
            return it != code.end() && it->second.thumb == thumb;
        }
        if (cv != C_FREE) return false;
        Ins in = thumb ? decode_thumb(a) : decode_arm(a);
        if (in.flow == F_INTERP && !(in.cls == ARM_COP && !thumb)) return false;
        if (!thumb) {
            if (in.raw == 0 || in.cond == 15) return false;
            if (n == 0 && in.cond != 14) return false;
            if (in.cls == ARM_COP && ((in.raw >> 8) & 15) != 15) return false;
            if ((in.flow == F_BRANCH || in.flow == F_CALL) && !img.has(in.target, 4)) return false;
        } else {
            if (n == 0 && (in.raw == 0 || in.raw == 0xFFFF)) return false;
            if (in.size == 4) a += 2;
            if ((in.flow == F_BRANCH || in.flow == F_CALL) && !img.has(in.target, 2)) return false;
        }
        if (in.cond == 14 && (in.flow == F_BRANCH || in.flow == F_RET || in.flow == F_JUMP || in.flow == F_TAIL)) return n > 0 || in.flow != F_JUMP;
    }
    return true;
}

void Analysis::explore(uint32_t a, bool thumb) {
    uint32_t known = 0, val[16] = {0}; // constant propagation within a straight run
    int cmp_reg = -1;
    uint32_t cmp_imm = 0;
    bool lr_is_ret = false; // previous instruction was "mov lr, pc"
    bool first = true;

    auto pointer = [&](uint32_t v) { // a constant that may be a code address
        if (!img.has(v & ~1u, 2)) return;
        if (v & 1) strong.insert({v & ~1u, true});
        else if (!(v & 3)) strong.insert({v, false});
    };

    for (;;) {
        unsigned step = thumb ? 2 : 4;
        if ((a & (step - 1)) || !img.has(a, step)) return;
        if (cov(a) == C_CODE) {
            if (code[a].thumb == thumb) leaders.insert(a);
            return;
        }
        if (cov(a) != C_FREE) return;
        if (!first && leaders.count(a)) known = 0, cmp_reg = -1, lr_is_ret = false;
        first = false;

        Ins in = thumb ? decode_thumb(a) : decode_arm(a);
        if (in.size == 4 && thumb && cov(a + 2) != C_FREE) in = decode_thumb(a), in.size = 2, in.flow = F_INTERP;
        uint32_t next = a + in.size, raw = in.raw;
        bool always = in.cond == 14;

        if (lr_is_ret && (in.flow == F_JUMP || in.flow == F_TAIL || in.flow == F_RET)) in.flow = F_INDCALL;
        // "pop {rX}; bx rX" is how Thumb interworking code returns
        if (thumb && in.flow == F_TAIL) {
            auto prev = code.find(a - 2);
            unsigned rm = (raw >> 3) & 15;
            if (prev != code.end() && prev->second.thumb && rm < 8 && prev->second.raw == (0xBC00u | (1u << rm))) in.flow = F_RET;
        }

        cov(a) = C_CODE;
        for (unsigned i = 1; i < in.size; i++) cov(a + i) = C_TAIL;

        // ---- statically known targets of indirect jumps -------------------
        if (!thumb) {
            if (in.cls == ARM_BX && (raw & 15) != 15 && (known & (1u << (raw & 15)))) {
                uint32_t t = val[raw & 15];
                if (img.has(t & ~1u, 2)) add_func(t & ~1u, t & 1);
            } else if (in.flow == F_JUMP || in.flow == F_INDCALL) {
                if (in.cls == ARM_DP && (raw & 0x0FFFFFF0u) == 0x01A0F000u && (known & (1u << (raw & 15)))) { // mov pc, rX
                    uint32_t t = val[raw & 15] & ~3u;
                    if (img.has(t, 4)) add_func(t, false);
                } else if (in.cls == ARM_LDRSTR && (raw & 0x0F7F0000u) == 0x051F0000u) { // ldr pc, [pc, #imm]
                    uint32_t lit = (raw & 0x00800000u) ? a + 8 + (raw & 0xFFF) : a + 8 - (raw & 0xFFF);
                    if (img.has(lit, 4)) {
                        mark_data(lit, 4);
                        uint32_t t = img.rd32(lit) & ~3u;
                        if (img.has(t, 4)) add_func(t, false);
                    }
                } else if (in.cls == ARM_DP && (raw & 0x0FFFFFF0u) == 0x008FF100u) { // add pc, pc, rX, lsl #2
                    unsigned n = (cmp_reg == (int)(raw & 15)) ? cmp_imm + 1 : 0;
                    for (unsigned i = 0; i < 1024; i++) {
                        uint32_t e = a + 8 + 4 * i;
                        if (!img.has(e, 4)) break;
                        if (n ? i >= n : (img.rd32(e) & 0xFF000000u) != 0xEA000000u) break;
                        add_leader(e, false);
                        tables[a].push_back(e);
                    }
                } else if (in.cls == ARM_LDRSTR && (raw & 0x0FFFFFF0u) == 0x079FF100u) { // ldr pc, [pc, rX, lsl #2]
                    unsigned n = (cmp_reg == (int)(raw & 15)) ? cmp_imm + 1 : 0;
                    for (unsigned i = 0; i < 1024; i++) {
                        uint32_t e = a + 8 + 4 * i;
                        if (!img.has(e, 4) || (n ? i >= n : cov(e) != C_FREE)) break;
                        uint32_t t = img.rd32(e);
                        if ((t & 3) || !img.has(t, 4)) break;
                        mark_data(e, 4);
                        add_leader(t, false);
                        tables[a].push_back(t);
                    }
                }
            }
        } else if (in.flow == F_TAIL || in.flow == F_JUMP || in.flow == F_INDCALL) {
            unsigned rm = (raw >> 3) & 15;
            if (rm == 15 && (raw & 0xFF00u) == 0x4700u && img.has((a + 4) & ~3u, 4)) add_func((a + 4) & ~3u, false); // bx pc: switch to ARM
            if (rm != 15 && (known & (1u << rm))) {
                uint32_t t = val[rm];
                bool tt = (raw & 0xFF00u) == 0x4700u ? (t & 1) : true;
                if (img.has(t & ~1u, 2)) add_func(t & ~1u, tt);
            }
        }

        // ---- constants ----------------------------------------------------
        lr_is_ret = false;
        if (!thumb) {
            uint32_t w = arm_writes(in);
            unsigned rd = (raw >> 12) & 15;
            bool set = false;
            if (always && in.cls == ARM_LDRSTR && (raw & 0x0F7F0000u) == 0x051F0000u && rd != 15) { // literal load
                uint32_t lit = (raw & 0x00800000u) ? a + 8 + (raw & 0xFFF) : a + 8 - (raw & 0xFFF);
                if (img.has(lit, 4)) {
                    mark_data(lit, 4);
                    val[rd] = img.rd32(lit);
                    set = true;
                    pointer(val[rd]);
                }
            } else if (always && in.cls == ARM_DP && (raw & 0x0FF00000u) == 0x03A00000u && rd != 15) { // mov rd, #imm
                val[rd] = arm_imm(raw);
                set = true;
            } else if (always && in.cls == ARM_DP && (raw & 0x0FFF0000u) == 0x028F0000u && rd != 15) { // add rd, pc, #imm
                val[rd] = a + 8 + arm_imm(raw);
                set = true;
                pointer(val[rd]);
            } else if (always && in.cls == ARM_DP && (raw & 0x0FFF0000u) == 0x024F0000u && rd != 15) { // sub rd, pc, #imm
                val[rd] = a + 8 - arm_imm(raw);
                set = true;
                pointer(val[rd]);
            }
            if (in.cls == ARM_DP && (raw & 0x0FF0F000u) == 0x03500000u) { // cmp rn, #imm
                cmp_reg = (raw >> 16) & 15;
                cmp_imm = arm_imm(raw);
            } else if (cmp_reg >= 0 && (w & (1u << cmp_reg))) {
                cmp_reg = -1;
            }
            known &= ~w;
            if (set) known |= 1u << rd;
            if (always && (raw & 0x0FFFFFFFu) == 0x01A0E00Fu) lr_is_ret = true; // mov lr, pc
        } else {
            unsigned top = raw >> 11;
            if (top == 9) { // ldr rX, [pc, #imm]
                unsigned rd = (raw >> 8) & 7;
                uint32_t lit = ((a + 4) & ~2u) + ((raw & 0xFF) << 2);
                known &= ~(1u << rd);
                if (img.has(lit, 4)) {
                    mark_data(lit, 4);
                    val[rd] = img.rd32(lit);
                    known |= 1u << rd;
                    pointer(val[rd]);
                }
            } else if (top == 20) { // add rX, pc, #imm
                unsigned rd = (raw >> 8) & 7;
                val[rd] = ((a + 4) & ~2u) + ((raw & 0xFF) << 2);
                known |= 1u << rd;
            } else if (top == 4) {
                val[(raw >> 8) & 7] = raw & 0xFF;
                known |= 1u << ((raw >> 8) & 7);
            } else if ((raw & 0xFF00u) == 0x4600u) { // mov between any registers
                unsigned hd = (raw & 7) | ((raw >> 4) & 8), hs = (raw >> 3) & 15;
                if (hs != 15 && (known & (1u << hs))) val[hd] = val[hs], known |= 1u << hd;
                else known &= ~(1u << hd);
            } else {
                known &= ~thumb_writes(in);
            }
        }

        // ---- control flow -------------------------------------------------
        switch (in.flow) {
        case F_BRANCH:
            if (img.has(in.target, step)) add_leader(in.target, thumb);
            break;
        case F_CALL:
            if (img.has(in.target, step)) add_func(in.target, thumb);
            leaders.insert(next);
            known = 0;
            cmp_reg = -1;
            break;
        case F_INDCALL: case F_SWI:
            leaders.insert(next);
            known = 0;
            cmp_reg = -1;
            break;
        default: break;
        }
        code[a] = in;
        bool stops = always && (in.flow == F_BRANCH || in.flow == F_RET || in.flow == F_JUMP || in.flow == F_TAIL);
        if (in.flow == F_INTERP && !(in.cls == ARM_COP && !thumb)) stops = true;
        if (stops) return;
        a = next;
    }
}

void Analysis::run() {
    add_func(img.entry, false);
    for (auto &f : opt.extra_funcs) add_func(f.first, f.second);
    drain();
    if (opt.scan) {
        // Addresses loaded by code, then every word of data that points into
        // the image: callbacks, jump tables, vtables, interrupt handlers.
        for (int pass = 0; pass < 2; pass++) {
            bool again = true;
            while (again) {
                again = false;
                std::vector<std::pair<uint32_t, bool>> cand;
                if (pass == 0) {
                    cand.assign(strong.begin(), strong.end());
                } else {
                    for (uint32_t a = img.base; img.has(a, 4); a += 4) {
                        if (cov(a) == C_CODE || cov(a) == C_TAIL) continue;
                        uint32_t v = img.rd32(a);
                        if (!img.has(v & ~1u, 4) || (v & 3) == 2) continue;
                        if (v & 1) cand.emplace_back(v & ~1u, true);
                        else cand.emplace_back(v, mode_near(v));
                    }
                }
                for (auto &c : cand) {
                    if (cov(c.first) != C_FREE || !plausible(c.first, c.second)) continue;
                    add_func(c.first, c.second);
                    drain();
                    again = true;
                }
            }
        }
    }
    // every block start that turned out to be code is an entry point
    for (auto it = leaders.begin(); it != leaders.end();)
        it = code.count(*it) ? std::next(it) : leaders.erase(it);
    for (auto it = funcs.begin(); it != funcs.end();)
        it = code.count(*it) ? std::next(it) : funcs.erase(it);
}

// ---------------------------------------------------------------------------
// Grouping blocks into C functions
// ---------------------------------------------------------------------------
struct Block {
    uint32_t addr = 0, end = 0;
    bool thumb = false;
    unsigned count = 0;
    uint32_t owner = 0;
    bool falls = false; // control can run off the end into `end`
};

struct Program {
    std::map<uint32_t, Block> blocks;
    std::map<uint32_t, std::vector<uint32_t>> funcs; // entry -> blocks, ascending
};

void build_blocks(Analysis &an, Program &p) {
    for (uint32_t l : an.leaders) {
        Block b;
        b.addr = l;
        b.thumb = an.code[l].thumb;
        uint32_t a = l;
        for (;;) {
            auto it = an.code.find(a);
            if (it == an.code.end()) { b.falls = true; break; }
            const Ins &in = it->second;
            b.count++;
            a += in.size;
            bool always = in.cond == 14;
            if (always && (in.flow == F_BRANCH || in.flow == F_RET || in.flow == F_JUMP || in.flow == F_TAIL)) break;
            if (in.flow == F_INTERP && !(in.cls == ARM_COP && !in.thumb)) break;
            if (an.leaders.count(a)) { b.falls = true; break; }
        }
        b.end = a;
        p.blocks[l] = b;
    }
    // A function is its entry plus every block reachable from it without
    // passing through another function's entry.
    const unsigned limit = an.opt.max_func_blocks;
    for (uint32_t f : an.funcs) p.blocks[f].owner = f;
    auto grow = [&](uint32_t f) {
        std::vector<uint32_t> stack{f};
        std::vector<uint32_t> &mine = p.funcs[f];
        while (!stack.empty()) {
            uint32_t ba = stack.back();
            stack.pop_back();
            Block &b = p.blocks[ba];
            mine.push_back(ba);
            auto visit = [&](uint32_t t) {
                auto it = p.blocks.find(t);
                if (it == p.blocks.end() || it->second.owner || it->second.thumb != b.thumb || mine.size() + stack.size() >= limit) return;
                it->second.owner = f;
                stack.push_back(t);
            };
            for (uint32_t a = b.addr; a < b.end;) {
                const Ins &in = an.code[a];
                if (in.flow == F_BRANCH) visit(in.target);
                if (in.flow == F_CALL || in.flow == F_INDCALL || in.flow == F_SWI) visit(a + in.size);
                if (in.flow == F_JUMP) {
                    auto tb = an.tables.find(a);
                    if (tb != an.tables.end())
                        for (uint32_t t : tb->second) visit(t);
                }
                a += in.size;
            }
            if (b.falls) visit(b.end);
        }
        std::sort(mine.begin(), mine.end());
    };
    for (uint32_t f : an.funcs) grow(f);
    // jump-table targets and blocks cut off by the size limit start functions of their own
    for (auto &kv : p.blocks)
        if (!kv.second.owner) {
            kv.second.owner = kv.first;
            grow(kv.first);
        }
}

// ---------------------------------------------------------------------------
// C output
// ---------------------------------------------------------------------------
struct Out {
    FILE *f = nullptr;
    void open(const std::string &path) {
        f = fopen(path.c_str(), "w");
        if (!f) die("cannot write %s", path.c_str());
    }
    void close() { if (f) fclose(f); f = nullptr; }
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    void p(const char *fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
    }
};

struct Emitter {
    Analysis &an;
    Program &prog;
    Out o;
    uint32_t cur_fn = 0;
    bool uses_dispatch = false;
    std::string body;

    void line(const char *fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        body += buf;
    }

    bool local(uint32_t t, bool thumb) const {
        auto it = prog.blocks.find(t);
        return it != prog.blocks.end() && it->second.owner == cur_fn && it->second.thumb == thumb;
    }
    // text of a jump to a known address
    std::string go(uint32_t t, bool thumb) const {
        char buf[96];
        if (local(t, thumb)) snprintf(buf, sizeof buf, "goto L_%08X;", t);
        else snprintf(buf, sizeof buf, "{ c->r[15] = 0x%08Xu; GP32_TAIL(); }", t);
        return buf;
    }

    void emit_ins(uint32_t a, const Ins &in) {
        const char *ind = "    ";
        uint32_t next = a + in.size, raw = in.raw;
        if (in.thumb) {
            switch (in.flow) {
            case F_BRANCH:
                if (in.cond != 14) line("%sif (gp32_cond(c, %u)) %s\n", ind, in.cond, go(in.target, true).c_str());
                else line("%s%s\n", ind, go(in.target, true).c_str());
                return;
            case F_CALL: {
                auto it = prog.blocks.find(in.target);
                line("%sc->r[14] = 0x%08Xu;\n", ind, next | 1);
                if (it != prog.blocks.end() && it->second.thumb && an.funcs.count(in.target))
                    line("%sGP32_CALL(fn_%08X, 0x%08Xu, 0x%08Xu);\n", ind, in.target, in.target, next);
                else
                    line("%sc->r[15] = 0x%08Xu;\n%sGP32_CALL_IND(0x%08Xu);\n", ind, in.target, ind, next);
                return;
            }
            case F_SWI: line("%sGP32_SWI(0x%Xu, 0x%08Xu);\n", ind, raw & 0xFF, next); return;
            case F_INTERP: line("%sGP32_INTERP(0x%08Xu, 0x%08Xu);\n", ind, a, next); return;
            default: break;
            }
            line("%sthumb_g%u(c, 0x%04Xu, 0x%08Xu);\n", ind, thumb_group(raw), raw, a + 4);
            switch (in.flow) {
            case F_RET: line("%sreturn;\n", ind); break;
            case F_TAIL: line("%sGP32_TAIL();\n", ind); break;
            case F_JUMP: line("%sgoto L_dispatch;\n", ind); uses_dispatch = true; break;
            case F_INDCALL: line("%sGP32_CALL_IND(0x%08Xu);\n", ind, next); break;
            default: break;
            }
            return;
        }

        std::string op;
        char buf[160];
        switch (in.cls) {
        case ARM_DP: snprintf(buf, sizeof buf, "arm_dp(c, 0x%08Xu, 0x%08Xu);", raw, a + 8); break;
        case ARM_MUL: snprintf(buf, sizeof buf, "arm_mul(c, 0x%08Xu);", raw); break;
        case ARM_MULL: snprintf(buf, sizeof buf, "arm_mull(c, 0x%08Xu);", raw); break;
        case ARM_SWP: snprintf(buf, sizeof buf, "arm_swp(c, 0x%08Xu);", raw); break;
        case ARM_BX: snprintf(buf, sizeof buf, "arm_bx(c, 0x%08Xu, 0x%08Xu);", raw, a + 8); break;
        case ARM_MRS: snprintf(buf, sizeof buf, "arm_mrs(c, 0x%08Xu);", raw); break;
        case ARM_MSR: snprintf(buf, sizeof buf, "arm_msr(c, 0x%08Xu);", raw); break;
        case ARM_LDRSTR: snprintf(buf, sizeof buf, "arm_ldrstr(c, 0x%08Xu, 0x%08Xu);", raw, a + 8); break;
        case ARM_LDRSTRH: snprintf(buf, sizeof buf, "arm_ldrstrh(c, 0x%08Xu, 0x%08Xu);", raw, a + 8); break;
        case ARM_LDM: snprintf(buf, sizeof buf, "arm_ldm(c, 0x%08Xu);", raw); break;
        case ARM_STM: snprintf(buf, sizeof buf, "arm_stm(c, 0x%08Xu, 0x%08Xu);", raw, a + 8); break;
        case ARM_COP: snprintf(buf, sizeof buf, "gp32_coproc(c, 0x%08Xu);", raw); break;
        default: buf[0] = 0; break;
        }
        op = buf;

        switch (in.flow) {
        case F_BRANCH: op = go(in.target, false); break;
        case F_CALL: {
            auto it = prog.blocks.find(in.target);
            if (it != prog.blocks.end() && !it->second.thumb && an.funcs.count(in.target))
                snprintf(buf, sizeof buf, "c->r[14] = 0x%08Xu; GP32_CALL(fn_%08X, 0x%08Xu, 0x%08Xu);", next, in.target, in.target, next);
            else
                snprintf(buf, sizeof buf, "c->r[14] = 0x%08Xu; c->r[15] = 0x%08Xu; GP32_CALL_IND(0x%08Xu);", next, in.target, next);
            op = buf;
            break;
        }
        case F_SWI: snprintf(buf, sizeof buf, "GP32_SWI(0x%Xu, 0x%08Xu);", raw & 0x00FFFFFFu, next); op = buf; break;
        case F_INTERP: snprintf(buf, sizeof buf, "GP32_INTERP(0x%08Xu, 0x%08Xu);", a, next); op = buf; break;
        case F_RET: op += " return;"; break;
        case F_TAIL: op += " GP32_TAIL();"; break;
        case F_JUMP: op += " goto L_dispatch;"; uses_dispatch = true; break;
        case F_INDCALL:
            snprintf(buf, sizeof buf, " GP32_CALL_IND(0x%08Xu);", next);
            op += buf;
            break;
        default: break;
        }
        if (in.flow == F_INTERP && in.cond != 15) {
            line("%s%s\n", ind, op.c_str()); // the interpreter evaluates the condition itself
        } else if (in.cond == 14) {
            if (in.flow == F_NONE || in.flow == F_SWI || in.flow == F_BRANCH) line("%s%s\n", ind, op.c_str());
            else line("%s{ %s }\n", ind, op.c_str());
        } else if (in.cond != 15) {
            line("%sif (gp32_cond(c, %u)) { %s }\n", ind, in.cond, op.c_str());
        }
    }

    void emit_function(uint32_t f) {
        const std::vector<uint32_t> &blocks = prog.funcs[f];
        cur_fn = f;
        uses_dispatch = false;
        body.clear();
        for (size_t i = 0; i < blocks.size(); i++) {
            const Block &b = prog.blocks[blocks[i]];
            line("L_%08X:\n    GP32_BLOCK(0x%08Xu, %u)\n", b.addr, b.addr, b.count);
            for (uint32_t a = b.addr; a < b.end;) {
                const Ins &in = an.code[a];
                emit_ins(a, in);
                a += in.size;
            }
            if (b.falls) {
                bool next_here = i + 1 < blocks.size() && blocks[i + 1] == b.end && local(b.end, b.thumb);
                if (!next_here) {
                    if (prog.blocks.count(b.end)) line("    %s\n", go(b.end, b.thumb).c_str());
                    else line("    c->r[15] = 0x%08Xu;\n    return;\n", b.end);
                }
            } else {
                // a block that ends in an interpreted instruction never falls through
                const Ins &last = std::prev(an.code.upper_bound(b.end - 1))->second;
                if (last.flow == F_INTERP) line("    return;\n");
            }
        }
        o.p("void fn_%08X(gp32_cpu *c) {\n", f);
        if (uses_dispatch) o.p("L_dispatch:\n");
        o.p("    switch (c->r[15]) {\n");
        for (uint32_t b : blocks) o.p("    case 0x%08Xu: goto L_%08X;\n", b, b);
        o.p("    default: GP32_TAIL();\n    }\n");
        fputs(body.c_str(), o.f);
        o.p("}\n\n");
    }
};

std::string c_ident(const std::string &s) {
    std::string r;
    for (char ch : s) r += (isalnum((unsigned char)ch) ? ch : '_');
    if (r.empty() || isdigit((unsigned char)r[0])) r = "game_" + r;
    return r;
}

} // namespace

int recompile(const Options &opt) {
    Image img;
    load_image(opt.input, img);
    img.base = opt.base;
    img.entry = opt.entry ? opt.entry : opt.base;

    Analysis an(img, opt);
    an.run();
    Program prog;
    build_blocks(an, prog);

    std::string name = c_ident(opt.name);
    std::string dir = opt.outdir + "/";
    make_dir(opt.outdir);

    // ---- translated code, split into several files -------------------------
    Emitter em{an, prog, {}, 0, false, {}};
    std::vector<std::string> sources;
    unsigned in_file = 0, file_no = 0;
    size_t total_ins = an.code.size(), thumb_ins = 0;
    for (auto &kv : an.code) thumb_ins += kv.second.thumb;
    for (auto &kv : prog.funcs) {
        if (!em.o.f) {
            char fn[64];
            snprintf(fn, sizeof fn, "%s_code%02u.c", name.c_str(), file_no++);
            sources.push_back(fn);
            em.o.open(dir + fn);
            em.o.p("/* Generated by gp32recomp from %s - do not edit. */\n#include \"gp32_ops.h\"\n#include \"%s_funcs.h\"\n\n",
                   opt.input_name.c_str(), name.c_str());
            in_file = 0;
        }
        em.emit_function(kv.first);
        for (uint32_t b : kv.second) in_file += prog.blocks[b].count;
        if (in_file >= opt.ins_per_file) em.o.close();
    }
    em.o.close();

    Out h;
    h.open(dir + name + "_funcs.h");
    h.p("/* Generated by gp32recomp - do not edit. */\n#ifndef GP32_FUNCS_H\n#define GP32_FUNCS_H\n#include \"gp32rt.h\"\n");
    for (auto &kv : prog.funcs) h.p("void fn_%08X(gp32_cpu *c);\n", kv.first);
    h.p("#endif\n");
    h.close();

    // ---- image, entry table and main() --------------------------------------
    Out m;
    std::string main_c = name + "_main.c";
    sources.push_back(main_c);
    m.open(dir + main_c);
    m.p("/* Generated by gp32recomp from %s - do not edit. */\n#include \"gp32rt.h\"\n#include \"%s_funcs.h\"\n\n", opt.input_name.c_str(), name.c_str());
    m.p("static const gp32_entry entries[] = {\n");
    size_t nent = 0;
    for (auto &kv : prog.blocks) {
        m.p("    { 0x%08Xu, fn_%08X },\n", kv.first | (kv.second.thumb ? 1u : 0u), kv.second.owner);
        nent++;
    }
    m.p("};\n\nstatic const uint8_t image[%zu] = {", img.data.size());
    for (size_t i = 0; i < img.data.size(); i++) m.p("%s%u,", (i % 24) ? "" : "\n", img.data[i]);
    m.p("\n};\n\n");
    std::string title = img.title.empty() ? opt.name : img.title;
    std::string esc;
    for (char ch : title) if (ch >= 32 && ch < 127 && ch != '"' && ch != '\\') esc += ch;
    m.p("static const gp32_game game = {\n    \"%s\", 0x%08Xu, 0x%08Xu, %zuu, image, entries, %zuu\n};\n\n", esc.c_str(), img.base, img.entry,
        img.data.size(), nent);
    m.p("int main(int argc, char **argv) { return gp32_main(argc, argv, &game); }\n");
    m.close();

    // ---- build files ---------------------------------------------------------
    write_runtime(opt, dir + "runtime");
    Out cm;
    cm.open(dir + "CMakeLists.txt");
    cm.p("# Generated by gp32recomp.  Build with:\n#   cmake -B build -DCMAKE_BUILD_TYPE=Release\n#   cmake --build build --config Release\n");
    cm.p("cmake_minimum_required(VERSION 3.15)\nproject(%s C)\n", name.c_str());
    cm.p("if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)\n  set(CMAKE_BUILD_TYPE Release)\nendif()\n");
    cm.p("add_subdirectory(runtime)\nadd_executable(%s\n", name.c_str());
    for (auto &s : sources) cm.p("  %s\n", s.c_str());
    cm.p(")\ntarget_link_libraries(%s PRIVATE gp32rt)\n", name.c_str());
    cm.p("if(MSVC)\n  target_link_options(%s PRIVATE /STACK:16777216)\nelseif(MINGW)\n  target_link_options(%s PRIVATE -Wl,--stack,16777216)\nendif()\n",
         name.c_str(), name.c_str());
    cm.close();

    printf("%s: %zu bytes at %08X\n", opt.input_name.c_str(), img.data.size(), img.base);
    printf("  %zu instructions translated (%zu Thumb), %zu blocks, %zu functions, %zu source files\n", total_ins, thumb_ins, prog.blocks.size(),
           prog.funcs.size(), sources.size());
    size_t code_bytes = 0;
    for (uint8_t cv : an.cover) code_bytes += (cv == C_CODE || cv == C_TAIL);
    printf("  %.1f%% of the image is code; anything missed runs in the interpreter\n", 100.0 * code_bytes / img.data.size());
    printf("  wrote %s\n", opt.outdir.c_str());
    return 0;
}
