// swift-tools-version:6.0
import PackageDescription

let package = Package(
  name: "power4d",
  platforms: [
    .macOS(.v13)
  ],
  dependencies: [
    .package(
      url: "https://github.com/apple/swift-argument-parser",
      from: "1.8.2"
    )
  ],
  targets: [
    .target(name: "Power4DSystem"),
    .target(
      name: "Power4DCore",
      dependencies: ["Power4DSystem"]
    ),
    .target(
      name: "Power4DCommands",
      dependencies: [
        "Power4DCore",
        .product(name: "ArgumentParser", package: "swift-argument-parser"),
      ]
    ),
    .executableTarget(
      name: "power4d",
      dependencies: ["Power4DCommands"]
    ),
    .testTarget(
      name: "Power4DCoreTests",
      dependencies: ["Power4DCore"]
    ),
    .testTarget(
      name: "Power4DCommandsTests",
      dependencies: [
        "Power4DCommands",
        .product(name: "ArgumentParser", package: "swift-argument-parser"),
      ]
    ),
  ]
)
