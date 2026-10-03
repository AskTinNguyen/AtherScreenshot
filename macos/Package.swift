// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "AtherScreenshot",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "AtherScreenshot",
            path: "Sources/AtherScreenshot",
            linkerSettings: [.linkedFramework("Carbon"), .linkedFramework("ScreenCaptureKit")]
        ),
        .testTarget(name: "AtherScreenshotTests", dependencies: ["AtherScreenshot"], path: "Tests/AtherScreenshotTests"),
    ]
)
