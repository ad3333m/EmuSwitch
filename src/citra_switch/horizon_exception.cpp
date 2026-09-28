// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// The C side of the replacement __libnx_exception_entry.

#include <cstddef>
#include <cstdint>
#include <unistd.h>

#include <switch.h>

#include "horizon_exception_entry.h"

#ifndef DEKOPON_VERSION
#define DEKOPON_VERSION "unknown"
#endif

// The stub in horizon_exception_entry.S builds a ThreadExceptionDump from fixed offsets. If libnx
// ever moves a field, these catch it at compile time instead of leaving a dispatcher that silently
// never recognises a fault.
static_assert(offsetof(ThreadExceptionDump, error_desc) == DUMP_ERROR_DESC);
static_assert(offsetof(ThreadExceptionDump, cpu_gprs) == DUMP_GPRS);
static_assert(offsetof(ThreadExceptionDump, fp) == DUMP_FP);
static_assert(offsetof(ThreadExceptionDump, lr) == DUMP_LR);
static_assert(offsetof(ThreadExceptionDump, sp) == DUMP_SP);
static_assert(offsetof(ThreadExceptionDump, pc) == DUMP_PC);
static_assert(offsetof(ThreadExceptionDump, fpu_gprs) == DUMP_FPU);
static_assert(offsetof(ThreadExceptionDump, pstate) == DUMP_PSTATE);
static_assert(offsetof(ThreadExceptionDump, far) == DUMP_FAR);

extern "C" bool DekoponFastmemArenaContains(std::uintptr_t addr);
extern "C" bool DynarmicHorizonHandleFastmemFault(std::uint64_t host_pc, std::uint64_t* new_pc);
extern "C" void _start();
// The last log lines (common/logging/backend.cpp), oldest first.
extern "C" std::size_t DekoponRecentLogLines(const char** lines, std::size_t max_lines,
                                             std::size_t* width);

namespace {
// ESR exception classes for a data abort taken from a lower or the current exception level.
constexpr std::uint32_t ESR_EC_DATA_ABORT_LOWER = 0x24;
constexpr std::uint32_t ESR_EC_DATA_ABORT_SAME = 0x25;

constexpr const char* CRASH_PATH = "/switch/dekopon/log/crash.txt";

char s_report[16384];
std::size_t s_report_len;

void Put(char c) {
    if (s_report_len < sizeof(s_report)) {
        s_report[s_report_len++] = c;
    }
}

void Put(const char* text) {
    while (*text != '\0') {
        Put(*text++);
    }
}

void PutLine(const char* text, std::size_t max_chars) {
    for (std::size_t i = 0; i < max_chars && text[i] != '\0'; ++i) {
        Put(text[i]);
    }
    Put("\n");
}

void PutHex(std::uint64_t value, int digits) {
    static constexpr char HEX[] = "0123456789abcdef";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
        Put(HEX[(value >> shift) & 0xF]);
    }
}

void PutField(const char* name, std::uint64_t value) {
    Put(name);
    Put(" = ");
    PutHex(value, 16);
    Put("\n");
}

void PutOffset(std::uint64_t value, std::uintptr_t base) {
    if (value >= base) {
        Put("  (+0x");
        PutHex(value - base, 8);
        Put(")");
    }
}

void PutCodeField(const char* name, std::uint64_t value, std::uintptr_t base) {
    Put(name);
    Put(" = ");
    PutHex(value, 16);
    PutOffset(value, base);
    Put("\n");
}

// True if [addr, addr + size) is mapped and readable, so the report can follow pointers taken
// from a thread that may have crashed on a bad one.
bool IsReadable(std::uint64_t addr, std::uint64_t size) {
    MemoryInfo info{};
    u32 page_info = 0;
    if (R_FAILED(svcQueryMemory(&info, &page_info, addr))) {
        return false;
    }
    return info.type != MemType_Unmapped && (info.perm & Perm_R) != 0 &&
           addr + size <= info.addr + info.size;
}

