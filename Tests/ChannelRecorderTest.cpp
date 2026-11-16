#include <JuceHeader.h>

#include "I18n.h"
#include "ChannelRecorder.h"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSizes[] = { 512, 257, 1024 };
    constexpr int totalFrames = 512 + 257 + 1024;

    int failures = 0;

    void expect (bool condition, const juce::String& message)
    {
        if (! condition)
        {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    float sourceSample (int channel, int side, int frame)
    {
        const double t = static_cast<double> (frame) / sampleRate;
        constexpr double twoPi = juce::MathConstants<double>::twoPi;

        if (channel == 0)
        {
            if (side == 0)
                return static_cast<float> (0.10 + 0.25 * std::sin (twoPi * 431.0 * t));
            return static_cast<float> (-0.35 + 0.20 * std::cos (twoPi * 733.0 * t));
        }

        if (side == 0)
            return static_cast<float> (-0.20 + 0.15 * std::cos (twoPi * 283.0 * t));
        return static_cast<float> (-0.10 + 0.30 * std::sin (twoPi * 997.0 * t));
    }

    struct TestDirectory
    {
        TestDirectory()
        {
            const auto unique = juce::String (juce::Time::getCurrentTime().toMilliseconds())
                              + "_" + juce::String (juce::Random::getSystemRandom().nextInt64());
            root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("MicroBridgeRecorderTests")
                    .getChildFile (unique);
            expect (root.createDirectory().wasOk(), "create temporary test directory");
        }

        ~TestDirectory() { root.deleteRecursively(); }

        juce::File child (const juce::String& name)
        {
            auto dir = root.getChildFile (name);
            expect (dir.createDirectory().wasOk(), "create case directory " + name);
            return dir;
        }

        juce::File root;
    };

    struct DecodedStereo
    {
        int bits = 0;
        std::array<std::vector<float>, 2> samples;
    };

    bool readStereo (juce::AudioFormatManager& formats, const juce::File& file,
                     int expectedFrames, int expectedBits, DecodedStereo& decoded,
                     const juce::String& label)
    {
        expect (file.existsAsFile(), label + ": output file exists");
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        expect (reader != nullptr, label + ": create reader");
        if (reader == nullptr) return false;

        expect (reader->numChannels == 2,
                label + ": stereo channels, actual=" + juce::String (reader->numChannels));
        expect (reader->lengthInSamples == expectedFrames,
                label + ": frame count, actual=" + juce::String (reader->lengthInSamples));
        expect (reader->sampleRate == sampleRate, label + ": sample rate");
        expect (static_cast<int> (reader->bitsPerSample) == expectedBits,
                label + ": bit depth, actual=" + juce::String (reader->bitsPerSample));
        if (reader->numChannels != 2 || reader->lengthInSamples != expectedFrames)
            return false;

        juce::AudioBuffer<float> buffer (2, expectedFrames);
        buffer.clear();
        expect (reader->read (&buffer, 0, expectedFrames, 0, true, true),
                label + ": read samples");

        decoded.bits = static_cast<int> (reader->bitsPerSample);
        for (int side = 0; side < 2; ++side)
        {
            const auto* src = buffer.getReadPointer (side);
            decoded.samples[static_cast<size_t> (side)].assign (src, src + expectedFrames);
        }
        return true;
    }

    void compareSamples (const DecodedStereo& decoded, int channel,
                         float gainL, float gainR, float tolerance,
                         const juce::String& label)
    {
        std::array<float, 2> maxError { 0.0f, 0.0f };
        const auto frames = static_cast<int> (decoded.samples[0].size());
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < frames; ++i)
            {
                const float gain = side == 0 ? gainL : gainR;
                const auto expected = sourceSample (channel, side, i) * gain;
                const auto actual = decoded.samples[static_cast<size_t> (side)][static_cast<size_t> (i)];
                maxError[static_cast<size_t> (side)] = juce::jmax (
                    maxError[static_cast<size_t> (side)], std::abs (actual - expected));
            }

        expect (maxError[0] <= tolerance,
                label + ": left samples, max error=" + juce::String (maxError[0], 9));
        expect (maxError[1] <= tolerance,
                label + ": right samples, max error=" + juce::String (maxError[1], 9));
    }

    void testMixingHelpers()
    {
        std::array<float, totalFrames> left {}, right {}, sumL {}, sumR {};
        for (int i = 0; i < totalFrames; ++i)
        {
            left[static_cast<size_t> (i)] = sourceSample (0, 0, i);
            right[static_cast<size_t> (i)] = sourceSample (0, 1, i);
        }

        mb::ChannelRecorder::copyForRecording (left.data(), right.data(),
                                               sumL.data(), sumR.data(), 0.5f, totalFrames);
        for (int i = 0; i < totalFrames; ++i)
        {
            expect (sumL[static_cast<size_t> (i)] == left[static_cast<size_t> (i)] * 0.5f,
                    "mix helper: copied left sample " + juce::String (i));
            expect (sumR[static_cast<size_t> (i)] == right[static_cast<size_t> (i)] * 0.5f,
                    "mix helper: copied right sample " + juce::String (i));
        }

        mb::ChannelRecorder::addForRecording (left.data(), right.data(),
                                              sumL.data(), sumR.data(), 0.25f, totalFrames);
        float maxErrorL = 0.0f, maxErrorR = 0.0f;
        for (int i = 0; i < totalFrames; ++i)
        {
            maxErrorL = juce::jmax (maxErrorL, std::abs (
                sumL[static_cast<size_t> (i)] - left[static_cast<size_t> (i)] * 0.75f));
            maxErrorR = juce::jmax (maxErrorR, std::abs (
                sumR[static_cast<size_t> (i)] - right[static_cast<size_t> (i)] * 0.75f));
        }
        expect (maxErrorL < 1.0e-7f && maxErrorR < 1.0e-7f,
                "mix helper: independent stereo accumulation");
    }

    void testMultiTrack (juce::AudioFormatManager& formats, const juce::File& dir)
    {
        mb::ChannelRecorder recorder;
        recorder.setDirectory (dir);
        recorder.setMode (mb::ChannelRecorder::Mode::MultiTrack);
        recorder.setFormat ("WAV");
        recorder.setBitDepth (24);
        expect (recorder.start (0, "StereoA", sampleRate), "multi: start channel 0");
        expect (recorder.start (1, "StereoB", sampleRate), "multi: start channel 1");

        std::array<std::array<float, 1024>, 2> scratchL {}, scratchR {};
        int position = 0;
        for (const int n : blockSizes)
        {
            for (int channel = 0; channel < 2; ++channel)
            {
                const float gain = channel == 0 ? 0.75f : 1.25f;
                for (int i = 0; i < n; ++i)
                {
                    scratchL[static_cast<size_t> (channel)][static_cast<size_t> (i)]
                        = sourceSample (channel, 0, position + i);
                    scratchR[static_cast<size_t> (channel)][static_cast<size_t> (i)]
                        = sourceSample (channel, 1, position + i);
                }
                mb::ChannelRecorder::copyForRecording (
                    scratchL[static_cast<size_t> (channel)].data(),
                    scratchR[static_cast<size_t> (channel)].data(),
                    scratchL[static_cast<size_t> (channel)].data(),
                    scratchR[static_cast<size_t> (channel)].data(), gain, n);
                recorder.push (channel,
                               scratchL[static_cast<size_t> (channel)].data(),
                               scratchR[static_cast<size_t> (channel)].data(), n);
            }
            position += n;
        }

        const auto file1 = recorder.stop (1);
        const auto file0 = recorder.stop (0);
        expect (file0 != file1, "multi: independent output files");
        expect (recorder.activeCount() == 0, "multi: all writers stopped");

        for (int channel = 0; channel < 2; ++channel)
        {
            DecodedStereo decoded;
            const auto label = "multi CH" + juce::String (channel);
            if (readStereo (formats, channel == 0 ? file0 : file1,
                            totalFrames, 24, decoded, label))
                compareSamples (decoded, channel, channel == 0 ? 0.75f : 1.25f,
                                channel == 0 ? 0.75f : 1.25f, 2.0e-5f, label);
        }
    }

    void testSum (juce::AudioFormatManager& formats, const juce::File& dir)
    {
        mb::ChannelRecorder recorder;
        recorder.setDirectory (dir);
        recorder.setMode (mb::ChannelRecorder::Mode::Sum);
        recorder.setFormat ("WAV");
        recorder.setBitDepth (24);
        expect (recorder.start (0, "SumA", sampleRate), "sum: start channel 0");
        expect (recorder.start (1, "SumB", sampleRate), "sum: start channel 1");

        int position = 0;
        for (const int n : blockSizes)
        {
            std::array<float, 1024> mixL {}, mixR {};
            std::array<float, 1024> inL {}, inR {};
            for (int channel = 0; channel < 2; ++channel)
            {
                for (int i = 0; i < n; ++i)
                {
                    inL[static_cast<size_t> (i)] = sourceSample (channel, 0, position + i);
                    inR[static_cast<size_t> (i)] = sourceSample (channel, 1, position + i);
                }
                mb::ChannelRecorder::addForRecording (
                    inL.data(), inR.data(), mixL.data(), mixR.data(),
                    channel == 0 ? 0.5f : 0.75f, n);
            }
            recorder.pushSum (mixL.data(), mixR.data(), n);
            position += n;
        }

        const auto file0 = recorder.stop (0);
        const auto file1 = recorder.stop (1);
        expect (file0 == file1, "sum: one shared output file");
        expect (recorder.activeCount() == 0, "sum: shared writer stopped");

        DecodedStereo decoded;
        if (! readStereo (formats, file0, totalFrames, 24, decoded, "sum")) return;
        std::array<float, 2> maxError { 0.0f, 0.0f };
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < totalFrames; ++i)
            {
                const auto expected = sourceSample (0, side, i) * 0.5f
                                    + sourceSample (1, side, i) * 0.75f;
                maxError[static_cast<size_t> (side)] = juce::jmax (
                    maxError[static_cast<size_t> (side)],
                    std::abs (decoded.samples[static_cast<size_t> (side)][static_cast<size_t> (i)]
                              - expected));
            }
        expect (maxError[0] <= 2.0e-5f,
                "sum: left mix, max error=" + juce::String (maxError[0], 9));
        expect (maxError[1] <= 2.0e-5f,
                "sum: right mix, max error=" + juce::String (maxError[1], 9));
    }

    void testFormats (juce::AudioFormatManager& formats, const juce::File& root)
    {
        const juce::StringArray formatNames { "WAV", "FLAC", "AIFF" };
        for (const auto& format : formatNames)
            for (const int bits : { 16, 24 })
            {
                const auto label = format + "-" + juce::String (bits);
                mb::ChannelRecorder recorder;
                recorder.setDirectory (root.getChildFile (label));
                recorder.setMode (mb::ChannelRecorder::Mode::MultiTrack);
                recorder.setFormat (format);
                recorder.setBitDepth (bits);
                expect (recorder.start (0, "Format", sampleRate), label + ": start");

                std::array<float, totalFrames> left {}, right {};
                for (int i = 0; i < totalFrames; ++i)
                {
                    left[static_cast<size_t> (i)] = sourceSample (0, 0, i);
                    right[static_cast<size_t> (i)] = sourceSample (0, 1, i);
                }
                recorder.push (0, left.data(), right.data(), totalFrames);
                const auto file = recorder.stop (0);

                DecodedStereo decoded;
                const float tolerance = bits == 16 ? 3.0e-4f : 2.0e-5f;
                if (readStereo (formats, file, totalFrames, bits, decoded, label))
                    compareSamples (decoded, 0, 1.0f, 1.0f, tolerance, label);
            }
    }
}

int main()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    TestDirectory temp;

    testMixingHelpers();
    testMultiTrack (formats, temp.child ("MultiTrack"));
    testSum (formats, temp.child ("Sum"));
    testFormats (formats, temp.child ("Formats"));

    if (failures == 0)
    {
        std::cout << "ChannelRecorderTest: ALL PASS (mix helpers, MultiTrack, Sum, "
                     "WAV/FLAC/AIFF 16+24-bit stereo readback)\n";
        return 0;
    }

    std::cerr << "ChannelRecorderTest: " << failures << " check(s) failed\n";
    return 1;
}
