# Q25 graphics: Minecraft 26.3 and experimental Vulkan

Offline follow-up: 2026-11-09. No phone commands, package changes, game launches,
Phosh restarts, or kernel changes during this follow-up. Connectivity trials stay
paused; existing call/message safeguards remain untouched.

## Established baseline (earlier isolated live probe)

Q25's native Linux driver is **Panfrost**, not Android's proprietary `mali_kbase`.
Mesa 26.2.6 exposes the **Mali-G57 MC2** renderer with desktop OpenGL 3.1.
The earlier temporary Java probe used the instance's LWJGL 3.4.3 libraries,
created only a hidden window, and did not launch Minecraft or access worlds or
account data. The probe's temporary directory was removed.

| Probe | Result |
|---|---|
| Default SDL selection + default LWJGL | SDL selected X11, LWJGL supplied `libEGL.so.1`; `Could not retrieve OpenGL functions` |
| SDL X11 + `-Dorg.lwjgl.opengl.contextAPI=native` | GLX library loaded, function pointers matched; OpenGL 3.3 context rejected |
| SDL Wayland + explicit EGL | EGL library loaded, function pointers matched; OpenGL 3.3 context rejected |
| SDL X11 + native, requesting 3.1 | Context succeeded; renderer `Mali-G57 MC2 (Panfrost)` |

Thus the loader mismatch and the default OpenGL version limit are separate.
A successful context is not a successful game or performance test.

## Important OpenGL lead: experimental mode already exists

Mesa's **26.2.6** `src/gallium/drivers/panfrost/pan_screen.c` contains:

- `gl3`: described as "Enable experimental GL 3.x implementation, up to 3.3".
- `PAN_MESA_DEBUG` selects the driver debug options.
- `glsl_feature_level` and its compatibility counterpart are **140 normally**,
  **330 when `PAN_DBG_GL3` is selected**.
- `panfrost_init_shader_caps()` enables vertex, fragment and compute stages;
  this is not evidence of complete geometry/tessellation support.
- Timer queries also depend on kernel-exposed timestamp capability/frequency.

Mesa's desktop version calculation in `src/mesa/main/version.c` requires
GLSL >=150 for 3.2 and >=330 for 3.3, plus the listed extension requirements.
The default GLSL cap is therefore one concrete blocker; the source inspection
does not establish that it is the only blocker on this phone.

**Candidate for a separately approved isolated live test:** use
`PAN_MESA_DEBUG=gl3` only in the hidden probe process, paired with consistent
SDL/LWJGL backend selection. First report context creation, renderer, GLSL and
extensions; then, only if successful, test a tiny offscreen shader/readback.
Keep caches in the temporary probe directory, preserve desktop configuration,
and stop on faults. A userspace timeout does not guarantee containment of a GPU
hang affecting the compositor.

Do not set this globally or treat it as conformant OpenGL 3.3. It is not a
recommendation to use `MESA_GL_VERSION_OVERRIDE` to falsify the version string.
No experimental-mode live test was performed in this follow-up.

## Authorized isolated live follow-up: 2026-11-09

After the user approved the probe, created and verified read-only root snapshot
46 and separate home snapshot
`/.snapshots/manual-home/pre-gl3-20261109T063952Z`. These do not cover raw boot
partitions, and no boot/driver changes were made.

Using the instance's existing LWJGL 3.4.3 JARs and its
`3.4.3-lwjgl.1` ARM64 native archives, ran temporary hidden-window Java probes
as `max`, with caches/config/data/temp directories isolated under `/tmp` and
30-second process timeouts. An initial attempt stopped before graphics startup
because the native archive directory version was different; corrected archive
selection before the actual probes. No Minecraft launch or instance edits.

| Probe | Result |
|---|---|
| Wayland/EGL, normal Mesa, GL 3.3 requested | `EGL_BAD_MATCH`, no context |
| Wayland/EGL, `PAN_MESA_DEBUG=gl3` | Hardware Panfrost GL 3.3, GLSL 3.30; draw/readback passed |
| X11/native GLX, `PAN_MESA_DEBUG=gl3` | Hardware Panfrost GL 3.3, GLSL 3.30; draw/readback passed |

Both experimental contexts reported `Mali-G57 MC2 (Panfrost)`, Mesa
26.2.6-arch1.1, LWJGL OpenGL33 entry points, ARB timer query and
ARB blend-func-extended capabilities. A GLSL 330 vertex/fragment program drew
one triangle into an 8x8 RGBA8 framebuffer; pixel readback was exactly
`64,128,191,255`, with `GL_NO_ERROR`. SDL/LWJGL function pointers matched.
This is a basic rendering smoke test, **not** conformance, performance, or
Minecraft compatibility validation.

