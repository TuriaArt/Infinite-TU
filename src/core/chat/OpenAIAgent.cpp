#include "core/chat/OpenAIAgent.h"

#include "core/chat/LocalProtocol.h"
#include "core/I18n.h"
#include "platform/Platform.h"

#include <algorithm>
#include <cctype>

namespace chat
{
   namespace
   {
      std::string LowerAscii(std::string s)
      {
         for (char& c : s)
            c = (char)std::tolower((unsigned char)c);
         return s;
      }

      bool ContainsAscii(const std::string& hay, const char* needle)
      {
         // Case-insensitive on both sides: callers pass either raw platform strings (mixed case)
         // or lowercase fragments, and both must match.
         return LowerAscii(hay).find(LowerAscii(needle)) != std::string::npos;
      }
   }

   const std::vector<CustomPreset>& CustomPresets()
   {
      static const std::vector<CustomPreset> kPresets = {
         { "ollama", "Ollama (local)", "http://localhost:11434/v1", "qwen2.5-coder:7b" },
         { "deepseek", "DeepSeek", "https://api.deepseek.com/v1", "deepseek-chat" },
         { "openrouter", "OpenRouter", "https://openrouter.ai/api/v1", "anthropic/claude-sonnet-4" },
         { "custom", "Custom", "", "" },
      };
      return kPresets;
   }

   const CustomPreset* FindCustomPreset(const std::string& id)
   {
      for (const CustomPreset& p : CustomPresets())
         if (id == p.id)
            return &p;
      return nullptr;
   }

   std::string NormalizeOpenAIBaseUrl(const std::string& baseUrl)
   {
      std::string out = baseUrl;
      while (!out.empty() && std::isspace((unsigned char)out.front()))
         out.erase(out.begin());
      while (!out.empty() && (std::isspace((unsigned char)out.back()) || out.back() == '/'))
         out.pop_back();
      return out;
   }

