# Changelog
## P001 / 0.1.0
- Standalone phase-integrating audio gain modulation and functional contract.
- START/MID/END, LENGTH, midpoint, curve, width, five shapes, attack/release, MIX, activation, one-shot/loop.
- Deterministic tests added; no VST3 adapter or GUI yet.

## P002 / 0.2.0
- Stable normalized parameter IDs 100–113, shared discrete mapping and defaults for future host/controller paths.
- Extended local Release tests for all note divisions and lengths, envelope timing, 64 bars and unusual sample rates.

## P003 / 0.3.0
- Host-engine state mailbox, pending restore precedence and warm bypass.
- Initial native Windows VST3 adapter: sample-accurate parameter queues, stereo/mono and 32/64-bit processing, versioned state, host tempo/meter; generic host controls only.
- Add one Windows build/validator workflow; no release acceptance yet.

## P004 / 0.4.0
- Real packaged-VST3 native timing/automation/recall test harness (Windows CI gate).
- Reset reused automation lane on initialization; preserve established DSP and parameter IDs.

## P005 / 0.5.0
- Add actual-module coherent AM spectrum, folded rectangular sideband, all five shapes, and paced native dense automation measurements.
- No DSP algorithm change. Verify spectral oracle with a deliberately wrong expected carrier.
