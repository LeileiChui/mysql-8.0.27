/* Implement of TO_NUMBER()
 *
 */

#include "m_string.h"
#include "my_sys.h"
#include "mysqld_error.h"  // ER_...
#include "sql_string.h"

#include "boost/regex.hpp"
#include "sql/oracle_compatibility/convert_number.h"
#include "sql/oracle_compatibility/ora_format.h"
#include "sql/sql_class.h"

// Flags
#define NUM_F_DECIMAL (1 << 1)
#define NUM_F_LDECIMAL (1 << 2)
#define NUM_F_DIGIT_0 (1 << 3)
#define NUM_F_DIGIT_9 (1 << 4)
#define NUM_F_GROUP (1 << 5)
#define NUM_F_LGROUP (1 << 6)
#define NUM_F_LCURRENCY (1 << 7)
#define NUM_F_X (1 << 8)
#define NUM_F_DOLLAR (1 << 9)
#define NUM_F_CURRENCY_MID (1 << 10)

#define IS_DECIMAL(_f) ((_f)->flag & NUM_F_DECIMAL)
#define IS_GROUP(_f) ((_f)->flag & NUM_F_GROUP)
#define IS_LGROUP(_f) ((_f)->flag & NUM_F_LGROUP)
#define IS_X(_f) ((_f)->flag & NUM_F_X)
#define IS_DOLLAR(_f) ((_f)->flag & NUM_F_DOLLAR)
#define IS_CURRENCY(_f) ((_f)->flag & NUM_F_LCURRENCY)
#define IS_G(_f) (IS_GROUP(_f) && IS_LGROUP(_f))
#define IS_CURRENCY_MID(_f) ((_f)->flag & NUM_F_CURRENCY_MID)
#define IS_X_NOT_COMPATIBLE(_f) ((_f)->flag & (~(NUM_F_X)))

/* ----------
 * 数字描述结构体
 * ----------
 */
typedef struct NUMDesc {
  size_t pre = 0;     /* (count) 小数点前的数字 */
  size_t post = 0;    /* (count)小数点后的数字  */
  size_t x_len = 0;   /* 'X' fmt 中的字符（包括 0' 和 'X'） */
  size_t cnt_G = 0;   /* (count)组分隔符的数量，包括'G'或',' */
  size_t cur_pos = 0; /*fmt中处理字符的位置 */
  size_t flag = 0;    /* 参数       */
  size_t isbegin = 0;
  size_t fmt_len = 0; /* fmt 字符串的长度 */
} NUMDesc;

typedef struct NUMProc {
  char is_to_char; /* 0 表示 to_number，其他表示 to_char */
  NUMDesc *Num;

  size_t sign; /* '-' / '+'            */
  size_t sign_wrote;
  size_t num_count;
  size_t num_in;
  size_t num_curr;
  size_t out_pre_spaces;

  size_t read_post;
  size_t read_pre;
  size_t xmatch_len; /* 参与匹配的字符数与输入字符串中的 'X' 的个数 */

  NumBaseType in_type;  //进制

  char *number;           /* 数字字符串    */
  char *number_p;         /* 指向当前数字位置的指针 */
  const char *number_end; /* 结束位置 */
  const char *inout;      /*in out 缓冲区    */
  const char *inout_p;    /* 指向当前 inout 的指针 */
  const char *inout_end;  /*inout 的结束位置 */
  const char *exp_end;    /*指向值字符串中指数结束位置的指针*/

  const char *decimal;
  const char *L_thousands_sep;

  const char *Local_currency;

  bool read_dec; /* to_number - dec 是否读取   */

} NUMProc;

/**
  Enums the type of a character
*/
enum class enum_value_type {
  V_DIGIT,  // digit
  V_DECI,   // decimal point
  V_SIGN,   // sign symbol
  V_SPACE,  // space
  V_CURR,   // currency symbol

  V_LAST
};

static bool prepare_to_num_9(NUMDesc *num) {
  if (IS_X(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0), "invalid \"X\" format.");
    return true;
  }

  if (IS_DECIMAL(num))
    ++num->post;
  else
    ++num->pre;

  num->flag |= NUM_F_DIGIT_9;
  return false;
}

