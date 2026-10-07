#include "ui/ExportJob.h"

#include "engine/Log.h"
#include "render/OffscreenView.h"

#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <QPainter>
#include <QScopeGuard>
#include <QSurfaceFormat>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>

namespace scvp::ui {
namespace {

using Clock = std::chrono::steady_clock;
double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// A small FIFO between two stages. close() wakes everyone: push() fails from then on, pop()
// returns what is left and then nothing.
template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity)
        : m_capacity(capacity)
    {
    }
    bool push(T v)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_items.size() < m_capacity || m_closed; });
        if (m_closed)
            return false;
        m_items.push_back(std::move(v));
        m_cv.notify_all();
        return true;
    }
    bool pop(T& out)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return !m_items.empty() || m_closed; });
        return takeLocked(out);
    }
    bool tryPop(T& out)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return takeLocked(out);
    }
    bool hasSpace() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.size() < m_capacity && !m_closed;
    }
    void close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_cv.notify_all();
    }

private:
    bool takeLocked(T& out)
    {
        if (m_items.empty())
            return false;
        out = std::move(m_items.front());
        m_items.pop_front();
        m_cv.notify_all();
        return true;
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<T> m_items;
    size_t m_capacity;
    bool m_closed = false;
};

struct DecodedFrame {
    int32_t number = -1;  // the frame the timeline wants (a neighbour stands in for a missing one)
    FramePtr frame;
};

// Frames decoded ahead per video: enough to keep the GPU busy, little memory (4K: ~50 MB).
constexpr size_t kDecodedAhead = 4;
// Pictures read back and waiting for the encoder.
constexpr size_t kPicturesAhead = 3;

QSurfaceFormat glFormat()
{
    QSurfaceFormat f;
    f.setRenderableType(QSurfaceFormat::OpenGL);
    f.setProfile(QSurfaceFormat::CoreProfile);
#if defined(Q_OS_MACOS)
    f.setVersion(4, 1);
#else
    f.setVersion(3, 3);
#endif
    f.setDepthBufferSize(0);
    f.setStencilBufferSize(0);
    return f;
}

