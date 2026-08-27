# Crash audit — RampAgent × vSMR

Working tracker for the "EuroScope crashes ~30 min into a session with a vSMR radar
screen displayed and RampAgent loaded" investigation.

| | |
|---|---|
| Opened | 2026-08-27 |
| RampAgent baseline | `04f2338` (v2.0.1) |
| vSMR baseline | `b059223` (v1.0.9) |
| Reported symptom | Hard crash ~30 min in, controlling on a vSMR screen. Did not reproduce after unloading RampAgent — but that session also sat mostly on a CoFrance screen, so the test is confounded. |

## Framing — why it has to be both plugins

RampAgent registers **no display type and no `CRadarScreen`** (no `RegisterDisplayType`,
no `OnRadarScreenCreated` anywhere in `src/`). Everything it runs is screen-independent
and byte-for-byte identical whether vSMR is on top or not. So "same plugin, no vSMR
displayed, no crash" cannot be explained by RampAgent's own code path.

The crash therefore executes inside `CSMRRadar::OnRefresh`. But vSMR alone was fine
before RampAgent existed. The only channels between them are:

1. **Flight strip annotations 3 and 4.** RampAgent writes them
   (`RampAgent.cpp:466`, `:473`); vSMR reads them (`SMRRadar.cpp:1550`, `:1555`).
2. **One direct call.** vSMR's right-click on the stand tag item calls
   `StartTagFunction(..., "RampAgent", 0, ...)` (`SMRRadar.cpp:873`).

Two candidate shapes, both supported by findings below:

- **(A)** vSMR's render path mishandles annotation content it only ever sees when
  RampAgent is loaded. → fix belongs mostly in vSMR.
- **(B)** RampAgent's 1 Hz annotation write storm corrupts EuroScope's own state, and
  the corruption surfaces wherever ES is hammered hardest — i.e. vSMR's `OnRefresh`,
  which makes hundreds of `AddScreenObject` / `FlightPlanSelect` calls per frame,
  far more than a CoFrance screen. → fix belongs mostly in RampAgent.

## How to use this file

Tick the checkbox in the heading when a finding is fixed or explicitly dismissed.
Keep the `Status` line current. Put anything learned in `Notes` — including
"investigated, not the cause", which is just as valuable as a fix.

**Status legend:** `TODO` · `IN PROGRESS` · `FIXED` · `WONTFIX` · `RULED OUT` (verified
not to be the crash, but may still be worth fixing) · `NEEDS INFO`

**Severity** = how bad if it fires. **Crash-suspect** = how likely this *specific*
finding is the reported ~30-minute crash. They are deliberately separate: several
critical bugs are poor fits for the reported timing, and one low-severity leak is a
very good fit.

---

# Part 0 — Diagnosis (do this before writing any fix)

The current evidence is confounded: the no-crash session changed **both** the screen
*and* the plugin. Resolve that first, or you will be fixing blind.

## [ ] D1 — Run the 2×2 matrix

**Status:** TODO

|                       | RampAgent loaded    | RampAgent unloaded |
|-----------------------|---------------------|--------------------|
| **vSMR displayed**    | ← reported crash    | **RUN THIS**       |
| **CoFrance displayed**| **RUN THIS**        | reported OK        |

The top-right cell is decisive. vSMR displayed for a full hour with RampAgent
unloaded: if it still crashes, RampAgent is a bystander and the fix is entirely in
Part 1 (V1 / V3 / V4 / V6 / V7 / V11).

**Notes:**

## [ ] D2 — Watch GDI and USER handle counts

**Status:** TODO

Task Manager → Details tab → right-click column headers → add **GDI objects** and
**USER objects**, watch `EuroScope.exe` with the vSMR screen up. Default per-process
cap is 10,000 for each. A steady climb confirms the leak family (V4, V11, V13) in
about ten minutes with no debugger.

Cheapest possible test. Do this one first.

**Notes:**

## [ ] D3 — Capture a real dump

**Status:** TODO

```
procdump -ma -e -w EuroScope.exe C:\dumps
```

The faulting module plus stack settles hypothesis (A) vs (B) instantly.

**Notes:**

## [ ] D4 — One session under PageHeap

**Status:** TODO

```
gflags /p /enable EuroScope.exe /full
```

If R1c (23-char annotation write) is overflowing an EuroScope-side buffer, full page
heap turns a silent corruption into an immediate, correctly-attributed access
violation. Slow — one session only. Disable afterwards:

```
gflags /p /disable EuroScope.exe
```

**Notes:**

## [ ] D5 — Fastest probe for hypothesis (B)

**Status:** TODO

Ship the affected user a RampAgent build with the annotation truncation clamped to
**10** characters instead of 23 (`RampAgent.cpp:463`, `:471`). If the crash stops,
it is the annotation length and the fix belongs in RampAgent (see R1c).

**Notes:**

## [ ] D6 — Collect missing information

**Status:** NEEDS INFO

- [ ] The affected user's actual `vSMR_Profiles.json` and `.asr`. **V1 is
      config-dependent** — this file confirms or kills the top suspect in two minutes.
- [ ] Hard crash (EuroScope vanishes / WER dialog) or freeze/hang? Does WER name a
      faulting module?
- [ ] Does that user have the DEP LIST / TTT LIST or an approach inset window open?
      Those run a second copy of the tag pipeline in `InsetWindow.cpp` (see V11, V13).
- [ ] Exact versions: EuroScope 3.2.13? vSMR 1.0.9? RampAgent 2.0.1?
- [ ] Did it crash with RampAgent **v1.0.8**? The annotation code is byte-identical
      between v1 and v2 (diffed) — so if v1 was fine, the trigger is the v2
      tag-menu/assign path (R2), not the annotation sweep.
- [ ] Does it still crash if they leave vSMR displayed and do not touch the mouse?
      That isolates V4 and R2 from everything else.
- [ ] Does the crash follow a right-click on the stand field? That points straight
      at R2.
- [ ] Has that user ever run `.smr log`? See V16.
- [ ] Which other plugins are loaded — UK Controller Plugin, vStrips? See X1.

**Notes:**

---

# Part 1 — vSMR (`D:/Dev/vSMR`, baseline `b059223`)

| ID | Finding | Severity | Crash-suspect | Status |
|----|---------|----------|---------------|--------|
| V1 | `OnRefresh` early-returns while holding EuroScope's HDC | Critical | **High** | **FIXED** |
| V2 | Annotations read off an unvalidated `CFlightPlan` | High | Medium | **FIXED** |
| V3 | Use-after-free on plugin exit (`RadarScreensOpened`) | High | Low | **FIXED** |
| V4 | `CopyCursor` handle leak on every tag hover | Medium | **High** | **FIXED** |
| V5 | Always-true condition in `OnMoveScreenObject` | Medium | Low | TODO |
| V6 | `PointF lpPoints[100]` filled with no bounds check | High | Medium | **FIXED** |
| V7 | `substr(1, 6)` throws on an empty system ID | Medium | Medium | TODO |
| V8 | Tag substitution is order-dependent and unescaped | Low | Low | TODO |
| V9 | `appWindows` is a global re-`new`ed by every screen | Medium | Low | TODO |
| V10 | `AircraftWilco` erased while range-for iterating | Medium | Low | TODO |
| V11 | `SelectObject(&oldPen)` selects a garbage handle | High | **High** | **FIXED** |
| V12 | `curl_easy_setopt` passes `std::string` through varargs | Medium | Low | TODO |
| V13 | `lpPoints[5000]` — 40 KB stack arrays, no bounds check | Medium | Low | TODO |
| V14 | `CSMRRadar` destructor leaks nearly everything it owns | Low | Low | TODO |
| V15 | Per-callsign maps grow forever | Low | Low | TODO |
| V16 | `Logger::info` per aircraft per frame | Medium | Low | TODO |
| V17 | Main-window subclass installed via globals, never removed | Medium | Low | TODO |
| V18 | `TagClickableMap[element]` — empty-key collisions | Low | Low | TODO |
| V19 | `GetBottomLine` called once per tag element per frame | Low | Low | TODO |
| V20 | O(N²) tag deconfliction mutating maps mid-iteration | Low | Low | TODO |
| V21 | `char* configKey = "definition"` — literal to non-const | Trivial | None | TODO |
| V22 | `SMRSharedData` statics give every TU its own private copy | Medium | Low | TODO |

---

## [x] V1 — `OnRefresh` early-returns while still holding EuroScope's HDC

**Severity:** Critical · **Crash-suspect:** High · **Status:** FIXED — `return` → `continue` at `SMRRadar.cpp:2434`. Verified: it is the only `return` after `dc.Attach`, so `ReleaseHDC` / `Detach` can no longer be skipped. Profile-level validation at `LoadProfile` time is still worth doing and is *not* covered by this fix.
**Location:** `vSMR/SMRRadar.cpp:2426` (return), `:1851` (attach), `:2678` / `:3185` (cleanup)

