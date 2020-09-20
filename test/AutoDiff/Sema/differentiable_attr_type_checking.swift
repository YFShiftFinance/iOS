// RUN: %target-swift-frontend-typecheck -verify -disable-availability-checking %s
// RUN: %target-swift-frontend-typecheck -enable-testing -verify -disable-availability-checking %s

import _Differentiation

// Dummy `Differentiable`-conforming type.
public struct DummyTangentVector: Differentiable & AdditiveArithmetic {
  public static var zero: Self { Self() }
  public static func + (_: Self, _: Self) -> Self { Self() }
  public static func - (_: Self, _: Self) -> Self { Self() }
  public typealias TangentVector = Self
}

@differentiable // expected-error {{'@differentiable' attribute cannot be applied to this declaration}}
let globalConst: Float = 1

@differentiable // expected-error {{'@differentiable' attribute cannot be applied to this declaration}}
var globalVar: Float = 1

func testLocalVariables() {
  // expected-error @+1 {{'_' has no parameters to differentiate with respect to}}
  @differentiable
  var getter: Float {
    return 1
  }

  // expected-error @+1 {{'_' has no parameters to differentiate with respect to}}
  @differentiable
  var getterSetter: Float {
    get { return 1 }
    set {}
  }
}

@differentiable // expected-error {{'@differentiable' attribute cannot be applied to this declaration}}
protocol P {}

@differentiable() // ok!
func no_jvp_or_vjp(_ x: Float) -> Float {
  return x * x
}

// Test duplicate `@differentiable` attributes.

@differentiable // expected-error {{duplicate '@differentiable' attribute with same parameters}}
@differentiable // expected-note {{other attribute declared here}}
func dupe_attributes(arg: Float) -> Float { return arg }

@differentiable(wrt: arg1)
@differentiable(wrt: arg2) // expected-error {{duplicate '@differentiable' attribute with same parameters}}
@differentiable(wrt: arg2) // expected-note {{other attribute declared here}}
func dupe_attributes(arg1: Float, arg2: Float) -> Float { return arg1 }

struct ComputedPropertyDupeAttributes<T: Differentiable>: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(along _: TangentVector) {}

  var value: T

  @differentiable // expected-note {{other attribute declared here}}
  var computed1: T {
    @differentiable // expected-error {{duplicate '@differentiable' attribute with same parameters}}
    get { value }
    set { value = newValue }
  }

  // TODO(TF-482): Remove diagnostics when `@differentiable` attributes are
  // also uniqued based on generic requirements.
  @differentiable(where T == Float) // expected-error {{duplicate '@differentiable' attribute with same parameters}}
  @differentiable(where T == Double) // expected-note {{other attribute declared here}}
  var computed2: T {
    get { value }
    set { value = newValue }
  }
}

// Test TF-568.
protocol WrtOnlySelfProtocol: Differentiable {
  @differentiable
  var computedProperty: Float { get }

  @differentiable
  func method() -> Float
}

class Class: Differentiable {
  typealias TangentVector = DummyTangentVector
  func move(along _: TangentVector) {}
}
@differentiable(wrt: x)
func invalidDiffWrtClass(_ x: Class) -> Class {
  return x
}

protocol Proto {}
// expected-error @+1 {{can only differentiate functions with results that conform to 'Differentiable', but 'Proto' does not conform to 'Differentiable'}}
@differentiable(wrt: x)
func invalidDiffWrtExistential(_ x: Proto) -> Proto {
  return x
}

// expected-error @+1 {{can only differentiate with respect to parameters that conform to 'Differentiable', but '@differentiable (Float) -> Float' does not conform to 'Differentiable'}}
@differentiable(wrt: fn)
func invalidDiffWrtFunction(_ fn: @differentiable(Float) -> Float) -> Float {
  return fn(.pi)
}

// expected-error @+1 {{'invalidDiffNoParams()' has no parameters to differentiate with respect to}}
@differentiable
func invalidDiffNoParams() -> Float {
  return 1
}

