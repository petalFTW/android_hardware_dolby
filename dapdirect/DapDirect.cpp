/*
 * Dolby DAP-direct AIDL effect — IEffect implementation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * A thin AIDL effect that our AHAL_EffectFactoryQti loads via createEffect().
 * All DSP happens in DapDirectContext, which reuses the Dolby game-DAP software
 * engine (dap_cpdp) directly, with no DMS / DSP-offload / license dependency.
 */
#define LOG_TAG "AHAL_Effect_DapDirectQti"

#include "DapDirect.h"

#include <cstring>
#include <optional>

#include <aidl/android/hardware/audio/effect/DefaultExtension.h>
#include <aidl/android/hardware/audio/effect/VendorExtension.h>
#include <android-base/logging.h>

using aidl::android::hardware::audio::effect::DefaultExtension;
using aidl::android::hardware::audio::effect::Descriptor;
using aidl::android::hardware::audio::effect::IEffect;
using aidl::android::hardware::audio::effect::Parameter;
using aidl::android::hardware::audio::effect::State;
using aidl::android::hardware::audio::effect::VendorExtension;
using aidl::android::media::audio::common::AudioUuid;
using aidl::qti::effects::DapDirect;
using aidl::qti::effects::kDapDirectUUID;

static bool isUuidSupported(const AudioUuid* uuid) {
    return uuid != nullptr && *uuid == kDapDirectUUID;
}

extern "C" binder_exception_t createEffect(const AudioUuid* uuid,
                                           std::shared_ptr<IEffect>* instanceSpp) {
    if (!isUuidSupported(uuid)) {
        LOG(ERROR) << __func__ << " uuid not supported";
        return EX_ILLEGAL_ARGUMENT;
    }
    if (!instanceSpp) {
        LOG(ERROR) << __func__ << " null instance pointer";
        return EX_ILLEGAL_ARGUMENT;
    }
    *instanceSpp = ndk::SharedRefBase::make<DapDirect>(*uuid);
    LOG(INFO) << __func__ << " instance " << instanceSpp->get() << " created";
    return EX_NONE;
}

extern "C" binder_exception_t queryEffect(const AudioUuid* uuid, Descriptor* _aidl_return) {
    if (!isUuidSupported(uuid)) {
        LOG(ERROR) << __func__ << " uuid not supported";
        return EX_ILLEGAL_ARGUMENT;
    }
    *_aidl_return = aidl::qti::effects::kDapDirectDesc;
    return EX_NONE;
}

namespace aidl::qti::effects {

DapDirect::DapDirect(const AudioUuid& uuid) {
    LOG(VERBOSE) << __func__ << toString(uuid);
    mDescriptor = &kDapDirectDesc;
    mEffectName = &kDapDirectEffectName;
}

ndk::ScopedAStatus DapDirect::getDescriptor(Descriptor* _aidl_return) {
    *_aidl_return = kDapDirectDesc;
    return ndk::ScopedAStatus::ok();
}

std::shared_ptr<EffectContext> DapDirect::createContext(const Parameter::Common& common,
                                                        bool processData) {
    if (mContext) {
        LOG(DEBUG) << __func__ << " context already exists";
    } else {
        mContext = std::make_shared<DapDirectContext>(common, processData);
    }
    return mContext;
}

RetCode DapDirect::releaseContext() {
    if (mContext) {
        mContext->disable();
        mContext.reset();
    }
    return RetCode::SUCCESS;
}

ndk::ScopedAStatus DapDirect::commandImpl(CommandId command) {
    RETURN_IF(!mContext, EX_NULL_POINTER, "nullContext");
    LOG(VERBOSE) << __func__ << toString(command);
    switch (command) {
        case CommandId::START:
            mContext->enable();
            break;
        case CommandId::STOP:
            mContext->disable();
            break;
        case CommandId::RESET:
            mContext->reset();
            break;
        default:
            LOG(ERROR) << __func__ << " commandId " << toString(command) << " not supported";
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "commandIdNotSupported");
    }
    return ndk::ScopedAStatus::ok();
}

