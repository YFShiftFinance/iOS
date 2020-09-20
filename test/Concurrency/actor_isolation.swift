// RUN: %target-typecheck-verify-swift -enable-experimental-concurrency

let immutableGlobal: String = "hello"
var mutableGlobal: String = "can't touch this" // expected-note{{var declared here}}

func globalFunc() { }
func acceptClosure<T>(_: () -> T) { }
func acceptEscapingClosure<T>(_: @escaping () -> T) { }

// ----------------------------------------------------------------------
// Actor state isolation restrictions
// ----------------------------------------------------------------------
actor class MySuperActor {
  var superState: Int = 25

  func superMethod() { }
  func superAsyncMethod() async { }

  subscript (index: Int) -> String { // expected-note{{subscript declared here}}
    "\(index)"
  }
}

actor class MyActor: MySuperActor {
  let immutable: Int = 17
  var text: [String] = [] // expected-note 5{{mutable state is only available within the actor instance}}

  class func synchronousClass() { }
  static func synchronousStatic() { }

  func synchronous() -> String { text.first ?? "nothing" } // expected-note 5{{only asynchronous methods can be used outside the actor instance; do you want to add 'async'?}}
  func asynchronous() async -> String { synchronous() }
}

extension MyActor {
  func testAsynchronous(otherActor: MyActor) async {
    _ = immutable
    _ = synchronous()
    _ = text[0]

    // Accesses on 'self' are okay.
    _ = self.immutable
    _ = self.synchronous()
    _ = await self.asynchronous()
    _ = self.text[0]
    _ = self[0]

    // Accesses on 'super' are okay.
    _ = super.superState
    super.superMethod()
    await super.superAsyncMethod()
    _ = super[0]

    // Accesses on other actors can only reference immutable data or
    // call asychronous methods
    _ = otherActor.immutable // okay
    _ = otherActor.synchronous() // expected-error{{actor-isolated instance method 'synchronous()' can only be referenced on 'self'}}
    _ = await otherActor.asynchronous()
    _ = otherActor.text[0] // expected-error{{actor-isolated property 'text' can only be referenced on 'self'}}

    // Global data is okay if it is immutable.
    _ = immutableGlobal
    _ = mutableGlobal // expected-warning{{reference to var 'mutableGlobal' is not concurrency-safe because it involves shared mutable state}}

    // Global functions are not actually safe, but we allow them for now.
    globalFunc()

    // Class methods are okay.
    Self.synchronousClass()
    Self.synchronousStatic()

    // Closures.
    let localConstant = 17
    var localVar = 17 // expected-note 3{{var declared here}}

    // Non-escaping closures are okay.
    acceptClosure {
      _ = text[0]
      _ = self.synchronous()
      _ = localVar
      _ = localConstant
    }

    // Escaping closures might run concurrently.
    acceptEscapingClosure {
      _ = self.text[0] // expected-error{{actor-isolated property 'text' is unsafe to reference in code that may execute concurrently}}
      _ = self.synchronous() // expected-error{{actor-isolated instance method 'synchronous()' is unsafe to reference in code that may execute concurrently}}
      _ = localVar // expected-warning{{local var 'localVar' is unsafe to reference in code that may execute concurrently}}
      _ = localConstant
    }

    // Local functions might run concurrently.
    func localFn1() {
      _ = self.text[0] // expected-error{{actor-isolated property 'text' is unsafe to reference in code that may execute concurrently}}
      _ = self.synchronous() // expected-error{{actor-isolated instance method 'synchronous()' is unsafe to reference in code that may execute concurrently}}
      _ = localVar // expected-warning{{local var 'localVar' is unsafe to reference in code that may execute concurrently}}
      _ = localConstant
    }

    func localFn2() {
      acceptClosure {
        _ = text[0]  // expected-error{{actor-isolated property 'text' is unsafe to reference in code that may execute concurrently}}
        _ = self.synchronous() // expected-error{{actor-isolated instance method 'synchronous()' is unsafe to reference in code that may execute concurrently}}
        _ = localVar // expected-warning{{local var 'localVar' is unsafe to reference in code that may execute concurrently}}
        _ = localConstant
      }
    }

    localVar = 0
  }
}

// ----------------------------------------------------------------------
// Non-actor code isolation restrictions
// ----------------------------------------------------------------------
func testGlobalRestrictions(actor: MyActor) async {
  let _ = MyActor()

  // Asynchronous functions and immutable state are permitted.
  _ = await actor.asynchronous()
  _ = actor.immutable

  // Synchronous operations and mutable state references are not.
  _ = actor.synchronous() // expected-error{{actor-isolated instance method 'synchronous()' can only be referenced inside the actor}}
  _ = actor.text[0] // expected-error{{actor-isolated property 'text' can only be referenced inside the actor}}
  _ = actor[0] // expected-error{{actor-isolated subscript 'subscript(_:)' can only be referenced inside the actor}}

  // Operations on non-instances are permitted.
  MyActor.synchronousStatic()
  MyActor.synchronousClass()
}
