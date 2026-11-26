import Foundation

// AI agents' tools for meetings (MCP, see Agent.swift): record a meeting's sound, read its transcript word by word,
// clean it up and cut it, export the edited audio and transcript, and open any audio file for the same treatment.
// Whatever a person can do in the Meetings window, an agent can do here.
enum MeetingTools {
    typealias JSON = [String: Any]

    private static func schema(_ props: JSON, required: [String] = []) -> JSON {
        var j: JSON = ["type": "object", "properties": props]
        if !required.isEmpty { j["required"] = required }
        return j
    }
    private static let boolean: JSON = ["type": "boolean"]
    private static let meetingID: JSON = ["type": "string", "description": "The meeting's id from list_meetings, part of its title, or \"latest\" (the default)."]
    private static let spans: JSON = ["type": "array", "items": ["type": "object", "properties": ["start": ["type": "number"], "end": ["type": "number"]], "required": ["start", "end"]],
                                      "description": "Time spans in seconds of the original recording."]

    private static func onMain(_ reply: @escaping MCP.Reply, _ body: @escaping @MainActor () async throws -> JSON) {
        Task { @MainActor in
            do { reply(try await body()) } catch { reply(MCP.error(error.localizedDescription)) }
        }
    }

    static func tools() -> [MCP.Tool] {
        func tool(_ name: String, _ title: String, _ description: String, _ schema: JSON, readOnly: Bool = false, timeout: TimeInterval = 60,
                  _ body: @escaping @MainActor (JSON) async throws -> JSON) -> MCP.Tool {
            MCP.Tool(name: name, title: title, description: description, schema: schema, readOnly: readOnly, mainThread: true, timeout: timeout) { a, reply in
                onMain(reply) { MCP.result(try await body(a)) }
            }
        }
        return [
            tool("start_meeting_recording", "Record a meeting",
                 "Starts recording a meeting's sound (no video): everyone else in the call (system audio, optionally only one app's) and the person at the computer (microphone), on separate tracks. Returns right away; call stop_meeting_recording to finish. After it stops, the meeting is transcribed on this Mac with who spoke (You / Others).",
                 schema(["title": ["type": "string"], "system_audio": boolean, "microphone": boolean,
                         "app_bundle_id": ["type": "string", "description": "Only this app's sound for Others, e.g. us.zoom.xos. Default: the app's setting (all apps)."],
                         "transcribe": ["type": "boolean", "description": "Transcribe after stopping. Default: the app's setting (on)."]])) { a in try start(a) },
            tool("stop_meeting_recording", "Stop the meeting recording",
                 "Stops the meeting recording, waits until meeting.m4a is saved and returns the meeting (id, folder, files, length, markers). The transcript follows; get_meeting waits for it.",
                 schema(["discard": ["type": "boolean", "description": "Throw the recording away instead."]]), timeout: 300) { a in try await stop(a) },
            tool("add_meeting_marker", "Mark a moment in the meeting", "Marks a moment of the running meeting recording (e.g. \"pricing decision\"). Markers are saved with the meeting.",
                 schema(["label": ["type": "string"], "seconds": ["type": "number", "minimum": 0, "description": "Recording time. Default: now."]], required: ["label"])) { a in try mark(a) },
            tool("list_meetings", "List meetings",
                 "Meetings and opened audio files, newest first, with id, title, date, length, edited length, transcript state and whether there's a summary. query searches titles, notes and transcripts.",
                 schema(["query": ["type": "string"], "limit": ["type": "integer", "minimum": 1, "maximum": 200, "description": "Default 20."]]), readOnly: true) { a in list(a) },
            tool("get_meeting", "Read a meeting",
                 "A meeting's transcript (speaker, start/end seconds, text, and the text left after cuts), markers, notes, summary, outputs and file paths. Waits for a transcript still being made (up to wait_seconds). words: true adds every word's time (for precise cuts with edit_meeting).",
                 schema(["id": meetingID, "words": ["type": "boolean", "description": "Include word times. Default false."],
                         "wait_seconds": ["type": "number", "minimum": 0, "maximum": 600, "description": "How long to wait for the transcript. Default 120."]]),
                 readOnly: true, timeout: 620) { a in try await get(a) },
            tool("edit_meeting", "Clean up and cut a meeting",
                 "Edits a meeting by cutting audio (the files are never changed; playback and exports skip cuts). remove_fillers cuts um/uh/er…, remove_repeats cuts stammered repeats (\"the the\"), shorten_pauses turns silences over min_pause seconds into 0.4 s. cut / restore take time spans; cut_lines takes line ids from get_meeting. Returns the edited length.",
                 schema(["id": meetingID, "remove_fillers": boolean, "remove_repeats": boolean, "shorten_pauses": boolean,
                         "min_pause": ["type": "number", "minimum": 0.3, "maximum": 10, "description": "Default 1.0 s."],
                         "cut": spans, "cut_lines": ["type": "array", "items": ["type": "string"]], "restore": spans,
                         "restore_all": ["type": "boolean", "description": "Remove every cut first."]]), timeout: 180) { a in try await edit(a) },
            tool("export_meeting", "Export a meeting",
                 "Writes the edited audio (edited.m4a; enhance adds a rumble filter, compression and even loudness) and/or the transcript in the formats asked (markdown, text, srt, json), edited (cuts left out, re-timed) or full. Returns the file paths.",
                 schema(["id": meetingID, "audio": ["type": "boolean", "description": "Export edited.m4a. Default true."], "enhance": ["type": "boolean", "description": "Default: the app's setting (on)."],
                         "formats": ["type": "array", "items": ["type": "string", "enum": ["markdown", "text", "srt", "json"]], "description": "Default [\"markdown\"]."],
                         "edited": ["type": "boolean", "description": "Transcripts without the cut words. Default true."]]), timeout: 600) { a in try await export(a) },
            tool("transcribe_meeting", "Transcribe a meeting again",
                 "Transcribes a meeting again on this Mac (e.g. in another language), then summarizes it if the app's setting says so. Returns at once; get_meeting waits for the result.",
                 schema(["id": meetingID, "language": ["type": "string", "description": "A locale like en-US or vi-VN. Default: the app's setting."]])) { a in try transcribe(a) },
            tool("import_audio", "Open an audio file to edit",
                 "Copies an audio file's sound (or a video's) into Meetings as a new meeting and transcribes it, so it can be read, cleaned up and exported like a recording. Returns its id.",
                 schema(["path": ["type": "string"], "title": ["type": "string"]], required: ["path"]), timeout: 300) { a in try await importAudio(a) },
        ]
    }

