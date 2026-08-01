import ArgumentParser

public struct Power4DCommand: ParsableCommand {
  public static let greeting = "hello world"

  public static let configuration = CommandConfiguration(
    commandName: "power4d",
    abstract: "Host services for power4 controllers."
  )

  public init() {}

  public mutating func run() throws {
    print(Self.greeting)
  }
}
