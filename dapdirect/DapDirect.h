/*
 * Dolby DAP-direct AIDL effect — IEffect implementation.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <aidl/android/hardware/audio/effect/BnEffect.h>

#include "DapDirectContext.h"
#include "effect-impl/EffectImpl.h"
#include "effect-impl/EffectUUID.h"

namespace aidl::qti::effects {

// Dolby DAP-direct effect UUID (type == impl; registered with typeSameAsUuid).
// da9d0b17-dade-4daf-9b3c-d01b9a0da000
static const ::aidl::android::media::audio::common::AudioUuid kDapDirectUUID = {
        static_cast<int32_t>(0xda9d0b17),
        0xdade,
        0x4daf,
        0x9b3c,
        {0xd0, 0x1b, 0x9a, 0x0d, 0xa0, 0x00}};

static const std::string kDapDirectEffectName = "DolbyDapDirect";

static const Flags kDapDirectFlags = {
        .type = Flags::Type::INSERT,
        .insert = Flags::Insert::LAST,
        .volume = Flags::Volume::NONE,
};

static const Descriptor kDapDirectDesc = {
        .common = {.id = {.type = kDapDirectUUID, .uuid = kDapDirectUUID, .proxy = std::nullopt},
                   .flags = kDapDirectFlags,
                   .name = kDapDirectEffectName,
                   .implementor = "Dolby (sw DAP, RaB1D port)"}};

class DapDirect final : public EffectImpl {
  public:
    explicit DapDirect(const ::aidl::android::media::audio::common::AudioUuid& uuid);
    ~DapDirect() override = default;

    ndk::ScopedAStatus getDescriptor(Descriptor* _aidl_return) override;
    ndk::ScopedAStatus setParameterSpecific(const Parameter::Specific& specific) override;
    ndk::ScopedAStatus getParameterSpecific(const Parameter::Id& id,
                                            Parameter::Specific* specific) override;

    std::shared_ptr<EffectContext> createContext(const Parameter::Common& common,
                                                 bool processData) override;
    RetCode releaseContext() override;

    std::string getEffectName() override { return kDapDirectEffectName; }

    IEffect::Status effectProcessImpl(float* in, float* out, int samples) override;

  protected:
    ndk::ScopedAStatus commandImpl(CommandId id) override;

  private:
    std::shared_ptr<DapDirectContext> mContext;

    // --- App-facing parameter read-back shadow ---
    // The Dolby app talks to us through the *legacy* android.media.audiofx.AudioEffect
    // API, which reads state back with getParameter(). libaudiohal turns that into a
    // vendorEffect VendorExtension carrying a raw effect_param_t. Our engine (dap_cpdp)
    // has no readable parameter store, so we shadow every value the app writes and
    // answer reads from it. Without a valid, well-formed reply, AudioEffect.checkStatus()
    // throws "AudioEffect: bad parameter value", which crash-loops the app's
    // DolbyEffectService.onCreate() the moment Dolby is enabled.
    //
    // set/getParameterSpecific are both dispatched under EffectImpl::mImplMutex, so the
    // shadow needs no additional locking.
    bool mShadowEnabled = false;
    int32_t mShadowProfile = 0;
    // key = (profile << 32) | (uint32_t)DsParam.id  ->  the values the app last wrote
    std::map<uint64_t, std::vector<int32_t>> mShadowDap;

    // Record one legacy effect_param_t SET blob into the shadow (bounds-checked).
    void shadowSet(const uint8_t* bytes, size_t len);
    // Build the effect_param_t reply for one legacy GET request; false if malformed.
    bool buildGetResponse(const uint8_t* reqBytes, size_t reqLen,
                          std::vector<uint8_t>& outParam) const;
};

} // namespace aidl::qti::effects
