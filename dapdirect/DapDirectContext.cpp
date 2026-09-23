/*
 * Dolby DAP-direct AIDL effect — engine context.
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "AHAL_Effect_DapDirectQti"

#include "DapDirectContext.h"

#include <dlfcn.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <android-base/logging.h>

namespace aidl::qti::effects {

// Engine loaded at runtime from the (already-shipped) game-DAP effect library.
static constexpr const char* kEngineLib = "libswgamedapaidl.so";
static constexpr const char* kEngineLibPath = "/vendor/lib64/soundfx/libswgamedapaidl.so";

// exact mangled names of the exported wrapper-class constructors
static constexpr const char* kSymGenCtor = "_ZN5dolby12DlbGenClass2C1Ev";
static constexpr const char* kSymDapCtor =
        "_ZN5dolby11Dap2ProcessC1ENS_11IDlbProcess23dap_tunable_init_paramsEbbPKc";
static constexpr const char* kSymAdaCtor = "_ZN5dolby16DapBufferAdapterC1EPNS_11IDlbProcessE";
static constexpr const char* kSymGameCtor =
        "_ZN5dolby13DlbEffectGameC1EPNS_11IDlbProcessEPKcS4_";

// heap sizes for the engine objects (from the sizes GameEffectContext::init news)
static constexpr size_t kSzGen = 0x140;
static constexpr size_t kSzDap = 0x340;
static constexpr size_t kSzAda = 0x120;
static constexpr size_t kSzGame = 0x240;

// engine (DlbEffectGame / IDlbProcess) vtable byte-offsets, driven exactly as
// DlbEffectContext does (via the live object vptr, NOT hardcoded addresses).
static constexpr int kVtInit = 0x10;      // int init(void*)
static constexpr int kVtProcess = 0x20;   // int process(void*, audio_buffer_t* in, out)
static constexpr int kVtEnable = 0x30;    // int setEnabled(void*, int)
static constexpr int kVtConfigure = 0x48; // int configure(void*, buffer_config_t* in, out)

static constexpr int kBlockFrames = 256;  // engine block at <=48k

// dap_cpdp handle lives at Dap2Process + 0x30 (str x0,[x19,#0x30] right after
// dap_cpdp_init in Dap2Process::open()).
static constexpr int kDapCpdpHandleOff = 0x30;

// legacy buffer_config_t (system/audio_effect.h), 0x38 bytes
struct buffer_config_t {
    uint64_t frameCount;
    void* raw;
    uint32_t samplingRate;
    uint32_t channels;
    uint8_t provider[24];
    uint8_t format;
    uint8_t accessMode;
    uint16_t mask;
    uint8_t pad[4];
};
static_assert(sizeof(buffer_config_t) == 0x38, "buffer_config_t size");

// audio_buffer_t descriptor: {frameCount; raw}
struct audio_buffer_t {
    uint64_t frameCount;
    void* raw;
};

static inline void* vslot(void* obj, int byteoff) {
    void** vt = *reinterpret_cast<void***>(obj);
    return vt[byteoff / 8];
}

DapDirectContext::DapDirectContext(const Parameter::Common& common, bool processData)
    : EffectContext(common, processData) {
    mSampleRate = common.input.base.sampleRate;
    mChannelCount = static_cast<int>(getChannelCount(common.input.base.channelMask));
    LOG(INFO) << __func__ << " sr=" << mSampleRate << " ch=" << mChannelCount;

    if (mChannelCount != 2) {
        LOG(WARNING) << __func__ << " non-stereo (" << mChannelCount
                     << "ch) — DAP runs stereo only, will pass through";
        return;
    }
    if (!loadEngine() || !buildGraph() || !configureEngine(mSampleRate, mChannelCount)) {
        LOG(ERROR) << __func__ << " engine setup failed — will pass through";
        teardown();
        return;
    }
    mScratch.resize(static_cast<size_t>(kBlockFrames) * mChannelCount);
    mEngineReady = true;
    LOG(INFO) << __func__ << " DAP engine ready";
}

DapDirectContext::~DapDirectContext() {
    teardown();
}

bool DapDirectContext::loadEngine() {
    mLib = dlopen(kEngineLib, RTLD_NOW | RTLD_GLOBAL);
    if (!mLib) mLib = dlopen(kEngineLibPath, RTLD_NOW | RTLD_GLOBAL);
    if (!mLib) {
        LOG(ERROR) << __func__ << " dlopen(" << kEngineLib << ") failed: " << dlerror();
        return false;
    }
    mGenCtor = reinterpret_cast<decltype(mGenCtor)>(dlsym(mLib, kSymGenCtor));
    mDapCtor = reinterpret_cast<decltype(mDapCtor)>(dlsym(mLib, kSymDapCtor));
    mAdaCtor = reinterpret_cast<decltype(mAdaCtor)>(dlsym(mLib, kSymAdaCtor));
    mGameCtor = reinterpret_cast<decltype(mGameCtor)>(dlsym(mLib, kSymGameCtor));
    if (!mGenCtor || !mDapCtor || !mAdaCtor || !mGameCtor) {
        LOG(ERROR) << __func__ << " missing engine symbols gen=" << (void*)mGenCtor
                   << " dap=" << (void*)mDapCtor << " ada=" << (void*)mAdaCtor
                   << " game=" << (void*)mGameCtor;
        return false;
    }
    return true;
}

bool DapDirectContext::buildGraph() {
    mGen = calloc(1, kSzGen);
    mDap = calloc(1, kSzDap);
    mAdapter = calloc(1, kSzAda);
    mGame = calloc(1, kSzGame);
    if (!mGen || !mDap || !mAdapter || !mGame) return false;

    mGenCtor(mGen);
    int status = (reinterpret_cast<int (*)(void*)>(vslot(mGen, 0x10)))(mGen);
    (reinterpret_cast<void (*)(void*)>(vslot(mGen, 0x08)))(mGen);

    // Dap2Process(dap_tunable_init_params=0 /*8B by value*/, false, status&1, name)
    mDapCtor(mDap, 0, false, (status & 1) != 0, "DlbGameDap2Process");
    mAdaCtor(mAdapter, mDap);
    mGameCtor(mGame, mAdapter, "dummy.txt", "GameDap");
    LOG(INFO) << __func__ << " graph built (genStatus=" << status << ")";
    return true;
}

