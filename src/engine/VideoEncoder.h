#pragma once

#include "engine/FfmpegCompat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace scvp {

enum class OutputFormat {
    H264,         // MP4, 4:2:0
    Hevc,         // MP4 ('hvc1'), 4:2:0
    Ffv1,         // Matroska, lossless RGB
    PngSequence,  // one PNG per picture (name_000123.png), lossless RGB
};

enum class OutputQuality { High, Medium, Small };  // lossy formats only

// An encoder that can be used for a format here: found in this FFmpeg build and opened once
// successfully (hardware encoders need their device and driver).
struct EncoderChoice {
    std::string name;   // FFmpeg encoder name ("h264_nvenc", "libx264", ...)
    std::string label;  // for people ("NVIDIA NVENC (GPU)")
    bool hardware = false;
};

struct EncoderSettings {
    OutputFormat format = OutputFormat::H264;
    std::string encoder;  // "" = the first available for the format (hardware first)
    OutputQuality quality = OutputQuality::High;
    int width = 0;        // picture size given to write()
    int height = 0;
    AVRational frameRate{25, 1};
    int startNumber = 0;  // PNG sequence: number of the first file
};

// Encodes pictures to a file with FFmpeg: H.264/HEVC in MP4, FFV1 in Matroska or a PNG
// sequence. Pictures come as 8-bit BGRX rows (the GPU read-back layout) and are converted to the
// encoder's pixel format: BT.709 limited range YUV for the lossy formats (tagged as such), RGB
// without any change for the lossless ones. 4:2:0 needs even sizes: an odd picture is encoded
// one pixel larger with its last column/row repeated. Not thread-safe; one thread drives it.
class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();  // abort() unless finish() succeeded
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool open(const std::string& path, const EncoderSettings& settings, std::string* error);
    // One picture, settings.width x height BGRX, `stride` bytes from one row to the next
    // (negative: rows go up in memory from `rows`, as glReadPixels leaves them).
    bool write(const uint8_t* rows, int stride, std::string* error);
    // Flushes the encoder and completes the file.
    bool finish(std::string* error);
    // Stops and deletes what was written (the file, or every PNG of the sequence).
    void abort();

    const std::string& encoderName() const { return m_encoderName; }
    int encodedWidth() const;   // the stream's size (4:2:0: rounded up to even)
    int encodedHeight() const;
    int64_t framesWritten() const { return m_frames; }

    // Encoders usable for `format`, preferred first (hardware before software for H.264/HEVC).
    // Probed once per process (opening hardware encoders takes up to a few hundred ms);
    // thread-safe.
    static std::vector<EncoderChoice> available(OutputFormat format);
    static const char* extension(OutputFormat format);  // "mp4", "mkv", "png"
    static bool lossless(OutputFormat format);
    // File of picture `number` of a PNG sequence written to `path` ("dir/x.png" -> "dir/x_000012.png").
    static std::string sequenceFile(const std::string& path, int number);

private:
    bool fail(std::string* error, const std::string& what, int err = 0);
    bool drain(bool flush, std::string* error);
    void close();
    void removeOutput();

    EncoderSettings m_settings;
    std::string m_path;
    std::string m_encoderName;
    ff::CodecContextPtr m_codec;
    AVFormatContext* m_format = nullptr;
    AVStream* m_stream = nullptr;
    ff::FramePtr m_frame;   // the encoder's input picture, reused
    ff::PacketPtr m_packet;
    int64_t m_frames = 0;
    int64_t m_packets = 0;  // PNG sequence: files written so far
    bool m_created = false;  // the output exists (abort() deletes it)
    bool m_headerWritten = false;
    bool m_finished = false;
};

} // namespace scvp
