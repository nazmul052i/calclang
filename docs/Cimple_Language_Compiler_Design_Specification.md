# Cimple Language + Compiler Design Specification

## 1. Purpose

**Cimple** is a small compiled programming language designed for learning how computers work.

It should be:

- Simple like Python for beginners.
- Explicit like C for memory and machine understanding.
- Safer than C where possible.
- Compiled, not interpreted.
- Small enough that a student can build the full compiler.
- Useful for teaching variables, control flow, functions, memory, pointers, stack, heap, CPU execution, and eventually operating system concepts.

Cimple is not intended to replace C, Rust, Python, or C++. It is a teaching language whose main goal is to show how source code becomes machine behavior.

---

## 2. Design Philosophy

Cimple follows these principles:

1. **Readable first**  
   Code should be easy to read for beginners.

2. **Explicit types**  
   Variables should have clear types so the compiler can teach the user what is happening.

3. **Small grammar**  
   The first version should have a small number of language rules.

4. **Compiled by default**  
   Cimple programs are compiled into another lower-level form, initially C code, and later assembly or LLVM IR.

5. **Memory should be visible**  
   Cimple should teach stack, heap, addresses, pointers, and arrays.

6. **Safety should be introduced gradually**  
   Start with simple pointer rules, then add safer ownership-style rules later.

7. **Friendly errors**  
   Compiler errors should explain what went wrong in beginner-friendly language.

---

## 3. Example Program

```cimple
func main() -> int {
    let x: int = 10
    let y: int = 20
    let result: int = x + y

    print result

    return 0
}
```

Equivalent C output:

```c
#include <stdio.h>

int main() {
    int x = 10;
    int y = 20;
    int result = x + y;

    printf("%d\n", result);

    return 0;
}
```

---

## 4. File Extension

Cimple source files use:

```text
.cimple
```

Example:

```text
hello.cimple
```

Compiled output may produce:

```text
hello.c
hello.exe
hello.out
hello.asm
hello.o
```

---

## 5. Program Structure

Every executable Cimple program must contain a `main` function.

```cimple
func main() -> int {
    return 0
}
```

Rules:

- Program execution starts at `main`.
- `main` returns an `int`.
- Returning `0` means success.
- Non-zero return values mean error or abnormal termination.

---

## 6. Comments

Cimple supports single-line comments:

```cimple
// This is a comment
let x: int = 10 // This is also a comment
```

Block comments may be added later:

```cimple
/*
This is a block comment.
*/
```

Version 1 only requires `//` comments.

---

## 7. Keywords

Reserved keywords:

```text
func
let
mut
const
if
else
while
for
return
true
false
print
int
float
bool
char
string
ptr
addr
deref
sizeof
struct
new
delete
null
```

Early compiler versions do not need to implement all of these immediately. Reserved keywords are protected so they cannot be used as variable names.

Invalid:

```cimple
let if: int = 10
```

---

## 8. Identifiers

Identifiers name variables, functions, and user-defined types.

Rules:

- Must start with a letter or underscore.
- May contain letters, digits, and underscores.
- Cannot be a keyword.
- Case-sensitive.

Valid:

```cimple
x
temperature
feed_rate
_pressure
ValvePosition1
```

Invalid:

```cimple
1value
feed-rate
let
main value
```

Naming recommendation:

```text
snake_case for variables and functions
PascalCase for structs later
```

---

## 9. Primitive Types

Initial Cimple types:

| Cimple Type | Meaning | Suggested C Target |
|---|---|---|
| `int` | Signed integer | `int` |
| `float` | Floating-point number | `double` |
| `bool` | Boolean true/false | `int` or `bool` |
| `char` | Single character | `char` |
| `string` | Text string | `char*` initially |

Examples:

```cimple
let age: int = 10
let temperature: float = 98.6
let running: bool = true
let grade: char = 'A'
let name: string = "Nazmul"
```

---

## 10. Variable Declarations

Immutable variable:

```cimple
let x: int = 10
```

Mutable variable:

```cimple
mut count: int = 0
count = count + 1
```

Constant:

```cimple
const PI: float = 3.14159
```

