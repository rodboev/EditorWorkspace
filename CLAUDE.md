# Windhawk Mod Development

This directory contains Windhawk, a Windows customization tool that loads C++ mods
as DLLs into target processes (usually `explorer.exe`).

## Active mod: add-virtual-folders-to-nav-top

The primary mod under development adds Desktop and This PC to the top of Explorer's
navigation pane. It hooks `CNscTree::AppendRoot` and `CNscTree::s_SubClassTreeWndProc`
in `ExplorerFrame.dll`.

### Key files

| File | Purpose |
|------|---------|
| `AppData\EditorWorkspace\mod.wh.cpp` | **Working file** — opened when you click "Edit" in Windhawk. Ephemeral: Windhawk replaces it each time. |
| `AppData\ModsSource\local@add-virtual-folders-to-nav-top.wh.cpp` | Persistent local copy. This is what gets pushed to the fork. |
| `C:\Dropbox\Projects\windhawk-mods-fork\` | GitHub fork clone (`rodboev/windhawk-mods`, branch `add-virtual-folders-to-nav-top`). PR #4159 to `ramensoftware/windhawk-mods`. |

### Data model

Items are identified by `NavItemId` (`NAV_THISPC=0`, `NAV_DESKTOP=1`,
`NAV_HOME=2`, `NAV_GALLERY=3`, `NAV_COUNT=4`). Only This PC and Desktop
are "insertable" (`IsInsertableItem`); Home and Gallery are "inherited"
(added by Explorer, managed by our code).

`NavItemDef g_navItems[NAV_COUNT]` holds per-item PIDL and display label.
`Settings::items[NAV_COUNT]` (type `NavItemSettings`) holds per-item
settings: `showAtTop`, `expandable`, `startExpanded`, `hideFromQA`,
`hide`. `Settings` also has cross-item fields: `desktopAboveThisPC`,
separator toggles, chevron/pin settings, and the precomputed
`hasItemsAtTop` guard.

### Helpers and RAII types

- **`ComRef<T>`**: Lightweight COM pointer wrapper. AddRefs on
  construction (unless `addRef=false`), Releases on destruction.
  Move-only.

- **`RootShellItems`**: Creates `IShellItem` wrappers for all known
  PIDLs. `Create()` returns false if the Desktop item fails.
  `RemoveInsertableRoots(pNsc)` calls `RemoveRoot` for This PC and
  Desktop in a retry loop.

- **`InsertionScope`**: RAII wrapper for the insert sequence. Constructor
  AddRefs `pNscTree` and `pFilter` from `TreeState`, sets
  `g_ins.pNsc`/`g_ins.enumFlags`/`g_ins.forTree`/`g_inCustomAppend`.
  Destructor clears the globals.

- **`NavItem`** + **`BuildItemOrder`**: Struct holding pidl, expandable,
  enabled, id, style for one insertable item. `BuildItemOrder` fills a
  2-element array respecting `desktopAboveThisPC`.

- **`InsertItems`**: Iterates the `NavItem` array in reverse (so
  `TVI_FIRST` produces correct visual order), sets `g_ins.item`
  for each, calls `AppendOneItem`.

- **`InsertOurItems`** + **`InsertFlags`**: The single consolidated
  insert path. `InsertOurItems(hTree, flags)` does delete + re-insert
  + redraw under one set of options. The `InsertFlags` enum:
  `INS_DELETE_FIRST` (delete existing items first),
  `INS_DEFER_IF_EMPTY` (bail via `WORK_HOT_INSERT` if the tree has no
  depth-1 child yet), `INS_RESET_SEPCOLOR` (reset separator color),
  `INS_UPDATE_NOW` (`RedrawWindow` with `RDW_UPDATENOW`),
  `INS_DEFERRED_GUARD` (set `g_deferredOpInProgress` + call
  `DrainPendingRebuilds`). `RefreshNavPane` calls it with
  `INS_DELETE_FIRST | INS_DEFERRED_GUARD | INS_UPDATE_NOW`;
  `HotEnableInsert` with `INS_DEFER_IF_EMPTY | INS_RESET_SEPCOLOR`.

- **`RunDeferredWork`**: Signature
  `RunDeferredWork(HWND, TreeState*, uint8_t flag, void(*op)(HWND))`.
  If the flag is set, clears it, runs `op`, re-fetches `ts` via
  `GetTree`, and returns it. Used only for `WORK_FULL_REBUILD`,
  `WORK_HOT_INSERT`, and `WORK_HOME_SPACER`; the other deferred steps
  are inline `if` blocks (see Deferred work dispatch).

- **`FindSectionLayout`**: Walks depth-1 items to find `boundaryItem`
  and `belowQAItem`. Also serves as the fallback cache walk for
  Home/Gallery handles (replacing the old `CacheHomeGalleryHandles`):
  when `ts.hItems[NAV_HOME]`/`ts.hItems[NAV_GALLERY]` are null, matches
  by PIDL-derived display name and caches the handle.

- **`ForEachDepth1Item`**: Template walker over depth-1 siblings.
  Visitor returns false to break.

- **`BuildMatchList`**: Template that builds a `WCHAR[][64]` array of
  display names for items matching a predicate. Used by dedup, collapse,
  and restore paths.

### Multi-window architecture

Per-tree state is stored in `std::unordered_map<HWND, TreeState> g_trees`.
Each `TreeState` holds: `pNscTree`, `enumFlags`, `pFilter`, item handles
(`hItems[NAV_COUNT]`), section state (`hiddenDuplicate`, `homeSpacerItem`,
`boundaryItem`, `belowQAItem`), pending work (`pendingWork` — bitmask of
`WORK_FULL_REBUILD`, `WORK_HOT_INSERT`, `WORK_QA_CLEANUP`,
`WORK_HG_CLEANUP`, `WORK_DUP_COLLAPSE`, `WORK_EXPAND`,
`WORK_HOME_SPACER`), and lifecycle state (`ownsNscRef`,
`triedHomeSpacer`, `savedStateImageList`).
`GetTree(HWND)` returns a pointer or null.
`GetHiddenItem(ts)` returns `ts.hiddenDuplicate ? ts.hiddenDuplicate
: ts.homeSpacerItem` — the single item (a kept QA dup OR an injected
Home spacer) that, when present, occupies the below-nav boundary and
gets painted over with a separator line drawn through it.

**Lifecycle**: `g_trees` entries are created via `try_emplace` in the
`TVM_INSERTITEM` handler (not `operator[]` — avoids inserting defaults).
Entries are erased in `SubClassTreeWndProc_hook`'s `WM_NCDESTROY`
handler when the tree window is destroyed. `Wh_ModUninit` also calls
`g_trees.clear()` after processing all trees.

**Iterator invalidation**: `AppendRoot_orig` pumps messages internally,
which can trigger `TVM_INSERTITEM` on other trees and rehash `g_trees`.
`Wh_ModSettingsChanged` and `Wh_ModUninit` snapshot HWNDs into a
`std::vector` first. After any call that pumps messages, `ts` pointers
must be re-fetched via `GetTree()`.

**`g_ins.forTree`**: Scopes `g_ins.item` to a specific tree
HWND so TVM_INSERTITEM messages from other trees during message pumping
are ignored.

**`g_mutatingTree`**: Set to the HWND of the tree currently being
mutated (during the `WM_DEFERRED_REBUILD` dispatch, `FullRebuildTree`,
and the `INS_DEFERRED_GUARD` insert path). `SepParentSubclassProc`
ignores `NM_CUSTOMDRAW` when `hTree == g_mutatingTree`, preventing
cross-tree paint side effects during multi-window rebuilds. Cleared by
`DrainPendingRebuilds`.

**`g_deferredOpInProgress`**: Re-entrancy guard set during the
`WM_DEFERRED_REBUILD` dispatch, the `INS_DEFERRED_GUARD` insert path
(`RefreshNavPane`), and the `Wh_ModSettingsChanged` TIER_REBUILD branch
around `FullRebuildTree`. Prevents `NM_CUSTOMDRAW` handling, `WM_PAINT`
deferred ops, and `WM_DEFERRED_REBUILD` from running during tree
mutation.

### Architecture

- **AppendRoot_hook**: Intercepts `CNscTree::AppendRoot`. Home/Gallery
  PIDL check runs **BEFORE** the `g_inCustomAppend` guard — this is
  critical because during `FullRebuildTree`, `AppendRoot_orig` for the
  hidden root can trigger Explorer to add Home/Gallery internally while
  `g_inCustomAppend` is true. Without this ordering,
  `ts.hItems[NAV_HOME]`/`ts.hItems[NAV_GALLERY]` would never be cached
  via the hook. When the hidden root (rootStyle & 0x1) is added, uses
  `InsertionScope` + `BuildItemOrder` + `InsertItems` to add
  Desktop/This PC as children via `TVI_FIRST`. Uses `g_inCustomAppend`
  (static, not thread_local) as a re-entrancy guard.

- **g_ins** (`static InsertionCtx`): Consolidates the per-insert
  globals into one struct: `{ int item = -1; void* pNsc; unsigned long
  enumFlags; IShellItemFilter* filter; HWND forTree; }`. `g_ins.item`
  identifies which item is being inserted so the `TVM_INSERTITEM`
  handler can cache the HTREEITEM without comparing localized text.
  Values: `-1=none`, `NAV_THISPC=0`, `NAV_DESKTOP=1`, `NAV_HOME=2`,
  `NAV_GALLERY=3`. Cleared inside the handler after caching.
  `g_ins.forTree` scopes the insert to one tree HWND.

- **FindSectionLayout** (replaces old `CacheHomeGalleryHandles`):
  Fallback walk for Home/Gallery handles that couldn't be cached via the
  hook. Called in WM_PAINT via `FindSectionLayout` when
  `ts.hItems[NAV_HOME]`/`ts.hItems[NAV_GALLERY]` are null and the items
  aren't hidden. Also finds `boundaryItem` and `belowQAItem` in the same
  walk. Needed because: (1) COM vtable calls bypass the inline hook, so
  Explorer's internal `AppendRoot` for Home/Gallery may not fire our
  hook; (2) after `FullRebuildTree`, `RemoveRoot(pDesktop)` destroys
  all children of the hidden root (including Home/Gallery) — their old
  handles become stale and must be re-discovered.

- **SubClassTreeWndProc_hook**: Intercepts `CNscTree::s_SubClassTreeWndProc`.
  Handles `TVM_INSERTITEM` (TVI_FIRST positioning, spacer insert-after
  via `g_spacerInsertAfter`, and handle caching), `TVM_SETITEMW`
  (separator collapse + hidden item clamping + Home/Gallery iIntegral
  clamping), interaction blocking on the hidden item (`GetHiddenItem`:
  click, context menu, cursor, keyboard skip), `WM_DEFERRED_REBUILD`,
  `WM_PAINT` (deferred work dispatch, dedup, Home/Gallery removal, handle
  caching via `FindSectionLayout`, iIntegral collapse, Home spacer
  trigger, visible dup collapse, pin hiding, chevrons, SEP-VERIFY), and
  `WM_NCDESTROY`.

- **Hot enable** (`Wh_ModAfterInit`): `Wh_ModInit` only installs the
  hooks; discovery happens in `Wh_ModAfterInit`, on the DLL loader
  thread (not the UI thread). `EnumWindows` walks top-level windows in
  the current process. For each `CabinetWClass` OR `#32770` (modern
  dialog) window it enumerates **all** `ShellTabWindowClass` children
  (multi-tab Explorer), gets an `IShellBrowser` per tab via
  `SendMessage(WM_USER+7)`, and calls `DiscoverTreeFromBrowser`
  (IShellBrowser → IServiceProvider → INameSpaceTreeControl →
  IOleWindow::GetWindow → `FindWindowExW(L"SysTreeView32")`). It dedups
  by `hTree` (the active tab can resolve through more than one host).
  If no `ShellTabWindowClass` child is found, it falls back to the top
  window's own `IShellBrowser`. Each discovered tree gets a `TreeState`
  entry with `ownsNscRef=true` (the COM ref is AddRef'd and must be
  Released in `Wh_ModUninit`), `WORK_FULL_REBUILD` in `pendingWork`,
  and a posted `WM_DEFERRED_REBUILD` to defer to the UI thread.
  Deferral is necessary because `AppendRoot_orig` (trampoline) bypasses
  the inline hook, so `TVM_INSERTITEM` subclass doesn't fire from the
  loader thread. On the UI thread, insertion is intra-thread and all
  hooks work.

