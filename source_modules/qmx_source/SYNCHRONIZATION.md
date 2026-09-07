# QMX Source Module — Frequency & Mode Synchronization

## Overview

The `qmx_source` module bridges a QRP Labs QMX transceiver with SDRIAK.
QMX provides IQ audio samples (via USB audio) and a Kenwood TS-480–style
CAT control interface (via USB CDC serial).  The module must keep three
frequency domains in agreement:

| Domain | Description |
|--------|-------------|
| **QMX rig frequency** | The frequency reported by QMX via CAT (`IF;`, `FA;`). This is the "radio dial" frequency. |
| **IQ center frequency** | The center of the SDRIAK waterfall / IQ stream. Offset from the rig frequency by a fixed IF offset (12 kHz) plus a mode-dependent CW offset. |
| **SDRIAK VFO frequency** | The frequency of the currently selected SDRIAK demodulator VFO. Only relevant when "Sync VFO" is enabled. |

The relationship between rig and IQ center is:

```
iq_center = rig_freq - offset(status)
```

where

```
offset(status) = 12000                            (SSB, DIGI, AM)
               = 12000 + cw_offset                (CW)
               = 12000 - cw_offset                (CW-R)
```

`cw_offset` defaults to 700 Hz if not yet polled from the QMX menu.

The inverse:

```
rig_freq = round(iq_center + offset(status))
```

When RIT is enabled, the effective receive frequency is:

```
effective_rig_freq = rig_freq + rit_hz
```

See `qmxRigToIqOffset()`, `rigFrequencyToCenterFrequency()`,
`centerFrequencyToRigFrequency()`, `effectiveReceiveRigFrequency()` in
`main.cpp`.

---

## Threading Model

```
┌─────────────────────────────┐
│  GUI thread (ImGui)         │
│                             │
│  menuHandler()              │  ← SDRIAK calls per frame when source panel visible
│  frameDraw()                │  ← SDRIAK calls every frame unconditionally
│    ├─ syncSdrToQmx()        │
│    ├─ syncVfoToQmx()        │
│    └─ applyPendingStatus()  │
│                             │
│  tune()                     │  ← SDRIAK calls when IQ center changes
│                             │
└─────────────────────────────┘
           ▲ pendingStatus (mutex-protected)
           │
┌──────────┴──────────────────┐
│  CatPoller thread           │
│                             │
│  pollLoop()                 │
│    ├─ drain command queue   │  ← enqueueCommand() from GUI thread
│    ├─ sendCommand()         │
│    ├─ readResponsesFor()    │
│    └─ publishBatch()        │
│         └─ statusHandler()  │  → writes pendingStatus
│                             │
└─────────────────────────────┘
```

**Key invariant:** Only the CatPoller thread touches the serial port
(`sendCommand` / `readBytes`).  The GUI thread sends commands by
enqueuing them via `CatPoller::enqueueCommand()`, which returns a
`std::future<bool>` that resolves once the poller thread has sent the
command.  This eliminates all locking on the serial handle.

---

## CAT Polling

`CatPoller::pollLoop()` runs continuously on its own thread while streaming:

| Command | Interval | Replies | Purpose |
|---------|----------|---------|---------|
| `IF;` | 100 ms | 1 | Primary status: freq, RIT, TX, mode, VFO, split |
| `SM;` or `PC;SW;` | 250 ms | 1 or 2 | S-meter (RX) or power+SWR (TX) |
| `FA;FB;FT;` | 1 s | 3 | VFO A/B frequencies, TX VFO |
| `MMCW\|CW offset;` | 5 s | 1 | CW offset from QMX menu |

Responses are parsed by `QmxCatStatusParser` into a sticky `QmxStatus`
struct.  Fields are never cleared between poll cycles — once a field is
learned (e.g., mode from the first IF response), it persists until
overwritten by a subsequent response.  `beginBatch` / `publishBatch`
group responses per poll cycle; a status callback fires per batch if
anything changed or an IF response was seen.

### CAT Setter Commands

Setter commands (`FA<freq>;`, `MD<mode>;`, `Q9<0|1>;`) produce **no
response** from QMX when well-formed.  A malformed command returns `?;`.
Because setters produce no response, the poller drains the command queue
and sends them without adjusting `expectedReplies`.

---

## Synchronization Modes

A `syncVfo` config flag (exposed as a "Sync VFO" checkbox) selects
between two modes.

