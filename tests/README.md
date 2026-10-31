# Runtime regression tests

## Purpose

Checks native helpers, engine-interface contracts, frontend JavaScript and optional resolution against local DOOM executables.

## Contents

- `run-tests.ps1` builds and runs the native and JavaScript suites. `-Only`
  accepts complete native test names, including `_test`, and rejects every
  unknown selection instead of reporting success for an empty or partial run.
  `-Skip` omits explicitly named native suites, reports each omission, and still
  runs JavaScript when `-Only` is absent. Unknown names or conflicting selectors
  fail; the default continues to run every suite.
- `*_test.c` and `*_test.cpp` exercise runtime helpers and source contracts.
- `*_test.js` checks page behavior, browser modules and the feedback relay.
- `nav_menu_test.c` checks native action ordering, progress and completion,
  repeated requests, cancellation, and rollback after a partial installation.
  `nav_bake_test.c` collects background builds without preview drawing.
  `apply_dispatch_test.c` checks Start-menu scheduling and worker completion.
- Installer onboarding tests cover original SnapHak profile relocation, exact
  copies and idempotency, and check that retired package folders are neither
  copied nor created.
- `nav_heap_test.c` checks full path-search queues, repeated route improvements,
  allocation canaries and the subtree metadata consumed by native removal.
- `nav_heap_hook_test.c` checks queue hook publication, rollback and bounded
  insertion through the installed detour with an authored native-code fixture.
- `hook_test.c` executes hook publication and restoration, including explicit
  RIP-relative LEA, RIP-relative 64-bit load and relative-call relocation with
  positive and negative displacements, preserved call arguments and return flow,
  and the refused load destinations.
- `rawmap_paths_test.c` checks save destinations, live exports and read-only
  snapshots that leave no save side effects.
- `config_json_test.c` separates strict UTF-8 documents from native byte strings,
  preserving every high byte and mixed text while rejecting invalid syntax,
  escapes, duplicate keys, raw controls and excess nesting.
- `sig_test.c`, `globals_test.c` and `hooktol_test.c` use optional local game images.
- `startup_bindings_test.c` checks early observer binding, delayed code readiness,
  preserved pre-install addresses and partial startup without repeated hooks.
- `serialized_entities_guard_test.c` executes the serialized-list cleanup guard
  with valid and empty handles, checking CPU state and patch rollback ownership.
- `config_json_test.c` checks rejection reasons and byte offsets as well as
  strict validation.
- `map_payload_test.c` executes authored native save/read length-check fixtures
  before and after repair, including signed boundaries, signature refusal and
  rollback of both sites after a partial failure. Rawmap path tests also read
  a file over 64 MiB.
- `rawmap_paths_test.c` also drives the load and save detours: a local save, a
  published map and a staged rawmap that carry an unconvertible `smpkg.`
  package load as plain maps (the variables removed, `allocCount` corrected, one
  verdict line), lookalike names stay, and a save keeps the engine's own bytes.
- `modmanifest_test.c` checks the `smpmod1.` manifest rows: grammar, the value
  round trip, rows appended last and stripped with `allocCount` restored, the
  per-row budget that keeps them out of the 4 KiB playtest message, placed names,
  the save closure and the load step against serving-layer doubles.
- `modpublished_test.c` checks the published-map and offline-play gates with
  the real manifest and engine doubles: return-address classification, vanilla
  and installed maps passing through, the hold and the question on a later
  tick, Yes fetching on a worker thread then activating and resuming with the
  retained text, No, a failed fetch, a bundle that will not install, another
  load or a newer launch abandoning the hold mid-fetch, the joiner's host
  re-check, the warm-cache joiner branch and My Maps play.
- `modlegacy_test.c` checks the legacy carrier policy: header grammar per era,
  completeness and digest, stored and deflate ZIP members, the older layout
  mapping, conversion, cleaning and verdict lines with a builder double, and
  the serving-index rescan a new conversion requests. Set
  `SH_MODLEGACY_FIXTURES` to a folder holding the campaign's carrier maps to add
  an integration pass; it is skipped otherwise. The maps are not in the
  repository because their packages contain game files.
- `modnet_test.c` and `modsync_test.c` cover the store client, the upload, download, release and
  retry flows against a loopback stub of the store (`mod_store_stub.h`); no real network is used.
  `modexport_test.c` covers the map and prefab ZIP exports, which carry each mod as its installed
  folder, and their import as the ordinary install of each folder, with pure file work.
