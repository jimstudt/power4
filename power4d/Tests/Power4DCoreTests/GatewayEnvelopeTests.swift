import Foundation
import XCTest

@testable import Power4DCore

final class GatewayEnvelopeTests: XCTestCase {
  func testDecodesAndValidatesGatewayEnvelope() throws {
    let content = Data([0x00, 0xff, 0x10])
    let envelope = try GatewayEnvelope.decodeAndValidate(
      gatewayJSON(content: content)
    )

    XCTAssertEqual(envelope.protocolName, "power4-espnow-gateway")
    XCTAssertEqual(envelope.sourceMac, "01:23:45:67:89:ab")
    XCTAssertEqual(envelope.destinationMac, "de:ad:be:ef:00:01")
    XCTAssertEqual(envelope.rssiDbm, -67)
    XCTAssertEqual(envelope.size, 3)
    XCTAssertEqual(try envelope.decodedContent(), content)
  }

  func testAcceptsContentLargerThanCurrentFirmwarePayload() throws {
    let content = Data(repeating: 0xa5, count: 4_096)
    let envelope = try GatewayEnvelope.decodeAndValidate(gatewayJSON(content: content))
    XCTAssertEqual(envelope.size, content.count)
  }

  func testRejectsMalformedJSON() {
    XCTAssertThrowsError(
      try GatewayEnvelope.decodeAndValidate(Data("not json".utf8))
    )
  }

  func testRejectsWrongProtocol() throws {
    var object = gatewayObject(content: Data())
    object["protocol"] = "other"
    XCTAssertThrowsError(
      try GatewayEnvelope.decodeAndValidate(try JSONSerialization.data(withJSONObject: object)))
  }

  func testRejectsWrongVersion() throws {
    var object = gatewayObject(content: Data())
    object["version"] = 2
    XCTAssertThrowsError(
      try GatewayEnvelope.decodeAndValidate(try JSONSerialization.data(withJSONObject: object)))
  }

  func testRejectsWrongEncoding() throws {
    var object = gatewayObject(content: Data())
    object["payload_encoding"] = "hex"
    let data = try JSONSerialization.data(withJSONObject: object)
    XCTAssertThrowsError(try GatewayEnvelope.decodeAndValidate(data))
  }

  func testRejectsInvalidMacAddresses() throws {
    for field in ["source_mac", "destination_mac"] {
      var object = gatewayObject(content: Data())
      object[field] = "not-a-mac"
      XCTAssertThrowsError(
        try GatewayEnvelope.decodeAndValidate(try JSONSerialization.data(withJSONObject: object)))
    }
  }

  func testRejectsInvalidBase64() throws {
    var object = gatewayObject(content: Data())
    object["content"] = "%%%"
    XCTAssertThrowsError(
      try GatewayEnvelope.decodeAndValidate(try JSONSerialization.data(withJSONObject: object)))
  }

  func testRejectsNegativeSize() throws {
    var object = gatewayObject(content: Data())
    object["size"] = -1
    XCTAssertThrowsError(
      try GatewayEnvelope.decodeAndValidate(try JSONSerialization.data(withJSONObject: object)))
  }

  func testRejectsSizeMismatch() throws {
    var object = gatewayObject(content: Data([1, 2, 3]))
    object["size"] = 4
    XCTAssertThrowsError(
      try GatewayEnvelope.decodeAndValidate(try JSONSerialization.data(withJSONObject: object)))
  }

  private func gatewayJSON(content: Data) -> Data {
    try! JSONSerialization.data(withJSONObject: gatewayObject(content: content))
  }

  private func gatewayObject(content: Data) -> [String: Any] {
    [
      "protocol": "power4-espnow-gateway",
      "version": 1,
      "source_mac": "01:23:45:67:89:ab",
      "destination_mac": "de:ad:be:ef:00:01",
      "rssi_dbm": -67,
      "channel": 6,
      "rate": 2,
      "signal_mode": 1,
      "mcs": 3,
      "noise_floor_dbm": -96,
      "size": content.count,
      "payload_encoding": "base64",
      "content": content.base64EncodedString(),
    ]
  }
}
