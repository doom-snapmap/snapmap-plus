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
- `*_test.js` checks page behavior, browser modules and Worker services.
- `package_audio_test.c` checks effective bank event indexes, locale paths,
  duplicate contributors, transient map replacements, stale sources and sealed
  cache reads. `package_source_graph_test.c` checks that reachable sounds select
  bank-only package owners while unrelated and editor-only references do not.
  `audio_banks_test.c` also indexes a synthetic bank larger than 4 GiB while
  reading only its 53 bytes of metadata, and distinguishes malformed data from
  an IO failure. These are offline checks, not native bank activation proof.
- `audio_packages_test.c` validates all three PCK tables, local language IDs,
  bounds and metadata-only reads above 4 GiB. An optional directory argument
  audits installed PCK metadata. `audio_originals_test.c` checks packed-before-
  loose originals, exact language lookup, identical/ambiguous duplicates, pinned
  handles and original-relative ownership. Its runtime test verifies installed
  audio satisfies map availability while map bytes remain transient.
- `nav_heap_test.c` checks full path-search queues, repeated route improvements,
  allocation canaries and the subtree metadata consumed by native removal.
- `nav_heap_hook_test.c` checks queue hook publication, rollback and bounded
  insertion through the installed detour with an authored native-code fixture.
- `hook_test.c` executes hook publication and restoration, including explicit
  RIP-relative LEA and relative-call relocation with positive and negative
  displacements, preserved call arguments and return flow.
- `rawmap_paths_test.c` checks save destinations and read-only Play snapshots,
  unsaved policy changes, refusal before editor deactivation, and retry.
- `sig_test.c`, `globals_test.c` and `hooktol_test.c` use optional local game images.
- `startup_bindings_test.c` checks early observer binding, delayed code readiness,
  preserved pre-install addresses and partial startup without repeated hooks.
- `strids_packages_test.c` checks early built-in text, published package policy,
  precedence, Unicode decoding and same-process refresh. Scale cases supply
  1,200 package strings, distinct 1,100-byte IDs, a local document over 16 MiB
  and a native dictionary over two million rows. Malformed signed native
  extents, locked files and invalid bytes after NUL refuse without silent
  truncation; repaired input and repeated refresh preserve unique sorted rows.
- `serialized_entities_guard_test.c` executes the serialized-list cleanup guard
  with valid and empty handles, checking CPU state and patch rollback ownership.
- `map_package_fixtures.h` contains packaged test vectors.
  The map-package suite also verifies private map source contexts: whole-tree
  fingerprints, duplicate assets and empty folders, exclusion from installed
  discovery, independent lifetimes, malformed-set cleanup and retry when a
  source file is still held open.
  After building the suite, `tests/obj/map_package_test.exe --map-context
  <scratch-data-root> <rawmap-json>` validates real embedded archives, reports
  their intact inventories and removes the private context. The scratch data
  root must exist. This never installs packages or activates native consumers.
- `decl_graph_test.c` checks native envelope order, repeated graph records,
  cross-subgraph and forward endpoints, first-match node lookup, layer masks,
  unresolved references and ordered syntax round trips. Graph dependency cases
  in `decl_native_schema_test.c` verify reader binding, class ancestry, every
  repeated object's references and explicit gaps for unsupported state.
- `decl_graph_compose_test.c` checks original-relative graph state and collection
  composition, duplicate occurrence retention, compatible ordering, layer unions,
  duplicate extent ambiguity, and native name/endpoint targets after merging.
  Native-schema and package-family fixtures additionally exercise registered
  readers and built-in contributions through the compiler.
- `decl_md6_compose_test.c` checks named joint-group, event-block and alias
  composition, duplicate rejection, preserved repeated events, deletion conflicts,
  local group-copy ordering, model-binding changes and native envelope syntax.
  Deep opaque payloads use a heap delimiter stack. Package-family fixtures include
  built-in contributions and intact duplicate files. These are compiler tests,
  not native skeleton or animation-command validation.
- `decl_tree_test.c` checks shared declaration parsing, deep and large inputs,
  and malformed syntax. `decl_dependencies_test.c` checks typed references,
  native collection shapes, editor exclusion and incomplete-coverage reporting.
- `decl_polymorphic_test.c` checks class-selected object composition, class-only
  defaults, nested wrappers, fixed-array entries, all six three-source orders,
  class and field conflicts, deletion, null state, malformed wrapper forms and
  dependency gaps without losing known siblings. Native metadata fixtures in
  `decl_native_schema_test.c` check reader-family agreement, template elements,
  ancestry, callback forwarding and selected classes with custom readers.