Rules:

- `let` creates an immutable variable.
- `mut` creates a mutable variable.
- `const` creates a compile-time constant.
- Variables must have a type in Version 1.
- Variables must be initialized when declared.

Invalid:

```cimple
let x: int
x = 10
```

This may be allowed in a later version, but the first version should require initialization to keep things simple.

---

## 11. Assignment

Only mutable variables can be reassigned.

Valid:

```cimple
mut x: int = 10
x = 20
```

Invalid:

```cimple
let x: int = 10
x = 20
```

Compiler error:

```text
Cannot assign to immutable variable 'x'.
Use 'mut x' if the value should change.
```

---

## 12. Literals

Integer literals:

```cimple
10
0
-5
```

Float literals:

```cimple
3.14
0.5
-12.75
```

Boolean literals:

```cimple
true
false
```

Character literals:

```cimple
'A'
'7'
'\n'
```

String literals:

```cimple
"hello"
"temperature high"
```

---

## 13. Operators

### Arithmetic Operators

| Operator | Meaning |
|---|---|
| `+` | Add |
| `-` | Subtract |
| `*` | Multiply |
| `/` | Divide |
| `%` | Remainder |

Example:

```cimple
let result: int = 10 + 5 * 2
```

Operator precedence:

1. Parentheses
2. Unary `-`
3. `*`, `/`, `%`
4. `+`, `-`
5. Comparison operators
6. Logical `and`
7. Logical `or`

### Comparison Operators

| Operator | Meaning |
|---|---|
| `==` | Equal |
| `!=` | Not equal |
| `<` | Less than |
| `<=` | Less than or equal |
| `>` | Greater than |
| `>=` | Greater than or equal |

Example:

```cimple
if temperature > 100.0 {
    print "High temperature"
}
```

### Logical Operators

Cimple should use readable logical operators:

```cimple
and
or
not
```

Example:

```cimple
if pressure > 100.0 and temperature > 80.0 {
    print "Warning"
}
```

Equivalent C:

```c
if (pressure > 100.0 && temperature > 80.0) {
    printf("Warning\n");
}
```

---

## 14. Blocks and Scope

A block is surrounded by braces:

```cimple
{
    let x: int = 10
    print x
}
```

Scope rules:

- Variables exist only inside the block where they are declared.
- Inner blocks can read outer variables.
- Inner blocks may shadow outer variables, but the compiler should warn beginners.

Example:

```cimple
let x: int = 10

if true {
    let y: int = 20
    print x
    print y
}

// y is not available here
```

Invalid:

```cimple
if true {
    let y: int = 20
}

print y
```

Compiler error:

```text
Variable 'y' does not exist in this scope.
It was declared inside an if block.
```

---

## 15. If / Else

Syntax:

```cimple
if condition {
    // statements
} else {
    // statements
}
```

Example:

```cimple
if level > 80.0 {
    print "High level"
} else {
    print "Normal level"
}
```

Rules:

- Condition must be `bool`.
- Braces are required.
- Parentheses around condition are not required.

Invalid:

```cimple
if 10 {
    print "bad"
}
```

Compiler error:

```text
The condition of an if statement must be bool.
Found int instead.
```

---

## 16. While Loop

Syntax:

```cimple
while condition {
    // statements
}
```

Example:

```cimple
mut i: int = 0

while i < 5 {
    print i
    i = i + 1
}
```

Rules:

- Condition must be `bool`.
- Braces are required.

---

## 17. For Loop

For loops may be added after while loops.

Suggested syntax:

```cimple
for i in 0..10 {
    print i
}
```

This means:

```cimple
mut i: int = 0
while i < 10 {
    print i
    i = i + 1
}
```

Version 1 can skip `for` loops and only support `while`.

---

## 18. Functions

Syntax:

```cimple
func name(parameter: type, parameter: type) -> return_type {
    return value
}
```

Example:

```cimple
func add(a: int, b: int) -> int {
    return a + b
}

func main() -> int {
    let result: int = add(10, 20)
    print result
    return 0
}
```

Rules:

