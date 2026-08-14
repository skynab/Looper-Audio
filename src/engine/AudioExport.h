#pragma once

#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/Mp3Encoder.h"

namespace looper::engine
{
/**
    Writing a rendered mix to a file, in whichever format was asked for.

    One place that knows what each format can and cannot do, so the export
    dialog can be built from these queries rather than from a second, parallel
    table of its own. That matters more than it sounds: an unsupported
    combination (FLAC at 32 bits, say) doesn't fail loudly, it returns a null
    writer - and it does so at the *end* of a long render, after the work is
    already done. A UI that can't offer the bad combination in the first place
    is the fix.
*/
enum class ExportFormat
{
    Wav = 0,
    Aiff,
    Flac,
    OggVorbis,
    Mp3
};

inline constexpr int kNumExportFormats = 5;

inline const std::array<ExportFormat, kNumExportFormats>& allExportFormats()
{
    static const std::array<ExportFormat, kNumExportFormats> formats {
        ExportFormat::Wav, ExportFormat::Aiff, ExportFormat::Flac,
        ExportFormat::OggVorbis, ExportFormat::Mp3
    };
    return formats;
}

struct ExportOptions
{
    ExportFormat format        = ExportFormat::Wav;

    /** 16, 24, or 32. For WAV and AIFF, 32 means IEEE float rather than
        32-bit integer - a float file is what you want when the mix will be
        processed further, since it cannot clip on the way out. Ignored by the
        formats where usesBitDepth() is false. */
    int          bitsPerSample = 24;

    double       sampleRate    = 48000.0;