static bool prepare_to_num_0(NUMDesc *num) {
  if (IS_X(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0), "invalid \"X\" format.");
    return true;
  }

  if (IS_DECIMAL(num))
    ++num->post;
  else
    ++num->pre;

  num->flag |= NUM_F_DIGIT_0;
  return false;
}

static inline bool is_start_separator(NUMDesc *num) {
  assert(nullptr != num);
  if (0 == num->cur_pos) return true;
  if (1 == num->cur_pos && IS_CURRENCY(num)) return true;
  return false;
}

//分隔符
static bool prepare_to_num_comma(NUMDesc *num) {
  // start ',' is not valid in a fmt
  if (is_start_separator(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "\",\" cannot at be first or second with some particular "
             "character in fmt.");
    return true;
  }

  if (IS_DECIMAL(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "\",\" must be ahead of decimal point.");
    return true;
  }

  if (IS_LGROUP(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             R"("," should not use with "G".)");
    return true;
  }

  if (IS_CURRENCY_MID(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             R"("," should not use with middle "L")");
    return true;
  }
  ++num->cnt_G;
  num->flag |= NUM_F_GROUP;
  return false;
}

static bool prepare_to_num_G(NUMDesc *num) {
  // G标识不能为开头
  if (is_start_separator(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "\"G\" cannot at be first or second with some particular "
             "character in fmt.");
    return true;
  }
  // G不能位于小数点后
  if (IS_DECIMAL(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "\"G\" must be ahead of decimal point.");
    return true;
  }

  if (IS_GROUP(num) && !IS_LGROUP(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             R"("G" should not use with ",".)");
    return true;
  }
  if (IS_CURRENCY_MID(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             R"("G" should not behind middle "L".)");
    return true;
  }
  ++num->cnt_G;
  num->flag |= NUM_F_GROUP;
  num->flag |= NUM_F_LGROUP;
  return false;
}

static bool prepare_to_num_D(NUMDesc *num) {
  //小数点重复
  if (IS_DECIMAL(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "repeated decimal character in fmt.");
    return true;
  }
  //分隔符后不能有小数点
  if (IS_GROUP(num) && !IS_LGROUP(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             R"("D" should not use with ",")");
    return true;
  }
  if (IS_CURRENCY_MID(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "\"D\" should not use with middle currency character in fmt.");
    return true;
  }

  num->flag |= NUM_F_LDECIMAL;
  num->flag |= NUM_F_DECIMAL;
  return false;
}

static bool prepare_to_num_dec(NUMDesc *num) {
  if (IS_DECIMAL(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "repeated decimal character in fmt.");
    return true;
  }
  if (IS_LGROUP(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             R"("." should not use with "G" in fmt)");
    return true;
  }
  if (IS_CURRENCY_MID(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "\".\" should not use with middle currency character in fmt.");
    return true;
  }

  num->flag |= NUM_F_DECIMAL;
  return false;
}

inline bool is_middle_currency(NUMDesc *num) {
  if (0 != num->cur_pos && num->fmt_len != num->cur_pos + 1) {
    return true;
  }
  return false;
}

static inline bool check_middle_currency(NUMDesc *num) {
  if (is_middle_currency(num)) {
    if (IS_GROUP(num) && !IS_LGROUP(num)) {
      my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
               R"(middle "L" can't use with "," in fmt.)");
      return true;
    }

    if (IS_DECIMAL(num)) {
      my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
               R"(middle "L" can't use with decimal character in fmt.)");
      return true;
    }

    num->flag |= NUM_F_CURRENCY_MID;
    num->flag |= NUM_F_DECIMAL;
    num->flag |= NUM_F_LDECIMAL;
  }
  return false;
}

static bool prepare_to_num_L(NUMDesc *num) {
  if (IS_CURRENCY(num)) {
    //货币标识不能重复
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "repeated currency character in fmt.");
    return true;
  }

  if (IS_X(num)) {
    // 16进制与货币不能同存
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0), "invalid \"X\" format.");
    return true;
  }
  // 检查货币标识位置是否合法
  if (check_middle_currency(num)) return true;

  num->flag |= NUM_F_LCURRENCY;
  return false;
}