bool DapDirectContext::configureEngine(int sampleRate, int channelCount) {
    buffer_config_t in{};
    in.frameCount = kBlockFrames;
    in.raw = nullptr;
    in.samplingRate = static_cast<uint32_t>(sampleRate);
    in.channels = (channelCount == 2) ? 0x3u : 0x1u; // stereo | mono mask
    in.format = 5;                                   // AUDIO_FORMAT_PCM_FLOAT
    in.accessMode = 0;
    in.mask = 0x1f;
    buffer_config_t out = in;

    int cfg = (reinterpret_cast<int (*)(void*, buffer_config_t*, buffer_config_t*)>(
            vslot(mGame, kVtConfigure)))(mGame, &in, &out);
    int ini = (reinterpret_cast<int (*)(void*)>(vslot(mGame, kVtInit)))(mGame);
    LOG(INFO) << __func__ << " configure=" << cfg << " init=" << ini;
    return cfg == 0 && ini == 0;
}

// DsParam ids (must match DolbyConstants.DsParam in the app).
static constexpr int32_t kPHeadphoneVirt = 101;
static constexpr int32_t kPSpeakerVirt = 102;
static constexpr int32_t kPVolumeLeveler = 103;
static constexpr int32_t kPIeqPreset = 104;
static constexpr int32_t kPDialogueEnable = 105;
static constexpr int32_t kPDialogueAmount = 108;
static constexpr int32_t kPBassEnable = 111;
static constexpr int32_t kPGeqBands = 110;       // 20 band gains
static constexpr int32_t kPStereoWidening = 113; // surround boost amount

// Fixed DAX 20-band centre frequencies (Hz), from dax-default.xml's band grid.
// dap_cpdp_graphic_equalizer_bands_set wants (dap, nbands, freqs[], gains[]).
static constexpr int kGeqBands = 20;
static constexpr int kGeqFreqs[kGeqBands] = {
        47, 141, 234, 328, 469, 656, 844, 1031, 1313, 1688,
        2250, 3000, 3750, 4688, 5813, 7125, 9000, 11250, 13875, 19688};

