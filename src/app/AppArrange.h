#pragma once
// Arrangement (timeline) panel state, editing, rendering and terminal PDC.
// Split out of AppShared.h so ArrangeModel.h, NodeViewport.h and, through it,
// Geometry3DNodes.h are not in every app/ TU
// (docs/plans/compile-times/include-split.md, Phase 3).
#include "app/AppShared.h"
#include "arrange/ArrangeMediaImport.h"
#include "arrange/ArrangeModel.h"
#include "audio/CompensationDelay.h"
#include "core/NodeViewport.h"

namespace app
{
extern bool  gArrangePanelOpen;

extern float gArrangePanelWidth;

extern float gArrangePanelHeight;


   inline const float kArrangePanelMinWidth = 300.0f;


   inline const float kArrangePanelMinHeight = 140.0f;

extern float gArrangePixelsPerBeat;

extern double gArrangeScrollBeats;


   inline const float kArrangeMinPixelsPerBeat = 5.0f;


   inline const float kArrangeMaxPixelsPerBeat = 500.0f;

extern bool  gArrangeViewportOnRight;

extern bool  gArrangeShowViewport;

extern int   gArrangeLastSnapDivision;

extern bool  gArrangeScrubbing;

extern int64_t gArrangeScrubTick;

extern ImGuiMouseButton gArrangeScrubButton;

extern uint64_t gArrangeMarkerDragId;

extern int64_t gArrangeMarkerDragGrabTick;

extern int64_t gArrangeMarkerDragOrigPos;

extern uint64_t gArrangeRenamingMarkerId;

extern char gArrangeRenameMarkerBuffer[64];

extern uint64_t gArrangeCtxMarkerId;

extern bool   gArrangeShiftDraggingLoop;

extern int64_t gArrangeLoopDragAnchorTick;


   enum ArrangeLoopDragMode {
      kArrangeLoopDragNone = 0,
      kArrangeLoopDragStart, // dragging left border
      kArrangeLoopDragEnd,   // dragging right border
      kArrangeLoopDragMove   // dragging header / entire loop
   };

extern ArrangeLoopDragMode gArrangeLoopDragMode;

extern Arrange::Tick gArrangeLoopDragOrigStart;

extern Arrange::Tick gArrangeLoopDragOrigEnd;

extern Arrange::Tick gArrangeLoopDragGrabTick;


   // Arrangement tools (Select A, Trim T, Range R, Blade B, Zoom Z, Hand H, Pencil P)
   enum class ArrangeTool {
      Select = 0, // A - Standard selection, move, and edge-trim
      Trim,       // T - Focused trim / slip tool
      Range,      // R - Range / marquee selection tool
      Blade,      // B - Cut / split tool
      Zoom,       // Z - Zoom tool (click in, Alt-click out, drag scrub)
      Hand,       // H - Hand / pan tool (drag canvas to pan & scroll)
      Pencil      // P - Pencil / draw tool (click or drag to spawn new unassigned clip)
   };

extern ArrangeTool gArrangeTool;

extern bool  gArrangeBladeOn;

extern bool  gArrangeHandDragging;

extern bool  gArrangeZoomDragging;

extern ImVec2 gArrangeZoomDragStart;

extern float  gArrangeZoomDragStartPpb;

extern double gArrangeZoomDragStartBeats;

extern uint64_t gArrangeRevealClipId;

extern int      gArrangeRevealFrames;

extern uint64_t gArrangeFlashClipId;

extern double   gArrangeFlashStart;

extern bool  gArrangeClaimedKeys;

extern bool  gArrangeFocused;


extern int   gArrangeAddTrackInsertAfter;

extern ImVec2 gArrangePanelRectMin;

extern ImVec2 gArrangePanelRectMax;

extern Arrange::Model gArrange;

extern uint64_t gArrangePatchGeneration;

int ArrangePanelDock();

extern std::set<uint64_t> gArrangeSel;

extern uint64_t gArrangeSelAnchor;

extern uint64_t gArrangeSelGeneration;

extern std::set<uint64_t> gArrangeRowSel;

extern uint64_t gArrangeRowSelAnchor;


