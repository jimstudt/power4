import ArgumentParser
import XCTest

@testable import Power4DCommands

final class Power4DCommandTests: XCTestCase {
  func testParsesWithoutArguments() throws {
    _ = try Power4DCommand.parse([])
  }

  func testGreeting() {
    XCTAssertEqual(Power4DCommand.greeting, "hello world")
  }

  func testRejectsRemovedMonitorCommand() {
    XCTAssertThrowsError(try Power4DCommand.parse(["monitor"]))
  }
}
