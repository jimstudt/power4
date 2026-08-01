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
    .target(
      name: "Power4DCommands",
      dependencies: [
        .product(name: "ArgumentParser", package: "swift-argument-parser")
      ]
    ),
    .executableTarget(
      name: "power4d",
      dependencies: ["Power4DCommands"]
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
