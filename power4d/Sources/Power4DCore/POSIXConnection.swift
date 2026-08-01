import Foundation
import Power4DSystem

final class POSIXConnection: @unchecked Sendable {
  private static let readChunkBytes = 4096

  private(set) var fileDescriptor: Int32
  private var buffered = Data()
  private let node: String
  private let logger: DaemonLogger

  init(fileDescriptor: Int32, node: String, logger: DaemonLogger) {
    self.fileDescriptor = fileDescriptor
    self.node = node
    self.logger = logger
  }

  deinit {
    close()
  }

  static func openSerial(
    path: String,
    baud: Int,
    node: String,
    logger: DaemonLogger
  ) throws -> POSIXConnection {
    var descriptor: Int32 = -1
    var systemError: Int32 = 0
    let status = path.withCString { pointer in
      power4_open_serial(pointer, Int32(baud), &descriptor, &systemError)
    }
    switch status {
    case 0:
      return POSIXConnection(fileDescriptor: descriptor, node: node, logger: logger)
    case 3:
      throw TransportError.busy(path)
    default:
      throw TransportError.system(
        operation: "open \(path)",
        detail: systemErrorDescription(systemError)
      )
    }
  }

  static func openTCP(
    host: String,
    addresses: [UInt32],
    deadline: Int64,
    node: String,
    logger: DaemonLogger
  ) throws -> POSIXConnection {
    var lastError = "no IPv4 addresses"
    for address in addresses {
      var descriptor: Int32 = -1
      var systemError: Int32 = 0
      let status = power4_connect_ipv4(
        address,
        4244,
        deadline,
        &descriptor,
        &systemError
      )
      if status == 0 {
        return POSIXConnection(fileDescriptor: descriptor, node: node, logger: logger)
      }
      if status == 1 {
        throw TransportError.timeout("connect to \(host):4244")
      }
      lastError = systemErrorDescription(systemError)
    }
    throw TransportError.system(operation: "connect to \(host):4244", detail: lastError)
  }

  func close() {
    if fileDescriptor >= 0 {
      power4_close(fileDescriptor)
      fileDescriptor = -1
    }
  }

  func write(_ data: Data, deadline: Int64) throws {
    guard fileDescriptor >= 0 else { throw TransportError.closed }
    logger.trace(node: node, direction: ">>>", data: data)
    var systemError: Int32 = 0
    let status = data.withUnsafeBytes { bytes in
      power4_write_all(
        fileDescriptor,
        bytes.baseAddress,
        bytes.count,
        deadline,
        &systemError
      )
    }
    try checkStatus(status, operation: "write", systemError: systemError)
  }

  func readLine(deadline: Int64, maximumBytes: Int = 131_072) throws -> Data {
    while true {
      if let newline = buffered.firstIndex(of: 0x0a) {
        var line = Data(buffered[..<newline])
        buffered.removeSubrange(...newline)
        if line.last == 0x0d {
          line.removeLast()
        }
        return line
      }
      guard buffered.count < maximumBytes else {
        throw TransportError.lineTooLong(maximumBytes)
      }
      try readMore(deadline: deadline)
    }
  }

  func readUntil(_ marker: Data, deadline: Int64, maximumBytes: Int = 4096) throws {
    while true {
      if let range = buffered.range(of: marker) {
        buffered.removeSubrange(..<range.upperBound)
        return
      }
      guard buffered.count < maximumBytes else {
        throw TransportError.protocolError("response exceeded \(maximumBytes) bytes")
      }
      try readMore(deadline: deadline)
    }
  }

  func consumeIfPresent(_ marker: Data) -> Bool {
    guard let range = buffered.range(of: marker) else { return false }
    buffered.removeSubrange(..<range.upperBound)
    return true
  }

  private func readMore(deadline: Int64) throws {
    guard fileDescriptor >= 0 else { throw TransportError.closed }
    var bytes = [UInt8](repeating: 0, count: Self.readChunkBytes)
    var count = 0
    var systemError: Int32 = 0
    let status = bytes.withUnsafeMutableBytes { buffer in
      power4_read_some(
        fileDescriptor,
        buffer.baseAddress,
        buffer.count,
        deadline,
        &count,
        &systemError
      )
    }
    try checkStatus(status, operation: "read", systemError: systemError)
    let data = Data(bytes.prefix(count))
    logger.trace(node: node, direction: "<<<", data: data)
    buffered.append(data)
  }

  private func checkStatus(_ status: Int32, operation: String, systemError: Int32) throws {
    switch status {
    case 0: return
    case 1: throw TransportError.timeout(operation)
    case 2: throw TransportError.closed
    default:
      throw TransportError.system(
        operation: operation,
        detail: Self.systemErrorDescription(systemError)
      )
    }
  }

  private static func systemErrorDescription(_ error: Int32) -> String {
    String(cString: power4_system_error_string(error))
  }
}

public enum TransportError: Error, CustomStringConvertible {
  case busy(String)
  case timeout(String)
  case closed
  case lineTooLong(Int)
  case protocolError(String)
  case system(operation: String, detail: String)

  public var description: String {
    switch self {
    case .busy(let path): return "serial port is busy: \(path)"
    case .timeout(let operation): return "timed out during \(operation)"
    case .closed: return "connection closed"
    case .lineTooLong(let limit): return "response line exceeds \(limit) bytes"
    case .protocolError(let detail): return detail
    case .system(let operation, let detail): return "\(operation): \(detail)"
    }
  }
}

func monotonicMilliseconds() -> Int64 {
  power4_monotonic_milliseconds()
}