- `modpublish_test.c` checks the publish side against the loopback stub with
  engine doubles: a save without rows passing through, the over-bound refusal,
  the first-time question (No cancels and leaves the setting, Yes sets it,
  uploads and resumes, the toast route agrees on the next publish), a failed
  upload cancelling, the editor leaving and another save, the claim at the
  feed post with and without a stored secret, a republish replacing the set,
  release on a republish without mods and on unpublish, the `mods` search tag
  written into the armed publish's own local save only (not a plain save,
  another slot, a second write or an expired arm; a held publish whose save
  was not tagged logs it; a tag that cannot be written does not stop the
  publish), the delete check (no link, no cloud-saved id, still published, a
  failed, late or unstarted answer all keep the claim; an item missing from
  the answer is released and its link forgotten), the copy gate on both sides
  of the bound, and the launch step settling provisional refs and retrying the
  queue.
- `mod_seam_stub.c` stands in for the serving layer and the builder entry in
  tests that link the map manifest or the legacy converter without them.
- `modbundle_test.c` builds `fixtures\mods\alex.r2-imp` and pins its bundle
  hash against `alex.r2-imp.canonical.json`; it also checks logical names, the
  needs scan, build numbering and each refusal rule, and the render-program
  checks on small synthetic cooked programs and SPIR-V files (MD5fold, stage
  texts, SPIR-V names, checksum, header, closure, quoting and refused files).
  The fixture files are
  hashed byte for byte, so their line endings are never converted.
- `modmodule_test.c` covers SnapMap modules served from a bundle over a
  synthetic install: snapModule typing, the pointer-only kinds (`.sbsp`,
  `.ambientsh`, `.tome`, `.pages`) refused as carried bytes, stock restores
  outside the namespace, a module without its snapModuleInfo, serving and the
  activation plan. Set `SH_MODMODULE_SOURCE` to a source mod folder (and
  `SH_MODMODULE_DOOM` to the install) to build it against the real game and
  write its `bundle.json`; `SH_MODMODULE_PREFAB` adds a check of a module
  prefab that names its module, `SH_MODMODULE_MERGE_INTO` and
  `SH_MODMODULE_MERGE_OUT` merge it into a map as Load / Place does and write
  that map with its `smpmod1.` row, and `SH_MODMODULE_SLOTS_FROM` names a map
  whose entities give the reference slot counts the merged entities must match
  (the map validator refuses a mismatch with result 4).
- `modstore_test.c` installs the fixture into a temporary store: blobs, hard
  links and the copy fallback, verification and corruption, in-place builds,
  need resolution across installed bundles, and the store lock (an installer
  waits for a holder instead of failing, two installers at once both succeed,
  the lock is recursive).
