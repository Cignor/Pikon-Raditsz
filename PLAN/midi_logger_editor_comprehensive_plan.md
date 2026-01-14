# MIDI Logger & Editor Comprehensive Plan

## Executive Summary

This plan outlines the transformation of `MidiLoggerModuleProcessor` into a truly polyvalent MIDI logger and editor, or alternatively, the creation of a dedicated MIDI Editor node. The goal is to provide professional-grade MIDI recording, editing, and playback capabilities within the modular synth environment.

**Decision Point**: Extend existing `MidiLoggerModuleProcessor` vs. Create new `MidiEditorModuleProcessor`

---

## Current State Analysis

### Existing Features (MidiLoggerModuleProcessor)
✅ **Recording**
- Records MIDI from CV inputs (Gate, Pitch, Velocity) per track
- Sample-accurate timing (stores in samples)
- Multi-track support (up to 12 tracks)
- Legato pitch change detection
- Thread-safe event storage with ReadWriteLock

✅ **Playback**
- Playback with looping (configurable loop length in bars)
- CV outputs per track (Gate, Pitch, Velocity)
- MIDI output to VSTi plugins and external devices
- BPM sync from transport

✅ **UI**
- Piano roll visualization with timeline ruler
- Multi-track stacked view
- Note dragging (time position only)
- Right-click erase
- Zoom controls
- Click-to-seek timeline

✅ **File I/O**
- Export to MIDI file (.mid)
- Saves tempo, time signature, track names

### Limitations & Gaps
❌ **Editing Capabilities**
- Can only drag notes horizontally (time position)
- Cannot resize note durations
- Cannot edit pitch/velocity directly
- No copy/paste/duplicate operations
- No quantization tools
- No undo/redo system
- No selection/multi-select
- No grid snapping

❌ **MIDI Features**
- No MIDI CC (Control Change) support
- No pitch bend recording/editing
- No aftertouch support
- No program change
- No MIDI file import (only export)
- No MIDI channel assignment per track

❌ **Advanced Editing**
- No note velocity editing (visual or numeric)
- No note length editing (resize handles)
- No transposition tools
- No scale quantization
- No humanization/randomization
- No pattern/loop editing

❌ **Workflow**
- No undo/redo
- No clipboard operations
- No track duplication
- No track merging/splitting
- No time signature changes
- No tempo changes within sequence

---

## Architecture Decision: Extend vs. New Module

### Option A: Extend MidiLoggerModuleProcessor
**Pros:**
- Preserves existing user workflows
- Single module for recording + editing
- Less code duplication
- Existing state management in place

**Cons:**
- Risk of feature bloat
- Harder to maintain clean separation of concerns
- May confuse users (is it a logger or editor?)
- Performance concerns (always running editor code)

### Option B: Create Separate MidiEditorModuleProcessor
**Pros:**
- Clear separation: Logger = record, Editor = edit
- Can optimize each for its purpose
- Users can have multiple editors for different sequences
- Cleaner architecture
- Can load/edit external MIDI files independently

**Cons:**
- Need to transfer data between modules
- More modules to maintain
- Potential confusion about which to use

### Option C: Hybrid Approach (RECOMMENDED)
**Keep MidiLoggerModuleProcessor** for recording and basic playback
**Create MidiEditorModuleProcessor** for advanced editing

**Workflow:**
1. Record in MidiLogger → Export to .mid
2. Load .mid in MidiEditor → Edit → Export back
3. OR: MidiEditor can directly load from MidiLogger's memory (shared state)

**Benefits:**
- Best of both worlds
- Clear separation of concerns
- Can use editor without recording
- Can edit external MIDI files
- Logger stays lightweight for real-time recording

---

## Feature Requirements

### Phase 1: Core Editing (MVP)
**Priority: CRITICAL**

#### 1.1 Note Editing
- ✅ Drag notes horizontally (already exists)
- 🔨 Drag notes vertically (change pitch)
- 🔨 Resize note duration (drag left/right edge)
- 🔨 Select notes (click, shift-click, drag-select)
- 🔨 Multi-select operations (move, delete, transpose)
- 🔨 Direct pitch/velocity editing (double-click → numeric input)

#### 1.2 Basic Operations
- 🔨 Undo/Redo system (infinite levels with memory limit)
- 🔨 Copy/Cut/Paste (with clipboard)
- 🔨 Delete (keyboard shortcut + context menu)
- 🔨 Duplicate selection
- 🔨 Grid snapping (1/4, 1/8, 1/16, 1/32, 1/64 notes)