    // MARK: resolving

    @MainActor
    static func find(_ a: JSON) throws -> Meeting {
        let store = MeetingStore.shared
        store.loadIfNeeded()
        let key = (MCP.str(a["id"]) ?? "latest").trimmingCharacters(in: .whitespaces)
        if key.isEmpty || key == "latest" {
            guard let m = store.meetings.first else { throw MCP.failed("There are no meetings yet.") }
            return m
        }
        if let id = UUID(uuidString: key), let m = store.meeting(id) { return m }
        if let m = store.meetings.first(where: { $0.title.localizedCaseInsensitiveContains(key) }) { return m }
        throw MCP.failed("No meeting matches “\(key)”. Use list_meetings.")
    }

    static func brief(_ m: Meeting) -> JSON {
        func r(_ v: Double) -> Double { (v * 10).rounded() / 10 }
        var j: JSON = [
            "id": m.id.uuidString, "title": m.title, "created": ISO8601DateFormatter().string(from: m.created),
            "duration": r(m.duration), "edited_duration": r(MeetingEdit.editedDuration(m)), "sources": m.sourcesLabel,
            "transcript": m.transcript.rawValue, "lines": m.lines.count, "markers": m.markers.count, "has_summary": m.summary.map { !$0.isEmpty } ?? false,
            "folder": m.folder.path, "audio": m.audioURL.path, "transcript_json": m.agentURL.path, "transcript_md": m.markdownURL.path,
        ]
        if m.imported { j["imported"] = true }
        if let e = m.transcriptError { j["transcript_error"] = e }
        return j
    }

    // MARK: tools