### Mode 1: IQ Center Sync Only (`syncVfo = false`)

This is the basic mode.  Only the IQ center frequency is kept in sync
between SDRIAK and QMX.  SDRIAK VFO position and radio mode are
independent.

#### SDRIAK → QMX (`syncSdrToQmx`)

Runs every GUI frame.  If the current SDRIAK IQ center frequency (`freq`),
converted to a rig frequency, differs from the QMX reported rig
frequency, send `FA<newfreq>;` to QMX.

```
rigFreqFromSdr = centerFrequencyToRigFrequency(freq, currentStatus)
rigFreqFromQmx = effectiveReceiveRigFrequency(currentStatus)
if rigFreqFromSdr != rigFreqFromQmx → device.setFrequency(rigFreqFromSdr)
```

After sending, `patchPendingFrequency()` overwrites the pending status
frequency to prevent `applyPendingStatus()` from bouncing the stale QMX
frequency back before the next poll confirms the change.

This function is **skipped** when `syncVfo` is on (VFO sync handles it
instead).

#### QMX → SDRIAK (`applyPendingStatus`)

When new status arrives from the poller and `syncVfo` is off:

```
centerFrequency = rigFrequencyToCenterFrequency(effectiveRigFreq, status)
if llround(freq) != llround(centerFrequency) → tuner::tune(TUNER_MODE_IQ_ONLY, "", centerFrequency)
```

`TUNER_MODE_IQ_ONLY` updates the waterfall center and calls the source
`tune` callback, which updates `freq`.  The `llround` comparison
prevents feedback loops from sub-Hz floating-point discrepancies.

#### `tune()` callback

Called by SDRIAK whenever the IQ center frequency changes (user drag,
`tuner::tune`, etc.).  Always stores the new `freq`.  In mode 1 this is
sufficient — `syncSdrToQmx()` on the next frame will push it to QMX.

### Mode 2: VFO + Mode Sync (`syncVfo = true`)

Extends Mode 1 with bidirectional VFO frequency and radio mode
synchronization between QMX and the currently selected SDRIAK radio VFO.

#### SDRIAK VFO → QMX (`syncVfoToQmx`)

Runs every GUI frame when `syncVfo` is on.

**Frequency:** Computes the absolute frequency of the selected SDRIAK VFO
(`waterfall center + VFO offset`) and compares to the QMX rig frequency.
If they differ, sends `FA<vfoRigFreq>;`.

**Mode:** Reads the SDRIAK radio module's current demod mode via
`core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_GET_MODE)`.
Maps it to a QMX mode.  If it differs from `currentStatus.mode`, sends
`MD<mode>;`.

#### QMX → SDRIAK VFO (`applyPendingStatus`, syncVfo branch)

When new status arrives and `syncVfo` is on:

1. **IQ center:** Same as Mode 1 — update via `TUNER_MODE_IQ_ONLY`.
2. **VFO position:** Move the selected VFO to the QMX rig frequency via
   `tuner::tune(TUNER_MODE_NORMAL, vfoName, rigFreq)`.  This handles
   waterfall scrolling and VFO repositioning.
3. **Mode:** Read QMX mode, map to `RADIO_IFACE_MODE_*`, set via
   `core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE)`.

#### Mode Mapping

| QMX Mode | Radio Interface Mode |
|----------|---------------------|
| `LSB` | `RADIO_IFACE_MODE_LSB` |
| `USB` | `RADIO_IFACE_MODE_USB` |
| `CW` | `RADIO_IFACE_MODE_CW` |
| `CW-R` | `RADIO_IFACE_MODE_CWR` |
| `AM` | `RADIO_IFACE_MODE_AM` |
| `DIGI`, `FM` | Not mapped (ignored) |

QMX firmware implements AM, so it is a full peer of the SSB/CW modes in both
directions: `MD5;` is sent for it, and an `MD5` / `IF` response switches the
SDRIAK radio to AM.  `FM` is still only parsed for Kenwood MD compatibility —
QMX has no FM mode to switch to, so `encodeModeCommand()` refuses it.

In AM the QMX dial frequency is the carrier, which is also where the SDRIAK AM
demodulator places its VFO, so AM uses the plain 12 kHz IF offset with no
mode-specific correction.

---

## Feedback Loop Prevention

Bidirectional sync creates a classic feedback loop risk:

```
SDRIAK sets freq → QMX echoes back → SDRIAK re-applies → ...
```

### Mechanisms

1. **Integer comparison with `std::llround()`.**  The IQ center frequency
   is a `double`.  The rig frequency is an `int64_t`.  The round-trip
   `double → int64 → double` can introduce sub-Hz discrepancies.
   Comparing with `llround()` truncates these to 1 Hz resolution, which
   is the QMX's precision.

2. **`patchPendingFrequency()`.**  After sending a frequency to QMX,
   immediately overwrite the pending status frequency (under
   `statusMutex`).  This prevents `applyPendingStatus()` from seeing a
   stale polled frequency and bouncing it back as an IQ center change
   before the next poll cycle confirms the new value.

3. **`lastRigFreqSentToQmx` / `lastModeSentToQmx`.**  When
   `applyPendingStatus()` pushes a QMX frequency/mode to the SDRIAK VFO,
   it records the value.  `syncVfoToQmx()` skips sending to QMX if the
   VFO value matches what was just received.  This breaks the
   QMX→SDRIAK→QMX echo.

4. **Skipping during TX.**  All sync is suppressed while
   `currentStatus.transmit` is true to avoid disturbing an active
   transmission.

5. **`syncSdrToQmx()` is disabled when `syncVfo` is on.**  VFO sync
   (`syncVfoToQmx`) handles the SDRIAK→QMX direction instead, using the
   VFO frequency rather than the IQ center.  Running both would conflict.

### Steady-State Convergence

In the steady state with no user action:

- `syncSdrToQmx` / `syncVfoToQmx` see matching frequencies → no-op.
- `applyPendingStatus` receives a poll with the same frequency →
  `llround` comparison matches → no-op.
- No commands are sent, no tuner calls are made.

---

## Execution Points

All sync functions run on the **GUI thread** only.  They are called from
two places:

| Call site | When |
|-----------|------|
| `menuHandler()` | Every frame when the QMX source panel is visible |
| `frameDraw()` | Every frame unconditionally (bound to `onFrameDraw`) |

Both call the same sequence: `syncSdrToQmx()` → `syncVfoToQmx()` →
`applyPendingStatus()`.  This means sync runs at ImGui frame rate
(typically 60 fps) even when the source panel is not visible.

The `tune()` callback may be called from any context that invokes
`sigpath::sourceManager.tune()`.  It only stores `freq` and sets a flag;
it does not touch the serial port.

---

## SDRIAK APIs Used

| API | Purpose |
|-----|---------|
| `tuner::tune(TUNER_MODE_IQ_ONLY, "", freq)` | Set IQ center frequency without moving VFOs |
| `tuner::tune(TUNER_MODE_NORMAL, vfoName, freq)` | Set VFO to absolute frequency, scrolling waterfall if needed |
| `gui::waterfall.selectedVFO` | Name of the currently selected VFO |
| `gui::waterfall.getCenterFrequency()` | Current IQ center frequency |
| `sigpath::vfoManager.getOffset(name)` | VFO offset from waterfall center |
| `sigpath::vfoManager.vfoExists(name)` | Check if VFO still exists |
| `core::modComManager.getModuleName(name)` | Get module type for a VFO (e.g., `"radio"`) |
| `core::modComManager.callInterface(name, cmd, in, out)` | Call radio module to get/set mode |

---

## Configuration

Persisted in `qmx_source_config.json`:

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `frequency` | `double` | `7000000.0` | Last IQ center frequency |
| `syncVfo` | `bool` | `false` | Enable VFO + mode sync |
| `audioDevice` | `string` | `""` | Selected audio device (desktop) |
| `serialPort` | `string` | `""` | Selected CAT serial port (desktop) |
| `device` | `string` | `""` | Selected USB device (Android) |

---

## File Map

| File | Role |
|------|------|
| `src/main.cpp` | SDRIAK module: UI, sync logic, sample forwarding |
| `libqmx/src/CatPoller.h/.cpp` | CAT poll loop, command queue, response batching |
| `libqmx/src/QmxCatStatus.h/.cpp` | CAT response parser, sticky `QmxStatus` accumulation |
| `libqmx/src/SerialCat.h/.cpp` | Serial port transport (`CatTransport` impl for desktop) |
| `libqmx/include/qmx/QmxDevice.h` | Public API: `QmxDevice`, `QmxStatus`, `QmxMode` |
