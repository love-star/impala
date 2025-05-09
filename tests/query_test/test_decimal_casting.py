# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# Validates that casting to Decimal works.
#
from __future__ import absolute_import, division, print_function
from builtins import range
from decimal import Decimal, getcontext, ROUND_DOWN, ROUND_HALF_UP
from random import randint

from tests.common.impala_test_suite import ImpalaTestSuite
from tests.common.test_dimensions import create_exec_option_dimension_from_dict
from tests.common.test_vector import ImpalaTestDimension, ImpalaTestMatrix


class TestDecimalCastingBase(ImpalaTestSuite):
  """Test Suite to verify that casting to Decimal works.

  Specifically, this test suite ensures that:
    - overflows and underflows and handled correctly.
    - casts from decimal/string to their exact decimal types are correct.
    - max/min/NULL/0 can be expressed with their respective decimal types.
    - TODO: Add cases for cast from float/double to decimal types.
  """
  # We can cast for numerics or string types.
  CAST_FROM = ['string', 'number']
  # Set the default precision to 38 to operate on decimal values.
  getcontext().prec = 38
  # Represents a 0 in decimal
  DECIMAL_ZERO = Decimal('0')

  @classmethod
  def add_test_dimensions(cls):
    cls.ImpalaTestMatrix = ImpalaTestMatrix()

    # Core/pairwise only deals with precision 6, 16, 26 (different integer types)
    precision = [6, 16, 26]
    # (0 <= scale <= 38)
    scale = list(range(0, 39))
    if cls.exploration_strategy() == 'exhaustive':
      # (0 < precision <= 38)
      precision = list(range(1, 39))

    cls.ImpalaTestMatrix.add_dimension(ImpalaTestDimension('precision', *precision))
    cls.ImpalaTestMatrix.add_dimension(ImpalaTestDimension('scale', *scale))
    cls.ImpalaTestMatrix.add_dimension(
        ImpalaTestDimension('cast_from', *TestDecimalCasting.CAST_FROM))
    cls.ImpalaTestMatrix.add_dimension(create_exec_option_dimension_from_dict(
        {'decimal_v2': ['false', 'true']}))
    cls.iterations = 1

    # Scale must be less than or equal to precision.
    cls.ImpalaTestMatrix.add_constraint(
        lambda v: v.get_value('scale') <= v.get_value('precision'))

  def _gen_decimal_val(self, precision, scale):
    """Generates a Decimal object with the exact number of digits as the precision."""
    # Generates numeric string which has as many digits as the precision.
    num = str(randint(10**(precision - 1), int('9' * precision)))
    # Incorporate scale into the string.
    if scale != 0: num = "{0}.{1}".format(num[:-scale], num[precision - scale:])
    # Convert the generated decimal string into a Decimal object and return a -ive/+ive
    # version of it with equal probability.
    return Decimal(num) if randint(0, 1) else Decimal("-{0}".format(num))

  def _assert_decimal_result(self, cast, actual, expected):
    assert expected == actual, "Cast: {0}, Expected: {1}, Actual: {2}".format(cast,
        expected, actual)

  def _normalize_cast_expr(self, decimal_val, precision, cast_from):
    if cast_from == 'string':
      return "select cast('{0}' as Decimal({1},{2}))"
    else:
      return "select cast({0} as Decimal({1},{2}))"


class TestDecimalCasting(TestDecimalCastingBase):

  @classmethod
  def add_test_dimensions(cls):
    super(TestDecimalCasting, cls).add_test_dimensions()

  def test_min_max_zero_null(self, vector):
    """Sanity test at limits.

    Verify that:
      - We can read decimal values at their +ive and -ive limits.
      - 0 is expressible in all decimal types.
      - NULL is expressible in all decimal types
    """
    precision = vector.get_value('precision')
    scale = vector.get_value('scale')
    dec_max = Decimal('{0}.{1}'.format('9' * (precision - scale), '9' * scale))
    # Multiplying large values eith -1 can produce an overflow.
    dec_min = Decimal('-{0}'.format(str(dec_max)))
    cast = self._normalize_cast_expr(dec_max, precision, vector.get_value('cast_from'))
    # Test max
    res = Decimal(self.execute_scalar(cast.format(dec_max, precision, scale)))
    self._assert_decimal_result(cast, res, dec_max)
    # Test Min
    res = Decimal(self.execute_scalar(cast.format(dec_min, precision, scale)))
    self._assert_decimal_result(cast, res, dec_min)
    # Test zero
    res = Decimal(self.execute_scalar(
        cast.format(TestDecimalCasting.DECIMAL_ZERO, precision, scale)))
    self._assert_decimal_result(cast, res, TestDecimalCasting.DECIMAL_ZERO)
    # Test NULL
    null_cast = "select cast(NULL as Decimal({0}, {1}))".format(precision, scale)
    res = self.execute_scalar(null_cast)
    self._assert_decimal_result(null_cast, res, 'NULL')

  def test_exact(self, vector):
    """Test to verify that an exact representation of the desired Decimal type is
    maintained."""
    precision = vector.get_value('precision')
    scale = vector.get_value('scale')
    for i in range(self.iterations):
      val = self._gen_decimal_val(precision, scale)
      cast = self._normalize_cast_expr(val, precision, vector.get_value('cast_from'))\
          .format(val, precision, scale)
      res = Decimal(self.execute_scalar(cast))
      self._assert_decimal_result(cast, res, val)

  def test_overflow(self, vector):
    """Test to verify that we always return NULL when trying to cast a number with greater
    precision that its intended decimal type"""
    precision = vector.get_value('precision')
    scale = vector.get_value('scale')
    for i in range(self.iterations):
      # Generate a decimal with a larger precision than the one we're casting to.
      from_precision = randint(precision + 1, 39)
      val = self._gen_decimal_val(from_precision, scale)
      cast = self._normalize_cast_expr(val, from_precision,
          vector.get_value('cast_from')).format(val, precision, scale)
      self.execute_query_expect_failure(self.client, cast)


class TestDecimalCastingUnderflow(TestDecimalCastingBase):

  @classmethod
  def add_test_dimensions(cls):
    super(TestDecimalCastingUnderflow, cls).add_test_dimensions()
    # "Cannot underflow scale when precision and scale are equal"
    cls.ImpalaTestMatrix.add_constraint(
        lambda v: v.get_value('scale') < v.get_value('precision'))

  def test_underflow(self, vector):
    """Test to verify that we truncate when the scale of the number being cast is higher
    than the target decimal type (with no change in precision).
    """
    precision = vector.get_value('precision')
    scale = vector.get_value('scale')
    is_decimal_v2 = vector.get_value('exec_option')['decimal_v2'] == 'true'
    cast_from = vector.get_value('cast_from')

    for i in range(self.iterations):
      from_scale = randint(scale + 1, precision)
      val = self._gen_decimal_val(precision, from_scale)
      cast = self._normalize_cast_expr(val, precision, cast_from)\
          .format(val, precision, scale)
      res = Decimal(self.execute_scalar(cast, vector.get_value('exec_option')))
      if is_decimal_v2:
        expected_val = val.quantize(Decimal('0e-%s' % scale), rounding=ROUND_HALF_UP)
      else:
        expected_val = val.quantize(Decimal('0e-%s' % scale), rounding=ROUND_DOWN)
      self._assert_decimal_result(cast, res, expected_val)