- **Hot disable** (`Wh_ModUninit` via `RestoreTree`): Snapshots all tree
  HWNDs, then for each: removes the Home spacer (if any) via
  `RemoveRoot`, `RemoveInsertableRoots` via `RootShellItems`,
  `AppendRoot_orig` for hidden root with zeroed settings (hook passes
  through), collapses expanded duplicates. Restores
  `savedStateImageList` for pin icons. Removes `TreeInteractionProc`
  subclass. Releases COM refs only for trees with `ownsNscRef=true`.
  Removes all parent subclasses. Clears `g_trees`.

### Separator system

CDDS_PREPAINT suppression fires whenever `hasItemsAtTop` (at least one
of our items is enabled) OR `hasDupHide`. This hides ALL native separator
lines. `RedrawSeps` at CDDS_POSTPAINT redraws only the wanted
ones.

Three mechanisms control which separators appear:

- *Internal separator* (`ShouldRemoveInternalSep`): Between Desktop and
  This PC. Fires when **both** items are enabled. Collapses iIntegral on
  the visually lower item via `TVM_SETITEMW` interception. Home/Gallery
  are treated as part of this section (no separator between our items
  and them).

- *Below-nav separator* (`removeSepBelowNav`): Between our section
  (including Home/Gallery) and Quick Access. The boundary item is the
  first non-our/non-HG depth-1 item after our section. The hidden item
  — `GetHiddenItem(ts)`, i.e. a kept QA duplicate OR an injected Home
  spacer — sits at the boundary, has iIntegral=1, is painted over with
  bg color, and gets the separator line drawn through it at
  CDDS_POSTPAINT. When `removeSepBelowNav=0` and **no** hidden item
  exists, the separator is drawn at `rc.top` of the boundary item (or
  `rc.top + baseHeight/2` if naturally tall). When `removeSepBelowNav=1`:
  all dups deleted, boundary item iIntegral collapsed to 1, no separator
  drawn. Boundary iIntegral is collapsed when `removeSepBelowNav ||
  GetHiddenItem(ts)` (the hidden item provides the separator space
  instead). `RedrawSeps` draws the boundary separator when
  `!removeSepBelowNav && !GetHiddenItem(ts)`; otherwise the line is
  drawn through the hidden item. Adjacency of the hidden item to our
  section is checked via `TVGN_PREVIOUS` (previous SIBLING, not
  `TVGN_PREVIOUSVISIBLE` which walks into expanded children).

