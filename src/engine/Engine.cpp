#include "engine/Engine.h"

#include "engine/GpuFrame.h"
#include "engine/Log.h"
#include "engine/PrefetchPlanner.h"

#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

namespace scvp {
namespace {

size_t physicalMemory()
{
#if defined(_WIN32)
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    return GlobalMemoryStatusEx(&st) ? size_t(st.ullTotalPhys) : size_t(8) << 30;
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    return sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0 ? size_t(mem) : size_t(8) << 30;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && page > 0 ? size_t(pages) * size_t(page) : size_t(8) << 30;
#endif
}

size_t estimateFrameBytes(const VideoInfo& info)
{
    const AVPixelFormat fmt = info.pixelFormat != AV_PIX_FMT_NONE ? info.pixelFormat : AV_PIX_FMT_YUV420P;
    const int bytes = av_image_get_buffer_size(fmt, std::max(info.width, 16), std::max(info.height, 16), int(FramePool::kAlign));
    return size_t(std::max(bytes, 4096)) + 8192;
}

} // namespace

Engine::Engine()
    : m_budget(defaultMemoryBudget())
    , m_gpuBudget(size_t(-1))  // resolved lazily (creating the CUDA device takes time)
{
}

size_t Engine::defaultGpuMemoryBudget()
{
    static const size_t budget = [] {
        AVBufferRef* dev = HwDevices::instance().get(AV_HWDEVICE_TYPE_CUDA);
        auto pool = GpuFramePool::forDevice(dev);
        av_buffer_unref(&dev);
        size_t freeB = 0, total = 0;
        if (!pool || !pool->memoryInfo(&freeB, &total))
            return size_t(0);
        return std::min(total / 10 * 4, freeB / 10 * 8);
    }();
    return budget;
}

void Engine::setGpuMemoryBudget(size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_gpuBudget = bytes;
    rebalanceLocked();
}

size_t Engine::gpuMemoryBudget() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_gpuBudget == size_t(-1) ? defaultGpuMemoryBudget() : m_gpuBudget;
}

Engine::~Engine()
{
    for (int i = 0; i < kSlots; ++i)
        close(i);
}

size_t Engine::defaultMemoryBudget() { return physicalMemory() / 4; }

bool Engine::open(int slot, const std::string& path, const InputOptions& opts, std::string* error)
{
    if (slot < 0 || slot >= kSlots)
        return false;
    close(slot);
    auto s = std::make_unique<Slot>();
    s->source = openVideoSource(path, opts, error);
    if (!s->source)
        return false;
    s->info = s->source->info();
    s->pool = std::make_shared<FramePool>(slot == 0 ? "A" : "B");
    s->cache = std::make_unique<FrameCache>(0);
    s->mailbox = std::make_unique<WantMailbox>();
    s->frameBytesEstimate = estimateFrameBytes(s->info);

    uint64_t epoch;
    HwConfig hw;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        epoch = m_nextEpoch++;
        s->epoch = epoch;
        hw = m_hw;
    }
    s->lane = std::make_unique<DecodeLane>(*s->source, *s->cache, *s->mailbox, SessionConfig{s->pool, 0, hw},
                                           [this, slot, epoch] { onFrames(slot, epoch); });
    if (!s->lane->start(error))
        return false;
    s->source->setIndexListener([this, slot, epoch] { onIndex(slot, epoch); });

    std::lock_guard<std::mutex> lock(m_mutex);
    m_slots[size_t(slot)] = std::move(s);
    rebalanceLocked();
    return true;
}

void Engine::close(int slot)
{
    if (slot < 0 || slot >= kSlots)
        return;
    std::unique_ptr<Slot> s;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        s = std::move(m_slots[size_t(slot)]);
        if (s)
            rebalanceLocked();
    }
    if (!s)
        return;
    // Outside the lock: stopping joins threads whose callbacks take the lock.
    s->source->setIndexListener({});
    s->lane->stop();
    s->lane.reset();
    s->source.reset();
}

bool Engine::isOpen(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return slot >= 0 && slot < kSlots && m_slots[size_t(slot)];
}

VideoInfo Engine::info(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Slot* s = slot >= 0 && slot < kSlots ? m_slots[size_t(slot)].get() : nullptr;
    return s ? s->info : VideoInfo{};
}

FrameIndexPtr Engine::index(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Slot* s = slot >= 0 && slot < kSlots ? m_slots[size_t(slot)].get() : nullptr;
    return s ? s->source->index() : nullptr;
}

std::shared_ptr<IVideoSource> Engine::source(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Slot* s = slot >= 0 && slot < kSlots ? m_slots[size_t(slot)].get() : nullptr;
    return s ? s->source : nullptr;
}

