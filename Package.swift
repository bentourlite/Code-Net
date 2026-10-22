// swift-tools-version:5.9
// The Code Net menu-bar app. Build from the repository root: `swift build`.
import PackageDescription

let package = Package(
    name: "CodeNet",
    platforms: [.macOS(.v13)],
    targets: [
        // The C++ receiver engine and LTC code shared with the Ableton plugin. Swift sees
        // only its C interface (include/codenet/codenet_c.h, via include/module.modulemap).
        .target(
            name: "CodeNetCore",
            path: "cpp/core",
            exclude: ["CMakeLists.txt", "tests"],
            sources: ["src"],
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedFramework("CoreAudio"), .linkedFramework("AudioToolbox"),
                .linkedFramework("CoreMIDI"), .linkedFramework("CoreFoundation"),
            ]),
        .executableTarget(name: "CodeNetHost", dependencies: ["CodeNetCore"], path: "mac/app/Sources/CodeNetHost"),
        .testTarget(name: "CodeNetHostTests", dependencies: ["CodeNetHost"], path: "mac/app/Tests/CodeNetHostTests"),
    ],
    cxxLanguageStandard: .cxx17
)
