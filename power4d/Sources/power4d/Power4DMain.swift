import Power4DCommands

@main
struct Power4DMain {
  static func main() async {
    let entryPoint: () async -> Void = Power4DCommand.main
    await entryPoint()
  }
}