- *Below-QA separator* (`removeSepBelowQA`): Below Quick Access.
  Collapses the first tall item after the boundary via `belowQAItem`.
  `RedrawSeps` skips it via `foundBelowQA` flag.

**Home/Gallery as part of our section**: Home and Gallery (when visible)
are treated identically to our items in `RedrawSeps` — tagged
`H`/`G`. Their iIntegral is clamped to 1 at three levels: post-insertion
in `TVM_INSERTITEM` handler, `TVM_SETITEMW` interception, and `WM_PAINT`
active collapse.

**Home spacer subsystem** (`InsertHomeSpacer`): When Desktop is the
bottom of our section (alone, or below This PC), Home/Gallery are
hidden, no QA dup sits at the boundary, and a below-nav separator is
wanted, Explorer sometimes creates NO separator between our section
and Quick Access. `InsertHomeSpacer` injects a real Home (or, if no
Home PIDL, Gallery) namespace item via `AppendRoot_orig`, positioned
right after Desktop using the `g_spacerInsertAfter` global (consumed
in the `TVM_INSERTITEM` handler), collapses it to iIntegral=1, and the
CDDS_POSTPAINT paint-over fills it with background and draws the
separator line through it. State: `homeSpacerItem` (the spacer's
handle), `triedHomeSpacer` (one-shot guard so we attempt insertion
once per layout), `WORK_HOME_SPACER` (deferred trigger, set in WM_PAINT
when the gap condition holds). The spacer IS part of our section
(`IsOurSection` returns true for it). It is removed inline in BOTH
`FullRebuildTree` and `RestoreTree` (a separate `RemoveRoot` before the
insertable roots). Interaction blocking treats it like
`hiddenDuplicate` (both surfaced via `GetHiddenItem`).