static bool prepare_to_num_X(NUMDesc *num) {
  if (IS_X_NOT_COMPATIBLE(num)) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0), "invalid \"X\" format.");
    return true;
  }
  ++num->x_len;
  num->flag |= NUM_F_X;
  return false;
}

/*
 * 准备 to_number 不支持的格式
 */
static bool prepare_to_num_others() {
  my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0), "not support");
  return true;
}

//通过 FormatNode 结构预处理 NUMDesc

static bool NUMDesc_prepare_for_to_num(NUMDesc *num, FormatNode *n) {
  bool res = false;
  switch (n->key->id) {
    case NUM_9:
      res = prepare_to_num_9(num);
      break;

    case NUM_0:
      res = prepare_to_num_0(num);
      break;

    case NUM_COMMA:
      res = prepare_to_num_comma(num);
      break;

    case NUM_G:
      res = prepare_to_num_G(num);
      break;

    case NUM_D:
      res = prepare_to_num_D(num);
      break;

    case NUM_DEC:
      res = prepare_to_num_dec(num);
      break;

    case NUM_L:
      res = prepare_to_num_L(num);
      break;

    case NUM_X:
      res = prepare_to_num_X(num);
      break;

    default:
      res = prepare_to_num_others();
      break;
  }

  return res;
}

/* ----------
 * Format parser, search small keywords and make format-node tree.
 * ----------
 * return: true  -- error
 *         false -- ok
 */
static bool parse_format_for_to_num(FormatNode *node, const char *p_fmt_str,
                                    NUMDesc *Num) {
  FormatNode *n = node;

  while (*p_fmt_str) {
    if ((n->key = index_seq_search(p_fmt_str)) != nullptr) {
      n->type = FormatNodeType::ACTION;
      n->suffix = 0;
      if (n->key->len) p_fmt_str += n->key->len;

      if (NUMDesc_prepare_for_to_num(Num, n)) {
        return true;
      }
      Num->cur_pos += n->key->len;
      n++;
    } else {
      my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
               "unsupported or incorrect format");
      return true;
    }
  }

  Num->cur_pos = 0;  // reset cur_pos in fmt for matching value later
  n->type = FormatNodeType::END;
  n->suffix = 0;

  return false;
}

/* ----------
 * Locale
 * ----------
 */
static inline void TO_NUM_prepare_locale(NUMProc *Np) {
  Np->decimal = ".";
  Np->L_thousands_sep = ",";
  Np->Local_currency = "$";
}

//该函数用于判断该字符是否是 fmt 中最后一个字符。
static bool is_end_fmt(NUMProc *Np) {
  int len = 0;
  if (Np->Num->cur_pos == Np->Num->fmt_len - len - 1) {
    return true;
  }
  return false;
}

//跳过值字符串中的连续“,”
static inline void skip_consecutive_comma(NUMProc *Np) {
  while (Np->inout_p < Np->inout_end && *Np->L_thousands_sep == *Np->inout_p)
    ++Np->inout_p;
}

static inline void skip_decimal(NUMProc *Np) {
  assert(Np->inout_p <= Np->inout_end);
  if (*Np->decimal == *Np->inout_p) ++Np->inout_p;
}

/* 跳过 ',' 和 '.'在'G' 和'D' 或',' 和'.' 之间与fmt 匹配的值*/
static inline void skip_specified_comma_and_decimal(NUMProc *Np) {
  if (0 == Np->Num->cnt_G) skip_consecutive_comma(Np);

  if (!IS_DECIMAL(Np->Num)) skip_decimal(Np);
}