   // Copy/paste clipboard: clips by value, with each clip's lane and tick
   // offset relative to the copied block's first lane / earliest start.
   struct ArrangeClipboardItem
   {
      Arrange::Clip clip;
      int laneOffset = 0;
      int laneType = Arrange::kLaneVideo;
      Arrange::Tick tickOffset = 0;
   };


   struct ArrangeClipboardData
   {
      std::vector<ArrangeClipboardItem> items;
      uint64_t generation = 0;
   };

extern ArrangeClipboardData gArrangeClipboard;

extern Arrange::Model gArrangeGestureBefore;

extern bool gArrangeGestureOpen;


   // Live clip drag. Every frame the model is rebuilt from the gesture
   // snapshot plus the current (delta, laneDelta) through the same op the
   // release would run, so what is drawn is exactly what the drop will do.
   enum ArrangeDragMode
   {
      kArrangeDragNone = 0,
      kArrangeDragMove,
      kArrangeDragTrimStart,
      kArrangeDragTrimEnd,
      kArrangeDragGroupEdge,   // TrimGroupEdge: only members flush with the edge
      kArrangeDragGroupScale,  // ScaleGroup: Shift-drag on a group edge
   };


   struct ArrangeDragState
   {
      int mode = kArrangeDragNone;
      uint64_t clipId = 0;      // the clip under the mouse at mouse-down
      uint64_t groupId = 0;     // group edge / scale modes
      int edge = Arrange::kEdgeStart;
      std::vector<uint64_t> ids; // move mode: every clip that moves
      Arrange::Tick grabTick = 0; // mouse tick at mouse-down
      Arrange::Tick origStart = 0, origEnd = 0; // the grabbed clip (or group) at mouse-down
      int grabLane = 0;
      Arrange::Tick appliedDelta = 0; // what the model currently reflects
      int appliedLaneDelta = 0;
      Arrange::Tick appliedTick = -1;
      bool live = false;          // past the drag threshold; until then nothing moves
      bool collapseOnClick = false; // plain click on an already-selected clip: a
                                    // release without a drag narrows the selection to it
      bool singleMember = false;  // Alt held at mouse-down
   };

extern ArrangeDragState gArrangeDrag;


   // Shift+drag rectangle-select / Range Tool (R). Screen-space corners (updated
   // live as the mouse moves) plus the selection captured at mouse-down.
   struct ArrangeMarqueeState
   {
      bool active = false;
      bool allTracks = false; // Started from ruler/header -> spans all tracks vertically
      int  startLane = -1;    // -1 if all tracks or not track-bounded
      ImVec2 anchor{ 0, 0 };
      ImVec2 current{ 0, 0 };
      std::set<uint64_t> baseSel;
   };

extern ArrangeMarqueeState gArrangeMarquee;

extern uint64_t gArrangeRenamingClipId;

extern char     gArrangeRenameClipBuffer[64];

extern uint64_t gArrangeRenamingLaneId;

extern bool     gArrangeRenameJustStarted;

extern bool     gArrangeClipSettingsPanelOpen;

extern uint64_t gArrangeSettingsPanelTarget;

extern float sArrangeLastRulerStartX;

extern uint64_t gArrangeMixGestureLaneId;

extern uint64_t gArrangeCtxClipId;

extern uint64_t gArrangeAssigningClipId;

extern std::vector<uint64_t> gArrangeRenameTargetIds;

extern std::vector<uint64_t> gArrangeAssignTargetIds;


   // One dropped-media-file decode in flight (or about to be), tracking the
   // clip/node already placed in a "loading" state so ArrangePollMediaImports
   // can find them again once Arrange::MediaImportManager finishes decoding.
   // See ArrangeImportMediaFile/ArrangePollMediaImports below.
   struct ArrangePendingImport
   {
      uint64_t jobId = 0;
      uint64_t clipId = 0;
      uint64_t nodeUid = 0;
      Arrange::ImportMediaKind kind = Arrange::ImportMediaKind::Audio;
      // True for a re-decode kicked by ArrangeRespawnCloneNode (paste/
      // duplicate/split of an existing Sample clip) rather than a fresh
      // drop from ArrangeImportMediaFile - the clip already has correct
      // sampleBpm/origBpm/length/sourceDurationSeconds from the original,
      // so ArrangePollMediaImports must not re-estimate/overwrite them,
      // just attach the newly decoded buffer to the clone's own node.
      bool isClone = false;
      // True for the WAV a "Render to track" job just wrote: the clip must
      // stay at native speed (Sync to Tempo off) so it lines up sample for
      // sample with what was rendered - the BPM analysis still fills
      // sampleBpm/origBpm for display, but never switches sync on.
      bool keepNativeSpeed = false;
   };

extern std::vector<ArrangePendingImport> gArrangePendingImports;