void Engine::setMemoryBudget(size_t bytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_budget = bytes;
    rebalanceLocked();
}

size_t Engine::memoryBudget() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_budget;
}

void Engine::setHardware(const HwConfig& hw)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hw = hw;
}

HwConfig Engine::hardware() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hw;
}

size_t Engine::frameBytesOf(const Slot& s) const
{
    const size_t measured = s.lane ? s.lane->frameBytes() : 0;
    return measured ? measured : s.frameBytesEstimate;
}

// Splits the budget so every open video keeps the same number of frames, minus a reserve for
// frames the decoder itself holds (references, frame threads), which the pool also counts.
void Engine::rebalanceLocked()
{
    size_t total = 0;
    for (const auto& s : m_slots)
        if (s)
            total += frameBytesOf(*s);
    if (!total)
        return;
    // CPU-only decoding never touches CUDA (a context alone costs a few hundred MB of VRAM).
    // A requested GPU budget is capped by what is actually free on the device (other
    // applications share it), counting what our caches already hold.
    size_t gpuBudgetTotal = m_hw.policy == HwPolicy::Off ? 0 : m_gpuBudget == size_t(-1) ? defaultGpuMemoryBudget() : m_gpuBudget;
    if (gpuBudgetTotal) {
        AVBufferRef* dev = HwDevices::instance().get(AV_HWDEVICE_TYPE_CUDA);
        auto pool = GpuFramePool::forDevice(dev);
        av_buffer_unref(&dev);
        size_t freeB = 0, totalB = 0;
        if (pool && pool->memoryInfo(&freeB, &totalB)) {
            size_t held = pool->cachedBytes();
            for (const auto& s : m_slots)
                if (s)
                    held += s->cache->gpuBytes();
            gpuBudgetTotal = std::min(gpuBudgetTotal, (freeB + held) / 10 * 9);
        } else {
            gpuBudgetTotal = 0;
        }
    }
    for (auto& s : m_slots) {
        if (!s)
            continue;
        const size_t fb = frameBytesOf(*s);
        const size_t share = size_t(double(m_budget) * double(fb) / double(total));
        FrameIndexPtr idx = s->source->index();
        const size_t reserveFrames = size_t(idx ? idx->reorderDepth() : 4) + 20;
        const size_t reserve = std::min(share / 2, reserveFrames * fb);
        s->cacheBudget = std::max(fb, share - reserve);
        const size_t gpuTotal = gpuBudgetTotal;
        s->gpuBudget = size_t(double(gpuTotal) * double(fb) / double(total));
        // 1 % headroom: frames of slightly different sizes (VFR resolution changes, tier moves)
        // must not push a full plan over what the cache can hold.
        const size_t frames = s->cacheBudget / fb + (s->onGpu ? s->gpuBudget / fb : 0);
        s->capacity = int32_t(std::max<size_t>(1, frames - std::min(frames - 1, std::max<size_t>(1, frames / 100))));
        s->frameBytesPlanned = fb;
        s->cache->setBudget(s->cacheBudget);
        s->cache->setGpuBudget(s->gpuBudget);
        s->pool->setCacheLimit(std::min<size_t>(s->cacheBudget / 8, size_t(512) << 20));
        replanLocked(*s);
    }
}

void Engine::replanLocked(Slot& s)
{
    if (!s.hasIntent)
        return;
    FrameIndexPtr idx = s.source->index();
    if (!idx)
        return;
    PlannerInput in;
    in.playhead = s.intent.playhead;
    in.frameCount = idx->frameCount();
    in.direction = s.intent.direction;
    in.playing = s.intent.playing;
    in.speed = s.intent.speed;
    in.fps = idx->frameRate();
    in.capacity = s.capacity;
    in.gop = idx->gopLengthAround(std::clamp(s.intent.playhead, 0, std::max(0, idx->frameCount() - 1)));
    in.loop = s.intent.loop;
    in.loopActive = s.intent.loopActive;
    in.jumpDistances = s.intent.jumpDistances;
    in.aheadLimit = s.intent.aheadLimit;
    WantList w = planPrefetch(in);
    s.cache->setPolicy(w.policy);
    s.mailbox->post(std::move(w));
}

void Engine::request(int slot, const PlaybackIntent& intent)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Slot* s = slot >= 0 && slot < kSlots ? m_slots[size_t(slot)].get() : nullptr;
    if (!s)
        return;
    s->intent = intent;
    s->hasIntent = true;
    replanLocked(*s);
}

FramePtr Engine::frame(int slot, int32_t n) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Slot* s = slot >= 0 && slot < kSlots ? m_slots[size_t(slot)].get() : nullptr;
    return s ? s->cache->get(n) : nullptr;
}