#### 1.3 MIDI File Import
- 🔨 Load .mid files (full Standard MIDI File support)
- 🔨 Parse multiple tracks
- 🔨 Preserve tempo changes
- 🔨 Preserve time signature changes
- 🔨 Handle MIDI channels per track

### Phase 2: Advanced Editing
**Priority: HIGH**

#### 2.1 Quantization
- 🔨 Quantize to grid (1/4, 1/8, 1/16, etc.)
- 🔨 Quantize strength (0-100%)
- 🔨 Quantize note starts only vs. note ends
- 🔨 Swing quantization (shuffle feel)

#### 2.2 Transposition & Transformation
- 🔨 Transpose selection (semitones, octaves)
- 🔨 Scale quantization (force notes to scale)
- 🔨 Invert (mirror around root)
- 🔨 Reverse (time-reverse selection)
- 🔨 Stretch/compress time (percentage)

#### 2.3 Velocity Editing
- 🔨 Velocity handles on notes (visual editing)
- 🔨 Velocity curves (draw velocity over time)
- 🔨 Velocity scaling (multiply by factor)
- 🔨 Randomize velocity (with range)

### Phase 3: MIDI Features
**Priority: MEDIUM**

#### 3.1 MIDI CC Support
- 🔨 Record MIDI CC from CV inputs
- 🔨 Display CC lanes in piano roll
- 🔨 Edit CC curves
- 🔨 Per-track CC assignment

#### 3.2 Advanced MIDI
- 🔨 Pitch bend recording/editing
- 🔨 Aftertouch support
- 🔨 Program change events
- 🔨 MIDI channel per track
- 🔨 MIDI channel filtering

#### 3.3 Tempo & Time Signature
- 🔨 Tempo changes within sequence
- 🔨 Time signature changes
- 🔨 Tempo curves (gradual tempo changes)

### Phase 4: Workflow & Polish
**Priority: LOW**

#### 4.1 Pattern Management
- 🔨 Pattern/loop regions
- 🔨 Pattern library
- 🔨 Pattern chaining
- 🔨 Pattern variations

#### 4.2 Humanization
- 🔨 Timing randomization
- 🔨 Velocity randomization
- 🔨 Micro-timing adjustments
- 🔨 Groove templates

#### 4.3 Advanced UI
- 🔨 Keyboard shortcuts (full set)
- 🔨 Context menus (right-click)
- 🔨 Tool palette (pencil, eraser, select, etc.)
- 🔨 Zoom presets
- 🔨 Multi-view (piano roll + list editor)

---

## Technical Architecture

### Data Structure Design

#### Current Structure (MidiLoggerModuleProcessor)
```cpp
struct MidiEvent {
    int pitch;
    float velocity;
    int64_t startTimeInSamples;
    int64_t durationInSamples;
};
```

#### Proposed Enhanced Structure (MidiEditorModuleProcessor)
```cpp
struct MidiNote {
    int pitch = 60;
    int velocity = 100;  // 0-127, not normalized
    double startTime = 0.0;  // In beats/ticks, not samples
    double duration = 0.0;   // In beats/ticks
    int trackIndex = 0;
    int midiChannel = 1;  // 1-16
    juce::Uuid id;  // Unique ID for selection/undo
};

struct MidiCCEvent {
    int ccNumber = 1;  // 0-127
    int value = 0;     // 0-127
    double time = 0.0;
    int trackIndex = 0;
    int midiChannel = 1;
};

struct MidiTrack {
    juce::String name;
    juce::Colour color;
    int midiChannel = 1;
    bool muted = false;
    bool soloed = false;
    
    std::vector<MidiNote> notes;
    std::vector<MidiCCEvent> ccEvents;
    std::vector<PitchBendEvent> pitchBendEvents;
    
    // Thread safety
    mutable juce::ReadWriteLock lock;
};
```

**Key Differences:**
- Time stored in beats/ticks (not samples) for musical precision
- Velocity as integer (0-127) matching MIDI standard
- Unique IDs for undo/redo
- Support for CC, pitch bend, etc.

### Undo/Redo System

#### Command Pattern Implementation
```cpp
class MidiEditCommand {
public:
    virtual ~MidiEditCommand() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual juce::String getDescription() const = 0;
};

class UndoManager {
    std::vector<std::unique_ptr<MidiEditCommand>> undoStack;
    std::vector<std::unique_ptr<MidiEditCommand>> redoStack;
    size_t maxUndoLevels = 100;
    
public:
    void executeCommand(std::unique_ptr<MidiEditCommand> cmd);
    void undo();
    void redo();
    bool canUndo() const;
    bool canRedo() const;
};
```

