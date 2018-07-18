# typed: true
class CSV < Object
  include Enumerable

  extend T::Generic
  Elem = type_member(:out, fixed: T::Array[String])

  DEFAULT_OPTIONS = T.let(T.unsafe(nil), Hash)
  VERSION = T.let(T.unsafe(nil), String)

  type_parameters(:U).sig(
      path: T.any(String, File),
      options: T::Hash[Symbol, T.type_parameter(:U)],
      blk: T.proc(arg0: T::Array[String]).returns(BasicObject),
  )
  .returns(NilClass)
  def self.foreach(path, options=_, &blk); end

  sig(
      str: String,
      options: T::Hash[Symbol, T.untyped],
      blk: T.proc(arg0: T::Array[T.nilable(String)]).returns(BasicObject),
  )
  .returns(T.nilable(T::Array[T::Array[T.nilable(String)]]))
  def self.parse(str, options=_, &blk); end

  sig(
      str: String,
      options: T::Hash[Symbol, T.untyped],
  )
  .returns(T::Array[T.nilable(String)])
  def self.parse_line(str, options=_); end

  sig(
      path: String,
      options: T::Hash[Symbol, T.untyped],
  )
  .returns(T::Array[T::Array[T.nilable(String)]])
  def self.read(path, options=_); end

  sig(
      io: T.any(IO, StringIO, String),
      options: T::Hash[Symbol, T.untyped],
  )
  .returns(NilClass)
  def initialize(io=_, options=_); end
end

sig(
    io: T.any(IO, StringIO, String),
    options: T::Hash[Symbol, T.untyped],
)
.returns(CSV)
def CSV(io=_, options=_); end
