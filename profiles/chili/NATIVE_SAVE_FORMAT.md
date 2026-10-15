# Native PC saves (work in progress)

The port uses a distinct authenticated PC save format. PSP save import/export
and firmware secure-container compatibility are unfinished. Native PC saves
belong to the port's private `game/ms0` directory; their familiar PSP filenames
do not make them transferable PSP saves.

The actual game supplies a 1,066-byte serialized profile, a 16-byte game key,
and secureVersion 0. Version 0 still requests secure PSP saving. The PC backend
uses Windows BCrypt AES-128-GCM with a fresh system-generated 12-byte nonce.
No dummy PSP authentication fields or plaintext fallback are generated.

The native payload file has magic `CHLSAVE` followed by a zero byte, format
version 1, a 48-byte header, exact payload and raw SFO lengths, a nonce, four
reserved zero bytes, ciphertext, and a 16-byte authentication tag. The header
and exact stored PARAM.SFO bytes are authenticated together. Payload length
has no block padding; the captured 1,066-byte profile becomes 1,130 bytes.
Decrypt stages data privately and publishes it only after authentication.

Native PARAM.SFO identifies its native format, selected directory and data file
and preserves the game's title, profile title, detail and parental metadata.
The backend is scoped to this USA profile's ULUS10216 / SECURE.BIN request.
Unknown/PSP containers are rejected explicitly. Metadata edits require a new
matching authenticated payload. The game's key is part of the original game;
the format does not claim secrecy against inspection or prevent save rollback.

A save transaction stages the complete SFO, ciphertext and optional media file
set in a work folder, `PSP\SAVEDATA\._CHILI_TXN_<process>_<n>_<clock>_<game+slot>\new`,
and flushes each file before publication. A new slot is published by a directory
rename. A slot that holds a save is replaced with rollback: the old folder is
moved into the work folder (`old`), the new one is renamed into place, and a
failure at any step before that puts the old folder back and tells the game the
save failed. Windows directory replacement has a brief absence window for
outside readers. Participating native readers use one storage guard. Power-loss
atomicity and hostile outside-process race immunity are not claimed.

When a save is written over. An automatic save (savedata
mode 1) replaces the save in its slot without a question, as a PSP does. A list
save (mode 5) replaces one when the player answered Overwrite in the port's
dialog; a slot the dialog showed as empty that holds files by the time of the
save is refused. The request's word at offset 56 decides neither. An earlier
version read it as the permission to write over a save. The game's request
builder (0x08830014) clears it, its list-save branch alone sets it to 1, and its
automatic save, which a PSP writes over existing data regardless, leaves it 0:
every save after a profile's first one failed with 0x80110385
(SCE_UTILITY_SAVEDATA_ERROR_SAVE_ACCESS_ERROR). The tests had not met this
because their automatic save was the list-save capture with only the mode
changed, the 1 still in place.

One earlier save is kept. After a replacement is published the old folder is
moved from the work folder to `PSP\SAVEDATA_PREVIOUS\<game+slot>`, beside
`SAVEDATA` (the same volume, so a rename; inside the saves folder; outside the
folder the game's slot list reads), in place of the one kept there before, which
is set aside first and removed last. Keeping never fails a save: if the old
folder cannot be moved there at that moment it stays whole in the work folder,
the save is committed and logged with the reason, and the next savedata
operation keeps it or, failing again, gives it up. Files are deleted only out of
a folder that was first renamed to `discard`, so a folder named `old` is always a
complete save.

Interrupted saves. Every savedata operation first looks for work folders in
`SAVEDATA` (`Store::sweep`). An `old` whose slot is missing (the run ended
between the two renames) goes back into the slot. An `old` beside a published
slot becomes the kept earlier save. Unpublished `new` files are removed. A work
folder of a process that is still running, or one this version cannot make
sense of, is left alone (and reported once per run). Each action is one
`[savedata]` line of the log.

Delete. Savedata mode 7, the game's "Delete save games", shows the slot dialog
as a delete list. Only a slot that holds a save can be chosen, and only after
the separate question "Delete this save?". The slot's folder is not erased: it
is moved to the same `PSP\SAVEDATA_PREVIOUS\<game+slot>` place. The game is then
given common.result 1, the result a PSP's delete list ends with when the player
backs out of it (on a PSP the list comes back after each deletion). The game's
result handler has no branch of its own for the delete.

Modes the port does not implement (any but 0, 1, 4, 5, 7, 8 and 11) and requests
with a bind, language, focus or media flag outside what it handles run through
the utility's ordinary life cycle and end with common.result 1, the result of a
cancelled dialog, with one log line. They used to stop the runtime, which ended
the game from a menu.

The native dialog requires a real user or sampled controller action. Selecting
an existing slot requires a separate Overwrite (or Delete) confirmation, whose
default is Back; the presenter marks a selection made through it as confirmed,
and the runtime takes a choice of a slot that holds a save without that mark as
cancelled. Holding Cross across opening does not select a slot. Recorded
controller input is diagnostic testing and does not establish physical PC input
acceptance. A scripted dialog (`CHILI_AUTO_DIALOGS`, deterministic runs) answers
the confirmation with its pick; such runs work on a copy of the save folder only.

Not done: a slot folder that holds anything but plain files with short names
(letters, digits, `_`, `-`, one extension, 12 characters) cannot be listed,
loaded or saved over; the store refuses it with a parameter error. A copy such
as `SECURE.BIN.bak` made by hand inside a slot folder does that. Keep copies
outside `PSP\SAVEDATA`.

Current evidence: all five integrated component CTests pass (metadata,
authenticated codec, opaque storage, hidden native presenter, captured ABI).
The complete native40783 game build integrates the actual savedata lifecycle,
Win32 slot presenter and SFO/storage/AES backend. Official operation/runtime
checks, existing LIST/SIZES regressions, 148 backend checks and 568 production
composition assertions pass. The production composition uses genuine sampled
PSP input edges, actual captured profile/media, real disposable disk files,
native overwrite and authenticated AUTOLOAD, and atomic errors for tag/SFO
tampering. No crypto/backend testing macros are enabled in the game or this
composition test. An independent OpenSSL-backed check authenticates the exact
written native SFO/header and recovers the captured payload. Optional media
is not authenticated. Original-game native81601 Save commits DATA00 and continues to the autosave
information screen; original-game LISTLOAD now succeeds and advances to the title Start screen.
Physical input and mission gameplay remain unverified by this test.

Private owned-game captures and older key-bearing analysis files must be kept
out of distributed source packages. Production includes no owned game data,
key dump, deterministic nonce API or third-party PSP crypto implementation.