   // A browser-panel media drag (gSampleDragActive) released over the
   // Arrange panel rect - stashed here rather than resolved on the spot,
   // since the lane/tick under a screen point is only computable from
   // inside DrawArrangePanelContent's own layout state (scroll, zoom, lane
   // rows). Picked up and cleared on that function's next call, which is at
   // most one frame later - not perceptible for a released drag.
   struct ArrangePendingBrowserDrop
   {
      bool pending = false;
      ImVec2 screenPos { 0.0f, 0.0f };
      std::string path;
   };

extern ArrangePendingBrowserDrop gArrangePendingBrowserDrop;

void ArrangeCommitEdit();


   // Runs `op` against gArrange and, if it changed anything (the revision
   // moved), pushes one timeline undo entry holding the pre-edit model.
   // Returns whether anything changed. The one shape every discrete timeline
   // edit (key, menu item, button) goes through.
   template <class Op>
   bool ArrangeEdit(Op&& op)
   {
      Arrange::Model before = gArrange;
      op();
      if (gArrange.revision == before.revision)
         return false;
      PushArrangeUndoSnapshot(before);
      ArrangeCommitEdit();
      return true;
   }

   // Continuous-gesture undo (drag, popup DragFloat, rename): snapshot at the
   // gesture's start, push at its end only if the revision moved.
   bool ArrangeGestureEnd();

void ArrangeGestureBegin();

bool ArrangeGestureEnd();

void PublishArrangeLoop();

void ArrangeSetLoop(bool enabled, Arrange::Tick start, Arrange::Tick end);


   // View settings that live in the model (so they save with the patch) but
   // are not edits: undo, redo and a drag's snapshot restore all carry the
   // live values across instead of rewinding them (WP5 dockSide, WP6 the
   // display unit and the snap grid).
   struct ArrangeViewSettings
   {
      int  dockSide = 0;
      int  timeDisplay = 0;
      int  snapDivision = 16;
      bool snapTriplet = false;
   };

ArrangeViewSettings ArrangeKeepViewSettings(const Arrange::Model& m);

void ArrangeRestoreViewSettings(Arrange::Model& m, const ArrangeViewSettings& v);

void ArrangeSetTimeDisplay(int mode);

void ArrangeSetSnap(int division, bool triplet);

Arrange::Tick ArrangeSnapGridTicks();

Arrange::Tick ArrangeNudgeStepTicks();

Arrange::Tick ArrangePlayTick();

void ArrangeSeekTick(Arrange::Tick t);

void ArrangeScrubBegin(Arrange::Tick t, ImGuiMouseButton button = ImGuiMouseButton_Left);

void ArrangeScrubUpdate(Arrange::Tick t);

bool ArrangeScrubEnd();

void ArrangeScrubCancel();

Arrange::Tick ArrangeEndKeyTargetTick();

ImU32 ArrangeMarkerColU32(uint32_t rgba);

uint32_t ArrangeMarkerRGBA(ImU32 col);


   inline constexpr uint32_t kArrangeDefaultMarkerRGBA = 0xF59E0BFFu;

 // amber

   // The one colour list the timeline's Color Tint menu and the marker menu
   // both offer. Entry 0 is "no tint" for clips and the default for markers.
   struct ArrangePaletteEntry { const char* name; ImU32 col; };


   inline const ArrangePaletteEntry kArrangePalette[10] = {
      { "Default", IM_COL32(110, 120, 140, 255) },
      { "Crimson", IM_COL32(239, 68, 68, 255) },
      { "Orange",  IM_COL32(249, 115, 22, 255) },
      { "Amber",   IM_COL32(245, 158, 11, 255) },
      { "Emerald", IM_COL32(16, 185, 129, 255) },
      { "Cyan",    IM_COL32(6, 182, 212, 255) },
      { "Blue",    IM_COL32(59, 130, 246, 255) },
      { "Purple",  IM_COL32(139, 92, 246, 255) },
      { "Magenta", IM_COL32(217, 70, 239, 255) },
      { "Rose",    IM_COL32(244, 63, 94, 255) }
   };