// Follows the frame records (x29) up the crashing thread's stack: the return address into each
// caller, innermost first. Leaf functions keep no record, so the crashing one may only be in pc.
void PutBacktrace(const ThreadExceptionDump* ctx, std::uintptr_t base) {
    Put("backtrace:\n");
    std::uint64_t fp = ctx->fp.x;
    for (int depth = 0; depth < 32; ++depth) {
        if (fp == 0 || (fp & 7) != 0 || !IsReadable(fp, 16)) {
            break;
        }
        const auto* record = reinterpret_cast<const std::uint64_t*>(fp);
        const std::uint64_t caller_fp = record[0];
        const std::uint64_t return_address = record[1];
        if (return_address == 0) {
            break;
        }
        Put("  #");
        Put(static_cast<char>('0' + depth / 10));
        Put(static_cast<char>('0' + depth % 10));
        Put(" ");
        PutHex(return_address, 16);
        PutOffset(return_address, base);
        Put("\n");
        // Callers' records sit higher up the stack; anything else is a broken chain.
        if (caller_fp <= fp) {
            break;
        }
        fp = caller_fp;
    }
}

// What was logged just before the crash, which the log file on the SD card usually misses.
void PutRecentLog() {
    const char* lines[64];
    std::size_t width = 0;
    const std::size_t count = DekoponRecentLogLines(lines, sizeof(lines) / sizeof(lines[0]), &width);
    Put("recent log:\n");
    for (std::size_t i = 0; i < count; ++i) {
        Put("  ");
        PutLine(lines[i], width);
    }
}

void BuildReport(ThreadExceptionDump* ctx) {
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(&_start);

    Put("EmuSwitch " DEKOPON_VERSION ": unhandled CPU exception\n");
    PutField("module base", base);

    if (!threadExceptionIsAArch64(ctx)) {
        PutField("aarch32 pc ", ctx->pc.w);
        return;
    }

    PutField("error_desc ", ctx->error_desc);
    PutField("esr        ", ctx->esr);
    PutCodeField("pc         ", ctx->pc.x, base);
    PutCodeField("lr         ", ctx->lr.x, base);
    PutField("sp         ", ctx->sp.x);
    PutField("fp         ", ctx->fp.x);
    PutField("far        ", ctx->far.x);
    PutField("pstate     ", ctx->pstate);

    for (int i = 0; i < 29; ++i) {
        Put("x");
        Put(i < 10 ? ' ' : static_cast<char>('0' + i / 10));
        Put(static_cast<char>('0' + i % 10));
        Put("         = ");
        PutHex(ctx->cpu_gprs[i].x, 16);
        Put("\n");
    }

    PutBacktrace(ctx, base);
    PutRecentLog();
}

void WriteReportToSd() {
    FsFileSystem* sdmc = fsdevGetDeviceFileSystem("sdmc");
    if (sdmc == nullptr) {
        return;
    }

    fsFsCreateDirectory(sdmc, "/switch");
    fsFsCreateDirectory(sdmc, "/switch/dekopon");
    fsFsCreateDirectory(sdmc, "/switch/dekopon/log");
    fsFsDeleteFile(sdmc, CRASH_PATH);
    if (R_FAILED(fsFsCreateFile(sdmc, CRASH_PATH, static_cast<s64>(s_report_len), 0))) {
        return;
    }

    FsFile file;
    if (R_FAILED(fsFsOpenFile(sdmc, CRASH_PATH, FsOpenMode_Write, &file))) {
        return;
    }
    fsFileWrite(&file, 0, s_report, s_report_len, FsWriteOption_Flush);
    fsFileClose(&file);
}
}  // namespace

extern "C" bool HorizonExceptionDispatch(ThreadExceptionDump* ctx) {
    if (!threadExceptionIsAArch64(ctx)) {
        return false;
    }

    const std::uint32_t exception_class = (ctx->esr >> 26) & 0x3F;
    if (exception_class != ESR_EC_DATA_ABORT_LOWER && exception_class != ESR_EC_DATA_ABORT_SAME) {
        return false;
    }

    if (!DekoponFastmemArenaContains(static_cast<std::uintptr_t>(ctx->far.x))) {
        return false;
    }

    std::uint64_t new_pc = 0;
    if (!DynarmicHorizonHandleFastmemFault(ctx->pc.x, &new_pc)) {
        return false;
    }

    ctx->pc.x = new_pc;
    return true;
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    BuildReport(ctx);
    WriteReportToSd();
    write(STDERR_FILENO, s_report, s_report_len);
}
