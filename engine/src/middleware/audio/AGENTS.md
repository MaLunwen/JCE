# engine/src/middleware/audio — Audio (L4)

> miniaudio mixer, decoders (Opus/AAC/M4A), reverb zones, occlusion, ECS audio sources.

## Identity

- **Layer**: L4. C99 (+ minor `.cpp` for audio_stream where needed).
- **Public umbrella**: `<jce/api_audio.h>` → `<jce/middleware/audio/jce_*.h>`
- **Deps (PRIVATE)**: `miniaudio`, `Opus::opus`, `Ogg::ogg`, `flecs::flecs_static`, optionally `fdk-aac` (gated by `JCE_PATENTED_CODECS_ENABLED`), `jce_video` (for shared codec types).

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_audio.h` / `jce_audio_types.h` | `jce_audio.c` | Public façade: device, sound handles, play/stop/pitch/volume, named `ma_sound_group` buses (`jce_audio_bus_*` / `jce_audio_voice_set_bus`), global Freeverb reverb node (`jce_audio_set_reverb`) |
| `jce_audio_mixer.h` | `jce_audio_mixer.c` | Bus/group mixer (CPU solo/mute/volume tree; runtime mirrors its resolved gains onto the device buses each frame) |
| `jce_audio_occlusion.h` | `jce_audio_occlusion.c` | Geometry-based occlusion query (uses scene partition) |
| `jce_reverb_zones.h` | `jce_reverb_zones.c` | Per-zone reverb + crossfade |
| `jce_audio_ecs.h` | `jce_audio_ecs.c` | flecs components: AudioSource, AudioListener |
| `jce_m4a_decode.h` | `jce_m4a_decode.c` | M4A container + AAC decode (gated when patented codecs off) |
| (internal) `jce_aac_decode.h/.c` | — | AAC decode wrapper (fdk-aac) — gated |
| (internal) `jce_miniaudio_impl.c` | — | Single TU that #includes miniaudio implementation; **OBJC on Apple** (CMake sets `LANGUAGE OBJC`) |
| (internal) `jce_miniaudio_opus_backend.h/.c` | — | Opus decoder plugged into miniaudio |
| (internal) `jce_audio_stream.h/.cpp` | — | Streaming source for long-form audio |

## Rules

1. **miniaudio is private.** Public headers expose handles, not `ma_*` types.
2. **Apple platforms**: `jce_miniaudio_impl.c` MUST compile as Objective-C (AVFoundation). Don't change its extension or skip the CMake flag.
3. **Patented codecs (AAC via fdk-aac)** gated by `JCE_PATENTED_CODECS_ENABLED`. Default royalty-free path = Opus. Don't hard-require AAC.
4. **3D audio** uses `JceAudioListener` (singleton) + `JceAudioSource` components. Spatialization, attenuation, doppler all happen on the audio thread.
5. **Allocations** via `jce_alloc`; audio thread must not touch the log file directly (use ring buffer).
6. **Reverb zones** query `scene` space partition — depend on it but don't reach into its internals; use the public scene query API.

## Don't

- Don't expose miniaudio handles in public headers.
- Don't write your own resampler / mixer in parallel.
- Don't add a hard dep on a patented codec.

## 2026-09-21 — `JCE_NO_AUDIO` 分支**今天就链接不过**，而且没有任何构建编译它

```
jce_audio.c:22    #ifndef JCE_NO_AUDIO
jce_audio.c:2304  #else  /* JCE_NO_AUDIO */
jce_audio.c:2475  #endif
```

实测（不是推测）：real 分支定义 **48** 个 `jce_audio_*`，stub 分支 **43** 个，差 5。
逐个核过是不是公共符号——因为「差 5 个」在它们全是 static 时毫无意义：

| 符号 | |
|---|---|
| `jce_audio_cpu_free` | **PUBLIC (JCE_API)** |
| `jce_audio_decode_cpu` | **PUBLIC (JCE_API)** |
| `jce_audio_master_tap_set` | **PUBLIC (JCE_API)** |
| `jce_audio_upload_cpu` | **PUBLIC (JCE_API)** |
| `jce_audio_load_inner` | static（无害，正是会把结论吹大的那个假阳性） |

**四个公共符号在头文件里无条件声明、在 `JCE_NO_AUDIO` 下没有定义** ⟹
定义这个宏今天就是四个 undefined reference。

**为什么没人发现**：`CMakeLists.txt`、`cmake/`、`CMakePresets.json` **全都不定义它**。
全树唯一的其它出现是 `tools/lint/check_logged_env_vars.py` 的 **docstring** 拿它举例。
**门禁不可能在它从不编译的代码上失败。** 守卫点共 10 处、跨 4 个文件
（`jce_audio.c`、`jce_miniaudio_impl.c`、`jce_asset_cooker.c`、`jce_async_pool.c`）。

> **这个发现此前已经被做出过一次，而且是对的。**
> `docs/audits/engine_standard_parity/00_REPOSITORY_FACTS.md:253` 写着
> 「a complete audio-disabled build mode … **that no configuration can compile**」。
> 它没起任何作用，因为 **`docs/*` 被 gitignore（`.gitignore:199`）**——
> clean clone 上它不存在。
> **一个正确的发现，记录在一个没有读者的地方**，和没发现的区别只在于有人白花了时间。
> 这条所以写在这里：**这个文件是被跟踪的，而且改这个模块的人会读它。**

**对下一个改动这里的人（包括明天的我）的硬约束：**
**新增任何公共 audio 函数必须在两个分支里都定义，而没有任何东西会告诉你漏了。**
默认构建带音频，所以漏掉 stub 照样编译通过、门禁照样全绿、缺口照样出货。

**不要靠「补上那四个 stub」来修。** 那会让它链接通过，
而真正的缺陷——**一个没有任何东西验证的配置**——原封不动。
正确的修法是 owner 决定：要么删掉这个分支（如果 no-audio 不是受支持的配置），
要么加一个编译它的构建（如果是）。

## 2026-09-21 — 侧链闪避的**头**在 `jce_audio_duck_pump.c`，而它是一个独立 TU 有理由

FEATURE 5.2 把闪避拆成两半，**两半互相不可 include**：

| 文件 | 它是什么 | 它的约束 |
|---|---|---|
| `jce_audio_mixer.c` | 包络跟随器 + 压缩曲线 | 头文件首句写着 **"Engine-agnostic — no miniaudio/FMOD coupling"**。正是这句话让同一份跟随器可以被离线驱动，并且**逐步就是活路径跑的那份代码** |
| `jce_audio.c` | 活总线电平表 `jce_audio_bus_get_peak` | 在真实节点图上由音频线程采样 |
| **`jce_audio_duck_pump.c`** | **接缝** | 唯一同时知道两者的文件 |

**为什么不放进任何一边**：放进 mixer 头就让它那句核心声明变成假的——
而「一句写在公共头里的假话」正是这一行 ledger 存在的原因。
放进 `jce_audio.h` 则给后端加上一份它不需要的总线记账依赖。

**为什么是一个函数而不是两处**：第一版是两处。runtime 里内联一份，
单元测试里另一份，**测试那份的注释自称是「rt_apply_mixer 用的同样几行」**。
这种布置**结构上抓不到接缝里的缺陷**——测试那份可以是对的而 runtime 那份是错的。
当时注释写着「删掉 memo 这条就会红」，而它说的是**另一个 memo**。

### 改这里之前必须知道的两条

1. **`jce_audio_bus_get_peak` 是 read-and-clear**。第二个读者不是「缺了个功能」，
   是**错**：`duck_advance` 每个已安装侧链回调一次，而**两个 target 共用一个 key
   是常规配置**（Music 和 SFX 同时压在 Voice 下）。每次回调都读表 ⟹ 第一个 target
   吃掉 key 的峰值，第二个拿到那几微秒里到达的东西 ⟹ **那条总线就是不闪避**，
   静默，且只在配置了第二条同 key 侧链的项目里。所以 pump 内部**按 pump 记忆**。
   编辑器里的 VU 表同理——见 `tools/lint/editor_consumption_exempt.txt` 里那两行。

2. **电平是 POST-FADER**，`duck_advance` 因此只折叠 key 的 **mute/solo**、
   **绝不折叠它的 volume**——volume 已经在测量里了，再乘一次是把 key 增益**平方**：
   在 1.0 上精确，在 0.5 上差 6 dB。**任何人在 volume 1.0 上写的测试都不会红。**
   `tests/middleware/audio/test_jce_audio_bus_meter.c` 的变异对照因此建在 **0.25** 上。

### 离线渲染：`jce_audio_create_offline` + `jce_audio_render_offline`

它们存在是为了让音频测试**不需要设备、因而不会 skip**
（`test_jce_audio_master_tap.c` 没设备就 self-ignore，而 **SKIP 不是 PASS**）。

- `render_offline` **拒绝**设备泵的引擎，不是警告它——一张图两个泵会争同一批读游标，
  而**注释是调用者最有把握不读的东西**。
- 它**先填静音再读**：端点上什么都没挂的图，miniaudio **没有静音可混所以一帧都不产出**。
  共享设备回调也是这么干的（memset 再累加），那正是 master tap「构造上无缝」的来源。

## File streams and waveform

`jce_audio_file.c` is the public host-file stream facade: native miniaudio, raw
ADTS, or MP4 audio; bounded PCM ring and fixed 2048-bin background waveform.
`jce_ogg_opus.c` is the canonical incremental Ogg-Opus reader reused by JCE
miniaudio file/memory adapters. `jce_adts_file.c` retains a bounded frame-offset
index and feeds/drains the existing AAC wrapper. No full input/PCM fallback.
Stop a voice before closing its file; discard analysis tasks before freeing state.