   // The snap grids the timeline offers: MusicTime RateDivision entries
   // (names and lengths come from that one table - rhythmic-quantization-
   // standard) mapped onto Settings::snapDivision / snapTriplet. rd -1 = Off.
   // INFINITE_ARRANGEMARKERTEST checks every row against MusicTime::BeatsFor.
   struct ArrangeGridChoice { int rd; int division; bool triplet; };


   inline const ArrangeGridChoice kArrangeGridChoices[10] = {
      { -1, 0, false },
      { MusicTime::k1Bar, 1, false },
      { MusicTime::kHalf, 2, false },          { MusicTime::kHalfTrip, 2, true },
      { MusicTime::kQuarter, 4, false },       { MusicTime::kQuarterTrip, 4, true },
      { MusicTime::kEighth, 8, false },        { MusicTime::kEighthTrip, 8, true },
      { MusicTime::kSixteenth, 16, false },    { MusicTime::kSixteenthTrip, 16, true },
   };

uint64_t ArrangeAddMarkerAtPlayhead();

bool ArrangeJumpToMarker(int dir);

void ArrangeModelToPatchData(const Arrange::Model& m, Patch::Data& data);

void PatchDataToArrangeModel(const Patch::Data& data, Arrange::Model& m,
                                const std::function<uint64_t(int)>& resolveLegacy = {});

void ArrangeRespawnCloneNode(uint64_t clipId);

   // ---- arrangement video compositing (overhaul WP4) ---------------------
   //
   // One pass per active video lane, bottom lane first, so the lane drawn at
   // the TOP of the panel lands in front - the NLE convention (spec §1). Each
   // caller owns an ArrangeCompositeTarget: the live monitor has one, the
   // offline render another. They used to share a single static scratch FBO,
   // which the two resized against each other every frame a render ran with
   // the panel open.

   // A render target's private GL state. `scratch` is the ping-pong pair the
   // lane passes alternate between; `result` is the stable output for a
   // caller that has no FBO of its own to land in (the monitor). `slot` keys
   // this target's own geometry viewports (gArrangeGeomViewports), so two
   // targets at different sizes never share - and thrash - one NodeViewport.
   struct ArrangeCompositeTarget
   {
      int slot = 0;
      GLUtil::Fbo scratch[2];
      GLUtil::Fbo result;
      // `result` from before its last resize. The monitor's texture id goes
      // into the ImGui draw list during the UI pass, and the composite runs
      // after the cook loop but before ImGui::Render - so a resize there
      // would leave the draw list sampling a deleted texture for one frame.
      // Kept one composite longer, then freed.
      GLUtil::Fbo retiredResult;
      // Set by the monitor during the UI pass; consumed by the post-cook
      // composite. 0 = the panel did not draw the monitor this frame.
      int requestW = 0;
      int requestH = 0;
   };

extern ArrangeCompositeTarget gArrangeMonitorTarget;

extern ArrangeCompositeTarget gArrangeRenderTarget;


   // Geometry clips' solo renders. A geometry node has no image of its own
   // (GeometryNode::GetOutputTexture), so a clip of one needs a NodeViewport
   // sized to the composite. These used to borrow gPanelViewports, which the
   // Viewport Panel erases every frame for any node it is not showing - so a
   // geometry clip allocated and freed a full-size FBO every frame (4K per
   // frame during a render). Keyed by (node uid, target slot); an entry
   // unused for kArrangeGeomEvictFrames main-loop frames is dropped by
   // ReapArrangeGeomViewports(). std::map, not unordered: NodeViewport is
   // neither copyable nor movable, and a node-based map never relocates it.
   struct ArrangeGeomViewport
   {
      NodeViewport viewport;
      uint64_t lastUsedFrame = 0;
   };

extern std::map<std::pair<uint64_t, int>, ArrangeGeomViewport> gArrangeGeomViewports;

extern uint64_t gArrangeGeomFrame;


