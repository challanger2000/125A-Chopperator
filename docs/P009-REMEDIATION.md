# P009 consolidated audit remediation — candidate, not a release

Baseline: main 33d81e8a2a66c95999207819efeecfe9de313ede; prior verified Glass V2 binary P008. Original independent reports and evidence remain unchanged. Windows x64 VST3. No shipping FULL/Demo exists. No new product features or GUI redesign.

| Finding | Candidate change and intended regression | Status at source commit |
|---|---|---|
| C01 | independently validate tempo/meter; compare native PCM with valid and unchanged controls | NOT RUN |
| C02 | explicit little-endian codec checks status and count; injected full-count errors on all fields of component, controller component recall and UI; compare saved state | NOT RUN |
| C03 | gate both buffer access and silenceFlags on SDK bus activation; native nonnull sentinel and reactivation | NOT RUN |
| C04 | shared UI preference notifications for attached editors; peer resize uses host acceptance, rejected geometry not represented as applied | NOT RUN: live recall/multi-view native tests still needed |
| C05 | bypass stale private SDK aspect zoom cache on DPI notification; preserve current zoom authority | NOT RUN: real DPI transitions and rejection need verification |
| C06 | use SDK EditController::setDirty on user preference changes, never recall/repaint | NOT RUN: callback and failure checks still needed |
| C07 | reject double signs, native direct-entry and standalone parser tests | NOT RUN |
| C08 | validate representability across allowed length range before clock/setup mutation; test rollback, normal unusual rates | NOT RUN |
| C09 | existing per-instance clock retention across missing/invalid fields and setup remains intentional; no transport dependence | SPECIFICATION: lifecycle PCM test pending |
| C10 | preserve local platform text while editing; require loaded 3x logo; preview label/density, cancel and host focus/adversarial tests pending | PARTIAL / NOT RUN |
| C11 | original realtime deadline findings retained; no speculative DSP optimization; controlled native scheduling/load verification pending | INCONCLUSIVE |
| C12 | package the exact pinned SDK and VSTGUI notices, check staged bytes | NOT RUN: exact archive/clean install and rights review pending |

Native tests contain independent counterexamples; none are considered passed until actual execution. Existing test report gate now rejects missing positive evidence rather than accepting an empty file. The complete independent tester/test lab report, DAW recall/bypass/automation and real musical checks remain release blockers. No final ZIP or public release authorized.


## Initial Windows evidence and follow-up
Exact initial P009 eace8def76c167046ed13c6cf4b4a2720a91d7da [Windows run 45468223314](https://github.com/challanger2000/125A-Chopperator/actions/runs/45468223314) SUCCESS: 4/4 CTests, 47/47 Validator, released 125A Tester 57/0/0 with 100 fresh-process profiles; actual Clock/Stream/Bus/Rate regression PASS, native all 13 audio chains, all 10 text readouts/defaults, all seven live preview controls, existing fresh recall/zoom/help and programme PCM PASS. Paced native 384 callbacks p99 173.1us/max 656.3us vs 2666.67us deadline, 0 overruns; tester logs 10 host scheduling misses per format, not callback deadline misses. Artifact 14957270604 server SHA256 19064715cff022fd2598a489e140b5420983bea36f4e7644a8b85bb711e0b606; evidence 14955354581 SHA256 ff07b8e84ed119c7af352ec43ceaad815e1f1c9a310027db1d1473698f1b2769. Server digests only; ZIP bytes not independently checked.

Follow-up adds exact native two-view live recall, host DPI denial/recovery, dirty callback counts and automation while a native text field is open. DPI rejection rolls back contentScaleFactor, preattach DPI updates declared dimensions. New tests NOT RUN until exact CI. No release promotion. Glass V2 artwork and audio path unchanged from initial P009. Full Test Lab, independent re-audit and owner Studio Pro remain outstanding.



Follow-up run 45470099197 FAILED at native test compilation (MSVC C2660, MemoryStream::seek requires an explicit third argument in pinned SDK). No follow-up functional tests executed. Correct the test fixture to verify the returned cursor position as well as the result code. Product implementation unchanged by this correction. Initial P009 verified evidence remains valid only for its original commit. New run pending.

## Verified follow-up and remaining gates
Exact source a5e3b4747d08a50479b9e82425b186a4cdcd8bab, Windows run 45470809451 SUCCESS. 4/4 CTests, 47/47 Validator, 57/0/0 released Tester, all 13 GUI audio chains, all seven timing display controls, six programme PCM fixtures, native text/caret under automation and two-view live recall/DPI/rejection/dirty gates PASS. Paced 384 callbacks, p99 225.6 us, max 529.1 us, deadline 2666.67 us, 0 overruns. Artifact 14956533445 SHA256 (server) 068f86ef324e3d1ce3e7caf240fc6f682679f63916338991d2fb1345317670c7; QA 14956548292 SHA256 (server) f55f3c75401d912245e9f5026ade391e4ffbc77e5912b816102dabb1aee13cd7. Archive bytes/clean machine not independently verified.

Next candidate changes only tests/CI: native clock retention lifecycle and Codex malformed-prefix queue probe; paced output must not be silent or unchanged, and observed deadline overruns now fail the gate. Adopt explicit per-instance clock retention through reactivation and fresh-instance 120/4/4 (existing behavior, not a new feature). Build independently pinned Test Lab 7213fe1f81f4036f74431cb5b2d7dbd09b73ee5c and run P74 native technical baseline on exact staged module. NOT RUN until new CI. Technical baseline is not full declared product coverage. Remaining: native extreme preview/label/density, corrupt-resource rejection, original-defect negative controls, full product-specific Lab matrix, independent re-audit, actual Studio Pro/ASIO and listening. No release approval. No product code or Glass V2 changes.

## Exact P009 Windows and independent technical Lab
Run 45474137150 / 854d0fb0ebdd41df4ce52cfab6175879de753cba SUCCESS; 4/4 CTests, 47/47 Validator, 57/0/0 Tester, native Clock/Lifecycle/Malformed Queue and previous GUI regressions PASS. Strict realtime signal gate PASS: wet energy ratio .222227, difference energy ratio .624714, p99 108.8us, max 864.9us, deadline 2666.67us, 0/384 overruns. Independently pinned P74 Lab TECHNICAL_BASELINE_PASS, native technical PASS, no faults; no declared product coverage or release approval. Artifact 14958597941 server SHA256 2ea64f84c8cfcf9adb0c98bada73683f67a96f8fbf4fb40bbdbee69d8f39dea5; QA 14958627936 SHA256 b36588dcce3d43c041967efb611794a8710b3029eff6972a46e38d274ed0269b. 
Next candidate: preserve Glass V2 geometry and timing, constrain MID label only when measured text widths imply collision; add composed-pixel label and maximum-density width controls, 20 native editor lifecycle cycles and fresh-process malformed logo/knob asset rejection. NOT RUN until exact Windows result. Archive inspection, actual DAW/ASIO, remaining DPI/preview matrix, independent Codex re-audit and complete declared product coverage remain open. No release approval.
