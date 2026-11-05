## Core principles

Do not modify this section without explicit approval.

### Documentation

- [`docs/dev/`](docs/dev/) and this file are for developers and agents.
- [`docs/user/`](docs/user/) and [`README.md`](README.md) are for users.
- [`docs/archive/`](docs/archive/) is legacy. The code is authoritative.
- Keep documentation short. When the project changes, update it: a new
  reader should recover the decisions and the scars from these files, the
  comments, and the Git history.
- Write down non-trivial findings, especially from online/offline search.
- Unicode is encouraged to enrich user documents.
- Prefer mermaid over ASCII diagrams.

### Development

- Git history follows [upstream Linux kernel conventions](https://www.kernel.org/doc/html/latest/process/submitting-patches.html):
  focused, bisectable commits and a topic branch per independent change.
  Subject line names the change; the body says why. That includes merge
  commits: `--no-ff` only when the branch carries a series worth summarising
  or when it genuinely diverged, and then the merge body *is* that summary.
  A topic branch holding one commit off the current tip fast-forwards — an
  empty `Merge branch 'x'` carries no information and should not exist.
- Follow Linus Torvalds' code taste.
- Prefer self-explanatory code over verbose comments. Comments should be non-obvious.
- Avoid trivial, meaningless test.
- Launch a reviewer subagent before a branch is merged into main.
- Helper scripts and tools that paid for themselves stay in the tree so
  the next task can reuse them.
- Prevent duplication: Always search the codebase (e.g., existing utils/helpers) for reusable functions before implementing new ones.

### Tooling

- Prefer lsp over grep when feasible.
- Don't spawn a subagent when you already have all the context to finish the task.
- Reuse subagents when appropriate rather than spawn new ones.

## Modern C Expertise

name: modern-c
description: Use this skill when writing, reviewing, refactoring, or explaining modern C code in the latest C23/C2y style. It guides agents toward warning-free, performance-conscious C that respects the object model, lifetimes, aliasing, arrays and pointers, concurrency, and standard-library contracts, and it treats GCC/Clang shared extensions as normal tools when the standard is not expressive enough.

Use this skill for current GCC/Clang-targeted C in C23/C2y style. The default target is practical modern systems code: warning-free, performance-conscious, explicit about contracts, and willing to use shared extensions, POSIX, and glibc when they improve the result.

### Non-negotiable defaults

- Target the newest available GCC/Clang language mode, not legacy compilers.
- Assume builds use `-std=gnu2y -fms-extensions -Wno-microsoft-anon-tag` by default.
- Prefer standard C23/C2y facilities when they are good enough, but use GCC/Clang shared extensions freely when they give better contracts, diagnostics, code generation, or low-level control.
- Do not preserve C11/C17-era compatibility unless the task explicitly requires it.
- Do not add fallback probes such as `__STDC_VERSION__`, `__has_include`, or `__has_c_attribute` unless compatibility handling is the task.
- Treat POSIX and glibc APIs as normal tools, not portability failures by default.
- Avoid GCC-only or Clang-only solutions unless the task explicitly allows single-compiler code.
- Keep performance in scope from the start, but do not force permanent safety overhead into hot paths when a fast primitive plus a safer wrapper gives a better design.
- Keep code warning-free. Warnings usually point to real contract, correctness, or portability problems.
- Do not import C++ assumptions into C.

### Design and correctness rules

- Use `constexpr` when an object must be a true constant expression. Plain `const` only means read-only.
- In modern C, `{}` is the default initializer spelling.
- Every function needs a prototype. Old K&R-style declarations do not belong in new code.
- Prefer standard `[[...]]` syntax for contracts and diagnostics; use `[[nodiscard]]`, `[[maybe_unused]]`, `[[deprecated("reason")]]`, `[[fallthrough]]`, and `[[noreturn]]` when they express real intent, and treat `[[unsequenced]]`/`[[reproducible]]` as proof-heavy optimization contracts.
- Keep side effects out of value expressions when sequencing matters. Function-call arguments are not evaluated in portable left-to-right order.
- Avoid narrowing conversions and mixed-signedness expressions unless the conversion behavior is deliberate and reviewed.
- Use `size_t` for sizes and counts, and `ptrdiff_t` for signed differences between positions.
- Use unsigned types for bit manipulation.
- Prefer designated initializers for aggregates.
- Treat string literals as read-only data.
- Arrays are not pointers. Pass lengths explicitly and do not expect `sizeof` on a parameter to recover the original array extent.
- `nullptr` is the preferred null-pointer spelling.
- A usable pointer is null, points to a live object, or points one past the end of the same array object.
- Think of object pointers as pointers into arrays of their base type; that model keeps pointer arithmetic honest.
- Avoid `void *` in interfaces when a more precise type expresses the contract better.
- Avoid unnecessary casts. They often suppress the compiler's best diagnostics.
- Access object representations through character types when raw bytes are intended.
- Do not cast the result of `malloc` and friends.
- Handle allocation failure. Every successful allocation needs a matching `free` on the original allocator-returned pointer.
- Storage from `malloc` is uninitialized and has no effective type until written through an appropriate lvalue.
- Flexible-array-member objects need explicit consistency between metadata and allocated storage.
- Check return values from allocation, I/O, parsing, library, and synchronization functions.
- `printf` and `scanf` format strings must exactly match the promoted argument types.
- Prefer `snprintf` over `sprintf` when the size is not trivially fixed.
- Do not use `gets`.
- End-of-file is only known after a read attempt fails.
- Prefer `return` from `main` for ordinary completion and reserve `exit` for deeper frames that must terminate the program.
- Prefer low-overhead primitives when their preconditions already hold, and layer safer wrappers on top instead of paying both costs everywhere.
- Use `goto` for cleanup and other exceptional control flow, not as a substitute for ordinary structure.
- Use `unreachable` only when correctness is actually proven.
- `restrict` is an exclusivity contract. Use it only when callers can satisfy it.

### Concurrency rules

- Unsynchronized conflicting access to a non-atomic object across threads is a data race and a correctness failure.
- Use `_Atomic(T)` as the primary declaration form for atomic objects.
- Atomics are the standard tool for race-free shared state; mutexes and condition variables still need correct protocol.
- Initialize and destroy mutexes and condition variables correctly, pair each successful lock with exactly one unlock, and re-check the condition after every wakeup.
- Lock multiple mutexes in one global order.
- In signal handlers, restrict yourself to async-signal-safe operations and the smallest possible shared state.

### Array and pointer parameter notation

Use array syntax when a pointer parameter semantically designates an array. In parameter arrays, `static` means the argument must be nonnull and must provide at least the stated number of accessible elements.

```c
// p must be a nonnull pointer
int f(int p[static 1]);

// p must be a nonnull pointer to at least N elements
int f(int p[static N]);

// ptr must be nonnull and must provide at least len elements
void f(size_t len, int ptr[static len]);

// ptr may be null, or may point to at least len elements;
// without static, len does not imply nonnull
void f(size_t len, int ptr[len]);

// p may be null, or may point to at least N elements;
// without static, N does not imply nonnull
int f(int p[N]);

// p is a nullable pointer to a single object or unspecified extent
int f(int *p);
```

Put length parameters before dependent array parameters. Still pass explicit lengths; array notation does not let the callee recover the element count with `sizeof`.

### Prefer these facilities

Use these directly when they improve the code.

#### Core modern C

- `constexpr` objects for true constant-expression objects.
- Storage-class compound literals when lifetime control matters.
- Empty initializer `{}` for default initialization.
- Fixed-underlying-type enums when range or ABI matters.
- `nullptr` and `nullptr_t` in pointer-only contexts.
- `<stdckdint.h>` checked arithmetic when overflow must be detected.
- `<stdbit.h>` utilities instead of handwritten bit hacks or vendor intrinsics.
- Qualifier-preserving search interfaces such as modern `strchr`, `strstr`, `memchr`, and `bsearch`.
- `auto` for local temporaries and macro internals when it improves type consistency; reserve `typeof` and `typeof_unqual` for advanced generic code.
- `__VA_OPT__` for portable variadic macros.
- `memset_explicit` for scrubbing secrets.
- `_BitInt(N)` for exact-width numeric bit-fields.

#### Emerging C2y and useful extensions

- `_Countof` / `countof` for actual arrays, never pointers.
- `if` declarations when a temporary only belongs to one conditional chain.
- `0o...` literals and `\o{...}` / `\x{...}` escapes instead of legacy octal spellings.
- `#embed` for small immutable build-time assets when supported.
- Labeled `break` and `continue` when they make deeply nested control flow clearer.
- Standardized endian-aware load/store, byte-reverse, rotate, and related bit facilities when available.
- GCC/Clang shared extensions for attributes, builtins, diagnostics, type introspection, or code-generation control when the standard is weaker than the code needs. e.g. [[gnu::cleanup]]

### C23 attributes

Use the standard attribute syntax directly in modern C.

- Syntax and placement: `[[attr]]`, `[[attr(args)]]`, and `[[vendor::attr(args)]]` are the standard forms. Attributes may appear before a declaration or immediately after the declared name; prefer `[[gnu::...]]` or `[[clang::...]]` over legacy `__attribute__((...))` or `__declspec(...)` spellings when you need vendor extensions.
- `[[deprecated]]` and `[[deprecated("reason")]]`: use on declarations that remain available for compatibility but should trigger migration. Include a concrete replacement hint when you have one.
- `[[fallthrough]]`: use only as a standalone attribute declaration immediately before the next `case` or `default` label in the same `switch`; prefer it over ad hoc fallthrough comments.
- `[[nodiscard]]` and `[[nodiscard("reason")]]`: use on functions, or on struct/union/enum result types returned by value, when silently discarding the result is usually a bug. Callers may still cast to `void` when discarding is deliberate.
- `[[maybe_unused]]`: use for intentionally unused declarations that are still part of a correct design, such as assertion-only locals, callback parameters, debug helpers, or compatibility shims. Prefer deleting dead code over annotating it.
- `[[noreturn]]`: use on functions that never return normally, such as fatal-exit helpers; prefer it over the deprecated `_Noreturn` spelling, and never put it on a function that can fall through or execute `return`.
- `[[unsequenced]]`: reserve for functions whose calls are effectless, idempotent, stateless, and independent. This is an optimization contract, not decorative documentation.
- `[[reproducible]]`: reserve for functions whose calls are effectless and idempotent. If you cannot prove the contract, do not guess.

### Review prompts

When generating or reviewing code, actively check these:

- Are the toolchain assumptions aligned with `-std=gnu2y -fms-extensions -Wno-microsoft-anon-tag`?
- Are preconditions, ownership, nullability, bounds, and failure paths explicit in the interface?
- Are array lengths passed explicitly and declared before dependent array parameters?
- Are there hidden sequencing hazards, narrowing conversions, or mixed-signedness bugs?
- Are `void *`, casts, and aliasing-sensitive operations used only where truly necessary?
- Are allocations sized from the pointed-to type, checked for failure, and paired with cleanup?
- Are the chosen library interfaces the right ones, including POSIX or glibc interfaces where those are better fits?
- Is a newer C23/C2y feature being used because it improves correctness, clarity, or performance rather than novelty?
- Should the API be split into a fast primitive and a safer wrapper instead of forcing one policy on every caller?
- Is concurrency free of unsynchronized conflicting access to non-atomic shared state?
- Is a macro being used where an `inline` function would be safer?
- If code changed and the review is safety-focused, should GCC `-fanalyzer` be run as a separate pass?

### Default style for answers

- Prefer small, warning-free examples in C23/C2y style.
- Keep ownership, bounds, lifetimes, preconditions, and failure handling explicit.
- Explain concrete benefits when replacing a legacy idiom: stronger contracts, less UB, clearer lifetime, better diagnostics, or better performance.
- Recommend POSIX, glibc, or GCC/Clang shared extensions directly when they are the cleanest solution under this skill's defaults.