- `decl_entity_class_test.c` follows parent source views, checks explicit class
  compatibility, missing parents, cycles and deep chains without native loading.
  A changed parent view changes the result without relying on loaded objects.
- `decl_native_lex_test.c` checks custom-reader token slices under the native
  declaration flags: literal backslashes, path names, separate adjacent strings,
  line boundaries and heap-paired delimiters. Unsupported preprocessing and
  numeric/continuation forms refuse explicitly. This tokenizer is not a native
  semantic validator or a replacement for the assignment-style tree parser.
- `decl_material_test.c` reads ordered typed parameter writes, scalar/vector
  constants, partial masks, expressions and resource references without loading
  game resources. It preserves String line boundaries and rejects unsupported
  reader forms. Composition and native resource availability need separate tests.
- `decl_material_compose_test.c` checks ordered/repeated material writes,
  independent vector-component edits, opaque expressions/masks, conflicting
  values, multiplicity ambiguity and per-contributor schema/prior-write bindings.
  Private merge fields must not enter emitted native text. These standalone
  fixtures do not replace package/runtime integration or rendering tests.
- `package_decl_families_test.c` exercises non-entity root binding, fixed arrays,
  nested edits, inheritance ordering, blocking-volume collection composition,
  opaque custom grammar conflicts and required-metadata failures through the
  complete package compiler. Authored source files are checked after each case.
- `package_producers_test.c` checks separate original/effective generated inputs,
  dependency order, generated baselines, alias grouping without source changes,
  compatible edits, cycles, missing inputs, producer collisions and conflicts
  attributed to direct and upstream packages. Removal regenerates previous
  outputs and drops retired package ownership. `override_packages_test.c` also
  exercises private Grid outputs through the actual runtime and file hook,
  including duplicate authored aliases, navigation base reads, failed-publication
  preservation and byte snapshots held across a refresh. Grid alias tests verify
  idempotent normalization and unchanged stock-source selection for every family.
- `package_source_view_test.c` checks original, outer-package and result inputs
  supplied to custom readers, including nested components, canonical aliases,
  product defaults, missing resources and integrity failures after discovery.
  Raw source reads must preserve ownership without selecting another package's
  values or treating the returned inputs as a compiled dependency.
- `audio_banks_test.c` checks cooked bank bounds, event identities, native load
  ownership and recovery after partial load/release failures. Already-loaded
  banks remain native-owned. Interrupted calls cannot certify a clean state.
  `audio_banks_native_test.c` checks directory indexing, localized-bank identity
  coalescing, dependency selection and catalog failure before native mutation.
- `package_compiler_test.c` checks effective resource changes independently of
  authored ownership, duplicates and storage representation, including binary
  NULs, large captured lengths, invalid sources and detached result lifetimes.
  `package_runtime_context_test.c` checks forward and recovery change directions,
  removed map-only resources and unchanged-map activation with an empty list.
- `package_opaque_test.c` checks streamed original identities, stock-copy versus
  campaign-import ownership, intact duplicate contributors, distinct replacement
  conflicts, stale duplicate refusal, product defaults and producers, and an
  original larger than 4 GiB without invoking its byte reader. Declaration
  composition must continue using its text baseline. `package_sources_test.c`
  checks captured original-file identities against SHA-256 and protected streams.
- `resource_graph_test.c` checks reverse file-to-consumer closure across native
  source/state and producer edges, cycles, duplicate seeds, replaced dependencies,
  incomplete/active coverage, detached identity lifetimes and a 4096-model chain.
  It also checks independent product-stage input sets, replacement,
  clearing on source reload, cycles, duplicate/self edges and growth while
  interning dependencies. `package_usage_test.c` verifies their complete package
  and policy selection, including intact duplicates and editor exclusion.
  `nav_bake_test.c` verifies cached base/table provenance without input rereads,
  stale worker rejection, fallback provenance and a concurrent reader held at
  the publication boundary. Provider tests exercise both committed and refused
  activation callbacks and verify the prior snapshot on failure.
- `decl_native_schema_test.c` uses an authored native-metadata fixture to check
  reader identity selection, inheritance, arrays, registry inconsistency and
  memory-read failures without constructing game objects. The same native adapter
  drives sparse fixed-array composition and refuses an index outside its extent.
  Accessor-binding cases reject conflicting native address computations,
  uninitialized metadata, changed instruction layouts and failed reads.
  Class ancestry reads do not require readable field arrays; unknown equal
  class names and cyclic native metadata cannot certify a class.
