import Foundation

final class DeviceSession {
  private static let prompt = Data("power4> ".utf8)

  private let connection: POSIXConnection
  private let serial: Bool
  private let cycleDeadline: Int64

  init(connection: POSIXConnection, serial: Bool, cycleDeadline: Int64) {
    self.connection = connection
    self.serial = serial
    self.cycleDeadline = cycleDeadline
  }

  func authenticate(password: String) throws {
    let challengeLine = try connection.readLine(deadline: cycleDeadline, maximumBytes: 96)
    let prefix = Data("authenticate ".utf8)
    guard challengeLine.starts(with: prefix) else {
      throw TransportError.protocolError("invalid authentication challenge")
    }
    let challengeData = challengeLine.dropFirst(prefix.count)
    guard challengeData.count == 43,
      let challenge = String(data: challengeData, encoding: .utf8)
    else {
      throw TransportError.protocolError("invalid authentication challenge")
    }

    let digest = Power4Crypto.hex(Power4Crypto.hmacSHA256(key: password, message: challenge))
    try connection.write(
      Data("authenticate \(digest)\r\n".utf8),
      deadline: cycleDeadline
    )
    let response = try connection.readLine(deadline: cycleDeadline, maximumBytes: 96)
    guard response == Data("authenticated".utf8) else {
      throw TransportError.protocolError("authentication failed")
    }
  }

  func requestReport(_ name: String) throws -> Result<Data, ReportResponseError> {
    try waitForPrompt()
    let command = "p4exec report \(name)"
    try connection.write(Data((command + "\r").utf8), deadline: ioDeadline())
    return try readDotResponse(echoedCommand: serial ? command : nil)
  }

  private func waitForPrompt() throws {
    if connection.consumeIfPresent(Self.prompt) {
      return
    }
    if !serial {
      try connection.readUntil(Self.prompt, deadline: cycleDeadline)
      return
    }

    let operationDeadline = ioDeadline()
    for attempt in 0..<3 {
      try connection.write(Data([0x0d]), deadline: operationDeadline)
      let now = monotonicMilliseconds()
      let remainingAttempts = Int64(3 - attempt)
      let attemptDeadline = min(
        operationDeadline,
        now + max(1, (operationDeadline - now) / remainingAttempts)
      )
      do {
        try connection.readUntil(Self.prompt, deadline: attemptDeadline)
        return
      } catch TransportError.timeout {
        continue
      }
    }
    throw TransportError.timeout("serial prompt")
  }

  private func readDotResponse(
    echoedCommand: String?
  ) throws -> Result<Data, ReportResponseError> {
    var echoPending = echoedCommand != nil
    var payload: Data?
    var frameError: ReportResponseError?

    while true {
      var line = try connection.readLine(deadline: ioDeadline())
      if echoPending, line == Data(echoedCommand!.utf8) {
        echoPending = false
        continue
      }
      echoPending = false

      if line == Data([0x2e]) {
        if let frameError {
          return .failure(frameError)
        }
        guard let payload else {
          return .failure(.missingFrame)
        }
        return .success(payload)
      }
      if line.first == 0x2e {
        guard line.count > 1, line[line.index(after: line.startIndex)] == 0x2e else {
          frameError = .invalidDotStuffing
          continue
        }
        line.removeFirst()
      }

      do {
        if let parsed = try P4J1.parse(line) {
          payload = parsed
        }
      } catch {
        frameError = .invalidFrame(String(describing: error))
      }
    }
  }

  private func ioDeadline() -> Int64 {
    serial ? min(cycleDeadline, monotonicMilliseconds() + 2000) : cycleDeadline
  }
}

public enum ReportResponseError: Error, CustomStringConvertible {
  case missingFrame
  case invalidDotStuffing
  case invalidFrame(String)

  public var description: String {
    switch self {
    case .missingFrame: return "response did not contain a P4J1 frame"
    case .invalidDotStuffing: return "response contained invalid dot stuffing"
    case .invalidFrame(let detail): return detail
    }
  }
}
