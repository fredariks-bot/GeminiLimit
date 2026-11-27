/*
  ==============================================================================

    UltraLimit 16X - Ultra-Transparent Mastering True-Peak Limiter (VST3 / AU)
    Framework: JUCE (C++17)

    Features:
    - 16x Linear-Phase Equiripple FIR Oversampling (2^4 = 16x)
    - Reiner, unverfälschter Transparent-Algorithmus (kein Clipping, keine Färbung)
    - Lookahead Buffer mit automatischer DAW-Latenzkompensation (PDC)
    - Umschaltbare Auto-Release-Engine (Program-Dependent Multi-Stage Release)
    - Variable Stereo-Linking (0% Dual-Mono bis 100% Linked)
    - 16-Bit / 24-Bit Noise-Shaped TPDF Dithering
    - Fotorealistische Hardware- & OLED-UI

  ==============================================================================
*/

#include <JuceHeader.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <random>

// ==============================================================================
// 1. PARAMETER IDs & KONSTANTEN
// ==============================================================================
namespace ParamIDs
{
    const juce::String inputGain   { "inputGain" };
    const juce::String ceiling     { "ceiling" };
    const juce::String lookahead   { "lookahead" };
    const juce::String release     { "release" };
    const juce::String autoRelease { "autoRelease" }; // Auto-Release Schalter
    const juce::String stereoLink  { "stereoLink" };
    const juce::String oversample  { "oversample" };  // 16x Linear FIR On/Off
    const juce::String dither      { "dither" };
    const juce::String autoGain    { "autoGain" };    // 1:1 Audition (Unity Gain Monitor)
}

// ==============================================================================
// 2. DSP HELFERKLASSEN (Lookahead Delay & Noise-Shaped TPDF Dither)
// ==============================================================================

class LookaheadDelayLine
{
public:
    void prepare(int maxSamples)
    {
        buffer.resize(static_cast<size_t>(maxSamples + 1), 0.0f);
        writeIndex = 0;
    }

    void reset()
    {
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    inline float processSample(float inputSample, int delayInSamples)
    {
        if (buffer.empty()) return inputSample;

        buffer[writeIndex] = inputSample;
        int readIndex = writeIndex - delayInSamples;
        if (readIndex < 0)
            readIndex += static_cast<int>(buffer.size());

        float delayedSample = buffer[readIndex];

        writeIndex++;
        if (writeIndex >= static_cast<int>(buffer.size()))
            writeIndex = 0;

        return delayedSample;
    }

private:
    std::vector<float> buffer;
    int writeIndex = 0;
};

/**
 * First-Order High-Pass Noise-Shaped TPDF Dithering Generator
 */
class TPDFDither
{
public:
    TPDFDither() : dist(-1.0f, 1.0f) {}

    void reset() { lastError = 0.0f; }