// Fixed engine value scales (dap_cpdp units, sourced from Dolby's dax-default.xml).
static constexpr int kVolLevelerAmount = 2;  // gentle; see note below
static constexpr int kBassBoost = 144;

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Intelligent-EQ amount per UI preset (1=Open, 2=Rich, 3=Focused). Without the
// dax band tables we approximate the presets by strength; on/off is exact.
static inline int ieqAmountForPreset(int preset) {
    switch (preset) {
        case 1: return 6;
        case 2: return 10;
        case 3: return 14;
        default: return 0;
    }
}

// Push the current mFeatures state onto the live dap_cpdp engine. Idempotent, so
// it is safe to call on every enable and on every app parameter change. Runs under
// EffectImpl::mImplMutex (shared with process()), so no engine data race.
void DapDirectContext::applyFeatures() {
    if (!mEngineReady || !mLib || !mDap) return;
    void* h = *reinterpret_cast<void**>(reinterpret_cast<char*>(mDap) + kDapCpdpHandleOff);
    if (!h) {
        LOG(INFO) << __func__ << " dap_cpdp handle not ready yet (engine not opened)";
        return; // retried from process()
    }
    auto set = [&](const char* name, int v) {
        auto fn = reinterpret_cast<void (*)(void*, int)>(dlsym(mLib, name));
        if (fn) {
            fn(h, v);
        } else {
            LOG(WARNING) << __func__ << " missing " << name;
        }
    };

    // The engine's own automatic EQ/optimizer stages are not user-facing; hard off so the
    // sound stays flat unless the app pushes an explicit user choice below.
    set("dap_cpdp_audio_optimizer_enable_set", 0);
    set("dap_cpdp_complex_equalizer_enable_set", 0);
    set("dap_cpdp_process_optimizer_enable_set", 0);

    // Surround decoder is the always-on scaffolding that makes the stereo graph
    // actually transform; the rest follow the user's per-feature choices.
    set("dap_cpdp_surround_decoder_enable_set", mFeatures.surround ? 1 : 0);

    set("dap_cpdp_speaker_virtualizer_mode_set", mFeatures.spkVirt ? 1 : 0);
    set("dap_cpdp_headphone_virtualizer_mode_set", mFeatures.hpVirt ? 1 : 0);

    set("dap_cpdp_bass_enhancer_enable_set", mFeatures.bass ? 1 : 0);
    if (mFeatures.bass) set("dap_cpdp_bass_enhancer_boost_set", kBassBoost);

    // Volume leveler runs on the whole session-0 output mix; an aggressive amount
    // pulls short quiet sounds (keyboard/UI clicks) up toward target loudness,
    // making them far too loud. Keep it gentle when enabled.
    set("dap_cpdp_volume_leveler_enable_set", mFeatures.volLeveler ? 1 : 0);
    if (mFeatures.volLeveler) set("dap_cpdp_volume_leveler_amount_set", kVolLevelerAmount);

    set("dap_cpdp_de_enable_set", mFeatures.deEnable ? 1 : 0);
    if (mFeatures.deEnable) set("dap_cpdp_de_amount_set", clampi(mFeatures.deAmount, 0, 16));

    const bool ieqOn = mFeatures.ieqPreset > 0;
    set("dap_cpdp_ieq_enable_set", ieqOn ? 1 : 0);
    if (ieqOn) set("dap_cpdp_ieq_amount_set", ieqAmountForPreset(mFeatures.ieqPreset));

    // Custom graphic EQ: engage only for a non-flat preset (a zeroed EQ = bypass).
    set("dap_cpdp_graphic_equalizer_enable_set", mFeatures.geqActive ? 1 : 0);
    if (mFeatures.geqActive) {
        auto bands = reinterpret_cast<void (*)(void*, unsigned int, const int*, const int*)>(
                dlsym(mLib, "dap_cpdp_graphic_equalizer_bands_set"));
        if (bands) {
            bands(h, kGeqBands, kGeqFreqs, mFeatures.geqGains);
        } else {
            LOG(WARNING) << __func__ << " missing dap_cpdp_graphic_equalizer_bands_set";
        }
    }

    // Stereo widening -> surround boost (engine clamps 0..96 internally).
    set("dap_cpdp_surround_boost_set", clampi(mFeatures.stereoWidening, 0, 96));

    mFeaturesApplied = true;
    LOG(INFO) << __func__ << " applied: spkVirt=" << mFeatures.spkVirt
              << " hpVirt=" << mFeatures.hpVirt << " bass=" << mFeatures.bass
              << " volLvl=" << mFeatures.volLeveler << " de=" << mFeatures.deEnable
              << "/" << mFeatures.deAmount << " ieq=" << mFeatures.ieqPreset
              << " geq=" << mFeatures.geqActive << " widen=" << mFeatures.stereoWidening;
}

