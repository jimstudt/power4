import ArgumentParser
import XCTest

@testable import Power4DCommands

final class Power4DCommandTests: XCTestCase {
  func testDefaultsAndMixedNodes() throws {
    let command = try Power4DCommand.parse([
      "tcp:controller.example:PW1:shed",
      "serial:/dev/serial/by-id/controller:house",
    ])
    XCTAssertEqual(command.interval, 60)
    XCTAssertEqual(command.baud, 115_200)
    XCTAssertEqual(command.outputDirectory, "/run/power4")
    XCTAssertFalse(command.verbose)
    XCTAssertEqual(command.nodes.count, 2)
  }

  func testCustomGlobalOptions() throws {
    let command = try Power4DCommand.parse([
      "--interval", "120",
      "--baud", "57600",
      "--output-directory", "/tmp/reports",
      "--verbose",
      "serial:/dev/ttyUSB0:workshop",
    ])
    XCTAssertEqual(command.interval, 120)
    XCTAssertEqual(command.baud, 57_600)
    XCTAssertEqual(command.outputDirectory, "/tmp/reports")
    XCTAssertTrue(command.verbose)
  }

  func testRejectsEmptyAndInvalidConfiguration() {
    XCTAssertThrowsError(try Power4DCommand.parse([]))
    XCTAssertThrowsError(try Power4DCommand.parse(["monitor"]))
    XCTAssertThrowsError(
      try Power4DCommand.parse([
        "serial:/dev/ttyUSB0:same",
        "serial:/dev/ttyUSB1:same",
      ])
    )
    XCTAssertThrowsError(
      try Power4DCommand.parse(["--interval", "1", "serial:/dev/ttyUSB0:node"])
    )
    XCTAssertThrowsError(
      try Power4DCommand.parse(["--baud", "12345", "serial:/dev/ttyUSB0:node"])
    )
  }
}