    @MainActor
    static func start(_ a: JSON) throws -> JSON {
        guard !MeetingRecorder.isActive else { throw MCP.failed("A meeting is already being recorded. Call stop_meeting_recording first.") }
        var o = MeetingRecorder.Options.fromSettings()
        if let v = MCP.bool(a["system_audio"]) { o.others = v }
        if let v = MCP.bool(a["microphone"]) { o.you = v }
        if let v = MCP.str(a["app_bundle_id"]) { o.appBundleID = v }
        if let v = MCP.bool(a["transcribe"]) { o.transcribe = v }
        o.title = MCP.str(a["title"]) ?? ""
        guard o.others || o.you else { throw MCP.failed("Turn on system_audio or microphone.") }
        guard let r = MeetingRecorder.start(o) else { throw MCP.failed("Couldn't start the meeting recording.") }
        return ["recording": true, "id": r.meeting.id.uuidString, "title": r.meeting.title, "system_audio": o.others, "microphone": o.you,
                "app": o.appBundleID.isEmpty ? "all apps" : o.appBundleID]
    }

    @MainActor
    static func stop(_ a: JSON) async throws -> JSON {
        guard let r = MeetingRecorder.current else { throw MCP.failed("No meeting is being recorded.") }
        let discard = MCP.bool(a["discard"]) ?? false
        let outcome: MeetingRecorder.Outcome = await withCheckedContinuation { k in
            var done = false
            r.whenFinished.append { o in if !done { done = true; k.resume(returning: o) } }
            r.stop(discard: discard)
        }
        if let e = outcome.error { throw MCP.failed(e) }
        guard let m = outcome.meeting else { return ["discarded": true] }
        var j = brief(m)
        j["markers_list"] = m.markers.map { ["time": ($0.time * 10).rounded() / 10, "label": $0.label] as JSON }
        return j
    }

    @MainActor
    static func mark(_ a: JSON) throws -> JSON {
        guard let r = MeetingRecorder.current else { throw MCP.failed("No meeting is being recorded.") }
        guard let label = MCP.str(a["label"]), !label.isEmpty else { throw MCP.failed("A marker needs a label.") }
        guard let m = r.mark(label, at: MCP.num(a["seconds"])) else { throw MCP.failed("The recording hasn't started yet.") }
        return ["time": (m.time * 10).rounded() / 10, "label": m.label, "markers": r.meeting.markers.count]
    }

    @MainActor
    static func list(_ a: JSON) -> JSON {
        MeetingStore.shared.loadIfNeeded()
        let limit = max(1, min(200, MCP.int(a["limit"]) ?? 20))
        let all = MeetingStore.shared.search(MCP.str(a["query"]) ?? "")
        var j: JSON = ["meetings": all.prefix(limit).map(brief), "total": all.count]
        if let r = MeetingRecorder.current { j["recording_now"] = ["id": r.meeting.id.uuidString, "seconds": Int(r.elapsed), "paused": r.isPaused] as JSON }
        return j
    }

    @MainActor
    static func get(_ a: JSON) async throws -> JSON {
        var m = try find(a)
        let wait = min(600, max(0, MCP.num(a["wait_seconds"]) ?? 120))
        let until = Date().addingTimeInterval(wait)
        while (m.transcript == .pending || m.transcript == .running || MeetingStore.shared.busy.contains(m.id)), Date() < until {
            try await Task.sleep(nanoseconds: 500_000_000)
            m = MeetingStore.shared.meeting(m.id) ?? m
        }
        var j = MeetingAgentExport.json(m)
        if !(MCP.bool(a["words"]) ?? false), var lines = j["lines"] as? [JSON] {
            for i in lines.indices { lines[i]["words"] = nil }
            j["lines"] = lines
        }
        j["id"] = m.id.uuidString
        j["folder"] = m.folder.path
        j["files"] = ["audio": m.audioURL.path, "transcript_md": m.markdownURL.path, "transcript_json": m.agentURL.path,
                      "edited_audio": FileManager.default.fileExists(atPath: m.editedAudioURL.path) ? m.editedAudioURL.path as Any : NSNull() as Any]
        if m.transcript == .running || m.transcript == .pending { j["note"] = "The transcript isn't finished yet; call get_meeting again." }
        if let e = m.transcriptError { j["transcript_error"] = e }
        return j
    }

    static func parseSpans(_ v: Any?) -> [(Double, Double)] {
        ((v as? [Any]) ?? []).compactMap { x in
            guard let d = x as? [String: Any], let s = MCP.num(d["start"]), let e = MCP.num(d["end"]), e > s else { return nil }
            return (s, e)
        }
    }