FramePtr Engine::nearestFrame(int slot, int32_t n, int32_t maxDistance) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Slot* s = slot >= 0 && slot < kSlots ? m_slots[size_t(slot)].get() : nullptr;
    return s ? s->cache->nearest(n, maxDistance) : nullptr;
}

RangeSetPtr Engine::cachedRanges(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Slot* s = slot >= 0 && slot < kSlots ? m_slots[size_t(slot)].get() : nullptr;
    return s ? s->cache->ranges() : nullptr;
}

int32_t Engine::capacityFrames(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Slot* s = slot >= 0 && slot < kSlots ? m_slots[size_t(slot)].get() : nullptr;
    return s ? s->capacity : 0;
}

std::string Engine::debugState(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const Slot* s = slot >= 0 && slot < kSlots ? m_slots[size_t(slot)].get() : nullptr;
    if (!s)
        return "closed";
    std::string out = "lane: " + s->lane->describe() + "\ncached:";
    for (const FrameRange& r : s->cache->ranges()->ranges)
        out += " " + std::to_string(r.first) + "-" + std::to_string(r.last);
    const WantList w = s->mailbox->snapshot();
    out += "\nplan (gen " + std::to_string(w.generation) + ", target " + std::to_string(w.target) + "):";
    for (const Want& want : w.wants)
        out += " [" + std::to_string(want.range.first) + "," + std::to_string(want.range.last) + "]c" +
               std::to_string(int(want.cls)) + (want.fill > 0 ? "+" : "-");
    return out;
}

SlotStats Engine::stats(int slot) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SlotStats st;
    const Slot* s = slot >= 0 && slot < kSlots ? m_slots[size_t(slot)].get() : nullptr;
    if (!s)
        return st;
    st.cachedFrames = s->cache->frames();
    st.cachedBytes = s->cache->bytes();
    st.budgetBytes = s->cacheBudget;
    st.capacityFrames = s->capacity;
    st.poolLiveBytes = s->pool->liveBytes();
    st.framesDecoded = s->lane->stats().framesDecoded.load();
    st.seeks = s->lane->stats().seeks.load();
    st.cancellations = s->lane->stats().cancellations.load();
    st.hardwareDevice = s->lane->hardwareDevice();
    st.gpuFrames = s->cache->gpuFrames();
    st.gpuBytes = s->cache->gpuBytes();
    st.gpuBudgetBytes = s->gpuBudget;
    st.spills = s->lane->stats().spills.load();
    st.indexProgress = s->source->indexProgress();
    st.indexComplete = s->source->indexComplete();
    return st;
}

void Engine::setFramesCallback(std::function<void(int)> cb)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_framesCb = std::move(cb);
}

void Engine::setIndexCallback(std::function<void(int)> cb)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_indexCb = std::move(cb);
}

int Engine::slotOfLocked(uint64_t epoch) const
{
    for (int i = 0; i < kSlots; ++i)
        if (m_slots[size_t(i)] && m_slots[size_t(i)]->epoch == epoch)
            return i;
    return -1;
}

void Engine::swapSlots()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::swap(m_slots[0], m_slots[1]);
}

// Callbacks identify their video by epoch: slots may have been swapped since it was opened.
void Engine::onFrames(int, uint64_t epoch)
{
    std::function<void(int)> cb;
    int slot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        slot = slotOfLocked(epoch);
        if (slot < 0)
            return;
        Slot* s = m_slots[size_t(slot)].get();
        // The first decoded frames replace the size estimate the budget split was based on, and
        // tell whether the VRAM tier is in use.
        const size_t fb = s->lane->frameBytes();
        const bool onGpu = s->lane->hardwareDevice() == "cuda" && s->gpuBudget > 0;
        if (onGpu != s->onGpu) {
            s->onGpu = onGpu;
            rebalanceLocked();
        } else if (fb && fb != s->frameBytesPlanned) {
            // Plan with the real frame size: a capacity from the estimate that is a few frames
            // too large makes a full plan never fit, and the lane refills evicted frames forever.
            rebalanceLocked();
        }
        cb = m_framesCb;
    }
    if (cb)
        cb(slot);
}

void Engine::onIndex(int, uint64_t epoch)
{
    std::function<void(int)> cb;
    int slot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        slot = slotOfLocked(epoch);
        if (slot < 0)
            return;
        Slot* s = m_slots[size_t(slot)].get();
        if (s->source->indexComplete())
            s->cache->dropProvisional();
        replanLocked(*s);
        s->mailbox->nudge();
        cb = m_indexCb;
    }
    if (cb)
        cb(slot);
}

} // namespace scvp