- `decl_native_registry_test.c` checks native manager and ancestor lookup order,
  overlapping loaded and source identities, unknown source availability,
  name normalization and malformed or changing memory snapshots.
  Type-to-class lookup uses copied registration metadata even when loaded
  objects are unreadable, and rejects ambiguous type registrations.
  Prospective source resolution ignores conflicting resident families and their
  unreadable arrays; absent/unknown candidate sources cannot reuse local objects.
- `decl_visibility_test.c` checks quiet production source probes, published
  declaration fallback, unreadable input and long native paths.
- `resource_graph_prepared_test.c` combines the native adapters and typed walker
  to verify prepared-state capture, stale-generation rejection and preservation
  of existing complete or partial observations.
- `strids_packages_test.c` also exercises native vanilla-row replacement,
  exact restoration, absent-key retirement, user precedence after package
  removal, hash collisions and repeated remove/reinstall cycles. Its activation
  callback combines the real compiler/provider and dictionary injector: a later
  failure or exception restores old values and keys and removes introduced keys;
  retry then accepts the prospective strings without duplicate rows.
- `override_packages_test.c` checks compiled stock inputs through Grid Room and
  navigation readers, refresh, retained open copies, disabled layers, missing
  cache files, generated-source dependency paths and failed-publication cache
  preservation. Activation failure and native-exception fixtures restore the
  previous text, opaque files and selected policy; concurrent readers exercise
  both provisional publication and recovery without held provider locks. They
  also check recovery failure, closed map admission and a successful retry.
  `nav_bake_test.c` rejects stale completed worker results after
  source invalidation while retaining map geometry. `nav_traversal_test.c` checks
  cached-table invalidation and selection from a newly loaded table.
- `package_usage_test.c` checks direct and transitive package selection,
  vanilla dependency replacements, duplicate owners, editor subtree pruning
  and incomplete coverage without losing known owners.
- `decl_server_test.c` compiles and captures eight 17 MiB declarations with a
  descriptor containing 2 MiB of inline strings. It also checks 4,097-candidate
  admission and runtime refresh, allocation failure before reconstruction, and
  cleanup when the last declaration remains pending. Boot and browser activation
  failures recover against the retained provider without rescanning sources;
  a successful recovery still refuses the failed install, and retry succeeds.
- `map_payload_test.c` executes authored native save/read length-check fixtures
  before and after repair, including signed boundaries, signature refusal and
  rollback of both sites after a partial failure. Rawmap path tests also read
  a file over 64 MiB.
- `decl_compose_test.c` checks merging all contributions, new nested entries,
  authored ordering, cycles, deletion conflicts and precise source attribution
  across input permutations. Typed cases cover sparse positions, independent
  fields in the same slot, nested fixed arrays in keyed lists, native bounds,
  custom readers and dynamic collections that omit their serialized count.
  Resource-family emission preserves entity header order for new declarations
  and newly added headers, without reordering similarly named nested fields.
- `package_context_test.c` checks map-over-local resource precedence, unchanged
  authored inventories, nested policy, duplicate resources, ownership beyond64
  packages, and readable-versus-missing resource probes including immutable
  caches. Its opaque fixtures test provider bytes, not native format validity.
- `package_runtime_context_test.c` exercises separate local/map/effective
  providers through the real runtime transaction: map switches, local refresh
  during a map, restoring local resources, ordinary/exception recovery, retired
  map policy and already-open opaque streams. Prepared candidates survive source
  edits, cancellation and a later local refresh without changing active state.
  The fixtures include declarations, models, animation, images, both shader
  formats, audio and an unknown future family. Native consumers are callbacks.
  Availability checks distinguish SnapMap originals from campaign-only archives
  and exclude active map-private files from the installed library.
- `rawmap_paths_test.c` also drives saved-map preflight: retained JSON replaces a
  different second engine read, refusal prevents native loading, parse failures
  defer retirement until the browser, and held files retain cleanup ownership.
  Its executable hook fixture verifies original/helper publication and retry
  after failed patch rollback. Native load/read functions are modeled here.
  Full payload coverage skips exact matching; missing files, failed probes,
  probe exceptions and failed activation readiness prevent that route loading.
  Owned launch-context tests cover source mutation during consent, cancellation
  before/after publication, invalid owners, native faults, activation failure,
  request replacement and exactly-once owner release. Inspection-scope tests
  preserve active policy, render state, rawmap arms and map generation; nested
  reads remain thread-local and failed carrier stripping refuses the decode.