   inline constexpr uint64_t kArrangeGeomEvictFrames = 120;

void ReapArrangeGeomViewports();


   // ---- Live clip waveforms (WP8) --------------------------------------
   // One position-indexed peak cache per audio clip, filled by the audio
   // thread as the clip plays (AudioEngine::ClipPeaks) and drained here.
   // Position-indexed, not streamed: bucket k always means the same slice of
   // the clip, so playing the same bar twice overwrites rather than appends,
   // and scrubbing backwards fills in what was skipped.
   //
   // Never saved and never pre-decoded: a clip's source is a live node, not a
   // file, so there is nothing to read ahead of the playhead. A clip that has
   // not been played yet draws a flat centre line, which is the honest
   // picture of "nothing has come out of this node here yet".
   struct ArrangeClipWave
   {
      // The clip shape this cache was sized and measured for. Any change to
      // these four means the buckets no longer describe what the clip holds,
      // so the cache is cleared - they are exactly the fields WP8 names
      // (src, output, start, length). `start` is in the list because a clip's
      // source is a live node rather than a file: moved two beats later, the
      // clip plays whatever the node emits two beats later, not the material
      // that was measured. Gains and fades are deliberately NOT here - the
      // audio thread measures pre-envelope, pre-gain, so they change how the
      // clip sounds without changing what the buckets describe.
      uint64_t srcUid = 0;
      int      srcOutput = 0;
      Arrange::Tick start = 0;
      Arrange::Tick length = 0;
      // The same four fields hashed, as ArrangeClipShape spells it. Carried
      // into every ClipWindow so a bucket measured under the previous shape
      // and still in flight when an edit lands is dropped on arrival rather
      // than written into the reshaped array at a coincidentally valid index.
      uint64_t shape = 0;
      // Audio Sample static waves only: the remaining inputs the slice
      // depends on, so ArrangeSyncClipVisuals can tell a stale entry from a
      // current one after ANY edit (trim, split, paste, undo, BPM or sync
      // change, a project-tempo change on an unsynced clip).
      double   sampleEffBpm = 0.0;
      float    sampleOffset = 0.0f;
      const Platform::SampleBuffer* sampleBuf = nullptr;
      std::vector<float>   minv;
      std::vector<float>   maxv;
      std::vector<uint8_t> filled;
   };

extern std::unordered_map<uint64_t, ArrangeClipWave> gArrangeClipWaves;

extern std::unordered_map<uint64_t, ArrangeClipWave> gArrangeSampleStaticWaves;


   // Ticks per bucket, the main-thread spelling of kClipPeakBucketsPerBeat.
   inline constexpr Arrange::Tick kArrangeWaveBucketTicks = Arrange::kPPQ / 16;


   // A clip longer than this many buckets (~1 hour at 120 bpm) stops being
   // cached rather than allocating without bound. Nothing in the model caps
   // clip length, so this is a guard, not a policy.
   inline constexpr int kArrangeWaveMaxBuckets = 128 * 1024;

uint64_t ArrangeClipShape(uint64_t srcUid, int srcOutput, Arrange::Tick start, Arrange::Tick length);

int ArrangeWaveBucketCount(Arrange::Tick length);

void ArrangeComputeSampleStaticWave(uint64_t clipId, uint64_t srcUid, int srcOutput,
                                        Arrange::Tick start, Arrange::Tick length,
                                        double effBpm, float sourceOffsetSeconds,
                                        const Platform::SampleBuffer* buf);

double ArrangeSampleEffBpm(const Arrange::Clip& c);

void ArrangeSetSampleSync(uint64_t clipId, bool sync);

void ArrangeSetSampleBpm(uint64_t clipId, float bpm);

void ArrangeDrawSampleTempoInfo(const Arrange::Clip& c);


   // ---- Clip thumbnails (WP8) ------------------------------------------
   // One 96x54 FBO per *video* clip id, blitted from whatever texture the
   // composite already resolved for that clip - so a thumbnail costs one
   // small aspect-fit pass and never a second decode or a second render of
   // the source. Refreshed at most once a second while the clip is active,
   // and immediately after a reassign (lastCapture reset below).
   struct ArrangeClipThumb
   {
      GLUtil::Fbo fbo;
      double   lastCapture = -1.0; // glfwGetTime(); < 0 means "never captured"
      uint64_t srcUid = 0;
      int      srcOutput = 0;
   };

extern std::map<uint64_t, ArrangeClipThumb> gArrangeClipThumbs;