/* process the integer part of digit */
static inline bool process_integer_part_for_digit(NUMProc *Np) {
  /*  当 fmt 的左侧不匹配部分中没有组分隔符（'G' 或 ','）时，需要跳过 value
整数部分中的连续逗号。例如： select to_number('12,,,3.4','99G9D9') from dual;
--- 好的，当我们匹配 fmt 中 'G' 后的 '9' 时，我们需要跳过值中的第二个和第三个
',' */
  if (0 == Np->Num->cnt_G) {
    skip_consecutive_comma(Np);
  }
  if (!isdigit(*Np->inout_p)) {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), "number not match the format!");
    return true;
  }
  assert(Np->read_pre > 0);
  assert(Np->Num->pre > 0);

  assert(Np->number_p < Np->number_end);
  *Np->number_p = *Np->inout_p;
  ++Np->number_p;
  ++Np->inout_p;
  --Np->Num->pre;
  --Np->read_pre;
  /* 如果 value string 中没有更多的整数需要匹配，并且我们到达
   * end_fmt，我们需要跳过额外的逗号 (',') 和一个小数点 ('.')。例如： select
   * to_number('1,,','9') from dual; --- ok select to_number('1,,+','9S') from
   * dual; - - 好的
   */
  if (0 == Np->read_pre) {
    if (is_end_fmt(Np)) skip_specified_comma_and_decimal(Np);
  }
  return false;
}

/* 处理数字的小数部分 */
static inline bool process_decimal_part_for_digit(NUMProc *Np) {
  assert(Np->read_pre == 0);
  assert(Np->Num->pre == 0);

  if (Np->read_post > Np->Num->post) {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), "number not match the format!");
    return true;
  }

  if (0 == Np->read_post) return false;

  if (!isdigit(*Np->inout_p)) {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), "number not match the format!");
    return true;
  }

  assert(Np->number_p < Np->number_end);
  *Np->number_p = *Np->inout_p;
  ++Np->number_p;
  ++Np->inout_p;
  --Np->Num->post;
  --Np->read_post;
  return false;
}

/*
 * 处理 to_num_0 格式
 * return: true  -- error
 *         false -- ok
 */
static bool process_to_num_0(NUMProc *Np) {
  Np->Num->isbegin = true;
  if (Np->read_pre < Np->Num->pre) {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), "number not match the format!");
    return true;
  }

  if (Np->inout_p == Np->inout_end) {
    assert(0 == Np->read_pre && 0 == Np->read_post);
    assert(0 == Np->Num->pre);
    return false;
  }

  if (Np->read_dec)
    return process_decimal_part_for_digit(Np);
  else
    return process_integer_part_for_digit(Np);
}
/*
 *处理 to_num_9 格式
 * return: true  -- error
 *         false -- ok
 */
static bool process_to_num_9(NUMProc *Np) {
  if (Np->read_pre < Np->Num->pre) {
    --Np->Num->pre;
    return false;
  }

  if (Np->inout_p == Np->inout_end) {
    assert(0 == Np->read_pre);
    assert(0 == Np->read_post);
    assert(0 == Np->Num->pre);
    return false;
  }

  Np->Num->isbegin = true;

  if (Np->read_dec)
    return process_decimal_part_for_digit(Np);
  else
    return process_integer_part_for_digit(Np);
}

/*
 * 处理格式（'.' 或 'D'）
 * return: true  -- error
 *         false -- ok
 */
static bool process_to_num_decimal_point(NUMProc *Np) {
  // assert(0 == Np->read_pre);
  assert(0 == Np->Num->pre);

  /*
 跳过小数点前的连续逗号，例如： select to_number('123,,,.4','999.9') from
 dual;我们需要在匹配 '.' 时跳过三个连续的 ',' 值。在 fmt。
  */
  skip_consecutive_comma(Np);

  if (0 == Np->read_post) {
    // select to_number('-1.','9.') from dual;
    // select to_number('-1','9.') from dual;
    skip_decimal(Np);
    return false;
  }
  if (*Np->decimal != *Np->inout_p) {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0),
             R"(not match "D" or "." in the format!)");
    return true;
  }

  Np->read_dec = true;
  assert(Np->number_p < Np->number_end);
  *Np->number_p = *Np->inout_p;
  ++Np->number_p;
  ++Np->inout_p;
  return false;
}

/*
 * 处理 to_num_group_separator 格式（'G' 或 ','）
 * return: true  -- error
 *         false -- ok
 */
