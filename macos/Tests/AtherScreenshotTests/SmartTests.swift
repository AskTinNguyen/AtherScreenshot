import XCTest
@testable import AtherScreenshot

// Suggested tags, search by related words and dates, version stacks.
final class SmartTests: XCTestCase {
    private func meta(app: String = "", text: String = "", w: Int = 1600, h: Int = 1000, mtime: Double = 1000) -> ItemMeta {
        var m = ItemMeta()
        m.app = app; m.text = text; m.w = w; m.h = h; m.mtime = mtime
        return m
    }
    private let u = URL(fileURLWithPath: "/tmp/ather-smart/a.png")

    func testSuggestedTags() {
        XCTAssertEqual(AutoTag.compute(u, meta(app: "Slack", text: "Ship it on Friday?")), ["chat"])
        XCTAssertTrue(AutoTag.compute(u, meta(app: "Safari", text: "Payment failed. Error 402: card declined")).contains("error"))
        XCTAssertTrue(AutoTag.compute(u, meta(app: "Safari", text: "Payment failed")).contains("web"))
        XCTAssertTrue(AutoTag.compute(u, meta(text: "import Foundation\nfunc main() {\n  let x = 1\n  return x\n}")).contains("code"))
        XCTAssertTrue(AutoTag.compute(u, meta(text: "Sign in to continue · Forgot password?")).contains("login"))
        XCTAssertTrue(AutoTag.compute(u, meta(w: 1179, h: 2556)).contains("mobile"))
        XCTAssertFalse(AutoTag.compute(u, meta(text: "display settings for the paystub")).contains("receipt"))
        // Dismissed and existing tags aren't offered again.
        var m = meta(app: "Slack")
        m.dismissed = ["chat"]
        XCTAssertEqual(AutoTag.pending(u, m), [])
    }

    func testRelatedWordsAndRanking() {
        let f = Filter(text: "sales graph")
        XCTAssertEqual(f.match(u, meta(text: "Weekly revenue chart")), .related)
        XCTAssertEqual(f.match(u, meta(text: "sales graph for Q3")), .exact)
        XCTAssertNil(f.match(u, meta(text: "Team chat")))
        XCTAssertEqual(Filter(text: "conversation").match(u, meta(app: "Slack", text: "Ship it")), .related)   // via the suggested "chat" tag
        XCTAssertEqual(Filter(text: "login").match(u, meta(text: "Sign in to your account")), .exact)        // via the suggested "login" tag
        XCTAssertEqual(Filter(text: "authentication").match(u, meta(text: "Sign in to your account")), .related)
        // Short related words only match whole words: "ui" must not match "build".
        XCTAssertNil(Filter(text: "design").match(u, meta(text: "build succeeded")))
    }

    func testDatePhrases() {
        let now = Date()
        let q = SearchQuery.parse("the payment error from last week", now: now)
        XCTAssertEqual(q.terms.map(\.word), ["payment", "error"])
        XCTAssertNotNil(q.since)
        let start = Calendar.current.dateInterval(of: .weekOfYear, for: now)!.start
        XCTAssertNotNil(q.match("payment error", mtime: start.addingTimeInterval(-86400).timeIntervalSince1970))
        XCTAssertNil(q.match("payment error", mtime: now.timeIntervalSince1970))           // this week: excluded
        XCTAssertNil(q.match("payment error", mtime: start.addingTimeInterval(-20 * 86400).timeIntervalSince1970))
        XCTAssertNotNil(SearchQuery.parse("today", now: now).match("anything", mtime: now.timeIntervalSince1970))
    }

    func testVersionStacks() {
        let l = Library(persists: false)
        let a = URL(fileURLWithPath: "/tmp/ather-smart/orig.png"), b = URL(fileURLWithPath: "/tmp/ather-smart/edit1.png")
        let c = URL(fileURLWithPath: "/tmp/ather-smart/edit2.png"), d = URL(fileURLWithPath: "/tmp/ather-smart/other.png")
        l.apply([(a, 1, 1), (d, 2, 1)])
        l.noteEdit(b, from: a, info: NameInfo(), edited: true)
        l.apply([(a, 1, 1), (d, 2, 1), (b, 3, 1)])
        l.noteEdit(c, from: b, info: NameInfo(), edited: true)
        l.apply([(a, 1, 1), (d, 2, 1), (b, 3, 1), (c, 4, 1)])
        let m = GalleryModel(library: l)
        XCTAssertEqual(Set(m.visible), [c, d])          // newest version stands for the stack
        XCTAssertEqual(m.stacks[c], 3)
        XCTAssertEqual(m.versions(of: a), [c, b, a])
        m.toggleStack(c)
        XCTAssertEqual(Set(m.visible), [a, b, c, d])
        m.stackEdits = false
        XCTAssertEqual(m.visible.count, 4)
        XCTAssertTrue(m.stacks.isEmpty)
        m.stackEdits = true
        m.selection = [c]
        m.focus = c
        m.compareSelected()
        XCTAssertEqual(m.compare?.0, b)
        XCTAssertEqual(m.compare?.1, c)
    }
}
