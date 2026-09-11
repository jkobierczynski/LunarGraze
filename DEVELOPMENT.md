# Developing LunarGraze: a working log

This is a narrative record of how LunarGraze (and the accuracy work behind
it) actually got built -- what was tried, what broke, how each bug was
found, and why the fixes look the way they do. The app was named `graze_gui`
for most of this history and only renamed to LunarGraze afterwards; quoted
identifiers, paths and messages below (`graze_gui`, the old User-Agent
string, etc.) are left as they were at the time, as an accurate record of
what actually existed -- they don't reflect the current names in `src/`.
`README.md` documents the finished tool; this document exists so a future
change doesn't have to rediscover the same dead ends, and so "why does the
code do this oddly specific thing" has an answer.

Three threads run through this, in the order they happened:

1. Porting `graze_finder.py`'s search algorithm to a from-scratch C++
   engine, and validating it hard enough to trust it.
2. Building the Qt desktop app on top of that engine, including KML/CSV
   export.
3. Making the map actually render tiles -- which turned out to be a much
   longer story than it had any right to be.

## 1. Porting the search algorithm to C++

### Why a port, not a wrapper

The ask was a zoomable, pannable map with the graze lines drawn on it --
not achievable by shelling out to `graze_finder.py` and parsing its static
PNG. That meant re-implementing its algorithm (golden-section search for
closest approach, then a latitude-marching root search to trace the actual
graze line) in C++, reusing the existing, already-validated `mg::` engine
(`../../include`, `../../src` -- ephemeris interpolation, reference frames,
time scales) rather than re-deriving DE421 handling from scratch. That
reuse turned out to matter later (see "Bundling the engine files", below).

### Two-stage validation, on purpose

Before any GUI code was written, a headless CLI (`graze_core_test`,
`src/main_cli.cpp`) was built that deliberately mirrors `graze_finder.py`'s
own CLI output format, so the two could be diffed line for line. Only once
that agreed with Python was the Qt GUI built on top of the now-trusted
`graze_core` library. This caught two real bugs before any GUI work even
started -- the kind of thing that's easy to ship silently if you build the
pretty map first and eyeball it for plausibility instead of diffing numbers.

### Bug: date display off by ~10 months

`jd_to_string()` originally used a hand-rolled bisection plus a
Howard-Hinnant-style `civil_from_days` algorithm, seeded with an epoch-shift
constant (`730120`) that was simply wrong -- the correct value is
`~730425` (719468 days from 0000-03-01 to 1970-01-01, plus 10957 days from
1970-01-01 to 2000-01-01). Every displayed date came out about 305 days
early (e.g. an event actually on 2026-09-10 displayed as 2025-11-09).
Fixed by throwing out the bisection approach entirely and using the
standard Meeus Julian-Date-to-Gregorian-calendar algorithm (works directly
on the JD number, no epoch-shift constant to get wrong), verified with
isolated round-trip tests. `mainwindow.cpp`'s `jdToDisplayString()` is a
deliberate duplicate of this, not a shared call into `graze_core` --
kept that way so the core library stays free of GUI string-formatting
concerns.

### Bug: real events silently missed (the Maia case)

A full-year, unfiltered run at the Diepenbeek test site initially found
only 1 raw event where Python found 9 -- and specifically missed a real
event (Maia, December 2026) that showed up clearly in `graze_finder.py`.
Chasing this down took the most effort of anything in the C++ port, and is
worth recording in full because the root cause was two independent, stacked
effects:

**Part 1 -- window-size-dependent minimization.** `margin_at()`'s
golden-section search result genuinely depends on the width of the search
window handed to it. For a near-tangent event, a narrow window converges to
a shallow, boundary-pinned pseudo-minimum; a wider window finds the true
(deeper) minimum, tens of minutes away in time. This is inherent to the
golden-section approach itself, not a coding mistake, and affects the
Python implementation equally.