    @MainActor
    static func edit(_ a: JSON) async throws -> JSON {
        var m = try find(a)
        guard m.transcript == .done || (MCP.bool(a["shorten_pauses"]) ?? false) || a["cut"] != nil || a["restore"] != nil || a["restore_all"] != nil else {
            throw MCP.failed("This meeting has no transcript yet (\(m.transcript.rawValue)); fillers and repeats need one.")
        }
        let before = MeetingEdit.editedDuration(m)
        if MCP.bool(a["restore_all"]) == true { m.cuts = [] }
        for (s, e) in parseSpans(a["restore"]) { MeetingEdit.restore(&m, s, e) }
        for (s, e) in parseSpans(a["cut"]) { MeetingEdit.cut(&m, s, e) }
        let ids = Set(MCP.strings(a["cut_lines"]))
        for l in m.lines where ids.contains(l.id.uuidString) { MeetingEdit.cut(&m, l.start, l.end) }
        let doPauses = MCP.bool(a["shorten_pauses"]) ?? false
        let url = m.audioURL
        let levels: [Float] = doPauses ? await Task.detached { MeetingEdit.rms(url) }.value : []
        let r = MeetingEdit.cleanUp(&m, fillers: MCP.bool(a["remove_fillers"]) ?? false, repeats: MCP.bool(a["remove_repeats"]) ?? false,
                                    pauses: doPauses, minPause: MCP.num(a["min_pause"]) ?? 1.0, rms: levels)
        MeetingStore.shared.save(m)
        func rd(_ v: Double) -> Double { (v * 100).rounded() / 100 }
        return ["id": m.id.uuidString, "duration": rd(m.duration), "edited_duration": rd(MeetingEdit.editedDuration(m)),
                "shorter_by": rd(before - MeetingEdit.editedDuration(m)), "fillers_cut": r.fillers, "repeats_cut": r.repeats, "pauses_shortened": r.pauses,
                "cuts": MeetingEdit.merged(m.cuts).map { ["start": rd($0.start), "end": rd($0.end)] }]
    }

    @MainActor
    static func export(_ a: JSON) async throws -> JSON {
        let m = try find(a)
        var files: JSON = [:]
        if MCP.bool(a["audio"]) ?? true {
            try await MeetingEdit.exportAudio(source: m.audioURL, duration: m.duration, cuts: m.cuts,
                                              enhance: MCP.bool(a["enhance"]) ?? Settings.shared.bool("MeetingEnhance"), to: m.editedAudioURL)
            files["audio"] = m.editedAudioURL.path
        }
        let edited = MCP.bool(a["edited"]) ?? true
        let formats = MCP.strings(a["formats"]).isEmpty ? ["markdown"] : MCP.strings(a["formats"])
        for f in formats {
            guard let k = MeetingExport(rawValue: f) else { throw MCP.failed("Unknown format \(f).") }
            let u = m.folder.appendingPathComponent((edited ? "edited" : "transcript-full") + "." + k.ext)
            try MeetingStore.shared.export(m, as: k, edited: edited, to: u)
            files[f] = u.path
        }
        return ["id": m.id.uuidString, "files": files, "edited_duration": (MeetingEdit.editedDuration(m) * 100).rounded() / 100]
    }

    @MainActor
    static func transcribe(_ a: JSON) throws -> JSON {
        let m = try find(a)
        if let lang = MCP.str(a["language"]), !lang.isEmpty { Settings.shared.set("MeetingLanguage", lang) }
        MeetingPipeline.shared.process(m.id)
        return ["id": m.id.uuidString, "transcript": "running"]
    }

    @MainActor
    static func importAudio(_ a: JSON) async throws -> JSON {
        guard let p = MCP.str(a["path"]) else { throw MCP.failed("Give a path.") }
        let u = URL(fileURLWithPath: (p as NSString).expandingTildeInPath)
        guard FileManager.default.fileExists(atPath: u.path) else { throw MCP.failed("File not found: \(p)") }
        let id = try await MeetingImport.importFile(u)
        if let t = MCP.str(a["title"]), !t.isEmpty { MeetingStore.shared.rename(id, to: t) }
        if Settings.shared.bool("MeetingTranscribe") { MeetingPipeline.shared.process(id) }
        guard let m = MeetingStore.shared.meeting(id) else { throw MCP.failed("The import failed.") }
        return brief(m)
    }
}
