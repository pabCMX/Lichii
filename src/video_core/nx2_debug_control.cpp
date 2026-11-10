// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "core/core.h"
#include "video_core/nx2_debug_control.h"

namespace VideoCore::Nx2Debug {
namespace {

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<CaptureRequest> captures;
    std::unordered_map<u64, CaptureResult> finished;
    std::deque<GpuReadRequest> gpu_reads;
    std::unordered_map<u64, GpuReadResult> gpu_read_results;
    std::vector<u64> rejected;
    u64 reject_generation{};
    u64 applied_generation{};
    bool reject_pending{};
    bool unrejected{};
    bool open{};
};

State state;
std::atomic<bool> pending{};
std::atomic<bool> any_rejected{};

struct LumaWatch {
    std::mutex mutex;
    std::vector<LumaSample> samples;
};
LumaWatch luma_watch;
std::atomic<u32> luma_remaining{};

void UpdatePending() {
    pending.store(!state.captures.empty() || !state.gpu_reads.empty() || state.reject_pending, std::memory_order_relaxed);
}

} // Anonymous namespace

void BeginSession() {
    std::scoped_lock lock{state.mutex};
    state.captures.clear();
    state.finished.clear();
    state.gpu_reads.clear();
    state.gpu_read_results.clear();
    state.rejected.clear();
    state.reject_pending = false;
    state.unrejected = false;
    state.applied_generation = state.reject_generation;
    state.open = true;
    any_rejected.store(false, std::memory_order_relaxed);
    UpdatePending();
}

void EndSession() {
    ArmLumaWatch(0);
    {
        std::scoped_lock lock{state.mutex};
        state.open = false;
        state.captures.clear();
        state.gpu_reads.clear();
        UpdatePending();
    }
    state.changed.notify_all();
}

bool HasRequests() noexcept {
    return pending.load(std::memory_order_relaxed);
}

void RequestCapture(CaptureRequest request) {
    std::scoped_lock lock{state.mutex};
    state.captures.push_back(std::move(request));
    UpdatePending();
}

void CancelCapture(u64 id) {
    std::scoped_lock lock{state.mutex};
    std::erase_if(state.captures, [id](const CaptureRequest& request) { return request.id == id; });
    state.finished.erase(id);
    UpdatePending();
}

std::optional<CaptureResult> WaitCapture(u64 id, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    state.changed.wait_for(lock, timeout, [id] { return !state.open || state.finished.contains(id); });
    const auto it = state.finished.find(id);
    if (it == state.finished.end()) {
        return std::nullopt;
    }
    CaptureResult result = std::move(it->second);
    state.finished.erase(it);
    return result;
}

std::optional<CaptureRequest> TakeCapture() {
    std::scoped_lock lock{state.mutex};
    if (state.captures.empty()) {
        return std::nullopt;
    }
    CaptureRequest request = std::move(state.captures.front());
    state.captures.pop_front();
    UpdatePending();
    return request;
}

void FinishCapture(u64 id, CaptureResult result) {
    {
        std::scoped_lock lock{state.mutex};
        if (!state.open) {
            return;
        }
        state.finished.insert_or_assign(id, std::move(result));
    }
    state.changed.notify_all();
}

void RequestGpuRead(GpuReadRequest request) {
    std::scoped_lock lock{state.mutex};
    state.gpu_reads.push_back(request);
    UpdatePending();
}

void CancelGpuRead(u64 id) {
    std::scoped_lock lock{state.mutex};
    std::erase_if(state.gpu_reads, [id](const GpuReadRequest& request) { return request.id == id; });
    state.gpu_read_results.erase(id);
    UpdatePending();
}

std::optional<GpuReadResult> WaitGpuRead(u64 id, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    state.changed.wait_for(lock, timeout,
                           [id] { return !state.open || state.gpu_read_results.contains(id); });
    const auto it = state.gpu_read_results.find(id);
    if (it == state.gpu_read_results.end()) {
        return std::nullopt;
    }
    GpuReadResult result = std::move(it->second);
    state.gpu_read_results.erase(it);
    return result;
}

