
#ifndef ORA_FORMAT_H
#define ORA_FORMAT_H

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ----------
 * Routines type
 * ----------
 */

// 判断字符是否位于 ' ' 和 '~' 之间
#define KeyWord_INDEX_FILTER(_c) ((_c) <= ' ' || (_c) >= '~' ? 0 : 1)

/* the max length of the format keyword in TO_DATE, TO_TIMESTAMP, TO_NUMBER,
 * normal character */
const size_t MAX_MULTIBYTE_CHAR_LEN = 4;

/* -------------------------------
 *   Common structures of format
 * -------------------------------
 */

enum class SuffixType { NONE = 0, PREFIX = 1, POSTFIX };

/* KeySuffix: prefix or postfix
 */
struct KeySuffix {
  const char *name; /* name of suffix        */
  size_t len;       /* length of suffix      */
  int id;           /* used in node->suffix  */
  SuffixType type;  /* prefix / postfix type */
};

/* DateFormatMode
 *
 * This value is used to nominate one of several distinct (and mutually
 * exclusive) date conventions that a keyword can belong to.
 */

typedef struct {
  const char *name; /* name of keyword */
  size_t len;       /* length of keyword */
  int id;           /* identifier, DTF_* or NUM_* */
  bool is_digit;    /* the value is digit or not */
} KeyWord;

enum class FormatNodeType {
  END = 1, /* end of format tree */
  ACTION,  /* action type means this format need process its value */
  //  CHAR,    /* normal character */
  /*
   * String enclosed in double quotes.
   * 双引号中的字符必须完全一样（除了大小写）
   * oracle 不支持下面语法
   * select to_date('ba2010', '"a-"yyyy');
   */
  //  STR
};

enum class NumBaseType {
  DEC = 10, /* Decimal:10-base type number */
  HEX = 16  /* Hexadecimal:16-base type number */
};

typedef struct {
  FormatNodeType type; /* NODE_TYPE_XXX, see below */
  const KeyWord *key;  /* if type is ACTION */
  //  char character[MAX_MULTIBYTE_CHAR_LEN + 1]; /* if type is CHAR */
  int suffix; /* keyword prefix/suffix code, if any */
  // int precision; /* for Keyword FF:how many bits in the microseconds */
} FormatNode;

/* ------------------------------------------------
 *   KeyWord definitions (DTF: Date time format)
 * ------------------------------------------------
 */

typedef enum {
  NUM_COMMA,  //,
  NUM_DEC,    //.
  NUM_0,      // 0
  NUM_9,      // 9
  NUM_D,      // D
  NUM_G,      // G
  NUM_L,      // L
  NUM_X,      // X
  NUM_d,      // d
  NUM_g,      // g
  NUM_l,      // l
  NUM_x,      // x
  /* last */
  NUM_last  // NULL
} NUM_poz;

/* ----------
 * KeyWords for NUMBER version
 *
 * The is_digit and date_mode fields are not relevant here.
 * ----------
 */
static const KeyWord NUM_keywords[] = {
    /* name, len, id     */
    {",", 1, NUM_COMMA, true}, /* , */
    {".", 1, NUM_DEC, true},   /* . */
    {"0", 1, NUM_0, true},     /* 0 */
    {"9", 1, NUM_9, true},     /* 9 */
    {"D", 1, NUM_D, true},     /* D */
    {"G", 1, NUM_G, true},     /* G */
    {"L", 1, NUM_L, true},     /* L */
    {"X", 1, NUM_X, true},     /* X */
    {"d", 1, NUM_D, true},     /* d */
    {"g", 1, NUM_G, true},     /* g */
    {"l", 1, NUM_L, true},     /* l */
    {"x", 1, NUM_X, true},     /* x */

    /* last */
    {nullptr, 0, NUM_last, true}};

/* ----------
 * KeyWords index for NUMBER version
 * ----------
 */
static const int NUM_index[] = {
    /*
    0    1    2    3    4    5    6    7    8    9
    */
    /*---- first 0..31 chars are skipped ----*/

    -1,        -1,    -1,      -1,    -1,    -1, -1, -1, -1,    -1, -1, -1,
    NUM_COMMA, -1,    NUM_DEC, -1,    NUM_0, -1, -1, -1, -1,    -1, -1, -1,
    -1,        NUM_9, -1,      -1,    -1,    -1, -1, -1, -1,    -1, -1, -1,
    NUM_D,     -1,    -1,      NUM_G, -1,    -1, -1, -1, NUM_L, -1, -1, -1,
    -1,        -1,    -1,      -1,    -1,    -1, -1, -1, NUM_X, -1, -1, -1,
    -1,        -1,    -1,      -1,    -1,    -1, -1, -1, NUM_d, -1, -1, NUM_g,
    -1,        -1,    -1,      -1,    NUM_l, -1, -1, -1, -1,    -1, -1, -1,
    -1,        -1,    -1,      -1,    NUM_x, -1, -1, -1, -1,    -1

    /*---- chars over 126 are skipped ----*/
};

/* ---------------------------------
 *   Functions
 * ---------------------------------
 */

extern const KeyWord *index_seq_search(const char *str);

#endif  // ORA_FORMAT_H
