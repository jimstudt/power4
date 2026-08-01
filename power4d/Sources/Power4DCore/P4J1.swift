import Foundation

public enum P4J1 {
  public static func parse(_ line: Data) throws -> Data? {
    let prefix = Data("P4J1 ".utf8)
    guard line.starts(with: prefix) else {
      return nil
    }

    let bytes = [UInt8](line)
    guard let sizeEnd = bytes[prefix.count...].firstIndex(of: 0x20) else {
      throw P4J1Error.malformed
    }
    let sizeBytes = bytes[prefix.count..<sizeEnd]
    guard !sizeBytes.isEmpty,
      sizeBytes.allSatisfy({ $0 >= 0x30 && $0 <= 0x39 }),
      let declaredSize = Int(String(decoding: sizeBytes, as: UTF8.self))
    else {
      throw P4J1Error.malformed
    }

    let digestStart = sizeEnd + 1
    let digestEnd = digestStart + 40
    guard digestEnd < bytes.count, bytes[digestEnd] == 0x20 else {
      throw P4J1Error.malformed
    }
    let digestBytes = bytes[digestStart..<digestEnd]
    guard
      digestBytes.allSatisfy({ byte in
        (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x46)
          || (byte >= 0x61 && byte <= 0x66)
      })
    else {
      throw P4J1Error.malformed
    }

    let payload = Data(bytes[(digestEnd + 1)...])
    guard payload.count == declaredSize else {
      throw P4J1Error.sizeMismatch(declared: declaredSize, actual: payload.count)
    }
    let expected = String(decoding: digestBytes, as: UTF8.self).lowercased()
    let actual = Power4Crypto.sha1Hex(payload)
    guard expected == actual else {
      throw P4J1Error.checksumMismatch(expected: expected, actual: actual)
    }
    return payload
  }
}

public enum P4J1Error: Error, CustomStringConvertible, Equatable {
  case malformed
  case sizeMismatch(declared: Int, actual: Int)
  case checksumMismatch(expected: String, actual: String)

  public var description: String {
    switch self {
    case .malformed:
      return "malformed P4J1 frame"
    case .sizeMismatch(let declared, let actual):
      return "P4J1 size mismatch: expected \(declared), got \(actual)"
    case .checksumMismatch(let expected, let actual):
      return "P4J1 SHA-1 mismatch: expected \(expected), got \(actual)"
    }
  }
}