// Map one app DsParam id + values into mFeatures, then re-apply live.
void DapDirectContext::setFeature(int32_t paramId, const int32_t* vals, int n) {
    if (n < 1) return;
    const int32_t v = vals[0];
    switch (paramId) {
        case kPHeadphoneVirt:  mFeatures.hpVirt = (v != 0); break;
        case kPSpeakerVirt:    mFeatures.spkVirt = (v != 0); break;
        case kPVolumeLeveler:  mFeatures.volLeveler = (v != 0); break;
        case kPBassEnable:     mFeatures.bass = (v != 0); break;
        case kPDialogueEnable: mFeatures.deEnable = (v != 0); break;
        case kPDialogueAmount: mFeatures.deAmount = v; break;
        case kPIeqPreset:      mFeatures.ieqPreset = v; break;
        case kPStereoWidening: mFeatures.stereoWidening = v; break;
        case kPGeqBands: {
            // 20 band gains; engage the EQ only if the preset isn't flat.
            bool any = false;
            for (int i = 0; i < kGeqBands; ++i) {
                const int32_t g = (i < n) ? vals[i] : 0;
                mFeatures.geqGains[i] = g;
                if (g != 0) any = true;
            }
            mFeatures.geqActive = any;
            break;
        }
        default:
            LOG(VERBOSE) << __func__ << " ignoring unmapped DsParam " << paramId;
            return; // nothing changed -> no need to re-apply
    }
    applyFeatures();
}

// Decode a legacy effect_param_t blob (the app's setParameter payload, forwarded
// verbatim by the AIDL vendor-extension path). Layout of `bytes`:
//   [status:4][psize:4][vsize:4][paramKey(psize, padded to 4)][value(vsize)]
// The app always uses psize=4, paramKey=5 (EFFECT_PARAM_CPDP_VALUES); the value is
// its own buffer whose first int32 is a sub-command. We only act on the
// SET_PROFILE_PARAMETER (0x1000000) sub-command: [sub][count][profile][id][vals...].
void DapDirectContext::handleLegacyParam(const uint8_t* bytes, size_t len) {
    static constexpr int32_t kParamKeyCpdp = 5;
    static constexpr int32_t kSubSetProfileParam = 0x1000000;

    if (bytes == nullptr || len < 12) return;
    auto rd = [&](size_t off) -> int32_t {
        int32_t x;
        std::memcpy(&x, bytes + off, sizeof(x));
        return x;
    };
    const int32_t psize = rd(4);
    const int32_t vsize = rd(8);
    if (psize != 4 || vsize < 4) return;
    const size_t paddedP = 4;               // (psize + 3) & ~3, psize == 4
    const size_t valOff = 12 + paddedP;     // == 16
    if (valOff + static_cast<size_t>(vsize) > len) return;
    if (rd(12) != kParamKeyCpdp) return;    // not our CPDP channel

    const size_t nInts = static_cast<size_t>(vsize) / 4;
    if (nInts < 1) return;
    auto vbuf = [&](size_t i) -> int32_t { return rd(valOff + i * 4); };

    if (vbuf(0) != kSubSetProfileParam) return; // enable/profile/reset: handled elsewhere
    // [0]=sub [1]=count [2]=profile [3]=paramId [4..]=values
    if (nInts < 5) return;
    const int32_t paramId = vbuf(3);
    const int nvals = static_cast<int>(nInts) - 4;
    std::vector<int32_t> vals(static_cast<size_t>(nvals));
    for (int i = 0; i < nvals; ++i) vals[static_cast<size_t>(i)] = vbuf(4 + static_cast<size_t>(i));
    setFeature(paramId, vals.data(), nvals);
}

