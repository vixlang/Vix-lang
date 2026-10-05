# Methods

Vix methods are declared at top level with a receiver. There is no impl block syntax.

## Declaration

A method has an implicit first parameter named self:

```vix
fn (Point) copy(): Point {
    return self
}

fn (&Point) length(): i32 {
    return self.x + self.y
}

fn (&mut Point) move_by(dx: i32, dy: i32) {
    self.x = self.x + dx
    self.y = self.y + dy
}
```

The receiver forms are:

- `(Point)`: owned receiver. Calling the method moves the receiver into self.
- `(&Point)`: shared receiver. The call creates a shared borrow.
- `(&mut Point)`: mutable receiver. The receiver must be mutable and the call creates a mutable borrow.

The declaration does not write self in the parameter list. The compiler adds it to the method scope and to the lowered function signature.

## Calls and Resolution

A call such as `point.length()` is resolved using the static receiver type and method name. The compiler lowers it to an ordinary call with the receiver as the first argument:

```text
point.length()       -> Point::length(point)
point.move_by(1, 2)  -> Point::move_by(point, 1, 2)
```

The language method table stores receiver type, receiver mode, generic parameters, parameters, return type, and declaration span. Unknown methods, invalid receiver modes, duplicate declarations, and argument mismatches are reported during semantic/type checking.

## Ownership

Owned methods consume non-copy receivers. Shared methods do not consume the receiver. Mutable methods require an assignable mutable receiver and cannot overlap an incompatible borrow.

## Backend ABI

After receiver adjustment, all backends consume the same ordinary-call representation. The receiver is parameter zero. The source-level name `Type::method` is converted to the backend symbol using the common symbol mangling helper.

## Analyzer

The analyzer indexes methods by their qualified internal name while displaying the receiver form in signatures. Method declarations and calls participate in symbols, hover, definition, references, rename, completion, and incremental declaration updates.

## Migration

Replace:

```vix
impl Point {
    fn distance(self: &Point): i32 { ... }
}
```

with:

```vix
fn (&Point) distance(): i32 { ... }
```

For mutable methods, use `fn (&mut Point) method(...)`; do not write `mut self: &Point`.
