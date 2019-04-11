---
id: rbi
title: RBI Files
---

<!-- TODO(jez) Linkify all `sorbet-typed` references in this document -->

RBI files are "Ruby Interface" files. Sorbet uses RBI files to learn about
constants, ancestors, and methods defined in ways it doesn't understand
natively. By default, Sorbet does **not** know about:

- anything defined within a gem
- ancestors modified with dynamic includes (`other.extend(...)`)
- constants accessed or defined with `const_get` or `const_set`
- methods defined with `define_method` or `method_missing`

RBI files lie at the intersection of the [static](static.md) and
[dynamic](runtime.md) components of Sorbet. They can be autogenerated using
Ruby's reflection APIs at runtime or written by users of Sorbet. This doc
answers three main questions:

- How can we create and update RBI files?
- What is the syntax of an RBI file?
- What kinds of RBI files exist in a project?

We'll cover these questions in order.


## Quickref

These are the many commands to create or update RBI files within a Sorbet
project:

| I want to:                                                         | so I'll run:                 |
| ---                                                                | ---                          |
| Initialize a new Sorbet project, including all RBI files           | `srb init`                   |
| Update every kind of RBI in an existing project                    | `srb rbi update`             |
| &nbsp;                                                             | &nbsp;                       |
| Fetch pre-written RBIs (either from gem sources or `sorbet-typed`) | `srb rbi sorbet-typed`       |
| (Re)generate RBIs for all gems using runtime reflection            | `srb rbi gems`               |
| (Re)generate an RBI for all "hidden definitions" in a project      | `srb rbi hidden-definitions` |
| (Re)generate a the TODO RBI file (for missing constants)           | `srb rbi todo`               |

For more information about `srb init`, see [Adopting Sorbet](adopting.md).

Note that `srb rbi update` is an alias for `srb init`.

The subcommands are discussed below (alongside the different kinds of RBI
files).

## Syntax

The syntax of RBI files is the same as normal Ruby files, except that method
definitions do not need implementations. The only new syntax is for method
signatures (which [are themselves valid Ruby syntax][why-ruby-syntax]). See
[Writing sigs](sigs.md) for the syntax of method signatures.

[why-ruby-syntax]: sigs.md#why-are-signatures-ruby-syntax

```ruby
# -- example.rbi --
# typed: strict

# Declares a module
module MyModule
end

# Declares a class
class Parent
  # Declares what modules this class mixes in
  include MyModule
  extend MyModule

  # Declares an untyped method
  def foo
  end

  # Declares a method with input and output types
  Sorbet.sig {params(x: Integer).returns(String)}
  def bar(x)
  end
end

# Declares a class that subclasses another class
class Child < Parent
  # Declares an Integer constant, with an arbitrary value
  X = T.let(T.unsafe(nil), Integer)
end
```


## Gem RBIs