**Part 2 -- a small geometry difference exposing part 1.** The C++ engine
(matching `frames.hpp`'s documented, deliberate simplification) applies
Earth rotation via GMST only -- no precession/nutation -- while
`graze_finder.py`'s PyEphem backend does the fuller "equinox of date"
reduction. This leaves a residual of a few arcseconds between the two.
Normally this "mostly cancels" for a relative star-to-Moon separation (see
the caveat in `README.md`), but for this specific event it was just enough
to put one latitude sample on the wrong side of zero margin in C++ where
Python's corresponding sample was barely negative -- so the step-march
search saw two same-sign samples straddling a very narrow (well under
0.5 degrees) negative-margin band without ever sampling inside it, and
concluded there was no crossing there at all.

**The actual C++-only bug**, once a fix for the above was attempted: even
after adding bisection to catch a hidden sign change between two close
same-sign samples, `brent_root`'s own re-evaluation of the bracket
endpoints used the *current* iteration's search window, which could differ
from the (narrower) window used when the *previous* march point's margin
was first computed. That mismatch made `f(lo)`/`f(hi)` disagree with the
values that triggered the crossing detection in the first place, so
`brent_root` threw "root not bracketed" even for a genuine crossing.

**Fix, two parts:** `find_hidden_crossing()` -- recursive bisection (depth
4, triggered when both endpoint margins are within `kNearZeroThresholdDeg
= 0.02 deg`) -- catches the narrow hidden sign flip from Part 2.
`bracket_and_root()` was then changed to refresh the *previous* point's
margin using the *current* iteration's window before any sign comparison,
so every margin evaluation feeding a given crossing check uses one
consistent window (removing the `brent_root` bracket mismatch).

**Verification:** after the fix, the same full-year run found the same 9
events as Python, times agreeing to about a second, distances agreeing to
single-digit km (Maia: 101.3 km in C++ vs. 104.5 km in Python; 18 Lam Psc:
46.3 km vs. 53.4 km), Moon/Sun altitude and illuminated fraction matching
closely. This cross-check, and its numbers, are also recorded in
`README.md`'s "Differences from graze_finder.py" section.

### Smaller bug: map centering on row click

An early `onRowSelected()` implementation tried to center the map with a
nonsensical expression (`mapView_->centerOn(mapView_->mapFromScene(...)
...)`) that didn't actually compute anything meaningful. Fixed by adding a
proper public `MapView::centerOnLonLat(lon, lat)` method and rewriting the
handler to find the line point nearest the site and call it correctly.

### Bundling the engine files

The first delivered zip only worked if kept nested exactly two directories
inside a full `moon-graze` checkout, because `CMakeLists.txt` reached out
to `../../src/frames.cpp` and `../../include` by relative path. A user
unzipping just `graze_gui` on its own hit `CMake Error: Cannot find source
file .../src/frames.cpp`. Fixed by copying `frames.{hpp,cpp}`,
`time_utils.{hpp,cpp}`, `star.hpp` and `vec3.hpp` (unmodified) into
`graze_gui/src/` itself and dropping the `MG_ROOT` relative-path
plumbing from `CMakeLists.txt` entirely. Verified by extracting a fresh
copy into a directory with no `../..` structure at all and building both
targets from scratch there.

## 2. Accuracy: how good is a traced line, really

### The documented simplifications

Two are deliberate and written up in `README.md`: no precession/nutation
in the C++ engine's Earth-rotation frame (see the Maia case above for what
that can cost in a knife-edge case), and no atmospheric refraction (matches
the geometric convention IOTA/VVS tables use -- refraction affects the
Moon and a grazed star's *common* altitude almost identically, so it
largely cancels for their relative separation).

The big one, shared with `graze_finder.py`, is **no lunar limb
topography** -- the Moon is modeled as a perfect sphere at its mean radius
(`kMoonRadiusKm = 1737.4`), not the real, mountain-and-valley limb that
makes a graze a graze in the first place. This is why the tool's own
framing is "does a graze line cross near me, roughly" rather than
"exactly where should I stand."

