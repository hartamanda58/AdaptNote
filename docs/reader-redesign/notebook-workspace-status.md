# Notebook workspace reconstruction — implementation checkpoint

Reference: operator-supplied BookxNote video (484.786 s), inspected through sampled frames.
Do not describe this checkpoint as full visual/behavioral equivalence.

## Implemented
- Workspace-level notebook tabs (close is not delete), notebook header and document tabs.
- Create empty notebooks; attach multiple PDF/EPUB files; reject missing files.
- Reader + right-side notebook outline, alongside existing document TOC and searchable list.
- Shared `mind::buildFromTopic` output for outline and independent mind-map window.
- Cross-document source navigation; modeless excerpt/comment editor; source text read-only.
- Optimistic comment change guard; undoable comment saves.
- Preserve outline collapsed/selected items during refresh.
- Note drop into explicit same-document group, insert before grouped note; undoable.
- Group reparenting within one document, cycle rejection; undoable.
- Independent map with source navigation, note comment editing, group rename and grouped-note reordering.
- Actual application QSS resource alias fixed; regression asserts runtime theme availability.
- Reading zoom persisted per document; empty notebooks do not expose the previous document in reading mode.

## Known incomplete behavior
- Groups remain per-document. Arbitrary note-under-note nesting is NOT implemented.
- Editable note headings are persisted separately and travel in note JSON. Free-standing rich text notes are not implemented.
- Group sibling-order drop gestures, complete drop preview and keyboard accessibility need further work.
- Independent map supports link creation/deletion, note deletion and PNG export. Full group creation menus and link label/arrow menus still need parity.
- Independent map retains camera when reopened in the same scope; switching notebook refocuses root.
- Open notebook tabs and active notebook are persisted; interactive startup restores the active notebook. Home still opens the existing library window.
- Toolbar labels were compacted and reviewed at 1360px without overflow; a final icon redesign and narrow/high-DPI combinations remain.
- Export uses existing exporters; notebook-specific selection UI and source-link interoperability require review.
- Real tablet, dual-device WebDAV and third-party Anki/XMind clients remain unverified.

## Validation evidence
Full local build plus all 11 suites passed after the final local changes. Syntax checks and
modal-dialog guard passed. LSP probe was inconclusive (not evidence of clean).
Commit 8c358f1: Windows build 41327460385 succeeded; packaged runtime smoke 41329232907 succeeded.
Pre-release: https://github.com/hartamanda58/AdaptNote/releases/tag/notebook-preview-8c358f1
Group deletion undo now restores original note memberships and child-group hierarchy.

## Safety boundaries
Do not automatically replace the database with a new universal node table.
Do not change source page coordinates when moving notes in the outline.
Do not infer source identity from a displayed document title.
Do not equate successful test execution with accepted product experience.