   nlohmann::json OpenAIAgentTools()
   {
      auto fn = [](const char* name, const char* desc, nlohmann::json params) -> nlohmann::json {
         return {
            { "type", "function" },
            { "function", {
               { "name", name },
               { "description", desc },
               { "parameters", std::move(params) }
            } }
         };
      };
      return nlohmann::json::array({
         fn("propose",
            "Validate and atomically apply a batch of patch edits with a single undo step. "
            "All graph mutations (create_node, connect, disconnect, set_param, set_code, delete_node, etc.) "
            "must go through propose. Rolls back completely if any call or validation fails.",
            {
               { "type", "object" },
               { "properties", {
                  { "label", { { "type", "string" }, { "description", "Short description for the undo history." } } },
                  { "calls", {
                     { "type", "array" },
                     { "description", "Ordered list of RPC calls {method, params}. Later calls may reference $0.index etc." },
                     { "items", {
                        { "type", "object" },
                        { "properties", {
                           { "method", { { "type", "string" } } },
                           { "params", { { "type", "object" } } }
                        } },
                        { "required", nlohmann::json::array({ "method" }) }
                     } }
                  } }
               } },
               { "required", nlohmann::json::array({ "calls" }) }
            }),
         fn("check",
            "Check the outcome after a propose: does every node reach an Output or Audio Out, and has the patch "
            "sounded since the edit, free of NaN and clipping. propose only proves an edit is valid; call check "
            "after every propose that changes sound or wiring, and fix any E_ problem (and W_ ones that matter) in "
            "one more propose. If sound.settling is true, call check once more.",
            {
               { "type", "object" },
               { "properties", {
                  { "reset", { { "type", "boolean" }, { "description", "Restart the listening window without an edit." } } }
               } }
            }),
         fn("get_graph",
            "Inspect all nodes and connections in the current patch.",
            { { "type", "object" }, { "properties", nlohmann::json::object() } }),
         fn("get_params",
            "Inspect parameter keys, values, and ranges on a node.",
            {
               { "type", "object" },
               { "properties", {
                  { "index", { { "type", "integer" }, { "description", "Node index." } } },
                  { "full", { { "type", "boolean" } } }
               } },
               { "required", nlohmann::json::array({ "index" }) }
            }),
         fn("list_node_types",
            "List all available node types and categories.",
            { { "type", "object" }, { "properties", nlohmann::json::object() } }),
         fn("describe",
            "Describe pins, parameters, and usage of a node type.",
            {
               { "type", "object" },
               { "properties", {
                  { "typeName", { { "type", "string" } } }
               } },
               { "required", nlohmann::json::array({ "typeName" }) }
            }),
         fn("code_errors",
            "Check shader or code compile errors in the patch or on a specific node.",
            {
               { "type", "object" },
               { "properties", {
                  { "index", { { "type", "integer" } } }
               } }
            }),
         fn("status",
            "Inspect transport playback, FPS, and patch status.",
            { { "type", "object" }, { "properties", nlohmann::json::object() } }),
         fn("attention",
            "Inspect what the user is pointing at: selected nodes, last touched control, hovered control, keyboard focus, and open panels.",
            { { "type", "object" }, { "properties", nlohmann::json::object() } }),
         fn("selection",
            "List the currently selected nodes on the canvas.",
            { { "type", "object" }, { "properties", nlohmann::json::object() } }),
         fn("explain",
            "Explain the open patch: nodes, non-default controls, wires, modulation ranges, and geometry bounds.",
            {
               { "type", "object" },
               { "properties", {
                  { "all", { { "type", "boolean" }, { "description", "Include default parameter values too." } } }
               } }
            }),
         fn("panel",
            "Query all panels and their open/dock state, or open/close/dock a panel (e.g. mixer, drums, view2d, view3d, fieldpatcher, curveeditor, timeline, session, samplestudio, pianoroll, perf, modmatrix, viewport, terminal, settings/<place>).",
            {
               { "type", "object" },
               { "properties", {
                  { "name", { { "type", "string" } } },
                  { "open", { { "type", "boolean" } } },
                  { "dock", { { "type", "string" }, { "description", "bottom, right, left, top, or fill (for view2d/view3d)." } } }
               } }
            }),
         fn("view2d",
            "Inspect or edit 2D View (Scene 2D) layers, shapes, text, paint, and effects. Pass op='layers' or op='fields' to inspect, or op='batch' with calls.",
            {
               { "type", "object" },
               { "properties", {
                  { "op", { { "type", "string" } } },
                  { "calls", { { "type", "array" }, { "items", { { "type", "object" } } } } }
               } }
            }),
         fn("settings",
            "Read or update app settings (pass key, and optionally value).",
            {
               { "type", "object" },
               { "properties", {
                  { "key", { { "type", "string" } } },
                  { "value", {} }
               } }
            }),
      });
   }

   void OpenAIStreamParser::Feed(const std::string& chunk)
   {
      mBuffer += chunk;
      size_t pos = 0;
      while (true)
      {
         size_t nl = mBuffer.find('\n', pos);
         if (nl == std::string::npos)
            break;
         std::string line = mBuffer.substr(pos, nl - pos);
         if (!line.empty() && line.back() == '\r')
            line.pop_back();
         ProcessLine(line);
         pos = nl + 1;
      }
      if (pos > 0)
         mBuffer.erase(0, pos);
   }

   void OpenAIStreamParser::Finish()
   {
      if (!mBuffer.empty())
      {
         std::string line = std::move(mBuffer);
         mBuffer.clear();
         if (!line.empty() && line.back() == '\r')
            line.pop_back();
         ProcessLine(line);
      }
   }

