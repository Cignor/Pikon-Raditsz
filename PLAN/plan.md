## Plan: Integrate Range Start/End into `VideoDrawImpactModuleProcessor` Timeline

### 1. Goal & Context

- **Overall goal**: Ensure `VideoDrawImpactModuleProcessor` fully respects the source clip’s **range start (IN)** and **range end (OUT)** when:
  - Creating timeline keyframes from user drawing.
  - Rendering those keyframes over time during playback.
  - Computing persistence and cleanup of keyframes when the source loops.
- **Source of truth for timeline**:
  - `VideoFileLoaderModule` already implements the **timeline provider interface** (`canProvideTimeline`, `getTimelinePositionSeconds`, `getTimelineDurationSeconds`, `isTimelineActive`).
  - It also has **trim range controls**:
    - `inNormParam` (`"in"`) and `outNormParam` (`"out"`) as normalized [0..1] positions within the media.
  - Its internal loop logic uses these in/out values to define the effective playback window and audio/video loop behavior.
- **Current `VideoDrawImpactModuleProcessor` behavior**:
  - Reads `SourceTimelineState` via `getSourceTimelineState()`, using `module->getTimelinePositionSeconds() / getTimelineDurationSeconds() / isTimelineActive()`.
  - When timeline is valid, it assumes **full media duration** as the domain [0, durationSeconds], ignoring the video module’s IN/OUT range.
  - Keyframes are stored with:
    - `timeSeconds` (absolute within full duration or synthetic frame-based).
    - `persistenceSeconds`.
    - `normalizedX`, `normalizedY` (spatial).
  - Rendering (`drawStrokesOnFrame`) and cleanup (`cleanupExpiredKeyframes`) operate over the full duration / frame indices, not the trimmed range window.

**Target behavior**: The drawing “impact” timeline should effectively be **clipped and looped** to the same IN/OUT range as the underlying video/audio, so the user experience is consistent with what they see and hear from `VideoFileLoaderModule`.

---

### 2. High-Level Design Options

- **Option A (Minimal integration, low risk)**: 
  - Keep `SourceTimelineState` API as-is.
  - In `VideoDrawImpactModuleProcessor`, **derive effective range start/end** from the source module (via new helper calls or extended interface) and:
    - Normalize all keyframe times into **range-local coordinates** when creating/updating them.
    - During rendering, interpret current playhead time modulo the IN/OUT window.
  - No changes to `VideoFileLoaderModule` public interface (except possibly a **read-only query** for its IN/OUT range).

- **Option B (Shared “range-aware” timeline interface, medium risk)**:
  - Extend the timeline provider interface with **explicit range data**:
    - e.g. `getTimelineRangeSeconds(double& start, double& end)` and/or a normalized `[0,1]` range for UI alignment.
  - Update all modules that implement `canProvideTimeline()` to respect this extended interface.
  - `VideoDrawImpactModuleProcessor` uses this richer API instead of ad‑hoc introspection.

- **Option C (Full “clip domain” abstraction, higher complexity)**:
  - Define a generic **ClipTimelineDomain** that:
    - Encapsulates original media duration.
    - Effective IN/OUT range.
    - Loop semantics and wrap handling.
  - Both `VideoFileLoaderModule` and `VideoDrawImpactModuleProcessor` use this shared abstraction for all time calculations.
  - Requires broader refactor and more modules to opt into the domain.

For this task, **Option A** is preferred (smaller blast radius, faster to implement, still robust). We can design it in a way that’s compatible with Option B/C later.

---

### 3. Detailed Plan (Option A – Range-Aware Consumer)

#### 3.1. Discover & Access Source Range Information

