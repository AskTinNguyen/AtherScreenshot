# Handoff: make saving videos fast on the Mac

On Windows, saving from the video editor is now about **36× faster** (build 0.0.2.2, merged to `main` in `5fdd8a0`). This note asks for the same on the Mac (`macos/`, Swift). Port the **ideas**, not the C++: AVFoundation, VideoToolbox, Core Image and Metal are different tools, so measure everything on the Mac.

- **The goal:** saving MP4s and GIFs from the Mac video editor as fast as possible, with output that looks the same as today.
  - Work as a hill climb: measure, change one thing, measure again, keep what wins.
- **Reference:** the Windows code on `main`, listed under "Windows reference" below. Its commit messages explain each step and its measured gain: `git log --oneline 7f12b31..5fdd8a0 -- src`.
- **Version (the owner's rule):** the version people see stays **0.0.2**.
  - A Mac re-release raises only the build number (`CFBundleVersion`).
  - `macos/build.sh` reads `ATHER_BUILD_STR` from `src/version.h`, which is now **0.0.2.2** on `main` because of the Windows release. The next Mac build will therefore be 0.0.2.2, which is fine for a first Mac re-release.
- **Ground rules:**
  - Work on a branch (`mac-faster-export`).
  - Make one change per commit, and give its measured gain in the commit message.
  - Build and test as you go: `cd macos && ./build.sh` and `xcrun swift test`.
  - Use no third-party dependencies.
  - Tests never touch the real support folder.
- **Don't merge, publish, tag or change `latest.json`** until the owner says so. Report back with the numbers table (see "Report" below).

## Where the Mac spends its time today

- **MP4** (`VideoExport.mp4` in `macos/Sources/AtherScreenshot/VideoEditor.swift`, around line 107):
  - The composition is an `AVMutableComposition` from `VideoSequence.build` (`VideoSequence.swift`).
  - It uses a **custom compositor** (`SequenceCompositor`) for every frame. The compositor asks for **32BGRA** source and output buffers, wraps the source in a `CIImage`, runs `FrameRenderer.render` (`VideoRender.swift`) and renders the result with a `CIContext` into the output buffer.
  - It is exported with `AVAssetExportSession`, preset **HighestQuality**, `.spectral` pitch for speed changes, and `shouldOptimizeForNetworkUse = true`.
- **What that costs:**
  - Every frame is decoded, converted from YUV to BGRA, rendered by Core Image, and converted from BGRA back to YUV for the encoder, even when nothing is drawn on it.
  - The export session decides how many frames run in parallel and which encoder settings to use.
  - `shouldOptimizeForNetworkUse` rewrites the whole file at the end to move the index to the front.
- **GIF** (`VideoExport.gif`, around line 119): it exports a full MP4 to a temp file first, then pulls frames out one at a time with `AVAssetImageGenerator` (`await` per frame), then writes them with ImageIO. That's two full passes over the video, and the second one is sequential.
- **Expect** the biggest wins from skipping work on frames without edits, from cutting the BGRA round trip, from removing the GIF's double pass, and from encoding in parallel where the chip allows it.
  - The Mac's hardware encoder (Apple silicon media engine) is fast. Base M-series chips have one video encode engine; Pro, Max and Ultra chips have two or more. Check the chip with `sysctl -n machdep.cpu.brand_string`.

## Step 0: build the measuring stick first (don't skip)

Windows has `AtherScreenshot.exe --bench-export <outDir> [tap] <clips…>` in `src/videobench.cpp`. Build the Mac equivalent before changing anything: a hidden CLI mode or a test target that exports the same scenarios and prints wall time and CPU time for each.

- **Clips:** use 2–4 of the owner's own recordings on that Mac.
  - Pick a ~20 s high-resolution one (Retina, e.g. 2560×1600 or larger), a 1080p60 one if available, a 4 s one, and a long one (≥ 40 s, ideally several minutes).
  - Copy them somewhere outside the repo. Never commit them or put them in anything shared.
- **Scenarios**, the same as Windows (`Scenarios()` in `src/videobench.cpp`; copy the exact mark and caption setup):

| Scenario | What it is |
|---|---|
| `plain` | First 20 s, no edits |
| `edits` | Crop (5% margin), a title card for the first 12%, box, arrow, typewriter text, blur, pixelate, zoom, emoji and bubble, 6 captions (`AddMarkup`) |
| `speed2` | First 30 s at 2× |
| `captions` | 6 captions, nothing else |
| `light` | 6 captions, a box and an arrow now and then, a small blur all along |
| `long` | The whole long clip (≥ 40 s) |
| `gif` | First 8 s as a 12 fps GIF |

- **Tap mode:** add a way to see every rendered output frame and hash it, like Windows's `g_exportTap`. Run it once on the current code and keep the hashes as the reference, so you can prove later changes still render the same frames.
- **Baseline:** run the unchanged app a few times, and keep the best of 3 for each scenario. Compare old and new builds in alternating runs, not "yesterday versus today", because machine load moves the numbers by 10–30%.
- **Pitfall from Windows:** a fast path that fails falls back to a slow path silently. Make the bench print which path each export took (parallel or not, how many pieces). Count frames too, so a fallback can't hide.

## What worked on Windows, and what to try on the Mac

In rough order of payoff. Each gain is from the Windows A/B; the Mac will differ.

1. **Frames without edits skip all pixel work.**
   - **Windows:** decoded frames went straight to the encoder, with no RGB round trip.
   - **Mac:** in `SequenceCompositor.startRequest`, when the instruction has nothing to draw at that time (no active marks or captions, no zoom, no crop, same size and no rotation), finish with the **source pixel buffer itself** (`req.finish(withComposedVideoFrame: sourceBuffer)`). To make that possible:
     - Ask for YUV buffers (`kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange`, plus full range if needed) in `sourcePixelBufferAttributes` and in `requiredPixelBufferAttributesForRenderContext`, instead of only BGRA.
     - Check that the composition really hands those buffers to the encoder untouched.
   - For a plain trim with no speed change, crop or edits, also measure `AVAssetExportPresetPassthrough`. It's the fastest possible, but it may cut on key frames rather than the exact trim point. Only use it if the result starts and ends exactly where it should; verify with ffprobe frame timestamps.
2. **Only redraw what the edits change.**
   - **Windows:** `EditAreas` and `DrawEdits` in `src/videoedit.cpp`. Caption-only and light-edit exports cost about 5% more than plain.
   - **Mac:** copy the source buffer into the output buffer (or render the source as is), then let Core Image render only the edited rectangles: `CIContext.render(_:to:bounds:colorSpace:)` with `bounds` set to each rect.
     - Blur reads pixels around its rect, so grow the rect by about 3σ.
     - Areas that touch are drawn as one.
     - With a crop, the crop's origin must be on whole 2×2 pixel blocks for YUV, which is why Windows snaps the export crop by up to 1 px (commit `d012171`).
3. **A frame under a fully shown, still title card is just the card.** Cache the rendered card and skip the video (commit `b3bffa9`).
4. **Don't pay for things the export doesn't need.**
   - Measure `shouldOptimizeForNetworkUse = false`. It saves a full rewrite of the file; Windows writes the index at the end.
   - Measure HighestQuality against explicit encoder settings via `AVAssetWriter` + `AVAssetReader` (with `AVAssetReaderVideoCompositionOutput` for the composition):
     - Same quality target: keep the file size and PSNR against the source in the same ballpark as today.
     - Turn on `kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality` and leave `RealTime` off.
     - Windows uses about 0.12 bits per pixel per frame of H.264 High (`Mp4Writer::Begin` in `src/media.cpp`).
5. **Encode longer videos in pieces in parallel, then join without re-encoding.**
   - **Windows:** `ExportMp4Parallel` in `src/videoio.cpp`.
     - The output is split into 2–4 contiguous pieces: 2 from 3 s of output, 3 from 15 s, 4 from 40 s (`EncodersFor`).
     - Each piece has its own reader, which starts 1 s before the piece's first frame so it picks the same source frames, plus its own encoder.
     - The sound is encoded on its own thread.
     - The pieces are joined by copying the MP4 samples and rebuilding the index (`JoinMp4` in `src/media.cpp`).
     - This was the biggest single win once rendering was fast, because one hardware encoder session topped out at about 300 frames/s at 1440p.
   - **Mac:** first measure whether one export already saturates the media engine. If it doesn't, or the chip has two or more engines, try N `AVAssetWriter`s on N time ranges at once.
     - Each piece must start on a key frame; H.264 writers do this.
     - Join them with `AVMutableComposition` + `AVAssetExportPresetPassthrough`, or with `AVAssetWriter` in passthrough (`outputSettings: nil`).
     - Check that the joined file has every frame at the right time (ffprobe packet listing), and that the sound is continuous.
6. **Sound.**
   - **Windows:** the AAC encoder was very slow when fed 1024 samples at a time (9.8 s for 4.7 minutes of sound); a second at a time it took 1.8 s, with byte-identical output.
   - **Mac:** if you move to `AVAssetWriter`, append large audio buffers. Measure `.spectral` time-pitch on the 2× export, because it may be the slow part there.
7. **GIF: one pass, in parallel.**
   - Render the GIF frames directly from the composition at 12 fps, at most 960 px wide (`AVAssetReaderVideoCompositionOutput`, or the compositor at the GIF size), instead of exporting an MP4 and decoding it again.
   - Make the frames' palettes in parallel and write them in order (Windows: `GifWriter::Quantize` / `AddQuantized`).
   - Measure ImageIO's own GIF encoding cost first. If it's the bottleneck, look at handing it pre-quantized images.
8. **Use all the cores for whatever pixel work remains.** Core Image already uses the GPU. If any CPU drawing remains (CoreText captions, marks), render frames ahead in parallel.
   - Cache what repeats: caption images and text layout. Windows measured caption text once per frame instead of twice.

**Tried on Windows and not worth it** (could differ on the Mac, but low priority):
- Wider SIMD (memory-bound).
- Encoder "quality vs speed" knobs (the default was already fast).
- Pre-warming the encoder at app start (about 20 ms).

## Verification (every change)

- **Same frames:** rendered output frames hash the same as the baseline in tap mode for every scenario.
  - If a change can't be bit-exact (YUV pass-through skips a color round trip), show the difference is invisible: PSNR against the old output above 50 dB, and PSNR against the source no worse than before.
  - Windows accepted ±1 differences on untouched frames for this reason.
- **Files:**
  - `ffprobe` shows the right duration and frame count, with audio present.
  - `ffmpeg -v error -i out.mp4 -f null -` prints nothing.
  - For joined files, compare packet timestamps and key-frame flags against a one-pass export: `ffprobe -show_packets -show_entries packet=pts,dts,duration,flags`.
- **Tests:** `xcrun swift test` passes. Add tests like the Windows ones in `src/videoio.cpp`:
  - an export in pieces renders the same frames as one pass;
  - edited-areas-only rendering matches the whole-frame render in the edited areas;
  - cancelling stops cleanly and leaves no temp pieces;
  - a long export uses several pieces and doesn't silently fall back on a machine that can.
- **The real app:** save from the actual editor at least once per milestone (MP4 and GIF, with edits), open the files in QuickTime, and check sound sync near the end of a long video.

## Report

Send back a table like the Windows one: scenario, before, after and speedup, best of 3, alternating runs, plus the chip name. Add a short list of what each kept change gave, and what you tried and dropped. Windows, for reference, measured on an RTX 5090 PC:

| Clip | Export | Before | After | Speedup |
|---|---|---|---|---|
| 1440p, 20 s | plain | 30.8 s | 1.09 s | 28× |
| 1440p, 20 s | edits | 77.3 s | 1.36 s | 57× |
| 1440p, 20 s | speed 2× | 38.3 s | 1.04 s | 37× |
| 1440p, 20 s | GIF | 17.9 s | 0.57 s | 31× |
| 1080p60, 20 s | plain / edits | 31.4 / 72.2 s | 1.27 / 1.52 s | 25× / 48× |
| 4 s clip | plain / edits / GIF | 4.3 / 13.1 / 10.0 s | 0.46 / 0.46 / 0.22 s | 9× / 28× / 45× |
| all 12 | total | 369.5 s | 10.3 s | 36× |

## Windows reference

| Idea | Where (on `main`) |
|---|---|
| Bench tool, scenarios, tap/compare | `src/videobench.cpp` |
| Export entry, retries, parallel pieces, encoder counts | `src/videoio.cpp`: `ExportMp4`, `ExportMp4Parallel`, `EncodersFor`, `ExportMp4Single` |
| Per-frame paths (pass-through, edited areas, full render) | `src/videoio.cpp`: `Mp4Frames`, `EditFrames::Passthrough` / `WithEdits` |
| Edited areas and drawing them | `src/videoedit.cpp`: `FrameRenderer::EditAreas`, `DrawEdits`, `Untouched`, title shortcut in `Draw` |
| Ordered parallel rendering | `src/videoio.cpp`: `WorkerPool`, `OrderedWork`, `ExportFrames` |
| Sound on its own thread, large writes | `src/videoio.cpp`: `AudioPipe::WriteUntil`, the sound thread in `ExportMp4Parallel` |
| Joining MP4 pieces without re-encoding | `src/media.cpp`: `JoinMp4` |
| GIF palettes in parallel | `src/media.cpp`: `GifWriter::Quantize` / `AddQuantized` |
| GPU decoding and its pitfall | `src/videoio.cpp`: `VideoReader::Seek`. A hardware decoder sought before it had decoded anything sometimes returned end-of-stream at once under load. Watch for the same on the Mac: a piece that comes out empty or short. |
