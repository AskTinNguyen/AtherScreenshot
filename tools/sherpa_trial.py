"""Bounded offline Sherpa KWS trial. Never sends app commands or uploads audio."""
import argparse
import hashlib
import json
import queue
import sys
import time
import wave
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--deps", type=Path, help="Isolated pip --target directory")
    parser.add_argument("--wav", type=Path, nargs="+", default=[])
    parser.add_argument("--live-seconds", type=int, default=0)
    parser.add_argument("--save-live-wav", type=Path, help="Optional local PCM evidence for replay; never uploaded")
    parser.add_argument("--device", type=int, help="sounddevice device index")
    parser.add_argument("--list-devices", action="store_true")
    parser.add_argument("--threshold", type=float, default=0.25)
    parser.add_argument("--score", type=float, default=1.0)
    parser.add_argument("--precision", choices=["int8", "fp32"], default="int8")
    parser.add_argument("--input-gain-db", type=float, default=0,
                        help="Inference-only gain; does not change Windows or saved recordings")
    parser.add_argument("--keywords", nargs="+", default=["BUG", "DONE"])
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if not 0 <= args.live_seconds <= 120:
        parser.error("Live trial must be between 0 and 120 seconds")
    if not 0 < args.threshold <= 1 or args.score <= 0:
        parser.error("Threshold must be in (0, 1]; score must be positive")
    if not 0 <= args.input_gain_db <= 24:
        parser.error("Inference gain must be between 0 and 24 dB")
    if args.save_live_wav and (not args.live_seconds or args.save_live_wav.exists()):
        parser.error("Live WAV requires live capture and a new, unused file path")
    if not args.list_devices and not args.wav and not args.live_seconds:
        parser.error("Supply WAV files or a bounded live duration")
    if args.deps:
        sys.path.insert(0, str(args.deps.resolve()))
    import numpy as np
    import sentencepiece as spm
    import sherpa_onnx

    if args.list_devices:
        import sounddevice as sd
        print(sd.query_devices())
        return
    model = args.model.resolve()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    # Put generated keywords with the report, never modify the downloaded model.
    keywords = args.report.with_suffix(".keywords.txt")
    if (model / "en.phone").exists():
        lexicon = {}
        for line in (model / "en.phone").read_text(encoding="utf-8").splitlines():
            fields = line.split()
            if len(fields) > 1:
                lexicon.setdefault(fields[0], fields[1:])
        encoded = [" ".join(phone for word in k.upper().split() for phone in lexicon[word])
                   for k in args.keywords]
    else:
        tokenizer = spm.SentencePieceProcessor(model_file=str(model / "bpe.model"))
        encoded = [" ".join(tokenizer.encode(k.upper(), out_type=str)) for k in args.keywords]
    keywords.write_text("\n".join(tokens + " @" + key.upper().replace(" ", "_")
                                  for tokens, key in zip(encoded, args.keywords)) + "\n", encoding="utf-8")
    suffix = ".int8.onnx" if args.precision == "int8" else ".onnx"
    weights = {}
    for name in ("encoder", "decoder", "joiner"):
        matches = sorted(model.glob(f"{name}-epoch-*-avg-2-chunk-16-left-64{suffix}"))
        if not matches and name == "decoder":  # newer model uses fp32 decoder with int8 encoder/joiner
            matches = sorted(model.glob(f"{name}-epoch-*-avg-2-chunk-16-left-64.onnx"))
        if not matches:
            raise FileNotFoundError(f"Missing {name} weights in the selected model")
        weights[name] = matches[0]
    started = time.perf_counter()
    kws = sherpa_onnx.KeywordSpotter(
        tokens=str(model / "tokens.txt"), keywords_file=str(keywords),
        encoder=str(weights["encoder"]), decoder=str(weights["decoder"]),
        joiner=str(weights["joiner"]), provider="cpu", num_threads=1,
        keywords_threshold=args.threshold, keywords_score=args.score,
    )
    report = {"runtime": sherpa_onnx.__version__, "provider": "cpu", "threads": 1,
              "threshold": args.threshold, "score": args.score,
              "precision": args.precision, "input_gain_db": args.input_gain_db,
              "keywords": args.keywords, "load_seconds": time.perf_counter() - started,
              "weights_bytes": sum(p.stat().st_size for p in weights.values()),
              "weights_sha256": {n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in weights.items()},
              "samples": []}

    def decode(stream, samples, rate, at):
        if args.input_gain_db:
            samples = np.clip(samples * (10 ** (args.input_gain_db / 20)), -1, 1)
        stream.accept_waveform(rate, samples)
        events = []
        while kws.is_ready(stream):
            kws.decode_stream(stream)
            keyword = kws.get_result(stream)
            if keyword:
                events.append({"keyword": keyword, "detected_by_audio_s": round(at, 3)})
                kws.reset_stream(stream)
        return events

    for path in args.wav:
        with wave.open(str(path), "rb") as wav:
            if wav.getsampwidth() != 2:
                raise ValueError("Expected PCM16 WAV")
            rate, channels = wav.getframerate(), wav.getnchannels()
            samples = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float32) / 32768
            samples = samples.reshape(-1, channels).mean(axis=1)
        seconds = len(samples) / rate
        peak = float(np.abs(samples).max()) if len(samples) else 0
        stream, events = kws.create_stream(), []
        # Endpoint tail allows a command at EOF to finish decoding.
        padded = np.concatenate((samples, np.zeros(int(rate * 0.8), dtype=np.float32)))
        wall, cpu = time.perf_counter(), time.process_time()
        step = max(1, int(rate * 0.08))
        for offset in range(0, len(padded), step):
            block = padded[offset:offset + step]
            events.extend(decode(stream, block, rate, (offset + len(block)) / rate))
        cpu = time.process_time() - cpu
        # A hash ID avoids logging descriptions embedded in personal folder/file names.
        sample = {"id": hashlib.sha256(str(path.resolve()).encode("utf-8")).hexdigest()[:12],
                  "seconds": seconds, "peak_percent": peak * 100, "events": events,
                  "wall_seconds": time.perf_counter() - wall, "cpu_seconds": cpu,
                  "cpu_rtf": cpu / seconds if seconds else None}
        report["samples"].append(sample)
        print(json.dumps(sample), flush=True)

    if args.live_seconds:
        import sounddevice as sd
        device = sd.query_devices(args.device, "input")
        rate = int(device["default_samplerate"])
        blocks = queue.Queue(maxsize=32)
        dropped = 0

        def callback(data, frames, timing, status):
            nonlocal dropped
            if status:
                dropped += 1
            try:
                blocks.put_nowait(bytes(data))
            except queue.Full:
                dropped += 1

        events, peak, received, decode_cpu = [], 0.0, 0, 0.0
        evidence = []
        stream = kws.create_stream()
        print(json.dumps({"ready": True, "device": device["name"], "rate": rate,
                          "duration_s": args.live_seconds, "dry_run": True}), flush=True)
        cpu, wall = time.process_time(), time.perf_counter()
        with sd.RawInputStream(device=args.device, samplerate=rate, channels=1, dtype="int16",
                               blocksize=max(1, rate // 10), callback=callback):
            while time.perf_counter() - wall < args.live_seconds:
                try:
                    data = blocks.get(timeout=0.25)
                except queue.Empty:
                    continue
                samples = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768
                if args.save_live_wav:
                    evidence.append(data)
                peak = max(peak, float(np.abs(samples).max()))
                received += len(samples)
                work = time.process_time()
                found = decode(stream, samples, rate, received / rate)
                decode_cpu += time.process_time() - work
                events.extend(found)
                for event in found:
                    print(json.dumps(event), flush=True)
                if int(received / rate * 10) % 20 == 0:
                    print(json.dumps({"audio_s": round(received / rate, 1),
                                      "block_peak_percent": round(float(np.abs(samples).max()) * 100, 2)}), flush=True)
        # Drain pending input and flush EOF after the capture is closed.
        while not blocks.empty():
            data = blocks.get_nowait()
            if args.save_live_wav:
                evidence.append(data)
            samples = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768
            received += len(samples)
            events.extend(decode(stream, samples, rate, received / rate))
        events.extend(decode(stream, np.zeros(int(rate * 0.8), dtype=np.float32), rate, received / rate + 0.8))
        if not received:
            raise RuntimeError("No microphone frames received; do not treat this as a recognition result")
        if args.save_live_wav:
            args.save_live_wav.parent.mkdir(parents=True, exist_ok=True)
            with args.save_live_wav.open("xb") as file, wave.open(file, "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(rate)
                wav.writeframes(b"".join(evidence))
        report["live"] = {"device": device["name"], "events": events,
                          "seconds": time.perf_counter() - wall, "received_audio_s": received / rate,
                          "peak_percent": peak * 100, "cpu_seconds": time.process_time() - cpu,
                          "decode_cpu_seconds": decode_cpu, "dropped_or_status_blocks": dropped}
        print(json.dumps(report["live"]), flush=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