### Case study: 25 Vir vs. the printed Sterrengids 2025

A user cross-check against the printed *Sterrengids 2025* almanac found
the KML-exported line for **25 Vir, 2025-01-20** about 5 km off from the
published position (event circumstances: mag 5.88, closest approach
00:33 UT, Moon alt 16 deg, Sun alt -58 deg, illum 0.67). Investigation:

- Re-ran the exact event independently in both `graze_core_test` (C++) and
  `graze_finder.py` (Python/PyEphem). They agreed with each other to
  within 0.3 km and 17 seconds (75.3 km vs. 75.6 km) -- ruling out a
  bug specific to this event or a regression in either tool.
- Checked the WGS84 geodetic-to-ECEF site-position code (`frames.cpp`) --
  correctly accounts for Earth's oblateness, so that's not the source
  either (a classic cause of exactly this scale of graze-line error when
  it's missing).
- Concluded the ~5 km gap is the spherical-Moon simplification showing up
  at a scale bigger than its usual "few hundred metres to ~1 km" caveat
  suggests -- and that this specific magnitude isn't surprising. The
  original `moon-graze` engine's own DEM-vs-sphere validation work
  (`../../VALIDATION.md`) independently measured mean-sphere-vs-real-terrain
  line shifts landing in the tens-of-kilometres range from perfectly
  ordinary 1-2 km lunar relief (one case there found a 62 km shift traced
  to real terrain about 2 degrees off the naive tangent point). A 5 km
  gap is well inside that already-observed range.

### The attempted DEM/limb-profile correction, and why it stalled

Given that finding, a follow-up ask was to make the KML/CSV *exports*
DEM-corrected -- i.e. trace lines against the Moon's actual limb instead
of the mean sphere. Before writing any code, the feasibility was checked
directly:

- `curl`/network tests from this development sandbox to USGS Astrogeology,
  PDS, NASA Trek, and Zenodo all failed at the proxy level (`connect_rejected`)
  -- the same wall `../../include/limb.hpp`'s own header comment already
  documents from when the original `DemLimbModel` was built. This isn't a
  one-off; the sandbox genuinely cannot reach any current source of real
  lunar topography.
- PyPI and npm's registry *are* reachable from here, but neither hosts real
  planetary elevation data (too large, too specific -- the "moon" packages
  that exist there are photographic textures for 3D-rendering demos, not
  surveyed terrain, and using one would have been actively misleading:
  looks more "real" while being scientifically meaningless).
- GitHub (including raw file serving and Pages), and every general CDN
  tried (jsDelivr, cdnjs, unpkg), were also blocked from this sandbox.

Given that, a full global DEM bake-in from this environment isn't
buildable -- unlike the DE421 ephemeris table, which came from a normal
PyPI package. The architecturally sound path (matching how `../../src`'s
own `DemLimbModel`/`gen_dem_grid.py` already work: one small local grid,
built from LOLA tiles downloaded on a machine with normal internet access,
for one already-known event) was scoped out as a "refine this specific
result with real terrain" feature, but was **not implemented** -- the user
asked to set it aside for now. If picked back up, `include/limb.hpp` and
`src/limb.cpp` are the reusable, already-validated pieces; the work would
be wiring a `DemLimbModel`-based re-trace into LunarGraze for one selected
event/line, not touching the broad multi-star search.

## 3. KML/CSV export

Added directly in response to "can I map a graze on Google Earth": `.kml`
with an observer-site `Placemark`/`Point`, the search radius drawn as a
`LineString` ring (~72 points, same flat-Earth approximation the rest of
the tool already uses via `gg::km_per_deg_lon()`), and one
`Placemark`/`LineString` per graze line with a `CDATA` description (star,
magnitude, time, distance, Moon/Sun altitude, illuminated fraction, star
alt/az) -- all XML-escaped where it isn't inside `CDATA` (which is
verbatim text and must *not* be entity-escaped, an easy mistake to make).
`.csv` mirrors the CLI's own `--csv-out` column layout so the two are
directly comparable. Both wired to `QFileDialog::getSaveFileName()`,
enabled only once a search has results.

