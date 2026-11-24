# Chopperator static GUI design — draft 01, not implemented or owner-approved

One dark, compact hardware-inspired panel, logical 960 × 576 px. Preserve the already agreed control hierarchy; no filters, menus for unrelated features, additional effects or GUI-only sound parameters. The final artwork is still a product-design draft.

## Layout coordinates (logical pixels)

| Region | x/y/w/h | Purpose |
|---|---|---|
| Header | 16/12/928/48 | official master logo, CHOPPERATOR title, ACTIVE and MODE |
| Rate/envelope display | 24/72/912/138 | actual predicted gain envelope; start/mid/end markers, no editing by drawing |
| Primary controls | 24/224/912/176 | START, MID, END, LENGTH — four large knobs with explicit values |
| Secondary controls | 24/414/912/104 | MID POSITION, CURVE, WIDTH, ATTACK, RELEASE, MIX — six smaller knobs |
| Shape selector | 24/528/912/36 | RECT, SAW UP, SAW DOWN, TRIANGLE, SINE — five equal-width mutually exclusive buttons |

- Primary centres: 138, 366, 594, 822; 128 px official L assets. Secondary centres: 100, 252, 404, 556, 708, 860; 64 px official S assets.
- Values: note denominators as 1/N; length as 1/4, 1/2 or 1–64 bars; curve signed percentage; midpoint 1–99%; attack/release percentage of instantaneous period; all visible.
- Header ACTIVE is a two-state automatable parameter (111), not global host BYPASS (113). MODE is a two-state parameter (112). Do not draw a second global bypass control in the panel. Shape ID 106.
- Length must allow all 66 positions, with mouse wheel/drag and direct value text interaction if supported. No long dropdown.
- Display must use the same settings/phase functions as DSP and visualize the configured finite trajectory, not pretend to show live audio. Live playhead is optional only if independently verified; no invented position. At long lengths with dense cycles use per-pixel min/max rather than sample-drop aliasing in the drawing.
- Exact existing IDs/defaults/state schema retained. Standard Ctrl+left-click resets to real default, not 0.5. Editor changes notify host begin/perform/end edit; automation reflects in view.
- Continuous proportional corner resize target after native VSTGUI implementation; minimum readable logical scale 0.75 and max 1.5 initially. Host negotiation, hit tests and DPI need native verification. Do not claim resize inside Studio Pro until tested.
- Neutral charcoal, black inset control areas, grey labels, orange waveform and selected indicators. Use official red/silver logo unchanged. Subtle screws at corners only; no excessive shadows. No promotional renders.

## Authoritative source asset pins

Branding main 20c4d9d2a3b37e6a8d17e693ed76d9351203c288, master 125A_Logo_Master_FINAL.svg blob bd398ffa3b728b1a847bb519fad8168222bccb48. Preserve original viewBox 0 0 1774 887, geometry and colors.
Knob Designer main 45565ff036eb469b9b0c092744233961b1dec837 identifies approved MixEngine Analog L/S multi-resolution set. Matching generated asset branch commit c09cff5d958969eb025fe09255173e65103deae9:
- L 128/192/256/384 px blobs 76f9f064a2a234bbf5675f6770416bc03b4adfd6, 70f4f68b7462fbee4f52199d50d3af7b7b25c7af, 1c25b2ff43ce237f136054624ea9ee7fe4602ec7, 776a12411487ea5e2903c367529f662029171cf9.
- S 64/96/128/192 px blobs f273d308cc71616ad6c006293ad8e965afd0ed5c, 72b60d2e088d0bb3966143ebde1bbc2230e1232d, f228c6187aef44ab328a91eede7329e15d9c8372, 697a2e1ddd730227de91b6da9c0280088a510974.
Paths assets/mixengine/knobs/125A_MixEngine_Analog_{size}_{pixels}px_{scale}pct_128f.png. Verify actual bytes and dimensions; each strip vertical 128 frames. If a native graphics texture limit requires tiling, preserve frame order and pixels, verify all 128 frames at each scale, no lossy redraw or interpolation between frames.

