import Foundation

public final class DaemonLogger: @unchecked Sendable {
  private let verbose: Bool
  private let lock = NSLock()
  private let sink: @Sendable (String) -> Void

  public init(
    verbose: Bool,
    sink: @escaping @Sendable (String) -> Void = { message in
      FileHandle.standardError.write(Data((message + "\n").utf8))
    }
  ) {
    self.verbose = verbose
    self.sink = sink
  }

  public func log(node: String? = nil, _ message: String) {
    emit(node.map { "power4d[\($0)]: \(message)" } ?? "power4d: \(message)")
  }

  func trace(node: String, direction: String, data: Data) {
    guard verbose else { return }
    var rendered = ""
    rendered.reserveCapacity(data.count)
    for byte in data {
      switch byte {
      case 0x0a: rendered += "\\n"
      case 0x0d: rendered += "\\r"
      case 0x09: rendered += "\\t"
      case 0x20...0x7e: rendered.append(Character(UnicodeScalar(byte)))
      default: rendered += String(format: "\\x%02x", byte)
      }
    }
    emit("power4d[\(node)]: \(direction) \(rendered)")
  }

  private func emit(_ message: String) {
    lock.lock()
    sink(message)
    lock.unlock()
  }
}