**Command Types:**
- `AddNoteCommand`
- `DeleteNoteCommand`
- `MoveNoteCommand`
- `ResizeNoteCommand`
- `ChangeVelocityCommand`
- `TransposeCommand`
- `QuantizeCommand`
- `PasteCommand`

### Thread Safety Model

#### Current Model (MidiLogger)
- Audio thread: Writes to `activeNotes` map, then creates `MidiEvent`
- UI thread: Reads via `getEventsCopy()` (ReadWriteLock)
- UI thread: Writes via `setEvents()` (WriteLock)

#### Proposed Model (MidiEditor)
- **Audio thread**: Read-only access for playback
- **UI thread**: Full read/write access for editing
- **Synchronization**: 
  - Double-buffered event lists (swap on commit)
  - Atomic flags for "dirty" state
  - Message queue for audio thread updates

```cpp
class MidiEditorData {
    // Active data (for audio thread playback)
    std::vector<MidiTrack> activeTracks;
    juce::CriticalSection activeLock;
    
    // Editing data (for UI thread)
    std::vector<MidiTrack> editingTracks;
    juce::CriticalSection editingLock;
    
    // Commit changes from editing → active
    void commitChanges() {
        juce::ScopedLock lock1(editingLock);
        juce::ScopedLock lock2(activeLock);
        activeTracks = editingTracks;  // Deep copy
    }
};
```

### MIDI File Format Support

#### Import Requirements
- Standard MIDI File (SMF) format 0, 1
- Multiple tracks
- Tempo meta events
- Time signature meta events
- Track names
- MIDI channels
- Note on/off
- CC events
- Pitch bend
- Program change

#### Export Requirements
- All import features
- Preserve editing metadata (if any)
- Option to export selected tracks only
- Option to merge tracks

### UI Architecture

#### Piano Roll Component
```
┌─────────────────────────────────────────┐
│ Toolbar (tools, zoom, grid, snap)      │
├─────────────────────────────────────────┤
│ Timeline Ruler (bars, beats, ticks)     │
├──────┬──────────────────────────────────┤
│ Piano │ Piano Roll Area                 │
│ Keys  │ - Notes (rectangles)            │
│       │ - CC lanes (curves)            │
│       │ - Selection handles            │
│       │ - Playhead                     │
└──────┴──────────────────────────────────┘
│ Track List (names, mute, solo, color)   │
└─────────────────────────────────────────┘
```

#### Interaction Model
- **Tool Selection**: Pencil, Eraser, Select, Zoom
- **Note Creation**: Click in piano roll → create note
- **Note Editing**: 
  - Click note → select
  - Drag note → move (with grid snap)
  - Drag top/bottom → change pitch
  - Drag left/right edge → resize duration
  - Double-click → edit properties dialog
- **Selection**:
  - Click → single select
  - Shift+Click → add to selection
  - Drag → marquee select
  - Ctrl+A → select all
- **Context Menu**: Right-click → copy/paste/delete/quantize/etc.

---

## Implementation Phases

### Phase 1: Foundation (Weeks 1-2)
**Goal**: Create MidiEditorModuleProcessor with basic structure

