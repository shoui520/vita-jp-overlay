# Vita JP Overlay — local Japanese OCR

This [bee-san fork](https://github.com/bee-san/vita-jp-overlay) reads visual-novel
text and shows Japanese dictionary entries over your game. **The v0.7.12 release defaults to
on-console vita-vn-ocr, automatic OCR, and bundled plain-English JMdict.**
Recognition and dictionary lookup work without Wi-Fi or a running computer.
The release includes **Overlay**, a standalone per-game settings app. Optional
[Anki card mining](docs/anki.md) saves the shown word as a card in an offline
queue on the memory card, with or without Wi-Fi, and sends the queue to
AnkiConnect on a computer on your local network when you press Triangle. It is
off until you set `anki_mining = on`. There is no AnkiWeb sync.

Open the **Overlay** app, press **Select**, and choose **VOCR**,
**[VOCR2](docs/vita-vn-ocr.md#experimental-vocr2-trial)**, or **Google Lens**
in the OCR engine row.
Tap an engine directly, or use **Left/Right** or **Cross** to cycle the choices.
VOCR is the default and supports automatic or manual local OCR. Google Lens requires
Wi-Fi and sends the selected image to Google, using the original
[upstream implementation](https://github.com/Dartv/vita-jp-overlay).
The release stores the local models; choosing an engine keeps them installed
and applies when you next open the overlay.

The optional VOCR2 trial pairs a separately trained FL10-sized model with a
textbox finder. Shared screenshot totals improved on PSP and Vita, but some
games regressed by one or two characters and its strict promotion guard failed.
It is experimental, has no physical-device validation, and must be selected
explicitly in the Overlay app; VOCR remains the default. The
[`0.7.12` release](https://github.com/bee-san/vita-jp-overlay/releases/tag/v0.7.12)
contains the three plugins, Settings, FL10/H15/VOCR2 and JMdict; it also
carried the optional Meiki engine. Earlier releases remain available.

Meiki, a manual single-line recognizer, has been removed from the source after
0.7.12. Its MNN engine had to be copied into each game's own `app0:` folder
through a per-title rePatch mount, and two observed jobs took about eight
seconds each. A saved `ocr_backend = meiki` now loads as VOCR with a config
warning, and the installer rewrites it to `vocr`. Files copied for it,
`ux0:data/VitaJPOverlay/ocr/meiki-stream-int8.mnn` and any
`ux0:rePatch/TITLEID/VitaJPOverlay/meiki/` folders, are no longer used and can be deleted.

VOCR has completed actual CLANNAD OCR jobs on the physical Vita.
Hardware accuracy/CER and broader game compatibility remain unmeasured; host crop results are separate.
See [OCR measurements and limits](docs/vita-vn-ocr.md) and
[project lessons](docs/project-lessons.md).

The v0.7.10 release prepares the first local match during automatic OCR and
reuses completed results when reopening unchanged dialogue. In Whole word
mode, Right skips the matched source word and Left retraces recent selections.
The header also explicitly refreshes its highlight, with bounded layout retries.
Host tests cover these paths; the physical Vita result remains unverified.
This release does not include
the experimental local-dictionary memory reservation from
[issue #42](https://github.com/bee-san/vita-jp-overlay/issues/42).

All six published v0.7.9 assets match the audited bundle. That version's installed
Shell/Settings bytes and native Settings registration/launch are verified, and
four actual Settings touch/controller actions saved both highlight modes while
preserving VOCR and Japanese-only ON. In-game header taps and highlight geometry
still require hardware checks. See the
[installation checkpoint](docs/todo-20261011.md#v079-touchhighlight-installation-checkpoint)
for the measured results and remaining hardware checks.

## Install

Requires a PS Vita on firmware 3.60–3.74 with HENkaku/Ensō or h-encore.
Download `VitaJPOverlay-0.7.12.zip` from this fork's
[release](https://github.com/bee-san/vita-jp-overlay/releases/tag/v0.7.12) and
extract it. It includes all three plugins, their RCO, the settings VPK, FL10/H15
int8 models including the opt-in VOCR2 model, and the complete converted
JMdict snapshot. The 0.7.12 archive also still carries the Meiki recognizer and
engine, which have since been removed. Python 3.9+, bash,
and curl are needed for the computer-side installer.

Close games, open VitaShell, press Select to start FTP, and run from the
extracted folder, naming each game that should have OCR (here CLANNAD):

```sh
bash install-vita.sh --title PCSG00415 VITA_IP:1337
```

The installer checks the package manifest, backs up both taiHEN configurations
and your existing overlay settings, uploads and checksum-reads back each file,
and registers the three plugins without duplicating entries: the kernel plugin
under `*KERNEL`, the Shell plugin under `*main`, and the game OCR worker in one
section per game (`*PCSG00415`), never under `*ALL`, which would load it into
SceShell and every app as well ([why](docs/vita-memory.md#43-what-this-means-for-all)).
It selects the new local defaults while preserving other settings and per-game
choices. Older experimental Text registrations, duplicate worker entries and an
`*ALL` worker line are commented out. The backup is in `build/device/backup/`
in the extracted folder.

Repeat `--title` for more games. Games registered by an earlier install are
kept, so later updates need no `--title`; with none given and none registered
yet, as after an install that used `*ALL`, the installer changes nothing and
says so. A game added later needs `--title` and a reboot. The Overlay app
shows each game's title ID. For Adrenaline's PSP games add both
`--title NPXS10028`, the PSP emulator's ID that the overlay uses for them, and
`--title PSPEMUCFW`, the title ID of the process Adrenaline's own kernel module
patches; which one taiHEN matches is not checked on a console yet, and a section
that matches no process loads nothing. A PSP game started from its own bubble
(Adrenaline Bubble Manager) may run under that bubble's ID instead; with
`log_file = on` (the installer's default), `log.txt` names each enabled game as
it starts (`game … started`), and that ID is the one to add. The 0.7.12 archive's own `install-vita.sh` predates `--title`
and registers the worker under `*ALL`; use the installer from `main` or a
later release.

In VitaShell, install `ux0:data/JPOverlaySettings.vpk`, then reboot the Vita.
Open **Overlay** from LiveArea to enable or disable individual games. Older
builds, including 0.7.12, call it **JP Overlay Settings**; the title ID
(`VJOSET001`) and VPK file name are unchanged, so the new VPK installs over it.
The settings app itself is always excluded from the overlay.

For manual installation, copy `ur0/` and `ux0/` to their matching Vita partitions
and install `JPOverlaySettings.vpk`. Back up your old overlay `config.ini` before
copying the included one. Add `ur0:tai/VitaJPOverlay_Kernel.skprx` under `*KERNEL`,
`ur0:tai/VitaJPOverlay_Shell.suprx` under `*main`, and
`ur0:tai/VitaJPOverlay_OCR.suprx` under a section for each game, such as
`*PCSG00415` (for Adrenaline `*NPXS10028` and `*PSPEMUCFW`), never under `*ALL`, in the active
taiHEN config (`ux0:tai/config.txt` if it exists, else `ur0:tai/config.txt`).
Disable old `VitaJPOverlay_Text.suprx` entries, an `*ALL` worker line and
duplicate worker registrations, then reboot. Keep unrelated plugins and
configuration intact. For CLANNAD the worker's part is:

```ini
*PCSG00415
ur0:tai/VitaJPOverlay_OCR.suprx
```

## Use in a game

| Control | Action |
| --- | --- |
| L + R | Open or close the dictionary overlay |
| Select + R | Toggle the subtitle strip |
| Left / Right | Skip matched words in Whole word mode; step one character in Character mode |
| Touch a recognized character | Look up the word starting there with a local dictionary |
| Up / Down | Show the next/previous local dictionary's entry when **Dictionaries shown** is 2–8; navigate words with a network dictionary |
| Square | Draw the dialogue region with the left stick or the touchscreen ([Drawing the region](#drawing-the-region)) |
| Hold Square | Restore the full-screen region |
| Cross | Save the shown word as an Anki card in the offline queue, with `anki_mining = on` ([Anki mining](docs/anki.md)) |
| Triangle | Send the queued cards to Anki at `anki_host` |
| Circle | Close the overlay |

Choose a tight dialogue region first: smaller regions need less RAM and reduce
recognition errors. Regions are saved per game. Automatic mode recognizes
settled dialogue in the background and prepares its first local match in Whole
word mode. A completed lookup appears immediately when opening unchanged
dialogue. Character mode defers dictionary lookup until opening the overlay.
Selection changes never recapture the screen or rerun OCR. Set `ocr_mode = on_press` to recognize only
when opening the overlay. With `anki_mining = on`, Cross saves the shown word
and its sentence to the card queue on the memory card, even with Wi-Fi off or
Anki closed; Triangle sends the queue to AnkiConnect. The card has no
screenshot: the Picture field's full-frame capture is the kernel buffer #74's
console refused, so the build leaves it out until it comes from the game's own
memory ([Anki](docs/anki.md#memory-and-network)).

### Drawing the region

Square dims the game and shows a cursor in the middle of the screen. Move it
with the left stick, which speeds up while you hold it, or one pixel per press
with the d-pad (held, the d-pad repeats). Press Cross at one corner of the text
box, then move to the opposite corner: the box follows the cursor, and the line
at the top shows its size in screen pixels. Cross saves it as this game's
region. A box smaller than 32 × 32 pixels, the capture's minimum, is not saved.
Circle takes the first corner back; with no first corner it leaves without
saving. A touch drag still draws the box: the first corner is where the finger
lands, and after it lifts the stick or the d-pad can still move the opposite
corner before Cross. Hold Square for one second for the full screen. The cursor
and its acceleration are covered by host tests; how they feel on the console is
not tested yet.

## Per-game settings

The **Overlay** app shows three large game cards at a time, with wrapped
titles, visible IDs and clear ON/OFF badges. Tap the **Games** or **OCR & lookup**
tab, or press Select, to change pages. Choices save automatically to
`ux0:data/VitaJPOverlay/games.ini` and take effect on game start/resume.

| Control | Action |
| --- | --- |
| D-pad / L or R | Select a game / move a page |
| Cross or touch a game | Toggle OCR and overlay input for that game |
| Triangle or touch the default row | Choose all games by default or selected games only |
| Square | Filter all, enabled or disabled games |
| Start | Refresh the installed-title list |
| Select or touch a page tab | Switch between Games and OCR/lookup settings |
| Circle | Exit |

On the **OCR & lookup** page, select a card with Up/Down. Cross
or touch a card changes its choice: OCR engine, Japanese-only selection,
whole-word/character highlighting, or how many dictionaries are shown.
Touch an engine button or a number to choose it directly;
Left/Right cycle the three engine choices on the model card, OFF/ON on the selection card,
Whole word/Character on the Highlight card, or 1–8 on Dictionaries shown.
Choices save automatically to `config.ini` and apply when the overlay next opens.
Model files remain stored; only the selected engine runs. Start reloads
the saved preferences. Other config values, including dictionary priority,
paths, capture mode and API keys, are preserved.

Japanese-only selection is **ON by default**. The initial cursor and Left/Right
skip Latin text, digits, punctuation and Hangul while selecting kana and Han
characters. It does not tokenize words or hide the recognized sentence. With
it OFF, every Unicode character is selectable. No Japanese characters means
no character lookup or dictionary allocation.

**Whole word** highlighting is the default. It highlights the actual dictionary
match, including the original inflected text, when lookup finishes. **Character**
highlights only the selected character. A pending or unsuccessful word lookup
keeps the character highlighted. In Whole word mode, Right advances past the
matched source span and Left retraces up to 16 recent forward selections. With
no match or previous selection, movement falls back to one character.
Character mode always moves one character; Japanese-only applies to both modes.
Tap a character in the recognized sentence at the top of the open overlay to
look up the word starting there; Japanese-only selection also applies to taps.
Kana text shows the kana word (から, する) and inflected text its dictionary
form (した → する); a short built-in list gives very frequent kanji their common
reading (私 → わたし). Each lookup writes one `log.txt` line with the text at the
cursor, the entry, its length, the dictionary, file reads and time
([word lookup on recognized text](docs/touch-lookup.md#word-lookup-on-recognized-text)).
[Touch selection details and validation](docs/touch-lookup.md).

Disable games such as Uncharted when you need their normal L+R controls.
Disabled games have no overlay capture or input filtering. An unreadable or
corrupt policy fails closed and reports the problem rather than enabling every
game. Adrenaline uses one shared policy for its PSP titles. Local OCR also needs
the game to be registered for the OCR worker at install time (`--title`, see
[Install](#install)); the settings app does not change the taiHEN configuration.

## Settings and dictionaries

`ux0:data/VitaJPOverlay/config.ini` is reloaded when the overlay opens:

```ini
dictionary = local
local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries
local_dictionaries = plaintext.vjdict
local_dictionary_results = 1
text_source = ocr
ocr_backend = vocr
ocr_model_dir = ux0:data/VitaJPOverlay/ocr
vocr_model = VOCR3_H15_w8.vocr
vocr_fallback_model = VOCR3_FL10_w8.vocr
ocr_mode = auto
japanese_only_selection = on
log_file = on
```

VOCR3-H15 (`VOCR3_H15_w8.vocr`, vita-vn-ocr #67) is the default recognizer and
VOCR3-FL10 its fallback: H15's and FL10's shapes retrained, the same sizes and
game memory as v0.2.0's H15 and FL10. On vita-vn-ocr's hand-checked host test
lines VOCR3-H15 has 1.73% CER against v0.2.0 H15's 2.21%, VOCR3-FL10 2.13%
against v0.2.0 FL10's 3.82%; neither passed that repository's promotion gate
and no console has run them ([models](docs/vita-vn-ocr.md#models)). When a game
has too little free memory for VOCR3-H15, that job runs VOCR3-FL10 instead,
automatically; if `VOCR3_H15_w8.vocr` is missing or corrupt, the game's jobs use
the fallback as well. The v0.2.0 `H15_w8.vocr` and `FL10_w8.vocr` are still
installed: set `vocr_model = H15_w8.vocr` and `vocr_fallback_model = FL10_w8.vocr`
to go back to them, `vocr_model = VOCR3_FL10_w8.vocr` to always use the smaller
model, or `vocr_fallback_model = none` to never fall back. `install-vita.sh` sets
the package's default models each time it runs, so set the choice again after a
reinstall. Console speed and
accuracy are not measured yet
(see [the checklist](docs/vita-vn-ocr.md#on-console-checklist)). A memory
error names where the memory ran out: the game's memory for this region
(a tighter region helps) or for the model's weights (VOCR3-FL10 and FL10 are
the smallest), or the Shell's memory for results (restart the Vita). Local OCR
takes no kernel memory for the screen since API 19: the game reads its own
screen ([in-game capture](docs/ingame-capture.md)). Google Lens still uses the
kernel's capture buffer, where a tighter region helps; Anki cards take no
screenshot in this build, so they never use it. A Shell UI memory error
after recognition keeps the recognized text and identifies the unavailable
dictionary results.

Text that types out is read once it is whole: a job recognizes nothing until
every row of the region has held still for `ocr_settle_ms` (150 ms by default,
`0` turns it off), waiting at most `ocr_settle_max_ms` (2000) per recognition,
and a blinking "next" icon does not hold it up. `ocr_skip_blank_lines = on`
skips found lines with no text-like ink; it is off by default, because once it
keeps soft text (a PSP game shown 2x, an upscaled render) it finds next to
nothing to skip in the test screenshots. Both are logged per job; see
[stable text and blank lines](docs/vita-vn-ocr.md#stable-text-and-blank-lines-api-20)
for what each costs and what the host measurements show.
Recognition runs up to three lines at once (`ocr_threads = 3`, the default; `1` reads one line
after another), each on its own thread at the lowest priority on the game's cores, and a line whose
pixels are unchanged since an earlier job gets its earlier text without being recognized again; both
give exactly the serial job's text. `ocr_boost_clock = on` (off by default) raises the ARM clock to
444 MHz while a job recognizes and puts it back after. The log shows each job's helpers, its lines'
share of a core, the clock, and the game's frame pacing during the job against the time before it
([OCR speed](docs/ocr-speed.md); estimates until a console run).
The complete settings template is
[docs/config.example.ini](docs/config.example.ini).

The next release bundles one frozen disk-backed `plaintext.vjdict`, combining
**Daijirin4 → Sankoku8 → JMdict** in that priority order. The original archives
and processed file are committed in the private
[plaintext-dicts repository](https://github.com/bee-san/plaintext-dicts).
The public release includes the processed dictionary and its attribution.
The 2026-10-13 snapshot is pinned by exact commit and SHA-256; builds perform
no dictionary updates and need no conversion or current upstream snapshot.

Deduplication keeps a spelling and normalized reading pair from the first
source that contains it, preserving that source's separate senses, distinct
readings and inflection rules. The result has 485,410 entries (301,927
Daijirin4, 12,561 Sankoku8, 170,922 JMdict) and occupies about 134 MiB on disk.
It stays on disk and is never loaded whole into RAM. Twelve definitions carry
a truncation notice at the existing 7,000-byte limit; 16 empty or overlong
source rows cannot be indexed. Exact provenance and counts are in
[docs/plaintext-dictionaries.json](docs/plaintext-dictionaries.json), with
[attribution](docs/plaintext-attribution.txt) and the
[JMdict license](docs/JMdict-LICENSE.html).

You can add your own Yomitan dictionaries with
[the converter and installation guide](docs/local-dictionaries.md).
Up to eight dictionary files can be enabled in priority order. The Overlay
app's **Dictionaries shown** (`local_dictionary_results`, 1–8, default 1) pages
through files for the selected word. The combined bundle is one file, so
it displays the preferred source; add separate dictionaries to page through
other definitions. Only the page on screen is held in memory.

Google Lens can also be selected with `ocr_backend = lens`; it uploads the
selected image to Google and works with the configured dictionary, including
local JMdict. Selecting an OCR engine preserves the dictionary, OCR mode and
other settings. `dictionary = hachidori` uses Hachidori Relay on
your trusted LAN (`hachidori_host = COMPUTER_IP`, port 19633). Jiten and JPDB
require their respective API keys. With either key set, the subtitle strip
(Select + R) shows furigana over its kanji once the words are looked up, so
each settled subtitle's recognized text is also sent to that service. Local
dictionaries and Hachidori show the strip without readings and add no lookups
for it. Local defaults make no OCR or dictionary network requests and have no
cloud fallback.

### Anki

Card mining is off by default (the test builds turn it on). With
`anki_mining = on`, Cross saves cards as readable JSON files, text only (this
build takes no screenshot), in `ux0:data/VitaJPOverlay/anki_queue/`; they
survive restarts and need no network.
To send them to Anki on your computer, install AnkiConnect, set its
`webBindAddress` to `0.0.0.0`, set `anki_host = COMPUTER_IP` (or `IP:port`, or
`auto` to search the local network once), and press Triangle. Cards go one at
a time; a card leaves the queue only when Anki confirms it, so a failed or
interrupted send can be repeated without duplicates. `anki_deck`,
`anki_note_type`, `anki_tags` and the `anki_field_*` keys choose the deck,
note type and field mapping when you send; the defaults fit the Lapis note
type. An empty field key leaves that item out; `anki_audio_url` adds word
audio from a Yomitan audio source. AnkiConnect is unauthenticated plain HTTP:
use it only on a trusted network. These settings are edited in `config.ini`,
not in the Overlay app. See [docs/anki.md](docs/anki.md) for the queue,
fields, messages and memory bounds.

## Memory and troubleshooting

The game OCR worker runs the selected local OCR engine in the enabled game's process, allocating its
region-sized workspace from that game's `USER_RW`. taiHEN loads it only into the
games it is registered for (one `*TITLEID` section each), never into SceShell or
the system apps. It still checks the title at startup and unloads itself at once
(`SCE_KERNEL_START_NO_RESIDENT`) in SceShell, VitaShell, the settings app and
other system titles, should a hand-edited configuration load it there.
Candidate games keep only an idle worker, at the lowest user priority so every
game thread preempts a recognition, until the Shell confirms their per-game
policy; disabled games load no weights and allocate no OCR workspace.
Adrenaline's PSP titles share its Vita process.

The kernel transports bounded jobs and text. Game workers cannot
submit jobs or control input. A job is tied to its game, capture sequence and
foreground generation; suspend, game changes, cancellation and Shell restarts
invalidate old work. The screen is read in the game: the display hook writes
the region's rows, as 8-bit grey, a strip at a time (under 64 KiB at any
region) into a block of the game worker's own memory, where the recognizer
reads them; no copy of the region or of the frame exists, and the kernel pool
gives OCR nothing. If the game's `USER_RW` refuses that block the worker tries
CDRAM, then phycont, then a kernel strip of at most 64 KiB for one call, and
logs each attempt ([docs/ingame-capture.md](docs/ingame-capture.md)). The
worker frees each job's workspace before returning text. The verified weights stay in their own `USER_RW` block between jobs
(657,664 B for FL10 and VOCR3-FL10 and 1,155,968 B for H15 and VOCR3-H15,
held as 659,456 and 1,159,168 B page-rounded blocks) and are read and
SHA-256 checked again only when the model file's size or modification time
changes, or the model changes. They are released when a job's memory check
fails, when the file is missing or wrong, when the worker finds the game is
no longer the active one (suspend, another app in front, title disabled) and
on exit. A job
then needs only its region-sized workspace. The worker leaves a 4 MiB free
USER memory reserve before allocating either block; allocation can still fail
because the game or system uses memory concurrently.

Model files are opened by the existing kernel worker because some games cannot
access the `ux0:` storage namespace (`0x80010013`, ENODEV). The broker accepts
only the two VOCR3 model names, the three original VOCR model names and the
separate pinned VOCR2 trial model in `ux0:data/VitaJPOverlay/ocr`, checks
their lengths, reports their modification time, and copies at most 32 KiB per
request into game-owned memory. Each model call waits (at most 50 ms, without
the kernel lock) for the kernel worker instead of polling with a 1 ms sleep.
The game then checks SHA-256 and runs inference. The kernel holds one 32 KiB
staging buffer, no complete model, and no saved game pointers. Cancellation or
process exit drains any file operation before the next job can reuse its slot.
The shared API 13+ error codes keep bit 30 clear. On the physical Vita, the
previous BUSY value `-3` arrived as `0xBFFFFFFD`, so the game failed to poll a
pending model operation and retained its job ownership. The new codes match
that return encoding and also remain consistent in copied status fields.
Native SDK file errors are unchanged. Install all three plugins of one build
together (API 21 since the OCR speed settings) and reboot; older plugin versions
cannot use this transport.

The `*main` plugin handles UI and disk-backed dictionary lookup with one
64 KiB result arena and no TLS pool for local lookup. Its module holds 299,008 B
of SceShell's partition (down from 405,504 B in 0.7.12's successor builds): the
overlay's text arena is a 20 KiB Paf allocation taken when the overlay opens and
returned when it closes, never a static buffer; if it is refused, the overlay
does not open that time and the game keeps its input. Local OCR-only/subtitle
jobs use a static 4 KiB filtered-text buffer and no dynamic dictionary arena.
The UI keeps its own 4 KiB text copy for Unicode character navigation. A lookup
reads only the selected position and shows zero or one entry. With Dictionaries
shown above 1 it also keeps a 208-byte table locating the other dictionaries'
entries in the same arena; Up/Down releases the shown entry and reads one other
payload, so several definitions are never resident together.
The previous local result is retired before either is reused; network jobs
retain two result arenas. Paf allocations preserve a 1.5 MiB UI reserve. If a
local result allocation is refused after OCR succeeds, the recognized text
stays visible and only dictionary lookup backs off. Failed joins/frees keep
ownership for a safe stop retry.
With `anki_mining = on`, saving a card (×) or sending the queue (△) takes one
bounded Paf allocation through the same reserve guard (128 KiB: cards are
text-only, no screenshot and no kernel capture) and a 64 KiB thread stack; both
are released when the job is done. Counting the queue when the
overlay opens takes only the thread's stack. Anki failures, storage errors
and refused memory never touch the recognized text.
The kernel startup instruction and all three plugins' syscall imports are audited.

These are bounded allocations and host checks, not a measured physical Vita
peak or a guarantee that every full-screen region fits in every game.
[docs/vita-memory.md](docs/vita-memory.md) is the reference for the Vita's memory
pools, which process each plugin runs in, and the budgets each context can use.
[docs/memory-ledger.md](docs/memory-ledger.md) is the ledger of every byte the
three plugins take from those pools: a budget table checked in CI (an allocation
outside the wrappers, a worst case over a pool's limit, or a module that grew by a
page fails the build), printed in the build log and, with current and peak per
pool, in `status.txt` and `kernel.txt` at boot and at each game start and exit.

`log_file = on`, the default, records bounded diagnostic logs:
`ux0:data/VitaJPOverlay/log.txt` holds at most 1 MB and one rotated
`log.old.txt`, each line written and closed at once so a crash keeps every line
before it; `kernel.txt` holds up to 256 KB per boot (the previous boot's is
`kernel.prev.txt`). `bash tools/install_ftp.sh --collect VITA_IP` fetches all of
it read-only into one timestamped folder and zip, with `config.ini`'s API keys
redacted, both taiHEN configurations, the Anki queue, crash dumps and the
installed files' hashes, and prints its analysis
([self-test](docs/self-test.md#collecting-everything---collect)); `--status VITA_IP`
fetches just the bootstrap logs. Recognition timings and memory attempts are in
`log.txt`. For a decisive console test,
arm the **self-test** with `bash tools/install_ftp.sh --self-test VITA_IP`, reboot,
start a game and reach a text screen: on the first settled screen one job runs
the fallback model, the configured model, the full screen, the capture probe and
the configured model again, in your saved region, each stage with its own record,
and the report has every pool's free memory at each step by every query route;
`--status` fetches it and `python3 tools/analyse_selftest.py` prints a PASS/FAIL
row per stage and the diagnostics (probe, memory routes) as PASS/WARN, which never change
the OCR verdict. See [docs/self-test.md](docs/self-test.md). Each game OCR job logs the
game's free USER memory against what the job needed (also when it refused the
job) and, for each recognized line, its size, CTC frames and recognition time.

A boot that does not complete (the Shell plugin not alive a minute after start,
or SceShell exiting with the plugin loaded) makes the next boot a **safe boot**:
the kernel plugin installs no hooks and the Shell and game plugins unload
themselves, so the Vita starts as if the overlay were not installed, and
`status.txt` says why. Reboot once to try again; after two failed boots in a
row safe mode stays until you install a new build (the installer resets the
record) or delete `ur0:data/VitaJPOverlay/boot.txt`. See
[docs/boot-safety.md](docs/boot-safety.md). If the Vita does not boot at all,
hold L while powering on to skip plugins, start VitaShell FTP, and run
`bash tools/install_ftp.sh --uninstall VITA_IP`. Uninstalling preserves data.

## Build and validation

The Vita build checks the artwork inside `JPOverlaySettings.vpk` before it can
ship. Bubble and LiveArea PNGs must use opaque, non-interlaced, 8-bit indexed
color, and the bubble icon may be at most 32 KiB; RGBA PNGs can fail installation
with `0x8010113D`. The Overlay mascot was drawn by a pinned, Apache-2.0
text-to-image model; [settings/assets/ICON-SOURCE.md](settings/assets/ICON-SOURCE.md)
records the model, checksums, prompt and seed. To recreate the committed artwork
from that image, install `tools/requirements-assets.txt` and run
`python3 tools/make_settings_assets.py`; it writes each image with at most 256
colours and checks that the palette PNG decodes to exactly that art.

The default build enables vocr. Its runtime is external to this repository and
checksum-pinned; see [the build guide](docs/vita-vn-ocr.md#build). The OCR source
repository currently requires access. Set up VitaSDK/Paf with
`bash tools/setup_toolchain.sh`; standalone-app dependencies are hash-pinned.
Python and Pillow are needed for host image tests.

```sh
cmake -S . -B build/host -G Ninja -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr
cmake --build build/host
ctest --test-dir build/host --output-on-failure
cmake -S vita -B build/vita -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr \
  -DVJO_PLAINTEXT_SOURCE=/path/to/plaintext-dicts
cmake --build build/vita --target release
```

Check out the exact `plaintext-dicts` commit recorded in
`docs/plaintext-dictionaries.json` and point `VJO_PLAINTEXT_SOURCE` at that
checkout. CI uses a read-only deploy key and the same exact commit. To prepare
the dictionary separately, run `python3 tools/prepare_plaintext.py --source
/path/to/plaintext-dicts -o plaintext.vjdict`; without `--source`, authenticated
GitHub CLI access can fetch the pinned archive. Both the archive and unpacked
file must match their committed checksums.

The cross-build produces a base ZIP containing the frozen dictionary and
attribution. `tools/package_release.py BASE.zip -o RELEASE.zip --models
/path/to/models` adds the checksum-pinned OCR models and per-file manifest.
Packaging reads the dictionary already in the base ZIP, requires its exact
checksum, and never substitutes a newer JMdict-only snapshot. Publication
still happens explicitly after validation.

The host suite covers configuration, disk-backed dictionary lookup,
deinflection, kernel/input ownership, per-game policies, and the actual Shell
worker with platform stubs. It checks cancelled jobs, denied input blocking,
OOM recovery, failed joins, 50 local OCR-only lifetimes, cursor coalescing,
close/reopen during lookup, scene/region invalidation, Unicode navigation and
100 game-worker
allocation/free cycles, bounded model reads and stale-job isolation. The Shell
Anki miner is tested with its real capture path and its offline queue on real
files: saves without a host or network, restarts, Japanese text and escaping,
failed and short writes, temp and damaged files, AnkiConnect requests and field
mapping, lost replies and retries, duplicates, refused memory, cancellation and
50 card lifetimes; crash/restart tests run each step in a fresh process. Real
model tests also run the actual game worker through the actual kernel broker
with FL10/H15, unavailable game storage, short reads, corrupt weights and
failed cleanup. The integration tests model the observed syscall return
encoding and require a subsequent real capture request after each completed job.
Timeout and ownership tests run without release weights. Real
accuracy is measured separately against vita-vn-ocr's hand-checked shared
`eval/` screenshots. The experimental text-extraction branch remains
separate from this release.

[docs/offline-testing.md](docs/offline-testing.md) says what of this runs where:
the real plugin sources with the SDK boundary stubbed on the host, the pure-C
kernel and core suites on the Vita's CPU under qemu-arm (`tools/arm_host_tests.sh`),
nothing in Vita3K (it has no SceShell, no kernel plugins and no memory budgets),
and the Shell budget replay model (`tools/shell_budget_model.py`) that ranks a
build's SceShell footprint against the console's measured samples. Boot and
memory on the console remain the self-test's to answer.

Controller regressions exercise all 12 hooked read/peek variants, including
negative logic and 64-sample batches. The [pinned VitaSDK headers](https://github.com/vitasdk/vita-headers/blob/e66ebe90b73d1fa4cce005a5b2072cec27322544/include/psp2/ctrl.h)
map Vita shoulders to L1/R1 in `*2`/`Ext2` calls. A physical v0.7.3 trace
confirmed a game `ReadPositive2` sample of `0xC00` alongside raw L+R `0x300`;
the older filter allowed that opening chord to reach the game. The repaired
filter uses shared canonical chord/replay state, preserves genuine L2/R2
in the mapped calls, and clears hidden buttons' pressure values. Full overlay
blocking clears all 12 pressure fields while preserving timestamps and the
SDK's four reserved bytes. Only a replayed short tap receives a fabricated
pressure of 255. Host checks cover mixed APIs, held suppression and contended
reads; physical validation of this input repair remains required.

## Credits and license

Based on [Dartv/vita-jp-overlay](https://github.com/Dartv/vita-jp-overlay).
Thanks to [meikidroid](https://github.com/rtr46/meikidroid),
[jmdict-vita](https://github.com/shoui520/jmdict-vita),
[PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus),
[PSVshell](https://github.com/Electry/PSVshell),
[reVita](https://github.com/MERLev/reVita),
[ScePaf-RE](https://github.com/GrapheneCt/ScePaf-RE), VitaSDK, BearSSL,
jsmn, acutest, and Yomichan for its Japanese deinflection rules.
Pinned sources and attribution are in `third_party/`.

Code: GPL-3.0 ([LICENSE](LICENSE)). JMdict: CC BY-SA 4.0.
