import Foundation
import XCTest

@testable import Power4DCore

#if os(Linux)
  import Glibc
  private let testDatagramSocketType = Int32(SOCK_DGRAM.rawValue)
#else
  import Darwin
  private let testDatagramSocketType = SOCK_DGRAM
#endif

final class UDPDatagramListenerTests: XCTestCase {
  func testRejectsInvalidAddress() {
    XCTAssertThrowsError(
      try UDPDatagramListener(address: "localhost", port: 3366)
    )
  }

  func testReceivesDatagramAndSenderEndpoint() async throws {
    let listener = try UDPDatagramListener(address: "127.0.0.1", port: 0)
    let sender = try UDPSender(destinationPort: listener.port)
    defer { sender.close() }

    try sender.send(Data("hello".utf8))
    var iterator = listener.datagrams.makeAsyncIterator()
    let received = try await iterator.next()
    let datagram = try XCTUnwrap(received)

    XCTAssertEqual(datagram.data, Data("hello".utf8))
    XCTAssertEqual(datagram.source.address, "127.0.0.1")
    XCTAssertNotEqual(datagram.source.port, 0)
    XCTAssertEqual(datagram.droppedBefore, 0)
    await listener.cancel()
  }

  func testPreservesDatagramBoundariesAndOrder() async throws {
    let listener = try UDPDatagramListener(address: "127.0.0.1", port: 0)
    let sender = try UDPSender(destinationPort: listener.port)
    defer { sender.close() }
    let messages = ["one", "two", "three"].map { Data($0.utf8) }

    for message in messages {
      try sender.send(message)
    }

    var iterator = listener.datagrams.makeAsyncIterator()
    for expected in messages {
      let received = try await iterator.next()
      let datagram = try XCTUnwrap(received)
      XCTAssertEqual(datagram.data, expected)
    }
    await listener.cancel()
  }

  func testActorOwnedConsumerReceivesEntireBurst() async throws {
    let listener = try UDPDatagramListener(address: "127.0.0.1", port: 0)
    let sender = try UDPSender(destinationPort: listener.port)
    defer { sender.close() }
    let messages = (0..<32).map { Data("message-\($0)".utf8) }
    let collector = DatagramCollector(expectedCount: messages.count)

    for message in messages {
      try sender.send(message)
    }

    do {
      try await listener.consume { datagram in
        if await collector.append(datagram.data) {
          throw FinishedConsumingBurst()
        }
      }
    } catch is FinishedConsumingBurst {
      // Expected: the handler stops the otherwise long-lived listener.
    }

    let received = await collector.datagrams
    XCTAssertEqual(received, messages)
    await listener.cancel()
  }

  func testReportsBufferedDatagramDrops() async throws {
    let listener = try UDPDatagramListener(
      address: "127.0.0.1",
      port: 0,
      bufferCapacity: 2
    )
    let sender = try UDPSender(destinationPort: listener.port)
    defer { sender.close() }

    try sender.send(Data("one".utf8))
    try sender.send(Data("two".utf8))
    try sender.send(Data("dropped".utf8))
    try await Task.sleep(for: .milliseconds(100))

    var iterator = listener.datagrams.makeAsyncIterator()
    let receivedFirst = try await iterator.next()
    let first = try XCTUnwrap(receivedFirst)
    let receivedSecond = try await iterator.next()
    let second = try XCTUnwrap(receivedSecond)
    XCTAssertEqual(first.data, Data("one".utf8))
    XCTAssertEqual(second.data, Data("two".utf8))

    try sender.send(Data("after".utf8))
    let receivedAfter = try await iterator.next()
    let after = try XCTUnwrap(receivedAfter)
    XCTAssertEqual(after.data, Data("after".utf8))
    XCTAssertEqual(after.droppedBefore, 1)
    await listener.cancel()
  }
}

private struct FinishedConsumingBurst: Error {}

private actor DatagramCollector {
  private let expectedCount: Int
  private(set) var datagrams: [Data] = []

  init(expectedCount: Int) {
    self.expectedCount = expectedCount
  }

  func append(_ data: Data) -> Bool {
    datagrams.append(data)
    return datagrams.count == expectedCount
  }
}

private final class UDPSender {
  private var descriptor: Int32
  private var destination = sockaddr_in()

  init(destinationPort: UInt16) throws {
    descriptor = socket(AF_INET, testDatagramSocketType, 0)
    guard descriptor >= 0 else {
      throw UDPListenerError.systemCall(operation: "test socket", code: errno)
    }

    destination.sin_family = sa_family_t(AF_INET)
    destination.sin_port = destinationPort.bigEndian
    let parsed = "127.0.0.1".withCString { text in
      inet_pton(AF_INET, text, &destination.sin_addr)
    }
    guard parsed == 1 else {
      close()
      throw UDPListenerError.invalidAddress("127.0.0.1")
    }
  }

  func send(_ data: Data) throws {
    let sent = data.withUnsafeBytes { bytes -> ssize_t in
      withUnsafePointer(to: &destination) { pointer in
        pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
          sendto(
            descriptor,
            bytes.baseAddress,
            bytes.count,
            0,
            socketAddress,
            socklen_t(MemoryLayout<sockaddr_in>.size)
          )
        }
      }
    }
    guard sent == data.count else {
      throw UDPListenerError.systemCall(operation: "test sendto", code: errno)
    }
  }

  func close() {
    guard descriptor >= 0 else {
      return
    }
    #if os(Linux)
      _ = Glibc.close(descriptor)
    #else
      _ = Darwin.close(descriptor)
    #endif
    descriptor = -1
  }

  deinit {
    close()
  }
}