- `modserve_test.c` checks the serving index: admission and failure isolation,
  lookups in every spelling, provider precedence against the real `overrides.c`,
  probe claims, registration plans and their order, the rescan and the lookup
  cost, and the conflicts between mods: the winner rules (needs, shared bytes,
  installed over delivered, recency), the composition of list decls (the boss
  packs' conductor and whitelist), the animWebs a registration pass withdraws
  for a model nothing provides, and the Play-time requirements check.
- `mod_pipeline_test.c` links the builder, the serving index, the manifest and
  the legacy converter together: a built fixture is admitted, saved as one
  `smpmod1.` row and activated on the next load; a legacy-form bundle with
  non-stock shadows builds and registers only its new names; skins decls pass
  by their mesh path. With `SH_MODLEGACY_FIXTURES` set, each carrier map is
  converted with the real builder and activated in the same parse. Set
  `SH_MODLEGACY_BETA19` to the path of one rawmap file written by
  v0.2.1-beta.19 (the owner's acceptance map; not in the repository, its
  carriers hold game files) to add a pass that converts every carrier in it,
  prints per carrier what the bundle provides and how many placed names it
  covers, the parse time and the process peak working set, then checks that a
  second parse is answered by the memo and a fresh-process parse by the folders
  on disk without a build. `MOD_PIPELINE_ECHO=1` prints the log lines and the
  placed names each bundle covers.
- `modmigrate_test.c` moves a small synthetic `overrides\` tree into a
  temporary store with the real builder, store and legacy converter: an era C
  pack (an unsupported cvar dropped, strings and composes kept, a sidecar left
  out), an era B pack without an id, two packs with one id, a delivered
  `map-<16 hex>\<package>`, a stray ZIP, a folder holding one package one level
  down (migrated under its id) and one holding two (skipped), a folder over the 512 MiB cap and one over the bundle cap (sparse
  files, no disk cost), and an empty `override-backups`. It checks the store,
  `mods\.migrated.json`, the log lines, that `overrides\` is byte for byte
  unchanged, that a second pass does nothing and `--again` rebuilds the same
  bundles, that a map carrying the same delivery or the same authored tree is
  answered by the migrated bundle without a conversion, a non-empty
  `override-backups` left alone, the boot split (a small folder before the
  index, a large one on the worker, then one rescan), the rerun of an
  unfinished record, and the store lock (a pass waits for a holder, and
  installs during a worker pass wait their turn and succeed).
  `MODMIGRATE_ECHO=1` prints the log lines.
- `modstock_test.c` covers stock pointers end to end over two synthetic
  installs (`mod_stock_fixture.h` writes `gameresources` and
  `snap_gameresources` indexes with `.resources` and `.patch` slices, stored
  and raw-DEFLATE, and loose files under `base\` and the install root; the
  second install is another game revision): the bridge's match at the file's
  own path, of a renamed copy, in the `.patch` store and of a loose file, its
  identify, verify and read; the builder turning carried copies into pointers
  (no blob or size, outside the size total, a `.tome` admitted only because it
  is installed content), the descriptor's `stock` object and its refusals,
  the manifest grammar; the store (nothing stored or laid out for a pointer,
  verification, the first cause on the other revision); the store client
  against the loopback stub (an upload sends only carried blobs, a download
  fetches only them and is refused before any fetch on the other revision);
  and serving (installed bytes, a carried copy of the same bytes is not a
  conflict, a pointer the install cannot answer falls through). With
  `SH_MODSTOCK_DATA` naming a mods store (and `SH_MODSTOCK_DOOM` the install,
  default the Steam path) it measures the Imp, both cyberdemon packs and the
  doors pack: files, pointers, bytes not stored, bytes hashed and time;
  skipped otherwise.
- `modstock_migrate_test.c` runs the overrides migration over a synthetic
  install and tree: an authored pack and a delivered package whose unchanged
  stock files become pointers, with the counts in each folder's line, the
  summary and `mods\.migrated.json`, and only the edited file in `blobs\`.
- `modswf_test.c` drives the cooked-movie Load replacement against synthetic
  engine functions (a served `generated/swf/...bswf` takes the engine's below-2
  open; anything else, a failed open or a failed allocation goes to the
  original), checks that the builder admits `.bswf` only under `generated/swf/`
  with the shadow and `mods/<id>/` rules, and, with the executables, that
  `FileResourceLoad` decodes to the recorded engine calls, its open-into helper
  calls `+0x148`, and one read-only qword (the idFileResource vtable `+0x40`)
  holds it.
- `modsound_test.c` checks mod sound banks (`modsound.c`) and the builder's
  bank and sound rules: the served request to Wwise file name rule, the
  `s_loadBanks` decode, the bank table (stock pointers, duplicates and the cap
  refused), the open-by-name replacement against a real temporary file (the
  `AkFileDesc` the engine's default hook fills, another name, a write open or
  a changed size going to the original), the submit pass (nothing before
  Wwise is up, once per sound-system lifetime, again after LoadBanks, a new
  bank after a rebuilt table), and the builder (FNV-1 bank ids, the stem,
  folder and filter rules, the BKHD version and id, no bank shadows, a
  `mods/` `soundShader` as a `sound` need and a stock one as none). With the
  executables, the four signatures resolve at their recorded RVAs, the
  console function decodes to `soundSystem` and its `+0x140`, and exactly
  two read-only slots hold the open by name and one the LoadBanks.
  `SH_MODSOUND_FIXTURE` names a source mod folder (snaphak-re's `p6probe`)
  to build and print.
- `modvt_test.c` checks mod virtual-texture families: the builder's member
  rule (`virtualtextures/mods/<id>/<family>_sq<N>.mega2`, N 1 to 16, no
  `_vmtr`, the mod's own id), the families the real serving index yields (two
  per bundle, members folded) and the index listener, then registration
  against a fake 32-slot record table: first free records with flavor 2 and
  the `.pages` path, rescan idempotence, a changed build released and
  registered again, release on removal, the hold after a registration pass
  (logged once), a record the engine took back registered again in the same
  slot, a name another record holds never taken or released, a full table
  refused once with the stock records and record 31 untouched, OpenGL without
  sparse page tables refused and with them flavor 4, a refused SetSource
  retried, and a VT system that is not up. With the executables, VtSetSource
  and VtForMaterial resolve at their recorded addresses and the VT system,
  Release, the record table and (OpenGL) the sparse globals decode to the
  recorded values. `MODVT_ECHO=1` prints the log lines.
- `vt_harness_test.c` checks the `sh_vt` decoder's page-header swap and member
  cell lookup; with `SH_VT_RCTEST` naming the reference family bundle (built
  by snaphak-re's lane RC `make_family.py`; it holds stock page bytes, so it is
  not in the repository) it builds, admits and serves it, collects its family
  as `modvt.c` registers it, and checks the editor plan's order.
- `modhud_test.c` checks the `hud` ammo-display grammar, conflicts across
  bundles, the tree read of admitted installed and legacy bundles, and the
  call-site relay on an owned synthetic caller under a concurrent thread; with
  the executables, that the HUD call decodes to the predicate at an aligned
  rel32 with the widget in RBX.
- `modshadow_test.c` measures those rules on a player's real retired
  `overrides\` tree: `SH_MODSHADOW_DATA` names a scratch data root holding a
  copy of it and `SH_MODSHADOW_GAME_NAMES` a `type<TAB>name` list of
  `snap_gameresources`; the migration builds the tree, the index resolves its
  conflicts, and the editor plan and every bundle's gameplay plan run against a
  registry that knows those names. It prints each conflict and, per bundle,
  every animWeb withdrawn. Skipped without the two variables (the tree holds
  game files); `SH_MODSHADOW_DUMP` names a folder for the composed texts.
- `webview_profile_test.cpp` checks where the companion window's WebView2
  profile goes (`webview_profile.h`): created under `cache\webview2`, an older
  top-level `webview2` folder renamed into it with the same files, an old
  folder beside an existing new one left alone, and a failed rename keeping
  the old folder.
- `modui_test.c` runs the Mods tab's backend against a temporary store, the
  real serving index and the map manifest: the document on an empty store,
  install through the tab, every state string, manifest rows and the placed
  closure, stock shadows and a conflict, the removal of a covered build, the
  main-thread refusal and the slot's size query. It also checks that every key
  in `fixtures\mods_tab_payload.json` is one the backend writes (including a
  bundle's `pointers` and a shadow's `installed`, for stock pointers).
- `mods_tab_test.js` renders the Mods tab from that fixture with a minimal DOM:
  the windowed lists, the lazily built shadow list, the action messages, the
  empty and error states, and the page, host and interface contract.
- `startup_dirs_test.c` runs the startup folder creation and settings
  initialization against a temporary root and checks that only the root,
  `strings\` and `prefabs\` appear, then scans the backend, frontend and
  installer sources for any remaining mention of the retired package folders.
- `strids_test.c` checks user-document precedence over the built-in strings,
  escapes, refusal of invalid documents, rearm after native sorting, retirement
  of removed keys, sort-hook recovery and native table limits.
- `decl_scroll_probe.html` is a manual declaration-editor probe.

## Working here

From the repository root, run `powershell -NoProfile -ExecutionPolicy Bypass -File tests/run-tests.ps1` with Node 22.13 or newer installed. Build outputs go to ignored `tests/obj/`.

The Worker runtime suite runs the feedback relay under Miniflare against a GitHub mock. Install its pinned
dependency with `npm install --prefix build/worker-tests --no-audit --no-fund --no-package-lock miniflare@4.20260730.0`, then run
`node tests/worker_runtime_test.js build/worker-tests/node_modules/miniflare`.

Changes to signatures or engine-global anchors require `-Doom <unpacked-image> -DoomAlt <other-renderer-image>`. See [contributing](../docs/contributing.md#7-run-the-tests) for the full requirements. Installer and release-tool tests live with those components. Preserve fixture data unless intentionally changing the case it represents.