// expected-error @+1 {{cannot differentiate void function 'invalidDiffVoidResult(x:)'}}
@differentiable
func invalidDiffVoidResult(x: Float) {}

// Test static methods.
struct StaticMethod {
  // expected-error @+1 {{'invalidDiffNoParams()' has no parameters to differentiate with respect to}}
  @differentiable
  static func invalidDiffNoParams() -> Float {
    return 1
  }

  // expected-error @+1 {{cannot differentiate void function 'invalidDiffVoidResult(x:)'}}
  @differentiable
  static func invalidDiffVoidResult(x: Float) {}
}

// Test instance methods.
struct InstanceMethod {
  // expected-error @+1 {{'invalidDiffNoParams()' has no parameters to differentiate with respect to}}
  @differentiable
  func invalidDiffNoParams() -> Float {
    return 1
  }

  // expected-error @+1 {{cannot differentiate void function 'invalidDiffVoidResult(x:)'}}
  @differentiable
  func invalidDiffVoidResult(x: Float) {}
}

// Test instance methods for a `Differentiable` type.
struct DifferentiableInstanceMethod: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(along _: TangentVector) {}

  @differentiable // ok
  func noParams() -> Float {
    return 1
  }
}

// Test subscript methods.
struct SubscriptMethod: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(along _: TangentVector) {}

  @differentiable // ok
  subscript(implicitGetter x: Float) -> Float {
    return x
  }

  @differentiable // ok
  subscript(implicitGetterSetter x: Float) -> Float {
    get { return x }
    set {}
  }

  subscript(explicit x: Float) -> Float {
    @differentiable // ok
    get { return x }
    // expected-error @+1 {{'@differentiable' attribute cannot be applied to this declaration}}
    @differentiable
    set {}
  }

  subscript(x: Float, y: Float) -> Float {
    @differentiable // ok
    get { return x + y }
    // expected-error @+1 {{'@differentiable' attribute cannot be applied to this declaration}}
    @differentiable
    set {}
  }
}

// expected-error @+3 {{type 'Scalar' constrained to non-protocol, non-class type 'Float'}}
// expected-error @+2 {{no differentiation parameters could be inferred; must differentiate with respect to at least one parameter conforming to 'Differentiable'}}
// expected-note @+1 {{use 'Scalar == Float' to require 'Scalar' to be 'Float'}}
@differentiable(where Scalar: Float)
func invalidRequirementConformance<Scalar>(x: Scalar) -> Scalar {
  return x
}

@differentiable(where T: AnyObject)
func invalidAnyObjectRequirement<T: Differentiable>(x: T) -> T {
  return x
}

// expected-error @+1 {{'@differentiable' attribute does not yet support layout requirements}}
@differentiable(where Scalar: _Trivial)
func invalidRequirementLayout<Scalar>(x: Scalar) -> Scalar {
  return x
}

// expected-error @+1 {{no differentiation parameters could be inferred; must differentiate with respect to at least one parameter conforming to 'Differentiable'}}
@differentiable
func missingConformance<T>(_ x: T) -> T {
  return x
}

protocol ProtocolRequirements: Differentiable {
  // expected-note @+2 {{protocol requires initializer 'init(x:y:)' with type '(x: Float, y: Float)'}}
  @differentiable
  init(x: Float, y: Float)

  // expected-note @+2 {{protocol requires initializer 'init(x:y:)' with type '(x: Float, y: Int)'}}
  @differentiable(wrt: x)
  init(x: Float, y: Int)

  // expected-note @+2 {{protocol requires function 'amb(x:y:)' with type '(Float, Float) -> Float';}}
  @differentiable
  func amb(x: Float, y: Float) -> Float

  // expected-note @+2 {{protocol requires function 'amb(x:y:)' with type '(Float, Int) -> Float';}}
  @differentiable(wrt: x)
  func amb(x: Float, y: Int) -> Float

  // expected-note @+3 {{protocol requires function 'f1'}}
  // expected-note @+2 {{overridden declaration is here}}
  @differentiable(wrt: (self, x))
  func f1(_ x: Float) -> Float

