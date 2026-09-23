# AC-4 spectral bounds correction

`0001-validate-spectral-window-layout.patch` targets the patched FFmpeg n6.1
AC-4 source used by this component. Apply it after the existing AC-4 and
immersive-stereo patches, before generating the Android static libraries.

The band-offset table includes its terminal entry. A `max_sfb` equal to the
table length is invalid because the parser subsequently reads
`sfb_offset[max_sfb]`. The patch also rejects window layouts that exceed the
2048-element spectral buffers or the window-offset array before filling them.

The phone's decoder (build ID `7a0ad3ce9323dee24318f0650dfecc2c`) crashed on
2026-09-05 at 18:05:50 and 18:47:31 BST, with an invalid write in
`ac4_decode_frame`. Disassembly is consistent with the spectral-reordering
copy loop. The bounds bugs are confirmed by source inspection; the exact
triggering packet and playback recovery have not been verified.

## Fresh Tidal reproduction

At 19:10:48 BST the same installed decoder crashed again. Before the crash it
logged channel mode 6, entered `RB1 7x`, and repeatedly rejected codebooks
(`sect_cb > 11`, `AVERROR_INVALIDDATA`). Tidal then logged that its AC-4
Codec2 component died.

The logged first frame starts `BF CD FB 9C 20 09 6B F9`. Parsing its TOC
prefix gives bitstream version 2, one presentation, a single substream group,
and presentation version 2. This is the case covered by the existing
immersive-stereo channel-mode rewrite.

Disassembly confirms the installed library lacks that rewrite. At `0x31968`
it checks channel mode 16 and otherwise jumps to `0x319ec`, which handles
modes 11–14 without checking for immersive stereo. The existing local output
at `out/target/product/macanc/vendor/lib64/libcodec2_soft_ac4dec.so`, build ID
`e16438b09fb1627c60149194a890a689`, instead checks mode 6 at `0x319d0`, then
presentation version 2 at `0x319f0`, and branches to the stereo rewrite.
The current `lib/arm64/libavcodec.a` also contains this correction.

Thus the immediate playback failure is consistent with an older installed
decoder missing a correction already present in the current build inputs.
The existing local output predates the bounds/packet changes documented here.
Neither it nor any new artifact has been installed during this investigation;
successful playback remains unverified on the phone.

The patch was dry-run checked against
`/tmp/repos/ac4dec-open/ffmpeg/FFmpeg-n6.1/libavcodec/ac4dec.c` and applied to
that working source. No compilation or deployment was performed for this fix.
The `lib/arm64/libavcodec.a` archive is unchanged. Soong consumes that archive,
not this patch: a later authorized FFmpeg rebuild and archive update are
required for this correction to be included in a ROM build.

The accompanying `C2SoftAc4Dec.cpp` changes give input packets FFmpeg-owned,
zero-padded storage and validate decoded frame dimensions/format before PCM
conversion. They do not replace the bounds checks inside the decoder.