QString timeText(double seconds)
{
    const int64_t ms = int64_t(std::llround(std::max(0.0, seconds) * 1000));
    return QStringLiteral("%1:%2:%3.%4")
        .arg(ms / 3'600'000, 2, 10, QLatin1Char('0'))
        .arg(ms / 60'000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

} // namespace

// What the stages share: the queues between them, read-back buffers for reuse, and a signal
// the render stage sleeps on when it has to wait for decoding or encoding.
struct ExportJob::Shared {
    std::array<std::unique_ptr<BoundedQueue<DecodedFrame>>, 2> decoded;
    BoundedQueue<std::vector<uint8_t>> pictures{kPicturesAhead};

    void closeAll()
    {
        for (auto& q : decoded)
            if (q)
                q->close();
        pictures.close();
        notify();
    }
    // Picture buffers go round (render -> encode -> render): no large allocation per frame.
    std::vector<uint8_t> spare()
    {
        std::lock_guard<std::mutex> lock(spareMutex);
        if (spares.empty())
            return {};
        std::vector<uint8_t> v = std::move(spares.back());
        spares.pop_back();
        return v;
    }
    void giveBack(std::vector<uint8_t> v)
    {
        std::lock_guard<std::mutex> lock(spareMutex);
        if (spares.size() < kPicturesAhead + 2)
            spares.push_back(std::move(v));
    }
    void notify()
    {
        {
            std::lock_guard<std::mutex> lock(activityMutex);
            ++activity;
        }
        activityCv.notify_all();
    }
    uint64_t seen()
    {
        std::lock_guard<std::mutex> lock(activityMutex);
        return activity;
    }
    void waitFor(uint64_t seen, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(activityMutex);
        activityCv.wait_for(lock, timeout, [&] { return activity != seen; });
    }

    std::mutex spareMutex;
    std::vector<std::vector<uint8_t>> spares;
    std::mutex activityMutex;
    std::condition_variable activityCv;
    uint64_t activity = 0;
};

// Draws one picture per timeline position with its own GL context: Renderer for the view,
// OffscreenView for the overlay layers and the read-back. Never blocks on the other stages:
// pump() does what it can and says whether it got anywhere.
class ExportJob::RenderStage {
public:
    enum class Result { Progress, Idle, Done, Failed };

    RenderStage(const ExportSpec& spec, Shared& shared)
        : m_spec(spec)
        , m_shared(shared)
        , m_pos(spec.first)
    {
    }

    bool open(QOffscreenSurface* surface, QString* error)
    {
        m_surface = surface;
        m_ctx = std::make_unique<QOpenGLContext>();
        m_ctx->setFormat(glFormat());
        if (!m_ctx->create() || !m_ctx->makeCurrent(surface)) {
            *error = QStringLiteral("cannot create an OpenGL 3.3 context for the export");
            return false;
        }
        m_gl = QOpenGLVersionFunctionsFactory::get<render::GL>(m_ctx.get());
        if (!m_gl || !m_gl->initializeOpenGLFunctions()) {
            *error = QStringLiteral("OpenGL 3.3 core is not available for the export");
            return false;
        }
        m_renderer.initialize(m_gl);
        return m_view.create(m_gl, m_spec.view.framebufferSize, error);
    }
    void makeCurrent() { m_ctx->makeCurrent(m_surface); }
    void doneCurrent() { m_ctx->doneCurrent(); }
    void close()
    {
        if (!m_ctx)
            return;
        if (m_gl) {
            m_ctx->makeCurrent(m_surface);
            m_view.release();
            m_renderer.release();
            m_ctx->doneCurrent();
        }
        m_ctx.reset();
        m_gl = nullptr;
    }

    Result pump()
    {
        if (m_failed)
            return Result::Failed;
        const auto t0 = Clock::now();
        bool progress = false;
        for (;;) {
            bool step = false;
            // Hand finished pictures to the encoder; wait for the GPU only when nothing else
            // can be done (every read-back buffer in flight, or the range is drawn).
            const bool block = m_view.pending() == render::OffscreenView::kInFlight || m_pos > m_spec.last;
            if (m_view.pending() && m_shared.pictures.hasSpace()) {
                std::vector<uint8_t> buf = m_shared.spare();
                if (m_view.take(buf, block)) {
                    m_shared.pictures.push(std::move(buf));
                    step = true;
                } else {
                    m_shared.giveBack(std::move(buf));
                }
            }
            if (m_pos <= m_spec.last && m_view.pending() < render::OffscreenView::kInFlight) {
                const int r = drawNext();
                if (r < 0)
                    return Result::Failed;
                step = step || r > 0;
            }
            if (!step)
                break;
            progress = true;
        }
        if (progress)
            m_busy += secondsSince(t0);
        if (m_pos > m_spec.last && m_view.pending() == 0)
            return Result::Done;
        return progress ? Result::Progress : Result::Idle;
    }

    QString error() const { return m_error; }
    double busy() const { return m_busy; }

private:
    // 1: drawn, 0: a frame is not decoded yet, -1: failed.
    int drawNext()
    {
        std::array<FramePtr, 2> frames;
        std::array<bool, 2> hold{};
        std::array<int32_t, 2> numbers{-1, -1};
        for (int slot = 0; slot < 2; ++slot) {
            if (!m_spec.sources[size_t(slot)])
                continue;
            const TimelineMapper::Local loc = m_spec.mapper.local(slot, m_pos);
            DecodedFrame& cur = m_current[size_t(slot)];
            while (cur.number != loc.frame) {
                DecodedFrame next;
                if (!m_shared.decoded[size_t(slot)]->tryPop(next))
                    return 0;
                if (next.number > loc.frame)
                    return fail(QStringLiteral("frames of %1 came out of order").arg(slot ? QLatin1Char('B') : QLatin1Char('A')));
                cur = std::move(next);
            }
            frames[size_t(slot)] = cur.frame;
            hold[size_t(slot)] = loc.hold;
            numbers[size_t(slot)] = loc.frame;
        }
        render::RenderState state = m_spec.view;
        state.frames = frames;
        state.probe = false;
        state.sampleProbe = false;
        drawOverlay(state, hold, numbers);
        if (!m_view.render(m_renderer, state)) {
            const QString why = m_renderer.error();
            return fail(QStringLiteral("drawing the picture failed%1").arg(why.isEmpty() ? QString() : QStringLiteral(": ") + why));
        }
        ++m_pos;
        return 1;
    }

    // Labels or legend (layer 0, repainted when a video starts or stops holding) and the frame
    // number line (layer 1, every picture), painted by the same code as on screen.
    void drawOverlay(const render::RenderState& state, std::array<bool, 2> hold, std::array<int32_t, 2> numbers)
    {
        const double dpr = m_spec.scene.dpr;
        if (m_spec.labels && (!m_labelsPainted || hold != m_lastHold)) {
            OverlayScene scene = m_spec.scene;
            const int divider = std::max(1, int(std::lround(render::CompareLayout::kDivider * state.lineScale)));
            scene.layout = render::CompareLayout::compute(state.compare, state.framebufferSize, state.view.imageSize(),
                                                          state.frames[0] != nullptr, state.frames[1] != nullptr, divider);
            scene.hold = hold;
            QImage img(state.framebufferSize, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            img.setDevicePixelRatio(dpr);
            {
                QPainter p(&img);
                p.setRenderHint(QPainter::Antialiasing);
                p.setFont(m_spec.mono);
                if (m_spec.legend)
                    overlay::paintLegend(p, scene, QRect());
                else
                    overlay::paintPaneLabels(p, scene, QFontMetrics(m_spec.mono), false);
            }
            img.setDevicePixelRatio(1.0);
            m_view.setLayer(0, img, QPoint(0, 0));
            m_labelsPainted = true;
            m_lastHold = hold;
        }
        if (m_spec.frameInfo) {
            QString text = QStringLiteral("#%1").arg(m_pos);
            for (int slot = 0; slot < 2; ++slot)
                if (numbers[size_t(slot)] >= 0)
                    text += QStringLiteral("   %1 %2").arg(slot ? QLatin1Char('B') : QLatin1Char('A')).arg(numbers[size_t(slot)]);
            const int master = m_spec.mapper.master();
            if (numbers[size_t(master)] >= 0) {
                const FrameIndexPtr idx = m_spec.sources[size_t(master)]->index();
                text += QStringLiteral("   ") + timeText(idx ? idx->secondsOf(numbers[size_t(master)]) : 0.0);
            }
            const QFontMetrics fm(m_spec.mono);
            const QSize logical(fm.horizontalAdvance(text) + 32, fm.height() + 24);
            const QSize device(int(std::ceil(logical.width() * dpr)), int(std::ceil(logical.height() * dpr)));
            QImage img(device, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            img.setDevicePixelRatio(dpr);
            {
                QPainter p(&img);
                p.setRenderHint(QPainter::Antialiasing);
                overlay::paintFrameInfo(p, logical, text, m_spec.mono);
            }
            img.setDevicePixelRatio(1.0);
            m_view.setLayer(1, img, QPoint(0, state.framebufferSize.height() - device.height()));
        }
    }

    int fail(const QString& message)
    {
        m_error = message;
        m_failed = true;
        return -1;
    }

    const ExportSpec& m_spec;
    Shared& m_shared;
    std::unique_ptr<QOpenGLContext> m_ctx;
    QOffscreenSurface* m_surface = nullptr;
    render::GL* m_gl = nullptr;
    render::Renderer m_renderer;
    render::OffscreenView m_view;
    std::array<DecodedFrame, 2> m_current;
    int64_t m_pos;
    bool m_labelsPainted = false;
    std::array<bool, 2> m_lastHold{};
    bool m_failed = false;
    QString m_error;
    double m_busy = 0;
};

ExportJob::ExportJob(ExportSpec spec, QObject* parent)
    : QObject(parent)
    , m_spec(std::move(spec))
{
    const QSize size = m_spec.view.framebufferSize;
    m_spec.encoder.width = size.width();
    m_spec.encoder.height = size.height();
    m_spec.encoder.startNumber = int(m_spec.first);
}

ExportJob::~ExportJob()
{
    cancel();
    if (m_pump) {
        m_pump->stop();
        if (m_render)
            m_render->close();
    }
    for (std::thread& t : m_threads)
        if (t.joinable())
            t.join();
    m_render.reset();
    m_surface.reset();
}

void ExportJob::start()
{
    if (m_started)
        return;
    m_started = true;
    m_startTime = std::chrono::steady_clock::now();
    m_shared = std::make_shared<Shared>();
    m_stats = {};
    if (m_spec.frames() <= 0 || (!m_spec.sources[0] && !m_spec.sources[1]) || m_spec.view.framebufferSize.isEmpty()) {
        m_error = "nothing to export";
        QTimer::singleShot(0, this, [this] { finish(); });
        return;
    }
    // GL objects that need the GUI thread: the surface (a pbuffer or hidden window).
    m_surface = std::make_unique<QOffscreenSurface>();
    m_surface->setFormat(glFormat());
    m_surface->create();
    m_render = std::make_unique<RenderStage>(m_spec, *m_shared);
    // SCVP_EXPORT_GUI_RENDER=1 takes the GUI-thread path anyway (tests, diagnosis).
    const bool threaded = QOpenGLContext::supportsThreadedOpenGL() && qEnvironmentVariableIsEmpty("SCVP_EXPORT_GUI_RENDER");
    SCVP_INFO("export: %lld frames of %dx%d to %s, drawing on %s", static_cast<long long>(m_spec.frames()),
              m_spec.view.framebufferSize.width(), m_spec.view.framebufferSize.height(), m_spec.path.c_str(),
              threaded ? "a thread of its own" : "the GUI thread");

    int threads = 1 + (threaded ? 1 : 0);
    for (int slot = 0; slot < 2; ++slot)
        if (m_spec.sources[size_t(slot)]) {
            m_shared->decoded[size_t(slot)] = std::make_unique<BoundedQueue<DecodedFrame>>(kDecodedAhead);
            ++threads;
        }
    m_running = threads + (threaded ? 0 : 1);  // the GUI pump counts as one
    for (int slot = 0; slot < 2; ++slot)
        if (m_spec.sources[size_t(slot)])
            m_threads.emplace_back([this, slot] { decodeLoop(slot); });
    m_threads.emplace_back([this] { encodeLoop(); });
    if (threaded) {
        m_threads.emplace_back([this] { renderLoop(); });
        return;
    }
    // No GL on other threads here: draw on the GUI thread in slices of a few milliseconds.
    QString err;
    if (!m_render->open(m_surface.get(), &err)) {
        m_render->close();
        fail(err.toStdString());
        threadDone();
        return;
    }
    m_render->doneCurrent();
    m_pump = new QTimer(this);
    m_pump->setInterval(0);
    connect(m_pump, &QTimer::timeout, this, [this] {
        m_render->makeCurrent();
        const auto t0 = Clock::now();
        RenderStage::Result r = RenderStage::Result::Progress;
        while (!m_cancel && r == RenderStage::Result::Progress && secondsSince(t0) < 0.010)
            r = m_render->pump();
        m_render->doneCurrent();
        if (m_cancel || r == RenderStage::Result::Done || r == RenderStage::Result::Failed) {
            m_pump->stop();
            if (r == RenderStage::Result::Failed)
                fail(m_render->error().toStdString());
            else
                m_shared->pictures.close();  // the encoder finishes what is queued
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stats.render = m_render->busy();
            }
            m_render->close();
            threadDone();
        }
    });
    m_pump->start();
}

void ExportJob::cancel()
{
    if (m_cancel.exchange(true))
        return;
    m_cancelGeneration = 1;
    if (m_shared)
        m_shared->closeAll();
}

void ExportJob::fail(const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_error.empty())
            m_error = message;
    }
    SCVP_WARN("export: %s", message.c_str());
    cancel();
}

