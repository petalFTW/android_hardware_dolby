/*
 * Open AC-4 decoder Codec2 soft component (RaB1D).
 *
 * Drop-in replacement for the prebuilt Dolby libcodec2_soft_ac4dec.so.
 * The Dolby blob decodes AC-4 fine but renders 100% digital silence on this
 * OnePlus SM8850 platform (the Dolby ADSP endpoint topology was stripped from
 * firmware — proven byte-identical vs a working Xiaomi SM8850 donor). This
 * component decodes AC-4 (incl. Tidal Atmos immersive-stereo) entirely in
 * software via libavcodec's ac4 decoder, bypassing the missing DSP.
 *
 * Keeps the component name "c2.dolby.ac4.decoder" and the .so name so it slots
 * into the existing Dolby c2 store + media_codecs XML with zero other changes.
 */

#ifndef ANDROID_C2_SOFT_AC4_DEC_H_
#define ANDROID_C2_SOFT_AC4_DEC_H_

#include <SimpleC2Component.h>
#include <util/C2InterfaceHelper.h>

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace android {

struct C2SoftAc4Dec : public SimpleC2Component {
    class IntfImpl;

    C2SoftAc4Dec(const char *name, c2_node_id_t id,
                 const std::shared_ptr<IntfImpl> &intfImpl);
    C2SoftAc4Dec(const char *name, c2_node_id_t id,
                 const std::shared_ptr<C2ReflectorHelper> &helper);
    virtual ~C2SoftAc4Dec();

    // From SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool) override;

private:
    std::shared_ptr<IntfImpl> mIntf;

    const AVCodec *mCodec;
    AVCodecContext *mCtx;
    AVFrame *mFrame;
    AVPacket *mPkt;

    bool mSignalledError;
    bool mSignalledOutputEos;
    size_t mInputBufferCount;
    int32_t mSampleRate;
    int32_t mChannelCount;

    status_t initDecoder();
    void closeDecoder();
    // Update output sample-rate / channel-count config from a decoded frame.
    bool updateConfigFromFrame(const std::unique_ptr<C2Work> &work);

    C2_DO_NOT_COPY(C2SoftAc4Dec);
};

}  // namespace android

#endif  // ANDROID_C2_SOFT_AC4_DEC_H_