   void OpenAIStreamParser::ProcessLine(const std::string& line)
   {
      if (line.empty() || line[0] == ':')
         return;
      std::string payload = line;
      if (payload.rfind("data:", 0) == 0)
      {
         payload.erase(0, 5);
         while (!payload.empty() && payload.front() == ' ')
            payload.erase(payload.begin());
      }
      else
      {
         return;
      }
      if (payload == "[DONE]")
      {
         mDone = true;
         return;
      }
      auto j = nlohmann::json::parse(payload, nullptr, false);
      if (j.is_discarded() || !j.is_object())
         return;

      if (j.contains("usage") && j["usage"].is_object())
      {
         const auto& u = j["usage"];
         if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number_integer())
            mUsage.input = u["prompt_tokens"].get<long long>();
         if (u.contains("completion_tokens") && u["completion_tokens"].is_number_integer())
            mUsage.output = u["completion_tokens"].get<long long>();
         if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object())
         {
            const auto& d = u["prompt_tokens_details"];
            if (d.contains("cached_tokens") && d["cached_tokens"].is_number_integer())
               mUsage.cached = d["cached_tokens"].get<long long>();
         }
      }

      if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty())
         return;
      const auto& choice = j["choices"][0];
      const nlohmann::json* msg = nullptr;
      if (choice.contains("delta") && choice["delta"].is_object())
         msg = &choice["delta"];
      else if (choice.contains("message") && choice["message"].is_object())
         msg = &choice["message"];
      if (msg == nullptr)
         return;

      if (msg->contains("content") && msg->at("content").is_string())
         mText += msg->at("content").get<std::string>();

      if (msg->contains("tool_calls") && msg->at("tool_calls").is_array())
      {
         for (const auto& tc : msg->at("tool_calls"))
         {
            if (!tc.is_object())
               continue;
            const size_t idx = tc.contains("index") && tc["index"].is_number_unsigned()
                                  ? tc["index"].get<size_t>()
                                  : mToolCalls.size();
            if (mToolCalls.size() <= idx)
               mToolCalls.resize(idx + 1);
            OpenAIToolCall& dst = mToolCalls[idx];
            if (tc.contains("id") && tc["id"].is_string() && !tc["id"].get<std::string>().empty())
               dst.id = tc["id"].get<std::string>();
            if (tc.contains("function") && tc["function"].is_object())
            {
               const auto& fn = tc["function"];
               if (fn.contains("name") && fn["name"].is_string())
                  dst.name += fn["name"].get<std::string>();
               if (fn.contains("arguments") && fn["arguments"].is_string())
                  dst.arguments += fn["arguments"].get<std::string>();
            }
         }
      }
   }

   bool IsToolsUnsupportedError(int status, const std::string& errorBody)
   {
      if (status != 400 && status != 422 && status != 404)
         return false;
      return ContainsAscii(errorBody, "tool") || ContainsAscii(errorBody, "function");
   }

   bool TestOpenAIConnection(const std::string& baseUrl, const std::string& model,
                             const std::string& apiKey, std::string& outMessage)
   {
      const std::string base = NormalizeOpenAIBaseUrl(baseUrl);
      if (base.empty())
      {
         outMessage = "Base URL is empty.";
         return false;
      }
      if (model.empty())
      {
         outMessage = "Model ID is empty.";
         return false;
      }
      const nlohmann::json body = {
         { "model", model },
         { "messages", nlohmann::json::array({ { { "role", "user" }, { "content", "Reply with OK." } } }) },
         { "max_tokens", 4 },
         { "stream", false }
      };
      int status = 0;
      std::string errBody, netErr, raw;
      const bool ok = Platform::HttpPostStream(
         base + "/chat/completions", body.dump(), apiKey,
         [&](const std::string& chunk) {
            raw += chunk;
            return true;
         },
         status, errBody, netErr, kOpenAIRequestTimeoutSec);
      if (!ok)
      {
         if (!netErr.empty())
            outMessage = DescribeHttpError(ClassifyHttpError(netErr), netErr);
         else if (!errBody.empty())
            outMessage = "HTTP " + std::to_string(status) + ": " + errBody.substr(0, 180);
         else
            outMessage = "HTTP " + std::to_string(status);
         return false;
      }
      outMessage = "Connected (HTTP " + std::to_string(status) + ").";
      return true;
   }

   std::vector<std::string> ParseOpenAIModels(const std::string& text)
   {
      std::vector<std::string> out;
      const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
      if (!j.is_object() || !j.contains("data") || !j["data"].is_array())
         return out;
      for (const nlohmann::json& m : j["data"])
      {
         if (!m.is_object())
            continue;
         const std::string id = m.value("id", std::string());
         if (!id.empty())
            out.push_back(id);
      }
      return out;
   }

   HttpErrorKind ClassifyHttpError(const std::string& rawError)
   {
      if (rawError.empty())
         return HttpErrorKind::kNone;
      // Patterns cover both the WinHTTP paths (PlatformWin.cpp) and the curl paths
      // (PlatformLinux.cpp / Platform.mm): "WinHttpConnect failed", "WinHTTP error 12029",
      // "Connection refused", "curl error 7", and so on.
      if (ContainsAscii(rawError, "WinHttpConnect failed") || ContainsAscii(rawError, "WinHTTP error 12029") ||
          ContainsAscii(rawError, "connection refused") || ContainsAscii(rawError, "curl error 7") ||
          ContainsAscii(rawError, "failed to connect"))
         return HttpErrorKind::kConnectionRefused;
      if (ContainsAscii(rawError, "WinHTTP error 12005") || ContainsAscii(rawError, "timed out") ||
          ContainsAscii(rawError, "timeout") || ContainsAscii(rawError, "curl error 28"))
         return HttpErrorKind::kTimeout;
      if (ContainsAscii(rawError, "WinHTTP error 12027") || ContainsAscii(rawError, "security channel") ||
          ContainsAscii(rawError, "tls") || ContainsAscii(rawError, "ssl") || ContainsAscii(rawError, "certificate"))
         return HttpErrorKind::kTLS;
      if (ContainsAscii(rawError, "http status"))
         return HttpErrorKind::kHTTPStatus;
      return HttpErrorKind::kOther;
   }

   std::string DescribeHttpError(HttpErrorKind kind, const std::string& rawError)
   {
      std::string msg;
      switch (kind)
      {
      case HttpErrorKind::kConnectionRefused:
         msg = I18n::T("Connection refused. The server is not running, or is on another host/port.");
         break;
      case HttpErrorKind::kTimeout:
         msg = I18n::T("The request timed out.");
         break;
      case HttpErrorKind::kTLS:
         msg = I18n::T("TLS/certificate error while connecting.");
         break;
      case HttpErrorKind::kHTTPStatus:
         msg = I18n::T("The server answered with an error status.");
         break;
      default:
         msg = I18n::T("The request failed.");
         break;
      }
      // Keep the platform's original string visible so the diagnosis stays debuggable.
      if (!rawError.empty())
         msg += " (" + rawError + ")";
      return msg;
   }

   bool ListOpenAIModels(const std::string& baseUrl, const std::string& apiKey,
                         std::vector<std::string>& outModels, std::string& outMessage)
   {
      outModels.clear();
      const std::string base = NormalizeOpenAIBaseUrl(baseUrl);
      if (base.empty())
      {
         outMessage = "Base URL is empty.";
         return false;
      }
      std::string body, httpErr;
      if (!Platform::HttpGet(base + "/models", "Infinite", body, httpErr, kOpenAIRequestTimeoutSec, apiKey))
      {
         outMessage = httpErr.empty() ? I18n::T("Model list request failed.")
                                      : DescribeHttpError(ClassifyHttpError(httpErr), httpErr);
         return false;
      }
      outModels = ParseOpenAIModels(body);
      if (outModels.empty())
      {
         outMessage = "The endpoint lists no models.";
         return false;
      }
      outMessage = "Found " + std::to_string(outModels.size()) + " model(s).";
      return true;
   }

   HttpPostStreamHook gHttpHook;

   void SetHttpPostStreamHook(HttpPostStreamHook hook)
   {
      gHttpHook = std::move(hook);
   }

   std::shared_ptr<OpenAIAgentRun> OpenAIAgentRun::Start(const Options& options)
   {
      auto ptr = std::shared_ptr<OpenAIAgentRun>(new OpenAIAgentRun(options));
      ptr->mThread = std::thread([ptr]() { ptr->RunThread(); });
      return ptr;
   }

   OpenAIAgentRun::OpenAIAgentRun(Options options)
      : mOptions(std::move(options))
   {
   }

   OpenAIAgentRun::~OpenAIAgentRun()
   {
      Stop();
      if (mThread.joinable())
         mThread.join();
   }

   void OpenAIAgentRun::Stop()
   {
      mStop.store(true);
      {
         std::lock_guard<std::mutex> lock(mToolMutex);
         mPendingTool.completed = true;
      }
      mToolCv.notify_all();
   }

   void OpenAIAgentRun::PushEvent(AgentEvent ev)
   {
      std::lock_guard<std::mutex> lock(mEventMutex);
      mEvents.push_back(std::move(ev));
   }

   void OpenAIAgentRun::Poll(std::vector<AgentEvent>& out, const ToolExecutor& exec)
   {
      {
         std::lock_guard<std::mutex> lock(mToolMutex);
         if (mPendingTool.active && !mPendingTool.completed)
         {
            nlohmann::json res;
            std::string err;
            const bool ok = exec ? exec(mPendingTool.method, mPendingTool.params, res, err) : false;
            mPendingTool.ok = ok;
            mPendingTool.result = std::move(res);
            mPendingTool.error = std::move(err);
            mPendingTool.completed = true;
            mToolCv.notify_all();
         }
      }
      std::lock_guard<std::mutex> lock(mEventMutex);
      for (auto& e : mEvents)
         out.push_back(std::move(e));
      mEvents.clear();
   }

   nlohmann::json OpenAIAgentRun::ExecuteToolOnMain(const std::string& name, const std::string& argsJson)
   {
      nlohmann::json params = nlohmann::json::object();
      if (!argsJson.empty())
      {
         auto parsed = nlohmann::json::parse(argsJson, nullptr, false);
         if (parsed.is_object())
            params = std::move(parsed);
      }

      {
         std::lock_guard<std::mutex> lock(mToolMutex);
         mPendingTool = PendingTool{};
         mPendingTool.active = true;
         mPendingTool.method = name;
         mPendingTool.params = params;
      }

      std::unique_lock<std::mutex> lock(mToolMutex);
      mToolCv.wait(lock, [&]() { return mStop.load() || mPendingTool.completed; });
      const bool ok = !mStop.load() && mPendingTool.ok;
      const nlohmann::json res = mPendingTool.result;
      const std::string err = mPendingTool.error;
      mPendingTool = PendingTool{};
      lock.unlock();

      AgentEvent tr;
      tr.kind = AgentEvent::Kind::ToolResult;
      tr.error = !ok;
      tr.text = ok ? res.dump() : err;
      PushEvent(std::move(tr));

      if (!ok)
         return nlohmann::json{ { "ok", false }, { "error", err.empty() ? "Tool call failed" : err } };
      return res;
   }

   void OpenAIAgentRun::RunThread()
   {
      const std::string base = NormalizeOpenAIBaseUrl(mOptions.baseUrl);
      if (base.empty() || mOptions.model.empty())
      {
         AgentEvent end;
         end.kind = AgentEvent::Kind::End;
         end.error = true;
         end.text = base.empty() ? "Custom agent base URL is not set." : "Custom agent model is not set.";
         PushEvent(std::move(end));
         mDone.store(true);
         return;
      }

      nlohmann::json messages = nlohmann::json::array();
      // The pack's prompt wins; the plain chat briefing is the fallback when none is set.
      const std::string systemPrompt = !mOptions.harness.systemPrompt.empty() ? mOptions.harness.systemPrompt : mOptions.systemPrompt;
      if (!systemPrompt.empty())
         messages.push_back({ { "role", "system" }, { "content", systemPrompt } });
      for (const auto& [role, content] : mOptions.history)
         messages.push_back({ { "role", role }, { "content", content } });
      messages.push_back({ { "role", "user" }, { "content", mOptions.userPrompt } });

      const HarnessConfig& cfg = mOptions.harness;
      const bool protocolMode = cfg.mode == HarnessMode::Protocol;
      bool useTools = !protocolMode;
      TurnUsage totalUsage;
      std::string finalError;
      bool hadError = false;
      int parseErrors = 0;
      const int maxSteps = std::max(1, mOptions.maxSteps);

      for (int step = 0; step < maxSteps && !mStop.load(); ++step)
      {
         nlohmann::json req = {
            { "model", mOptions.model },
            { "messages", messages },
            { "stream", true }
         };
         if (useTools)
         {
            // The pack's tool subset (a weak model picks the right tool more often with fewer).
            nlohmann::json tools = OpenAIAgentTools();
            if (!cfg.tools.empty())
            {
               nlohmann::json subset = nlohmann::json::array();
               for (const nlohmann::json& t : tools)
                  if (t.is_object() && t.contains("function") && t["function"].is_object() &&
                      t["function"].contains("name") &&
                      std::find(cfg.tools.begin(), cfg.tools.end(), t["function"]["name"].get<std::string>()) != cfg.tools.end())
                    subset.push_back(t);
               tools = std::move(subset);
            }
            req["tools"] = std::move(tools);
         }

         OpenAIStreamParser parser;
         int status = 0;
         std::string errBody, netErr;
         auto feed = [&](const std::string& chunk) {
            if (mStop.load())
               return false;
            parser.Feed(chunk);
            return true;
         };
         const bool ok = gHttpHook
            ? gHttpHook(base + "/chat/completions", req.dump(), mOptions.apiKey, feed, status, errBody, netErr, 120)
            : Platform::HttpPostStream(base + "/chat/completions", req.dump(), mOptions.apiKey, feed, status, errBody, netErr, 120);
         parser.Finish();

         if (!ok)
         {
            if (useTools && IsToolsUnsupportedError(status, errBody))
            {
               useTools = false;
               --step;
               continue;
            }
            hadError = true;
            if (!netErr.empty())
               finalError = netErr;
            else if (!errBody.empty())
               finalError = "HTTP " + std::to_string(status) + ": " + errBody.substr(0, 240);
            else
               finalError = "HTTP " + std::to_string(status);
            break;
         }

         totalUsage.input += parser.Usage().input;
         totalUsage.cached += parser.Usage().cached;
         totalUsage.output += parser.Usage().output;

         if (protocolMode)
         {
            // Protocol mode: the model answers in plain text with tag lines. The parser is
            // deterministic; every action goes through the same propose gate as native tools.
            // Results come back as user messages so any OpenAI-compatible endpoint accepts them.
            const ProtocolReply pr = ParseProtocol(parser.Text());
            if (!pr.text.empty())
            {
               AgentEvent tev;
               tev.kind = AgentEvent::Kind::Text;
               tev.text = pr.text;
               PushEvent(std::move(tev));
            }
            for (const std::string& tag : pr.unknownTags)
            {
               AgentEvent tr;
               tr.kind = AgentEvent::Kind::ToolResult;
               tr.text = "ignored unknown tag: " + tag;
               PushEvent(std::move(tr));
            }
            if (!pr.hasAction)
               break; // a text-only reply: the turn is done

            const ProtocolBlock& a = pr.action;
            std::string method;
            if (a.kind == ProtocolKind::Propose)
               method = "propose";
            else if (a.kind == ProtocolKind::Check)
               method = "check";
            else
               method = a.json.value("name", std::string());
            AgentEvent te;
            te.kind = AgentEvent::Kind::Tool;
            te.text = method.empty() ? a.tag : method;
            PushEvent(std::move(te));

            if (!a.validJson || method.empty())
            {
               const std::string err = method.empty() ? "the name of the tool to call is missing"
                                                       : (a.err.empty() ? "the action block is not a JSON object" : a.err);
               ++parseErrors;
               AgentEvent tr;
               tr.kind = AgentEvent::Kind::ToolResult;
               tr.error = true;
               tr.text = a.tag + ": " + err;
               PushEvent(std::move(tr));
               messages.push_back({
                  { "role", "user" },
                  { "content", "Your " + a.tag + " block could not be used: " + err + ". Reply with one corrected " + a.tag + " block." }
               });
               if (parseErrors > cfg.maxParseErrors)
               {
                  hadError = true;
                  finalError = "The model kept sending unusable command blocks.";
                  break;
               }
               continue;
            }
            parseErrors = 0;

            // The TOOL: block is read-only by design; an edit method in it is unusable, the same
            // way as a broken JSON block - the model gets one correction attempt.
            if (a.kind == ProtocolKind::Tool && !IsReadOnlyToolName(method))
            {
               const std::string err = "the TOOL: block can only call a read-only tool (get_graph, get_params, "
                                       "list_node_types, describe, code_errors, status, attention, selection); "
                                       "edit methods go in a PROPOSE";
               ++parseErrors;
               AgentEvent tr;
               tr.kind = AgentEvent::Kind::ToolResult;
               tr.error = true;
               tr.text = a.tag + ": " + err;
               PushEvent(std::move(tr));
               messages.push_back({
                  { "role", "user" },
                  { "content", "Your " + a.tag + " block could not be used: " + err + ". Reply with one corrected " + a.tag + " block." }
               });
               if (parseErrors > cfg.maxParseErrors)
               {
                  hadError = true;
                  finalError = "The model kept sending unusable command blocks.";
                  break;
               }
               continue;
            }

            const nlohmann::json result = ExecuteToolOnMain(method, a.raw);
            if (method == "propose" && result.is_object() && result.value("ok", false) == false)
            {
               // A rejected propose (e.g. a $N reference with no earlier call, or a failed call):
               // tell the model what went wrong so it can answer with one corrected PROPOSE -
               // bounded by the same budget as unparseable blocks (max 2 correction attempts).
               const std::string err = result.value("error", std::string());
               ++parseErrors;
               AgentEvent tr;
               tr.kind = AgentEvent::Kind::ToolResult;
               tr.error = true;
               tr.text = "propose rejected: " + err;
               PushEvent(std::move(tr));
               messages.push_back({
                  { "role", "user" },
                  { "content", "Your propose was rejected: " + err + ". Reply with one corrected PROPOSE block." }
               });
               if (parseErrors > cfg.maxParseErrors)
               {
                  hadError = true;
                  finalError = "The model kept sending unusable command blocks.";
                  break;
               }
               continue;
            }
            messages.push_back({
               { "role", "user" },
               { "content", "Result of your " + method + " call: " + result.dump() }
            });
            if (pr.multipleActions)
               messages.push_back({ { "role", "user" }, { "content", "Only one action block per turn: the extra block was ignored." } });
            continue;
         }

         if (!parser.Text().empty())
         {
            AgentEvent tev;
            tev.kind = AgentEvent::Kind::Text;
            tev.text = parser.Text();
            PushEvent(std::move(tev));
         }

         const auto& toolCalls = parser.ToolCalls();
         if (toolCalls.empty())
            break;

         nlohmann::json assistantMsg = {
            { "role", "assistant" },
            { "content", parser.Text().empty() ? nlohmann::json(nullptr) : nlohmann::json(parser.Text()) },
            { "tool_calls", nlohmann::json::array() }
         };
         for (size_t i = 0; i < toolCalls.size(); ++i)
         {
            const std::string tcId = toolCalls[i].id.empty() ? ("call_" + std::to_string(step) + "_" + std::to_string(i)) : toolCalls[i].id;
            assistantMsg["tool_calls"].push_back({
               { "id", tcId },
               { "type", "function" },
               { "function", {
                  { "name", toolCalls[i].name },
                  { "arguments", toolCalls[i].arguments.empty() ? "{}" : toolCalls[i].arguments }
               } }
            });
         }
         messages.push_back(std::move(assistantMsg));

         for (size_t i = 0; i < toolCalls.size() && !mStop.load(); ++i)
         {
            const std::string tcId = toolCalls[i].id.empty() ? ("call_" + std::to_string(step) + "_" + std::to_string(i)) : toolCalls[i].id;
            AgentEvent te;
            te.kind = AgentEvent::Kind::Tool;
            te.text = toolCalls[i].name;
            PushEvent(std::move(te));

            const nlohmann::json result = ExecuteToolOnMain(toolCalls[i].name, toolCalls[i].arguments);
            messages.push_back({
               { "role", "tool" },
               { "tool_call_id", tcId },
               { "content", result.dump() }
            });
         }
      }

      AgentEvent end;
      end.kind = AgentEvent::Kind::End;
      end.error = hadError;
      end.text = finalError;
      end.usage = totalUsage;
      PushEvent(std::move(end));
      mDone.store(true);
   }
}