ExportStats ExportJob::stats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void ExportJob::threadDone()
{
    if (m_running.fetch_sub(1) == 1)
        QMetaObject::invokeMethod(this, [this] { finish(); }, Qt::QueuedConnection);
}

void ExportJob::finish()
{
    for (std::thread& t : m_threads)
        if (t.joinable())
            t.join();
    m_threads.clear();
    m_render.reset();
    m_surface.reset();
    m_done = true;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        error = m_error;
        m_stats.frames = m_encoded.load();
        m_stats.seconds = secondsSince(m_startTime);
    }
    if (!error.empty())
        emit finished(false, QString::fromStdString(error));
    else if (m_cancel)
        emit finished(false, tr("Cancelled"));
    else
        emit finished(true, QString());
}

void ExportJob::decodeLoop(int slot)
{
    const auto done = qScopeGuard([this] { threadDone(); });
    const char name = slot ? 'B' : 'A';
    const std::shared_ptr<IVideoSource>& source = m_spec.sources[size_t(slot)];
    BoundedQueue<DecodedFrame>& out = *m_shared->decoded[size_t(slot)];
    const FrameIndexPtr idx = source->index();
    // The frames the range shows, in order. Positions only move forward in this video (both
    // sync modes), so one seek and decoding on cover them.
    std::vector<int32_t> wanted;
    for (int64_t pos = m_spec.first; pos <= m_spec.last; ++pos) {
        const int32_t n = m_spec.mapper.local(slot, pos).frame;
        if (wanted.empty() || n != wanted.back()) {
            if (!wanted.empty() && n < wanted.back()) {
                fail(std::string("the timeline goes back in video ") + name);
                return;
            }
            wanted.push_back(n);
        }
    }
    std::string err;
    std::unique_ptr<IDecodeSession> session =
        source->openSession(SessionConfig{std::make_shared<FramePool>(std::string("export-") + name), 0, m_spec.hw}, &err);
    if (!session || !idx) {
        fail(std::string("cannot decode video ") + name + (err.empty() ? std::string() : ": " + err));
        return;
    }
    size_t next = 0;
    // Frames between the wanted ones are skipped where the codec allows (non-reference).
    const std::function<bool(int32_t)> needed = [&](int32_t f) {
        return std::binary_search(wanted.begin() + std::ptrdiff_t(next), wanted.end(), f);
    };
    // Frames decoded on a CUDA GPU stay there: the renderer maps them without a copy.
    const std::function<bool(int32_t)> keepOnGpu = [](int32_t) { return true; };
    DecodeHints hints;
    hints.frameNeeded = &needed;
    hints.keepOnGpu = &keepOnGpu;
    hints.cancel = &m_cancelGeneration;
    hints.generation = 0;
    double busy = 0;
    int missing = 0;
    FramePtr lastFrame;
    bool seek = true;
    while (next < wanted.size() && !m_cancel) {
        if (seek && session->seekTo(idx->seekPointFor(wanted[next])) != DecodeStatus::Ok) {
            fail(std::string("cannot seek in video ") + name);
            return;
        }
        seek = false;
        FramePtr f;
        const auto t0 = Clock::now();
        const DecodeStatus st = session->next(f, hints);
        busy += secondsSince(t0);
        if (st == DecodeStatus::Cancelled)
            break;
        if (st == DecodeStatus::Ok) {
            if (f->pres < wanted[next])
                continue;
            // A frame the decoder went past (corrupt, not decodable) gets this one instead.
            while (next < wanted.size() && wanted[next] <= f->pres) {
                missing += wanted[next] != f->pres;
                if (!out.push({wanted[next], f}))
                    break;
                m_shared->notify();
                ++next;
            }
            lastFrame = std::move(f);
            continue;
        }
        // End of stream or a decoding error: the last good frame stands in for the next one,
        // then decoding resumes from the key before the one after it.
        if (!lastFrame) {
            fail(std::string("cannot decode video ") + name + " at frame " + std::to_string(wanted[next]));
            return;
        }
        ++missing;
        if (!out.push({wanted[next], lastFrame}))
            break;
        m_shared->notify();
        ++next;
        seek = st == DecodeStatus::Error;
    }
    if (missing)
        SCVP_WARN("export: %d frames of video %c could not be decoded; neighbours were used", missing, name);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats.decode[size_t(slot)] = busy;
    m_stats.missing += missing;
}