**Boundary item walk** (WM_PAINT via `FindSectionLayout`): Always runs
when `hasItemsAtTop` and `g_sepColor` is valid. Walks depth-1 children
to find `boundaryItem` (first non-our/non-HG/non-hidden item after our
section). Collapses boundary iIntegral when `removeSepBelowNav=1` OR
`GetHiddenItem(ts)` (the hidden item provides the space instead). When
`removeSepBelowNav=0` and no hidden item, the boundary item keeps its
natural iIntegral (may be 2 for native section boundaries, providing
separator space). Also finds `belowQAItem` when `removeSepBelowQA=1`.
The `TVM_SETITEMW` hook clamps both to iIntegral=1, but the boundary
clamp is conditional: only when `removeSepBelowNav || GetHiddenItem(ts)`
(otherwise the boundary needs its natural height for the separator).
When the gap condition holds (`!triedHomeSpacer` and Desktop is at the
bottom and no hidden item and the boundary exists), the walk sets
`WORK_HOME_SPACER` and posts `WM_DEFERRED_REBUILD`.

**Walk log**: `RedrawSeps` builds a diagnostic string like
`walk=[OOPDB....S]` where each depth-1 item gets a tag: `O`=ours,
`P`=spacer (`homeSpacerItem`), `H`=Home, `G`=Gallery, `D`=hidden item
(via `GetHiddenItem`), `B`=boundary(tall), `b`=boundary(short),
`Q`=belowQA(skipped), `S`=sep drawn, `.`=other.