Status: layout and asset selection documented, no actual rendered static panel yet. Static panel must be completed and visually checked before GUI implementation, per central GUI guidelines. Native DSP unchanged.

Static proof generator: `python tools/prepare_gui_design.py gui-draft` with ffmpeg. It verifies all eight official strips against exact blob hashes and geometry, crops the default frames without altering pixels and embeds the unchanged master logo in an SVG. The illustrative envelope is a derived preview of the defaults, not a live DSP trace; final editor uses the shared core. Generation/visual inspection NOT RUN; binary source access unavailable in current local session. Run on next Windows CI, not a separate speculative build.

P006 run 45305616120 verified eight original public filmstrip downloads and sizes, then failed at anonymous private Branding URL (404). The designated visible product logo alone is now included at resources/125A_Logo_Master_FINAL.svg, copied directly from the authoritative master; script checks its exact Git blob hash. No credentials or other private files included. Render and visual review remain pending.

## P007 design review and native integration
P006 successful run 45306862260 delivered exact SVG; independently rendered and inspected locally. Excess transparent filmstrip margins made knobs too small. Cropping the central half of each source frame at its original resolution corrects the visible size. Locally rendered revised static draft inspected; approved for engineering implementation, not owner approval. P007 asset generation checks that no nontransparent pixels are removed and all 128 frames are pixel-identical after atlas tiling at every original resolution. Native controls use VSTGUI automatic parameter binding and a core-driven reference preview at 120 BPM, 4/4 (not live host position). Actual native appearance, DPI, interaction and host behavior still need tests. No DSP change.

Owner input requirement: no mouse wheel available. Left-click and linear drag is primary and independent of host knob mode. LENGTH uses 195 logical px full range (three px per discrete step), note and continuous knobs use 150 px. Fine-drag factor 5. Wheel is optional. Native interaction gate must exercise all 66 LENGTH choices in both directions by left-button events only, plus note and continuous drag. No UI changes to product layout.

Owner correction after actual P007 screenshot: the agreed display emphasizes variable-width orange rhythmic gate impulses. Do not substitute a thin curve display. Filled gain-envelope columns with a brighter peak edge make the impulse rhythm visible; RECT yields blocks, other shapes preserve their own contour, not a fake rectangle. Use the same core, configured duration and fixed labeled reference clock, min/max and mean per pixel for compressed long patterns. Capture default 1/2 → 1/16 → 1/2, two bars, RECT, rather than arbitrary post-test SINE/quarter-bar parameters. Native visual review pending.

## Binding display reference — owner SVG Unbenannt.svg, 2026-11-24 02:29 UTC
Supersedes all earlier waveform/filled envelope proposals. 340x110 design coordinates: dark #101113 panel, #454545 border, vertical dashed #343434 grid at x=10/90/170/250/330 from y=8 to100; flat equally high #e89b40 rectangular impulses y=16..69, their widths and spacing vary with gate timing; grey START/MID/END below at y=91. No waveform contour, top labels, extra horizontal grid or peak-edge line. A timing display, independent of shape/attack/release/mix, not a live gain oscilloscope. Native rendering derives each gate start/end from the shared phase integral inverted with bounded bisection, 4/4 reference, all 4096 possible pulses; width is open phase fraction. MID label follows MID POSITION. Owner sample depicts fast-slow-fast and approximately 36 pulses; native screenshot fixture 1/16 → 1/2 → 1/16, four bars, 50% WIDTH, not a change to product defaults. Actual native visual verification pending. Other GUI/DSP unchanged.

P008 native screenshot from exact run 45312830818 visually reviewed, matches owner timing layout (flat bars, dashed grid, lower labels). Owner also requires live reaction to controls, not static illustration. Added native rendered scanline tests for all seven relevant left-drag gestures and independent phase oracle, plus width endpoints, equal-height bars and invariance to shape/attack/release/mix. Static playback cursor is not acceptable; any future live marker must derive from the actual processing state, not wall-clock animation. Current preview has no playback cursor; no unrelated display modes added. Reactive oracle NOT RUN until Windows result.