  // expected-note @+2 {{protocol requires function 'f2'}}
  @differentiable(wrt: (self, x, y))
  func f2(_ x: Float, _ y: Float) -> Float
}

protocol ProtocolRequirementsRefined: ProtocolRequirements {
  // expected-error @+1 {{overriding declaration is missing attribute '@differentiable'}}
  func f1(_ x: Float) -> Float
}

// Test missing `@differentiable` attribute for internal protocol witnesses.
// No errors expected; internal `@differentiable` attributes are created.

struct InternalDiffAttrConformance: ProtocolRequirements {
  typealias TangentVector = DummyTangentVector
  mutating func move(along _: TangentVector) {}

  var x: Float
  var y: Float

  init(x: Float, y: Float) {
    self.x = x
    self.y = y
  }

  init(x: Float, y: Int) {
    self.x = x
    self.y = Float(y)
  }

  func amb(x: Float, y: Float) -> Float {
    return x
  }

  func amb(x: Float, y: Int) -> Float {
    return x
  }

  func f1(_ x: Float) -> Float {
    return x
  }

  @differentiable(wrt: (self, x))
  func f2(_ x: Float, _ y: Float) -> Float {
    return x + y
  }
}

// Test missing `@differentiable` attribute for public protocol witnesses. Errors expected.

// expected-error @+1 {{does not conform to protocol 'ProtocolRequirements'}}
public struct PublicDiffAttrConformance: ProtocolRequirements {
  public typealias TangentVector = DummyTangentVector
  public mutating func move(along _: TangentVector) {}

  var x: Float
  var y: Float

  // FIXME(TF-284): Fix unexpected diagnostic.
  // expected-note @+2 {{candidate is missing attribute '@differentiable'}} {{10-10=@differentiable }}
  // expected-note @+1 {{candidate has non-matching type '(x: Float, y: Float)'}}
  public init(x: Float, y: Float) {
    self.x = x
    self.y = y
  }

  // FIXME(TF-284): Fix unexpected diagnostic.
  // expected-note @+2 {{candidate is missing attribute '@differentiable'}} {{10-10=@differentiable }}
  // expected-note @+1 {{candidate has non-matching type '(x: Float, y: Int)'}}
  public init(x: Float, y: Int) {
    self.x = x
    self.y = Float(y)
  }

  // expected-note @+2 {{candidate is missing attribute '@differentiable'}} {{10-10=@differentiable }}
  // expected-note @+1 {{candidate has non-matching type '(Float, Float) -> Float'}}
  public func amb(x: Float, y: Float) -> Float {
    return x
  }

  // expected-note @+2 {{candidate is missing attribute '@differentiable(wrt: x)'}} {{10-10=@differentiable(wrt: x) }}
  // expected-note @+1 {{candidate has non-matching type '(Float, Int) -> Float'}}
  public func amb(x: Float, y: Int) -> Float {
    return x
  }

  // expected-note @+1 {{candidate is missing attribute '@differentiable'}}
  public func f1(_ x: Float) -> Float {
    return x
  }

  // expected-note @+2 {{candidate is missing attribute '@differentiable'}}
  @differentiable(wrt: (self, x))
  public func f2(_ x: Float, _ y: Float) -> Float {
    return x + y
  }
}

protocol ProtocolRequirementsWithDefault_NoConformingTypes {
  @differentiable
  func f1(_ x: Float) -> Float
}
extension ProtocolRequirementsWithDefault_NoConformingTypes {
  // TODO(TF-650): It would be nice to diagnose protocol default implementation
  // with missing `@differentiable` attribute.
  func f1(_ x: Float) -> Float { x }
}

protocol ProtocolRequirementsWithDefault {
  @differentiable
  func f1(_ x: Float) -> Float
}
extension ProtocolRequirementsWithDefault {
  func f1(_ x: Float) -> Float { x }
}
struct DiffAttrConformanceErrors2: ProtocolRequirementsWithDefault {
  func f1(_ x: Float) -> Float { x }
}

protocol NotRefiningDiffable {
  @differentiable(wrt: x)
  func a(_ x: Float) -> Float
}