- **Dedup** (`CleanupDups`): Walks depth-1 children of
  the hidden root. Matches items by text against our items' display
  names. Native sections (with children) are always deleted. Childless
  pins: when `removeSepBelowNav=0`, a match is kept as `hiddenDuplicate`
  (iIntegral=1) **only if it is at `hBoundaryPos`** (first non-our/non-HG
  item after our section, found via `FindSectionLayout`). Non-boundary
  dups are deleted — keeping them would leave an invisible gap in QA or
  the bottom nav. When `removeSepBelowNav=1`, all are deleted. Returns
  false if not all expected duplicates were found (async population race),
  causing retry on next WM_PAINT. Only matches items whose parent setting
  is enabled (e.g., `items[NAV_THISPC].showAtTop && items[NAV_THISPC].hideFromQA`).
  **Home/Gallery handle-null timing**: The `hBoundaryPos` walk uses
  `IsOurSection` to skip our items, but `ts.hItems[NAV_HOME]`/
  `ts.hItems[NAV_GALLERY]` may be null at dedup time (COM vtable
  bypass — see FindSectionLayout). To prevent Home/Gallery from being
  misidentified as the boundary, the walk falls back to PIDL-derived
  display name matching via `GetPidlDisplayName` when handles are null.

- **Visible QA duplicate collapse**: In WM_PAINT, via `WORK_DUP_COLLAPSE`
  flag. Uses `BuildMatchList` + `CollapseMatchingItems` to send
  `TVE_COLLAPSE` on expanded items matching our items' text (when shown
  at top but NOT hidden from QA). Prevents visible QA duplicates from
  mirroring the expanded state of our copies.