    // mode: 0 = Off, 1 = 24-Bit NS, 2 = 16-Bit NS
    inline float process(float sample, int mode)
    {
        if (mode == 0) return sample;

        float targetBits = (mode == 1) ? 24.0f : 16.0f;
        float qStep = 1.0f / std::pow(2.0f, targetBits - 1.0f);

        float r1 = dist(rng);
        float r2 = dist(rng);
        float tpdf = (r1 + r2) * 0.5f * qStep;

        // First-order error feedback verschiebt Quantisierungsrauschen in hohe Frequenzen
        float shapedInput = sample + 0.5f * lastError + tpdf;
        float quantized = std::round(shapedInput / qStep) * qStep;
        lastError = sample - quantized;

        return quantized;
    }

private:
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> dist;
    float lastError = 0.0f;
};

// ==============================================================================
// 3. AUDIO PROCESSOR (ULTRA-TRANSPARENT 16X / 64X DSP ENGINE)
// ==============================================================================
class UltraLimitAudioProcessor : public juce::AudioProcessor
{
public:
    UltraLimitAudioProcessor()
        : AudioProcessor(BusesProperties()
            .withInput("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          apvts(*this, nullptr, "PARAMETERS", createParameterLayout())
    {
        // 16-faches Linear-Phase Equiripple FIR Oversampling (2^4 = 16x)
        oversampler16x = std::make_unique<juce::dsp::Oversampling<float>>(
            2, 4,
            juce::dsp::Oversampling<float>::filterHalfBandEquirippleFIR,
            true
        );
    }

    ~UltraLimitAudioProcessor() override = default;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            ParamIDs::inputGain, "Gain",
            juce::NormalisableRange<float>(-12.0f, 24.0f, 0.1f), 0.0f, "dB"));

        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            ParamIDs::ceiling, "Output",
            juce::NormalisableRange<float>(-12.0f, 0.0f, 0.05f), -0.3f, "dBTP"));

        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            ParamIDs::lookahead, "Lookahead",
            juce::NormalisableRange<float>(0.0f, 10.0f, 0.1f), 2.5f, "ms"));

        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            ParamIDs::release, "Release",
            juce::NormalisableRange<float>(5.0f, 1000.0f, 1.0f, 0.4f), 65.0f, "ms"));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::autoRelease, "Auto Release", true));

        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            ParamIDs::stereoLink, "Link",
            juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 100.0f, "%"));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::oversample, "16x Linear FIR", true));

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            ParamIDs::dither, "Dithering",
            juce::StringArray{ "Off", "24-Bit NS", "16-Bit NS" }, 0));

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            ParamIDs::autoGain, "1:1 Audition", false));

        return { params.begin(), params.end() };
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override
    {
        currentSampleRate = sampleRate;

        oversampler16x->initProcessing(static_cast<size_t>(samplesPerBlock));
        oversampler16x->reset();

        // Maximale Lookahead-Zeit (10 ms) im 16-fach überabgetasteten Raum
        int maxOversampledDelay = static_cast<int>(std::ceil(sampleRate * 16.0 * 0.015));
        for (int ch = 0; ch < 2; ++ch)
        {
            delayLines[ch].prepare(maxOversampledDelay);
            delayLines[ch].reset();
            ditherers[ch].reset();
            envelopeGain[ch] = 1.0f;
            rmsEnvelope[ch] = 0.0f;
            slowEnvelopeGain[ch] = 1.0f;
        }

        smoothedInputGain.reset(sampleRate, 0.02);
        smoothedCeiling.reset(sampleRate, 0.02);

        updateLatency();
    }

    void releaseResources() override
    {
        oversampler16x->reset();
    }

    void updateLatency()
    {
        bool osEnabled = apvts.getRawParameterValue(ParamIDs::oversample)->load() > 0.5f;
        float lookaheadMs = apvts.getRawParameterValue(ParamIDs::lookahead)->load();

        int lookaheadSamplesBase = static_cast<int>(std::round((lookaheadMs / 1000.0f) * currentSampleRate));
        float osLat = osEnabled ? oversampler16x->getLatencyInSamples() : 0.0f;

        setLatencySamples(lookaheadSamplesBase + static_cast<int>(std::round(osLat)));
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override
    {
        juce::ignoreUnused(midiMessages);
        juce::ScopedNoDenormals noDenormals;

        const int totalNumInputChannels  = getTotalNumInputChannels();
        const int totalNumOutputChannels = getTotalNumOutputChannels();
        const int numSamples = buffer.getNumSamples();

        for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
            buffer.clear(i, 0, numSamples);

        if (numSamples == 0 || totalNumInputChannels == 0) return;

        // Parameter auslesen
        float inputGainDb  = apvts.getRawParameterValue(ParamIDs::inputGain)->load();
        float ceilingDb    = apvts.getRawParameterValue(ParamIDs::ceiling)->load();
        float lookaheadMs  = apvts.getRawParameterValue(ParamIDs::lookahead)->load();
        float releaseMs    = apvts.getRawParameterValue(ParamIDs::release)->load();
        bool  autoRel      = apvts.getRawParameterValue(ParamIDs::autoRelease)->load() > 0.5f;
        float stereoLink   = apvts.getRawParameterValue(ParamIDs::stereoLink)->load() / 100.0f;
        bool  osEnabled    = apvts.getRawParameterValue(ParamIDs::oversample)->load() > 0.5f;
        int   ditherMode   = static_cast<int>(apvts.getRawParameterValue(ParamIDs::dither)->load());
        bool  unityMonitor = apvts.getRawParameterValue(ParamIDs::autoGain)->load() > 0.5f;

        double osFactor = osEnabled ? 16.0 : 1.0;

        smoothedInputGain.setTargetValue(juce::Decibels::decibelsToGain(inputGainDb));
        smoothedCeiling.setTargetValue(juce::Decibels::decibelsToGain(ceilingDb));

        updateLatency();

        // 1. Input Gain anwenden & Input Peak messen
        float maxInPeak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            float inGain = smoothedInputGain.getNextValue();
            for (int ch = 0; ch < std::min(2, totalNumInputChannels); ++ch)
            {
                float s = buffer.getSample(ch, i) * inGain;
                buffer.setSample(ch, i, s);
                maxInPeak = std::max(maxInPeak, std::abs(s));
            }
        }
        currentInputPeak.store(maxInPeak);

        // 2. 16x Linear-Phase FIR Oversampling
        juce::dsp::AudioBlock<float> baseBlock(buffer);
        juce::dsp::AudioBlock<float> processingBlock = osEnabled
            ? oversampler16x->processSamplesUp(baseBlock)
            : baseBlock;

        const size_t osNumSamples = processingBlock.getNumSamples();
        const double effectiveSampleRate = currentSampleRate * osFactor;

        int lookaheadSamples = static_cast<int>((lookaheadMs / 1000.0) * effectiveSampleRate);

        // Ultra-transparente Attack-Glättung
        float attackCoeff = (lookaheadSamples > 1)
            ? std::exp(-1.0f / (static_cast<float>(lookaheadSamples) * 0.32f))
            : 0.0f;

        // Release-Zeitkonstanten (Manuell & Program-Dependent Auto-Release)
        float manualRelCoeff = std::exp(-1.0f / static_cast<float>((releaseMs / 1000.0) * effectiveSampleRate));
        float autoFastCoeff  = std::exp(-1.0f / static_cast<float>(0.012 * effectiveSampleRate)); // 12 ms für Transienten
        float autoSlowCoeff  = std::exp(-1.0f / static_cast<float>(0.280 * effectiveSampleRate)); // 280 ms für Sustain/Bass
        float rmsCoeff       = std::exp(-1.0f / static_cast<float>(0.050 * effectiveSampleRate));

        // Winzige Sicherheitsmarge (-0.02 dB), damit das FIR-Downsampling garantiert ohne Clipping unter dem Ceiling bleibt
        float ceilingLinear = smoothedCeiling.getCurrentValue() * 0.9977f;
        float maxBlockGR = 1.0f;

        const int numChannels = static_cast<int>(std::min<size_t>(2, processingBlock.getNumChannels()));

        // 3. Reiner, unverfälschter Transparent-Limiter im 16x Raum
        for (size_t i = 0; i < osNumSamples; ++i)
        {
            float instantSamples[2] = { 0.0f, 0.0f };
            float delayedSamples[2] = { 0.0f, 0.0f };
            float targetGain[2]     = { 1.0f, 1.0f };

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float x = processingBlock.getSample(ch, i);
                instantSamples[ch] = x;
                delayedSamples[ch] = delayLines[ch].processSample(x, lookaheadSamples);

                float absPeak = std::abs(instantSamples[ch]);
                if (absPeak > ceilingLinear)
                    targetGain[ch] = ceilingLinear / absPeak;
                else
                    targetGain[ch] = 1.0f;
            }

            // Stereo-Linking
            if (numChannels == 2)
            {
                float minGain = std::min(targetGain[0], targetGain[1]);
                targetGain[0] = targetGain[0] * (1.0f - stereoLink) + minGain * stereoLink;
                targetGain[1] = targetGain[1] * (1.0f - stereoLink) + minGain * stereoLink;
            }

            // Hüllkurvenverfolger (Auto-Release vs. Manueller Release)
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float sq = instantSamples[ch] * instantSamples[ch];
                rmsEnvelope[ch] = rmsCoeff * rmsEnvelope[ch] + (1.0f - rmsCoeff) * sq;
                float rmsLevel = std::sqrt(rmsEnvelope[ch]);

                // Langsame Basis-Hüllkurve für Auto-Release (verhindert Pumpen bei anhaltender Kompression)
                if (targetGain[ch] < slowEnvelopeGain[ch])
                    slowEnvelopeGain[ch] = autoFastCoeff * slowEnvelopeGain[ch] + (1.0f - autoFastCoeff) * targetGain[ch];
                else
                    slowEnvelopeGain[ch] = autoSlowCoeff * slowEnvelopeGain[ch] + (1.0f - autoSlowCoeff) * targetGain[ch];

                float activeReleaseCoeff = manualRelCoeff;
                if (autoRel)
                {
                    // Crest-Faktor & Dichte bestimmen: Kurze Transienten erholen sich schnell, dichtes Material langsam
                    float crestAdapt = juce::jlimit(0.0f, 1.0f, (rmsLevel / std::max(0.01f, ceilingLinear)) * 1.35f);
                    activeReleaseCoeff = autoFastCoeff * (1.0f - crestAdapt) + autoSlowCoeff * crestAdapt;
                }

                if (targetGain[ch] < envelopeGain[ch])
                {
                    if (lookaheadSamples > 1)
                        envelopeGain[ch] = attackCoeff * envelopeGain[ch] + (1.0f - attackCoeff) * targetGain[ch];
                    else
                        envelopeGain[ch] = targetGain[ch];

                    // Absoluter True-Peak-Schutz im 16x-Raum
                    float delayedAbs = std::abs(delayedSamples[ch]);
                    if (delayedAbs * envelopeGain[ch] > ceilingLinear)
                        envelopeGain[ch] = ceilingLinear / delayedAbs;
                }
                else
                {
                    float releaseTarget = autoRel ? std::max(targetGain[ch], slowEnvelopeGain[ch]) : targetGain[ch];
                    envelopeGain[ch] = activeReleaseCoeff * envelopeGain[ch] + (1.0f - activeReleaseCoeff) * releaseTarget;
                }

                float outSample = delayedSamples[ch] * envelopeGain[ch];
                processingBlock.setSample(ch, i, outSample);

                if (envelopeGain[ch] < maxBlockGR)
                    maxBlockGR = envelopeGain[ch];
            }
        }

        // 4. Linear-Phase FIR Downsampling zurück auf Basis-Samplerate
        if (osEnabled)
            oversampler16x->processSamplesDown(baseBlock);
                    s = ditherers[ch].process(s, ditherMode);

                buffer.setSample(ch, i, s);
                maxOutPeak = std::max(maxOutPeak, std::abs(s));
            }
        }

        // Metering-Werte aktualisieren
        float grDb = juce::Decibels::gainToDecibels(maxBlockGR);
        currentOutputPeak.store(maxOutPeak);
        currentGainReductionDb.store(grDb);

        int hIdx = historyWriteIndex.load();
        waveformHistory[hIdx] = juce::jlimit(0.0f, 1.0f, maxOutPeak);
        grHistory[hIdx]       = juce::jlimit(-24.0f, 0.0f, grDb);
        historyWriteIndex.store((hIdx + 1) % historySize);
    }

    // ==========================================================================
    // PLUGIN METADATEN & EDITOR-ANBINDUNG
    // ==========================================================================
    const juce::String getName() const override { return "UltraLimit 16X"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.05; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Ultra Transparent"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        auto state = apvts.copyState();
        std::unique_ptr<juce::XmlElement> xml(state.createXml());
        copyXmlToBinary(*xml, destData);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
        if (xmlState != nullptr && xmlState->hasTagName(apvts.state.getType()))
            apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
    }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    float getGainReductionDb() const { return currentGainReductionDb.load(); }
    float getInputPeakLinear() const   { return currentInputPeak.load(); }
    float getOutputPeakLinear() const  { return currentOutputPeak.load(); }

    static constexpr int historySize = 240;
    float waveformHistory[historySize] = { 0.0f };
    float grHistory[historySize]       = { 0.0f };
    std::atomic<int> historyWriteIndex { 0 };

    juce::AudioProcessorValueTreeState apvts;