- Function parameters are immutable by default.
- Function return type is required.
- A non-void function must return a value.
- Functions may call functions declared later in the file.

Void-like functions:

```cimple
func say_hello() -> void {
    print "Hello"
}
```

For Version 1, every function can return `int`, `float`, `bool`, `char`, or `string`. `void` can be added in Version 2.

---

## 19. Print Statement

Simple output:

```cimple
print x
print "Hello"
print x + y
```

Rules:

- `print` outputs a value followed by a newline.
- Supported initial print types: `int`, `float`, `bool`, `char`, `string`.

Possible C output:

```c
printf("%d\n", x);
printf("%f\n", y);
printf("%s\n", text);
```

---

## 20. Type Checking

Cimple is statically typed.

That means the compiler checks types before the program runs.

Example:

```cimple
let x: int = "hello"
```

Compiler error:

```text
Type mismatch for variable 'x'.
Expected int but found string.
```

Arithmetic rules:

| Expression | Result |
|---|---|
| `int + int` | `int` |
| `float + float` | `float` |
| `int + float` | Error in Version 1 |
| `float + int` | Error in Version 1 |

Keep Version 1 strict. Later, implicit numeric promotion may be added.

Recommended explicit conversion later:

```cimple
let x: int = 10
let y: float = to_float(x)
```

---

## 21. Memory Model

Cimple should teach memory clearly.

There are three major memory areas:

1. **Code memory**  
   Stores program instructions.

2. **Stack memory**  
   Stores local variables and function calls.

3. **Heap memory**  
   Stores dynamically allocated objects.

### Stack Example

```cimple
func main() -> int {
    let x: int = 10
    let y: int = 20
    return 0
}
```

Conceptually:

```text
Stack frame for main:
+----------------+
| x = 10         |
| y = 20         |
| return address |
+----------------+
```

### Address Example

```cimple
let x: int = 10
let p: ptr int = addr x
print deref p
```

Conceptually:

```text
x is stored somewhere in memory.
p stores the address of x.
deref p reads the value at that address.
```

---

## 22. Pointers

Pointers are used to teach memory addresses.

Pointer type syntax:

```cimple
ptr int
ptr float
ptr char
```

Address-of operator:

```cimple
addr x
```

Dereference operator:

```cimple
deref p
```

Example:

```cimple
func main() -> int {
    mut x: int = 10
    let p: ptr int = addr x

    print deref p

    return 0
}
```

Changing through pointer may be added later:

```cimple
set deref p = 50
```

Do not support pointer arithmetic in Version 1.

Invalid in Version 1:

```cimple
p = p + 1
```

Reason: pointer arithmetic is powerful but dangerous for beginners.

---

## 23. Null Pointers

Cimple may support `null` later.

Example:

```cimple
let p: ptr int = null
```

Version 1 should avoid null pointers if possible. It is better to introduce pointers using `addr` first.

Later safety rule:

- You cannot dereference a pointer unless the compiler knows it is not null.

---

## 24. Arrays

Arrays are fixed-size collections.

Suggested syntax:

```cimple
let numbers: array int 5 = [1, 2, 3, 4, 5]
```

Access:

```cimple
print numbers[0]
```

Rules:

- Index starts at 0.
- Array length is fixed.
- Index must be int.
- Runtime bounds checking should be enabled in beginner mode.

Version 1 can skip arrays. Version 2 should add them.

---

## 25. Structs

Structs group related data.

Suggested syntax:

```cimple
struct Sensor {
    temperature: float
    pressure: float
}

func main() -> int {
    let s: Sensor = Sensor { temperature: 80.0, pressure: 120.0 }
    print s.temperature
    return 0
}
```

C output:

```c
struct Sensor {
    double temperature;
    double pressure;
};
```

Structs are not required in Version 1. Add them after variables, functions, if, while, and basic pointers work.

---

## 26. Heap Allocation

Heap memory is explicit.

Possible future syntax:

```cimple
let p: ptr int = new int(10)
print deref p
delete p
```

Rules:

- `new` allocates memory on heap.
- `delete` frees memory.
- Compiler should warn if allocated memory is not freed.
- Compiler should warn if pointer is used after delete.

