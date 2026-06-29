# Render-submit race — fix design (#2)

Pairs with `render-submit-sync-RE.md` (subagent RE of UE/OVRPlugin submit timing).
Status: design draft; final approach (A vs B) gated on the RE findings.

## Confirmed mechanism
- Our `ovrp_EndFrame4` → `xrEndFrame` presents **synchronously** (composites immediately).
- UE 4.25's RHI thread `vkQueueSubmit`s the eye-render command buffer **after**
  `ovrp_EndFrame4` returns. **Proof:** `debug.re4vr.qwait` (a `vkQueueWaitIdle` on UE's
  queue *before* we release/present) is a **no-op** — if UE's render were already on the
  queue, draining it would turn the black image correct; it doesn't, so the submit isn't
  on the queue yet when `end_frame` runs.
- The original VrApi path tolerates this because VrApi's EndFrame **also defers** the
  present onto the RHI submit (render flush + present ride the same queue submission / RHI
  flush), so render is naturally ordered before present. We broke that by presenting
  eagerly inside `xrEndFrame`.

## Why the current sync can't fix it (`vk_session.c`)
`xrr_vk_flush_submit_ex` records a `COLOR_ATTACHMENT_WRITE→MEMORY_READ` barrier and
submits it on UE's queue (`s_queue`), then `xrr_vk_flush_wait` waits its fence. Vulkan
queue execution is in-order, so this is correct **iff UE already submitted the eye
render**. Under load UE hasn't, so the barrier resolves an un-rendered image and the fence
signals against empty content → we release+present black. Load-gated exactly as observed
(sparse bridge = render makes the deadline = no black; dense geo = late = black).

## Prior attempts and why they failed
- **Barrier-only (current default):** ordered before UE's later submit → black under load.
- **qwait (`vkQueueWaitIdle`):** no-op (UE hasn't submitted). Diagnostic only.
- **Deferred-flush pipeline (`pipeline=1`, present N-1):** conceptually right (gives UE a
  full frame to land its submit) but holds the **OpenXR swapchain image acquired across
  `xrEndFrame`** → Meta runtime mis-composites + crashes. Unstable; shelved.

## Fix options
**A. Observe UE's submit (hook/interpose `vkQueueSubmit`) — most robust.**
Wrap `vkQueueSubmit` so the shim sees exactly when UE flushes the eye render; record that
submit's fence (or a timeline value). In `end_frame`, wait that fence before releasing +
presenting. No swapchain-lifecycle hacks, minimal added latency (only the necessary wait).
Open question (→ RE): is UE's `vkQueueSubmit` interceptable from our in-process `.so`
(symbol interposition / a thin Vulkan layer), and which submit carries the eye render?

**B. Deferred present via a shim-owned copy (robust fallback, +1 frame latency).**
Decouple the deferral from the OpenXR swapchain lifecycle (the cause of pipeline=1's
instability): keep acquire→release **within a single frame**, but present one frame late.
At frame N+1, UE's render-N submit has landed; copy UE's frame-N eye image into a
shim-owned `VkImage` (barrier-ordered after UE's submit, on the same queue), then present
the shim copy via a normal same-frame acquire/release. Never holds an OpenXR image across
`xrEndFrame`. Costs 1 frame of latency + one image copy.

**C. Bounded spin-wait for the submit in `end_frame`** — fragile (no clean way to detect
the submit without a hook), adds latency/stalls. Not recommended.

**D. Use an ovrp call UE makes around submit as the sync point** — only viable if the RE
finds UE invokes an OVRPlugin entry point right after the render submit. Unlikely; → RE.

## RE outcome (`render-submit-sync-RE.md`) → Option A is feasible
- **Original = zero Vulkan sync, definitively:** libOVRPlugin imports *no* `vk*` (only
  `vrapi_*`); libvrapi imports no `vk*` either. The compositor is a separate system
  process; swapchains are cross-process system-owned (`vrapi_CreateTextureSwapChainCrossProcess`).
  EndFrame4 only hands over swapchain handle + image index + pose; ordering is implicit via
  system swapchain ownership — the exact analogue of OpenXR acquire/wait/release.
- **EndFrame4 runs on the RHI thread:** `FCustomPresent::FinishRendering_RHIThread` →
  `FOculusHMD::FinishRHIFrame_RHIThread` → `ovrp_EndFrame4` (via PluginWrapper dispatch).
- **UE's eye-render submit is interceptable:** OculusHMD never calls `vkQueueSubmit`
  directly; it goes through the FVulkan RHI's **global dispatch pointer**
  `VulkanDynamicAPI::vkQueueSubmit` (libUE4.so `0xa643da0`), batched/deferred on the RHI
  thread. A global PFN we can patch → trampoline. **This makes Option A viable and portable**
  (pure Vulkan + a UE symbol; no Quest/VrApi dependency, so it carries to Steam Frame).

## DECISION: Option A (hook `VulkanDynamicAPI::vkQueueSubmit`), but resolve the
## threading interleave FIRST (instrument before we sync)
Critical open question the RE could not pin from statics: **on the RHI thread, does the
eye-render `vkQueueSubmit` happen BEFORE or AFTER the `EndFrame4` call?**
- If **submit-before-EndFrame**: by EndFrame the render is on the queue; we just
  `vkWaitForFences` on the recorded submit fence before release/present. No deadlock, no
  added latency. (But the `qwait` no-op argues against this — nothing was on the queue.)
- If **EndFrame-before-submit** (what `qwait` implies, same thread): we must NOT block in
  EndFrame (that thread does the later submit → self-deadlock). Instead **present-on-submit**:
  EndFrame stores the pending composition; the `vkQueueSubmit` trampoline, on seeing the
  eye-render submit, triggers the barrier+release+present. This mimics the original (present
  rides the RHI submit) with **no fixed frame of latency** and no cross-frame swapchain hold.

The interleave decides wait-in-EndFrame vs present-on-submit, so **step 1 is the hook as
pure instrumentation** (no behavior change): patch the global PFN, log every submit with
tid + timestamp + a monotonic seq, and log EndFrame with the same clock. One device run off
the bridge confirms the order (and validates the patch offset + that `s_queue` is the queue
UE submits eyes on — the RE's two stated uncertainties). Then implement the matching
variant behind a `debug.re4vr.*` toggle.

Device test throughout: Standing, walk off the bridge toward the house (reliable black
trigger), `trace=1`.