void ExportJob::renderLoop()
{
    const auto done = qScopeGuard([this] { threadDone(); });
    QString err;
    if (!m_render->open(m_surface.get(), &err)) {
        m_render->close();
        fail(err.toStdString());
        return;
    }
    for (;;) {
        const uint64_t seen = m_shared->seen();
        const RenderStage::Result r = m_cancel ? RenderStage::Result::Failed : m_render->pump();
        if (r == RenderStage::Result::Done) {
            m_shared->pictures.close();  // the encoder finishes what is queued
            break;
        }
        if (r == RenderStage::Result::Failed) {
            if (!m_cancel)
                fail(m_render->error().toStdString());
            break;
        }
        if (r == RenderStage::Result::Idle)
            m_shared->waitFor(seen, std::chrono::milliseconds(20));
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.render = m_render->busy();
    }
    m_render->close();
}

void ExportJob::encodeLoop()
{
    const auto done = qScopeGuard([this] { threadDone(); });
    VideoEncoder enc;
    std::string err;
    if (!enc.open(m_spec.path, m_spec.encoder, &err)) {
        fail(err);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.encoder = enc.encoderName();
    }
    const int w = m_spec.encoder.width, h = m_spec.encoder.height;
    const int stride = w * 4;
    double busy = 0;
    std::vector<uint8_t> buf;
    while (m_shared->pictures.pop(buf)) {
        m_shared->notify();  // room for another picture
        if (m_cancel)
            break;
        const auto t0 = Clock::now();
        // Read back bottom row first: start at the top row and go up.
        const bool ok = enc.write(buf.data() + size_t(h - 1) * size_t(stride), -stride, &err);
        busy += secondsSince(t0);
        m_shared->giveBack(std::move(buf));
        if (!ok) {
            fail(err);
            break;
        }
        m_encoded.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.encode = busy;
    }
    if (m_cancel || m_encoded.load() != m_spec.frames()) {
        enc.abort();
        if (!m_cancel)
            fail("the export ended early");
        return;
    }
    const auto t0 = Clock::now();
    if (!enc.finish(&err))
        fail(err);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stats.encode += secondsSince(t0);
}

} // namespace scvp::ui
