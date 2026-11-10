// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

#include <fmt/format.h>

#ifndef _WIN32
#include <pthread.h>
#endif

#include "common/nx2_kit_stall.h"

namespace Common::KitStall {
namespace {

constexpr size_t MaxThreads = 64;
constexpr int MaxDepth = 4;

struct Mark {
    std::atomic<State> state{State::Waits};
    std::atomic<const char*> lock{};
    std::atomic<const char*> op{};
    std::atomic<u64> since_ns{};
};

/// One thread's marks. Only the owner writes them; the watchdog reads them.
struct Record {
    std::atomic<bool> used{};
    std::array<char, 40> name{};
    std::atomic<int> depth{};
    std::array<Mark, MaxDepth> marks{};
};

std::array<Record, MaxThreads> records;

std::atomic<int> hooks_enabled{-1};

/// The calling thread's record, claimed on first use and freed when the thread ends.
struct Owner {
    Record* record{};
    ~Owner() {
        if (record != nullptr) {
            record->depth.store(0, std::memory_order_release);
            record->used.store(false, std::memory_order_release);
        }
    }
};

Record* MyRecord() {
    thread_local Owner owner;
    if (owner.record != nullptr) {
        return owner.record;
    }
    for (Record& record : records) {
        bool expected = false;
        if (!record.used.load(std::memory_order_relaxed) &&
            record.used.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            char name[32] = "";
#ifndef _WIN32
            pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
            const auto id = std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000;
            const auto text = fmt::format("{}#{}", name, id);
            std::memset(record.name.data(), 0, record.name.size());
            std::memcpy(record.name.data(), text.data(), (std::min)(text.size(), record.name.size() - 1));
            record.depth.store(0, std::memory_order_release);
            owner.record = &record;
            return owner.record;
        }
    }
    return nullptr;
}

} // Anonymous namespace

std::atomic<void (*)()> g_first_stall_hook{};

bool HooksEnabled() noexcept {
    int enabled = hooks_enabled.load(std::memory_order_relaxed);
    if (enabled < 0) {
        const char* const text = std::getenv("EDEN_NX2_KIT_HOOKS");
        enabled = text != nullptr && text[0] == '0' ? 0 : 1;
        hooks_enabled.store(enabled, std::memory_order_relaxed);
    }
    return enabled != 0;
}

void SetHooksEnabled(bool enabled) noexcept {
    hooks_enabled.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

u64 NowNs() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

Scope::Scope(const char* lock, const char* op) noexcept : index{-1} {
    Record* const record = MyRecord();
    if (record == nullptr) {
        return;
    }
    const int depth = record->depth.load(std::memory_order_relaxed);
    if (depth >= MaxDepth) {
        return;
    }
    Mark& mark = record->marks[depth];
    mark.state.store(State::Waits, std::memory_order_relaxed);
    mark.lock.store(lock, std::memory_order_relaxed);
    mark.op.store(op, std::memory_order_relaxed);
    mark.since_ns.store(NowNs(), std::memory_order_relaxed);
    record->depth.store(depth + 1, std::memory_order_release);
    index = depth;
}

void Scope::Held() noexcept {
    if (index < 0) {
        return;
    }
    Mark& mark = MyRecord()->marks[index];
    mark.since_ns.store(NowNs(), std::memory_order_relaxed);
    mark.state.store(State::Holds, std::memory_order_release);
}

Scope::~Scope() {
    if (index >= 0) {
        MyRecord()->depth.store(index, std::memory_order_release);
    }
}

std::vector<Stall> Check(u64 limit_ns, u64 now_ns) {
    struct Seen {
        const char* thread;
        State state;
        const char* lock;
        const char* op;
        u64 since_ns;
        u64 key;
    };
    std::vector<Seen> seen;
    for (size_t index = 0; index < records.size(); ++index) {
        const Record& record = records[index];
        if (!record.used.load(std::memory_order_acquire)) {
            continue;
        }
        const int depth = (std::min)(record.depth.load(std::memory_order_acquire), MaxDepth);
        for (int level = 0; level < depth; ++level) {
            const Mark& mark = record.marks[level];
            const char* const lock = mark.lock.load(std::memory_order_relaxed);
            const char* const op = mark.op.load(std::memory_order_relaxed);
            const u64 since = mark.since_ns.load(std::memory_order_relaxed);
            if (lock == nullptr || op == nullptr || since == 0) {
                continue;
            }
            seen.push_back({record.name.data(), mark.state.load(std::memory_order_acquire), lock,
                            op, since, (u64{index} << 40) ^ (u64(level) << 36) ^ since});
        }
    }
    const auto seconds = [now_ns](u64 since) {
        return static_cast<double>(now_ns > since ? now_ns - since : 0) / 1e9;
    };
    std::vector<Stall> stalls;
    for (const Seen& mark : seen) {
        if (now_ns <= mark.since_ns || now_ns - mark.since_ns < limit_ns) {
            continue;
        }
        std::string text =
            mark.state == State::Waits
                ? fmt::format("{} has waited {:.1f} s for {} in {}", mark.thread,
                              seconds(mark.since_ns), mark.lock, mark.op)
                : fmt::format("{} has held {} for {:.1f} s in {}", mark.thread, mark.lock,
                              seconds(mark.since_ns), mark.op);
        if (mark.state == State::Waits) {
            for (const Seen& other : seen) {
                if (&other != &mark && other.state == State::Holds &&
                    std::strcmp(other.lock, mark.lock) == 0) {
                    text += fmt::format("; held by {} in {} for {:.1f} s", other.thread, other.op,
                                        seconds(other.since_ns));
                }
            }
        }
        stalls.push_back({std::move(text), mark.key});
    }
    return stalls;
}

} // namespace Common::KitStall