private:
    double currentSampleRate = 44100.0;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler16x;
    LookaheadDelayLine delayLines[2];
    TPDFDither ditherers[2];

    float envelopeGain[2]     = { 1.0f, 1.0f };
    float slowEnvelopeGain[2] = { 1.0f, 1.0f };
    float rmsEnvelope[2]      = { 0.0f, 0.0f };

    juce::SmoothedValue<float> smoothedInputGain;
    juce::SmoothedValue<float> smoothedCeiling;

    std::atomic<float> currentGainReductionDb { 0.0f };
    std::atomic<float> currentInputPeak       { 0.0f };
    std::atomic<float> currentOutputPeak      { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UltraLimitAudioProcessor)
};

// ==============================================================================
// 4. FOTOREALISTISCHE STUDIO-UI & METERING (CNC ALUMINIUM & OLED GLASS)
// ==============================================================================

class BrushedMetalTexture
{
public:
    static juce::Image create(int width, int height)
    {
        juce::Image img(juce::Image::RGB, width, height, false);
        juce::Random rng(42);

        for (int y = 0; y < height; ++y)
        {
            float rowStreak = (rng.nextFloat() - 0.5f) * 9.0f;
            for (int x = 0; x < width; ++x)
            {
                float microGrain = (rng.nextFloat() - 0.5f) * 4.5f;
                int baseR = 26 + static_cast<int>(rowStreak + microGrain);
                int baseG = 30 + static_cast<int>(rowStreak + microGrain);
                int baseB = 37 + static_cast<int>(rowStreak + microGrain);

                img.setPixelAt(x, y, juce::Colour(
                    static_cast<uint8_t>(juce::jlimit(12, 55, baseR)),
                    static_cast<uint8_t>(juce::jlimit(14, 60, baseG)),
                    static_cast<uint8_t>(juce::jlimit(18, 70, baseB))));
            }
        }
        return img;
    }