- **Objective**: Allow `VideoDrawImpactModuleProcessor` to know the **IN/OUT times** of its upstream video source.
- **Steps**:
  - **3.1.1**: Inspect `VideoFileLoaderModule` (header & implementation) for:
    - Public or protected access to normalized **IN/OUT parameters** or their converted sample/time equivalents.
    - Whether it already exposes trimmed duration or range somewhere in its public API.
  - **3.1.2**: If not available, plan a **minimal read-only accessor** in `VideoFileLoaderModule`:
    - e.g. `bool getTrimmedRangeSeconds(double& rangeStart, double& rangeEnd) const;`
      - Uses `inNormParam`, `outNormParam` and `totalDurationMs` (or audio length / sample rate) internally.
      - Returns `false` if duration is unknown.
  - **3.1.3**: In `VideoDrawImpactModuleProcessor::getSourceTimelineState()`:
    - After acquiring `module` and verifying `canProvideTimeline()`, **attempt dynamic_cast** to `VideoFileLoaderModule` (or an interface base class if available).
    - If cast succeeds, query range start/end into `state.rangeStartSeconds / rangeEndSeconds` (extend `SourceTimelineState` struct).
    - If cast fails, leave these as “no range” (default zero / full duration).
  - **3.1.4**: Extend `SourceTimelineState` to include:
    - `double rangeStartSeconds` (default 0.0).
    - `double rangeEndSeconds` (default = durationSeconds when valid, or 0.0 when unknown).
    - Possibly derived helpers:
      - `double effectiveDuration() const` → max(rangeEnd - rangeStart, 0).
      - `bool hasRange() const` → rangeEnd > rangeStart and both non‑negative.

**Risks / considerations**:
- **Dependency coupling**: `VideoDrawImpactModuleProcessor` will explicitly depend on `VideoFileLoaderModule` for richer behavior. Mitigate by:
  - Accessing via an **interface or minimal helper** where feasible.
  - Falling back gracefully if source is not a `VideoFileLoaderModule` (e.g. webcam, other video sources).

---

#### 3.2. Normalize Keyframe Times into Range-Local Coordinates

- **Objective**: New keyframes created while drawing should be aligned to the **effective playback window** (IN–OUT) so that:
  - They loop correctly when the source loops.
  - They don’t get “orphaned” outside the range if the user narrows the IN/OUT later.

- **Current behavior**:
  - In `processPendingDrawOps()`:
    - `SourceTimelineState timelineState = getSourceTimelineState();`
    - If `timelineState.isValid`:
      - `keyframe.timeSeconds` is `timelineState.positionSeconds` (bounded/wrapped by full media duration, not IN/OUT).
    - Else:
      - Uses `currentFrame` as synthetic time axis.

- **Planned behavior**:
  - **3.2.1**: When timeline is valid and range information is available:
    - Let:
      - `R0 = rangeStartSeconds`.
      - `R1 = rangeEndSeconds` (if zero or <= R0, fallback to full duration).
      - `D  = R1 - R0` (effective window).
      - `P  = timelineState.positionSeconds`.
    - Compute **range-local position**:
      - First map `P` into the range [R0, R1] respecting wrap:
        - If `P < R0`, repeatedly add `D` until `P >= R0` (or treat as equivalent to R0).
        - If `P > R1`, subtract `D` until `P <= R1` (or treat as R1).
      - Range-local time:
        - `localT = juce::jlimit(R0, R1, P)` or modulo mapping when integration with loop semantics is clear.
    - Store either:
      - **Absolute time** but documented as range-relative: `keyframe.timeSeconds = localT;` with the invariant that **all keyframes live within [R0, R1]**.
      - Or **normalized position within range** (requires new field or encoding; more complex).
  - **3.2.2**: Ensure `persistenceSeconds` logic is aware of range window:
    - Persistence should not exceed `D` in timeline-driven mode, or else markers will “leak” into next loop cycle.
    - Clamp `keyframe.persistenceSeconds` to `<= D` where `D > 0`.
  - **3.2.3**: When timeline is **not valid**:
    - Keep existing frame-based approach but document that this mode is **not range-aware** (used e.g. for generic video sources that don’t provide timeline).

**Risks / considerations**:
- **Behavior when IN/OUT are changed after drawing**:
  - Existing keyframes may fall outside the new window.
  - Need a policy (see 3.5) for:
    - Hiding them.
    - Soft-clamping them into the window.
    - Or re-normalizing them proportionally.

---