struct CertainlyNotDiffableWrtSelf: NotRefiningDiffable {
  func a(_ x: Float) -> Float { return x * 5.0 }
}

protocol TF285: Differentiable {
  @differentiable(wrt: (x, y))
  @differentiable(wrt: x)
  func foo(x: Float, y: Float) -> Float
}

struct TF285MissingOneDiffAttr: TF285 {
  typealias TangentVector = DummyTangentVector
  mutating func move(along _: TangentVector) {}

  // Requirement is missing the required `@differentiable(wrt: (x, y))` attribute.
  // Since `TF285MissingOneDiffAttr.foo` is internal, the attribute is implicitly created.
  @differentiable(wrt: x)
  func foo(x: Float, y: Float) -> Float {
    return x
  }
}

// TF-521: Test invalid `@differentiable` attribute due to invalid
// `Differentiable` conformance (`TangentVector` does not conform to
// `AdditiveArithmetic`).
struct TF_521<T: FloatingPoint> {
  var real: T
  var imaginary: T

  // expected-error @+1 {{can only differentiate functions with results that conform to 'Differentiable', but 'TF_521<T>' does not conform to 'Differentiable'}}
  @differentiable(where T: Differentiable, T == T.TangentVector)
  init(real: T = 0, imaginary: T = 0) {
    self.real = real
    self.imaginary = imaginary
  }
}
// expected-error @+1 {{type 'TF_521<T>' does not conform to protocol 'Differentiable'}}
extension TF_521: Differentiable where T: Differentiable {
  // expected-note @+1 {{possibly intended match 'TF_521<T>.TangentVector' does not conform to 'AdditiveArithmetic'}}
  typealias TangentVector = TF_521
}
// expected-error @+1 {{result type 'TF_521<Float>' does not conform to 'Differentiable', but the enclosing function type is '@differentiable'}}
let _: @differentiable(Float, Float) -> TF_521<Float> = { r, i in
  TF_521(real: r, imaginary: i)
}

// TF-296: Infer `@differentiable` wrt parameters to be to all parameters that conform to `Differentiable`.

@differentiable
func infer1(_ a: Float, _ b: Int) -> Float {
  return a + Float(b)
}

@differentiable
func infer2(_ fn: @differentiable(Float) -> Float, x: Float) -> Float {
  return fn(x)
}

struct DiffableStruct: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(along _: TangentVector) {}

  var a: Float

  @differentiable
  func fn(_ b: Float, _ c: Int) -> Float {
    return a + b + Float(c)
  }
}

struct NonDiffableStruct {
  var a: Float

  @differentiable
  func fn(_ b: Float) -> Float {
    return a + b
  }
}

// Index based 'wrt:'

struct NumberWrtStruct: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(along _: TangentVector) {}

  var a, b: Float

  @differentiable(wrt: 0) // ok
  @differentiable(wrt: 1) // ok
  func foo1(_ x: Float, _ y: Float) -> Float {
    return a*x + b*y
  }

  @differentiable(wrt: -1) // expected-error {{expected a parameter, which can be a function parameter name, parameter index, or 'self'}}
  @differentiable(wrt: (1, x)) // expected-error {{parameters must be specified in original order}}
  func foo2(_ x: Float, _ y: Float) -> Float {
    return a*x + b*y
  }

  @differentiable(wrt: (x, 1)) // ok
  @differentiable(wrt: (0)) // ok
  static func staticFoo1(_ x: Float, _ y: Float) -> Float {
    return x + y
  }

  @differentiable(wrt: (1, 1)) // expected-error {{parameters must be specified in original order}}
  @differentiable(wrt: (2)) // expected-error {{parameter index is larger than total number of parameters}}
  static func staticFoo2(_ x: Float, _ y: Float) -> Float {
    return x + y
  }
}

