// Standalone check: can a session-0 (OUTPUT_MIX) DolbyDapDirect effect be
// enabled via the framework? This mirrors exactly what the XiaomiDolby app does
// (AudioEffect on AUDIO_SESSION_OUTPUT_MIX + setEnabled(true)), so it verifies the
// enable path without needing to hot-swap the (un-overlayable) system app.
#include <media/AudioEffect.h>
#include <android/content/AttributionSourceState.h>
#include <system/audio_effect.h>
#include <utils/Errors.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <unistd.h>

using namespace android;
using android::content::AttributionSourceState;

int main() {
    // da9d0b17-dade-4daf-9b3c-d01b9a0da000  (type == uuid, typeSameAsUuid)
    effect_uuid_t type = {0xda9d0b17, 0xdade, 0x4daf, 0x9b3c,
                          {0xd0, 0x1b, 0x9a, 0x0d, 0xa0, 0x00}};

    AttributionSourceState attr;
    attr.uid = getuid();
    attr.pid = getpid();
    attr.packageName = "dap_enable_test";

    sp<AudioEffect> fx = new AudioEffect(attr);
    status_t st = fx->set(&type, nullptr, /*priority*/ 100,
                          (AudioEffect::legacy_callback_t) nullptr, /*user*/ nullptr,
                          AUDIO_SESSION_OUTPUT_MIX, AUDIO_IO_HANDLE_NONE);
    printf("set() -> %d\n", st);
    status_t ic = fx->initCheck();
    printf("initCheck() -> %d (0=OK, %d=ALREADY_EXISTS)\n", ic, (int)ALREADY_EXISTS);
    if (ic != NO_ERROR && ic != ALREADY_EXISTS) {
        printf("FAILED to create effect; cannot test enable\n");
        return 1;
    }
    status_t en = fx->setEnabled(true);
    printf("setEnabled(true) -> %d\n", en);
    printf("getEnabled() -> %d\n", fx->getEnabled());

    // Push a strong custom graphic-EQ preset exactly like the app does, to
    // exercise dap_cpdp_graphic_equalizer_bands_set live (verify no HAL crash).
    // value buf = [SET_PROFILE_PARAMETER=0x1000000][count=21][profile=0][id=110][20 gains]
    {
        const int32_t gains[20] = {100, 90, 80, 60, 40, 20, 0, 0, 0, 0,
                                   0, 0, 0, -20, -40, -60, -80, -90, -100, -100};
        const int nInts = 4 + 20;                 // header(4) + 20 gains
        const int vsize = nInts * 4;
        std::vector<uint8_t> raw(sizeof(effect_param_t) + 4 /*p*/ + vsize);
        effect_param_t* p = reinterpret_cast<effect_param_t*>(raw.data());
        p->psize = 4;
        p->vsize = vsize;
        int32_t* pk = reinterpret_cast<int32_t*>(p->data);
        pk[0] = 5;                                // EFFECT_PARAM_CPDP_VALUES
        int32_t* v = pk + 1;
        v[0] = 0x1000000;                         // SET_PROFILE_PARAMETER
        v[1] = 21;                                // count (length+1)
        v[2] = 0;                                 // profile
        v[3] = 110;                               // GEQ_BAND_GAINS
        for (int i = 0; i < 20; ++i) v[4 + i] = gains[i];
        status_t sp = fx->setParameter(p);
        printf("setParameter(GEQ preset) -> %d\n", sp);
    }

    printf("holding effect enabled for 20s — check dumpsys media.audio_flinger now\n");
    fflush(stdout);
    sleep(20);
    printf("done\n");
    return 0;
}
