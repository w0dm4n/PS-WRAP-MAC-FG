// swift-tools-version: 5.10
// PS-WRAP-MAC-FG: C ABI wrapper around cinemore/rife-metal (Apache-2.0) running Practical-RIFE v4.25 (MIT) on Metal
import PackageDescription
let package = Package(
    name: "PsWrapRife",
    platforms: [.macOS(.v13)],
    products: [.library(name: "PsWrapRife", type: .dynamic, targets: ["PsWrapRife"])],
    dependencies: [.package(path: "../../../_deps/rife-metal")],
    targets: [.target(name: "PsWrapRife", dependencies: [.product(name: "RifeMetal", package: "rife-metal")])]
)
