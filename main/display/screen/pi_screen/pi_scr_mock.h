/* pi-c — MetalioClaw5 pi_screen App: two-turn Anthropic Messages SSE mock
 * script (blueprint §3e / §2 "pi_scr_mock.h"). turn1 = thinking -> tool_use
 * (calc 37*89); turn2 = text (answer, split into several text_delta chunks
 * to drive incremental UI append). Consumed by pi_agent_task.c:
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
    "\"type\":\"text_delta\",\"text\":\"37 \\u00d7 89 = 3293\\uff0c\"}}\n\n"                         \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"   \
    "\"type\":\"text_delta\",\"text\":\"\\u5df2\\u7ecf\\u5e2e\\u4f60\\u5b58\\u8fdb\\u5907\\u5fd8\"}" \
    "}\n\n"                                                                                          \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"   \
    "\"type\":\"text_delta\",\"text\":\"\\u300c\\u8ba1\\u7b97\\u7ed3\\u679c\\u300d\\u3002\"}}\n\n"  \
    "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"   \
    "\"type\":\"text_delta\",\"text\":\"\\u8fd8\\u9700\\u8981\\u6211\\u7b97\\u522b\\u7684\\u5417"   \
    "\\uff1f\"}}\n\n"                                                                                \
    "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"            \
    "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_"     \
    "turn\"},\"usage\":{\"output_tokens\":96}}\n\n"                                                  \
    "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n"

#endif /* PI_SCR_MOCK_H */