**Tasks:**
1. Create `MidiEditorModuleProcessor` class skeleton
2. Implement MIDI file import (using JUCE's MidiFile)
3. Implement basic data structures (MidiNote, MidiTrack)
4. Implement thread-safe data access
5. Basic UI: Piano roll display (read-only)
6. Basic playback: Generate MIDI from edited data

**Deliverables:**
- New module compiles and runs
- Can load .mid file
- Can display notes in piano roll
- Can play back MIDI

**Risk**: LOW
**Confidence**: HIGH (90%) - Well-understood territory

---

### Phase 2: Basic Editing (Weeks 3-4)
**Goal**: Enable note editing operations

**Tasks:**
1. Implement note selection system
2. Implement note dragging (horizontal + vertical)
3. Implement note resize (duration editing)
4. Implement undo/redo system (Command pattern)
5. Add grid snapping
6. Add delete operation

**Deliverables:**
- Can select notes
- Can move notes (pitch + time)
- Can resize notes
- Can delete notes
- Undo/redo works

**Risk**: MEDIUM
**Confidence**: MEDIUM (70%) - Complex interaction handling

**Challenges:**
- ImGui interaction model (click detection, drag handling)
- Selection state management
- Undo/redo performance with large sequences

---

### Phase 3: Advanced Editing (Weeks 5-6)
**Goal**: Add quantization, transposition, copy/paste

**Tasks:**
1. Implement clipboard system
2. Implement copy/cut/paste
3. Implement quantization (grid + strength)
4. Implement transposition
5. Implement scale quantization
6. Add velocity editing (visual handles)

**Deliverables:**
- Copy/paste works
- Quantization works
- Transposition works
- Velocity editing works

**Risk**: MEDIUM
**Confidence**: MEDIUM (65%) - Algorithm complexity

**Challenges:**
- Quantization algorithms (swing, strength)
- Scale quantization (musical theory)
- Clipboard format (cross-module compatibility)

---

### Phase 4: MIDI Features (Weeks 7-8)
**Goal**: Add CC, pitch bend, advanced MIDI

**Tasks:**
1. Add MIDI CC event structure
2. Add CC lane display in piano roll
3. Implement CC curve editing
4. Add pitch bend support
5. Add aftertouch support
6. Per-track MIDI channel assignment

**Deliverables:**
- CC recording/editing works
- Pitch bend works
- MIDI channels per track

**Risk**: MEDIUM-HIGH
**Confidence**: MEDIUM (60%) - Less familiar territory

**Challenges:**
- CC curve visualization/editing
- MIDI channel routing in graph
- Performance with many CC events

---

### Phase 5: Polish & Integration (Weeks 9-10)
**Goal**: Workflow improvements and integration

**Tasks:**
1. Keyboard shortcuts
2. Context menus
3. Tool palette
4. Integration with MidiLogger (import from logger)
5. Performance optimization
6. Documentation

**Deliverables:**
- Professional workflow
- Integrated with existing modules
- Well-documented

**Risk**: LOW
**Confidence**: HIGH (80%) - Mostly UI polish

---

## Risk Assessment

### Technical Risks

#### HIGH RISK
1. **Thread Safety Issues**
   - **Risk**: Audio thread blocking on UI thread locks
   - **Mitigation**: Double-buffering, lock-free where possible, atomic flags
   - **Impact**: Audio dropouts, crashes

2. **Performance with Large Sequences**
   - **Risk**: UI lag with 1000+ notes
   - **Mitigation**: Culling (only render visible notes), spatial indexing, LOD
   - **Impact**: Poor UX, unusable with complex sequences

3. **Undo/Redo Memory Usage**
   - **Risk**: Memory exhaustion with deep undo stack
   - **Mitigation**: Limit undo levels, compress commands, lazy evaluation
   - **Impact**: Memory leaks, crashes

#### MEDIUM RISK
1. **MIDI File Format Compatibility**
   - **Risk**: Some .mid files don't import correctly
   - **Mitigation**: Extensive testing, fallback handling, error reporting
   - **Impact**: User frustration, data loss

2. **ImGui Interaction Complexity**
   - **Risk**: Difficult to implement precise note editing in ImGui
   - **Mitigation**: Custom ImGui extensions, careful event handling
   - **Impact**: Poor editing experience

3. **Integration with Existing Modules**
   - **Risk**: Breaking changes to MidiLogger or graph routing
   - **Mitigation**: Careful API design, backward compatibility
   - **Impact**: Breaking existing workflows

#### LOW RISK
1. **UI/UX Polish**
   - **Risk**: Interface not intuitive
   - **Mitigation**: User testing, iterative design
   - **Impact**: Learning curve, but not blocking

### Business/User Risks

1. **Feature Bloat**
   - **Risk**: Module becomes too complex
   - **Mitigation**: Clear separation (Logger vs Editor), modular features
   - **Impact**: Confusion, maintenance burden

2. **Learning Curve**
   - **Risk**: Users don't understand how to use editor
   - **Mitigation**: Tooltips, documentation, tutorials
   - **Impact**: Low adoption

---

## Difficulty Levels

### Level 1: Foundation (EASY)
- MIDI file import/export
- Basic piano roll display
- Simple playback
- **Estimated Time**: 1-2 weeks
- **Skills Required**: JUCE MIDI API, basic UI

### Level 2: Basic Editing (MEDIUM)
- Note selection
- Note dragging
- Undo/redo
- Grid snapping
- **Estimated Time**: 2-3 weeks
- **Skills Required**: ImGui interaction, state management, algorithms

### Level 3: Advanced Editing (HARD)
- Quantization algorithms
- Scale quantization
- Copy/paste with clipboard
- Velocity editing
- **Estimated Time**: 2-3 weeks
- **Skills Required**: Musical theory, algorithm design, UI/UX

### Level 4: MIDI Features (MEDIUM-HARD)
- CC editing
- Pitch bend
- MIDI channel routing
- **Estimated Time**: 2-3 weeks
- **Skills Required**: MIDI protocol, curve editing, routing

### Level 5: Polish (EASY-MEDIUM)
- Keyboard shortcuts
- Context menus
- Performance optimization
- **Estimated Time**: 1-2 weeks
- **Skills Required**: UI polish, profiling, optimization

**Total Estimated Time**: 8-13 weeks (2-3 months)

---

## Confidence Rating

### Overall Confidence: **MEDIUM-HIGH (72%)**

### Strong Points ✅
1. **Clear Requirements**: Well-defined feature set
2. **Existing Foundation**: MidiLogger provides reference implementation
3. **JUCE Support**: JUCE has excellent MIDI file support
4. **Modular Design**: Can implement incrementally
5. **User Need**: Clear demand for MIDI editing capabilities

### Weak Points ⚠️
1. **ImGui Limitations**: ImGui not designed for complex editing UIs
2. **Thread Safety Complexity**: Audio + UI threading is tricky
3. **Performance Unknowns**: Unclear how ImGui handles 1000+ notes
4. **Algorithm Complexity**: Quantization, scale quantization require musical knowledge
5. **Integration Risk**: Need to ensure compatibility with existing modules

### Mitigation Strategies
1. **Prototype Early**: Build minimal viable editor first, test performance
2. **Incremental Development**: Add features one at a time, test thoroughly
3. **User Testing**: Get feedback early and often
4. **Performance Profiling**: Profile early, optimize bottlenecks
5. **Fallback Plans**: Have simpler alternatives if complex features fail

---

## Alternative Approaches

### Approach A: Minimal Editor (RECOMMENDED FOR MVP)
**Focus**: Core editing only
- Note move/resize
- Basic undo/redo
- Copy/paste
- Grid snapping
- **Time**: 4-6 weeks
- **Risk**: LOW
- **Confidence**: HIGH (85%)

### Approach B: Full-Featured Editor
**Focus**: All features from plan
- Everything in phases 1-5
- **Time**: 10-13 weeks
- **Risk**: MEDIUM-HIGH
- **Confidence**: MEDIUM (65%)

### Approach C: Hybrid (Extend MidiLogger)
**Focus**: Add editing to existing module
- Keep recording, add editing UI
- **Time**: 6-8 weeks
- **Risk**: MEDIUM
- **Confidence**: MEDIUM (70%)

**Recommendation**: Start with Approach A (Minimal Editor), then expand based on user feedback.

---

## Success Criteria

### Must Have (MVP)
✅ Load .mid files
✅ Display piano roll
✅ Edit notes (move, resize, delete)
✅ Undo/redo
✅ Copy/paste
✅ Grid snapping
✅ Export .mid files
✅ Playback to VSTi

### Should Have (v1.0)
✅ Quantization
✅ Transposition
✅ Velocity editing
✅ Multi-select
✅ MIDI CC support
✅ Keyboard shortcuts

### Nice to Have (Future)
✅ Scale quantization
✅ Humanization
✅ Pattern management
✅ Tempo changes
✅ Advanced MIDI (pitch bend, aftertouch)

---

## Questions & Decisions Needed

1. **Architecture**: Extend MidiLogger or create new MidiEditor?
   - **Recommendation**: Create new MidiEditor (cleaner separation)

2. **Time Storage**: Samples vs. Beats/Ticks?
   - **Recommendation**: Beats/Ticks (musical precision, easier editing)

3. **Undo Limit**: How many levels?
   - **Recommendation**: 100 levels, with memory limit (e.g., 50MB)

4. **Performance Target**: How many notes before lag?
   - **Recommendation**: Smooth with 1000 notes, acceptable with 5000

5. **Integration**: How to transfer data from MidiLogger to MidiEditor?
   - **Recommendation**: Export to .mid, then import (simple, reliable)

6. **UI Framework**: Pure ImGui or custom rendering?
   - **Recommendation**: ImGui with custom extensions (piano roll rendering)

---

## Conclusion

This plan provides a comprehensive roadmap for creating a professional MIDI editor within the modular synth environment. The recommended approach is to create a new `MidiEditorModuleProcessor` module, starting with a minimal viable product (MVP) and expanding based on user feedback.

**Key Success Factors:**
1. Incremental development (build → test → iterate)
2. Performance-first mindset (optimize early)
3. User-centric design (get feedback early)
4. Clean architecture (separate concerns)

**Next Steps:**
1. Review and approve plan
2. Create detailed technical specifications for Phase 1
3. Set up development environment
4. Begin implementation

---

**Document Version**: 1.0  
**Last Updated**: 2025-12-12  
**Author**: AI Assistant  
**Status**: DRAFT - Awaiting Review





