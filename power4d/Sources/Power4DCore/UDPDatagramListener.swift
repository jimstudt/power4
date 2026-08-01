import Dispatch
import Foundation

#if os(Linux)
  import Glibc
#else
  import Darwin
#endif

private let maximumIPv4UDPDatagramBytes = 65_535

#if os(Linux)
  private let datagramSocketType = Int32(SOCK_DGRAM.rawValue)
#else
  private let datagramSocketType = SOCK_DGRAM
#endif

public struct IPv4Endpoint: Sendable, Equatable, CustomStringConvertible {
  public let address: String
  public let port: UInt16

  public var description: String {
    "\(address):\(port)"
  }
}

public struct UDPDatagram: Sendable, Equatable {
  public let data: Data
  public let source: IPv4Endpoint
  public let droppedBefore: Int
}

public enum UDPListenerError: Error, CustomStringConvertible, Sendable, Equatable {
  case invalidAddress(String)
  case invalidBufferCapacity(Int)
  case systemCall(operation: String, code: Int32)

  public var description: String {
    switch self {
    case .invalidAddress(let value):
      return "invalid IPv4 address: \(value)"
    case .invalidBufferCapacity(let value):
      return "UDP stream buffer capacity must be positive: \(value)"
    case .systemCall(let operation, let code):
      return "\(operation) failed (errno \(code): \(String(cString: strerror(code))))"
    }
  }
}

public func isValidIPv4Address(_ value: String) -> Bool {
  var address = in_addr()
  return value.withCString { text in
    inet_pton(AF_INET, text, &address) == 1
  }
}

public actor UDPDatagramListener {
  public nonisolated let datagrams: AsyncThrowingStream<UDPDatagram, Error>
  public nonisolated let port: UInt16

  private let pump: UDPDatagramPump

  public init(address: String, port: UInt16, bufferCapacity: Int = 64) throws {
    guard bufferCapacity > 0 else {
      throw UDPListenerError.invalidBufferCapacity(bufferCapacity)
    }

    let socket = try Self.openSocket(address: address, port: port)
    let pair = AsyncThrowingStream<UDPDatagram, Error>.makeStream(
      bufferingPolicy: .bufferingOldest(bufferCapacity)
    )
    let pump = UDPDatagramPump(
      descriptor: socket.descriptor,
      continuation: pair.continuation
    )

    self.datagrams = pair.stream
    self.port = socket.port
    self.pump = pump

    pair.continuation.onTermination = { [weak pump] _ in
      pump?.cancel()
    }
    pump.start()
  }

  public func cancel() {
    pump.cancel()
  }

  public func consume(
    _ handler: @Sendable (UDPDatagram) async throws -> Void
  ) async throws {
    for try await datagram in datagrams {
      try await handler(datagram)
    }
  }

  deinit {
    pump.cancel()
  }

  private static func openSocket(address: String, port: UInt16) throws -> (
    descriptor: Int32,
    port: UInt16
  ) {
    guard isValidIPv4Address(address) else {
      throw UDPListenerError.invalidAddress(address)
    }

    let descriptor = socket(AF_INET, datagramSocketType, 0)
    guard descriptor >= 0 else {
      throw systemError("socket")
    }

    do {
      var reuseAddress: Int32 = 1
      guard
        setsockopt(
          descriptor,
          SOL_SOCKET,
          SO_REUSEADDR,
          &reuseAddress,
          socklen_t(MemoryLayout.size(ofValue: reuseAddress))
        ) == 0
      else {
        throw systemError("setsockopt(SO_REUSEADDR)")
      }

      let flags = fcntl(descriptor, F_GETFL, 0)
      guard flags >= 0 else {
        throw systemError("fcntl(F_GETFL)")
      }
      guard fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0 else {
        throw systemError("fcntl(F_SETFL)")
      }

      var bindAddress = sockaddr_in()
      bindAddress.sin_family = sa_family_t(AF_INET)
      bindAddress.sin_port = port.bigEndian
      let parsed = address.withCString { text in
        inet_pton(AF_INET, text, &bindAddress.sin_addr)
      }
      guard parsed == 1 else {
        throw UDPListenerError.invalidAddress(address)
      }

      let bindResult = withUnsafePointer(to: &bindAddress) { pointer in
        pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
          bind(
            descriptor,
            socketAddress,
            socklen_t(MemoryLayout<sockaddr_in>.size)
          )
        }
      }
      guard bindResult == 0 else {
        throw systemError("bind")
      }

      var localAddress = sockaddr_in()
      var localAddressLength = socklen_t(MemoryLayout<sockaddr_in>.size)
      let nameResult = withUnsafeMutablePointer(to: &localAddress) { pointer in
        pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
          getsockname(descriptor, socketAddress, &localAddressLength)
        }
      }
      guard nameResult == 0 else {
        throw systemError("getsockname")
      }

      return (descriptor, UInt16(bigEndian: localAddress.sin_port))
    } catch {
      systemClose(descriptor)
      throw error
    }
  }
}