```cpp
const Value& LabelLines = LabelsSettings[Utils::getEnumString(TagType).c_str()][configKey];
vector<vector<string>> ReplacedLabelLines;
if (!LabelLines.IsArray())
    return;                       // skips ReleaseHDC (2678) and dc.Detach() (3185)
```

`dc` is a `CDC` attached to EuroScope's HDC at `:1851`. `CDC::~CDC()` calls
`::DeleteDC()` on any DC still attached — so this path **destroys EuroScope's device
context**. GDI+ never releases it either.

**Why it is the top suspect.** It is config-dependent, which fits "only one user is
reporting". RapidJSON's `operator[]` on a missing member returns a static null in
release builds — your own `Config.cpp` comment added in `b059223` says exactly this.
So any tag type in the deployed `vSMR_Profiles.json` whose `definition` /
`definitionDetailled` is missing or is not an array trips it. Note that
`definitionDetailled` is **not** in the repo's profile at all — it exists only in the
deployed vACC profile, which is also the profile carrying `uk_stand` / `remark`.

It also fits "~30 minutes" if the offending tag type only appears once that kind of
traffic shows up (arrivals, uncorrelated).

**Fix.** The `return` is inside the per-target tag loop — `continue` is what was meant.
Better: hoist the profile validation out of the render loop entirely and validate the
whole `labels` section once at `LoadProfile` time, falling back to a built-in default
definition rather than bailing mid-frame. Also consider restructuring `OnRefresh` so
`ReleaseHDC` / `Detach` cannot be skipped (RAII guard, or a single exit point).

**Notes:**

---

## [x] V2 — Annotations read off an unvalidated `CFlightPlan`

**Severity:** High · **Crash-suspect:** Medium · **Status:** FIXED — added `safeString()` to `Constant.hpp:29-34` and guarded every unvalidated `fp.` read in `GenerateTagData`. **The site list below was incomplete**: `origin` and `dest` (now `SMRRadar.cpp:1544`, `:1551`) were guarded only by `isAcCorrelated`, and `IsCorrelated()` returns `true` unconditionally when pro mode is off — so those two were *more* reachable than the annotations. Re-audited the whole function afterwards: every remaining `fp.Get*` sits inside an `fp.IsValid()` block.
**Location:** `vSMR/SMRRadar.cpp:1549-1557` (and `:1412`, `:1438`, `:1448`, `:1460`, `:1462`, `:1560`)

```cpp
string uk_stand;
uk_stand = fp.GetControllerAssignedData().GetFlightStripAnnotation(3);
if (uk_stand.length() == 0) uk_stand = "";

string remark = fp.GetControllerAssignedData().GetFlightStripAnnotation(4);
```

`GenerateTagData` is called for **every** radar target, including uncorrelated
primaries where `FlightPlanSelect(rt.GetCallsign())` returned an invalid `CFlightPlan`
(`SMRRadar.cpp:2281`). Every other `fp.` access in that function is wrapped in
`fp.IsValid()`; these two are not — nor are the squawk at `:1412`, the runways at
`:1438` / `:1448`, the gate at `:1460` / `:1462`, or the scratchpad at `:1560`.

`std::string = (const char*)nullptr` is an immediate access violation.

**Caveat.** The unguarded `GetSquawk` / `GetDepartureRwy` calls have shipped for years
without obvious trouble, which suggests EuroScope returns `""` rather than `nullptr`
for invalid objects. But `GetFlightStripAnnotation` indexes an array, so it is not
safe to assume it behaves the same. Treat this as cheap, unambiguous hardening rather
than a confirmed cause.

**Fix.** Guard the whole block with `fp.IsValid()`, and add a small helper that turns
any `const char*` from the SDK into a `std::string` with a null check. Apply it to
every unguarded SDK string read in `GenerateTagData`.

**Notes:**

---

## [x] V3 — Use-after-free on plugin exit

**Severity:** High · **Crash-suspect:** Low (fires at shutdown, not mid-session) · **Status:** FIXED — removed rather than patched. `EuroScopePlugInExitCustom` turned out to read only globals (`smrCursor`, `pluginWindow`, `gSourceProc`) and nothing from `this`, so the whole `RadarScreensOpened` registry existed only to find an instance for a function that needs none. It is now the free function `RestoreEuroscopeWindowProc()` (`SMRRadar.cpp:22-39`), called once from `EuroScopePlugInExit`; the registry, its `push_back`, and the member function are all deleted, so there is no longer a container of pointers to self-deleting objects to get stale. **Erasing on close was the documented fix and would have left a second bug**: with every screen closed the registry is empty, so the subclass would never have been undone. Calling through global state covers that case. Partially improves [[V17]]: the restore is now idempotent, clears `gSourceProc`/`pluginWindow`, and no longer keys off `smrCursor` (which is reassigned throughout the cursor handling and says nothing about whether the window was subclassed). V17 still stands for the `SetWindowSubclass` redesign and the per-window bookkeeping.
**Location:** `vSMR/SMRRadar.hpp:444` (`delete this`), `vSMR/SMRPlugin.cpp:663` (walk)

```cpp
inline virtual void OnAsrContentToBeClosed(void)
{
    ...
    delete RimcasInstance;
    delete this;                       // pointer stays in RadarScreensOpened
};
```

```cpp
for each (auto var in RadarScreensOpened)
    var->EuroScopePlugInExitCustom();  // walks freed objects
```

Every vSMR screen the user closes during the session leaves a dangling pointer that
`EuroScopePlugInExit` then calls a virtual function on.

**Fix.** Remove `this` from `RadarScreensOpened` in `OnAsrContentToBeClosed` before
`delete this`. Consider owning/weak pointers rather than raw ones.

**Notes:**

---

## [x] V4 — `CopyCursor` handle leak on every tag hover

**Severity:** Medium · **Crash-suspect:** High · **Status:** FIXED — added `loadSharedCursor()` at `SMRRadar.cpp:42-49` and dropped `CopyCursor` from all **ten** call sites (154, 167, 393, 395, 407, 428, 440, 516, 529, 1668). The audit originally listed seven; `OnOverScreenObject` (516, 529) and the `initCursor` block (1668) were leaking too.
**Location:** `vSMR/SMRRadar.cpp:145`, `:158`, `:384`, `:386`, `:398`, `:419`, `:431`

```cpp
smrCursor = CopyCursor((HCURSOR)::LoadImage(AfxGetInstanceHandle(),
                        MAKEINTRESOURCE(IDC_SMRMOVETAG), IMAGE_CURSOR, 0, 0, LR_SHARED));
```

`CopyCursor` allocates a new USER object every time; nothing ever calls
`DestroyCursor` on the previous one. One handle leaks per mouse enter/leave of a tag,
an inset window, or the correlate cursor. The per-process USER object cap is 10,000;
when exhausted, EuroScope itself can no longer create cursors, menus or windows.

**Why this is a strong ~30-minute fit.** It leaks only while the user is looking at
and moving the mouse over a vSMR screen — exactly the reported condition. And
**RampAgent amplifies it**: the stand/remark line makes every tag taller, so more of
the screen is covered by tags, so many more enter/leave transitions per minute of
normal mouse movement. V5 multiplies it further.

D2 confirms or kills this in ten minutes.

**Fix.** Keep one cached `HCURSOR` per resource ID, loaded once. `LR_SHARED` images do
not need copying at all — drop `CopyCursor` entirely, or `DestroyCursor(smrCursor)`
before each reassignment.

**Notes:**

---

## [ ] V5 — Always-true condition in `OnMoveScreenObject`

