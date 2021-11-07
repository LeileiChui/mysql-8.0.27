#ifndef TO_NUMBER_H
#define TO_NUMBER_H

#include "sql/item_strfunc.h"
#include "sql/sql_list.h"

#define TO_NUM_DEF_LEN 65
#define TO_NUM_DEF_DEC 30
class Item_func_to_number final : public Item_func {
 public:
  Item_func_to_number(const POS &pos, Item *a) : Item_func(pos, a) {
    set_data_type_decimal(TO_NUM_DEF_LEN, TO_NUM_DEF_DEC);
  }

  Item_func_to_number(const POS &pos, Item *a, Item *b) : Item_func(pos, a, b) {
    set_data_type_decimal(TO_NUM_DEF_LEN, TO_NUM_DEF_DEC);
  }

  String *val_str(String *str) override;
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal *) override;

  bool get_date(MYSQL_TIME *ltime, my_time_flags_t fuzzydate) override {
    return get_date_from_decimal(ltime, fuzzydate);
  }
  bool get_time(MYSQL_TIME *ltime) override {
    return get_time_from_decimal(ltime);
  }
  [[nodiscard]] enum Item_result result_type() const override {
    return DECIMAL_RESULT;
  }
  bool resolve_type(THD *) override { return false; }
  [[nodiscard]] const char *func_name() const override { return "to_number"; }

  bool is_decimal_value_null(my_decimal *tmp);
  bool is_str_value_null(String *pStr);
  bool check_two_params_null(String *fmt_str);
  my_decimal *get_decimal_value(my_decimal *tmp, my_decimal *dec);
  String *get_value_param_str(String *arg_str);
  String *get_format_param_str(String *arg_str);
  my_decimal *val_dec_one_param(my_decimal *);
  my_decimal *val_dec_two_params(my_decimal *);
  //在这种情况下：输入参数是表字段，如果一个字段的中间值为空，则 null_value
  //不会被重置，并且会出现我们不期望的输出。为了避免这种情况，我们需要重置。
  void reset_item() { null_value = false; }
};

#endif  // TO_NUMBER_H