RetCode DapDirectContext::enable() {
    if (!mEngineReady) return RetCode::SUCCESS; // passthrough mode
    int r = (reinterpret_cast<int (*)(void*, int)>(vslot(mGame, kVtEnable)))(mGame, 1);
    mEnabled = (r == 0);
    LOG(INFO) << __func__ << " setEnabled(1)=" << r;
    applyFeatures(); // apply current state now if the engine is open; else process() retries
    return (r == 0) ? RetCode::SUCCESS : RetCode::ERROR_EFFECT_LIB_ERROR;
}

RetCode DapDirectContext::disable() {
    if (!mEngineReady) return RetCode::SUCCESS;
    int r = (reinterpret_cast<int (*)(void*, int)>(vslot(mGame, kVtEnable)))(mGame, 0);
    mEnabled = false;
    // Force features to be re-applied on the next enable — the engine drops/neutralises
    // them on disable, and without this the one-time guard leaves a re-enable silent.
    mFeaturesApplied = false;
    LOG(INFO) << __func__ << " setEnabled(0)=" << r;
    return RetCode::SUCCESS;
}

RetCode DapDirectContext::reset() {
    return RetCode::SUCCESS;
}

bool DapDirectContext::processBlock(const float* in, float* out, int frames) {
    audio_buffer_t ib{static_cast<uint64_t>(frames), const_cast<float*>(in)};
    audio_buffer_t ob{static_cast<uint64_t>(frames), mScratch.data()};
    int r = (reinterpret_cast<int (*)(void*, audio_buffer_t*, audio_buffer_t*)>(
            vslot(mGame, kVtProcess)))(mGame, &ib, &ob);
    if (r != 0) return false;
    memcpy(out, mScratch.data(), static_cast<size_t>(frames) * mChannelCount * sizeof(float));
    return true;
}

int DapDirectContext::process(float* in, float* out, int samples) {
    // passthrough when the engine isn't up (non-stereo / load failure / disabled)
    if (!mEngineReady || !mEnabled) {
        if (in != out) memcpy(out, in, static_cast<size_t>(samples) * sizeof(float));
        return samples;
    }
    // dap_cpdp handle is created lazily (Dap2Process::open); if enable() ran before
    // it existed, apply features on the first processed block.
    if (!mFeaturesApplied) applyFeatures();
    const int ch = mChannelCount;
    const int totalFrames = samples / ch;
    int done = 0;
    for (int off = 0; off < totalFrames; off += kBlockFrames) {
        int frames = std::min(kBlockFrames, totalFrames - off);
        if (mScratch.size() < static_cast<size_t>(frames) * ch)
            mScratch.resize(static_cast<size_t>(frames) * ch);
        if (!processBlock(in + off * ch, out + off * ch, frames)) {
            // fall back to passthrough for the remainder on error
            int rem = (totalFrames - off) * ch;
            if (in != out) memcpy(out + off * ch, in + off * ch, rem * sizeof(float));
            done += rem;
            break;
        }
        done += frames * ch;
    }
    return done;
}

void DapDirectContext::teardown() {
    // The engine wrapper objects were raw-allocated at guessed sizes (kSz*), and
    // the vendor ctors/engine write internal state into (and, as it turns out,
    // slightly past) them. Calling free() on those blocks aborted the whole audio
    // HAL — SIGABRT inside free() from heap-metadata corruption
    // (teardown()+... -> ~DapDirectContext -> EffectImpl::close). We already
    // intentionally leak the engine's *internal* C++ state (no destructor calls);
    // extend that to the outer allocations too. Contexts are created/destroyed
    // rarely (once per effect session / stale-handle rebuild), so leaking a few KB
    // is a fair trade for never crashing audioserver. Likewise do NOT dlclose the
    // engine lib: engine threads/callbacks may still reference it, and unmapping it
    // under teardown is another crash vector; it stays mapped for the HAL's life.
    mEngineReady = false;
    mEnabled = false;
    mGame = nullptr;
    mAdapter = nullptr;
    mDap = nullptr;
    mGen = nullptr;
    mLib = nullptr;
}

} // namespace aidl::qti::effects