#### 3.3. Make Runtime Rendering Respect the Range

- **Objective**: Ensure `drawStrokesOnFrame()` and `cleanupExpiredKeyframes()` operate within the **active IN/OUT window** during playback and looping.

- **Current behavior**:
  - `drawStrokesOnFrame()`:
    - Computes `currentTime`:
      - If timeline valid: `timelineState.positionSeconds`.
      - Else: `currentFrameIndex * frameDurationSeconds`.
    - Keyframes are drawn if `dt` (time since keyframe) is within `persistenceSeconds`, with optional wrapping based on full `duration`.
  - `cleanupExpiredKeyframes()`:
    - If `getSourceTimelineState().isValid` → **returns early** and preserves keyframes (assumes persistent markers when timeline-driven).
    - Else: removes keyframes whose frame age exceeds `maxPersistence`.

- **Planned behavior**:
  - **3.3.1**: Adjust `drawStrokesOnFrame()` to use **range-local time**:
    - Retrieve `SourceTimelineState` with range info.
    - If `state.isValid && state.hasRange()`:
      - Define:
        - `R0, R1, D` as above.
        - `playheadT = state.positionSeconds`.
      - Compute range-local playhead time `playheadLocalT`:
        - Option A (clamp): `playheadLocalT = juce::jlimit(R0, R1, playheadT);`
        - Option B (wrap with loop): Map `playheadT` into `[R0, R1)` via modulo with `D`, matching what `VideoFileLoaderModule` does when looping.
      - For each keyframe:
        - Interpret its `timeSeconds` as within `[R0, R1]`.
        - Compute `dt` as **range-aware** difference with optional wrap:
          - If `playheadLocalT < kf.timeSeconds` and looping is active, add `D` to `playheadLocalT` or subtract from `kf.timeSeconds` to compute shortest forward difference.
        - Draw only if `0 <= dt <= persistenceSeconds`.
    - If `!state.hasRange()`:
      - Fall back to current full-duration logic.
  - **3.3.2**: Respect **loop boundaries**:
    - When `VideoFileLoaderModule` loops at OUT back to IN, `state.positionSeconds` will jump from ~R1 back to ~R0.
    - Range-local mapping and wrap-aware `dt` calculation should ensure keyframes:
      - Fade out near the boundary correctly.
      - Re-appear in a consistent pattern each loop.
  - **3.3.3**: Maintain consistent behavior for non-range sources:
    - If source has no range but provides a duration, keep existing semantics.
    - This avoids breaking webcam / other video modules that don’t use IN/OUT.

**Risks / considerations**:
- **Double-wrapping**: If `VideoFileLoaderModule` already reports `positionSeconds` within trimmed window rather than full duration, additional range mapping could mistakenly shrink domain. Need to:
  - Verify actual behavior empirically or via code inspection before implementing wrap.
  - Design mapping to be **idempotent** if `positionSeconds` is already clipped.

---

#### 3.4. Update Timeline UI to Visualize Range

- **Objective**: Ensure the ImGui timeline view in `drawParametersInNode()` visually reflects the **active IN/OUT window**, so that:
  - Keyframes appear in the correct sub-section of the timeline.
  - Panning/zooming feels consistent with what the user hears and sees from the video.

- **Current behavior**:
  - When timeline is valid:
    - `totalDuration` is based on `timelineStateUi.durationSeconds` (full media).
  - When timeline is not valid:
    - `totalDuration` is based on frame statistics.
  - Grid lines, playhead, and markers are all laid out over this `totalDuration`.