RBIs are predominantly used to define classes, constants, and methods to Sorbet.
Sorbet does not look at the source code of gems that a project depends on (in
general, this would make Sorbet slower to typecheck a project and also toilsome
if any gems don't typecheck on their own).

Instead, there are three ways to get RBI files to teach Sorbet about a gem:

- By using `srb` to create RBIs using runtime reflection.
- By handwriting RBIs for a gem, or using handwritten RBIs the Sorbet community
  has shared.
- By including RBI files directly within the source of a gem.

We'll discuss each one of these in turn.

### Autogenerated RBIs for gems

The most common way to get RBI files for gems is to generate (or regenerate)
them. To regenerate RBI files in an already-initialized project:

```bash
# Update only the autogenerated gem RBIs:
srb rbi gems

# Update every kind of RBI file, including autogenerated gem RBIs:
srb rbi update
```

The way this works is that `srb` will load a project's Gemfile and `require`
each gem declared within. While loading each gem, it uses runtime reflection to
learn what things that gem defines, and then serializes this information into a
single RBI file for that gem.

This process is somewhat imperfect, but it's a good start. Importantly, because
it only loads a gem's code, it can only know the arity of methods defined, but
not what types of values the methods accept or return.

### Hand-written RBIs for gems

An alternative to autogenerated RBI files are hand-written RBI files. For
example, Sorbet itself ships with hand-written RBIs for the Ruby standard
library. Hand-written RBI files have the advantage that in addition to capturing
a method's arity, they can also declare a method's input and output types.

Hand-written RBIs for gems can come from either:

- `sorbet-typed`, a central repository for sharing hand-written RBI files with
  the Sorbet community, or
- you!

When initializing a new Sorbet project, `srb init` reads the project's
`Gemfile`, checks to see if `sorbet-typed` already has any suitable RBI files,
and fetches them into the current project if so. After initialization, to
update or add new RBI files from `sorbet-typed`, run:

```bash
srb rbi sorbet-typed
```

When `sorbet-typed` does not have RBI files for a gem, `srb init` will still
have created some autogenerated RBI files for that gem. These autogenerated
files are a **great** way to start off writing hand-written RBIs for a gem!
Simply move the autogenerated file out of the RBI `sorbet/rbi/sorbet-typed/`
folder, and start modifying it by hand.

### RBIs within gems

The last way to include RBI files for a gem into a project is for the gem itself
to ship with RBI files. In the future, we anticipate this to be the preferred
way to include RBI files into a project.

The way this works is that when `srb` will load a project's `Gemfile` to find
the source folders for every gem in a project. If any gems have a top-level
`rbi/` or `rbis/` folder, `srb` will collect the paths to any such folders into
a file (`sorbet/rbi_list`) and mention this file in the `sorbet/config` file.


## The Hidden Definition RBI

When typechecking an existing Ruby project with Sorbet, usually type annotations
for all the gems are not enough to statically understand everything that's going
on. Ruby as a language is well-known for encouraging "metaprogramming," or
defining things at runtime. In Sorbet, we call anything that can't be seen
statically but which is defined at runtime a **hidden definition**. This
includes constants defined with `const_set`, methods defined with
`define_method`, ancestor lists modified within methods (instead of at the
top-level of a class), and more.

In order to find all the hidden definitions in a project, Sorbet does three
things:

- loads an entire project, and walks every object in `ObjectSpace` to see what's
  defined
- runs Sorbet on the entire project in a mode where Sorbet will emit something
  for every definition
- subtract these two lists of definitions, and serialize an RBI file for the
  difference

Like with autogenerated gem RBI files, the methods defined in the hidden
definitions RBI file will all be untyped.

Unlike gem RBIs which only need to be updated when a gem is added, removed, or
updated, the hidden definitions might need to be updated frequently, depending
on how much metaprogramming a codebase is using. To regenerate the hidden
definitions RBI file:

```bash
srb rbi hidden-definitions
```

On a philosophical level, we believe that while heavily metaprogrammed APIs 
can make it easy for a code author, they're frequently harder for consumers of
the API to understand where things are being defined. By making metaprogramming
explicit in one file, it's easy to track whether the amount of metaprogramming
in a codebase is going up or down over time.

Sorbet offers a number of powerful type-level features to enforce
ergonomic abstractions. When writing new code or refactoring old code,
be sure to read up on [Abstract methods and interfaces](abstract.md) and [Union
types](union-types.md), both of which allow code authors to write code that
Sorbet can understand and analyze more effectively.


## The TODO RBI file

Sometimes even autogenerated gem RBI files and the hidden definitions RBI file
aren't enough to adopt Sorbet in a new codebase. Sorbet requires that all
constants resolve, and our runtime reflection to find constants is still
imperfect.

As one last attempt to ease the adoption process of Sorbet, when running `srb
init` if after creating all these RBI files there are still missing constants,
we'll write out an RBI file that defines them, regardless of whether they exist
at runtime or not. **This can be dangerous!** In particular, at this point we're
not checking whether something was actually defined but Sorbet couldn't see it,
or whether it was never defined and is actually buggy code that hasn't been
caught by a project's test suite!

That's why this file is called the TODO RBI. After initializing a project to use
Sorbet, take a glance over the file at `sorbet/rbi/todo.rbi` and
attempt to find out why Sorbet thinks these constants are missing. Ask: is the
code actually covered by tests? Does the constant exist in an `irb` or `pry`
REPL session?

If after inspecting a constant in the TODO RBI file you're sure it should always
exist at runtime, feel free to move it out of the TODO RBI file and into a
hand-written RBI file.


## A note about vendoring RBIs

You might have noticed that we **vendor** all gems' RBI files into the current
directory, and commit them to version control. Why? When developing RBI files
for Sorbet, we referenced the prior art that [Flow](https://flow.org) developed.
Our reasoning is [the same as theirs]:

When an RBI is improved or updated, there's some chance that the change could
introduce new Sorbet errors into the project. As good as it is to find new
issues, we also want to make sure that Sorbet errors in a project are consistent
and predictable over time.

So if/when we wish to upgrade an RBI that we've already checked in to our
project's version control, we can do so explicitly with the `srb rbi` command.

[the same as theirs]: https://github.com/flow-typed/flow-typed/wiki/FAQs#why-do-i-need-to-commit-the-libdefs-that-flow-typed-installs-for-my-project