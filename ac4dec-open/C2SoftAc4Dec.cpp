/*
 * Open AC-4 decoder Codec2 soft component (RaB1D) — libavcodec backend.
 * See C2SoftAc4Dec.h for why this replaces the silent Dolby blob.
 */

#define LOG_NDEBUG 0
#define LOG_TAG "C2SoftAc4Dec"
#include <climits>
#include <cmath>
#include <log/log.h>

#include <media/stagefright/foundation/MediaDefs.h>

#include <C2PlatformSupport.h>
#include <SimpleC2Interface.h>

#include "C2SoftAc4Dec.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/log.h"
#include "libavutil/samplefmt.h"
#include <android/log.h>
}

namespace android {

namespace {

constexpr char COMPONENT_NAME[] = "c2.dolby.ac4.decoder";
constexpr int32_t kDefaultSampleRate = 48000;
constexpr int32_t kDefaultChannelCount = 2;
constexpr int32_t kMaxChannelCount = 8;
// AC-4 frame_len_base is at most 2048 samples/channel @48k.
constexpr size_t kMaxSamplesPerChannel = 2048;

// RaB1D: bridge libavcodec's internal av_log() (parse errors, bitstream
// diagnostics) into logcat under tag "C2SoftAc4Dec.ff". Without this the
// decoder's AV_LOG_ERROR lines ("sect_cb > 11", "unexpected sequence
// counter", ...) are emitted to stderr, which no vendor service can show —
// making AC-4 rejections appear as bare AVERROR_INVALIDDATA return codes.
void ac4AvLogCallback(void *avcl, int level, const char *fmt, va_list vl) {
    static constexpr android_LogPriority kPrioByLevel[] = {
            ANDROID_LOG_FATAL,   // AV_LOG_PANIC    0
            ANDROID_LOG_FATAL,   // AV_LOG_FATAL    8
            ANDROID_LOG_ERROR,   // AV_LOG_ERROR    16
            ANDROID_LOG_WARN,    // AV_LOG_WARNING  24
            ANDROID_LOG_INFO,    // AV_LOG_INFO     32
            ANDROID_LOG_DEBUG,   // AV_LOG_VERBOSE  40
            ANDROID_LOG_DEBUG,   // AV_LOG_DEBUG    48
            ANDROID_LOG_DEBUG,   // AV_LOG_TRACE    56
    };
    if (level < 0 || size_t(level / 8) >= std::size(kPrioByLevel)) return;
    if (av_log_get_level() < level) return;

    char line[1024];
    int printPrefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &printPrefix);
    __android_log_print(kPrioByLevel[level / 8], "C2SoftAc4Dec.ff", "%s", line);
}

}  // namespace