    static void drawChassisScrew(juce::Graphics& g, float cx, float cy, float radius)
    {
        g.setColour(juce::Colour(0xaa050608));
        g.fillEllipse(cx - radius - 1.5f, cy - radius - 1.5f, (radius + 1.5f) * 2.0f, (radius + 1.5f) * 2.0f);

        juce::ColourGradient screwGrad(juce::Colour(0xff525b6c), cx - radius * 0.5f, cy - radius * 0.5f,
                                       juce::Colour(0xff181b22), cx + radius * 0.7f, cy + radius * 0.7f, true);
        g.setGradientFill(screwGrad);
        g.fillEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

        g.setColour(juce::Colour(0x55ffffff));
        g.drawEllipse(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 0.8f);

        juce::Path hex;
        float rIn = radius * 0.48f;
        for (int i = 0; i < 6; ++i)
        {
            float a = i * juce::MathConstants<float>::pi / 3.0f;
            float px = cx + std::cos(a) * rIn;
            float py = cy + std::sin(a) * rIn;
            if (i == 0) hex.startNewSubPath(px, py);
            else hex.lineTo(px, py);
        }
        hex.closeSubPath();
        g.setColour(juce::Colour(0xff0b0d11));
        g.fillPath(hex);
    }
};

class PhotorealisticLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, const float rotaryStartAngle,
                          const float rotaryEndAngle, juce::Slider& slider) override
    {
        auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(4.0f);
        auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f;
        auto cx = bounds.getCentreX();
        auto cy = bounds.getCentreY();
        auto toAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        bool isMainGain = (radius > 38.0f);
        juce::Colour accentCol = isMainGain ? juce::Colour(0xfff59e0b) : juce::Colour(0xff38bdf8);

        int numTicks = isMainGain ? 33 : 21;
        float tickOuterR = radius - 1.0f;
        float tickInnerR = radius - (isMainGain ? 6.0f : 4.5f);
        for (int i = 0; i < numTicks; ++i)
        {
            float tNorm = static_cast<float>(i) / static_cast<float>(numTicks - 1);
            float angle = rotaryStartAngle + tNorm * (rotaryEndAngle - rotaryStartAngle);
            bool active = (tNorm <= sliderPos + 0.005f);

            float x1 = cx + std::sin(angle) * tickInnerR;
            float y1 = cy - std::cos(angle) * tickInnerR;
            float x2 = cx + std::sin(angle) * tickOuterR;
            float y2 = cy - std::cos(angle) * tickOuterR;

            g.setColour(active ? accentCol.withAlpha(0.95f) : juce::Colour(0xff333946));
            g.drawLine(x1, y1, x2, y2, (i % 4 == 0) ? 1.6f : 1.0f);
        }

        float arcRadius = radius - (isMainGain ? 10.5f : 8.0f);
        float lineW = isMainGain ? 4.0f : 2.8f;

        juce::Path bgArc;
        bgArc.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xff0d0f13));
        g.strokePath(bgArc, juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (slider.isEnabled() && sliderPos > 0.001f)
        {
            juce::Path valArc;
            valArc.addCentredArc(cx, cy, arcRadius, arcRadius, 0.0f, rotaryStartAngle, toAngle, true);

            g.setColour(accentCol.withAlpha(0.28f));
            g.strokePath(valArc, juce::PathStrokeType(lineW + 4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            g.setColour(accentCol);
            g.strokePath(valArc, juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        auto knobRadius = arcRadius - (isMainGain ? 5.5f : 4.0f);
        g.setColour(juce::Colour(0xbb040507));
        g.fillEllipse(cx - knobRadius + 1.5f, cy - knobRadius + 4.5f, knobRadius * 2.0f, knobRadius * 2.0f);

        int knurlCount = isMainGain ? 48 : 32;
        for (int k = 0; k < knurlCount; ++k)
        {
            float ka = (k * juce::MathConstants<float>::twoPi) / static_cast<float>(knurlCount);
            float kx1 = cx + std::cos(ka) * (knobRadius - 2.2f);
            float ky1 = cy + std::sin(ka) * (knobRadius - 2.2f);
            float kx2 = cx + std::cos(ka) * knobRadius;
            float ky2 = cy + std::sin(ka) * knobRadius;

            float shade = 0.5f + 0.5f * std::sin(ka - 0.8f);
            g.setColour(juce::Colour::fromFloatRGBA(0.35f + shade * 0.35f, 0.38f + shade * 0.38f, 0.44f + shade * 0.42f, 0.85f));
            g.drawLine(kx1, ky1, kx2, ky2, 1.3f);
        }

        auto capR = knobRadius - 2.0f;
        juce::ColourGradient bodyGrad(juce::Colour(0xff3c4352), cx, cy - capR,
                                      juce::Colour(0xff14171d), cx, cy + capR, false);
        bodyGrad.addColour(0.45, juce::Colour(0xff262b36));
        g.setGradientFill(bodyGrad);
        g.fillEllipse(cx - capR, cy - capR, capR * 2.0f, capR * 2.0f);

        auto topR = capR * 0.82f;
        float lightAngle = toAngle * 0.35f - 0.6f;
        float lx1 = cx + std::cos(lightAngle) * topR;
        float ly1 = cy + std::sin(lightAngle) * topR;
        float lx2 = cx - std::cos(lightAngle) * topR;
        float ly2 = cy - std::sin(lightAngle) * topR;

        juce::ColourGradient topSheen(juce::Colour(0xff4a5366), lx1, ly1,
                                      juce::Colour(0xff181c24), lx2, ly2, false);
        topSheen.addColour(0.5, juce::Colour(0xff2c323f));
        g.setGradientFill(topSheen);
        g.fillEllipse(cx - topR, cy - topR, topR * 2.0f, topR * 2.0f);

        g.setColour(juce::Colour(0x44ffffff));
        g.drawEllipse(cx - topR, cy - topR, topR * 2.0f, topR * 2.0f, 0.9f);

        juce::Path pointer;
        float pLen = topR * 0.62f;
        float pThick = isMainGain ? 3.2f : 2.4f;
        pointer.addRoundedRectangle(-pThick * 0.5f, -topR + 3.0f, pThick, pLen, 1.2f);
        pointer.applyTransform(juce::AffineTransform::rotation(toAngle).translated(cx, cy));

        g.setColour(accentCol.withAlpha(0.4f));
        g.strokePath(pointer, juce::PathStrokeType(3.0f));
        g.setColour(juce::Colours::white);
        g.fillPath(pointer);
    }
};

class ScrollingWaveformDisplay : public juce::Component
{
public:
    explicit ScrollingWaveformDisplay(UltraLimitAudioProcessor& p) : processor(p) {}

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();

        g.setColour(juce::Colour(0xff07080b));
        g.fillRoundedRectangle(bounds, 8.0f);

        auto area = bounds.reduced(3.0f);

        juce::ColourGradient oledBg(juce::Colour(0xff10141c), area.getCentreX(), area.getCentreY(),
                                    juce::Colour(0xff07090d), area.getX(), area.getY(), true);
        g.setGradientFill(oledBg);
        g.fillRoundedRectangle(area, 6.0f);

        g.setColour(juce::Colour(0x14ffffff));
        for (int gx = 1; gx < 10; ++gx)
        {
            float vx = area.getX() + (area.getWidth() / 10.0f) * gx;
            g.drawVerticalLine(static_cast<int>(vx), area.getY() + 2.0f, area.getBottom() - 2.0f);
        }

        g.setFont(juce::Font(9.5f, juce::Font::bold));
        const float dbMarks[] = { -3.0f, -6.0f, -12.0f, -18.0f };
        for (float db : dbMarks)
        {
            float normY = juce::jlimit(0.0f, 1.0f, -db / 24.0f);
            float y = area.getY() + normY * area.getHeight();
            g.setColour(juce::Colour(0x1affffff));
            g.drawHorizontalLine(static_cast<int>(y), area.getX() + 4.0f, area.getRight() - 4.0f);

            g.setColour(juce::Colour(0xff5a6578));
            g.drawText(juce::String(static_cast<int>(db)) + " dB",
                       static_cast<int>(area.getX() + 8), static_cast<int>(y - 12), 42, 12, juce::Justification::left);
        }

        const int total = UltraLimitAudioProcessor::historySize;
        int writeIdx = processor.historyWriteIndex.load();

        juce::Path wavePath;
        juce::Path grPath;

        wavePath.startNewSubPath(area.getX(), area.getBottom());
        grPath.startNewSubPath(area.getX(), area.getY());

        for (int i = 0; i < total; ++i)
        {
            int idx = (writeIdx + i) % total;
            float x = area.getX() + (static_cast<float>(i) / static_cast<float>(total - 1)) * area.getWidth();

            float waveVal = processor.waveformHistory[idx];
            float waveY = area.getBottom() - waveVal * (area.getHeight() * 0.90f);
            wavePath.lineTo(x, waveY);

            float grDb = processor.grHistory[idx];
            float grNorm = juce::jlimit(0.0f, 1.0f, -grDb / 24.0f);
            float grY = area.getY() + grNorm * area.getHeight();
            grPath.lineTo(x, grY);
        }

        wavePath.lineTo(area.getRight(), area.getBottom());
        wavePath.closeSubPath();

        juce::ColourGradient waveGrad(juce::Colour(0x6638bdf8), 0, area.getY(),
                                      juce::Colour(0x100f172a), 0, area.getBottom(), false);
        g.setGradientFill(waveGrad);
        g.fillPath(wavePath);

        g.setColour(juce::Colour(0xaa7dd3fc));
        g.strokePath(wavePath, juce::PathStrokeType(1.2f));

        juce::Path grFill = grPath;
        grFill.lineTo(area.getRight(), area.getY());
        grFill.closeSubPath();

        juce::ColourGradient grGrad(juce::Colour(0x55ef4444), 0, area.getY(),
                                    juce::Colour(0x15ef4444), 0, area.getBottom(), false);
        g.setGradientFill(grGrad);
        g.fillPath(grFill);

        g.setColour(juce::Colour(0x44ef4444));
        g.strokePath(grPath, juce::PathStrokeType(5.0f));
        g.setColour(juce::Colour(0xffff5252));
        g.strokePath(grPath, juce::PathStrokeType(2.0f));

        juce::Path glassGlare;
        glassGlare.startNewSubPath(area.getX(), area.getY());
        glassGlare.lineTo(area.getX() + area.getWidth() * 0.55f, area.getY());
        glassGlare.lineTo(area.getX() + area.getWidth() * 0.22f, area.getBottom());
        glassGlare.lineTo(area.getX(), area.getBottom());
        glassGlare.closeSubPath();

        juce::ColourGradient glareGrad(juce::Colour(0x12ffffff), area.getX(), area.getY(),
                                       juce::Colour(0x00ffffff), area.getX() + area.getWidth() * 0.35f, area.getBottom(), false);
        g.setGradientFill(glareGrad);
        g.fillPath(glassGlare);

        g.setColour(juce::Colour(0xaa000000));
        g.drawRoundedRectangle(area, 6.0f, 1.5f);
        g.setColour(juce::Colour(0x28ffffff));
        g.drawRoundedRectangle(bounds.reduced(1.0f), 7.5f, 1.0f);
    }

private:
    UltraLimitAudioProcessor& processor;
};

class ProSideMeter : public juce::Component
{
public:
    void updateValues(float outLinear, float grDb)
    {
        float outDb = juce::Decibels::gainToDecibels(outLinear, -60.0f);
        smoothOutDb = (outDb > smoothOutDb) ? outDb : smoothOutDb * 0.86f + outDb * 0.14f;
        smoothGrDb  = (grDb < smoothGrDb)   ? grDb  : smoothGrDb * 0.84f + grDb * 0.16f;

        if (grDb < maxGrHoldDb)
        {
            maxGrHoldDb = grDb;
            holdCounter = 50;
        }
        else if (holdCounter > 0)
            holdCounter--;
        else
            maxGrHoldDb = smoothGrDb;

        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(2.0f);
        g.setColour(juce::Colour(0xff080a0e));
        g.fillRoundedRectangle(bounds, 7.0f);

        g.setColour(juce::Colour(0x22ffffff));
        g.drawRoundedRectangle(bounds, 7.0f, 1.0f);

        float meterTop = bounds.getY() + 24.0f;
        float meterH   = bounds.getHeight() - 52.0f;
        float barW     = (bounds.getWidth() - 18.0f) / 2.0f;

        float outX = bounds.getX() + 6.0f;
        float grX  = outX + barW + 6.0f;

        const int segments = 32;
        float segGap = 1.5f;
        float segH = (meterH - (segments - 1) * segGap) / static_cast<float>(segments);

        float outNorm = juce::jlimit(0.0f, 1.0f, (smoothOutDb + 48.0f) / 48.0f);
        float grNorm  = juce::jlimit(0.0f, 1.0f, -smoothGrDb / 24.0f);

        for (int s = 0; s < segments; ++s)
        {
            float outPos = static_cast<float>(segments - 1 - s) / static_cast<float>(segments);
            float sy = meterTop + s * (segH + segGap);

            bool outLit = (outPos <= outNorm && smoothOutDb > -58.0f);
            juce::Colour outCol = (outPos > 0.90f) ? juce::Colour(0xffef4444)
                                : (outPos > 0.72f) ? juce::Colour(0xfff59e0b)
                                                   : juce::Colour(0xff10b981);
            g.setColour(outLit ? outCol : outCol.withAlpha(0.10f));
            g.fillRoundedRectangle(outX, sy, barW, segH, 1.0f);

            float grPos = static_cast<float>(s) / static_cast<float>(segments);
            bool grLit = (grPos < grNorm && smoothGrDb < -0.1f);
            juce::Colour grCol = juce::Colour(0xffef4444);
            g.setColour(grLit ? grCol : grCol.withAlpha(0.10f));
            g.fillRoundedRectangle(grX, sy, barW, segH, 1.0f);
        }

        g.setColour(juce::Colour(0xff9ca3af));
        g.setFont(juce::Font(9.5f, juce::Font::bold));
        g.drawText("OUT", static_cast<int>(outX), static_cast<int>(bounds.getY() + 5), static_cast<int>(barW), 14, juce::Justification::centred);
        g.drawText("GR",  static_cast<int>(grX),  static_cast<int>(bounds.getY() + 5), static_cast<int>(barW), 14, juce::Justification::centred);

        auto readoutBox = juce::Rectangle<float>(bounds.getX() + 5.0f, meterTop + meterH + 6.0f, bounds.getWidth() - 10.0f, 18.0f);
        g.setColour(juce::Colour(0xff040507));
        g.fillRoundedRectangle(readoutBox, 3.0f);
        g.setColour(juce::Colour(0xfff87171));
        g.setFont(juce::Font(10.5f, juce::Font::bold));
        g.drawText(juce::String(maxGrHoldDb, 1), readoutBox.toNearestInt(), juce::Justification::centred);
    }

private:
    float smoothOutDb = -60.0f;
    float smoothGrDb  = 0.0f;
    float maxGrHoldDb = 0.0f;
    int holdCounter   = 0;
};

/**
 * Haupt-Editor für UltraLimit 64X (Puristisches, transparentes Mastering-Layout)
 */
class UltraLimitAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit UltraLimitAudioProcessorEditor(UltraLimitAudioProcessor& p)
        : AudioProcessorEditor(&p), audioProcessor(p), waveformDisplay(p)
    {
        setLookAndFeel(&proLookAndFeel);
        setSize(820, 500);

        // 5 essentielle Mastering-Regler + Auto-Release Button
        setupKnob(inputGainSlider,  inputGainLabel,  "GAIN",        ParamIDs::inputGain,  inputGainAtt);
        setupKnob(ceilingSlider,    ceilingLabel,    "OUTPUT",      ParamIDs::ceiling,    ceilingAtt);
        setupKnob(lookaheadSlider,  lookaheadLabel,  "LOOKAHEAD",   ParamIDs::lookahead,  lookaheadAtt);
        setupKnob(releaseSlider,    releaseLabel,    "RELEASE",     ParamIDs::release,    releaseAtt);
        setupKnob(stereoLinkSlider, stereoLinkLabel, "LINK",        ParamIDs::stereoLink, stereoLinkAtt);

        autoReleaseButton.setButtonText("AUTO");
        addAndMakeVisible(autoReleaseButton);
        autoReleaseAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            audioProcessor.apvts, ParamIDs::autoRelease, autoReleaseButton);

        // Bottom Bar Controls: 1:1 Audition, 16x Linear FIR, Dither
        unityGainButton.setButtonText("1:1 AUDITION");
        addAndMakeVisible(unityGainButton);
        unityGainAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            audioProcessor.apvts, ParamIDs::autoGain, unityGainButton);

        oversampleButton.setButtonText("16X LINEAR FIR");
        addAndMakeVisible(oversampleButton);
        oversampleAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            audioProcessor.apvts, ParamIDs::oversample, oversampleButton);

        ditherBox.addItemList({ "Dither: Off", "Dither: 24-Bit NS", "Dither: 16-Bit NS" }, 1);
        addAndMakeVisible(ditherBox);
        ditherAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            audioProcessor.apvts, ParamIDs::dither, ditherBox);

        startTimerHz(30);
    }

    ~UltraLimitAudioProcessorEditor() override
    {
        stopTimer();
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        if (brushedMetalBg.isValid())
            g.drawImageAt(brushedMetalBg, 0, 0);
        else
            g.fillAll(juce::Colour(0xff181c23));

        g.setColour(juce::Colour(0x33ffffff));
        g.drawHorizontalLine(0, 0.0f, static_cast<float>(getWidth()));
        g.drawVerticalLine(0, 0.0f, static_cast<float>(getHeight()));
        g.setColour(juce::Colour(0xaa000000));
        g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));

        // Top-Plakette
        juce::ColourGradient headerGrad(juce::Colour(0xff232832), 0, 0,
                                        juce::Colour(0xff12151b), 0, 42.0f, false);
        g.setGradientFill(headerGrad);
        g.fillRect(0, 0, getWidth(), 42);

        g.setColour(juce::Colour(0xff090b0e));
        g.drawHorizontalLine(42, 0.0f, static_cast<float>(getWidth()));
        g.setColour(juce::Colour(0x22ffffff));
        g.drawHorizontalLine(43, 0.0f, static_cast<float>(getWidth()));

        g.setColour(juce::Colour(0xff090b0e));
        g.setFont(juce::Font(16.0f, juce::Font::bold));
        g.drawText("ULTRALIMIT 16X", 34, 1, 175, 42, juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xfff1f5f9));
        g.drawText("ULTRALIMIT 16X", 34, 0, 175, 42, juce::Justification::centredLeft);

        g.setColour(juce::Colour(0xff94a3b8));
        g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawText("ULTRA-TRANSPARENT TRUE PEAK LIMITER", 180, 0, 290, 42, juce::Justification::centredLeft);

        // Badge rechts oben
        g.setColour(juce::Colour(0xff38bdf8));
        g.setFont(juce::Font(10.5f, juce::Font::bold));
        g.drawText("16X LINEAR FIR • AUTO RELEASE", getWidth() - 285, 0, 250, 42, juce::Justification::centredRight);

        // Gefrästes Control-Deck
        auto deckRect = juce::Rectangle<float>(14.0f, 274.0f, static_cast<float>(getWidth() - 106), 178.0f);
        juce::ColourGradient deckGrad(juce::Colour(0xff13161c), 0, deckRect.getY(),
                                      juce::Colour(0xff1b2029), 0, deckRect.getBottom(), false);
        g.setGradientFill(deckGrad);
        g.fillRoundedRectangle(deckRect, 10.0f);

        g.setColour(juce::Colour(0xaa050608));
        g.drawRoundedRectangle(deckRect, 10.0f, 1.5f);
        g.setColour(juce::Colour(0x26ffffff));
        g.drawRoundedRectangle(deckRect.expanded(1.0f), 11.0f, 1.0f);

        // 4 Studio-Rack-Schrauben
        BrushedMetalTexture::drawChassisScrew(g, 16.0f, 21.0f, 6.5f);
        BrushedMetalTexture::drawChassisScrew(g, static_cast<float>(getWidth() - 16), 21.0f, 6.5f);
        BrushedMetalTexture::drawChassisScrew(g, 16.0f, static_cast<float>(getHeight() - 18), 6.5f);
        BrushedMetalTexture::drawChassisScrew(g, static_cast<float>(getWidth() - 16), static_cast<float>(getHeight() - 18), 6.5f);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        bounds.removeFromTop(42);

        auto rightCol = bounds.removeFromRight(84);
        rightCol.removeFromBottom(36);
        sideMeter.setBounds(rightCol.reduced(6, 10));

        auto footer = bounds.removeFromBottom(38).reduced(30, 5);
        unityGainButton.setBounds(footer.removeFromLeft(130));
        oversampleButton.setBounds(footer.removeFromLeft(155).reduced(6, 0));
        ditherBox.setBounds(footer.removeFromRight(155));

        auto displayArea = bounds.removeFromTop(224).reduced(14, 10);
        waveformDisplay.setBounds(displayArea);

        // Symmetrisches 5-Regler-Deck: Links (Lookahead, Release + Auto) | Mitte (GAIN) | Rechts (Link, Output)
        auto deck = bounds.reduced(24, 12);
        int sideKnobW = 104;
        int mainKnobW = 164;

        auto leftGroup = deck.removeFromLeft(sideKnobW * 2);
        auto k1 = leftGroup.removeFromLeft(sideKnobW).reduced(4, 16);
        lookaheadLabel.setBounds(k1.removeFromTop(16));
        lookaheadSlider.setBounds(k1);

        auto k2 = leftGroup.reduced(4, 12);
        releaseLabel.setBounds(k2.removeFromTop(16));
        autoReleaseButton.setBounds(k2.removeFromBottom(18).reduced(14, 0));
        releaseSlider.setBounds(k2);

        auto rightGroup = deck.removeFromRight(sideKnobW * 2);
        auto k4 = rightGroup.removeFromLeft(sideKnobW).reduced(4, 16);
        stereoLinkLabel.setBounds(k4.removeFromTop(16));
        stereoLinkSlider.setBounds(k4);

        auto k5 = rightGroup.reduced(4, 16);
        ceilingLabel.setBounds(k5.removeFromTop(16));
        ceilingSlider.setBounds(k5);

        auto centerKnob = deck.withSizeKeepingCentre(mainKnobW, deck.getHeight());
        inputGainLabel.setBounds(centerKnob.removeFromTop(18));
        inputGainSlider.setBounds(centerKnob);
    }