This should not be in Version 1. Add this only after stack pointers are understood.

---

## 27. Ownership Safety — Later Version

A later version of Cimple can add simple ownership rules inspired by Rust, but much simpler.

Possible rules:

1. Every heap allocation has one owner.
2. When the owner goes out of scope, memory is automatically freed.
3. You can borrow a reference temporarily.
4. You cannot use a value after ownership moves.

Example future syntax:

```cimple
own data: ptr int = new int(10)
borrow p: ptr int = addr data
```

This is advanced. Do not start here.

---

## 28. Grammar Overview

Simplified grammar:

```text
program        -> function_decl*

function_decl  -> "func" IDENT "(" params? ")" "->" type block
params         -> param ("," param)*
param          -> IDENT ":" type

type           -> "int"
                | "float"
                | "bool"
                | "char"
                | "string"
                | "ptr" type

block          -> "{" statement* "}"

statement      -> var_decl
                | assignment
                | if_stmt
                | while_stmt
                | return_stmt
                | print_stmt
                | expr_stmt

var_decl       -> ("let" | "mut" | "const") IDENT ":" type "=" expression
assignment     -> IDENT "=" expression
if_stmt        -> "if" expression block ("else" block)?
while_stmt     -> "while" expression block
return_stmt    -> "return" expression
print_stmt     -> "print" expression
expr_stmt      -> expression

expression     -> logical_or
logical_or     -> logical_and ("or" logical_and)*
logical_and    -> equality ("and" equality)*
equality       -> comparison (("==" | "!=") comparison)*
comparison     -> term ((">" | ">=" | "<" | "<=") term)*
term           -> factor (("+" | "-") factor)*
factor         -> unary (("*" | "/" | "%") unary)*
unary          -> ("-" | "not" | "addr" | "deref") unary
                | primary
primary        -> INT_LITERAL
                | FLOAT_LITERAL
                | STRING_LITERAL
                | CHAR_LITERAL
                | BOOL_LITERAL
                | IDENT
                | function_call
                | "(" expression ")"

function_call  -> IDENT "(" arguments? ")"
arguments      -> expression ("," expression)*
```

---

## 29. Tokens

The lexer converts source text into tokens.

Example source:

```cimple
let x: int = 10
```

Tokens:

```text
LET
IDENT(x)
COLON
TYPE_INT
EQUAL
INT_LITERAL(10)
NEWLINE
```

Token categories:

- Keywords
- Identifiers
- Integer literals
- Float literals
- String literals
- Character literals
- Operators
- Punctuation
- End of file

Important tokens:

```text
+ - * / %
= == != < <= > >=
( ) { } [ ]
: , . ->
```

Newlines can be treated as statement separators, or the compiler can ignore newlines and use grammar rules. For simplicity, Version 1 should allow newlines as separators but not require semicolons.

---

## 30. Semicolons

Cimple should not require semicolons.

Valid:

```cimple
let x: int = 10
print x
```

Invalid only if two statements are placed on the same line without a separator:

```cimple
let x: int = 10 print x
```

Optional semicolons may be supported later:

```cimple
let x: int = 10;
print x;
```

---

## 31. Compiler Architecture

The Cimple compiler should be built in stages.

```text
.cimple source code
        ↓
Lexer
        ↓
Tokens
        ↓
Parser
        ↓
AST
        ↓
Semantic Analyzer / Type Checker
        ↓
Intermediate Representation
        ↓
Code Generator
        ↓
C code / Assembly / LLVM IR
        ↓
Native executable
```

---

## 32. Compiler Stage 1 — Lexer

The lexer reads characters and produces tokens.

Input:

```cimple
let x: int = 10
```

Output:

```text
LET IDENT(x) COLON INT_TYPE EQUAL INT_LITERAL(10)
```

Responsibilities:

- Skip whitespace.
- Track line and column numbers.
- Recognize keywords.
- Recognize identifiers.
- Recognize numbers.
- Recognize strings.
- Recognize operators and punctuation.
- Report invalid characters.

Lexer error example:

```cimple
let x: int = @
```

Error:

```text
Unexpected character '@' at line 1, column 14.
```

---

## 33. Compiler Stage 2 — Parser

The parser reads tokens and produces an AST.

Input tokens:

```text
LET IDENT(x) COLON INT_TYPE EQUAL INT_LITERAL(10)
```

AST:

```text
VariableDeclaration
  name: x
  type: int
  mutable: false
  value:
    IntegerLiteral 10
```

Responsibilities:

- Enforce grammar.
- Build AST nodes.
- Report syntax errors.
- Recover from errors where possible.

Parser error example:

```cimple
let x int = 10
```

Error:

```text
Expected ':' after variable name 'x'.
Example: let x: int = 10
```

---

## 34. Compiler Stage 3 — AST

The AST represents the meaning structure of the program.

Main AST node types:

```text
Program
FunctionDecl
BlockStmt
VarDeclStmt
AssignStmt
IfStmt
WhileStmt
ReturnStmt
PrintStmt
BinaryExpr
UnaryExpr
LiteralExpr
VariableExpr
CallExpr
```

Example:

```cimple
let z: int = x + y
```

AST:

```text
VarDeclStmt
  name: z
  type: int
  value:
    BinaryExpr +
      left: VariableExpr x
      right: VariableExpr y
```

---

## 35. Compiler Stage 4 — Symbol Table

The symbol table stores names and their meanings.

For variables:

```text
name
kind: variable
scope
type
mutable
initialized
```

For functions:

```text
name
kind: function
parameter types
return type
```

Example:

```cimple
func add(a: int, b: int) -> int {
    return a + b
}
```

Symbol table:

```text
Function: add
  params: int, int
  return: int

Inside add scope:
  a: int immutable
  b: int immutable
```

---

## 36. Compiler Stage 5 — Type Checker

The type checker validates meaning.

Checks:

- Variable exists before use.
- Variable assignment respects mutability.
- Assignment type matches variable type.
- Function call argument count is correct.
- Function call argument types are correct.
- Return type matches function return type.
- If/while condition is bool.
- Binary operators are used with valid types.
- `addr` is used on addressable variables.
- `deref` is used only on pointer types.

Example error:

```cimple
let x: int = 10
let y: string = x
```

Error:

```text
Type mismatch.
Variable 'y' is declared as string but the assigned expression is int.
```

---

## 37. Compiler Stage 6 — Intermediate Representation

An intermediate representation, or IR, is a simpler internal form used before generating code.

For early versions, Cimple can skip a complex IR and generate C directly from the AST.

Later, use a simple three-address code IR:

```text
t1 = 10
t2 = 20
t3 = t1 + t2
print t3
return 0
```

This helps teach how compilers lower high-level code into simpler machine-like operations.

---

## 38. Compiler Stage 7 — Code Generation to C

The first compiler target should be C.

Reason:

- Easier than generating assembly directly.
- C compiler handles machine-specific details.
- User can inspect generated C code.
- Great teaching bridge between Cimple and real C.

Cimple:

```cimple
func main() -> int {
    let x: int = 10
    print x
    return 0
}
```

Generated C:

```c
#include <stdio.h>

int main() {
    int x = 10;
    printf("%d\n", x);
    return 0;
}
```

---

## 39. Code Generation Rules

Type mapping:

| Cimple | C |
|---|---|
| `int` | `int` |
| `float` | `double` |
| `bool` | `int` or `bool` |
| `char` | `char` |
| `string` | `char*` |
| `ptr int` | `int*` |
| `ptr float` | `double*` |

Expression mapping:

| Cimple | C |
|---|---|
| `and` | `&&` |
| `or` | `||` |
| `not` | `!` |
| `addr x` | `&x` |
| `deref p` | `*p` |

Print mapping:

| Type | C printf |
|---|---|
| `int` | `%d` |
| `float` | `%f` |
| `bool` | custom true/false output |
| `char` | `%c` |
| `string` | `%s` |

---

## 40. CLI Compiler Interface

Compiler executable name:

```text
cimplec
```

Basic usage:

```bash
cimplec hello.cimple
```

Output:

```text
hello.c
hello.exe or hello.out
```