**Severity:** Medium · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:412` (inside `OnMoveScreenObject`, `:371-538`)

```cpp
if (ObjectType == DRAWING_TAG || ... || ObjectType == TAG_CITEM_GROUNDSTATUS
    || TAG_CITEM_UKSTAND || TAG_CITEM_REMARK || TAG_CITEM_SCRATCHPAD) {
```

The last three are bare non-zero constants, not comparisons, so the condition is
always true. The entire tag-drag branch — cursor swapping, `TagsOffsets` /
`TagAngles` / `TagLeaderLineLength` writes, `SetASELAircraft`, `RequestRefresh` — runs
for **every** draggable object type on the screen: RIMCAS boxes, menus, inset windows,
toolbar items.

Introduced alongside the stand/remark tag items, so it arrived with the RampAgent
integration. Also directly multiplies V4.

**Fix.** `ObjectType == TAG_CITEM_UKSTAND || ObjectType == TAG_CITEM_REMARK ||
ObjectType == TAG_CITEM_SCRATCHPAD`. Grep the file for the same shape elsewhere.

**Notes:**

---

## [x] V6 — `PointF lpPoints[100]` filled with no bounds check

**Severity:** High (latent) · **Crash-suspect:** Medium · **Status:** FIXED — both failure modes closed. (1) `Patatoide_Points` now holds `vector<POINT2>` instead of `map<int, POINT2>` (`SMRRadar.hpp:72-81`), so a read can no longer insert and grow `size()` mid-loop; the producer uses `push_back` (`SMRRadar.cpp:1250`, `:1257`), which reproduces the identical 0..83 ordering the index arithmetic gave. (2) All four draw sites clamp to the new `MAX_PATATOIDE_POINTS` constant (`SMRRadar.cpp:42`) — used for both the array size and the loop bound so they cannot drift apart — and pass the clamped count to `FillPolygon`. Note the `lpPoints[5000]` runway sites are **not** covered here; that is V13.
**Location:** `vSMR/SMRRadar.cpp:2106`, `:2123`, `:2140`, `:2174`; producer at `:1221`

```cpp
PointF lpPoints[100];
for (unsigned int i = 0; i < acPatatoide.points.size(); i++)
{
    pos.m_Latitude  = acPatatoide.points[i].x;   // map<int,POINT2>::operator[] — INSERTS
    ...
    lpPoints[i] = { ... };                       // no bound on i
}
graphics.FillPolygon(&H_Brush, lpPoints, acPatatoide.points.size());
```

Today this is exactly 84 points (keys 0..83 from the 12×7 loop at `:1221`), so it
fits — with 16 slots of headroom and zero guard.

Two ways it turns fatal:

1. If the point count ever grows past 100 (a bigger base shape, more interpolation
   steps), it silently smashes the stack.
2. `points` is a `map<int, POINT2>`, and `operator[]` **inserts** a default entry on a
   missing key while `i < size()` is re-evaluated every iteration. A single gap in
   the key sequence makes the loop grow its own bound: it never terminates and walks
   straight off the 100-element stack array.

**Fix.** Bound the loop with `std::min<size_t>(points.size(), 100)`, and use `find()`
/ `at()` instead of `operator[]` so a missing key cannot mutate the map during a read.
Better still, store the polygon as a `std::vector<POINT2>` — the keys are already
dense 0..N-1 and the map buys nothing.

**Notes:**

---

## [ ] V7 — `substr(1, 6)` throws on an empty system ID

**Severity:** Medium · **Crash-suspect:** Medium · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:1569-1571`

```cpp
TagReplacingMap["systemid"] = "T:";
string tpss = rt.GetSystemID();
TagReplacingMap["systemid"].append(tpss.substr(1, 6));
```

If `GetSystemID()` returns an empty string, `substr(1, ...)` throws
`std::out_of_range`. Nothing in `GenerateTagData` or `OnRefresh` catches it, so it
unwinds into EuroScope across a DLL boundary — a hard crash, and one that would look
random and traffic-dependent.

**Fix.** `tpss.size() > 1 ? tpss.substr(1, 6) : tpss`. Consider a top-level
`try/catch` around the body of `OnRefresh` as a backstop, logging rather than
swallowing.

**Notes:**

---

## [ ] V8 — Tag substitution is order-dependent and unescaped

**Severity:** Low (cosmetic) · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:2440-2470` and `:1565-1624`; `Constant.hpp:35` (`replaceAll`)

```cpp
for (auto& kv : TagReplacingMap)
    replaceAll(element, kv.first, kv.second);
```

`TagReplacingMap` is a `std::map`, so keys are applied in **alphabetical order**, and
each substitution's *result* is then exposed to every later key. The order is:

```
actype, arvrwy, asid, callsign, deprwy, dest, flightlevel, gate, groundstatus, gs,
origin, remark, sate, scratchpad, sctype, seprwy, sqerror, srvrwy, ssid, ssr,
systemid, tendency, uk_stand, wake
```

`remark` is substituted 12th, so the keys `sate`, `scratchpad`, `sctype`, `seprwy`,
`sqerror`, `srvrwy`, `ssid`, `ssr`, `systemid`, `tendency`, `uk_stand` and `wake` are
then applied *to the remark text*. RampAgent remarks are free text coming from the
API, so a remark containing any of those lowercase tokens gets silently mangled.

This is a live data coupling between the two plugins, which is why it is listed even
though it only corrupts display text.

**Fix.** Use explicit delimiters in the profile (`{remark}`, `{uk_stand}`) and do a
single left-to-right pass that never re-scans substituted output.

**Notes:**

---

## [ ] V9 — `appWindows` is a global re-`new`ed by every screen

**Severity:** Medium · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:24` (declaration), `:109-112` (constructor)

```cpp
map<int, CInsetWindow *> appWindows;          // file-scope global
...
appWindows[AppWindow1] = new CApproachWindow(APPWINDOW_ONE);   // in every ctor
appWindows[AppWindow2] = new CApproachWindow(APPWINDOW_TWO);
appWindows[DEPList]    = new CListWindow(DEP_LIST, "DEP LIST", true);
appWindows[TTTList]    = new CListWindow(TTT_LIST, "TTT LIST");
```

Every `CSMRRadar` constructed overwrites (and leaks) the previous four inset windows,
and all vSMR screens then share one set. Opening a second vSMR screen silently steals
the first screen's inset window state.

The same pattern applies to `mouseLocation`, `TagBeingDragged`, `initCursor`,
`smrCursor`, `standardCursor`, `customCursor`, `gSourceProc`, `pluginWindow`
(`SMRRadar.cpp:6-19`) and `CSMRRadar::vStripsStands` (`:22`).

Relevant here because **the affected user has two screens open** (vSMR + CoFrance),
and `mouseLocation` is derived from `ScreenToClient(GetActiveWindow(), &p)` at
`:1841` — hover detection can be computed against the wrong window.

**Fix.** Make `appWindows` and the cursor/drag state per-instance members. If globals
must stay for now, at least guard the constructor so it does not re-`new` over live
pointers.

**Notes:**

---

## [ ] V10 — `AircraftWilco` erased while range-for iterating

**Severity:** Medium · **Crash-suspect:** Low (CPDLC only) · **Status:** TODO
**Location:** `vSMR/SMRPlugin.cpp:635-644`

```cpp
for (auto &ac : AircraftWilco)
{
    CRadarTarget RadarTarget = RadarTargetSelect(ac.c_str());
    if (RadarTarget.IsValid()) {
        if (RadarTarget.GetGS() > 160) {
            AircraftWilco.erase(std::remove(AircraftWilco.begin(),
                                AircraftWilco.end(), ac), AircraftWilco.end());
        }
    }
}
```

Erasing from a `std::vector` invalidates the range-for iterators. Undefined behaviour,
in `OnTimer`, i.e. once per second. Only reachable if Hoppie/CPDLC is in use — check
D6 before deprioritising.

**Fix.** `std::erase_if`, or collect the callsigns to drop and erase after the loop.

**Notes:**

---

## [x] V11 — `SelectObject(&oldPen)` selects a garbage handle

**Severity:** High · **Crash-suspect:** High (if an approach inset window is open) · **Status:** FIXED — `InsetWindow.cpp:241` now passes `oldPen`, not `&oldPen`. Swept both files for the same shape: this was the only site where the argument was already a pointer; the others (`SMRRadar.cpp:2002/2194/2905/2972`, `InsetWindow.cpp:173/214/233/292/330/640`) correctly take the address of a stack `CPen`.
**Location:** `vSMR/InsetWindow.cpp:173` (save), `:241` (restore)

```cpp
CPen* oldPen = dc.SelectObject(&RunwayPen);   // oldPen is CPen*
...
dc.SelectObject(&oldPen);                      // &oldPen is CPen** — the address of a local
```

`CPen**` is an object pointer, so it converts implicitly to `void*` / `HGDIOBJ` and
binds to the handle-taking overload instead of the `CPen*` one. The call therefore
selects **the address of a stack variable** as a GDI handle — it fails, and the
original pen is never restored.

Consequence: `RunwayPen` / `ExtendedCentreLinePen` are still selected into
EuroScope's DC when they go out of scope at `:242`. Deleting a GDI object that is
currently selected into a DC fails, so the object leaks and the DC keeps a stale
handle — **every frame the approach inset window renders**. That is a per-frame GDI
handle leak on the exact code path the user is looking at, and it is a very good fit
for both "~30 minutes" and "only while a vSMR screen is displayed".

Compare `:640`/`:682` and `SMRRadar.cpp:2905`/`:2958`, which restore correctly with
`dc.SelectObject(oldPen)` — no `&`.

**Fix.** `dc.SelectObject(oldPen);`. Then grep both files for `SelectObject(&` applied
to a variable that is already a pointer. D2 will show whether this is the leak.

**Notes:**

---

## [ ] V12 — `curl_easy_setopt` passes `std::string` through varargs

**Severity:** Medium · **Crash-suspect:** Low (CPDLC only) · **Status:** TODO
**Location:** `vSMR/HttpHelper.cpp:32`

```cpp
curl_easy_setopt(curl, CURLOPT_URL, url);      // url is std::string, not const char*
```

`curl_easy_setopt` is variadic and expects `const char*`. Passing a non-trivially
copyable class type through `...` is undefined behaviour. With MSVC's SSO it may
appear to work for short URLs (the object's first bytes *are* the character buffer)
and break for long ones.

Also in this file: `handle_data` (`:15-24`) writes a `'\0'` into libcurl's buffer and
restores it afterwards, and `downloadedContents` is a `static` shared across concurrent
`_beginthread` downloads (`SMRPlugin.cpp:80`, `:112`, `:249`, `:589`) with no
synchronisation.

**Fix.** `curl_easy_setopt(curl, CURLOPT_URL, url.c_str());`. Make
`downloadedContents` a per-request buffer passed via `CURLOPT_WRITEDATA` instead of a
static, and append with `(char*)ptr, numbytes` rather than mutating the input.

**Notes:**

---

## [ ] V13 — `lpPoints[5000]` — 40 KB stack arrays, no bounds check

**Severity:** Medium · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:1985`, `:2019`, `:2043`

```cpp
PointF lpPoints[5000];
int w = 0;
for (auto& Point : def) {
    lpPoints[w] = { ... };     // no check against 5000
    w++;
}
```

40 KB of stack per array, three of them in `OnRefresh` (in nested scopes, so the
compiler may or may not overlap them), and none bounds-checked against the sector
file or `vSMR_Maps.json` content that drives the loop. A pathological runway or custom
path definition overflows the stack; `/GS` would turn that into an immediate
termination.

Only runs when `drawRunways` is on or a runway is marked closed.

**Fix.** Bound the loops, or use a reusable `std::vector<PointF>` member so the size
is dynamic and the stack stays small.

**Notes:**

---

## [ ] V14 — `CSMRRadar` destructor leaks nearly everything it owns

**Severity:** Low · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:88-118` (allocations), `:172-190` (`LoadCustomFont`), `~CSMRRadar`

`Callsigns`, `ColorManager`, `CurrentConfig` and the five `customFonts` entries are
`new`ed and never deleted. `LoadCustomFont` calls `customFonts.clear()` at `:175`
without deleting the previous `Gdiplus::Font*` values, so every profile reload or ASR
load leaks five GDI+ font objects (`:224`, `:952`).

**Fix.** `std::unique_ptr` for the singletons; delete the old fonts before clearing,
or hold `unique_ptr<Gdiplus::Font>` in the map.

**Notes:**

---

## [ ] V15 — Per-callsign maps grow forever

**Severity:** Low · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.hpp:56`, `:79`, `:99-102`; `SMRRadar.cpp:1131`, `:1629`

`Patatoides`, `TagsOffsets`, `TagAngles`, `TagLeaderLineLength`, `previousTagSize`,
`TagDragOffsetFromCenter` and `RecentlyAutoMovedTags` are all keyed by callsign and
never pruned. `OnFlightPlanDisconnect` (`:1629`) only cleans `DistanceTools`, and it
does so with an erase-during-iteration pattern that is itself wrong:

```cpp
for (multimap<string,string>::iterator itr = DistanceTools.begin(); itr != DistanceTools.end(); ++itr) {
    if (itr->first == callsign || itr->second == callsign)
        itr = DistanceTools.erase(itr);     // then ++itr skips / can run past end
}
```

Memory growth over a long session, plus the deconfliction loop (V20) getting slower as
stale entries accumulate.

**Fix.** Prune all per-callsign maps in `OnFlightPlanDisconnect`, and fix the erase
loop to not increment after erasing.

**Notes:**

---

## [ ] V16 — `Logger::info` per aircraft per frame

**Severity:** Medium (if ever enabled) · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/Logger.h:16-23`; callers at `SMRRadar.cpp:1338`, `:1247`, `:1653`, and ~33 more

```cpp
static void info(string message) {
    if (Logger::ENABLED && Logger::DLL_PATH.length() > 0) {
        std::ofstream file;
        file.open(Logger::DLL_PATH + "\\vsmr.log", std::ofstream::out | std::ofstream::app);
        file << "INFO: " << message << endl;
        file.close();
    }
}
```

Opens, writes, flushes and closes a file **per call**. `GenerateTagData` and
`GetBottomLine` are called once per aircraft (and `GetBottomLine` once per *tag
element*) per frame, so enabling logging turns every repaint into hundreds of file
opens. `ENABLED` defaults to false but is user-toggleable at runtime via `.smr log`
(`SMRPlugin.cpp:348`) and is **never persisted or reset**, so a user who toggled it
once has it on for the whole session.

Worth checking with the affected user (D6) — this alone would make EuroScope
unusable-slow and could plausibly be described as "crashing".

**Fix.** Hold one open stream, or buffer. At minimum, remove `Logger::info` from
per-aircraft and per-element hot paths.

**Notes:**

---

## [ ] V17 — Main-window subclass installed via globals, never removed

**Severity:** Medium · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:2311-2334` (install), `:3190-3199` (`EuroScopePlugInExitCustom`)

```cpp
pluginWindow = GetActiveWindow();
gSourceProc = (WNDPROC)SetWindowLong(pluginWindow, GWL_WNDPROC, (LONG)WindowProc);
initCursor = false;
```

`initCursor`, `pluginWindow` and `gSourceProc` are file-scope globals, so only the
first vSMR screen ever subclasses — which is what saves this from infinite
`CallWindowProc` recursion, but it means the behaviour depends on which screen drew
first. `GetActiveWindow()` at first refresh is not guaranteed to be the EuroScope main
window.

The subclass is only removed in `EuroScopePlugInExitCustom`, which V3 shows is called
on possibly-freed objects. If the DLL is ever unloaded with the subclass still
installed, the window procedure points into unmapped memory.

**Fix.** Store the previous `WNDPROC` per subclassed window, use
`SetWindowSubclass` / `RemoveWindowSubclass`, and unsubclass in the screen's own
teardown rather than at plugin exit.

**Notes:**

---

## [ ] V18 — `TagClickableMap[element]` — empty-key collisions

**Severity:** Low · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:2354-2377` (build), `:2664` (use); mirrored in `InsetWindow.cpp:374-397`

The map is keyed by the *rendered text*, not by field identity. Every empty field
collapses onto the `""` key, so the last empty field written wins:

```cpp
TagClickableMap[TagReplacingMap["uk_stand"]] = TAG_CITEM_UKSTAND;   // "" when no stand
TagClickableMap[TagReplacingMap["remark"]]   = TAG_CITEM_REMARK;    // "" overwrites
```

Then `AddScreenObject(TagClickableMap[element], ...)` at `:2664` uses `operator[]`,
inserting a `0` object type for any element text not in the map. Two fields with the
same text (e.g. a stand named the same as a runway) also collide, so clicking one
invokes the other's menu.

**Fix.** Build the clickable map by field name alongside `ReplacedLabelLines`, so each
drawn element carries its own object type rather than looking it up by text. Use
`find()` with an explicit `TAG_CITEM_NO` default.

**Notes:**

---

## [ ] V19 — `GetBottomLine` called once per tag element per frame

**Severity:** Low (perf) · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:2664`, `:2632`, `:2250`; definition at `:1247`

```cpp
AddScreenObject(TagClickableMap[element], rt.GetCallsign(), ItemRect, true,
                GetBottomLine(rt.GetCallsign()).c_str());
```

`GetBottomLine` does a `FlightPlanSelect`, a `RadarTargetSelect`, a callsign-code
lookup and a pile of string concatenation — repeated for **every element of every tag
on every frame**. RampAgent adds two more elements per tag, so it makes this
measurably worse.

**Fix.** Compute it once per aircraft, before the element loop, and reuse the string.

**Notes:**

---

## [ ] V20 — O(N²) tag deconfliction mutating maps mid-iteration

**Severity:** Low · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:3062-3165`

The outer loop iterates `tagAreas` while the body writes `tagAreas[areas.first]`
(`:3158`). That specific write is safe — assigning to an existing key does not
invalidate `std::map` iterators — but it is fragile, and `TagAngles[areas.first]`
(`:3119`, `:3152`) uses `operator[]`, which inserts.

Cost is O(N² × 17) rect intersections per frame, and the intersection test at `:3095`
re-looks-up `tagAreas[areas.first]` instead of using `areas.second`. Bigger tags
(RampAgent's extra line) mean more conflicts, so more of the 17 rotation steps run.

Also: `ConvertCoordFromPositionToPixel(GetPlugIn()->RadarTargetSelect(...).GetPosition().GetPosition())`
at `:3106` is unvalidated — a disconnected target yields a garbage anchor position.

**Fix.** Snapshot `tagAreas` into a vector before the loop and write results back
afterwards; use `areas.second` in the intersection test; validate the radar target.

**Notes:**

---

## [ ] V21 — `char* configKey = "definition"` — literal to non-const

**Severity:** Trivial · **Crash-suspect:** None · **Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:2421`

Ill-formed in C++11 onwards; MSVC permits it outside `/permissive-`. Cosmetic, but it
is on the same three lines as V1 so fix it while you are there.

**Fix.** `const char* configKey = "definition";`

**Notes:**

---

## [ ] V22 — `SMRSharedData` statics give every TU its own private copy

**Severity:** Medium · **Crash-suspect:** Low · **Status:** TODO
**Location:** `vSMR/SMRRadar.hpp:30-34`

```cpp
namespace SMRSharedData
{
	static vector<string> ReleasedTracks;
	static vector<string> ManuallyCorrelated;
};
```

`static` at namespace scope means **internal linkage**. Declared in a header, each
translation unit that includes it gets its own private copy — despite the namespace
being named "SharedData". Three TUs include `SMRRadar.hpp`: `SMRRadar.cpp`,
`InsetWindow.cpp`, and `SMRPlugin.cpp` (via `SMRPlugin.hpp`). So there are three
independent pairs of vectors.

Writers and readers are split across them:

| | Touches | Copy |
|---|---|---|
| `SMRRadar.cpp:771-784` | writes both (release / acquire click actions) | SMRRadar.cpp's |
| `SMRPlugin.cpp:601-605` | erases both (`OnFlightPlanDisconnect`) | SMRPlugin.cpp's |
| `SMRRadar.cpp:2342` | reads `ReleasedTracks` (visibility filter) | SMRRadar.cpp's |
| `SMRRadar.hpp:207,212` | reads both (`IsCorrelated`) | see below |

Two distinct consequences:

1. **Disconnect cleanup never runs on the written copy.** `OnFlightPlanDisconnect`
   erases from `SMRPlugin.cpp`'s vectors, which the click handler never writes to. So
   entries added by a release/acquire are never removed — the vectors grow for the whole
   session and a recycled system ID can inherit stale released/correlated state.

2. **`IsCorrelated` is `inline virtual`.** It is emitted in every TU that needs it and
   the linker folds the copies into one, but each emitted body references *its own* TU's
   statics — and the vtable holds a single entry. Which TU's vectors pro mode actually
   consults is therefore decided by which COMDAT the linker happened to keep, and can
   change between builds.

Not a crash, and not traffic-driven, so it is a poor fit for the reported symptom. Found
while removing the `RadarScreensOpened` registry for [[V3]].

**Fix.** Give them external linkage: declare `extern` in the header and define once in a
single `.cpp`. Better still, make them members of `CSMRPlugin` (or a real singleton), so
the sharing is explicit rather than resting on linkage rules. Check the rest of the
header for the same pattern — `SMRPluginSharedData::io_service` at `SMRRadar.hpp:37-40`
is declared the same way.

**Notes:**

---

# Part 2 — RampAgent (`D:/DEV/EuroscopeRampAgent`, baseline `04f2338`)

| ID | Finding | Severity | Crash-suspect | Status |
|----|---------|----------|---------------|--------|
| R1a | Annotation sweep runs when not connected / not a controller | High | Medium | **FIXED** |
| R1b | `standsCacheMutex_` held across EuroScope calls | High | Medium | **FIXED** |
| R1c | 23-character annotation write against an undocumented limit | High | **High** | **FIXED** (limit still unmeasured) |
| R2 | `OnFunctionCall` never validates the ASEL flight plan | High | **High** | **FIXED** |
| R3 | `airportStandsCache_[icao]` read unlocked, and inserts | Medium | Medium | **FIXED** |
| R4 | `OnGetTagItem` sets `*pColorCode` before validating, leaves `*pRGB` unset | Low | Low | TODO |
| R5 | `Shutdown()` calls into EuroScope during teardown | Medium | Low | **FIXED** |
| R6 | `IsController()` builds a `std::string` before any validity check | Medium | Low | TODO |
| R7 | `std::string` built from `rt.GetCallsign()` with no null check | Medium | Low | **FIXED** (with R1a/R1b) |
| R8 | Query strings built with no URL encoding | Medium | Low | **FIXED** |
| R9 | Worker thread can block plugin unload for a long time | Medium | Low | **FIXED** (residual noted) |
| R10 | Worker thread started from the constructor | Low | Low | TODO |
| R11 | `PopulateICAOStandMap` runs once, never retried | Medium | None | **FIXED** |
| R12 | `AUTH_SECRET` compiled into a distributed DLL | Medium (security) | None | TODO |
| R13 | `RegisterTagItems` defined non-`inline` in a header | Low | None | TODO |
| R14 | `static size_t counter` inside a member function | Trivial | None | TODO |
| R15 | Dead code: `SortStandList` | Trivial | None | TODO |

---

## [x] R1a — Annotation sweep runs when not connected / not a controller

**Severity:** High · **Crash-suspect:** Medium · **Status:** FIXED — two guards. (1) The sweep returns immediately unless `isConnected_` *and* `isController_`, clearing `lastAnnotationWrite_` on the way out so a fresh connection starts clean. (2) For the case the gate cannot catch — a connected controller whose write EuroScope still refuses, e.g. an aircraft tracked elsewhere — `SetFlightStripAnnotation`'s return value is now checked and the attempted values remembered in `lastAnnotationWrite_`; identical values are not re-offered until something changes. The per-tick `Get != desired` comparison is deliberately kept on the success path, so an annotation cleared by another plugin is still restored.
**Location:** `src/RampAgent.cpp:414-433` (`OnTimer`), `:441-479` (`UpdateFlightStripAnnotations`)

`OnTimer` calls `UpdateFlightStripAnnotations()` unconditionally — it does not consult
`isConnected_` or `isController_`, which it just computed two lines earlier.

```cpp
if (assignedData.GetFlightStripAnnotation(STAND_FLIGHT_STRIP_INDEX) != truncatedStand) {
    assignedData.SetFlightStripAnnotation(STAND_FLIGHT_STRIP_INDEX, truncatedStand.c_str());
}
```

When the user cannot write controller-assigned data (observer, not connected, aircraft
tracked elsewhere), `SetFlightStripAnnotation` returns `false` and changes nothing —
so the `!=` guard stays true and the call is retried **every second for every on-ground
aircraft, forever**. At a busy airport that is ~100 failing SDK calls per second, each
of which EuroScope may also try to broadcast to the network (see X4).

**Fix.** Early-out of `UpdateFlightStripAnnotations` unless connected *and* a
controller. Track which callsign/index pairs were successfully written so a rejected
write is not retried every tick.

**Notes:**

---

## [x] R1b — `standsCacheMutex_` held across EuroScope calls

**Severity:** High · **Crash-suspect:** Medium · **Status:** FIXED — `standsCache_` is copied into a local under the lock, the lock is released, and only then does the sweep call into EuroScope. No SDK call now happens while `standsCacheMutex_` is held, so `OnGetTagItem` re-entering on the same thread can no longer self-deadlock, and the HTTP worker is no longer blocked for the length of a full sweep. `lastAnnotationWrite_` is main-thread-only and deliberately unlocked.
**Location:** `src/RampAgent.cpp:443` and `src/core/TagItem.h:14`

```cpp
void rampAgent::RampAgent::UpdateFlightStripAnnotations()
{
    std::lock_guard<std::mutex> lock(standsCacheMutex_);   // held for the whole sweep
    CRadarTarget rt = this->RadarTargetSelectFirst();
    while (rt.IsValid()) { ... assignedData.SetFlightStripAnnotation(...); ... }
}
```

`OnGetTagItem` takes the **same non-recursive mutex** on the **same thread**
(`TagItem.h:14`). Any re-entrancy from EuroScope during `SetFlightStripAnnotation` —
a synchronous list refresh, a strip repaint — is a recursive lock of a `std::mutex`:
undefined behaviour, which under MSVC is either a deadlock (EuroScope appears frozen,
which users report as a crash) or a `std::system_error` thrown out of `OnGetTagItem`
and across the DLL boundary into EuroScope.

The lock also serialises the render thread against the HTTP worker for the whole
duration of the sweep.

**Fix.** Copy the callsign→stand data you need under the lock into a local, release
the lock, *then* call into EuroScope. Nothing in the sweep needs the mutex while
talking to the SDK.

**Notes:**

---

## [x] R1c — 23-character annotation write against an undocumented limit

**Severity:** High · **Crash-suspect:** High · **Status:** FIXED in two parts. **EuroScope's real limit is still unmeasured** — searched the SDK header (no size constants, no fixed char arrays) and the shipped EuroScope 3.2.13 install (no documentation at all). It is not discoverable without a live test or disassembly, so the fix was built not to depend on it.

1. **`MAX_ANNOTATION_LENGTH` 23 → 15.** 15 is not another guess: it is the SDK's own budget for plugin supplied tag text (`OnGetTagItem`'s `char sItemString[16]`), and it is already what `TagItem.h:43,52` truncates to. The two output paths disagreed — the same stand name was cut to 15 on the tag item path and 23 on the annotation path. Nothing displayable is lost, since the tag item path already capped at 15.

2. **The write is now verified rather than trusted** (`RampAgent.cpp:569-577`). `SetFlightStripAnnotation` returns true even when EuroScope stores less than it was handed, so the annotation is read back after writing and a mismatch is treated as a rejection. This is the half that does not depend on knowing the limit: whatever it turns out to be, an over-long value is detected.

**This also closed a live bug that [[R1a]] did not cover.** R1a suppresses retries only when `SetFlightStripAnnotation` *returns false*. Silent truncation returns true, so the next tick's "value differs" comparison would fail again, rewrite again, and loop **every tick forever** — precisely the storm R1a set out to stop, through a door it left open. Verification closes it.

**Deliberately not shared with `TagItem.h`.** The two limits coincide at 15 today but are independent constraints: the tag item buffer is 16 bytes by SDK contract regardless of what the annotation slot holds. Tying them to one constant would silently break the tag item if the annotation limit is ever raised.

**Still worth doing:** D4/D5 would establish the real number. If EuroScope truncates below 15, the symptom is now visible rather than fatal — stands appear cut short in vSMR tags, and the write is suppressed after one attempt instead of retried. A one-shot chat diagnostic on the first verified mismatch would turn that into a precise measurement; not built, since after the clamp to 15 it is unlikely to fire.
**Location:** `src/RampAgent.cpp:463`, `:471`

```cpp
std::string truncatedStand = standName.length() > 23 ? standName.substr(0, 23) : standName;
// "Truncate to 23 characters to ensure it fits in the annotation field"
```

The EuroScope SDK documents no length limit for `SetFlightStripAnnotation`
(`EuroScopePlugIn.h:1647-1658` — parameters and return value only). The 23 reads like
a guess. If EuroScope's internal slot is shorter, this is a **heap overflow inside
EuroScope**, which is hypothesis (B): the corruption would surface wherever ES
allocates most, and that is vSMR's `OnRefresh` — hundreds of `AddScreenObject` and
`FlightPlanSelect` calls per frame, far more than a CoFrance screen.

This is the single best explanation for "only crashes while a vSMR screen is
displayed, and only with RampAgent loaded, and only after a while".

**Test:** D5 (clamp to 10) and D4 (PageHeap). Both are cheap and decisive.

**Fix.** Establish the real limit — check what UK Controller Plugin and vStrips write
to annotation 3, and test empirically. Until then clamp conservatively (10-16) and
never write a remark longer than what vSMR can usefully display anyway.

**Notes:**

---

## [x] R2 — `OnFunctionCall` never validates the ASEL flight plan

**Severity:** High · **Crash-suspect:** High · **Status:** FIXED — added `fp.IsValid()` guard plus an empty-callsign/ICAO guard in `TagFunctions.h`, and a `SafeString()` null-wrapper in `Helpers.h:15-23` for SDK `const char*` returns. `SafeString` is the helper V2 should reuse on the vSMR side.
**Location:** `src/core/TagFunctions.h:20-22`; caller in vSMR at `SMRRadar.cpp:873-877`

```cpp
auto fp = FlightPlanSelectASEL();
std::string callsign = ToUpper(fp.GetCallsign());
std::string icao = ToUpper(fp.GetFlightPlanData().GetDestination());
```

No `fp.IsValid()`. If ASEL is empty or stale, `std::string(nullptr)` is an immediate
access violation.

**This is the one path vSMR calls into RampAgent directly:**

```cpp
if (ObjectType == TAG_CITEM_UKSTAND) {
    CRadarTarget rt = GetPlugIn()->RadarTargetSelect(sObjectId);
    GetPlugIn()->SetASELAircraft(GetPlugIn()->FlightPlanSelect(sObjectId));
    StartTagFunction(rt.GetCallsign(), NULL, TAG_ITEM_TYPE_CALLSIGN,
                     rt.GetCallsign(), "RampAgent", 0, Pt, Area);
}
```

vSMR sets ASEL from `FlightPlanSelect(sObjectId)` first — but if that target has no
flight plan (uncorrelated primary), the `SetASELAircraft` is a no-op and
`FlightPlanSelectASEL()` returns whatever was selected before, or nothing.

Note also that the annotation code is byte-identical between RampAgent v1.0.8 and
v2.0.1 (verified by diff), so **if the crash is new in v2, this path is the prime
suspect, not R1**. See D6.

**Fix.** `if (!fp.IsValid()) return;` at the top, and a null-safe wrapper around every
`const char*` the SDK returns.

**Notes:**

---

## [x] R3 — `airportStandsCache_[icao]` read unlocked, and inserts

**Severity:** Medium · **Crash-suspect:** Medium · **Status:** FIXED — rather than switching the use site to the locked copy, the whole-cache copy is gone: one `find()` under the lock now copies just this airport's stands into a local `vector<Stand>`, used by both the popup loop and the manual-entry validation. `find()` in place of `operator[]` means an unsupported ICAO can no longer insert an empty entry and rehash the map underneath the worker thread. The "not supported" message is emitted *after* the lock is released, since `DisplayError` calls into EuroScope — same rule as [[R1b]]. Verified the only remaining `airportStandsCache_` accesses are this one and the writer at `RampAgent.cpp:325`, both under `standsCacheMutex_`.
**Location:** `src/core/TagFunctions.h:24-28` (locked copy), `:40` (unlocked use); writer at `src/RampAgent.cpp:324-325`

```cpp
std::unordered_map<std::string, std::vector<Stand>> localAirportStandsCache;
{
    std::lock_guard<std::mutex> lock(standsCacheMutex_);
    localAirportStandsCache = airportStandsCache_;      // correct copy made...
}
...
for (const auto& stand : airportStandsCache_[icao]) {  // ...then ignored
```

The locked copy three lines above is exactly what should be iterated. Instead the
shared member is read with no lock, and `unordered_map::operator[]` **inserts** an
empty vector for an unknown ICAO — which can rehash concurrently with the worker
thread's `airportStandsCache_[icao] = std::move(stands)` at `RampAgent.cpp:325`.

Narrow window (the worker only populates at startup), but it is a genuine data race
during exactly the period when a user is most likely to be clicking around.

**Fix.** `for (const auto& stand : localAirportStandsCache[icao])` — or better,
`.at(icao)`, since existence was already checked at `:30`.

**Notes:**

---

## [ ] R4 — `OnGetTagItem` sets `*pColorCode` before validating

**Severity:** Low · **Crash-suspect:** Low · **Status:** TODO
**Location:** `src/core/TagItem.h:19-35`

```cpp
*pColorCode = EuroScopePlugIn::TAG_COLOR_RGB_DEFINED;
if (!FlightPlan.IsValid()) return;              // *pRGB never written
...
if (!standsCache_.contains(callsign)) return;   // same
```

Every early return leaves EuroScope told to use an RGB-defined colour while `*pRGB` was
never set — the tag item renders in whatever colour was last in that variable.

The mutex is also held for the entire call including the early returns, on a function
EuroScope invokes for every tag item of every displayed aircraft on every refresh
(see R1b).

**Fix.** Set `*pColorCode` and `*pRGB` together, only on the success paths.

**Notes:**

---

## [x] R5 — `Shutdown()` calls into EuroScope during teardown

**Severity:** Medium · **Crash-suspect:** Low · **Status:** FIXED — the `DisplayMessage` is gone. Confirmed `Shutdown()` has exactly one caller, `~RampAgent()` (`RampAgent.cpp:32`), so emitting it "at the top before teardown" — the other option the fix listed — would only have moved the call earlier inside the destructor, not out of it. **Also fixed the startup mirror image**: `Initialize()`'s catch block called `DisplayError` from the constructor, before `EuroScopePlugInInit` has handed EuroScope the instance. It now uses `QueueError`, so the failure still surfaces, one tick later, via `OnTimer`. Swept the plugin afterwards: every remaining `DisplayMessage`/`DisplayError` is inside `OnTimer`, `OnCompileCommand` or `OnFunctionCall`, all of which only run while EuroScope is live.
**Location:** `src/RampAgent.cpp:67-82`, reached via `EuroScopePlugInExit` at `:44-47`

```cpp
void RampAgent::Shutdown()
{
    ...
    if (m_thread.joinable()) m_thread.join();
    DisplayMessage("Ramp Agent shutdown complete");   // DisplayUserMessage during destruction
}
```

`Shutdown` runs from `~RampAgent()`, which runs from `myPluginInstance.reset()` inside
`EuroScopePlugInExit`. Calling `DisplayUserMessage` on a partially destroyed `CPlugIn`
is a known crash-on-unload pattern.

Not the reported crash (that happens with the plugin loaded) but it is on the exact
path the user exercised when they unloaded the plugin to test.

**Fix.** Drop the message, or emit it at the top of `Shutdown` before anything is torn
down.

**Notes:**

---

## [ ] R6 — `IsController()` builds a `std::string` before any validity check

**Severity:** Medium · **Crash-suspect:** Low · **Status:** TODO
**Location:** `src/RampAgent.cpp:481-492`

```cpp
const std::string callsign = this->ControllerMyself().GetCallsign();   // constructed first
if (callsign.size() < 3) return false;                                 // checked after
...
std::lock_guard<std::mutex> lock(userCallsignMutex_);
userCallsign_ = callsign;   // comment says "Lock is held by calling function" — stale, it isn't
```

The `size() < 3` guard runs *after* the `std::string` has already been constructed
from the raw pointer. If `ControllerMyself()` is invalid and `GetCallsign()` returns
null, the crash happens on the line above the guard.

The trailing comment is stale and misleading — the lock is taken right there, and the
caller (`OnTimer`) holds nothing.

**Fix.** Null-check the pointer before constructing the string; delete the stale
comment.

**Notes:**

---

## [x] R7 — `std::string` built from `rt.GetCallsign()` with no null check

**Severity:** Medium · **Crash-suspect:** Low · **Status:** FIXED — absorbed by the R1a/R1b rewrite of the same lines. The callsign is now resolved once through `SafeString()` into a local (`RampAgent.cpp:465`), and the cache is read with a single `find()` on the snapshot instead of three `operator[]` lookups.
**Location:** `src/RampAgent.cpp:459`, `:462`, `:470`

```cpp
bool hasStand = standsCache_.contains(rt.GetCallsign());
...
const std::string standName = hasStand ? standsCache_[rt.GetCallsign()].name : "";
const std::string remark    = hasStand ? standsCache_[rt.GetCallsign()].remark : "";
```

Three separate temporary `std::string` constructions from the same raw pointer, per
aircraft, per second — none null-checked, and `standsCache_[...]` uses `operator[]`,
which would insert on a miss (harmless only because `contains()` gates it).

Note vSMR's own `b059223` commit message warns that "the `const char*` ES returns is
not guaranteed to keep the same address between calls" — the same caution applies here.

**Fix.** Resolve the callsign once into a local `std::string` with a null check, then
do a single `find()` and reuse the iterator.

**Notes:**

---

## [x] R8 — Query strings built with no URL encoding

**Severity:** Medium · **Crash-suspect:** Low · **Status:** FIXED — every interpolated value now goes through httplib's public encoders.

Two decisions worth recording. First, **`httplib::append_query_params` was the obvious choice and I rejected it**: it routes through `encode_query_component` with the default `space_as_plus = true`, so a space would be sent as `+` — correct only if the server parses the query as form-encoded. Calling `httplib::encode_query_component(value, false)` directly emits `%20`, which every server decodes, and keeps the parameter order unchanged (`Params` is a `multimap`, so it would have reordered them alphabetically). For all realistic values — ICAO codes, VATSIM callsigns, hex tokens — the wire format is byte-identical to before.

Second, **the site list was incomplete**: `PopulateICAOStandMap` interpolates the ICAO into the *path* (`"/api/airports/" + icao + "/stands"`), not a query parameter. That is the more dangerous of the two, since a stray `/` or `?` restructures the request rather than corrupting one value. It now uses `encode_path_component`, applied to the ICAO only so the surrounding separators stay literal.

Swept afterwards: all four `cli.Get` call sites are either fully encoded or a bare literal.
**Location:** `src/RampAgent.cpp:352-356` (assign), `:149` (occupancy)

```cpp
std::string apiEndpoint = "/api/assign?stand=" + standInfo.name +
    "&icao=" + standInfo.icao +
    "&callsign=" + callsign +
    "&token=" + token +
    "&client=" + userCallsign;
```

`standInfo.name` comes from the API, `callsign` from EuroScope, and neither is
percent-encoded. A stand name containing a space, `&`, `#`, `+` or `%` either breaks
the request or injects extra query parameters. Same at `:149` for the occupancy
lookup.

**Fix.** Percent-encode every interpolated value (`httplib` provides
`httplib::detail::encode_url`, or write a small helper).

**Notes:**

---

## [x] R9 — Worker thread can block plugin unload for a long time

**Severity:** Medium · **Crash-suspect:** Low · **Status:** FIXED, with one residual. The 100 ms `sleep_for` is now a `wait_for` on `m_stopCv` with an `m_stop` predicate, and `Shutdown()` writes `m_stop` **under `m_stopMutex`** before notifying — without that the worker could evaluate the predicate, then begin waiting past the notify and sleep out the full interval. `PopulateICAOStandMap` also checks `m_stop` between airports, so a startup fetch against a slow API abandons the remaining queue instead of working through it.

Worst-case unload drops from *N airports* x ~3 s (minutes, if the API is unreachable) to **one in-flight request, ~3 s** (2 s connect + 1 s read).

**Residual:** that last ~3 s is a socket already blocked in `httplib`. Cutting it needs `cli->stop()` called from `Shutdown()`, which means hoisting the `SSLClient` out of `WorkerThread`'s local scope into a member — it is currently created *and* destroyed on the worker thread, so sharing it introduces a lifetime problem this fix deliberately avoids. ~3 s on unload is not worth that; revisit only if unload latency is ever actually reported.
**Location:** `src/RampAgent.cpp:78-79` (join), `:105-144` (loop), `:313-344` (startup fetch)

The worker sleeps in 100 ms chunks with no condition variable, and `Shutdown()` joins
it. If unload lands while `PopulateICAOStandMap` is mid-run, the join waits for **all
remaining sequential airport requests** — each up to 2 s connect + 1 s read. With a
slow or unreachable API that is a multi-second-to-minute hang of EuroScope on plugin
unload or shutdown.

**Fix.** Use a `std::condition_variable` with `wait_for` so `m_stop` wakes the thread
immediately, and check `m_stop` between airport requests in `PopulateICAOStandMap`.

**Notes:**

---

## [ ] R10 — Worker thread started from the constructor

**Severity:** Low · **Crash-suspect:** Low · **Status:** TODO
**Location:** `src/RampAgent.cpp:25-29`, `:49-65`

`RampAgent::RampAgent()` calls `Initialize()`, which does `RegisterTagItems()`,
`RegisterTagActions()` and then `m_thread = std::thread(&RampAgent::WorkerThread, this)`
— all before `EuroScopePlugInInit` has handed the pointer to EuroScope. The worker
immediately calls `PopulateICAOStandMap`, which calls `QueueError` on an object
EuroScope does not yet know about.

Members are all constructed by the time the constructor body runs, so this is not
currently a bug — but it is fragile, and it means startup errors can be queued before
the plugin is registered.

**Fix.** Move thread start out of the constructor into an explicit `Start()` called
after `EuroScopePlugInInit` has published the instance.

**Notes:**

---

## [x] R11 — `PopulateICAOStandMap` runs once, never retried

**Severity:** Medium (functional) · **Crash-suspect:** None · **Status:** FIXED — it now returns `bool` and the worker retries until it succeeds, backing off 5s → 10 → 20 … capped at 5 minutes. Three details that mattered:

- **Retries chase only the gaps.** Airports already in `airportStandsCache_` are skipped, so a partial failure does not refetch what already worked, and a permanently broken single airport costs one request per attempt rather than the whole list.
- **The error spam had to go first**, otherwise retrying multiplies it. Per-airport failures are now counted, not reported, and all failure paths funnel through `ReportStandMapFailure()`, which emits one message per outage and returns `false` so it reads as a return value. The flag clears on success, so a *later* outage is still reported.
- **Success is announced only after a failure**, so a clean start stays silent — matching the existing `printError` behaviour in `FetchAndUpdateAssignedStands`.

Incidentally cleared the long-standing C4456 warning: the inner `res` that shadowed the outer one is now `standsRes`. Also collapsed the duplicate `nlohmann::json::exception` / `std::exception` catch pairs, since the former derives from the latter.

**Known edge:** an API that returns a valid but empty airport list counts as success and stops the retries. That is the server saying "no supported airports", which seems right, but if the API can return `[]` transiently it would want a different rule.
**Location:** `src/RampAgent.cpp:112`, `:251-345`

Called exactly once, before the worker loop. If the API is down, slow, or the user
starts EuroScope before their network is up, `airportStandsCache_` stays empty for the
whole session: the stand menu reports "`XXXX` is not supported." for every airport,
with no recovery short of restarting EuroScope. Each failed airport also queues an
error, so a full outage floods the chat window at load.

Contrast with `FetchAndUpdateAssignedStands`, which has the `printError` flag to
suppress repeats and a "Successfully reconnected" message.

**Fix.** Retry with backoff from inside the worker loop until it succeeds; collapse
the per-airport error spam into one summary message.

**Notes:**

---

## [ ] R12 — `AUTH_SECRET` compiled into a distributed DLL

**Severity:** Medium (security) · **Crash-suspect:** None · **Status:** TODO
**Location:** `src/Secret.h` (gitignored, not tracked — confirmed), used at `src/RampAgent.cpp:494-505`

```cpp
std::string s = AUTH_SECRET + callsign;
SHA256(...);
```

The secret is correctly kept out of git, but it is compiled into every shipped
`RampAgent.dll` and trivially recoverable with `strings`. Anyone can then mint a valid
token for any controller callsign and assign or free stands via `/api/assign`.

Server-side, `token = SHA256(secret + callsign)` is also a static per-callsign value —
no nonce, no expiry — so a token observed once is replayable forever.

Out of scope for the crash, but listed because the audit asked for every threat.

**Fix.** Move authorisation server-side against the VATSIM connection (the API already
knows the controller callsign from the datafeed). If a shared secret must stay, add a
timestamp to the hashed payload and enforce a window server-side.

**Notes:**

---

## [ ] R13 — `RegisterTagItems` defined non-`inline` in a header

**Severity:** Low · **Crash-suspect:** None · **Status:** TODO
**Location:** `src/core/TagItem.h:7`

```cpp
void RampAgent::RegisterTagItems() {      // not inline; TagFunctions.h / CompileCommands.h are
```

Links only because `RampAgent.cpp` is the single translation unit that includes it.
Adding a second `.cpp` that includes `TagItem.h` produces a duplicate-symbol error.

**Fix.** Mark it `inline`, or move these header-defined member functions into `.cpp`
files.

**Notes:**

---

## [ ] R14 — `static size_t counter` inside a member function

**Severity:** Trivial · **Crash-suspect:** None · **Status:** TODO
**Location:** `src/RampAgent.cpp:115`

Function-local `static` inside `WorkerThread`, so it is shared across all instances
rather than per-object. Harmless today (single instance) but wrong in principle, and
it never resets — so the `counter % 100 == 0` fetch cadence is tied to a value that
survives a hypothetical restart of the loop.

**Fix.** Make it a local variable outside the `while`, or a member.

**Notes:**

---

## [ ] R15 — Dead code: `SortStandList`

**Severity:** Trivial · **Crash-suspect:** None · **Status:** TODO
**Location:** `src/core/Helpers.h:19-77`

58 lines of natural-order stand sorting that nothing calls. The stand menu
(`TagFunctions.h:40`) presents stands in whatever order the API returned them.

**Fix.** Either wire it into the popup list (probably the intent) or delete it.

**Notes:**

---

# Part 3 — Cross-plugin interaction surface

These are not bugs in either plugin alone. They are the contract between them, and
they need an owner.

## [ ] X1 — Annotation index 3 is contested

**Status:** TODO

RampAgent writes the stand to flight strip annotation **3** (`RampAgent.cpp:44`), which
is the same slot UK Controller Plugin uses — vSMR's own comment at `SMRRadar.cpp:1548`
still says "UK Controller Plugin / Assigned Stand". If a user runs UKCP (or vStrips)
alongside RampAgent, both write index 3 every cycle and fight over it. Combined with
R1a's retry-forever behaviour, that is an unbounded write storm.

Worth asking the affected user which other plugins they have loaded (D6).

**Fix.** Make the annotation index configurable, and document which slot RampAgent
owns. Detect a value it did not write and back off rather than overwriting.

**Notes:**

---

## [ ] X2 — vSMR hardcodes RampAgent's plugin name and function ID

**Status:** TODO
**Location:** `vSMR/SMRRadar.cpp:873-877`

```cpp
StartTagFunction(rt.GetCallsign(), NULL, TAG_ITEM_TYPE_CALLSIGN,
                 rt.GetCallsign(), "RampAgent", 0, Pt, Area);
```

The literal `"RampAgent"` must match the plugin name in the `CPlugIn` constructor
(`RampAgent.cpp:25`), and the `0` must stay `TagActionID::OpenMENU`
(`RampAgent.h:34-37`). Reordering that enum or renaming the plugin breaks the
integration silently — no error, the menu just stops opening.

Also note `TagObjectRightTypes` maps `TAG_CITEM_UKSTAND` to the sentinel `999999`
(`SMRRadar.cpp:853`) purely to make the
`if (Button == BUTTON_RIGHT && TagObjectRightTypes[ObjectType])` guard pass — a magic
number with no name.

**Fix.** Name the constants on both sides, and add a comment in `RampAgent.h` above
`TagActionID` saying that vSMR depends on `OpenMENU == 0`.

**Notes:**

---

## [ ] X3 — Two TLS stacks in one process

**Status:** TODO

RampAgent links OpenSSL statically via vcpkg `x86-windows-static` with `/MT`
(`CMakeLists.txt:3`, `:11`, `:114`); vSMR links `libcurl.lib` with `/MT`
(`vSMR.vcxproj`, `lib/libcurl.lib`). Both static CRTs mean separate heaps, so
allocations do not cross — good. But two independent OpenSSL copies in one process is
worth confirming does not cause init/atexit interference, especially since both do
their networking on background threads.

Low priority: vSMR's HTTP only runs if Hoppie/CPDLC is configured.

**Fix.** Verify with `dumpbin /dependents` on both DLLs which crypto stack each
actually pulls in.

**Notes:**

---

## [ ] X4 — Network traffic from the annotation sweep

**Status:** TODO

Every successful `SetFlightStripAnnotation` on an aircraft tracked by another
controller causes EuroScope to broadcast an assigned-data update to the FSD server.
R1a's retry-forever loop means this can run at ~100 writes/second at a busy airport.
Worth confirming EuroScope actually suppresses no-op writes rather than flooding.

**Fix.** Covered by R1a. Confirm with a packet capture or the EuroScope network log if
D1-D5 do not resolve the crash.

**Notes:**

---

# Appendix — investigated and ruled out

Recorded so nobody re-investigates these.

| Area | Conclusion |
|------|------------|
| `replaceAll` infinite loop (`Constant.hpp:35`) | Cannot loop: it builds a new string with `lastPos` always advancing. Would only hang on an empty `from`, and all keys are non-empty. |
| RIMCAS container growth (`Rimcas.cpp:26-41`) | All per-frame containers are cleared in `OnRefreshBegin`; `statRPAtimer` has a cleanup pass. Not accumulating. |
| Tag deconfliction iterator invalidation (`SMRRadar.cpp:3158`) | `tagAreas[areas.first] = ...` assigns to an existing key — `std::map` iterators stay valid. Safe, but see V20. |
| `RadarTargetSelectFirst/Next` cursor interference between plugins | The SDK stores the position in the `CRadarTarget` / `CFlightPlan` object itself (`ESINDEX m_FpPosition`), not in a shared cursor. The two plugins' iterations cannot interfere. |
| Window-subclass infinite recursion | `initCursor` is a file-scope global, so only the first vSMR screen subclasses. No `CallWindowProc` recursion. See V17 for the residual issue. |
| `Logger::ENABLED` default | Initialised `false` in the `CSMRPlugin` constructor (`SMRPlugin.cpp:266`). Off unless the user ran `.smr log`. See V16. |
| Cross-DLL heap corruption between the two plugins | Both build with `/MT` (static CRT), so each has its own heap. A `std::string` allocated in one is never freed by the other. |
| RampAgent worker touching the EuroScope SDK off-thread | It does not. The worker only touches RampAgent's own caches under mutexes. Threading discipline here is sound. |
| RampAgent annotation code changed in v2 | Byte-identical to v1.0.8 (verified with `git show v1.0.8:src/RampAgent.cpp`). If the crash is new in v2, look at R2, not R1. |
