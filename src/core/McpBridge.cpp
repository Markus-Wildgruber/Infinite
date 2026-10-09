#include "McpBridge.h"

#include "Base64.h"
#include "json.hpp"
#include "platform/AppPaths.h"
#include "platform/NetCompat.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
   #include <fcntl.h>
   #include <io.h>
#endif

#ifndef INFINITE_VERSION_STRING
#define INFINITE_VERSION_STRING "0.0.0"
#endif

namespace McpBridge
{
   namespace
   {
      using json = nlohmann::json;

      // Read by the client once, at initialize: how to work with the tools as a
      // whole. Per-tool rules live in each tool's description.
      const char* kInstructions =
         "These tools edit the node patch open in the running Infinite app while the user watches. "
         "Work in this order: (1) get_graph to see what is already there - never rebuild what exists; "
         "(2) describe every node type before using it - never guess type names, slot names, outputs or "
         "parameter keys; (3) make changes with batch so each request is one undo step and fails as a whole; "
         "(4) check the result with get_graph / get_bindings and, for pictures, render_frame. "
         "Node indices change after undo, redo or a failed batch: re-read get_graph then. "
         "An image only shows if it reaches an Output node; sound only if it reaches Audio Out or an Output's audio input. "
         "Prefer modulators (LFO, Envelope, Audio Analyze, ...) bound with bind_modulator over per-frame expressions "
         "when the user asks for movement. Keep changes minimal and explain what you changed.";

      struct Tool
      {
         const char* name;
         const char* description;
         const char* schema; // JSON Schema of the arguments
      };

