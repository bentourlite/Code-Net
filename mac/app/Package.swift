// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "CodeNetHost",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(name: "CodeNetHost", path: "Sources/CodeNetHost"),
        .testTarget(name: "CodeNetHostTests", dependencies: ["CodeNetHost"], path: "Tests/CodeNetHostTests"),
    ]
)