    /** Index into qualityOptionsFor(format). Ignored where that is empty. */
    int          qualityIndex  = 3;
};

/** The file extension, without the dot. */
inline juce::String extensionFor (ExportFormat format)
{
    switch (format)
    {
        case ExportFormat::Wav:       return "wav";
        case ExportFormat::Aiff:      return "aiff";
        case ExportFormat::Flac:      return "flac";
        case ExportFormat::OggVorbis: return "ogg";
        case ExportFormat::Mp3:       return "mp3";
    }
    return "wav";
}

/** What the format is called in the export dialog, with the trade it makes -
    a format list that just says "FLAC" assumes the reader already knows. */
inline juce::String displayNameFor (ExportFormat format)
{
    switch (format)
    {
        case ExportFormat::Wav:       return "WAV (uncompressed)";
        case ExportFormat::Aiff:      return "AIFF (uncompressed)";
        case ExportFormat::Flac:      return "FLAC (lossless, compressed)";
        case ExportFormat::OggVorbis: return "Ogg Vorbis (lossy)";
        case ExportFormat::Mp3:       return "MP3 (lossy)";
    }
    return "WAV";
}

namespace detail
{
    inline std::unique_ptr<juce::AudioFormat> audioFormatFor (ExportFormat format)
    {
        switch (format)
        {
            case ExportFormat::Wav:       return std::make_unique<juce::WavAudioFormat>();
            case ExportFormat::Aiff:      return std::make_unique<juce::AiffAudioFormat>();
            case ExportFormat::Flac:      return std::make_unique<juce::FlacAudioFormat>();
            case ExportFormat::OggVorbis: return std::make_unique<juce::OggVorbisAudioFormat>();
            case ExportFormat::Mp3:       return std::make_unique<Mp3AudioFormat>();
        }
        return nullptr;
    }
}

/** Whether a bit depth means anything for this format. False for the lossy
    ones, which store frequency-domain data and have no sample depth at all. */
inline bool usesBitDepth (ExportFormat format)
{
    return format == ExportFormat::Wav
        || format == ExportFormat::Aiff
        || format == ExportFormat::Flac;
}

/** The bit depths this format can actually write, out of the ones worth
    offering.

    Asked of the codec rather than hand-listed. A hand-written table here was
    wrong twice on its first outing - it claimed 32 bits for AIFF, which JUCE's
    writer refuses - and the failure mode is the bad kind: a null writer at the
    *end* of a long render, after all the work is done. The codec is the only
    thing that actually knows.

    8-bit is filtered out rather than offered: WAV and AIFF can both write it,
    and nobody exporting a finished mix wants it. */
inline juce::Array<int> possibleBitDepths (ExportFormat format)
{
    if (! usesBitDepth (format))
        return {};

    static const juce::Array<int> worthOffering { 16, 24, 32 };

    juce::Array<int> depths;
    if (auto codec = detail::audioFormatFor (format))
        for (int bits : codec->getPossibleBitDepths())
            if (worthOffering.contains (bits))
                depths.add (bits);

    return depths;
}

/** The sample rates this format can write, out of the ones worth offering.

    Per format, not global: MPEG Layer III stops at 48kHz, so a single shared
    list would put 88.2 and 96 in front of someone choosing MP3 and fail at the
    end of the render. */
inline juce::Array<int> possibleSampleRates (ExportFormat format)
{
    // 44.1 and 48 are the two that matter; the higher pair is for handing work
    // to someone else's session.
    static const juce::Array<int> worthOffering { 44100, 48000, 88200, 96000 };

    juce::Array<int> rates;
    if (auto codec = detail::audioFormatFor (format))
        for (int rate : codec->getPossibleSampleRates())
            if (worthOffering.contains (rate))
                rates.add (rate);

    return rates;
}

/** Human-readable quality settings, or empty for the lossless formats where
    quality is not a choice. */
inline juce::StringArray qualityOptionsFor (ExportFormat format)
{
    if (usesBitDepth (format))
        return {};

    if (auto codec = detail::audioFormatFor (format))
        return codec->getQualityOptions();

    return {};
}

/**
    Writes @p buffer to @p file. Returns false, and leaves no file behind, on
    any failure.

    Deleting a failed export rather than leaving a truncated one is deliberate:
    a zero-byte or half-written .wav in the place the user chose looks like a
    successful export until they try to play it.
*/
inline bool writeAudioFile (const juce::File& file,
                            const juce::AudioBuffer<float>& buffer,
                            const ExportOptions& options)
{
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
        return false;

    auto format = detail::audioFormatFor (options.format);
    if (format == nullptr)
        return false;

    file.deleteFile();

    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    auto writerOptions = juce::AudioFormatWriterOptions{}
                             .withSampleRate (options.sampleRate)
                             .withNumChannels (buffer.getNumChannels())
                             .withQualityOptionIndex (options.qualityIndex);

    if (usesBitDepth (options.format))
    {
        const auto depths = possibleBitDepths (options.format);
        const int  bits   = depths.contains (options.bitsPerSample) ? options.bitsPerSample
                                                                    : depths.getLast();

        writerOptions = writerOptions.withBitsPerSample (bits);

        // 32-bit means *float* for WAV and AIFF. Written as 32-bit integer
        // instead, a mix that touches full scale would come back subtly
        // different, and the whole point of asking for 32 is that it doesn't.
        if (bits == 32)
            writerOptions = writerOptions.withSampleFormat (
                juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    }
    else
    {
        // The lossy formats still need a depth for the writer's bookkeeping,
        // and 16 is what either of them decodes to.
        writerOptions = writerOptions.withBitsPerSample (16);
    }

    // Consumed on success; see Mp3AudioFormat::createWriterFor for the one
    // case where a failure also takes the stream, which is why the file is
    // deleted below rather than only when the stream survives.
    auto writer = format->createWriterFor (stream, writerOptions);
    if (writer == nullptr)
    {
        file.deleteFile();
        return false;
    }

    const bool ok = writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());

    // Before the file is judged: the writer flushes its last frame and closes
    // the stream in its destructor, and for the compressed formats that is
    // where a meaningful part of the data gets written.
    writer.reset();

    if (! ok)
        file.deleteFile();

    return ok;
}

} // namespace looper::engine