namespace {

// effect_param_t (system/audio_effect.h) layout, little-endian:
//   [int32 status][uint32 psize][uint32 vsize][psize-byte key, padded to 4][vsize-byte value]
// The app always sends a single int32 key (psize == 4), so the key sits at offset 12 and
// the value starts at offset 16.
constexpr size_t kParamHdr = 12;   // status + psize + vsize
constexpr size_t kValueOff = 16;   // kParamHdr + padded psize(4)

// CPDP channel + sub-commands, mirrored from DolbyAudioEffect.kt.
constexpr int32_t kCpdpValues = 5;             // EFFECT_PARAM_CPDP_VALUES (the SET key)
constexpr int32_t kSubEnable = 0;              // EFFECT_PARAM_ENABLE
constexpr int32_t kSubProfile = 0xA000000;     // EFFECT_PARAM_PROFILE
constexpr int32_t kSubSetProfileParam = 0x1000000;   // EFFECT_PARAM_SET_PROFILE_PARAMETER
constexpr int32_t kSubResetProfile = 0xC000000;      // EFFECT_PARAM_RESET_PROFILE_SETTINGS

// GET keys (app computes: EFFECT_PARAM_CPDP_VALUES + <sub>, or a packed profile-param key).
constexpr int32_t kGetEnableKey = kCpdpValues + kSubEnable;    // 5
constexpr int32_t kGetProfileKey = kCpdpValues + kSubProfile;  // 0xA000005
constexpr int32_t kGetProfileParam = 0x1000005;                // EFFECT_PARAM_GET_PROFILE_PARAMETER

inline int32_t rdI32(const uint8_t* p, size_t off) {
    int32_t v;
    std::memcpy(&v, p + off, sizeof(v));
    return v;
}
inline void wrI32(std::vector<uint8_t>& b, size_t off, int32_t v) {
    std::memcpy(b.data() + off, &v, sizeof(v));
}
inline uint64_t dapKey(int32_t profile, int32_t paramId) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(profile)) << 32) |
           static_cast<uint32_t>(paramId);
}

} // namespace

// Record what the app writes so getParameterSpecific() can hand it back. The engine
// itself has no readable state, and the app reads every toggle/EQ value back through
// the effect, so this shadow is what makes the UI reflect reality (and stops the
// read path from throwing).
void DapDirect::shadowSet(const uint8_t* bytes, size_t len) {
    if (bytes == nullptr || len < kValueOff + 4) return;
    if (rdI32(bytes, 4) != 4) return;                       // psize must be one int32 key
    const int32_t vsize = rdI32(bytes, 8);
    if (vsize < 4 || kValueOff + static_cast<size_t>(vsize) > len) return;
    if (rdI32(bytes, kParamHdr) != kCpdpValues) return;     // not our CPDP channel

    const size_t nInts = static_cast<size_t>(vsize) / 4;
    auto v = [&](size_t i) { return rdI32(bytes, kValueOff + i * 4); };
    if (nInts < 1) return;

    switch (v(0)) {
        case kSubEnable:  // [0, 1, value]
            if (nInts >= 3) mShadowEnabled = (v(2) != 0);
            break;
        case kSubProfile:  // [0xA000000, 1, value]
            if (nInts >= 3) mShadowProfile = v(2);
            break;
        case kSubSetProfileParam: {  // [0x1000000, count, profile, id, vals...]
            if (nInts < 5) break;
            std::vector<int32_t> vals;
            vals.reserve(nInts - 4);
            for (size_t i = 4; i < nInts; ++i) vals.push_back(v(i));
            mShadowDap[dapKey(v(2), v(3))] = std::move(vals);
            break;
        }
        case kSubResetProfile: {  // [0xC000000, 1, profile]
            if (nInts < 3) break;
            const int32_t profile = v(2);
            for (auto it = mShadowDap.begin(); it != mShadowDap.end();) {
                if (static_cast<int32_t>(it->first >> 32) == profile)
                    it = mShadowDap.erase(it);
                else
                    ++it;
            }
            break;
        }
        default:
            break;
    }
}