@differentiable(wrt: y) // ok
func two1(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(wrt: (x, y)) // ok
func two2(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(wrt: (0, y)) // ok
func two3(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(wrt: (x, 1)) // ok
func two4(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(wrt: (0, 1)) // ok
func two5(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(wrt: 2) // expected-error {{parameter index is larger than total number of parameters}}
func two6(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(wrt: (1, 0)) // expected-error {{parameters must be specified in original order}}
func two7(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(wrt: (1, x)) // expected-error {{parameters must be specified in original order}}
func two8(x: Float, y: Float) -> Float {
  return x + y
}

@differentiable(wrt: (y, 0)) // expected-error {{parameters must be specified in original order}}
func two9(x: Float, y: Float) -> Float {
  return x + y
}

// Inout 'wrt:' arguments.

@differentiable(wrt: y)
func inout1(x: Float, y: inout Float) -> Void {
  let _ = x + y
}
// expected-error @+1 {{cannot differentiate functions with both an 'inout' parameter and a result}}
@differentiable(wrt: y)
func inout2(x: Float, y: inout Float) -> Float {
  let _ = x + y
}

// Test refining protocol requirements with `@differentiable` attribute.

public protocol Distribution {
  associatedtype Value
  func logProbability(of value: Value) -> Float
}

public protocol DifferentiableDistribution: Differentiable, Distribution {
  // expected-note @+2 {{overridden declaration is here}}
  @differentiable(wrt: self)
  func logProbability(of value: Value) -> Float
}

// Adding a more general `@differentiable` attribute.
public protocol DoubleDifferentiableDistribution: DifferentiableDistribution
  where Value: Differentiable {
  // expected-error @+1 {{overriding declaration is missing attribute '@differentiable(wrt: self)'}} {{3-3=@differentiable(wrt: self) }}
  func logProbability(of value: Value) -> Float
}

// Test failure to satisfy protocol requirement's `@differentiable` attribute.

public protocol HasRequirement {
  @differentiable
  // expected-note @+1 {{protocol requires function 'requirement' with type '<T> (T, T) -> T'; do you want to add a stub?}}
  func requirement<T: Differentiable>(_ x: T, _ y: T) -> T
}

// expected-error @+1 {{type 'AttemptsToSatisfyRequirement' does not conform to protocol 'HasRequirement'}}
public struct AttemptsToSatisfyRequirement: HasRequirement {
  // This `@differentiable` attribute does not satisfy the requirement because
  // it is mroe constrained than the requirement's `@differentiable` attribute.
  @differentiable(where T: CustomStringConvertible)
  // expected-note @+1 {{candidate is missing attribute '@differentiable(wrt: (x, y))'}}
  public func requirement<T: Differentiable>(_ x: T, _ y: T) -> T { x }
}

// Test protocol requirement `@differentiable` attribute unsupported features.

protocol ProtocolRequirementUnsupported: Differentiable {
  associatedtype Scalar

  // expected-error @+1 {{'@differentiable' attribute on protocol requirement cannot specify 'where' clause}}
  @differentiable(where Scalar: Differentiable)
  func unsupportedWhereClause(value: Scalar) -> Float
}
extension ProtocolRequirementUnsupported {
  func dfoo(_ x: Float) -> (Float, (Float) -> Float) {
    (x, { $0 })
  }
}

// Classes.

class Super: Differentiable {
  typealias TangentVector = DummyTangentVector
  func move(along _: TangentVector) {}

  var base: Float

  // expected-error @+1 {{'@differentiable' attribute cannot be declared on 'init' in a non-final class; consider making 'Super' final}}
  @differentiable
  init(base: Float) {
    self.base = base
  }

  // NOTE(TF-1040): `@differentiable` attribute on class methods currently
  // does two orthogonal things:
  // - Requests derivative generation for the class method.
  // - Adds JVP/VJP vtable entries for the class method.
  // There's currently no way using `@differentiable` to do only one of the
  // above.
  @differentiable
  func testClassMethod(_ x: Float) -> Float { x }

  @differentiable
  final func testFinalMethod(_ x: Float) -> Float { x }

  @differentiable
  static func testStaticMethod(_ x: Float) -> Float { x }

  @differentiable(wrt: (self, x))
  @differentiable(wrt: x)
  // expected-note @+1 2 {{overridden declaration is here}}
  func testMissingAttributes(_ x: Float) -> Float { x }

  @differentiable(wrt: x)
  func testSuperclassDerivatives(_ x: Float) -> Float { x }

  // Test duplicate attributes with different derivative generic signatures.
  // expected-error @+1 {{duplicate '@differentiable' attribute with same parameters}}
  @differentiable(wrt: x where T: Differentiable)
  // expected-note @+1 {{other attribute declared here}}
  @differentiable(wrt: x)
  func instanceMethod<T>(_ x: Float, y: T) -> Float { x }

  // expected-error @+1 {{'@differentiable' attribute cannot be declared on class members returning 'Self'}}
  @differentiable
  func dynamicSelfResult() -> Self { self }

  // expected-error @+1 {{'@differentiable' attribute cannot be declared on class members returning 'Self'}}
  @differentiable
  var testDynamicSelfProperty: Self { self }

  // TODO(TF-632): Fix "'TangentVector' is not a member type of 'Self'" diagnostic.
  // The underlying error should appear instead:
  // "covariant 'Self' can only appear at the top level of method result type".
  // expected-error @+1 2 {{'TangentVector' is not a member type of 'Self'}}
  func vjpDynamicSelfResult() -> (Self, (Self.TangentVector) -> Self.TangentVector) {
    return (self, { $0 })
  }
}

class Sub: Super {
  // expected-error @+2 {{overriding declaration is missing attribute '@differentiable(wrt: x)'}}
  // expected-error @+1 {{overriding declaration is missing attribute '@differentiable'}}
  override func testMissingAttributes(_ x: Float) -> Float { x }
}

final class FinalClass: Differentiable {
  typealias TangentVector = DummyTangentVector
  func move(along _: TangentVector) {}

  var base: Float

  @differentiable
  init(base: Float) {
    self.base = base
  }
}

// Test `inout` parameters.

@differentiable(wrt: y)
func inoutVoid(x: Float, y: inout Float) {}

// expected-error @+1 {{cannot differentiate functions with both an 'inout' parameter and a result}}
@differentiable
func multipleSemanticResults(_ x: inout Float) -> Float { x }

// expected-error @+1 {{cannot differentiate functions with both an 'inout' parameter and a result}}
@differentiable(wrt: y)
func swap(x: inout Float, y: inout Float) {}

struct InoutParameters: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(along _: TangentVector) {}
}

extension InoutParameters {
  @differentiable
  static func staticMethod(_ lhs: inout Self, rhs: Self) {}

  // expected-error @+1 {{cannot differentiate functions with both an 'inout' parameter and a result}}
  @differentiable
  static func multipleSemanticResults(_ lhs: inout Self, rhs: Self) -> Self {}
}

extension InoutParameters {
  @differentiable
  mutating func mutatingMethod(_ other: Self) {}

  // expected-error @+1 {{cannot differentiate functions with both an 'inout' parameter and a result}}
  @differentiable
  mutating func mutatingMethod(_ other: Self) -> Self {}
}

// Test accessors: `set`, `_read`, `_modify`.

struct Accessors: Differentiable {
  typealias TangentVector = DummyTangentVector
  mutating func move(along _: TangentVector) {}

  var stored: Float
  var computed: Float {
    // expected-error @+1 {{'@differentiable' attribute cannot be applied to this declaration}}
    @differentiable
    set { stored = newValue }

    // `_read` is a coroutine: `(Self) -> () -> ()`.
    // expected-error @+1 {{'@differentiable' attribute cannot be applied to this declaration}}
    @differentiable
    _read { yield stored }

    // `_modify` is a coroutine: `(inout Self) -> () -> ()`.
    // expected-error @+1 {{'@differentiable' attribute cannot be applied to this declaration}}
    @differentiable
    _modify { yield &stored }
  }
}

// expected-error @+1 {{cannot differentiate functions returning opaque result types}}
@differentiable
func opaqueResult(_ x: Float) -> some Differentiable { x }
