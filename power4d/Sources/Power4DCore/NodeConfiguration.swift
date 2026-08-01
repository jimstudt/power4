import Foundation
import Power4DSystem

public enum NodeAccess: Equatable, Sendable {
  case serial(path: String)
  case tcp(host: String, passwordEnvironment: String)
}

public struct NodeDescriptor: Equatable, Sendable {
  public let access: NodeAccess
  public let directory: String

  public init(specification: String) throws {
    if specification.hasPrefix("serial:") {
      let body = specification.dropFirst("serial:".count)
      guard let separator = body.lastIndex(of: ":") else {
        throw ConfigurationError.invalidNode(specification)
      }
      let path = String(body[..<separator])
      let directory = String(body[body.index(after: separator)...])
      guard !path.isEmpty else {
        throw ConfigurationError.invalidNode(specification)
      }
      try Self.validateDirectory(directory)
      self.access = .serial(path: path)
      self.directory = directory
      return
    }

    if specification.hasPrefix("tcp:") {
      let fields = specification.dropFirst("tcp:".count).split(
        separator: ":",
        omittingEmptySubsequences: false
      )
      guard fields.count == 3 else {
        throw ConfigurationError.invalidNode(specification)
      }
      let host = String(fields[0])
      let passwordEnvironment = String(fields[1])
      let directory = String(fields[2])
      guard !host.isEmpty, Self.validEnvironmentName(passwordEnvironment) else {
        throw ConfigurationError.invalidNode(specification)
      }
      try Self.validateDirectory(directory)
      self.access = .tcp(host: host, passwordEnvironment: passwordEnvironment)
      self.directory = directory
      return
    }

    throw ConfigurationError.invalidNode(specification)
  }

  public static func validateUniqueDirectories(_ nodes: [NodeDescriptor]) throws {
    var directories = Set<String>()
    for node in nodes where !directories.insert(node.directory).inserted {
      throw ConfigurationError.duplicateDirectory(node.directory)
    }
  }

  private static func validateDirectory(_ directory: String) throws {
    guard directory != ".", directory != "..", !directory.isEmpty else {
      throw ConfigurationError.invalidDirectory(directory)
    }
    let valid = directory.utf8.allSatisfy { byte in
      (byte >= 48 && byte <= 57) || (byte >= 65 && byte <= 90)
        || (byte >= 97 && byte <= 122) || byte == 45 || byte == 46 || byte == 95
    }
    guard valid else {
      throw ConfigurationError.invalidDirectory(directory)
    }
  }

  private static func validEnvironmentName(_ name: String) -> Bool {
    guard let first = name.utf8.first,
      first == 95 || (first >= 65 && first <= 90) || (first >= 97 && first <= 122)
    else {
      return false
    }
    return name.utf8.dropFirst().allSatisfy { byte in
      byte == 95 || (byte >= 48 && byte <= 57) || (byte >= 65 && byte <= 90)
        || (byte >= 97 && byte <= 122)
    }
  }
}

public enum PreparedAccess: Sendable {
  case serial(path: String)
  case tcp(host: String, addresses: [UInt32], password: String)
}

public struct PreparedNode: Sendable {
  public let access: PreparedAccess
  public let directory: String
}

public enum NodeConfiguration {
  public static func prepare(
    _ descriptors: [NodeDescriptor],
    environment: [String: String] = ProcessInfo.processInfo.environment
  ) throws -> [PreparedNode] {
    guard !descriptors.isEmpty else {
      throw ConfigurationError.noNodes
    }
    try NodeDescriptor.validateUniqueDirectories(descriptors)

    return try descriptors.map { descriptor in
      switch descriptor.access {
      case .serial(let path):
        try validateSerialPath(path)
        return PreparedNode(access: .serial(path: path), directory: descriptor.directory)
      case .tcp(let host, let passwordEnvironment):
        guard let password = environment[passwordEnvironment] else {
          throw ConfigurationError.missingPassword(passwordEnvironment)
        }
        try validatePassword(password, environmentName: passwordEnvironment)
        return PreparedNode(
          access: .tcp(
            host: host,
            addresses: try resolveIPv4(host),
            password: password
          ),
          directory: descriptor.directory
        )
      }
    }
  }

  private static func validateSerialPath(_ path: String) throws {
    var systemError: Int32 = 0
    let status = path.withCString { pointer in
      power4_validate_serial_path(pointer, &systemError)
    }
    switch status {
    case 0:
      return
    case 1:
      throw ConfigurationError.invalidSerialDevice(
        path: path,
        detail: "not a character device"
      )
    default:
      throw ConfigurationError.invalidSerialDevice(
        path: path,
        detail: String(cString: power4_system_error_string(systemError))
      )
    }
  }

  private static func validatePassword(_ password: String, environmentName: String) throws {
    let bytes = Array(password.utf8)
    guard bytes.count >= 16, bytes.count <= 128,
      bytes.allSatisfy({ $0 >= 0x20 && $0 <= 0x7e })
    else {
      throw ConfigurationError.invalidPassword(environmentName)
    }
  }

  private static func resolveIPv4(_ host: String) throws -> [UInt32] {
    var addresses = [UInt32](repeating: 0, count: 8)
    var count = 0
    var systemError: Int32 = 0
    let status = addresses.withUnsafeMutableBufferPointer { buffer in
      host.withCString { hostname in
        power4_resolve_ipv4(
          hostname,
          buffer.baseAddress,
          buffer.count,
          &count,
          &systemError
        )
      }
    }
    guard status == 0 else {
      let detail = String(cString: power4_resolve_error_string(systemError))
      throw ConfigurationError.cannotResolve(host: host, detail: detail)
    }
    return Array(addresses.prefix(count))
  }
}

public enum ConfigurationError: Error, CustomStringConvertible, Equatable {
  case noNodes
  case invalidNode(String)
  case invalidDirectory(String)
  case duplicateDirectory(String)
  case missingPassword(String)
  case invalidPassword(String)
  case invalidSerialDevice(path: String, detail: String)
  case cannotResolve(host: String, detail: String)

  public var description: String {
    switch self {
    case .noNodes:
      return "at least one node specification is required"
    case .invalidNode(let value):
      return "invalid node specification: \(value)"
    case .invalidDirectory(let value):
      return "invalid node output directory: \(value)"
    case .duplicateDirectory(let value):
      return "duplicate node output directory: \(value)"
    case .missingPassword(let name):
      return "TCP password environment variable is not set: \(name)"
    case .invalidPassword(let name):
      return "TCP password in \(name) must contain 16-128 printable bytes"
    case .invalidSerialDevice(let path, let detail):
      return "serial device is unavailable: \(path): \(detail)"
    case .cannotResolve(let host, let detail):
      return "cannot resolve \(host) to IPv4: \(detail)"
    }
  }
}
