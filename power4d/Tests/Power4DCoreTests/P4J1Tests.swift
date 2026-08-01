import Foundation
import XCTest

@testable import Power4DCore

final class P4J1Tests: XCTestCase {
  func testValidFrame() throws {
    let payload = Data(#"{"type":"input_state"}"#.utf8)
    let line = makeFrame(payload)
    XCTAssertEqual(try P4J1.parse(line), payload)
  }

  func testIgnoresNonFrameNoise() throws {
    XCTAssertNil(try P4J1.parse(Data("ordinary log line".utf8)))
  }

  func testRejectsMalformedSizeAndChecksum() {
    let payload = Data("{}".utf8)
    XCTAssertThrowsError(
      try P4J1.parse(Data("P4J1 nope 0000000000000000000000000000000000000000 {}".utf8))
    )
    XCTAssertThrowsError(
      try P4J1.parse(
        Data("P4J1 3 \(Power4Crypto.sha1Hex(payload)) {}".utf8)
      )
    )
    XCTAssertThrowsError(
      try P4J1.parse(Data("P4J1 2 0000000000000000000000000000000000000000 {}".utf8))
    )
  }

  func testHMACVector() {
    let digest = Power4Crypto.hmacSHA256(
      key: "key",
      message: "The quick brown fox jumps over the lazy dog"
    )
    XCTAssertEqual(
      Power4Crypto.hex(digest),
      "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"
    )
  }

  private func makeFrame(_ payload: Data) -> Data {
    Data("P4J1 \(payload.count) \(Power4Crypto.sha1Hex(payload)) ".utf8) + payload
  }
}