- **Planned behavior**:
  - **3.4.1**: Embed range information into UI:
    - Read `SourceTimelineState` with `rangeStartSeconds`, `rangeEndSeconds`.
    - Define:
      - `displayStart = hasRange ? rangeStartSeconds : 0.0`.
      - `displayEnd   = hasRange ? rangeEndSeconds : totalDuration`.
  - **3.4.2**: Restrict visible domain to IN/OUT:
    - For grid drawing:
      - Use `[displayStart, displayEnd]` instead of `[0, duration]`.
    - For keyframe positions:
      - Ensure `keyTime` used in UI converts from stored `keyframe.timeSeconds` appropriately:
        - If we chose to store range-local times already in `[R0, R1]`, we can use `keyTime = keyframe.timeSeconds` but treat `displayStart = R0`.
        - Alternatively, store times relative to `R0` and offset during display.
  - **3.4.3**: Show visual indication of range:
    - Optionally, draw:
      - A subtle background band only over `[R0, R1]`.
      - Or grey-out regions outside the range if the full duration still needs to be displayed (e.g. when user is editing range).
  - **3.4.4**: Scroll and zoom behavior:
    - Keep playhead centering logic, but ensure it’s **relative to `[displayStart, displayEnd]`**.
    - When user adjusts zoom via wheel over the timeline:
      - Keep the playhead within the range window.

**Risks / considerations**:
- **Backward compatibility**:
  - Existing presets with keyframes assuming “full duration” may display differently once UI is clamped to range.
  - Mitigate by carefully mapping old `timeSeconds` into the new range when loading (see 3.5).

---

#### 3.5. Migration & Preset Compatibility

- **Objective**: Avoid breaking existing presets and user data when changing how range and keyframe times are interpreted.

- **Current state**:
  - `getExtraStateTree()` saves:
    - `keyframe.timeSeconds` as double (possibly > IN/OUT once range is introduced).
    - `framePersistence`, `brushSize`, `normalizedX`, `normalizedY`, color, etc.
  - `setExtraStateTree()` restores values verbatim, without any range considerations.

- **Planned behavior**:
  - **3.5.1**: Decide on **versioning or detection strategy**:
    - Option 1: Introduce a `"version"` property in `VideoDrawImpactState` (e.g. `1` for legacy, `2` for range-aware).
      - On load:
        - If version missing or 1: treat times as full-duration domain and **convert** to range-aware domain if we can query the source at load-time.
        - If version >=2: assume they are already range-aware and do not transform.
    - Option 2: Use **heuristics**:
      - If all keyframes lie within `[0, duration]` but outside `[R0,R1]`, we can either:
        - Leave them “hidden”.
        - Or project them onto `[R0,R1]` proportionally.
  - **3.5.2**: Range-agnostic fallback for presets without a resolvable source:
    - When loading a preset, the upstream source module might not yet be instantiated or resolved.
    - Strategy:
      - Load keyframes unchanged.
      - Mark them as “legacy full-domain”.
      - Once `getSourceTimelineState()` becomes valid and range is known, apply **one-time migration** to clamp or rebase times into the range, then mark as upgraded.
  - **3.5.3**: Document behavior:
    - Within code comments and architecture docs, document that:
      - Post-upgrade, all keyframes are interpreted **relative to the active IN/OUT range** of their source.
      - Changing IN/OUT will **change where keyframes appear relative to absolute time**, but preserve timeline-local relationships and persistence.

**Risks / considerations**:
- **Race conditions / ordering**:
  - Draw module may load its state before the source module reports its duration and range.
  - Must handle deferred migration when the range becomes available later.

---

#### 3.6. Handling IN/OUT Changes at Runtime

- **Objective**: Define behavior when the **user changes IN or OUT** after they have already drawn impacts.

- **Possible policies**:
  - **Policy A (Clip & hide)**:
    - Any keyframe whose time is outside new [R0,R1] is:
      - Either removed (hard delete).
      - Or kept but simply not drawn (time-skip).
  - **Policy B (Reposition proportionally)**:
    - Treat keyframe times as normalized positions in the old range.
    - When range changes, re-map them proportionally:
      - `normOld = (t - oldR0) / (oldR1 - oldR0)`.
      - `tNew = newR0 + normOld * (newR1 - newR0)`.
  - **Policy C (Sticky absolute time)**:
    - Interpret keyframe times as absolute within full media duration.
    - Only rendering clamps to [R0,R1], so keyframes outside are naturally not drawn.