std::optional<GpuReadRequest> TakeGpuRead() {
    std::scoped_lock lock{state.mutex};
    if (state.gpu_reads.empty()) {
        return std::nullopt;
    }
    const GpuReadRequest request = state.gpu_reads.front();
    state.gpu_reads.pop_front();
    UpdatePending();
    return request;
}

void FinishGpuRead(u64 id, GpuReadResult result) {
    {
        std::scoped_lock lock{state.mutex};
        if (!state.open) {
            return;
        }
        state.gpu_read_results.insert_or_assign(id, std::move(result));
    }
    state.changed.notify_all();
}

u64 SetRejected(std::span<const u64> hashes, bool rejected) {
    std::scoped_lock lock{state.mutex};
    bool changed = false;
    for (const u64 hash : hashes) {
        const auto it = std::ranges::find(state.rejected, hash);
        if (rejected && it == state.rejected.end()) {
            state.rejected.push_back(hash);
            changed = true;
        } else if (!rejected && it != state.rejected.end()) {
            state.rejected.erase(it);
            state.unrejected = true;
            changed = true;
        }
    }
    any_rejected.store(!state.rejected.empty(), std::memory_order_relaxed);
    if (changed) {
        ++state.reject_generation;
        state.reject_pending = true;
        UpdatePending();
    }
    return state.reject_generation;
}

std::vector<u64> RejectedHashes() {
    std::scoped_lock lock{state.mutex};
    return state.rejected;
}

bool IsRejected(u64 hash) {
    if (!any_rejected.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{state.mutex};
    return std::ranges::find(state.rejected, hash) != state.rejected.end();
}

bool AnyRejected() noexcept {
    return any_rejected.load(std::memory_order_relaxed);
}

std::optional<RejectChange> TakeRejectChange() {
    std::scoped_lock lock{state.mutex};
    if (!state.reject_pending) {
        return std::nullopt;
    }
    const RejectChange change{.generation = state.reject_generation,
                              .unrejected = state.unrejected};
    state.reject_pending = false;
    state.unrejected = false;
    UpdatePending();
    return change;
}

void NoteRejectApplied(u64 generation) {
    {
        std::scoped_lock lock{state.mutex};
        state.applied_generation = (std::max)(state.applied_generation, generation);
    }
    state.changed.notify_all();
}

bool WaitRejectApplied(u64 generation, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    return state.changed.wait_for(lock, timeout, [generation] {
        return !state.open || state.applied_generation >= generation;
    }) && state.applied_generation >= generation;
}

void ArmLumaWatch(u32 composites) {
    std::scoped_lock lock{luma_watch.mutex};
    luma_watch.samples.clear();
    luma_watch.samples.reserve(composites);
    luma_remaining.store(composites, std::memory_order_relaxed);
}

std::pair<std::vector<LumaSample>, u32> ReadLumaWatch() {
    std::scoped_lock lock{luma_watch.mutex};
    return {luma_watch.samples, luma_remaining.load(std::memory_order_relaxed)};
}

bool LumaWatchArmed() noexcept {
    return luma_remaining.load(std::memory_order_relaxed) != 0;
}

void AddLumaSample(u64 present, std::span<const u8> bgra) {
    const float luma = MeanLuma(bgra);
    std::scoped_lock lock{luma_watch.mutex};
    const u32 remaining = luma_remaining.load(std::memory_order_relaxed);
    if (remaining == 0) {
        return;
    }
    luma_watch.samples.push_back({.present = present, .luma = luma});
    luma_remaining.store(remaining - 1, std::memory_order_relaxed);
}

void AddLumaSample(std::span<const u8> bgra) {
    AddLumaSample(Core::g_nx2_composite_present.load(std::memory_order_relaxed), bgra);
}

float MeanLuma(std::span<const u8> bgra) {
    const size_t pixels = bgra.size() / 4;
    if (pixels == 0) {
        return 0.0f;
    }
    // Rec. 601 weights in 1/256: 0.114 B + 0.587 G + 0.299 R.
    u64 sum = 0;
    for (size_t index = 0; index < pixels * 4; index += 4) {
        sum += u64{29} * bgra[index] + u64{150} * bgra[index + 1] + u64{77} * bgra[index + 2];
    }
    return static_cast<float>(static_cast<double>(sum) / (256.0 * static_cast<double>(pixels)));
}

} // namespace VideoCore::Nx2Debug