static bool process_to_num_group_separator(NUMProc *Np) {
  --Np->Num->cnt_G;
  if (!Np->Num->isbegin) {
    return false;
  }

  /* 如果fmt 有'G' 和 mid-currency，额外的'G's（isbegin 之前不包括'G'，当isbegin
   * 为false 时）不需要匹配值字符串中的','。例如： select
   * to_number('22338','9ggGGggggg0900U9') "Amount" from dual;--- ok
   * 但是如果货币不在中间，我们可以这样做。例如： select
   * to_number('2233USD','9ggg090C') "Amount" from dual; --- error num
   * Explanation for 'select to_number('2233USD','9ggGGggggg0900C') "Amount"
   * from dual;--- ok' 由于在 isbegin 之前有额外的 'G's 被跳过（isbegin now is
   * false）
   */

  assert(Np->inout_p <= Np->inout_end);
  if (*Np->L_thousands_sep != *Np->inout_p) {
    if (IS_G(Np->Num) && IS_CURRENCY_MID(Np->Num)) {
      return false;
    }
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), R"(not match "G" or "," in fmt)");
    return true;
  }
  ++Np->inout_p;
  /* 当 fmt 中没有更多的 'G' 或 ',' 需要匹配并且当前的 'G' 或 ',' 是 end_fmt
   * 时，我们需要在这里跳过值字符串中额外的 ','。例如： select
   * to_number('1,1,,','9G9G') from dual; select to_number('1,1,,,,+','9,9,S')
   * from dual;
   */
  if (0 == Np->Num->cnt_G) {
    if (is_end_fmt(Np)) skip_consecutive_comma(Np);
  }
  return false;
}

static bool process_to_num_L(NUMProc *Np) {
  skip_consecutive_comma(Np);

  const char *c_symbol = Np->Local_currency;

  if (Np->inout_p <= Np->inout_end - 1 &&
      strncmp(Np->inout_p, c_symbol, 1) == 0) {
    Np->inout_p += 1;
    if (IS_CURRENCY_MID(Np->Num)) {
      assert(Np->number_p < Np->number_end);
      *Np->number_p++ = *Np->decimal;
      Np->read_dec = true;
    }
  } else {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), R"(error "L" value)");
    return true;
  }
  return false;
}

//预处理 to_num_X 格式：计算用于匹配 'X'
//格式的值字符串中的字符数。逗号和前导零不参与匹配。
static void preprocess_to_num_X(NUMProc *Np, size_t input_len) {
  if (!IS_X(Np->Num)) return;

  Np->xmatch_len = input_len;
  Np->in_type = NumBaseType::HEX;
  const char *p = Np->inout;

  for (; p < Np->inout_end; ++p)
    if (*p == ',') --Np->xmatch_len;
}

static bool process_to_num_X_1(NUMProc *Np, ulonglong &sum) {
  ulonglong tmp = 0;
  if ('0' <= *Np->inout_p && *Np->inout_p <= '9') {
    tmp = *Np->inout_p - '0';
  } else if ('A' <= *Np->inout_p && *Np->inout_p <= 'F') {
    tmp = *Np->inout_p - 'A' + 10;
  } else if ('a' <= *Np->inout_p && *Np->inout_p <= 'f') {
    tmp = *Np->inout_p - 'a' + 10;
  } else {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), "error \"X\" value");
    return true;
  }

  sum = (sum << 4) + tmp;
  ++Np->inout_p;
  return false;
}

static bool process_to_num_X_2(NUMProc *Np, my_decimal &dec_sum) {
  uint mask = 0x1F;
  bool unsigned_flag = true;
  my_decimal dec_tmp;
  my_decimal mul_tmp;
  my_decimal weight;
  int2my_decimal(mask, 16, unsigned_flag, &weight);

  if ('0' <= *Np->inout_p && *Np->inout_p <= '9') {
    int2my_decimal(mask, *Np->inout_p - '0', unsigned_flag, &dec_tmp);
  } else if ('A' <= *Np->inout_p && *Np->inout_p <= 'F') {
    int2my_decimal(mask, *Np->inout_p - 'A' + 10, unsigned_flag, &dec_tmp);
  } else if ('a' <= *Np->inout_p && *Np->inout_p <= 'f') {
    int2my_decimal(mask, *Np->inout_p - 'a' + 10, unsigned_flag, &dec_tmp);
  } else {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), "error \"X\" value");
    return true;
  }
  my_decimal_mul(mask, &mul_tmp, &dec_sum, &weight);
  my_decimal_add(mask, &dec_sum, &mul_tmp, &dec_tmp);
  Np->inout_p++;
  return false;
}

