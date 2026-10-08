# NSDVR experimental protocol extension (v1)

**English** | [简体中文](nsdvr-ext-protocol.zh-CN.md)

A **backward-compatible** extension of SysDVR protocol 03 (TCP Bridge), shared by the experimental SysDVR build (the
`nsdvr` branch of [NSDVR-server](https://github.com/onewilk/NSDVR-server)) and the NSDVR client.
Goals:

1. Audio compression: PCM24, IMA ADPCM and Opus. The client can **switch** the codec and Opus parameters **at any
   time** without reconnecting.
2. Server diagnostics: send blocking, grc gaps, encode time and CPU reported every second, to find out why frames are
   lost.
3. IP_TOS (WMM video priority) marking.

Scope: **TCP Bridge only**. USB and RTSP never send the extension flag and behave as before. The screen-off logic
(upstream issue #402) is not changed by this extension and is left to upstream.

All multi-byte integers are **little endian**.

## 1. Handshake request (client → server, 16 bytes, sent once on each channel)

Extends the official `ProtoHandshakeRequest`:

- `FeatureFlags` bit 2 (`0x04`) = `ExtraFeatureFlags_NsdvrExt`: when set, `Reserved[0..5]` below are interpreted as in
  this table; when clear, the server behaves exactly like the official one.
- Official SysDVR ignores unknown FeatureFlags bits and the Reserved bytes (confirmed in upstream
  `ProtoHandshakeVersion`, which does not check them).

| Byte | Used on | Meaning |
|---|---|---|
| `Reserved[0]` | audio | Initial audio codec: 0 = PCM48 (unchanged), 1 = PCM24, 2 = ADPCM, 3 = OPUS; any other value is treated as 0 |
| `Reserved[1]` | audio | Opus bitrate in units of 2 kbps (32 = 64 kbps, 48 = 96 kbps); 0 means the default of 96 kbps; the server clamps to 16–256 kbps |
| `Reserved[2]` | audio | Low 4 bits: Opus complexity 0–10 (> 10 is treated as 10); high 4 bits: frame size, 0 = 20 ms, 1 = 10 ms (anything else is 20 ms) |
| `Reserved[3]` | both | bit0 = send diagnostics packets on this connection (video channel only); bit1 = set IP_TOS on this connection's socket |
| `Reserved[4..5]` | — | Reserved, must be 0 |

The handshake response is unchanged (72 bytes).

## 2. Detecting server support

- When the client requested the extension, an extended server writes `0xE0 | codec` into `PacketHeader.ReplaySlot` of
  **every audio packet** (`0xE0` = PCM48, `0xE1` = PCM24, `0xE2` = ADPCM, `0xE3` = OPUS). The official server always
  writes `0xFF` for audio.
- When diagnostics were requested on the video channel, an extended server sends a diagnostics packet every second
  (section 5).
- The client treats the server as supporting the extension, and shows the experimental options, only after it sees a
  `0xE0–0xE3` marker or a diagnostics packet; otherwise it handles audio as official PCM.

## 3. Audio packets (server → client, 9922)

`PacketHeader` fields keep their meaning: `Timestamp` is the time of the packet's first sample (microseconds),
`MetaData` is the audio type, and `ReplaySlot` is the codec marker above.

- **PCM48** (`0xE0`): same payload as official, s16le interleaved stereo at 48 kHz.
- **Compressed codecs** (`0xE1–0xE3`): payload = 12-byte `ExtAudioHeader` + body.

`ExtAudioHeader`:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | version = 1 |
| 1 | u8 | codec (1/2/3) |
| 2 | u8 | Low 4 bits Opus complexity, high 4 bits frame size code (0 for non-Opus codecs) |
| 3 | u8 | frameCount (number of Opus frames; 1 for other codecs) |
| 4 | u16 | bitrateKbps (configured value: PCM24 = 768, ADPCM = 384, Opus = the configured bitrate) |
| 6 | u16 | samplesPerChannel: samples per channel in this packet (at the codec's sample rate: 24 kHz for PCM24, 48 kHz otherwise) |
| 8 | u32 | encodeUs: time the server spent encoding this packet (microseconds, saturates at 0xFFFFFFFF) |

Body:

- **PCM24**: s16le interleaved stereo at 24 kHz, `samplesPerChannel` frames. The server downsamples with a 63-tap
  half-band low-pass filter (cutoff fs/4, filter state kept across packets); the reference is `HALFBAND_63` in
  `tools/audio_codec_eval.py`. The client upsamples back to 48 kHz (127 taps, cutoff about 11.5 kHz).
- **ADPCM**: standard IMA ADPCM (step and index tables as in `tools/audio_codec_eval.py`). First come 2 × 4 bytes of
  channel state (L then R; each: i16 predictor, u8 stepIndex, u8 reserved), which is the encoder state **before the
  packet's first sample** so that the decoder can resynchronize after a lost packet. Then `samplesPerChannel` bytes,
  one byte per sample instant: low 4 bits L, high 4 bits R.
- **OPUS**: `frameCount` × `(u16 len, len bytes of Opus packet)`. The frame length follows the frame size code
  (20 ms = 960 samples per channel, 10 ms = 480). The server re-chunks grc's 1024-sample captures into Opus frames
  with a small buffer; the header timestamp is the time of the first sample of the first frame. Stereo, 48 kHz,
  constant bitrate.

Addenda (confirmed after the server was implemented, 2026-09-30):

- **Error packets** for failed grc captures **keep the official format**: no ExtAudioHeader, `ReplaySlot = 0xFF`. A
  client must not conclude that the server lacks the extension because of one `0xFF` error packet.
- Opus uses CELT only (`OPUS_APPLICATION_RESTRICTED_CELT`) at a constant bitrate: every frame is exactly
  `kbps × frame samples / 384` bytes; the encoder delay is 120 samples (2.5 ms).
- When the codec changes, the server drops the incomplete Opus frame tail (< 20 ms). Changing only the bitrate,
  complexity or frame size keeps the tail and does not recreate the encoder.
- The timestamp of a PCM24 packet is the time of the first input sample of that capture batch; the ~0.65 ms group
  delay of the downsampling filter is not compensated.

## 4. Control messages (client → server, upstream on 9922, 8 bytes)

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | magic = `0x58564453` (byte sequence `S D V X`) |
| 4 | u8 | codec (0–3) |
| 5 | u8 | Opus bitrate in units of 2 kbps |
| 6 | u8 | Low 4 bits complexity, high 4 bits frame size code |
| 7 | u8 | reserved = 0 |

- The server's audio thread reads once per round without blocking; on a wrong magic it drops the bytes read so far
  and waits for the next aligned magic.
- Every value is validated and clamped first; invalid combinations are ignored. **Malformed input must never crash
  the sysmodule.**
- A message takes effect at the next packet boundary; switching codecs resets the encoder state. There is no reply:
  the client confirms the change from the packet's codec marker and `ExtAudioHeader`.

## 5. Diagnostics packets (server → client, 9911, about once per second)

`PacketHeader.MetaData = 0x07` (both type bits set = 3, plus the Data bit), `ReplaySlot = 0xFF`, and `Timestamp` is
the timestamp of the latest video frame. Client dispatchers must **check `(MetaData & 3) == 3` first**, otherwise the
packet is taken for video or audio. Type 3 is unused by the official sysmodule; the official C# client would treat it
as audio, so the server only sends it when the client asks for diagnostics.

Payload `ExtDiag v1`:

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | version = 1 |
| 1 | u8[3] | reserved |
| 4 | u32 | intervalMs: actual length of this statistics window |
| 8 | u32 | videoFramesSent |
| 12 | u32 | videoGrcGaps: gaps the server found in grc timestamps (interval > 1.5 × the normal frame interval) |
| 16 | u32 | videoSendBlockTotalUs: total time video sends were blocked |
| 20 | u32 | videoSendBlockMaxUs: longest single send |
| 24 | u32 | videoSendsOver20ms: number of sends that took more than 20 ms |
| 28 | u32 | gapsAfterSlowSend: gaps whose preceding send took more than 20 ms (directly answers "was the frame lost because sending stalled?") |
| 32 | u32 | audioPackets |
| 36 | u32 | audioEncodeTotalUs |
| 40 | u32 | audioSendBlockTotalUs |
| 44 | u32 | core3IdlePermille: idle permille of CPU core 3 (0xFFFFFFFF if unavailable) |
| 48 | u32 | sysdvrCpuPermille: combined permille of all SysDVR threads (from thread ticks, 0xFFFFFFFF if unavailable) |
| 52 | u32 | tosFlags: bit0 set on the video socket, bit1 set on the audio socket |

Future versions of `ExtDiag` may only append fields at the end and never move existing offsets. Clients accept
version ≥ 1 and read only the first 56 bytes they know.

## 6. IP_TOS

When `Reserved[3]` bit1 is requested, the server calls `setsockopt(IPPROTO_IP, IP_TOS, 0xA0)` on that socket (CS5,
which maps to the WMM video queue AC_VI) and records the result in the diagnostics `tosFlags`. A failure does not
affect streaming.

## 7. Compatibility

| Combination | Behavior |
|---|---|
| New client + official SysDVR | Audio marker is `0xFF`, no diagnostics packets → audio handled as PCM, experimental options hidden |
| Old client + extended SysDVR | No extension flag → identical to official |
| New client + extended SysDVR | Works as described in this document |

## 8. Resource constraints (server)

- No heap (`USE_HEAP 0` stays), static buffers only. When the thread stack is not enough, libopus gets a static
  pseudostack (`NONTHREADSAFE_PSEUDOSTACK`, not `VAR_ARRAYS`), and the worst case is measured.
- Each delivery reports the growth of `.text` and `.data/.bss` compared with the official build and the expected
  memory pool increase (free system memory on a Switch is limited; the test console has only about 4 MB left).
  Measured: +164 KB of process memory; details in `host/README.md` of the server repository.

## 9. Shared artifacts

- Test vectors: `tools/ext_vectors/` (about 1 second per codec: input PCM, encoded audio packets, expected decoder
  output for PCM24/ADPCM; Opus comes with a reference decode and is compared with a tolerance).
- Simulated server for macOS: `host/build/sysdvr_hostmock` in the server repository, see its `host/README.md` (by
  default the client looks for it in `../NSDVR-server` next to this repository). It uses the same protocol and
  codec C code as the sysmodule, streams an H.264 file and a 48 kHz stereo WAV, and supports the extension, control
  messages, diagnostics packets and options that inject send stalls.
