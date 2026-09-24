# 0.17.0-offline2 — open-first I/O for MO2 and Python 3.12+

This is a Python-tools fix only. Keep the 0.17.0 DLL, command ESP, base/user
configuration, existing Glass=1.1 and working barefoot behavior. No geometry,
fit thresholds, morph ranges or runtime controller changes are included.

The reported traceback stopped at path.stat() for an enabled USSEP plugin,
before opening or parsing the file. This resembles the upstream MO2/Python
3.12+ GetFileInformationByName compatibility issue, but the traceback alone
cannot prove whether a direct open succeeds on the user's machine:
https://github.com/ModOrganizer2/modorganizer/issues/2174

Changes:
- open the plugin first, obtain size/time/identity with fstat on its handle;
- hash the same mapped file that was parsed; check for replacement;
- discover implicit masters and associated BSA by opening, not path predicates;
- read loose NIF/TRI before trying BSA. Permission/parse errors do not cause a
  silent archive fallback; changed archives remain rejected;
- preserve lexical Data/profile paths instead of resolving their virtual names;
- create output directories without stat-based exist_ok fallbacks;
- read existing user overrides directly, including concurrency comparison and
  backups. A false exists() must never cause a manual setting to be overwritten;
- provide tools/vha_io_probe.py for an optional open/fstat/stat comparison.

A truly unreadable active plugin remains a hard error. Do NOT skip USSEP,
remove it from plugins.txt, copy ESPs to Data or build a partial load order.
Check launch through MO2, the correct Data/profile, and MO2's Data view if the
new direct-open error persists. File name capitalization in a traceback alone
is not enough evidence to identify the cause.

Install: close the scanner/editor, merge the hotfix tools directory into the
existing 0.17.0 update mod (overwrite its Python files, not the whole mod).
Keep all files together. Launch the same vha_offline.py via MO2 again. The
window version must say 0.17.0-offline2. Rescan and regenerate candidate reports;
offline1 reports are intentionally not applied by offline2. No game launch or
BodySlide rebuild is necessary to test this correction.

Validation: existing parser/scanner/editor tests plus simulated stat false
negatives across full scan -> recommendation -> application, BSA/loose priority,
manual value preservation, genuine missing plugins, permission errors, and
same-size/same-time archive replacement. Mocks are test-only; the shipped tools
do not monkeypatch Python. Actual MO2 injection is not reproduced by these tests.