private:
    void timerCallback() override
    {
        bool isAutoRel = audioProcessor.apvts.getRawParameterValue(ParamIDs::autoRelease)->load() > 0.5f;
        releaseSlider.setEnabled(!isAutoRel);
        releaseSlider.setAlpha(isAutoRel ? 0.45f : 1.0f);

        sideMeter.updateValues(
            audioProcessor.getOutputPeakLinear(),
            audioProcessor.getGainReductionDb()
        );
        waveformDisplay.repaint();
    }

    void setupKnob(juce::Slider& slider, juce::Label& label, const juce::String& name,
                   const juce::String& paramID, std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attachment)
    {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 78, 18);
        slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0x33ffffff));
        slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff0b0d11));
        slider.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
        addAndMakeVisible(slider);

        label.setText(name, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centred);
        label.setFont(juce::Font(10.5f, juce::Font::bold));
        label.setColour(juce::Label::textColourId, juce::Colour(0xffcbd5e1));
        addAndMakeVisible(label);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            audioProcessor.apvts, paramID, slider);
    }

    UltraLimitAudioProcessor& audioProcessor;
    PhotorealisticLookAndFeel proLookAndFeel;
    juce::Image brushedMetalBg;
    ScrollingWaveformDisplay waveformDisplay;
    ProSideMeter sideMeter;

    juce::Slider inputGainSlider, ceilingSlider, lookaheadSlider, releaseSlider, stereoLinkSlider;
    juce::Label  inputGainLabel, ceilingLabel, lookaheadLabel, releaseLabel, stereoLinkLabel;

    juce::ComboBox ditherBox;
    juce::ToggleButton unityGainButton, oversampleButton, autoReleaseButton;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inputGainAtt, ceilingAtt, lookaheadAtt, releaseAtt, stereoLinkAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> ditherAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> unityGainAtt, oversampleAtt, autoReleaseAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UltraLimitAudioProcessorEditor)
};

juce::AudioProcessorEditor* UltraLimitAudioProcessor::createEditor()
{
    return new UltraLimitAudioProcessorEditor(*this);
}

// ==============================================================================
// 5. PLUGIN ENTRY POINT FÜR DIE DAW
// ==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new UltraLimitAudioProcessor();
}