static bool process_to_num_X(NUMProc *Np, ulonglong &ullsum,
                             my_decimal &dec_sum) {
  // 不支持匹配 'X' 格式的负值
  if ('-' == *Np->inout) {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), "error \"X\" negative value");
    return true;
  }
  // 跳过额外的“X”。
  if (Np->xmatch_len < Np->Num->x_len) {
    --Np->Num->x_len;
    return false;
  }
  // 如果到达值字符串的末尾则返回
  if (Np->inout_end == Np->inout_p) return false;

  skip_consecutive_comma(Np);
  //如果 xmatch_len <= 16，我们可以使用 ulonglong
  //类型变量来存储临时和并参与计算，这样效率更高。否则我们需要十进制类型变量来存储临时和。
  if (Np->xmatch_len <= 16) {
    return process_to_num_X_1(Np, ullsum);
  } else {
    return process_to_num_X_2(Np, dec_sum);
  }
}

static inline void preprocess_to_num_currency(NUMProc *Np) {
  if (Np->inout_p == Np->inout_end) return;
  // 如果是$，直接越过
  if ('$' == *Np->inout_p && IS_DOLLAR(Np->Num)) {
    ++Np->inout_p;
    return;
  }
}

static inline bool is_sign(const char *p) {
  assert(nullptr != p);

  if ('+' == *p || '-' == *p) return true;
  return false;
}

//根据字符获取对应的枚举值
static enum_value_type get_value_type(const char *p) {
  assert(nullptr != p);
  if (isdigit(*p)) return enum_value_type::V_DIGIT;
  if (is_sign(p)) return enum_value_type::V_SIGN;
  if ('.' == *p) return enum_value_type::V_DECI;
  if (' ' == *p) return enum_value_type::V_SPACE;
  if ('$' == *p) return enum_value_type::V_CURR;
  return enum_value_type::V_LAST;
}

static void preprocess_to_num_digit(NUMProc *Np) {
  if (IS_X(Np->Num)) return;

  const char *p = Np->inout_p;
  const char *p_end = Np->inout_end;
  bool read_dec = false;
  enum_value_type val_type = enum_value_type::V_LAST;

  while (p < p_end) {
    val_type = get_value_type(p);
    switch (val_type) {
      case enum_value_type::V_DIGIT:
        if (read_dec)
          ++Np->read_post;
        else
          ++Np->read_pre;
        break;

      case enum_value_type::V_DECI:
        read_dec = true;
        break;

      case enum_value_type::V_CURR:
        // 如果$在数字中间，可视为小数点
        if (IS_CURRENCY_MID(Np->Num)) {
          read_dec = true;
        }
        break;

      default:
        break;
    }
    ++p;
  }
}

static void preprocess_to_num(NUMProc *Np, size_t inout_len) {
  assert(Np->inout_p == Np->inout);
  assert(Np->inout_p < Np->inout_end);

  preprocess_to_num_currency(Np);

  preprocess_to_num_digit(Np);

  preprocess_to_num_X(Np, inout_len);
}

static void init_NUMProc(NUMProc *Np, NUMDesc *Num, const char *inout,
                         size_t inout_len, char *number, size_t num_len) {
  memset(Np, 0, sizeof(NUMProc));

  Np->Num = Num;
  Np->is_to_char = 0;
  Np->number = number;
  Np->inout = inout;
  Np->inout_p = Np->inout;
  Np->inout_end = Np->inout + inout_len;

  Np->exp_end = Np->inout_end;

  Np->read_post = 0;
  Np->read_pre = 0;
  Np->xmatch_len = 0;
  Np->read_dec = false;
  Np->in_type = NumBaseType::DEC;

  Np->sign = false;

  Np->out_pre_spaces = 0;
  *Np->number = ' ';             /* sign space */
  Np->number_p = Np->number + 1; /* first char is space for sign */
  Np->number_end = Np->number + num_len - 1;

  Np->num_in = 0;
  Np->num_curr = 0;

  TO_NUM_prepare_locale(Np);
}