// The app drives every DAP feature through the AIDL vendor-extension passthrough:
// its legacy AudioEffect.setParameter() blob arrives here as a DefaultExtension
// (raw effect_param_t bytes) under Parameter::Specific::vendorEffect. Shadow it for
// read-back, then apply to the live engine. Anything else is accepted + ignored so
// the UI can't error.
ndk::ScopedAStatus DapDirect::setParameterSpecific(const Parameter::Specific& specific) {
    if (specific.getTag() != Parameter::Specific::vendorEffect) {
        return ndk::ScopedAStatus::ok();
    }
    const auto& vendor = specific.get<Parameter::Specific::vendorEffect>();
    std::optional<DefaultExtension> ext;
    if (vendor.extension.getParcelable(&ext) == STATUS_OK && ext.has_value()) {
        const auto* b = reinterpret_cast<const uint8_t*>(ext->bytes.data());
        const size_t n = ext->bytes.size();
        shadowSet(b, n);                                 // remember for read-back
        if (mContext) mContext->handleLegacyParam(b, n); // apply to the live engine
    }
    return ndk::ScopedAStatus::ok();
}

// Decode one legacy GET request key and return the shadowed value as a well-formed
// effect_param_t (value zero-padded to the size the app asked for).
bool DapDirect::buildGetResponse(const uint8_t* bytes, size_t len,
                                 std::vector<uint8_t>& out) const {
    if (bytes == nullptr || len < kParamHdr + 4) return false;
    if (rdI32(bytes, 4) != 4) return false;              // psize must be one int32 key
    const int32_t vsize = rdI32(bytes, 8);
    if (vsize < 4) return false;
    const int32_t key = rdI32(bytes, kParamHdr);

    std::vector<int32_t> vals;
    if (key == kGetEnableKey) {
        vals = {mShadowEnabled ? 1 : 0};
    } else if (key == kGetProfileKey) {
        vals = {mShadowProfile};
    } else if ((key & kGetProfileParam) == kGetProfileParam) {
        // packed: (id << 16) | (profile << 8) | EFFECT_PARAM_GET_PROFILE_PARAMETER
        const int32_t paramId = (key >> 16) & 0xFF;
        const int32_t profile = (key >> 8) & 0xFF;
        auto it = mShadowDap.find(dapKey(profile, paramId));
        if (it != mShadowDap.end()) vals = it->second;
        // never-written params fall through to a zero-filled reply (a sane default)
    }
    // Unknown keys also produce a zero-filled reply — a valid answer beats an error
    // the app would treat as fatal.

    const size_t total = kValueOff + static_cast<size_t>(vsize);
    out.assign(total, 0);
    wrI32(out, 0, 0);              // status = OK
    wrI32(out, 4, 4);             // psize
    wrI32(out, 8, vsize);        // vsize (echo the app's buffer size)
    wrI32(out, kParamHdr, key);  // key
    const size_t maxInts = static_cast<size_t>(vsize) / 4;
    for (size_t i = 0; i < vals.size() && i < maxInts; ++i) {
        wrI32(out, kValueOff + i * 4, vals[i]);
    }
    return true;
}

ndk::ScopedAStatus DapDirect::getParameterSpecific(const Parameter::Id& id,
                                                   Parameter::Specific* specific) {
    if (id.getTag() != Parameter::Id::vendorEffectTag) {
        return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                "unsupportedParamId");
    }
    const auto& ext = id.get<Parameter::Id::vendorEffectTag>();
    std::optional<DefaultExtension> req;
    if (ext.extension.getParcelable(&req) != STATUS_OK || !req.has_value()) {
        return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                "badVendorId");
    }
    std::vector<uint8_t> out;
    if (!buildGetResponse(reinterpret_cast<const uint8_t*>(req->bytes.data()),
                          req->bytes.size(), out)) {
        return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                "badGetParam");
    }
    DefaultExtension resp;
    resp.bytes = std::move(out);
    VendorExtension vext;
    vext.extension.setParcelable(resp);
    specific->set<Parameter::Specific::vendorEffect>(vext);
    return ndk::ScopedAStatus::ok();
}

IEffect::Status DapDirect::effectProcessImpl(float* in, float* out, int samples) {
    IEffect::Status s;
    s.status = STATUS_OK;
    s.fmqConsumed = samples;
    if (!mContext) {
        s.fmqProduced = samples;
        if (in != out) memcpy(out, in, samples * sizeof(float));
        return s;
    }
    s.fmqProduced = mContext->process(in, out, samples);
    return s;
}

} // namespace aidl::qti::effects
