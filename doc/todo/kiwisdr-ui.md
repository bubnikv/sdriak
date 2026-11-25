# KiwiSDR Map Selector — UI/UX Review & Improvements

Usability review of the KiwiSDR map selector on desktop and Android (touch),
covering touch handling, tooltips, and scaling on small screens.

Relevant files:
- `core/src/gui/remote/browser.cpp` / `.h` — remote-receiver browser: markers, cluster picker, panel (was the KiwiSDR map selector)
- `core/src/gui/remote/geomap.cpp` / `.h` — map widget: pan, zoom, tooltips, hit-test
- `core/src/gui/widgets/simple_widgets.h` — `doFingerButton`, `doOverlayText`, overlay styles

---

## Overall assessment

The design is thoughtful and clearly built for both input models. There is real
care in the touch adaptations (marker hit-widening, cluster picker,
footer-anchored Test button, hover-tooltip disabled on Android). Core
interactions — pan, wheel/pinch zoom, click-to-select, overlapping-marker
disambiguation — are all covered, and the z-ordering / hit-test sharing between
draw and click is correct.

---

## High-impact issues

### 1. Control-button row overflows and becomes unreachable on narrow screens
The map controls are laid out on a single `SameLine` row inside the child window:

`Zoom In` · `Zoom Out` · `Reset Map` · `Show EXT API only` (filter) · `Hide full servers` (marker style)

These are `doFingerButton`s (each width = text + one font-height;
`geomap.cpp:516-534` + `kiwisdr_map.cpp:133-139`). The child is created with
flags `0` (`kiwisdr_map.cpp:101`) — **no horizontal scrollbar** — and ImGui
`SameLine` never wraps. On a phone in portrait (or any narrow window), the row
is far wider than the viewport, so `Reset Map`, the filter toggle, and the
marker-style toggle get clipped past the right edge and **cannot be clicked at
all**. This is the most serious problem: on exactly the small screens the code
otherwise works hard to support, three of the five controls are unreachable. The
verbose action-style labels ("Show EXT API only", "Hide full servers", "Show
full as red") make it worse.

**Fix options:** let the row wrap (measure width, drop to a second line),
shorten to icons/short labels, or move the filter + marker-style toggles into an
overflow "⋯"/settings menu.

### 2. No legend for marker colors
Markers encode meaning in color — green/gray/dark-gray by SNR
(`markerFillForSnr`), red = full, violet = extended freq / VHF-capable
(`drawMarkers`, `kiwisdr_map.cpp:320-328`) — but nothing on screen explains
this. A user cannot know green means "good SNR" or violet means "VHF-capable"
without reading the source. Add a small (collapsible) legend, or fold it into
the tooltip/selection panel.

This compounds an **accessibility** problem: StatusColored puts "good SNR"
(green) and "full" (red) as the two most important states — the classic
red/green confusion pair (~8% of male ham operators). Add a shape/border cue for
"full" rather than relying on hue alone.

---

## Medium-impact issues

### 3. Tall button row eats the top strip of the map
The control row is `3× font-height` tall (`getFingerButtonHeight`) and painted
over the top-left of the map. On a short phone-landscape screen that is a big
fraction of the map's vertical space, and it permanently occludes
markers/countries in the top-left (northern Canada, Greenland, anything panned
up there). The selection info panel (`drawSelectionPanel`, up to ~11
`doOverlayText` lines + Test button) stacks down the left edge over the map too.
Together they cover a substantial L-shaped region. Consider a more compact
toolbar or a collapsible info panel.

### 4. Selection panel can overflow a short screen; overlay text isn't scrollable with the map
`drawSelectionPanel` emits up to ~11 lines. On phone-landscape the code
mitigates only the Test button (moves it to the footer,
`kiwisdr_map.cpp:167,207-217`); the info lines themselves can still run long.
The map is drawn at a fixed `recentCanvasPos` each frame, so if the child
scrolls, text and map decouple. Verify on a short screen; consider capping/eliding
the info block (hide URL / less-critical fields, or a two-column layout).

### 5. No text search / filter by name or location
With hundreds of KiwiSDR servers, finding a known station means visually hunting
on the map. A name/location filter box would be a large win, especially on touch
where panning to a dense region and disambiguating is tedious.

---

## Low-impact / touch-specific notes

- **Marker touch target is reasonable but fixed to font size.** On Android the
  hit half-extent is `sz` (full font-height), i.e. a target ~2× the visible
  marker (`markersAtScreenPos`, `kiwisdr_map.cpp:252-256`). Sensible, but tied to
  font size rather than a physical minimum (~9mm/48dp); on a high-DPI phone with
  small font scaling it could fall below a comfortable finger size. Consider
  `max(2*sz, fingerMinPx)`.
- **No double-tap-to-zoom.** Pinch works (via `MouseWheelH` folding,
  `geomap.cpp:458-464`) and zoom buttons exist, but double-tap zoom is the
  expected touch-map idiom and is absent.
- **Selection fires on press, not release** (`handleHitTest` uses
  `IsMouseClicked`). Can't abort a mis-tap by sliding off before lifting. Minor,
  and it's what enables clean "tap selects, doesn't pan"; a small drag threshold
  would feel more forgiving.
- **Potential pinch/pan interaction on Android:** during a two-finger pinch the
  backend parks the cursor at the centroid and drives zoom via `MouseWheelH`. If
  the backend also reports button-down during the gesture, the drag-to-pan block
  (`geomap.cpp:488`) could start a pan simultaneously. Verify on a real device
  that pinch doesn't also pan.
- **Tablet-sized Android gets a fixed 75% popup** with no resize and no maximize
  (maximize is desktop-only, `kiwisdr_map.cpp:181-200`; `shouldUseFullscreenPopup`
  only trips below `56×36` font-units). Not broken, just constrained.
- **Fullscreen popup has no title bar** (`ImGuiWindowFlags_NoTitleBar`), so the
  only exit is the `Cancel` button in the footer. No Android back-gesture
  handling — ensure Cancel is always visible on the shortest supported screen.

---

## What's done well (preserve)

- Shared `markersAtScreenPos` between draw, hover-tooltip, and click hit-test —
  the three can't disagree about what's clickable.
- Cluster picker for overlapping markers instead of guessing the topmost;
  capped-and-scrolled at 8 rows, keyed by URL to survive the selection-reorder.
- Hover tooltip correctly disabled on Android (finger occlusion); discovery
  routed through the selection panel.
- Pan/zoom pivot handling (cursor-anchored wheel/pinch, centre-anchored buttons)
  is correct.
- `isServerVisible` gates hit-test so hidden "full" servers can't be
  clicked-through.

---

## Suggested priority

1. **#1 — unreachable controls on narrow screens** (functional break on the
   targeted small-screen case).
2. **#2 — missing color legend** (+ red/green accessibility cue).
3. **#5 — name/location filter** (biggest workflow win for large server lists).
