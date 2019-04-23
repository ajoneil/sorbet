# typed: true
class FalseClass
  extend T::Sig

  sig {returns(TrueClass)}
  def blank?
    true
  end
end

class NilClass
  extend T::Sig

  sig {returns(TrueClass)}
  def blank?
    true
  end
end

class Object
  def blank?
    !self
  end
end

class A
  extend T::Sig

  sig {params(s: T.nilable(String)).returns(NilClass)}
  def test_return(s)
    if !s.blank?
      s.length
      nil
    end
    nil
  end

  sig {params(s: String).returns(NilClass)}
  def test_unchanged(s)
    if s.blank?
      s.length
    else
      s.length
    end
    nil
  end

  def unreachable()
    if !nil.blank?
      "foo" # error: This code is unreachable
    end
  end
end