## 4. Making the map actually render: a longer story than expected

### Starting point

`MapView` (`src/mapview.{hpp,cpp}`) is a `QGraphicsView`-based slippy map:
standard Web-Mercator "z/x/y" tiling, tiles fetched via
`QNetworkAccessManager` and cached to disk, site marker/radius
circle/graze lines drawn as overlay items in the same projection. This
part worked and was never the problem.

### Report 1: "only blue, zooming out doesn't help"

The first symptom -- a flat pale blue-grey area regardless of zoom level --
is the `QGraphicsView` background brush (`setBackgroundBrush(QColor(0xdd,
0xe6, 0xf0))`), i.e. what shows *before* any tile has loaded, not a real
"water" tile. Response: added a scale bar (drawn in screen coordinates via
an overridden `paintEvent`, independent of the current zoom transform) and
made tile failures visible instead of silent -- a `tileLoadError` signal
driving both a red on-canvas banner and a log-panel line, since the
previous behaviour was to just drop a failed `QNetworkReply` with no
visible trace at all.

Also fixed while investigating: the app never called
`QNetworkProxyFactory::setUseSystemConfiguration(true)`, meaning it always
connected directly and ignored any configured system/corporate HTTP(S)
proxy -- a real, independent bug, fixed in `main.cpp` regardless of
whether it was the actual cause here.

### Report 2: "high data transfer, but still just blue"