private final class UDPDatagramPump: @unchecked Sendable {
  private let descriptor: Int32
  private let continuation: AsyncThrowingStream<UDPDatagram, Error>.Continuation
  private let queue = DispatchQueue(label: "power4d.udp-listener")
  private let source: DispatchSourceRead
  private var receiveBuffer = [UInt8](repeating: 0, count: maximumIPv4UDPDatagramBytes)
  private var pendingDrops = 0

  init(
    descriptor: Int32,
    continuation: AsyncThrowingStream<UDPDatagram, Error>.Continuation
  ) {
    self.descriptor = descriptor
    self.continuation = continuation
    self.source = DispatchSource.makeReadSource(fileDescriptor: descriptor, queue: queue)

    source.setEventHandler { [weak self] in
      self?.drainSocket()
    }
    source.setCancelHandler { [continuation] in
      systemClose(descriptor)
      continuation.finish()
    }
  }

  func start() {
    source.resume()
  }

  func cancel() {
    source.cancel()
  }

  deinit {
    source.cancel()
  }

  private func drainSocket() {
    while true {
      do {
        let datagram = try receiveOne()
        let result = continuation.yield(
          UDPDatagram(
            data: datagram.data,
            source: datagram.source,
            droppedBefore: pendingDrops
          )
        )
        switch result {
        case .enqueued:
          pendingDrops = 0
        case .dropped:
          pendingDrops += 1
        case .terminated:
          source.cancel()
          return
        @unknown default:
          source.cancel()
          return
        }
      } catch UDPReceiveResult.wouldBlock {
        return
      } catch {
        continuation.finish(throwing: error)
        source.cancel()
        return
      }
    }
  }

  private func receiveOne() throws -> (data: Data, source: IPv4Endpoint) {
    while true {
      var sourceAddress = sockaddr_in()
      var sourceAddressLength = socklen_t(MemoryLayout<sockaddr_in>.size)
      let received = receiveBuffer.withUnsafeMutableBytes { bytes -> ssize_t in
        withUnsafeMutablePointer(to: &sourceAddress) { pointer in
          pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { socketAddress in
            recvfrom(
              descriptor,
              bytes.baseAddress,
              bytes.count,
              0,
              socketAddress,
              &sourceAddressLength
            )
          }
        }
      }

      if received >= 0 {
        let data = Data(receiveBuffer.prefix(Int(received)))
        return (data, endpoint(from: sourceAddress))
      }

      let code = errno
      if code == EINTR {
        continue
      }
      if code == EAGAIN || code == EWOULDBLOCK {
        throw UDPReceiveResult.wouldBlock
      }
      throw UDPListenerError.systemCall(operation: "recvfrom", code: code)
    }
  }
}

private enum UDPReceiveResult: Error {
  case wouldBlock
}

private func endpoint(from address: sockaddr_in) -> IPv4Endpoint {
  var binaryAddress = address.sin_addr
  var buffer = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
  let host = withUnsafePointer(to: &binaryAddress) { pointer in
    buffer.withUnsafeMutableBufferPointer { bufferPointer -> String in
      guard
        let text = inet_ntop(
          AF_INET,
          pointer,
          bufferPointer.baseAddress,
          socklen_t(bufferPointer.count)
        )
      else {
        return "<invalid>"
      }
      return String(cString: text)
    }
  }
  return IPv4Endpoint(
    address: host,
    port: UInt16(bigEndian: address.sin_port)
  )
}

private func systemError(_ operation: String) -> UDPListenerError {
  UDPListenerError.systemCall(operation: operation, code: errno)
}

private func systemClose(_ descriptor: Int32) {
  #if os(Linux)
    _ = Glibc.close(descriptor)
  #else
    _ = Darwin.close(descriptor)
  #endif
}