- **Home/Gallery removal** (`RemoveInherited`): Fallback walk
  for async-populated items that arrive after `AppendRoot_hook`
  suppression. Matches by display name. Hidden→visible transitions
  require full tree rebuild since the items are children of the hidden
  root. When the settings change path hides Home/Gallery (visible→hidden),
  it nulls the cached `hItems[]` handles first so `IsOurSection` won't
  skip the items during the walk.

- **Hidden item paint-over** (CDDS_POSTPAINT): Fills the hidden item's
  rect (`GetHiddenItem(ts)` — kept dup or spacer) with background color
  (2-level fallback: `GetPixel(hdc, 1, rcHide.top+2)` → `GetSysColor(
  COLOR_WINDOW)`). The separator line is drawn through it when the
  hidden item is the `homeSpacerItem`, OR when its `TVGN_PREVIOUS`
  sibling resolves to an item in `IsOurSection` (our items or
  Home/Gallery).

- **Hidden item interaction blocking** (`TreeInteractionProc`): A
  `SetWindowSubclass` callback installed on the tree during WM_PAINT
  when `GetHiddenItem(ts)` is set. Blocks right-click, right-button-up,
  and mouse-move on the hidden item. The `SubClassTreeWndProc_hook`
  handler also blocks left-click, context menu, and cursor changes,
  and skips it on VK_UP/VK_DOWN keyboard navigation.

### Separator color lifecycle (`g_sepColor`)

`g_sepColor` is **theme-dependent, not settings-dependent**. It is
**derived synthetically** from the tree background by
`DeriveSepColor(hTree)`: reads `TVM_GETBKCOLOR` (falling back to
`COLOR_WINDOW`), then lightens it by a fixed delta if the background is
dark or darkens it if light. There is no native-pixel sampling — the
old `SampleSeparatorColor` / two-paint-cycle / >200-RGB plausibility
check are gone. Resetting the color (via `ResetSepColor()`, which sets
`g_sepColor = CLR_INVALID`) therefore does NOT need a native-sampling
flash: `CDDS_PREPAINT` still suppresses native separators, and at
`CDDS_POSTPAINT`, if `g_sepColor == CLR_INVALID && hasItemsAtTop`, the
color is derived and `RedrawSeps` draws the wanted ones.

**Reset sites**:
- Static initialization (`= CLR_INVALID`)
- `WM_THEMECHANGED` / `WM_SYSCOLORCHANGE`
- `HotEnableInsert` (via `INS_RESET_SEPCOLOR` in `InsertOurItems`)
- `FullRebuildTree`
- `Wh_ModUninit`
- Separator settings changed (`Wh_ModSettingsChanged`, when
  `removeSepBelowNav`/`removeSepBelowQA` changed)

**NOT reset** on: `LoadSettings`, `RefreshNavPane`.

**SEP-VERIFY** (WM_PAINT, after `DefSubclassProc`): the handler records
the drawn separator's Y (`g_lastSepY`) and `GetPixel`s that position. If
the pixel doesn't match `g_sepColor` — or a separator was expected at
the boundary but none was drawn (`g_lastSepY == -1` with a boundary and
no hidden item and `!removeSepBelowNav`) — it calls `InvalidateRect` to
repaint, capped at `g_sepRetries < 3`. This single corrective repaint is
load-bearing because Explorer overpaints unpredictably.

### Settings change handling (`Wh_ModSettingsChanged`)

