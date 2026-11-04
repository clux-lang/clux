#ifndef _H_PARSER_LITERAL_
#define _H_PARSER_LITERAL_
#include "core/allocator.h"
#include "node.h"
#include "parser.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 各 literal 读取器：匹配成功则消费 token 并返回新节点，否则返回 NULL
   且不移动游标。调用方负责按优先级依次尝试（boolean 在 identifier 之前）。 */

node_t *parser_read_literal_nil(allocator_t *allocator,
                                parser_t *parser,
                                node_t *parent);
node_t *parser_read_literal_undefined(allocator_t *allocator,
                                      parser_t *parser,
                                      node_t *parent);
node_t *parser_read_literal_string(allocator_t *allocator,
                                   parser_t *parser,
                                   node_t *parent);
node_t *parser_read_literal_character(allocator_t *allocator,
                                      parser_t *parser,
                                      node_t *parent);
/* 数字后可紧跟可选类型后缀（i8..f64、isize/usize），中间不允许空白或注释。
   节点 location 覆盖数字与后缀的完整范围。 */
node_t *parser_read_literal_numeric(allocator_t *allocator,
                                    parser_t *parser,
                                    node_t *parent);
/* true / false（词法层识别为关键字）。 */
node_t *parser_read_literal_boolean(allocator_t *allocator,
                                    parser_t *parser,
                                    node_t *parent);
node_t *parser_read_literal_identifier(allocator_t *allocator,
                                       parser_t *parser,
                                       node_t *parent);
#ifdef __cplusplus
}
#endif
#endif
