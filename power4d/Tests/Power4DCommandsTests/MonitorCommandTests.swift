import ArgumentParser
import Power4DCore
import XCTest

@testable import Power4DCommands

final class MonitorCommandTests: XCTestCase {
  func testDefaults() throws {
    let command = try MonitorCommand.parse([])
    XCTAssertEqual(command.address, "0.0.0.0")
    XCTAssertEqual(command.port, 3366)
  }

  func testCustomAddressAndPort() throws {
    let command = try MonitorCommand.parse([
      "--address", "127.0.0.1",
      "--port", "4245",
    ])
    XCTAssertEqual(command.address, "127.0.0.1")
    XCTAssertEqual(command.port, 4245)
  }

  func testRejectsHostname() {
    XCTAssertThrowsError(
      try MonitorCommand.parse(["--address", "localhost"])
    )
  }

  func testRejectsPortZero() {
    XCTAssertThrowsError(
      try MonitorCommand.parse(["--port", "0"])
    )
  }

  func testSummaryContainsEnvelopeMetadata() throws {
    let content = #"{"protocol":"power4-espnow","version":1}"#
    let json = Data(
      """
      {"protocol":"power4-espnow-gateway","version":1,"source_mac":"01:23:45:67:89:ab","destination_mac":"de:ad:be:ef:00:01","rssi_dbm":-67,"channel":6,"rate":2,"signal_mode":1,"mcs":3,"noise_floor_dbm":-96,"size":\(content.utf8.count),"payload_encoding":"base64","content":"\(Data(content.utf8).base64EncodedString())"}
      """.utf8
    )
    let envelope = try GatewayEnvelope.decodeAndValidate(json)
    let summary = MonitorCommand.summary(envelope: envelope, source: "127.0.0.1:12345")

    XCTAssertEqual(
      summary,
      "from=127.0.0.1:12345 protocol=power4-espnow-gateway version=1 "
        + "source_mac=01:23:45:67:89:ab destination_mac=de:ad:be:ef:00:01 "
        + "rssi_dbm=-67 channel=6 rate=2 signal_mode=1 mcs=3 "
        + "noise_floor_dbm=-96 size=\(content.utf8.count)"
    )
    XCTAssertEqual(try MonitorCommand.printableContent(envelope: envelope), content)
  }
}
