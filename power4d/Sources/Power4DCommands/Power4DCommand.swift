import ArgumentParser
import Foundation
import Power4DCore

public struct Power4DCommand: AsyncParsableCommand {
  public static let configuration = CommandConfiguration(
    commandName: "power4d",
    abstract: "Collect state reports from power4 controllers."
  )

  @Option(name: [.short, .long], help: "Seconds between collection cycles.")
  var interval = 60

  @Option(name: [.short, .long], help: "Baud rate for every serial node.")
  var baud = 115_200

  @Option(
    name: [.customShort("o"), .customLong("output-directory")],
    help: "Root directory for node report directories."
  )
  var outputDirectory = "/run/power4"

  @Flag(name: [.short, .long], help: "Log escaped transport traffic.")
  var verbose = false

  @Argument(
    help:
      "Nodes as tcp:<host>:<password-env>:<directory> or serial:<device>:<directory>."
  )
  var nodes: [String] = []

  public init() {}

  public mutating func validate() throws {
    guard interval >= 2 else {
      throw ValidationError("--interval must be at least 2 seconds")
    }
    guard
      [1200, 2400, 4800, 9600, 19_200, 38_400, 57_600, 115_200, 230_400]
        .contains(baud)
    else {
      throw ValidationError("unsupported --baud value: \(baud)")
    }
    guard !outputDirectory.isEmpty else {
      throw ValidationError("--output-directory must not be empty")
    }
    do {
      let descriptors = try nodes.map(NodeDescriptor.init(specification:))
      guard !descriptors.isEmpty else {
        throw ConfigurationError.noNodes
      }
      try NodeDescriptor.validateUniqueDirectories(descriptors)
    } catch {
      throw ValidationError(String(describing: error))
    }
  }

  public mutating func run() async throws {
    let descriptors = try nodes.map(NodeDescriptor.init(specification:))
    let prepared = try NodeConfiguration.prepare(descriptors)
    let logger = DaemonLogger(verbose: verbose)
    logger.log(
      "starting \(prepared.count) node(s), interval=\(interval)s baud=\(baud) "
        + "output=\(outputDirectory)"
    )
    for node in prepared {
      switch node.access {
      case .serial(let path):
        logger.log(node: node.directory, "configured serial path \(path)")
      case .tcp(let host, _, _):
        logger.log(node: node.directory, "configured TCP host \(host):4244")
      }
    }

    let daemon = try ReportDaemon(
      nodes: prepared,
      intervalSeconds: interval,
      baud: baud,
      outputDirectory: outputDirectory,
      logger: logger
    )
    await daemon.run()
  }
}
