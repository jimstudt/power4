import Foundation
import XCTest

@testable import Power4DCore

final class NodeConfigurationTests: XCTestCase {
  func testParsesSerialPathUsingFinalColon() throws {
    let descriptor = try NodeDescriptor(
      specification: "serial:/dev/serial/by-id/controller:channel:house"
    )
    XCTAssertEqual(descriptor.access, .serial(path: "/dev/serial/by-id/controller:channel"))
    XCTAssertEqual(descriptor.directory, "house")
  }

  func testParsesTCPNode() throws {
    let descriptor = try NodeDescriptor(specification: "tcp:localhost:POWER4_PASSWORD:shed")
    XCTAssertEqual(
      descriptor.access,
      .tcp(host: "localhost", passwordEnvironment: "POWER4_PASSWORD")
    )
    XCTAssertEqual(descriptor.directory, "shed")
  }

  func testRejectsUnsafeAndMalformedNodes() {
    for specification in [
      "", "udp:host:node", "tcp:host:PW", "tcp:host:BAD-NAME:node",
      "serial::node", "serial:/dev/tty:..", "serial:/dev/tty:a/b",
      "serial:/dev/tty:two words",
    ] {
      XCTAssertThrowsError(try NodeDescriptor(specification: specification), specification)
    }
  }

  func testRejectsDuplicateDirectories() throws {
    let nodes = try [
      NodeDescriptor(specification: "serial:/dev/tty0:house"),
      NodeDescriptor(specification: "serial:/dev/tty1:house"),
    ]
    XCTAssertThrowsError(try NodeDescriptor.validateUniqueDirectories(nodes))
  }

  func testPreparesTCPPasswordAndIPv4Addresses() throws {
    let descriptor = try NodeDescriptor(specification: "tcp:127.0.0.1:PW1:shed")
    let nodes = try NodeConfiguration.prepare(
      [descriptor],
      environment: ["PW1": "correct horse battery staple"]
    )
    guard case .tcp(let host, let addresses, let password) = nodes[0].access else {
      return XCTFail("expected TCP node")
    }
    XCTAssertEqual(host, "127.0.0.1")
    XCTAssertFalse(addresses.isEmpty)
    XCTAssertEqual(password, "correct horse battery staple")
  }

  func testRejectsMissingAndInvalidPasswords() throws {
    let descriptor = try NodeDescriptor(specification: "tcp:127.0.0.1:PW1:shed")
    XCTAssertThrowsError(try NodeConfiguration.prepare([descriptor], environment: [:]))
    XCTAssertThrowsError(
      try NodeConfiguration.prepare([descriptor], environment: ["PW1": "short"])
    )
    XCTAssertThrowsError(
      try NodeConfiguration.prepare(
        [descriptor],
        environment: ["PW1": "valid length but\nnot printable"]
      )
    )
  }

  func testRejectsMissingAndNonDeviceSerialPathsAtStartup() throws {
    let missing = try NodeDescriptor(
      specification: "serial:/definitely/not/a/power4-device:missing"
    )
    XCTAssertThrowsError(try NodeConfiguration.prepare([missing])) { error in
      XCTAssertTrue(String(describing: error).contains("No such file"))
    }

    let regularFile = FileManager.default.temporaryDirectory.appendingPathComponent(
      UUID().uuidString
    )
    try Data().write(to: regularFile)
    defer { try? FileManager.default.removeItem(at: regularFile) }
    let nonDevice = try NodeDescriptor(
      specification: "serial:\(regularFile.path):regular"
    )
    XCTAssertThrowsError(try NodeConfiguration.prepare([nonDevice])) { error in
      XCTAssertTrue(String(describing: error).contains("not a character device"))
    }
  }

  func testRejectsUnusableOutputRootAtStartup() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(
      UUID().uuidString
    )
    try Data("not a directory".utf8).write(to: root)
    defer { try? FileManager.default.removeItem(at: root) }

    let node = PreparedNode(access: .serial(path: "/dev/null"), directory: "node")
    XCTAssertThrowsError(
      try ReportDaemon(
        nodes: [node],
        intervalSeconds: 60,
        baud: 115_200,
        outputDirectory: root.path,
        logger: DaemonLogger(verbose: false, sink: { _ in })
      )
    )
  }
}