- **Recommended for first iteration**:
  - **Policy C** for **backwards compatibility and simplicity**:
    - Keep keyframe times absolute; rendering becomes range-aware.
    - If a keyframe’s time is outside [R0,R1], runtime draw logic simply skips it.
  - Then optionally evolve to Policy B with explicit versioning if needed.

- **Implementation steps**:
  - **3.6.1**: Expose IN/OUT changes in `VideoFileLoaderModule` (if not already reflected in `getTimelineDurationSeconds()`).
  - **3.6.2**: In draw logic:
    - Check `kf.timeSeconds` against `[R0,R1]`:
      - If outside and Policy C chosen, `shouldDraw` remains false even if persistence would otherwise include it.
  - **3.6.3**: In UI:
    - Optionally provide a warning or indicator if many keyframes lie outside the active range (future enhancement).

---

#### 3.7. Testing Strategy

- **Unit-/component-level tests** (where practical in this codebase):
  - **Scenario 1**: Video loader with full range (IN=0, OUT=1).
    - Draw impacts at various times.
    - Confirm that behavior matches current implementation (regression test).
  - **Scenario 2**: Narrow range (e.g. IN=0.25, OUT=0.75) with loop enabled.
    - Draw strokes near IN, mid, and OUT positions.
    - Let playback loop; confirm that:
      - Keyframes fire in the same order each loop.
      - Nothing appears before IN or after OUT.
  - **Scenario 3**: Change IN/OUT after drawing.
    - With Policy C:
      - Confirm that keyframes outside new range are not drawn.
      - Keyframes within range keep their relative behavior.
  - **Scenario 4**: Non-timeline sources (e.g. webcam-like modules).
    - Verify fallback behavior is unchanged and stable.

- **Manual UX tests**:
  - Use the preset creator UI to:
    - Chain `VideoFileLoaderModule` → `VideoDrawImpactModuleProcessor`.
    - Enable loop, adjust speed, and scrub timeline.
    - Intentionally set weird ranges (IN > 0.9, OUT near 1.0, or very small windows) and ensure the drawing impacts remain predictable and responsive.

---

### 4. Risk Assessment

- **Overall risk rating**: **Medium**

- **Key risks**:
  - **Range semantics mismatch** (Medium):
    - If `VideoFileLoaderModule` internally defines IN/OUT differently (e.g. per-audio vs per-video domain), mapping may be off by a frame or more.
    - Mitigation: Rely on `getTimelinePositionSeconds()` / `getTimelineDurationSeconds()` as the **authoritative numeric domain**, and only use IN/OUT to constrain / filter, not recompute times from scratch.
  - **Preset compatibility regression** (Medium–High):
    - Existing presets could display impacts differently if times are reinterpreted.
    - Mitigation: Start with conservative Policy C and avoid modifying stored times; only adjust rendering behavior.
  - **Coupling between modules** (Low–Medium):
    - `VideoDrawImpactModuleProcessor` becomes aware of `VideoFileLoaderModule` details.
    - Mitigation: Keep cross-module interface very small and read-only; use dynamic_cast with fallback.
  - **Edge cases with unknown duration** (Low):
    - `VideoFileLoaderModule` may have `totalDurationMs == 0` initially.
    - Mitigation: Defer range-aware behavior until duration is known; fall back to existing behaviors early.

---

### 5. Difficulty Levels & Implementation Paths

- **Level 1 – Minimal behavior fix (Low difficulty)**:
  - Without changing any public API:
    - Use `getTimelinePositionSeconds()` / `getTimelineDurationSeconds()` as before.
    - Only **clamp drawing of keyframes** to `[IN, OUT]` range obtained via a simple helper or by logging/inspecting behavior.
    - Do not change stored `timeSeconds` or UI layout significantly.
  - Pros:
    - Quick and safe.
    - Few moving parts.
  - Cons:
    - Impacts might still have persistence behavior that “straddles” the boundary in subtle ways.

- **Level 2 – Full range-aware rendering & UI (Medium difficulty)**:
  - Implement the full plan in sections 3.1–3.4:
    - Add range info to `SourceTimelineState`.
    - Implement range-local logic in both drawing and UI.
  - Pros:
    - UX is consistent and clear.
    - Future features (e.g. multiple clips) benefit from the abstraction.
  - Cons:
    - More code changes; more integration points.

