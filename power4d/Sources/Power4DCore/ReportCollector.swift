import Dispatch
import Foundation

public final class ReportCollector: @unchecked Sendable {
  public static let reportNames = [
    "batteries", "banks", "relays", "inputs", "parameters", "logs",
  ]

  private let node: PreparedNode
  private let baud: Int
  private let outputDirectory: URL
  private let logger: DaemonLogger

  var nodeName: String { node.directory }

  public init(
    node: PreparedNode,
    baud: Int,
    outputRoot: URL,
    logger: DaemonLogger
  ) throws {
    self.node = node
    self.baud = baud
    self.outputDirectory = outputRoot.appendingPathComponent(node.directory, isDirectory: true)
    self.logger = logger
    try FileManager.default.createDirectory(
      at: outputDirectory,
      withIntermediateDirectories: true
    )
  }

  public func collectCycle(deadline: Int64) {
    let connection: POSIXConnection
    let serial: Bool
    do {
      switch node.access {
      case .serial(let path):
        serial = true
        connection = try POSIXConnection.openSerial(
          path: path,
          baud: baud,
          node: node.directory,
          logger: logger
        )
      case .tcp(let host, let addresses, let password):
        serial = false
        connection = try POSIXConnection.openTCP(
          host: host,
          addresses: addresses,
          deadline: deadline,
          node: node.directory,
          logger: logger
        )
        let session = DeviceSession(
          connection: connection,
          serial: false,
          cycleDeadline: deadline
        )
        try session.authenticate(password: password)
      }
    } catch {
      logger.log(node: node.directory, String(describing: error))
      return
    }
    defer { connection.close() }

    let session = DeviceSession(
      connection: connection,
      serial: serial,
      cycleDeadline: deadline
    )
    for report in Self.reportNames {
      do {
        switch try session.requestReport(report) {
        case .success(let json):
          do {
            try writeReport(json, named: report)
          } catch {
            logger.log(node: node.directory, "cannot write \(report).json: \(error)")
          }
        case .failure(let error):
          logger.log(node: node.directory, "\(report) report failed: \(error)")
        }
      } catch {
        logger.log(node: node.directory, "\(report) report failed: \(error)")
        return
      }
    }
  }

  private func writeReport(_ json: Data, named name: String) throws {
    var contents = json
    contents.append(0x0a)
    let destination = outputDirectory.appendingPathComponent("\(name).json")
    try contents.write(to: destination, options: .atomic)
  }
}

public final class ReportDaemon: @unchecked Sendable {
  private let collectors: [ReportCollector]
  private let intervalMilliseconds: Int64

  public init(
    nodes: [PreparedNode],
    intervalSeconds: Int,
    baud: Int,
    outputDirectory: String,
    logger: DaemonLogger
  ) throws {
    self.intervalMilliseconds = Int64(intervalSeconds) * 1000
    let outputRoot = URL(fileURLWithPath: outputDirectory, isDirectory: true)
    try FileManager.default.createDirectory(
      at: outputRoot,
      withIntermediateDirectories: true
    )
    self.collectors = try nodes.map { node in
      try ReportCollector(
        node: node,
        baud: baud,
        outputRoot: outputRoot,
        logger: logger
      )
    }
  }

  public func run() async {
    await withTaskGroup(of: Void.self) { group in
      for collector in collectors {
        group.addTask { [self] in
          await run(collector: collector)
        }
      }
      await group.waitForAll()
    }
  }

  private func run(collector: ReportCollector) async {
    let queue = DispatchQueue(label: "power4d.node.\(collector.nodeName)")
    while !Task.isCancelled {
      let cycleStart = monotonicMilliseconds()
      let deadline = cycleStart + (intervalMilliseconds / 2)
      await withCheckedContinuation { continuation in
        queue.async {
          collector.collectCycle(deadline: deadline)
          continuation.resume()
        }
      }

      let sleepMilliseconds = cycleStart + intervalMilliseconds - monotonicMilliseconds()
      if sleepMilliseconds > 0 {
        do {
          try await Task.sleep(for: .milliseconds(sleepMilliseconds))
        } catch {
          return
        }
      }
    }
  }
}
