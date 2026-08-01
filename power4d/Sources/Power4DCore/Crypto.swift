import Foundation
import Power4DSystem

enum Power4Crypto {
  static func sha1Hex(_ data: Data) -> String {
    var output = [CChar](repeating: 0, count: 41)
    data.withUnsafeBytes { bytes in
      sha1_hex_of(bytes.baseAddress, bytes.count, &output)
    }
    return String(
      decoding: output.prefix(40).map { UInt8(bitPattern: $0) },
      as: UTF8.self
    )
  }

  static func hmacSHA256(key: String, message: String) -> [UInt8] {
    let keyData = Data(key.utf8)
    let messageData = Data(message.utf8)
    var digest = [UInt8](repeating: 0, count: 32)
    keyData.withUnsafeBytes { keyBytes in
      messageData.withUnsafeBytes { messageBytes in
        hmac_sha256(
          keyBytes.baseAddress,
          keyBytes.count,
          messageBytes.baseAddress,
          messageBytes.count,
          &digest
        )
      }
    }
    return digest
  }

  static func hex(_ bytes: [UInt8]) -> String {
    let digits = Array("0123456789abcdef".utf8)
    var output = [UInt8]()
    output.reserveCapacity(bytes.count * 2)
    for byte in bytes {
      output.append(digits[Int(byte >> 4)])
      output.append(digits[Int(byte & 0x0f)])
    }
    return String(decoding: output, as: UTF8.self)
  }
}