Uses `ClassifySettingsChange(prev, cur)` to determine the appropriate
tier. Snapshots all tree HWNDs from `g_trees` into a `std::vector`,
then applies the tier to **each tree**. The snapshot is required because
`FullRebuildTree` pumps messages and can rehash `g_trees`.

Three tiers of response (`ChangeTier` enum) based on what changed:

1. **TIER_REBUILD** (RemoveRoot + AppendRoot via `FullRebuildTree`):
   For transitions where `RefreshNavPane` can't restore deleted/modified
   items:
   - `hide` hidden→visible (children of hidden root)
   - `removeSepBelowNav` toggled in either direction (on→off: deleted
     QA dup can't be re-created; off→on: hiddenDuplicate must be
     deleted and boundary recalculated)
   - `removeSepBelowQA` on→off (collapsed iIntegral can't be restored)
   - `hideFromQA` toggled in either direction (when parent enabled)
   - `showAtTop` off (when hideFromQA was on: QA dups were deleted
     and can't be restored without rebuild)
   - "Desktop is at the bottom of our section" changes between
     prev/cur (computed as `desktop.showAtTop && (!thisPC.showAtTop
     || !desktopAboveThisPC)`): the Home spacer collapsed the boundary
     iIntegral, so the tree must be rebuilt to restore it

2. **TIER_REFRESH** (`RefreshNavPane`): Item insertion parameters changed
   (enable/disable an item, expandable, startExpanded, order). Deletes
   our items, re-inserts via `InsertionScope`, redraws.

3. **TIER_REPAINT** (InvalidateRect): Cleanup flag or visual changes
   (`hide` visible→hidden, separator toggles, chevrons, pins). Sets
   relevant `pendingWork` flags; WM_PAINT handles everything without
   item re-insertion. For Home/Gallery visible→hidden, nulls the cached
   `hItems[]` handles so `RemoveInherited` can find them.

**Sub-settings are ignored when parent is disabled**: Toggling
`expandable` while `showAtTop=0` is a no-op. `desktopAboveThisPC` only
matters when both items are at top. Dedup sub-settings only matter when
their item is enabled.

**`FullRebuildTree` clears ALL item handles** (`hItems[0..NAV_COUNT-1]`)
because `RemoveRoot(pDesktop)` destroys the hidden root and all its
children. `FindSectionLayout` re-discovers Home/Gallery on the next
WM_PAINT.

### Deferred work dispatch

`WM_DEFERRED_REBUILD` (`WM_APP + 0x101`) is the deferred work message,
posted from `Wh_ModAfterInit` (hot enable), the WM_PAINT gap check, and
`DrainPendingRebuilds`. The `SubClassTreeWndProc_hook` handler sets
`g_deferredOpInProgress` and `g_mutatingTree`, then dispatches pending
work in this order: `WORK_FULL_REBUILD`, `WORK_HOT_INSERT`,
`WORK_QA_CLEANUP`, `WORK_HG_CLEANUP`, `WORK_DUP_COLLAPSE`, `WORK_EXPAND`,
`WORK_HOME_SPACER`. Only `WORK_FULL_REBUILD`, `WORK_HOT_INSERT`, and
`WORK_HOME_SPACER` go through `RunDeferredWork`; the rest are inline
`if` blocks. The QA and HG blocks gate on settings preconditions (the
parent item enabled + hideFromQA, or Home/Gallery hidden) and clear
their flag only when their cleanup returns true, retrying on the next
paint otherwise. WM_PAINT re-posts `WM_DEFERRED_REBUILD` whenever any
flag in a `DEFER_MASK` is still set.

`DrainPendingRebuilds` clears `g_mutatingTree` and flushes
`g_pendingRebuildTrees` — trees that received `WM_DEFERRED_REBUILD`
while `g_deferredOpInProgress` was true get re-posted after the guard
drops.

### Critical cross-thread details

- `g_ins` and `g_inCustomAppend` are `static` (not `thread_local`).
  `Wh_ModInit`/`Wh_ModAfterInit` run on the loader thread; hooks fire on
  the UI thread. `thread_local` makes the UI thread see default values.

- `AppendRoot_orig` is a trampoline that bypasses the inline hook. COM
  vtable calls also bypass it. Only Explorer's own internal calls go
  through the hook.

- The hidden root's children (including Home, Gallery, QA items) are
  populated **asynchronously** after `AppendRoot_orig` returns. Dedup
  and Home/Gallery cleanup must handle items arriving late via
  `pendingWork` flags (`WORK_QA_CLEANUP`, `WORK_HG_CLEANUP`).

### Settings groups

Settings are organized into groups in the metadata: `ThisPC`, `Desktop`,
`HomeGallery`, `Separators`, `Resources`. The `removeSepBelowNav`/
`removeSepBelowQA` settings are under `Separators`. Load paths must match
the YAML key hierarchy (e.g. `Wh_GetIntSetting(L"Separators.removeSepBelowNav")`),
not the `$name`.

"Show This PC" being checked refers to the Explorer context menu, not the
mod setting `thisPCAtTop=1`.

### Crash debugging

When investigating crashes, **use the debugger first, not guesswork**.
The target process is explorer.exe; crashing it repeatedly is expensive
(frozen taskbar, lost state, manual recovery).

1. Enable WER crash dumps (requires admin once):
   ```powershell
   Set-Service WerSvc -StartupType Manual; Start-Service WerSvc
   Set-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting" -Name Disabled -Value 0
   $p = "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\explorer.exe"
   New-Item -Path $p -Force | Out-Null
   Set-ItemProperty $p -Name DumpFolder -Value "$env:LOCALAPPDATA\CrashDumps" -Type ExpandString
   Set-ItemProperty $p -Name DumpType -Value 2 -Type DWord
   Set-ItemProperty $p -Name DumpCount -Value 5 -Type DWord
   ```
2. Reproduce the crash once.
3. Analyze the dump with cdb (`C:\Apps\Tools\SDK\cdb.exe`):
   ```
   cdb -z <dump> -c ".ecxr; kp 30; q"
   ```
   This gives the faulting instruction, registers, and full stack trace
   with parameter info. One crash, one stack trace, root cause identified.
4. Do NOT guess at crash causes from symptoms or error dialog text alone.
   The dialog only shows the faulting address; the stack trace shows the
   actual call chain and which function/line caused it.

### Rebuild cascade debugging protocol

Cascades are always caused by this mod's own code. No cross-mod
conflict has ever been confirmed. Other mods' hooks (paste-clipboard-
content-to-explorer, translucent-windows, AquaSnap) appear in every
stack trace; they are noise, not causes. Do not analyze their hook
architecture or recommend disabling them. Check Windhawk logs for
pendingWork flags and completion counts before analyzing stack traces.

When a cascade occurs:
1. Attach cdb non-invasively (-pv, not -p, which crashes explorer).
2. Check the stack depth. If shallow (2-3 frames of our code), it is
   NOT recursion; it is infinite repaint from repeated invalidation.
   Do not add re-entrancy guards for non-recursive loops.
3. Identify what is keeping pendingWork flags set. The most common
   cause is a cleanup function returning false every call because its
   completion check undercounts (e.g., not counting sections toward
   expectedCount). Fix the counting, not the call site.
4. Test immediately after each fix. If the cascade persists, revert
   and start from the stack trace with a different theory. Do not
   layer guards.
5. Guards added for the wrong reason can break unrelated features
   (e.g., g_inSubclassProc in WM_PAINT blocked NM_CUSTOMDRAW
   separator painting). Treat a guard that "also fixes" a cascade
   with suspicion; the cascade fix is probably elsewhere.

### Workflow/constraints

1. Working file is EditorWorkspace\mod.wh.cpp
2. Do not commit or push unless explicitly asked
3. Do not force push non-trivial changes to the fork
4. Do not update CLAUDE.md unless explicitly asked. Do not make wrapping-only changes