- `map_source_test.c` exercises native profile-file buffers: exact byte copies
  above10MiB, the native added terminator, owned/borrowed storage, partial reads,
  read/release/allocator faults, wrong-thread refusal and retry. Native callbacks
  are doubles; the live OpenGL byte comparison is separate acceptance evidence.
- `map_native_test.c` checks deep metadata/source ownership, decoding after the
  caller's input is destroyed, refused parses and launches, native exceptions,
  destructor pairing and thread ownership. `globals_test.c` binds the real
  constructor/destructor call sites and published launch interfaces on both PEs.
- `map_published_test.c` drives all four launch routes through retained consent,
  nested read-only completion, stale cache/host selections, request replacement,
  deferred cleanup and partial hook-group failure. Native functions are doubles;
  these checks do not establish live lobby or next-map acceptance.
- Required-resource checks in `package_context_test.c` cover distinct local/map
  bytes, alternate package IDs, unused bundle assets, nested delivery owners,
  duplicate suppliers, missing undeliverable files, invalid/unreadable paths,
  owner indices above64, and 131,073 repeated requests with two unique probes.
  `package_producers_test.c` verifies generated outputs require their recorded
  inputs. `package_usage_test.c` checks known canonical path collection, editor
  pruning, incomplete coverage and unavailable paths absent from authored data.
  Full-payload checks include every supplied asset while excluding pure product
  defaults. They preserve missing owners and distinguish that sufficient
  no-install check from a minimal gameplay dependency set. Runtime tests verify
  different package IDs, declaration values and shader bytes need no install
  when all paths are present, while private map files cannot certify themselves.
- `package_source_graph_test.c` inspects retained candidate bytes using synthetic
  native reader metadata. It covers transitive declaration pointers, changed
  candidate references, inherited and inline class resolution, editor pruning,
  cycles, a 1024-source chain, and failed-source/invalid-JSON cleanup. Reading
  native prepared entity state is refused and the observed graph stays unchanged.
  A non-declaration manager never manufactures a declaration file path. These
  checks establish explicit source edges, not full native gameplay closure.
  Expanded entity-state tests cover scalar replacement, retained nested fields
  and reset markers, list truncation between inheritance layers, unsupported
  shapes, source failures, cycles and a4096-parent chain without stack recursion.
- `package_compiler_test.c` checks compatible composition, duplicate ownership,
  new identities, built-in ordering and replacement conflicts. After a test build, pass a local application data
  root and DOOM `base` directory to read and compile installed packages.
- `package_sources_test.c` checks intact sources, nested ownership, 128 levels
  of folders and more than 2,048 components. Its optional `--large` probe creates
  131,073 files in a unique temporary directory, inventories and verifies every
  file, then removes those exact files. After building, run
  `tests/obj/package_sources_test.exe --large`; it performs substantial disk I/O.
- `package_archive_test.c` reads an independent Python ZIP64 fixture and rejects
  truncated or inconsistent extended headers. Its optional
  `tests/obj/package_archive_test.exe --large [output.zip]` probe packs, embeds,
  extracts and fingerprints 65,538 real filesystem entries, then removes the
  temporary trees. An optional output retains the ZIP for independent readers.
- `decl_scroll_probe.html` is a manual declaration-editor probe; `strip_dump.c` is a diagnostic helper.

## Working here

`package_material_test.c` exercises material composition through real package
discovery and verified source views: nested duplicates, product contributions,
shared peer schemas, schema/value conflicts and unreadable originals. Material
source graph tests retain parameter/table sources and use candidate schema types
without reading loaded state. Typed image/program/sampler records are not, by
themselves, proof of native resource availability or full dependency closure.

`decl_renderparm_test.c` covers native parameter-default grammar and references:
null program/sampler defaults, multiline Strings, typed images, named versus
inline buffer types, struct bodies, uniform byte counts and image-store formats.
The source-graph suite follows a buffer's source-selected struct definition.

From the repository root, run `powershell -NoProfile -ExecutionPolicy Bypass -File tests/run-tests.ps1` with Node 22.13 or newer installed. Build outputs go to ignored `tests/obj/`.

The Worker runtime suite uses local storage and a GitHub mock. Install its pinned
dependency with `npm install --prefix build/worker-tests --no-audit --no-fund --no-package-lock miniflare@4.20260730.0`, then run
`node tests/worker_runtime_test.js build/worker-tests/node_modules/miniflare`.

Changes to signatures or engine-global anchors require `-Doom <unpacked-image> -DoomAlt <other-renderer-image>`. See [contributing](../docs/contributing.md#7-run-the-tests) for the full requirements. Installer and release-tool tests live with those components. Preserve fixture data unless intentionally changing the case it represents.
