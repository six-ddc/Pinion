// device_config_models.h — 内置 models 目录模板（**不含密钥**，可入库）。
//
// pi-c 的 models.json schema：
//   {"providers":{<name>:{baseUrl,api,apiKey,models:[{id,name,contextWindow,
//     maxTokens,input,reasoning,cost,compat}]}}}
// provider 级 baseUrl/api/apiKey 是该 provider 下所有 model 的默认值。
//
// 常规配置模式：device_config::BuildModelsJson() 把用户在 Web 后台填的 API Key
// （及可选的 baseUrl 覆盖）注入本模板的**第一个 provider**，产出交给 pi_models_load。
// 想换供应商 / 加模型走后台的「高级」模式，粘一整份 JSON 存 NVS 覆盖本模板。
//
// 单行（无换行）不是必需的——这份只在内存里被 cJSON 解析，不进 NVS。

#pragma once

namespace device_config {

inline const char* ModelsTemplateJson() {
    return R"JSON({"providers":{"deepseek":{
  "baseUrl":"https://api.deepseek.com",
  "api":"openai-completions",
  "apiKey":"",
  "models":[
    {"id":"deepseek-v4-pro","name":"DeepSeek V4 Pro","contextWindow":1000000,
     "maxTokens":384000,"input":["text"],"reasoning":true,
     "cost":{"input":1.74,"output":3.48,"cacheRead":0.145,"cacheWrite":0},
     "compat":{"requiresReasoningContentOnAssistantMessages":true,"thinkingFormat":"deepseek"}},
    {"id":"deepseek-v4-flash","name":"DeepSeek V4 Flash","contextWindow":1000000,
     "maxTokens":384000,"input":["text"],"reasoning":true,
     "cost":{"input":0.14,"output":0.28,"cacheRead":0.028,"cacheWrite":0},
     "compat":{"requiresReasoningContentOnAssistantMessages":true,"thinkingFormat":"deepseek"}}
  ]}}})JSON";
}

}  // namespace device_config