Useful options:

```bash
cimplec hello.cimple --emit-c
cimplec hello.cimple --run
cimplec hello.cimple --tokens
cimplec hello.cimple --ast
cimplec hello.cimple --check
cimplec hello.cimple -o hello
```

Option meanings:

| Option | Meaning |
|---|---|
| `--emit-c` | Generate C but do not compile native executable |
| `--run` | Compile and run immediately |
| `--tokens` | Print lexer tokens |
| `--ast` | Print AST |
| `--check` | Type-check only |
| `-o` | Set output executable name |

---

## 41. Error Message Style

Cimple compiler errors should teach, not just complain.

Bad error:

```text
Syntax error.
```

Good error:

```text
Error at line 3, column 8:
Expected ':' after variable name.

  let x int = 10
        ^

Try:
  let x: int = 10
```

Error format:

```text
Error: short explanation
Location: file, line, column
Code snippet
Helpful suggestion
```

---

## 42. Beginner Mode

Cimple should support beginner-friendly checking by default.

Beginner mode should warn about:

- Variable shadowing.
- Unused variables.
- Unused functions.
- Possible division by zero when obvious.
- Missing return statements.
- Suspicious pointer usage.
- Unreachable code.

Example warning:

```text
Warning: variable 'result' is declared but never used.
```

---

## 43. Strict Mode

Strict mode treats warnings as errors.

Usage:

```bash
cimplec program.cimple --strict
```

This is useful for serious projects after learning.

---

## 44. Standard Library — Version 1

Keep the standard library tiny.

Built-in operations:

```text
print
sizeof
```

Possible later functions:

```text
input_string()
input_int()
to_int()
to_float()
len()
```

Do not build a large standard library early. The language and compiler should come first.

---

## 45. Build Strategy

Recommended implementation language for the compiler:

1. **C** if the goal is deep systems learning.
2. **Python** if the goal is fastest prototype.
3. **Rust** if the goal is safer compiler implementation.

For this project, a good path is:

```text
Prototype compiler in Python first.
Then rewrite core pieces in C later.
```

Reason:

- Python makes lexer/parser/compiler logic easier to understand.
- C rewrite later teaches memory and data structures deeply.

---

## 46. Minimum Viable Cimple — Version 0.1

Version 0.1 should support only:

- `func main() -> int`
- `let` variables
- `mut` variables
- `int` type
- integer literals
- arithmetic expressions
- assignment
- `print`
- `return`
- compile to C

Example supported program:

```cimple
func main() -> int {
    let x: int = 10
    let y: int = 20
    mut z: int = x + y
    z = z + 5
    print z
    return 0
}
```

Do not add floats, strings, pointers, arrays, structs, or heap memory in Version 0.1.

---

## 47. Version Roadmap

### Version 0.1 — Integer Calculator Language

Features:

- `main`
- `int`
- `let`
- `mut`
- arithmetic
- print
- return
- compile to C

### Version 0.2 — Conditions

Add:

- `bool`
- comparison operators
- `if`
- `else`
- logical operators

### Version 0.3 — Loops

Add:

- `while`
- loop scope
- break/continue later if needed

### Version 0.4 — Functions

Add:

- user-defined functions
- parameters
- return checking

### Version 0.5 — More Types

Add:

- `float`
- `char`
- `string`
- type-specific printing

### Version 0.6 — Stack Pointers

Add:

- `ptr T`
- `addr`
- `deref`
- no pointer arithmetic

### Version 0.7 — Arrays

Add:

- fixed-size arrays
- indexing
- bounds checking

### Version 0.8 — Structs

Add:

- user-defined structs
- field access

### Version 0.9 — Heap Memory

Add:

- `new`
- `delete`
- leak warnings

### Version 1.0 — Stable Teaching Language

Add:

- clean documentation
- test suite
- friendly diagnostics
- examples
- generated C inspection
- simple debugger-style memory visualization

---

## 48. Teaching Examples

### Example 1 — Variables

```cimple
func main() -> int {
    let x: int = 10
    let y: int = 20
    print x + y
    return 0
}
```

Teaches:

- variables
- types
- arithmetic
- print

### Example 2 — Mutable State

```cimple
func main() -> int {
    mut count: int = 0
    count = count + 1
    print count
    return 0
}
```

Teaches:

- mutable variables
- assignment

### Example 3 — Control Logic

```cimple
func main() -> int {
    let temperature: int = 120

    if temperature > 100 {
        print "High temperature"
    } else {
        print "Normal"
    }

    return 0
}
```

Teaches:

- conditionals
- comparison

### Example 4 — Loop

```cimple
func main() -> int {
    mut i: int = 0

    while i < 5 {
        print i
        i = i + 1
    }

    return 0
}
```

Teaches:

- loops
- repeated execution

### Example 5 — Pointers

```cimple
func main() -> int {
    mut x: int = 42
    let p: ptr int = addr x

    print deref p

    return 0
}
```

Teaches:

- address
- pointer
- dereference
- stack memory

---

## 49. Memory Visualization Goal

A special teaching feature can be added:

```bash
cimplec pointer_demo.cimple --show-memory
```

Example output:

```text
Stack frame: main
+----------+----------+-------+
| Variable | Address  | Value |
+----------+----------+-------+
| x        | 0x1000   | 42    |
| p        | 0x1008   | 0x1000|
+----------+----------+-------+
```

This is one of the most important teaching features because it connects code to memory.

---

## 50. Processor Learning Goal

Later, Cimple should support assembly output:

```bash
cimplec hello.cimple --emit-asm
```

Simple Cimple:

```cimple
let x: int = 10
let y: int = 20
let z: int = x + y
```

Conceptual assembly:

```asm
mov eax, 10
mov ebx, 20
add eax, ebx
```

This teaches:

- registers
- CPU instructions
- memory load/store
- stack frames
- function calls

---

## 51. Testing Strategy

Compiler should have tests for every stage.

Lexer tests:

```text
source -> expected tokens
```

Parser tests:

```text
source -> expected AST
```

Type checker tests:

```text
valid programs should pass
invalid programs should fail with useful errors
```

Code generation tests:

```text
Cimple source -> generated C -> executable output
```

Example test:

```cimple
func main() -> int {
    print 10 + 20
    return 0
}
```

Expected output:

```text
30
```

---

## 52. Project Folder Structure

Suggested compiler folder:

```text
cimple/
  README.md
  examples/
    hello.cimple
    variables.cimple
    pointers.cimple
  src/
    main.py
    lexer.py
    tokens.py
    parser.py
    ast_nodes.py
    type_checker.py
    codegen_c.py
    diagnostics.py
  tests/
    test_lexer.py
    test_parser.py
    test_type_checker.py
    test_codegen.py
  build/
```

Later C implementation:

```text
cimple-c/
  include/
  src/
    lexer.c
    parser.c
    ast.c
    typechecker.c
    codegen_c.c
    main.c
  tests/
  examples/
```

---

## 53. First Implementation Recommendation

Do not implement the whole language at once.

Start with this exact program:

```cimple
func main() -> int {
    let x: int = 10
    let y: int = 20
    print x + y
    return 0
}
```

The first compiler only needs to:

1. Tokenize it.
2. Parse it.
3. Build an AST.
4. Check that `x` and `y` are integers.
5. Generate C.
6. Compile the C using GCC or Clang.
7. Run the executable.

Once that works, expand feature by feature.

---

## 54. Non-Goals for Early Versions

Do not add these early:

- Classes
- Inheritance
- Generics
- Async/await
- Garbage collector
- Full ownership system
- Complex macros
- Package manager
- Threading
- Object-oriented programming
- Operator overloading
- Advanced templates

These are distractions at the beginning.

---

## 55. Final Definition

Cimple is:

> A small compiled teaching language that starts like simple Python, exposes memory like C, and gradually introduces safety ideas from modern systems programming.

The first real goal is not to make a perfect language.

The first real goal is to make this pipeline work:

```text
Cimple code → tokens → AST → checked program → generated C → executable
```

Once that pipeline works, Cimple becomes a powerful learning tool for programming, compilers, memory, and processors.
