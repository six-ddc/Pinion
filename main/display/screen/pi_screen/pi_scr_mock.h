/* pi-c — MetalioClaw5 pi_screen App: two-turn Anthropic Messages SSE mock
 * script (blueprint §3e / §2 "pi_scr_mock.h"). turn1 = thinking -> tool_use
 * (calc 37*89); turn2 = markdown answer exercising the full lv_markdown
 * subset (H1-3, both list kinds, fenced code with literal '#' + Chinese
 * comment, inline bold/code/link, quote, rule, literal "C#"), with the bold
 * run and the fence marker deliberately split across text_delta chunks to
 * drive the streaming tail re-render. Consumed by pi_agent_task.c:
 *   g_responses[] = {{200, PI_MOCK_TURN1}, {200, PI_MOCK_TURN2}};
 *   pi_mock_init(&g_mock, g_responses, 2, 24);
 * SPDX-License-Identifier: MIT */
#ifndef PI_SCR_MOCK_H
#define PI_SCR_MOCK_H

#define PI_MOCK_TURN1                                                                             \
    "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_"    \
    "tokens\":214}}}\n\n"                                                                          \
    "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_"   \
    "block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"                                       \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"  \
    "\"type\":\"thinking_delta\",\"thinking\":\"\\u7528\\u6237\\u8981\\u7b97 37x89 "                \
    "\\u518d\\u5b58\\u5907\\u5fd8\\u3002\"}}\n\n"                                                   \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"  \
    "\"type\":\"signature_delta\",\"signature\":\"sig\"}}\n\n"                                      \
    "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"           \
    "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,\"content_"   \
    "block\":{\"type\":\"tool_use\",\"id\":\"toolu_calc\",\"name\":\"calc\",\"input\":{}}}\n\n"    \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{"  \
    "\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"a\\\":37,\"}}\n\n"                      \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{"  \
    "\"type\":\"input_json_delta\",\"partial_json\":\"\\\"b\\\":89,\\\"op\\\":\\\"mul\\\"}\"}}\n\n" \
    "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n"           \
    "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_"   \
    "use\"},\"usage\":{\"output_tokens\":30}}\n\n"                                                  \
    "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n"

#define PI_MOCK_TURN2                                                                              \
    "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_"     \
    "tokens\":260}}}\n\n"                                                                           \
    "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_"    \
    "block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"                                                \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"   \
    "\"type\":\"text_delta\",\"text\":\"# 结果报告\\n37 × 89 = **32\"}}\n\n"                         \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"   \
    "\"type\":\"text_delta\",\"text\":\"93**，已存进备忘「计算结果」。\\n\\n## 实现细节\\n"          \
    "用的是 `ca\"}}\n\n"                                                                             \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"   \
    "\"type\":\"text_delta\",\"text\":\"lc` 工具，参考 [文档](https://example.com/calc)。\\n\\n"     \
    "### 注意事项\\n- 字面井号：C# 和 #tag 不应变色\\n- **粗体**与`行内代码`混排\\n\"}}\n\n"           \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"   \
    "\"type\":\"text_delta\",\"text\":\"- [x] 已接入 mock\\n- [ ] 待办：真机验证\\n\\n"              \
    "#### 四级标题\\n*斜体强调*与~~已废弃~~混排\\n\\n| 引脚 | 功能 |\\n|---|---|\\n"                  \
    "| 50 | `CMD` |\\n| 51 | CLK |\\n\\n\"}}\n\n"                                                    \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"   \
    "\"type\":\"text_delta\",\"text\":\"1. 第一步\\n2. 第二步\\n\\n> 引用：精度已核对过。\\n\\n"      \
    "--\"}}\n\n"                                                                                     \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"   \
    "\"type\":\"text_delta\",\"text\":\"-\\n\\n``\"}}\n\n"                                           \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"   \
    "\"type\":\"text_delta\",\"text\":\"`c\\n#include <stdio.h>\\nint main() {\\n"                   \
    "    printf(\\\"3293\\\\n\\\");  // 中文注释触发字体回退\\n    return 0;\\n}\\n```\\n\\n"          \
    "还需要我算别的吗？\"}}\n\n"                                                                      \
    "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"            \
    "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_"     \
    "turn\"},\"usage\":{\"output_tokens\":96}}\n\n"                                                  \
    "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n"

#endif /* PI_SCR_MOCK_H */
