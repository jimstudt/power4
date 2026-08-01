import ArgumentParser
import Foundation
import Power4DCore

public struct Power4DCommand: AsyncParsableCommand {
  public static let configuration = CommandConfiguration(
    commandName: "power4d",
    abstract: "Host services for power4 controllers.",
    subcommands: [MonitorCommand.self]
  )

  public init() {}
}

public struct MonitorCommand: AsyncParsableCommand {
  public static let configuration = CommandConfiguration(
    commandName: "monitor",
    abstract: "Monitor UDP datagrams forwarded by power4 ESP-NOW gateways."
  )

  @Option(help: "Numeric IPv4 address on which to listen.")
  public var address = "0.0.0.0"

  @Option(help: "UDP port on which to listen.")
  public var port: UInt16 = 3366

  public init() {}

  public mutating func validate() throws {
    guard isValidIPv4Address(address) else {
      throw ValidationError("--address must be a numeric IPv4 address")
    }
    guard port != 0 else {
      throw ValidationError("--port must be between 1 and 65535")
    }
  }

  public mutating func run() async throws {
    let listener = try UDPDatagramListener(address: address, port: port)
    writeStandardError("power4d monitor: listening on \(address):\(listener.port)")

    try await listener.consume { datagram in
      if datagram.droppedBefore > 0 {
        writeStandardError(
          "power4d monitor: dropped \(datagram.droppedBefore) datagram(s) "
            + "because the processing buffer was full"
        )
      }

      do {
        let envelope = try GatewayEnvelope.decodeAndValidate(datagram.data)
        print(Self.summary(envelope: envelope, source: datagram.source.description))
        print(try Self.printableContent(envelope: envelope))
      } catch {
        writeStandardError(
          "power4d monitor: ignored datagram from \(datagram.source): \(error)"
        )
      }
    }
  }

  static func summary(envelope: GatewayEnvelope, source: String) -> String {
    [
      "from=\(source)",
      "protocol=\(envelope.protocolName)",
      "version=\(envelope.version)",
      "source_mac=\(envelope.sourceMac)",
      "destination_mac=\(envelope.destinationMac)",
      "rssi_dbm=\(envelope.rssiDbm)",
      "channel=\(envelope.channel)",
      "rate=\(envelope.rate)",
      "signal_mode=\(envelope.signalMode)",
      "mcs=\(envelope.mcs)",
      "noise_floor_dbm=\(envelope.noiseFloorDbm)",
      "size=\(envelope.size)",
    ].joined(separator: " ")
  }

  static func printableContent(envelope: GatewayEnvelope) throws -> String {
    String(decoding: try envelope.decodedContent(), as: UTF8.self)
  }
}

private func writeStandardError(_ message: String) {
  FileHandle.standardError.write(Data((message + "\n").utf8))
}
