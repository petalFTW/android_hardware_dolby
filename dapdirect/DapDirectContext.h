/*
 * Dolby DAP-direct AIDL effect — self-contained software Dolby processing.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The context owns the Dolby "game DAP" software engine graph, built by
 * dlopen'ing libswgamedapaidl.so and reconstructing the exact object graph
 * that aidl::qti::effects::GameEffectContext::init assembles
 * (DlbGenClass2 -> Dap2Process -> DapBufferAdapter -> DlbEffectGame), then
 * driving it through the engine's IDlbProcess vtable the same way
 * DlbEffectContext::process does. This bypasses the Dolby DMS entirely (the
 * DMS only ever pushed an identity/passthrough virtualization matrix); the
 * engine's default profile applies real stereo processing on its own.
 *
 * Proven standalone by dap_harness.cpp (2026-07-26): configure/init/enable/
 * process all return 0 and the output is a genuine transform of the input.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "effect-impl/EffectContext.h"

namespace aidl::qti::effects {

// Desired DAP feature state, driven live from the app (DolbyController) via the
// vendor-extension parameter path. Defaults are flat/off: enabling Dolby must not
// switch on processing the user never asked for. The app pushes real choices on attach.
struct DapFeatures {
    bool spkVirt = false;     // DsParam SPEAKER_VIRTUALIZER (102)
    bool hpVirt = false;      // DsParam HEADPHONE_VIRTUALIZER (101)
    bool bass = false;        // DsParam BASS_ENHANCER_ENABLE (111)
    // OFF by default: the volume leveler is an AGC that pulls quiet content up toward
    // a loudness target. On the global session-0 mix that makes short quiet UI sounds
    // (keyboard/typing clicks) far too loud, and makes Dolby louder than bypass at low
    // volumes. Opt-in per profile only.
    bool volLeveler = false;  // DsParam VOLUME_LEVELER_ENABLE (103)
    bool deEnable = false;    // DsParam DIALOGUE_ENHANCER_ENABLE (105)
    int deAmount = 6;         // DsParam DIALOGUE_ENHANCER_AMOUNT (108), UI 1..12
    int ieqPreset = 0;        // DsParam IEQ_PRESET (104): 0=off,1..3=on presets
    bool surround = true;     // engine surround decoder (always-on scaffolding)

    // Custom graphic EQ (DsParam GEQ_BAND_GAINS 110). The app sends 20 band gains
    // in its backend unit (-100..100 == -10..+10 dB); we push them verbatim to
    // dap_cpdp_graphic_equalizer_bands_set on the fixed DAX 20-band grid (the
    // engine clamps to +-576). Engaged only when the preset isn't flat, so a
    // zeroed EQ stays transparent.
    int geqGains[20] = {0};
    bool geqActive = false;

    // Stereo widening (DsParam STEREO_WIDENING_AMOUNT 113) -> surround boost.
    // App range 4..64; engine dap_cpdp_surround_boost_set clamps 0..96.
    int stereoWidening = 0;
};

class DapDirectContext final : public EffectContext {
  public:
    DapDirectContext(const Parameter::Common& common, bool processData);
    ~DapDirectContext() override;

    RetCode enable();
    RetCode disable();
    RetCode reset();

    // Process `samples` interleaved floats in-place-safe (in may alias out).
    // Returns number of samples written to `out` (== samples, or 0 on bypass).
    int process(float* in, float* out, int samples);

    bool engineReady() const { return mEngineReady; }

    // Decode one legacy effect_param_t blob forwarded by the app (via the AIDL
    // vendor-extension path) and update/apply the corresponding DAP feature.
    // Fully bounds-checked: a malformed blob is ignored, never fatal.
    void handleLegacyParam(const uint8_t* bytes, size_t len);

  private:
    bool loadEngine();
    bool buildGraph();
    bool configureEngine(int sampleRate, int channelCount);
    // Push the current mFeatures state directly onto the engine's dap_cpdp handle
    // (Dap2Process+0x30). Our raw graph never loads dax-default.xml, so without
    // this the engine runs all features neutral = inaudible. Idempotent; applied
    // once the handle exists (retried from process() if not ready at enable()).
    void applyFeatures();
    // Update mFeatures from one app DsParam id + values, then re-apply live.
    void setFeature(int32_t paramId, const int32_t* vals, int n);
    void teardown();

    // one 256-frame block through the engine (frames <= kBlockFrames)
    bool processBlock(const float* in, float* out, int frames);

    // dlopen handle + engine objects
    void* mLib = nullptr;
    void* mGen = nullptr;
    void* mDap = nullptr;
    void* mAdapter = nullptr;
    void* mGame = nullptr;

    // engine ctor entry points (resolved by mangled name)
    void (*mGenCtor)(void*) = nullptr;
    void (*mDapCtor)(void*, uint64_t, bool, bool, const char*) = nullptr;
    void (*mAdaCtor)(void*, void*) = nullptr;
    void (*mGameCtor)(void*, void*, const char*, const char*) = nullptr;

    int mSampleRate = 48000;
    int mChannelCount = 2;
    bool mEngineReady = false;
    bool mEnabled = false;
    bool mFeaturesApplied = false;

    // live DAP feature state (driven by the app; see handleLegacyParam)
    DapFeatures mFeatures;

    // scratch output block (engine must not write aliased input)
    std::vector<float> mScratch;
};

} // namespace aidl::qti::effects