class C2SoftAc4Dec::IntfImpl : public SimpleInterface<void>::BaseParams {
public:
    explicit IntfImpl(const std::shared_ptr<C2ReflectorHelper> &helper)
        : SimpleInterface<void>::BaseParams(
                helper,
                COMPONENT_NAME,
                C2Component::KIND_DECODER,
                C2Component::DOMAIN_AUDIO,
                MEDIA_MIMETYPE_AUDIO_AC4) {
        noPrivateBuffers();
        noInputReferences();
        noOutputReferences();
        noInputLatency();
        noTimeStretch();
        setDerivedInstance(this);

        addParameter(
                DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                .withConstValue(new C2ComponentAttributesSetting(
                    C2Component::ATTRIB_IS_TEMPORAL))
                .build());

        addParameter(
                DefineParam(mSampleRate, C2_PARAMKEY_SAMPLE_RATE)
                .withDefault(new C2StreamSampleRateInfo::output(0u, kDefaultSampleRate))
                .withFields({C2F(mSampleRate, value).oneOf({44100, 48000})})
                .withSetter((Setter<decltype(*mSampleRate)>::StrictValueWithNoDeps))
                .build());

        addParameter(
                DefineParam(mChannelCount, C2_PARAMKEY_CHANNEL_COUNT)
                .withDefault(new C2StreamChannelCountInfo::output(0u, kDefaultChannelCount))
                .withFields({C2F(mChannelCount, value).inRange(1, kMaxChannelCount)})
                .withSetter(Setter<decltype(*mChannelCount)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
                .withDefault(new C2StreamBitrateInfo::input(0u, 256000))
                .withFields({C2F(mBitrate, value).inRange(1, 10000000)})
                .withSetter(Setter<decltype(*mBitrate)>::NonStrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mInputMaxBufSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                .withConstValue(new C2StreamMaxBufferSizeInfo::input(0u, 65536))
                .build());

        addParameter(
                DefineParam(mPcmEncodingInfo, C2_PARAMKEY_PCM_ENCODING)
                .withDefault(new C2StreamPcmEncodingInfo::output(0u, C2Config::PCM_16))
                .withFields({C2F(mPcmEncodingInfo, value).oneOf({
                     C2Config::PCM_16,
                     C2Config::PCM_FLOAT})
                })
                .withSetter((Setter<decltype(*mPcmEncodingInfo)>::StrictValueWithNoDeps))
                .build());
    }

    int32_t getPcmEncodingInfo() const { return mPcmEncodingInfo->value; }

private:
    std::shared_ptr<C2StreamSampleRateInfo::output> mSampleRate;
    std::shared_ptr<C2StreamChannelCountInfo::output> mChannelCount;
    std::shared_ptr<C2StreamBitrateInfo::input> mBitrate;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mInputMaxBufSize;
    std::shared_ptr<C2StreamPcmEncodingInfo::output> mPcmEncodingInfo;
};

C2SoftAc4Dec::C2SoftAc4Dec(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : SimpleC2Component(std::make_shared<SimpleInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
      mCodec(nullptr),
      mCtx(nullptr),
      mFrame(nullptr),
      mPkt(nullptr) {
}

C2SoftAc4Dec::C2SoftAc4Dec(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<C2ReflectorHelper> &helper)
    : C2SoftAc4Dec(name, id, std::make_shared<IntfImpl>(helper)) {
}

C2SoftAc4Dec::~C2SoftAc4Dec() {
    onRelease();
}

status_t C2SoftAc4Dec::initDecoder() {
    closeDecoder();

    mCodec = avcodec_find_decoder(AV_CODEC_ID_AC4);
    if (!mCodec) {
        ALOGE("initDecoder: AC-4 decoder not found in libavcodec");
        mSignalledError = true;
        return NO_INIT;
    }
    mCtx = avcodec_alloc_context3(mCodec);
    if (!mCtx) {
        ALOGE("initDecoder: avcodec_alloc_context3 failed");
        mSignalledError = true;
        return NO_MEMORY;
    }
    mCtx->sample_rate = kDefaultSampleRate;
    // request planar float output (the AC-4 decoder's native format)
    mCtx->request_sample_fmt = AV_SAMPLE_FMT_FLTP;

    int ret = avcodec_open2(mCtx, mCodec, nullptr);
    if (ret < 0) {
        ALOGE("initDecoder: avcodec_open2 failed (%d)", ret);
        mSignalledError = true;
        return NO_INIT;
    }
    mFrame = av_frame_alloc();
    mPkt = av_packet_alloc();
    if (!mFrame || !mPkt) {
        ALOGE("initDecoder: frame/packet alloc failed");
        mSignalledError = true;
        return NO_MEMORY;
    }

    mSignalledError = false;
    mSignalledOutputEos = false;
    mInputBufferCount = 0;
    mSampleRate = kDefaultSampleRate;
    mChannelCount = kDefaultChannelCount;
    // RaB1D: route libavcodec diagnostics to logcat (see ac4AvLogCallback).
    av_log_set_level(AV_LOG_DEBUG);
    av_log_set_callback(ac4AvLogCallback);
    ALOGD("initDecoder: libavcodec AC-4 decoder ready (av_log level=%d)",
          av_log_get_level());
    return OK;
}

void C2SoftAc4Dec::closeDecoder() {
    if (mPkt) { av_packet_free(&mPkt); mPkt = nullptr; }
    if (mFrame) { av_frame_free(&mFrame); mFrame = nullptr; }
    if (mCtx) { avcodec_free_context(&mCtx); mCtx = nullptr; }
    mCodec = nullptr;
}

c2_status_t C2SoftAc4Dec::onInit() {
    status_t err = initDecoder();
    return err == OK ? C2_OK : C2_NO_MEMORY;
}

c2_status_t C2SoftAc4Dec::onStop() {
    if (mCtx) avcodec_flush_buffers(mCtx);
    mSignalledError = false;
    mSignalledOutputEos = false;
    return C2_OK;
}

void C2SoftAc4Dec::onReset() {
    mInputBufferCount = 0;
    (void)onStop();
}

void C2SoftAc4Dec::onRelease() {
    mInputBufferCount = 0;
    closeDecoder();
}

c2_status_t C2SoftAc4Dec::onFlush_sm() {
    return onStop();
}

static void fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    work->worklets.front()->output.flags = work->input.flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

bool C2SoftAc4Dec::updateConfigFromFrame(const std::unique_ptr<C2Work> &work) {
    int32_t sr = mFrame->sample_rate;
    int32_t ch = mFrame->ch_layout.nb_channels;
    if (sr <= 0) sr = mSampleRate;
    if (ch <= 0) ch = mChannelCount;
    if (sr == mSampleRate && ch == mChannelCount && mInputBufferCount > 1) {
        return true;
    }
    mSampleRate = sr;
    mChannelCount = ch;

    C2StreamSampleRateInfo::output sampleRateInfo(0u, sr);
    C2StreamChannelCountInfo::output channelCountInfo(0u, ch);
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    c2_status_t err = mIntf->config(
            {&sampleRateInfo, &channelCountInfo}, C2_MAY_BLOCK, &failures);
    if (err != OK) {
        ALOGE("updateConfigFromFrame: config update failed (%d)", err);
        return false;
    }
    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(sampleRateInfo));
    work->worklets.front()->output.configUpdate.push_back(C2Param::Copy(channelCountInfo));
    ALOGD("config: %d Hz, %d channels", sr, ch);
    return true;
}

// AC-4 may arrive raw (mp4 sample) or wrapped in a sync frame
// (0xAC40 no-CRC / 0xAC41 with-CRC). libavcodec's ac4 decoder wants the raw
// ac4_frame, so strip a sync-frame header if present.  *outLen receives the
// raw-frame length; returns pointer to the raw frame within [in, in+inLen).
static const uint8_t *stripSyncFrame(const uint8_t *in, size_t inLen, size_t *outLen) {
    if (inLen >= 4 && in[0] == 0xAC && (in[1] == 0x40 || in[1] == 0x41)) {
        size_t hdr = 4;
        size_t sz = ((size_t)in[2] << 8) | in[3];
        if (sz == 0xFFFF && inLen >= 7) {
            sz = ((size_t)in[4] << 16) | ((size_t)in[5] << 8) | in[6];
            hdr = 7;
        }
        if (hdr + sz <= inLen) {
            *outLen = sz;
            return in + hdr;
        }
    }
    *outLen = inLen;
    return in;
}

void C2SoftAc4Dec::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    work->result = C2_OK;
    work->workletsProcessed = 1u;
    work->worklets.front()->output.configUpdate.clear();
    work->worklets.front()->output.flags = work->input.flags;

    if (mSignalledError || mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }

    C2ReadView rView = mDummyReadView;
    size_t inSize = 0u;
    if (!work->input.buffers.empty()) {
        if (!work->input.buffers[0] ||
            work->input.buffers[0]->data().linearBlocks().empty()) {
            work->result = C2_BAD_VALUE;
            return;
        }
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        if (inSize && rView.error()) {
            ALOGE("read view map failed %d", rView.error());
            work->result = C2_CORRUPTED;
            return;
        }
    }
    bool eos = (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0;
    bool codecConfig = (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) != 0;

    ALOGV("in buffer attr. size %zu ts %d idx %d", inSize,
          (int)work->input.ordinal.timestamp.peeku(),
          (int)work->input.ordinal.frameIndex.peeku());

    // AC-4 frames are self-describing (sample rate / channel mode live in the
    // frame TOC); the dac4 CSD carries no data libavcodec needs. Absorb CSD.
    if (codecConfig || inSize == 0) {
        fillEmptyWork(work);
        if (eos) {
            mSignalledOutputEos = true;
            ALOGV("signalled EOS");
        }
        if (inSize == 0 && !codecConfig) {
            // flush any pending output on an empty non-CSD buffer
        }
        return;
    }

    const uint8_t *rawIn = rView.data();
    size_t rawLen = 0;
    const uint8_t *frame = stripSyncFrame(rawIn, inSize, &rawLen);
    if (frame == nullptr || rawLen == 0 || frame < rawIn || rawLen > inSize) {
        mInputBufferCount++;
        fillEmptyWork(work);
        return;
    }

    // Codec2 mappings do not promise FFmpeg's zero-filled input padding. Own the
    // packet storage so bit-reader lookahead and retained packet references are safe.
    if (rawLen > INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
        work->result = C2_BAD_VALUE;
        return;
    }
    av_packet_unref(mPkt);
    if (av_new_packet(mPkt, static_cast<int>(rawLen)) < 0) {
        work->result = C2_NO_MEMORY;
        return;
    }
    memcpy(mPkt->data, frame, rawLen);

    int ret = avcodec_send_packet(mCtx, mPkt);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        if (mInputBufferCount < 5) {
            char hex[80] = {0};
            for (int i = 0; i < 16 && i < (int)rawLen; i++) {
                snprintf(hex + i * 5, sizeof(hex) - i * 5, "0x%02X ", frame[i]);
            }
            ALOGW("avcodec_send_packet err %d (size %zu) head: %s", ret, rawLen, hex);
        } else {
            ALOGW("avcodec_send_packet err %d (size %zu)", ret, rawLen);
        }
        // A single bad frame shouldn't kill the stream; emit empty and continue.
        mInputBufferCount++;
        fillEmptyWork(work);
        return;
    }

    ret = avcodec_receive_frame(mCtx, mFrame);
    if (ret == AVERROR(EAGAIN)) {
        mInputBufferCount++;
        fillEmptyWork(work);
        return;
    }
    if (ret < 0) {
        ALOGW("avcodec_receive_frame err %d — skip frame", ret);
        mInputBufferCount++;
        fillEmptyWork(work);
        return;
    }

    const int channels = mFrame->ch_layout.nb_channels;
    const int nbSamples = mFrame->nb_samples;
    if (channels <= 0 || channels > kMaxChannelCount || nbSamples <= 0 ||
        static_cast<size_t>(nbSamples) > kMaxSamplesPerChannel ||
        mFrame->format != AV_SAMPLE_FMT_FLTP || !mFrame->extended_data) {
        ALOGE("invalid decoded frame: format=%d channels=%d samples=%d",
              mFrame->format, channels, nbSamples);
        av_frame_unref(mFrame);
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        return;
    }
    for (int c = 0; c < channels; c++) {
        if (!mFrame->extended_data[c]) {
            av_frame_unref(mFrame);
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            return;
        }
    }

    mInputBufferCount++;
    if (!updateConfigFromFrame(work)) {
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        av_frame_unref(mFrame);
        return;
    }

    const bool outputFloat = mIntf->getPcmEncodingInfo() == C2Config::PCM_FLOAT;
    const size_t sampleSize = outputFloat ? sizeof(float) : sizeof(int16_t);
    const size_t outSize = (size_t)channels * nbSamples * sampleSize;

    std::shared_ptr<C2LinearBlock> block;
    C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
    c2_status_t err = pool->fetchLinearBlock(outSize, usage, &block);
    if (err != C2_OK) {
        ALOGE("fetchLinearBlock failed %d", err);
        work->result = C2_NO_MEMORY;
        av_frame_unref(mFrame);
        return;
    }
    C2WriteView wView = block->map().get();
    if (wView.error()) {
        ALOGE("write view map failed %d", wView.error());
        work->result = C2_CORRUPTED;
        av_frame_unref(mFrame);
        return;
    }

    // libavcodec ac4 output is AV_SAMPLE_FMT_FLTP (planar float, [-1,1]).
    // Interleave into the C2 linear block as PCM_16 or PCM_FLOAT.
    const uint8_t *const *planes = mFrame->extended_data;
    if (outputFloat) {
        float *out = reinterpret_cast<float *>(wView.data());
        for (int s = 0; s < nbSamples; s++) {
            for (int c = 0; c < channels; c++) {
                out[s * channels + c] =
                    reinterpret_cast<const float *>(planes[c])[s];
            }
        }
    } else {
        int16_t *out = reinterpret_cast<int16_t *>(wView.data());
        for (int c = 0; c < channels; c++) {
            const float *pc = reinterpret_cast<const float *>(planes[c]);
            for (int s = 0; s < nbSamples; s++) {
                float v = pc[s] * 32768.0f;
                if (v > 32767.0f) v = 32767.0f;
                else if (v < -32768.0f) v = -32768.0f;
                out[s * channels + c] = (int16_t)lrintf(v);
            }
        }
    }
    av_frame_unref(mFrame);

    ALOGV("out buffer size %zu (%d ch x %d samples)", outSize, channels, nbSamples);
    work->worklets.front()->output.flags = work->input.flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.buffers.push_back(createLinearBuffer(block, 0, outSize));
    work->worklets.front()->output.ordinal = work->input.ordinal;
    if (eos) {
        mSignalledOutputEos = true;
        ALOGV("signalled EOS");
    }
}

c2_status_t C2SoftAc4Dec::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    (void)pool;
    if (drainMode == NO_DRAIN) {
        ALOGW("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        ALOGW("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }
    if (mCtx) avcodec_flush_buffers(mCtx);
    return C2_OK;
}

class C2SoftAc4DecFactory : public C2ComponentFactory {
public:
    C2SoftAc4DecFactory() : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
            GetCodec2PlatformComponentStore()->getParamReflector())) {
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        *component = std::shared_ptr<C2Component>(
                new C2SoftAc4Dec(COMPONENT_NAME, id,
                                 std::make_shared<C2SoftAc4Dec::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        *interface = std::shared_ptr<C2ComponentInterface>(
                new SimpleInterface<C2SoftAc4Dec::IntfImpl>(
                        COMPONENT_NAME, id, std::make_shared<C2SoftAc4Dec::IntfImpl>(mHelper)),
                deleter);
        return C2_OK;
    }

    virtual ~C2SoftAc4DecFactory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
};

}  // namespace android

__attribute__((cfi_canonical_jump_table))
extern "C" ::C2ComponentFactory* CreateCodec2Factory() {
    ALOGV("in %s", __func__);
    return new ::android::C2SoftAc4DecFactory();
}

__attribute__((cfi_canonical_jump_table))
extern "C" void DestroyCodec2Factory(::C2ComponentFactory* factory) {
    ALOGV("in %s", __func__);
    delete factory;
}
