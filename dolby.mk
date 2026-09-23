# Dolby Atmos (DAX3 3.12) — DAP-first AIDL stack, ported from Xiaomi 15 (dada, HyperOS A16)
# SPDX-License-Identifier: Apache-2.0

DOLBY_PATH := hardware/dolby

PRODUCT_SOONG_NAMESPACES += $(DOLBY_PATH)

# Configs
# media_codecs_c2.xml is the Codec2InfoBuilder hook file that makes the Dolby
# decoders visible to MediaCodecList (see file header comment).
PRODUCT_COPY_FILES += \
    $(DOLBY_PATH)/configs/dax-default.xml:$(TARGET_COPY_OUT_VENDOR)/etc/dolby/dax-default.xml \
    $(DOLBY_PATH)/configs/dax-default-spatializer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/dolby/dax-default-spatializer.xml \
    $(DOLBY_PATH)/configs/media_codecs_dolby_audio.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_dolby_audio.xml \
    $(DOLBY_PATH)/configs/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml

# Dolby audio decoders (AC-4 -> Tidal Atmos, E-AC-3/JOC -> video Atmos):
# Codec2 software components + their HAL service. The components only appear in
# MediaCodecList if an XML entry exists in the Codec2InfoBuilder parse chain;
# media_codecs_c2.xml (copied above) provides it by including
# media_codecs_dolby_audio.xml.
PRODUCT_PACKAGES += \
    vendor.dolby.media.c2-default-service-dax \
    libcodec2_store_dolby \
    libcodec2_soft_ac4dec \
    libcodec2_soft_ddpdec \
    libdeccfg

# C2 HAL wrapper libs the Dolby c2 service links but that OnePlus does NOT ship in
# /vendor (they normally live only inside the com.android.media.swcodec APEX, which a
# vendor binary can't reach). Built here as vendor variants from source (all are
# vendor_available) so they land in /vendor/lib64 and match our tree's libcodec2_vndk.
PRODUCT_PACKAGES += \
    libcodec2_aidl.vendor \
    libcodec2_hidl@1.0.vendor \
    libcodec2_hidl@1.1.vendor \
    libcodec2_hidl@1.2.vendor \
    android.hardware.media.c2@1.0.vendor \
    android.hardware.media.c2@1.1.vendor \
    android.hardware.media.c2@1.2.vendor \
    libcodec2_soft_common.vendor

# libcodec2_soft_common is dlopen'd transitively by the Dolby decoder components
# (libcodec2_soft_ac4dec / libcodec2_soft_ddpdec); it also lives only in the swcodec
# APEX, so ship the vendor variant too or the store aborts on listComponents().

# Dolby DAP soundfx effects
PRODUCT_PACKAGES += \
    libhwdapaidl \
    libdlbvolaidl \
    libswgamedapaidl \
    libdapdirect

# Dolby support libs
PRODUCT_PACKAGES += \
    libdapparamstorage \
    libdlbpreg \
    libdmshal \
    libdlbdsservice \
    vendor.dolby.dms-V1-ndk \
    vendor.dolby.hardware.dms@2.0

# Interface libs the blobs were built against.
# types-V6: standalone prebuilt (absent from our tree; no source competitor).
# libdlbaudioeffectv2 / libdlbaudiocommonv3: donor effect-V2 / common-V3 renamed to
# private sonames (patchelf) so our soundfx libs load the working donor copies instead
# of the tree's frozen source vendor variant — no interface-soname install collision.
PRODUCT_PACKAGES += \
    android.media.audio.common.types-V6-ndk \
    libdlbaudioeffectv2 \
    libdlbaudiocommonv3

# Dolby Management Service (AIDL HAL) — init_rc + vintf via Android.bp
PRODUCT_PACKAGES += \
    vendor.dolby.dms.service

# Frontend app + framework overlay
PRODUCT_PACKAGES += \
    XiaomiDolby \
    DolbyFrameworksResCommon

# DAX props
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.audio.dolby.dax.support=true \
    ro.vendor.audio.dolby.dax.version=DAX3_3.12.0.8_r1 \
    ro.vendor.dolby.dax.version=DAX3_3.12.0.8_r1 \
    ro.vendor.audio.dolby.dax.game.enable=true \
    ro.vendor.audio.dolby.surround.enable=true \
    vendor.audio.dolby.control.support=true