   inline constexpr int kArrangeThumbW = 96;


   inline constexpr int kArrangeThumbH = 54;

void ArrangeSyncClipVisuals();

int CountActiveArrangeVideoClips(double beat, std::string* outFrontTitle = nullptr);

void ArrangeSeekVideoSampleSources(double beat);

unsigned int CompositeArrangeTimelineVideo(ArrangeCompositeTarget& target, GLUtil::Fbo* dest,
                                              double beat, int targetW, int targetH);

void CompositeArrangeMonitorIfRequested();

std::vector<int> ArrangeOutputsOfType(const GraphNode& gn, int laneType);

int ArrangeLaneTypeForNode(const GraphNode& gn);

void ArrangePruneSelection();

std::vector<uint64_t> ArrangeSelectionIds();

void ArrangeClickSelect(uint64_t clipId, bool toggle, bool singleMember);

bool ArrangeCopySelection();

bool ArrangePasteAt(Arrange::Tick atTick);

bool ArrangeDuplicateSelection();

bool ArrangeDeleteSelection();

bool ArrangeSplitSelectionAt(Arrange::Tick tick);

bool ArrangeBladeSplitAt(uint64_t clipId, Arrange::Tick tick);

bool ArrangeToggleEnabledSelection();

bool ArrangeCanGroupSelection();

bool ArrangeCanUngroupSelection();

bool ArrangeGroupSelection();

bool ArrangeUngroupSelection();

bool ArrangeGroupRowSelection();

bool ArrangeUngroupRowSelection();

bool ArrangeDuplicateRowSelection();

bool ArrangeDeleteRowSelection();

bool ArrangeAddTrackShortcut(bool isVideo);

bool ArrangeNudge(int dir);

bool ArrangeRenameSelection();

void ArrangeDragBegin(int mode, uint64_t clipId, int edge, Arrange::Tick grabTick);

bool ArrangeDragUpdate(Arrange::Tick value, int laneDelta);

bool ArrangeDragEnd();

uint64_t AddNodeToArrangeTimeline(int nodeIndex, int laneType = -1, int srcOutput = -1);

bool ArrangeAssignClipSource(uint64_t clipId, uint64_t uid);

// ---- group clip sources (ArrangeTools.cpp) --------------------------------
// A GroupNode is a valid clip source on any lane: its clip draws nothing and
// plays nothing itself, it only gates the group (GroupTimelineWindows).
bool ArrangeIsGroupNode(const GraphNode& gn);
// The lane a node lands on by default: ArrangeLaneTypeForNode, and for a
// group audio if a member makes sound, else video if one makes pictures.
int ArrangeNaturalLaneType(const GraphNode& gn);
// Whether `gn` may be the source of a clip on a lane of `laneType`.
bool ArrangeClipSourceFits(const GraphNode& gn, int laneType);
// Clip label for a source: a group's own label, else NodeTitle.
std::string ArrangeSourceTitle(const GraphNode& gn);
// The group's colour (GroupNode::color) when `uid` is a group.
bool ArrangeGroupSourceColor(uint64_t uid, ImU32& out);
void FrameTest_ARRANGEGROUPTEST(int frameId, GLFWwindow* window);

// ---- MIDI lanes (ArrangeMidi.cpp, ArrangePianoRoll.cpp) --------------------
void FrameTest_ARRANGEMIDITEST(int frameId, GLFWwindow* window);
// The first instrument whose note input is cabled from `producer`, or 0.
uint64_t ArrangeFirstInstrumentOf(const GraphNode& producer);
// The lane's instrument for display: its title, "(no instrument)" or "(missing)".
std::string ArrangeMidiInstrumentLabel(const Arrange::Lane& lane);
// The mini piano roll drawn inside a MIDI clip on the timeline. [clipX0,
// clipX1] is the whole clip's x span (it may be cut by the visible rect).
void ArrangeDrawMidiClipPreview(ImDrawList* dl, const Arrange::Clip& clip, const ImVec2& rectMin, const ImVec2& rectMax,
                                float clipX0, float clipX1, bool muted);
// One-bar empty MIDI clip at `atTick` (one undo step); returns its id or 0.
uint64_t ArrangeAddEmptyMidiClip(uint64_t laneId, Arrange::Tick atTick);
// "Add MIDI Track" submenu: one entry per note instrument on the canvas plus
// "No instrument yet". The new lane lands after lane index `insertAfter` (-1 =
// at the end) in track group `groupId`. One undo step.
void ArrangeDrawAddMidiTrackMenu(int insertAfter, uint64_t groupId);
// Inspector sections: a MIDI lane's instrument picker, a MIDI clip's notes.
void ArrangeDrawMidiLaneInspector(uint64_t laneId, float fieldW);
void ArrangeDrawMidiClipInspector(uint64_t clipId, float fieldW);

// The rate the last render-to-track job ran at (ArrangeRenderToTrack.cpp).
double RenderToTrackSampleRate();

ImU32 ArrangeGroupColor(uint64_t groupId, int alpha = 255);

void DrawArrangeHatch(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float spacing = 7.0f);

std::string ArrangeFormatBBT(Arrange::Tick t);

std::string ArrangeFormatBBTLength(Arrange::Tick t);

std::string ArrangeFormatSeconds(double sec, bool centis = true);

std::string ArrangeFormatTickSeconds(Arrange::Tick t);

std::string ArrangeFormatPos(Arrange::Tick t);

std::string ArrangeFormatLength(Arrange::Tick t);

Arrange::Tick ArrangeParsePos(const char* buf);

Arrange::Tick ArrangeParseLen(const char* buf);