Temporary phone files were removed. Phosh, NetworkManager and ModemManager
remained active with their original PIDs and zero restarts. A bounded kernel
journal filter found no new GPU/DRM fault/error/reset lines. No global graphics
settings, drivers, game files, connectivity safeguards or services were changed.
Evidence on the host: `~/q25-work/gpu-investigation/probe-gl3.py` and
`probe-gl3-run2.log` (initial archive-selection failure: `probe-gl3.log`).

Next step, if approved: scope matching backend selection and `gl3` to the
Minecraft instance and test startup; do not infer stability in worlds from this
small probe.

## Vulkan projects: useful source, not installable Q25 solutions

The official Mesa support table leaves G57/G68 Vulkan support blank. Its generic
experimental-enable variable does not itself implement a missing architecture.

### FourFectVK / Noysz panvk-g99-jm

The project specifically targets G57 MC2 / Helio G99. Its documented test device
is an Infinix Note 40 running Android with **kbase r54p1**, accessed through
`/dev/mali0`. It supplies Valhall-v9 Job Manager draw/compute work, patch series,
and reproduction records. Its test9 report explicitly calls it experimental and
non-conformant: 5,658 tracked CTS cases, 3,754 Pass, 1,890 NotSupported, 14 Fail.
These are the project's results, not independently reproduced Q25 results.

The test9 build instructions use a different base/patch series from the older
`docs/reproduction.md`; do not mix them. Its "host"/X11 tests still concern the
documented Android/kbase stack, not proof of mainline Panfrost compatibility.

### JimVulkan mali-panvk

The README claims testing G57/G99 and other GPUs on Android/kbase and Vulkan 1.4.
Source inspection finds real v9 build selection (`jm_archs = [6, 7, 9]`),
`jm/panvk_v9_cmd_draw.c`, and both DRM-node and kbase-device creation paths.
That makes it a source-review candidate, but retaining generic DRM code does
**not** demonstrate that its v9 draw, submission, synchronization, memory and
presentation paths work with Q25's native Panfrost kernel driver.

Its published build uses the Android NDK and Android platform dependencies.
A ZIP from this project is not a native Arch Linux Vulkan ICD installation.
The claimed conformance/features/performance have not been independently tested.

### FristOneRR

Also advertises G57/G99 support, but explicitly targets Android/kbase and
Winlator with a separate wrapper. Its README calls the driver non-conformant.
Treat game-performance claims as upstream project claims, not Q25 evidence.

## Recommendation

1. Prefer the small, process-local experimental OpenGL probe before a Vulkan port.
   It uses the already installed Mesa and does not require replacing the working
   desktop stack. Snapshot before potentially risky live graphics experiments.
2. If that does not meet the application's needs, review v9 Vulkan patches against
   native Panfrost DRM submission/synchronization and a glibc/Wayland build.
   Pin sources and build on the host before considering hardware tests.
3. Any eventual Vulkan candidate must be selected only for a test process via
   a private ICD manifest, not installed globally or used by Phosh initially.
4. Do not replace Panfrost with kbase, alter boot images, or flash userdata to
   match an Android driver's prerequisites. None of that is authorized here.

Offline downloads/source excerpts and prior probe evidence are retained in the
host workspace `~/q25-work/gpu-investigation/`. Downloaded upstream build scripts
were not executed. The offline findings establish leads and boundaries; the later authorized live
probe establishes only the bounded rendering result described above, not a game
repair.

## Sources

- [Official Mesa support table](https://docs.mesa3d.org/drivers/panfrost.html)
- [Mesa 26.2.6 Panfrost capabilities (source mirror)](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-26.2.6/src/gallium/drivers/panfrost/pan_screen.c)
- [Mesa 26.2.6 version calculation (source mirror)](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-26.2.6/src/mesa/main/version.c)
- [Mesa 26.2.6 Panfrost kernel interface (source mirror)](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-26.2.6/src/panfrost/lib/kmod/panfrost_kmod.c)
- [FourFectVK](https://github.com/Noysz/panvk-g99-jm)
- [FourFectVK reproduction prerequisites](https://github.com/Noysz/panvk-g99-jm/blob/main/docs/reproduction.md)
- [FourFectVK test9 build order](https://github.com/Noysz/panvk-g99-jm/blob/main/docs/test9/build-order.md)
- [JimVulkan source](https://github.com/JimVulkan/mali-panvk)
- [FristOneRR documentation](https://github.com/FristOneRR/FristOneRR-Panvk-Driver)

Fork HEADs recorded during inspection (moving-branch downloads are evidence,
not a fully frozen/reproduced build): JimVulkan
`58f2c55854bd48b18df756edb11f187af9cd2fde`; Noysz
`08aa2e62580e447b28fe419d8995b84d8d6f1fff`.
