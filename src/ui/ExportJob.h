#pragma once

#include "engine/TimelineMapper.h"
#include "engine/VideoEncoder.h"
#include "engine/VideoSource.h"
#include "render/Renderer.h"
#include "ui/OverlayPainter.h"

#include <QFont>
#include <QObject>
#include <QString>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class QOffscreenSurface;
class QTimer;

namespace scvp::ui {

// Everything an export needs, captured when it starts: what happens on screen afterwards
// (other frames, zoom, another offset, closing a video) does not change it.
struct ExportSpec {
    std::array<std::shared_ptr<IVideoSource>, 2> sources;  // null: no video in that slot
    HwConfig hw;
    TimelineMapper mapper;
    int64_t first = 0;  // timeline positions, inclusive
    int64_t last = -1;
    // The picture: frames are filled in per position; framebufferSize is the picture size.
    render::RenderState view;
    // Overlay drawn over each picture. The scene's layout and HOLD flags are filled in per
    // position; its dpr is picture px per logical px of the text.
    bool labels = false;
    bool legend = false;  // the legend's chips and names, else the short A/B labels
    bool frameInfo = false;
    OverlayScene scene;
    QFont mono;
    EncoderSettings encoder;  // width/height/frame rate follow the picture and the timeline
    std::string path;

    int64_t frames() const { return last - first + 1; }
};

// Busy time of each stage (seconds), to see what limits an export.
struct ExportStats {
    int64_t frames = 0;
    int width = 0;  // the picture
    int height = 0;
    double seconds = 0;
    std::array<double, 2> decode{};
    double render = 0;
    double encode = 0;
    int missing = 0;  // frames that could not be decoded (a neighbour was used)
    std::string encoder;
    bool cancelled = false;
};

// Exports a range of the timeline as a video in the background, independent of playback:
//
//   decode A, decode B  (one thread each: their own sessions on the open sources, hardware
//        |                decoding as configured; frames in the order the range needs them)
//   render              (a thread with its own GL context: Renderer + OffscreenView, overlay
//        |                layers, the encoder's 4:2:0 planes converted on the GPU, asynchronous
//        |                read-back straight into the encoder's input frames)
//   encode              (a thread: VideoEncoder)
//
// connected by small bounded queues, so memory stays flat and the slowest stage sets the pace.
// Nothing blocks the GUI thread; progress is polled, the end is signalled. When the platform
// cannot use GL from another thread, the render stage runs on the GUI thread in short slices.
class ExportJob : public QObject {
    Q_OBJECT
public:
    explicit ExportJob(ExportSpec spec, QObject* parent = nullptr);
    ~ExportJob() override;  // cancels and waits

    void start();
    void cancel();
    bool running() const { return m_started && !m_done; }
    const ExportSpec& spec() const { return m_spec; }
    int64_t total() const { return m_spec.frames(); }
    int64_t encoded() const { return m_encoded.load(std::memory_order_relaxed); }
    ExportStats stats() const;

signals:
    // Once, on the GUI thread. ok = false: failed or cancelled (message says which); the output
    // is removed then.
    void finished(bool ok, const QString& message);

private:
    struct Shared;
    class RenderStage;

    void decodeLoop(int slot);
    void encodeLoop();
    void renderLoop();
    void fail(const std::string& message);
    void threadDone();
    void finish();

    ExportSpec m_spec;
    std::shared_ptr<Shared> m_shared;
    std::unique_ptr<RenderStage> m_render;
    std::unique_ptr<QOffscreenSurface> m_surface;
    QTimer* m_pump = nullptr;  // render stage on the GUI thread (no threaded GL)
    std::vector<std::thread> m_threads;
    std::atomic<int> m_running{0};
    std::atomic<int64_t> m_encoded{0};
    std::atomic<bool> m_cancel{false};
    std::atomic<uint64_t> m_cancelGeneration{0};  // DecodeHints: != 0 stops decoding
    mutable std::mutex m_mutex;
    std::string m_error;
    ExportStats m_stats;
    std::chrono::steady_clock::time_point m_startTime;
    bool m_started = false;
    bool m_done = false;
};

} // namespace scvp::ui
