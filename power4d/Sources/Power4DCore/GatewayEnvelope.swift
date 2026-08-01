import Foundation

public struct GatewayEnvelope: Decodable, Sendable, Equatable {
  public let protocolName: String
  public let version: Int
  public let sourceMac: String
  public let destinationMac: String
  public let rssiDbm: Int8
  public let channel: UInt8
  public let rate: UInt8
  public let signalMode: UInt8
  public let mcs: UInt8
  public let noiseFloorDbm: Int8
  public let size: Int
  public let payloadEncoding: String
  public let content: String

  enum CodingKeys: String, CodingKey {
    case protocolName = "protocol"
    case version
    case sourceMac = "source_mac"
    case destinationMac = "destination_mac"
    case rssiDbm = "rssi_dbm"
    case channel
    case rate
    case signalMode = "signal_mode"
    case mcs
    case noiseFloorDbm = "noise_floor_dbm"
    case size
    case payloadEncoding = "payload_encoding"
    case content
  }

  public static func decodeAndValidate(_ data: Data) throws -> GatewayEnvelope {
    let envelope: GatewayEnvelope
    do {
      envelope = try JSONDecoder().decode(GatewayEnvelope.self, from: data)
    } catch {
      throw GatewayEnvelopeError.invalidJSON(String(describing: error))
    }
    try envelope.validate()
    return envelope
  }

  public func validate() throws {
    guard protocolName == "power4-espnow-gateway" else {
      throw GatewayEnvelopeError.unsupportedProtocol(protocolName)
    }
    guard version == 1 else {
      throw GatewayEnvelopeError.unsupportedVersion(version)
    }
    guard Self.isValidMacAddress(sourceMac) else {
      throw GatewayEnvelopeError.invalidMacAddress(field: "source_mac", value: sourceMac)
    }
    guard Self.isValidMacAddress(destinationMac) else {
      throw GatewayEnvelopeError.invalidMacAddress(
        field: "destination_mac",
        value: destinationMac
      )
    }
    guard size >= 0 else {
      throw GatewayEnvelopeError.negativeSize(size)
    }
    let decoded = try decodedContent()
    guard decoded.count == size else {
      throw GatewayEnvelopeError.sizeMismatch(declared: size, decoded: decoded.count)
    }
  }

  public func decodedContent() throws -> Data {
    switch payloadEncoding {
    case "base64":
      guard let decoded = Data(base64Encoded: content) else {
        throw GatewayEnvelopeError.invalidBase64
      }
      return decoded
    default:
      throw GatewayEnvelopeError.unsupportedPayloadEncoding(payloadEncoding)
    }
  }

  private static func isValidMacAddress(_ value: String) -> Bool {
    let components = value.split(separator: ":", omittingEmptySubsequences: false)
    guard components.count == 6 else {
      return false
    }
    return components.allSatisfy { component in
      component.count == 2 && UInt8(component, radix: 16) != nil
    }
  }
}

public enum GatewayEnvelopeError: Error, CustomStringConvertible, Sendable, Equatable {
  case invalidJSON(String)
  case unsupportedProtocol(String)
  case unsupportedVersion(Int)
  case invalidMacAddress(field: String, value: String)
  case unsupportedPayloadEncoding(String)
  case negativeSize(Int)
  case invalidBase64
  case sizeMismatch(declared: Int, decoded: Int)

  public var description: String {
    switch self {
    case .invalidJSON(let detail):
      return "invalid gateway JSON: \(detail)"
    case .unsupportedProtocol(let value):
      return "unsupported protocol: \(value)"
    case .unsupportedVersion(let value):
      return "unsupported gateway protocol version: \(value)"
    case .invalidMacAddress(let field, let value):
      return "invalid \(field): \(value)"
    case .unsupportedPayloadEncoding(let value):
      return "unsupported payload_encoding: \(value)"
    case .negativeSize(let value):
      return "size must not be negative: \(value)"
    case .invalidBase64:
      return "content is not valid base64"
    case .sizeMismatch(let declared, let decoded):
      return "size mismatch: declared \(declared), decoded \(decoded)"
    }
  }
}