The user's own packet capture showed real TLS traffic to a Fastly-fronted
host (`dualstack.n.sni.global.fastly.net` -- confirmed via search to be
OSM's real CDN front, not something to be suspicious of) but still no
rendered tiles. Diagnosis added per-tile logging (request dispatch, HTTP
status, byte count, decode success) to distinguish "never got a response"
from "got a response that wasn't a usable image." Also found and fixed a
real, independent bug here: zooming cleared the app's *bookkeeping* of
in-flight tile requests (`pendingTiles_`) without actually cancelling the
underlying `QNetworkReply` objects, so repeated zooming piled up abandoned
downloads that would complete (or eventually time out) long after they
stopped mattering -- wasted bandwidth, never shown. Fixed by tracking
`activeReplies_` and calling `abort()` on the stale ones in `setZoom()`. A
15-second `setTransferTimeout()` was also added so a connection that
completes its TLS handshake but then never finishes the HTTP response
doesn't sit "pending" forever with no visible sign anything is wrong.

None of this turned out to be the actual bug (see below), but all of it
was worth keeping: real robustness fixes independent of the root cause.

### Report 3: OSM's tile server actively 403s the app

A screenshot showed OSM's own "Access blocked -- App is not following the
tile usage policy" page. This was the real, if partial, explanation for
reports 1-2: OSM operations has tightened enforcement on
`tile.openstreetmap.org` to the point of blocking non-browser clients more
or less regardless of compliance details (see
`wiki.osmfoundation.org/wiki/Blocked_tiles`). Response: switched the
default tile provider from raw OSM tiles to
[MapTiler](https://www.maptiler.com/), whose terms explicitly support an
app distributed to end users who each supply their own free API key --
added a "Map tiles" section to the UI (key field, "Get a free key..."
button opening MapTiler's signup page, persisted via `QSettings`), scoped
the disk tile cache by provider so old OSM tiles could never mix with new
MapTiler ones, and updated the on-map attribution text to what MapTiler's
terms require (`© MapTiler © OpenStreetMap contributors`). Also: no API
key configured now means zero tile requests are fired at all (an
immediate, clear on-canvas message instead) -- deliberately, since
hammering a server with requests already known to be doomed is exactly
the kind of traffic pattern that got the OSM endpoint to block the app in
the first place.

### Report 4: MapTiler tiles *also* silently never load

Same symptom against a completely different provider: requests logged as
sent, never a single completion line -- not even an error, not even after
the 15-second timeout should have fired. Working hypothesis at the time:
`QNetworkAccessManager`'s HTTP/2 support has known issues where a stream
to certain CDN configurations can stall indefinitely mid-multiplex,
bypassing the transfer timeout (which applies at the connection level, not
inside a stuck h2 stream) -- both OSM (Fastly) and MapTiler (Cloudflare)
terminate HTTP/2, which fit. Added `req.setAttribute(
QNetworkRequest::Http2AllowedAttribute, false)` to force HTTP/1.1, and
verified the *mechanism* worked (a standalone test forcing HTTP/1.1
against a real HTTP/2-capable server completed correctly) -- but this
did **not** fix the actual problem. Recorded here specifically as a
plausible-looking, partially-verified hypothesis that turned out to be a
red herring, in case anyone re-encounters the "silent hang" pattern and is
tempted to chase HTTP/2 again: it's still a real Qt bug class in general,
just not what was happening here.

### The actual root cause: a signal/slot wiring mistake

Requested by the user: run `curl -v` against the exact failing MapTiler
URL, outside the app entirely. It succeeded instantly -- clean TLS 1.3
handshake, HTTP/2, 200 OK, correct PNG bytes. That single data point broke
the "it's a network/CDN/HTTP2 problem" theory completely: if curl gets a
perfect response in under a second, nothing on the wire or at the server
is actually wrong.

The real bug was entirely inside `onTileReply()`. The constructor connects
`QNetworkAccessManager::finished(QNetworkReply*)` to a slot declared as
`void onTileReply()` -- no parameters -- which then tried to recover the
reply via `qobject_cast<QNetworkReply*>(sender())`. That's the bug:
`sender()` returns whichever `QObject` is emitting the *currently handled*
signal, which for `QNetworkAccessManager::finished` is always the manager
itself, never the `QNetworkReply` the signal is *about*. So the cast
always produced `nullptr`, and the very first line of the handler --
`if (!reply) return;` -- fired unconditionally, before any logging,
before checking success or failure, before anything. Every tile request
could have succeeded perfectly over the network and the app would still
show nothing and log nothing, which is exactly what every report above
actually looked like from the outside. It's also why forcing HTTP/1.1
made no difference: the bug had nothing to do with what happened on the
wire.

This was verified, not just reasoned about: a standalone Qt test
reproduced the exact wiring (`QNetworkAccessManager::finished` connected
to a no-argument slot recovering the reply via `sender()`) against a real
server and confirmed the reply pointer came back null; the same test with
the corrected wiring -- the slot taking `QNetworkReply*` as its own
parameter, matching the signal's signature, so Qt passes it through
directly instead of needing `sender()` at all -- came back with a valid
pointer, correct HTTP status, and correct byte count.

**Fix:** `onTileReply(QNetworkReply* reply)` takes the reply as a real
parameter; the `connect()` call itself didn't need to change, since Qt
already matches signal/slot parameter lists structurally. Checked the rest
of the codebase for the same `qobject_cast<...>(sender())` pattern
elsewhere -- this was the only instance.

**Lesson for next time:** `sender()` recovers the object whose signal is
firing, not any argument that signal happens to carry. For any signal that
passes "which thing this event is about" as a parameter (as
`QNetworkAccessManager::finished` does), take that parameter directly in
the slot rather than trying to re-derive it from `sender()`.

### Cleanup

Once the root cause was fixed and confirmed, the verbose per-tile
diagnostic logging added while chasing reports 1-4 (`tileLog` signal:
per-request dispatch lines, per-completion HTTP-status/byte-count lines,
the `refreshTiles()` visible-range summary line) was removed -- it was
debugging instrumentation for a specific investigation, not something a
normal user needs to see on every pan/zoom. The genuinely useful pieces
were kept: the `tileLoadError` signal (red banner + one log line on a real
failure, and a one-line "loading OK" note on recovery), the system-proxy
fix, the stale-request cancellation on zoom, and the transfer timeout --
all real, independent improvements that came out of this investigation
even though none of them were the actual bug.