// 重新检查该值是否与 fmt 匹配

static inline bool process_to_num_end_fmt(NUMProc *Np) {
  // select to_number('-,,.','S99') from dual;
  skip_specified_comma_and_decimal(Np);

  //  fmt匹配完后inout仍未结束，报错
  if (Np->inout_p != Np->inout_end) {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), "error number.");
    return true;
  }
  return false;
}

//数字转hex
static void ull2char(const ulonglong *from, char *&to) {
  if (nullptr == from || nullptr == to) {
    return;
  }
  ulonglong tmp = *from;
  char *num_p = to;
  int x = 0, y = 0;
  if (0 == tmp) *num_p++ = '0';
  while (tmp) {
    *num_p++ = '0' + (char)(tmp % 10);
    tmp /= 10;
    ++y;
  }
  *num_p = '\0';
  --y;
  while (x < y) {
    char c = to[x];
    to[x] = to[y];
    to[y] = c;
    ++x;
    --y;
  }
  to = num_p;
}

static bool is_specified_number(const char *p) {
  if (nullptr == p) return false;
  if ('.' == *p || ' ' == *p) return true;
  return false;
}

static void finish_NUMProc(NUMProc *Np, const ulonglong &ull_hexsum,
                           const my_decimal &dec_hexsum) {
  assert(Np->in_type == NumBaseType::DEC || Np->in_type == NumBaseType::HEX);

  if (Np->in_type == NumBaseType::DEC) {
    // select to_number('$','L999') as result from dual;->" " -> " 0"
    // select to_number('+$','SL999') as result from dual;->"+." -> "+0"
    // fix Warning | 1366 | Incorrect DECIMAL value: '0' for column '' at row -1
    if (is_specified_number(Np->number_p - 1)) {
      assert(Np->number_p < Np->number_end);
      *Np->number_p = '0';
      ++Np->number_p;
    }
    assert(Np->number_p <= Np->number_end);
    *Np->number_p = '\0';
    /*
     * Correction - precision of dec. number
     */
    Np->Num->post = Np->read_post;

  } else if (Np->in_type == NumBaseType::HEX) {
    if (Np->xmatch_len <= 16) {
      // ull2char 方式转换
      assert(Np->number_p < Np->number_end);
      ull2char(&ull_hexsum, Np->number_p);
      assert(Np->number_p <= Np->number_end);
    } else {
      //内置方法转换
      String str_tmp, *pStr = &str_tmp;
      my_decimal2string(E_DEC_FATAL_ERROR, &dec_hexsum, pStr);
      char *pstr = pStr->c_ptr_safe();
      size_t str_len = pStr->length();
      assert(Np->number_p < Np->number_end);
      Np->number_p = my_stpncpy(Np->number, pstr, str_len);
      assert(Np->number_p <= Np->number_end);
      *Np->number_p = '\0';
    }
  }
}

static bool TO_NUM_processor(FormatNode *node, NUMDesc *Num, const char *inout,
                             size_t inout_len, char *number, size_t num_len) {
  assert(nullptr != node);
  assert(nullptr != Num);
  assert(nullptr != inout);
  assert(nullptr != number);
  assert(inout_len >= 0);

  FormatNode *n = node;

  NUMProc Np_tmp, *Np = &Np_tmp;

  ulonglong ullsum = 0;
  my_decimal dec_sum;
  int2my_decimal(0x1F, 0, false, &dec_sum);

  bool res = false;

  init_NUMProc(Np, Num, inout, inout_len, number, num_len);

  preprocess_to_num(Np, inout_len);

  for (; n->type != FormatNodeType::END; n++) {
    assert(n->type == FormatNodeType::ACTION);
    assert(Np->inout_p <= Np->inout_end);

    switch (n->key->id) {
      case NUM_0:
        res = process_to_num_0(Np);
        break;

      case NUM_9:
        res = process_to_num_9(Np);
        break;

      case NUM_DEC:
      case NUM_D:
        res = process_to_num_decimal_point(Np);
        break;

      case NUM_COMMA:
      case NUM_G:
        res = process_to_num_group_separator(Np);
        break;
      case NUM_L:
        res = process_to_num_L(Np);
        break;
      case NUM_X:
        res = process_to_num_X(Np, ullsum, dec_sum);
        break;
      default:
        my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), n->key->name);
        return true;
    }
    Np->Num->cur_pos += n->key->len;

    // 解析失败
    if (res) return true;
  }

  if (process_to_num_end_fmt(Np)) return true;
  finish_NUMProc(Np, ullsum, dec_sum);
  return false;
}