   // ---- inspector typed-value editing --------------------------------------
   // The canvas param widgets have had hover-and-type for a long time
   // (HandleParamTypeHotkeys / gTypedParam*): hover a slider, press a digit,
   // and you are typing, with that digit already in the box. The Arrange
   // inspector's fields did not behave the same way, and the difference was
   // the complaint - ImGui's own TempInput path opens the box on the OLD
   // text, and whether the keystroke that opened it replaces that text or
   // lands beside it depends on frame ordering, which is why typing "0.3"
   // over "0.80" needed a backspace first.
   //
   // So: same approach as the canvas. Seed the box with exactly the typed
   // character and put the cursor after it. One edit can be open at a time,
   // which is true of an inspector by construction.
   struct ArrangeTypedEditState
   {
      ImGuiID id = 0;
      std::string text;
      bool justOpened = false;
      bool pendingInit = false;
      bool noAutoSelect = false;
   };

extern ArrangeTypedEditState gArrangeTypedEdit;

extern int gArrangeFieldHotFrame;

void ArrangeMarkFieldHot();

bool ArrangeFieldHot();


   // Arrangement slider with smooth dragging, double-click to edit text,
   // hover-to-type (starts editing immediately upon typing any number/sign/dot),
   // and standard Ctrl+Click.
   //
   // A plain click's *first* frame can't tell whether a second click is about
   // to follow (ImGui only reports IsMouseDoubleClicked() on the second
   // click), so a single click can't be allowed to immediately SetActiveID +
   // snap the value the way vanilla SliderBehavior does - that snap would
   // commit a spurious edit a frame before the double-click is recognized and
   // opens the text box. Instead a fresh click is held as "pending" until
   // either the mouse drags past the threshold (genuine drag - activate now,
   // from the current position), the double-click window elapses (genuine
   // single click - activate now), or a second click arrives first (genuine
   // double-click - go straight to text input, no drag ever activated).
   struct ArrangeSliderPendingClick
   {
      bool waiting = false;
      double downTime = 0.0;
      ImVec2 downPos = ImVec2(0, 0);
   };

extern std::unordered_map<ImGuiID, ArrangeSliderPendingClick> sArrangeSliderPending;

bool ArrangeSliderFloat(const char* label, float* v, float v_min, float v_max, const char* format = "%.2f", ImGuiSliderFlags flags = 0);