      // Each tool is one RemoteControl method of the same name (rpc.md), except
      // get_graph (trimmed here) and render_frame (returns the image itself).
      const Tool kTools[] = {
         { "list_node_types", "All node types by category. Use describe for one type's details.",
           R"({"type":"object","properties":{}})" },
         { "describe",
           "One node type's schema: params (saved keys, kinds, defaults), inputs (slot number, slot name, kind) and "
           "outputs (label, kind, whether it is a modulator). Call before creating or wiring a type.",
           R"({"type":"object","properties":{"type":{"type":"string","description":"Node type name, e.g. Shape"}},"required":["type"]})" },
         { "get_graph",
           "The current patch, compact: nodes (index, type, category, bypassed), cables (image/geometry/audio/notes), "
           "modulation, expressions and globals. Parameter values are not included - use get_params for one node.",
           R"({"type":"object","properties":{}})" },
         { "get_params", "Every saved parameter value of one node, as {\"<kind> <key>\": value}.",
           R"({"type":"object","properties":{"index":{"type":"integer"}},"required":["index"]})" },
         { "get_bindings",
           "Modulation bindings, expressions and globals with parameter keys and ranges. With index, also lists that "
           "node's modulatable parameters (number, key, label, min, max) - only known once the node has drawn a frame.",
           R"({"type":"object","properties":{"index":{"type":"integer"}}})" },
         { "explain", "The patch as a readable signal-flow description, as `Infinite --explain` prints it.",
           R"({"type":"object","properties":{"all":{"type":"boolean"}}})" },
         { "create_node",
           "Create one node. Returns its index. Category is looked up when omitted.",
           R"({"type":"object","properties":{"typeName":{"type":"string"},"category":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"}},"required":["typeName"]})" },
         { "delete_node", "Delete a node with its cables and bindings.",
           R"({"type":"object","properties":{"index":{"type":"integer"}},"required":["index"]})" },
         { "connect",
           "Patch a cable from src's output into dst's input slot. srcOutput and dstSlot take a number or the name "
           "describe lists (dstSlot: inputs[].name like \"input_2\"; srcOutput: outputs[].label). The same rules as "
           "dragging a cable apply.",
           R"({"type":"object","properties":{"src":{"type":["integer","string"]},"dst":{"type":["integer","string"]},"srcOutput":{"type":["integer","string"]},"dstSlot":{"type":["integer","string"]}},"required":["src","dst","dstSlot"]})" },
         { "disconnect", "Remove a cable by its link id (the id field of a get_graph cable; it appears once the canvas has drawn).",
           R"({"type":"object","properties":{"linkId":{"type":"integer"}},"required":["linkId"]})" },
         { "set_param",
           "Set one saved parameter by its key (from describe / get_params). Dropdowns take the option value as saved.",
           R"({"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"value":{}},"required":["index","name","value"]})" },
         { "bind_modulator",
           "Drive a parameter from a modulator output (LFO, Envelope, Random, Macro, Audio Analyze bands, ...). param is "
           "the parameter number, key or label (get_bindings with index lists them). lo/hi: the swing in the "
           "parameter's own units, both or neither (neither = its full range).",
           R"({"type":"object","properties":{"dst":{"type":["integer","string"]},"param":{"type":["integer","string"]},"src":{"type":["integer","string"]},"srcOutput":{"type":["integer","string"]},"lo":{"type":"number"},"hi":{"type":"number"},"curve":{"type":"number","minimum":-1,"maximum":1},"enabled":{"type":"boolean"}},"required":["dst","param","src"]})" },
         { "unbind_modulator", "Remove the modulator binding from a parameter.",
           R"({"type":"object","properties":{"dst":{"type":["integer","string"]},"param":{"type":["integer","string"]}},"required":["dst","param"]})" },
         { "set_expression",
           "Drive a parameter from an expression evaluated every frame (t = seconds, plus globals). Empty text removes "
           "it. A modulator bound to the same parameter wins over the expression.",
           R"({"type":"object","properties":{"dst":{"type":["integer","string"]},"param":{"type":["integer","string"]},"text":{"type":"string"},"curve":{"type":"number"}},"required":["dst","param","text"]})" },
         { "set_global", "Create or replace a named global expression that parameter expressions can read.",
           R"({"type":"object","properties":{"name":{"type":"string"},"expr":{"type":"string"}},"required":["name","expr"]})" },
         { "remove_global", "Delete a global expression.",
           R"({"type":"object","properties":{"name":{"type":"string"}},"required":["name"]})" },
         { "batch",
           "Run several of these tools as ONE undo step, all or nothing: if any call fails, nothing is applied. "
           "Give create_node an \"id\" (letters, digits, _) and refer to that node as \"$id\" in later calls of the "
           "same batch. Parameters of a node created in the same batch must be given by number. "
           "Example: {\"calls\":[{\"method\":\"create_node\",\"params\":{\"typeName\":\"Shape\",\"id\":\"pic\"}},"
           "{\"method\":\"connect\",\"params\":{\"src\":\"$pic\",\"dst\":3,\"dstSlot\":\"in\"}}]}",
           R"({"type":"object","properties":{"calls":{"type":"array","items":{"type":"object","properties":{"method":{"type":"string"},"params":{"type":"object"}},"required":["method"]}}},"required":["calls"]})" },
         { "undo", "Undo the last change (a whole batch counts as one).", R"({"type":"object","properties":{}})" },
         { "redo", "Redo the last undone change.", R"({"type":"object","properties":{}})" },
         { "validate_patch_text",
           "Check patch-file source (the .inf text format) without touching the canvas. Returns errors and warnings "
           "with line numbers and hints.",
           R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})" },
         { "load_patch_text",
           "Replace the WHOLE patch with patch-file source (one undo step). Only when the user asks for a new patch; "
           "validate it first.",
           R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})" },
         { "render_frame",
           "Look at the picture: returns the Output node's current image plus brightness/colour statistics.",
           R"({"type":"object","properties":{"output":{"type":"integer","description":"Output node index when there are several"}}})" },
      };

      // ---- the connection to the running app ---------------------------------

      class Link
      {
      public:
         ~Link() { Close(); }

         // One request/reply round trip. False (with err) when the app is not
         // reachable; an RPC-level error comes back as `reply.error`.
         bool Call(const std::string& method, const json& params, json& reply, std::string& err)
         {
            for (int attempt = 0; attempt < 2; attempt++)
            {
               if (mFd < 0 && !Open(err))
                  return false;
               const json req = { {"jsonrpc", "2.0"}, {"id", ++mNextId}, {"token", mToken},
                                  {"method", method}, {"params", params} };
               const std::string line = req.dump() + "\n";
               std::string got;
               if (SendAll(line) && ReadLine(got))
               {
                  reply = json::parse(got, nullptr, false);
                  if (reply.is_discarded())
                  {
                     err = "unreadable reply from Infinite";
                     return false;
                  }
                  // A token from a previous launch: the app restarted, re-read it.
                  if (reply.contains("error") && reply["error"].value("message", "").find("auth token") != std::string::npos &&
                      attempt == 0)
                  {
                     Close();
                     continue;
                  }
                  return true;
               }
               Close(); // the app went away or restarted; reconnect once
            }
            err = "lost the connection to Infinite";
            return false;
         }

      private:
         int mFd = -1;
         int mNextId = 0;
         std::string mToken;
         std::string mBuf;

         bool Open(std::string& err)
         {
            // u8path: the same UTF-8-safe path RemoteControl writes it to.
            std::ifstream tok(std::filesystem::u8path(AppPaths::AppSupportDir() + "/control_token"));
            std::getline(tok, mToken);
            int port = 7777;
            if (const char* p = std::getenv("INFINITE_CONTROL_PORT"))
               port = std::atoi(p);
            const int fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons((unsigned short)port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (fd < 0 || ::connect(fd, (const sockaddr*)&addr, sizeof(addr)) != 0)
            {
               if (fd >= 0)
                  NetCompat::NetClose(fd);
               err = "Infinite is not running (nothing listens on 127.0.0.1:" + std::to_string(port) +
                     "). Ask the user to open Infinite, then try again.";
               return false;
            }
            mFd = fd;
            mBuf.clear();
            return true;
         }

         void Close()
         {
            if (mFd >= 0)
               NetCompat::NetClose(mFd);
            mFd = -1;
         }

         bool SendAll(const std::string& s)
         {
            size_t sent = 0;
            while (sent < s.size())
            {
               const NetSsize n = NetCompat::NetSend(mFd, s.data() + sent, s.size() - sent, 0);
               if (n <= 0)
                  return false;
               sent += (size_t)n;
            }
            return true;
         }

         bool ReadLine(std::string& out)
         {
            for (;;)
            {
               const size_t nl = mBuf.find('\n');
               if (nl != std::string::npos)
               {
                  out = mBuf.substr(0, nl);
                  mBuf.erase(0, nl + 1);
                  return true;
               }
               char chunk[65536];
               const NetSsize n = NetCompat::NetRecv(mFd, chunk, sizeof(chunk), 0);
               if (n <= 0)
                  return false;
               mBuf.append(chunk, (size_t)n);
            }
         }
      };

      // ---- tool calls -----------------------------------------------------------

      json TextResult(const std::string& text, bool isError)
      {
         return { {"content", json::array({ { {"type", "text"}, {"text", text} } })}, {"isError", isError} };
      }

      // get_graph without every node's full parameter dump, which would cost the
      // model thousands of tokens per call; get_params fetches one node's.
      json CompactGraph(const json& g)
      {
         json out = json::object();
         json nodes = json::array();
         for (const json& n : g.value("nodes", json::array()))
         {
            json c = { {"index", n.value("index", -1)}, {"type", n.value("typeName", "")}, {"category", n.value("category", "")} };
            if (n.value("bypassed", false))
               c["bypassed"] = true;
            nodes.push_back(c);
         }
         out["nodes"] = nodes;
         // Cables from the patch records, which are always complete; the live
         // `links` list is rebuilt when a frame draws (empty straight after a
         // batch) and is only used here for the id that disconnect takes.
         json cables = json::array();
         for (const char* kind : { "cables", "geometry", "audio", "notes" })
            for (const json& c : g.value(kind, json::array()))
            {
               json rec = { {"kind", std::string(kind) == "cables" ? "image" : kind},
                            {"src", c.value("srcIndex", -1)}, {"srcOutput", c.value("srcOutput", 0)},
                            {"dst", c.value("dstIndex", -1)}, {"dstSlot", c.value("dstSlot", -1)} };
               for (const json& l : g.value("links", json::array()))
                  if (l.value("srcIndex", -2) == rec["src"] && l.value("srcOutput", -2) == rec["srcOutput"] &&
                      l.value("dstIndex", -2) == rec["dst"] && l.value("dstSlot", -2) == rec["dstSlot"])
                     rec["id"] = l["id"];
               cables.push_back(rec);
            }
         out["cables"] = cables;
         for (const char* key : { "modulation", "expressions", "globals" })
            if (g.contains(key) && !g[key].empty())
               out[key] = g[key];
         return out;
      }

      json CallTool(Link& link, const std::string& name, const json& args)
      {
         bool known = false;
         for (const Tool& t : kTools)
            known = known || name == t.name;
         if (!known)
            return TextResult("unknown tool '" + name + "'", true);

         json params = args.is_object() ? args : json::object();
         std::string tmpImage;
         if (name == "render_frame")
         {
            std::error_code ec;
            tmpImage = (std::filesystem::temp_directory_path(ec) / "infinite_mcp_frame.jpg").string();
            params["path"] = tmpImage;
            params["maxSize"] = 1024; // enough to judge the picture; a 4K frame would blow the tool-output budget
         }

         json reply;
         std::string err;
         if (!link.Call(name, params, reply, err))
            return TextResult(err, true);
         if (reply.contains("error"))
            return TextResult(reply["error"].value("message", "error"), true);
         json result = reply.value("result", json::object());

         if (name == "get_graph")
            return TextResult(CompactGraph(result).dump(), false);
         if (name == "render_frame")
         {
            std::ifstream f(tmpImage, std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            result.erase("file");
            json content = json::array({ { {"type", "text"}, {"text", result.dump()} } });
            if (!bytes.empty())
               content.push_back({ {"type", "image"}, {"mimeType", "image/jpeg"},
                                   {"data", Base64::Encode((const uint8_t*)bytes.data(), bytes.size())} });
            return { {"content", content}, {"isError", false} };
         }
         return TextResult(result.dump(), false);
      }

      // ---- the MCP side ---------------------------------------------------------

      void Send(const json& msg)
      {
         const std::string s = msg.dump() + "\n";
         std::fwrite(s.data(), 1, s.size(), stdout);
         std::fflush(stdout);
      }

      void Reply(const json& id, const json& result) { Send({ {"jsonrpc", "2.0"}, {"id", id}, {"result", result} }); }

      void ReplyError(const json& id, int code, const std::string& message)
      {
         Send({ {"jsonrpc", "2.0"}, {"id", id}, {"error", { {"code", code}, {"message", message} }} });
      }
   }

   int Run()
   {
#if defined(_WIN32)
      // No CRLF translation on the protocol stream.
      _setmode(_fileno(stdin), _O_BINARY);
      _setmode(_fileno(stdout), _O_BINARY);
#endif
      Link link;
      std::string line;
      while (std::getline(std::cin, line))
      {
         if (!line.empty() && line.back() == '\r')
            line.pop_back();
         if (line.empty())
            continue;
         const json msg = json::parse(line, nullptr, false);
         if (msg.is_discarded() || !msg.is_object())
         {
            ReplyError(nullptr, -32700, "parse error");
            continue;
         }
         const std::string method = msg.value("method", "");
         // Notifications (no id) need no answer: initialized, cancelled, ...
         if (!msg.contains("id"))
            continue;
         const json id = msg["id"];
         const json params = msg.value("params", json::object());

         if (method == "initialize")
         {
            Reply(id, { {"protocolVersion", params.value("protocolVersion", "2025-06-18")},
                        {"capabilities", { {"tools", json::object()} }},
                        {"serverInfo", { {"name", "infinite"}, {"version", INFINITE_VERSION_STRING} }},
                        {"instructions", kInstructions} });
         }
         else if (method == "ping")
            Reply(id, json::object());
         else if (method == "tools/list")
         {
            json tools = json::array();
            for (const Tool& t : kTools)
               tools.push_back({ {"name", t.name}, {"description", t.description}, {"inputSchema", json::parse(t.schema)} });
            Reply(id, { {"tools", tools} });
         }
         else if (method == "tools/call")
         {
            try
            {
               Reply(id, CallTool(link, params.value("name", ""), params.value("arguments", json::object())));
            }
            catch (const std::exception& e)
            {
               Reply(id, TextResult(std::string("bridge error: ") + e.what(), true));
            }
         }
         else
            ReplyError(id, -32601, "method not found: " + method);
      }
      return 0;
   }
}