//去除前置空格
static const char *preprocess_value_leading_spaces(const char *pstr,
                                                   size_t &pstr_len) {
  assert(nullptr != pstr);
  while (*pstr == ' ' && pstr_len > 1) {
    pstr++;
    pstr_len--;
  }
  return pstr;
}
//判断+-号是否在开头
static inline bool is_start_sign(const char *p, const char *p_end,
                                 size_t p_len) {
  assert(nullptr != p);
  assert(p < p_end);
  assert('+' == *p || '-' == *p);

  if (p + p_len == p_end) return true;
  return false;
}

static bool is_end_space(const char *p, const char *p_end) {
  assert(nullptr != p);
  assert(p < p_end);

  while (p < p_end)
    if (' ' != *(p++)) return false;

  return true;
}

bool parse_to_num_one_param_str(const char *p_str, size_t str_len) {
  assert(nullptr != p_str);
  assert(0 != str_len);

  size_t p_len = str_len;
  const char *p = p_str;
  const char *p_end = p_str + p_len;

  p = preprocess_value_leading_spaces(p, p_len);

  bool res = false;
  bool find_num = false;
  bool find_dec = false;
  enum_value_type val_type = enum_value_type::V_LAST;
  while (p < p_end) {
    val_type = get_value_type(p);
    switch (val_type) {
      case enum_value_type::V_DIGIT:
        find_num = true;
        break;

      case enum_value_type::V_DECI:
        // 如果有两个小数点
        if (find_dec) res = true;
        find_dec = true;
        break;

      case enum_value_type::V_SIGN:
        // 如果正负号不在开头
        if (!is_start_sign(p, p_end, p_len)) res = true;
        break;

      case enum_value_type::V_SPACE:
        // 如果空格后有其他字符
        if (!is_end_space(p, p_end)) res = true;
        break;

      default:
        res = true;
        break;
    }
    if (res) {
      my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), "invalid number.");
      return true;
    }
    ++p;
  }

  if (!find_num) {
    my_error(ER_TO_NUM_VALUE_INVALID, MYF(0), "invalid number.");
    return true;
  }

  return false;
}

bool process_to_num_two_params(String *value, String *fmt, char *out_value,
                               size_t out_len) {
  NUMDesc Num;
  FormatNode *format_node = nullptr;
  bool result = true;

  assert(nullptr != value && nullptr != fmt && nullptr != out_value);

  const char *p_value_str = const_cast<const char *>(value->c_ptr_safe());
  size_t value_str_len = value->length();
  const char *p_fmt_str = const_cast<const char *>(fmt->c_ptr_safe());
  size_t fmt_str_len = fmt->length();

  assert(nullptr != p_value_str && nullptr != p_fmt_str);

  if (fmt_str_len >= TO_NUM_MAX_STR_LEN) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0), "error format_node length");
    return true;
  }

  Num.fmt_len = fmt_str_len;

  if (nullptr == current_thd || nullptr == current_thd->mem_root) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "to_number get current_thd->mem_root failed");
    return true;
  }

  size_t mem_size = (fmt_str_len + 1) * sizeof(FormatNode);

  format_node = (FormatNode *)current_thd->mem_root->Alloc(mem_size);
  if (nullptr == format_node) {
    my_error(ER_TO_NUM_FORMAT_INVALID, MYF(0),
             "to_number format_node malloc error");
    return true;
  }
  // format字符串解析错误
  if ((result = parse_format_for_to_num(format_node, p_fmt_str, &Num))) {
    goto end;
  }

  p_value_str = preprocess_value_leading_spaces(p_value_str, value_str_len);

  result = TO_NUM_processor(format_node, &Num, p_value_str, value_str_len,
                            out_value, out_len);

end:
  return result;
}