   // A clip tick value, in whichever unit that value is actually spoken in.
   //
   //   Position / Length  follow Settings::timeDisplay - bar.beat.sixteenth
   //                      ("9.3.3" for a position, "1.0.0" for a length) in
   //                      Bars, seconds in Time.
   //   FadeMs             is always milliseconds, whatever the display mode.
   //                      A fade is an envelope, not a place in the song: it
   //                      is chosen by ear in the tens of milliseconds, and
   //                      "0.0.0" gave no way to say 20 of them.
   //
   // Dragging, double-clicking and hover-and-type all work in that same unit,
   // and the typed text is parsed in it - so "9.3.3" in the Start box means
   // bar 9, beat 3, sixteenth 3, and "20" in a Fade box means 20 ms.
   enum class ArrangeTickUnit { Position, Length, FadeMs };

bool ArrangeTickField(const char* label, Arrange::Tick cur, Arrange::Tick lo, Arrange::Tick hi,
                         ArrangeTickUnit unit, float width, Arrange::Tick* out);

bool ArrangeDragFloat(const char* label, float* v, float speed, float v_min, float v_max,
                         const char* format, float width);

Arrange::Tick ArrangeRenderableEndTick();

void ArrangeRenderDetectClipSize(int& outW, int& outH);

int ArrangeRenderVideoClipsInRange(Arrange::Tick a, Arrange::Tick b);

int ArrangeRenderEffectiveAudioSource();

int ArrangeRenderEffectiveVideoSource(Arrange::Tick a, Arrange::Tick b);

bool ArrangeRenderPathQueued(const std::string& path, uint64_t exceptJobId);

void ArrangeRenderResolveRange(int kind, int markerA, int markerB, Arrange::Tick customStart,
                                  Arrange::Tick customEnd, Arrange::Tick& outA, Arrange::Tick& outB);

int ArrangeRenderFrameBudget(double durSec, int fps);

long long ArrangeRenderSampleBudget(double durSec, double rate);

double ArrangeRenderActiveSampleRate();

bool ArrangeMediaKindForPath(const std::string& path, Arrange::ImportMediaKind& outKind);

void ArrangeImportMediaFile(const std::string& path, uint64_t laneId, Arrange::Tick atTick,
                               Arrange::ImportMediaKind kind);

void ArrangeRespawnCloneNode(uint64_t clipId);

void ArrangeSharedSourceTooltip(const char* body);

bool ArrangeMakeClipSourceUnique(uint64_t clipId);

void ArrangePollMediaImports();

std::string ArrangeRenderUniquePath(const std::string& path);

ArrangeRenderJob ArrangeBuildLaneScopedRenderJob(const std::vector<uint64_t>& laneIds, const std::string& baseName);

void ArrangeCommitLaneScopedRenderJob(ArrangeRenderJob job);

void DrawArrangePanelDocked(const char* id, const ImVec2& size);

   // ---- timeline terminal PDC (overhaul WP3) -----------------------------
   //
   // A timeline clip terminal has no AudioCaptureRing to hang its delay-
   // compensation state on, and a value living in the disposable terminal
   // vector is rebuilt from zero every generation - which clicked on every
   // rebuild, and rebuilds used to happen at every clip boundary. So the
   // state lives here instead, keyed by the same (laneId, srcUid, srcOutput)
   // that identifies the terminal, and survives any number of rebuilds.
   //
   // unique_ptr, not a value: the audio thread holds a raw pointer to the
   // CompensationDelay for the life of a generation, so it must not move when
   // the map rehashes. Entries are dropped only once CompletedGeneration()
   // confirms the audio thread has finished with the last topology that
   // referenced them - the same rule gRetiredNodes uses.
   struct ArrangeTerminalComp
   {
      std::unique_ptr<CompensationDelay> delay;
      uint64_t lastUsedGeneration = 0;
      bool usedThisRebuild = false;
   };

extern std::unordered_map<uint64_t, ArrangeTerminalComp> gArrangeTerminalComp;

extern uint64_t gArrangeAudioBuiltRevision;

extern bool gArrangeAudioBuiltRouting;

extern std::set<uint64_t> gArrangeRetriggerConflictClipIds;

extern std::set<uint64_t> gArrangeVideoSourceConflictClipIds;

extern uint64_t gArrangeVideoConflictBuiltRevision;

const std::set<uint64_t>& ArrangeVideoSourceConflictClips();

}
