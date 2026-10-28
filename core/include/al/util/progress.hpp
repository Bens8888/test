#pragma once
// Progress reporting + cooperative cancellation for long-running core jobs. The mod runs
// these on a background thread and forwards progress to the UI on the main thread.
#include <atomic>
#include <functional>
#include <string>

namespace al {

struct Progress {
    // stage: short machine-friendly stage id ("decode", "beats", ...); frac in [0,1] within the stage.
    std::function<void(const std::string& stage, float frac)> onProgress;
    const std::atomic<bool>* cancelFlag = nullptr;

    void report(const std::string& stage, float frac) const { if (onProgress) onProgress(stage, frac); }
    bool cancelled() const { return cancelFlag && cancelFlag->load(std::memory_order_relaxed); }
};

struct Cancelled {}; // thrown by core stages when Progress::cancelled() is observed

inline void checkCancel(const Progress* p) { if (p && p->cancelled()) throw Cancelled{}; }

} // namespace al