- **Level 3 – Domain abstraction & versioned presets (Higher difficulty)**:
  - Add a reusable “clip domain”/timeline abstraction and explicit state versioning.
  - Extend more modules to use that abstraction.
  - Pros:
    - Architectural cleanliness, easier future extensions (multiple ranges, per-track ranges).
  - Cons:
    - Larger refactor, higher immediate risk, more testing required.

---

### 6. Confidence Assessment

- **Confidence rating**: **High (≈ 80–90%)** that the approach will:
  - Keep behavior backwards-compatible (especially under Policy C).
  - Provide intuitive range-aware drawing once wired into rendering and UI.

- **Strong points**:
  - **Leverages existing timeline interface** in `VideoFileLoaderModule`:
    - No need to redesign fundamental time reporting, just extend with range metadata.
  - **Incremental approach**:
    - We can start with minimal clamping and build up to full range-aware UI without breaking the existing pipeline.
  - **Graceful fallback**:
    - Non-timeline or non-`VideoFileLoaderModule` sources still work as before.

- **Weak points / uncertainties**:
  - **Exact behavior of `getTimelinePositionSeconds()` vs IN/OUT**:
    - It is not fully clear from the current code whether position is already clipped to the range or not.
    - Requires verification; if it is clipped, we must avoid double-clamping.
  - **Runtime ordering and availability of range info**:
    - At load time, duration and IN/OUT values may not be resolvable yet in the draw module.
    - Requires careful lazy initialization and maybe a one-time migration step when the source becomes available.

---

### 7. Potential Problems & Mitigations (Checklist)

- **Problem**: Keyframes created when no source or no duration is available later behave oddly when the source is connected.
  - **Mitigation**: Tag such keyframes as “pre-source”; once `getSourceTimelineState().isValid` with duration and range, re-interpret them in a consistent way (e.g. treat their `timeSeconds` as fractional offsets of current loop and anchor them).

- **Problem**: Loop boundary causes visible “popping” of impacts as range wrap mapping kicks in.
  - **Mitigation**: Use consistent wrap logic both in `VideoFileLoaderModule` and in `VideoDrawImpactModuleProcessor` (e.g. modular arithmetic on `[R0,R1)`), and ensure persistence windows cannot produce contradictory draw decisions just before/after the loop.

- **Problem**: User narrows range so much that persistence is effectively longer than the window, leading to “always on” impacts.
  - **Mitigation**: Clamp `persistenceSeconds` to the effective range length when range is valid.

- **Problem**: Multiple video sources or non-standard sources feeding the draw module.
  - **Mitigation**: Range logic only activates when upstream module is a known timeline provider with range support; otherwise, fallback stays unchanged.

- **Problem**: Preset saved in one range context, loaded in another (e.g. same video but different IN/OUT).
  - **Mitigation**: For first iteration, accept that keyframes are tied to absolute media time; document this. If needed later, implement proportional remapping with explicit preset versioning.

---

### 8. Summary of Next Implementation Steps (When Coding)

- **Step 1**: Extend `SourceTimelineState` to include `rangeStartSeconds`, `rangeEndSeconds`, plus helpers.
- **Step 2**: Add minimal, read-only range accessor(s) to `VideoFileLoaderModule`, or otherwise derive range via normalized IN/OUT and duration.
- **Step 3**: Update `VideoDrawImpactModuleProcessor::getSourceTimelineState()` to populate range info when the upstream module is a `VideoFileLoaderModule` (or equivalent).
- **Step 4**: Make `drawStrokesOnFrame()` compute range-local `currentTime` and respect IN/OUT when deciding which keyframes to draw.
- **Step 5**: Adjust timeline UI in `drawParametersInNode()` to use `[rangeStartSeconds, rangeEndSeconds]` as display domain when available.
- **Step 6**: Add regression tests and manual test scenarios covering looping, narrow ranges, runtime range changes, and non-timeline sources.

