#include "sql/oracle_compatibility/ora_format.h"
#include "m_ctype.h"  // CHARSET_INFO



//根据关键词首字母快速匹配到对应的枚举值
const KeyWord *index_seq_search(const char *str) {
  int poz;

  if (!KeyWord_INDEX_FILTER(*str)) {
    return nullptr;
  }

  if ((poz = *(NUM_index + (*str - ' '))) > -1) {
    const KeyWord *k = NUM_keywords + poz;

    do {
      if (strncasecmp(str, k->name, k->len) == 0) {
        return k;
      }

      k++;
      if (!k->name) {
        return nullptr;
      }
    } while (*str == *k->name);
  }

  return nullptr;
}
