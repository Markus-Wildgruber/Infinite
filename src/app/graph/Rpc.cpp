// RPC command handler (moved verbatim from main.cpp).
#include "app/AppShared.h"
// Non-core headers this file uses (docs/plans/compile-times/include-split.md).
#include "app/AppUndo.h"
#include "audio/AudioEngine.h"
#include "core/ExprGlobals.h"
#include "core/ImageCable.h"
#include "core/PatchJson.h"
#include "nodes/AudioToMidiNode.h"
#include "nodes/DrumPatterns.h"
#include "nodes/NoteNodes.h"
#include "nodes/SoundAnalyzeNode.h"
#include "nodes/UtilityNodes.h"
#include "nodes/Geometry3DNodes.h"
#include "nodes/OutputNode.h"
#include "app/AppArrange.h"

namespace app
{
   namespace
   {
      using json = nlohmann::json;

      bool IsDigits(const std::string& s)
      {
         return !s.empty() && s.size() < 10 &&
                std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
      }

      std::string JoinNames(const std::vector<std::string>& v)
      {
         std::string out;
         for (const std::string& s : v)
            out += (out.empty() ? "" : ", ") + s;
         return out;
      }

      // A live node's index from params[key] (or params[alias]): an integer or
      // a string of digits. Inside a batch, "$label" strings have already been
      // replaced with the index they stand for (see the batch branch).
      bool ReadNodeIndex(const json& params, const char* key, const char* alias, int& out, std::string& err)
      {
         const json* v = params.contains(key) ? &params[key]
                       : (alias != nullptr && params.contains(alias)) ? &params[alias] : nullptr;
         if (v == nullptr)
         {
            err = std::string("missing '") + key + "'";
            return false;
         }
         if (v->is_number_integer())
            out = v->get<int>();
         else if (v->is_string() && IsDigits(v->get<std::string>()))
            out = std::stoi(v->get<std::string>());
         else
         {
            const std::string s = v->is_string() ? v->get<std::string>() : v->dump();
            err = !s.empty() && s[0] == '$' ? "'" + s + "': $references only work inside a batch"
                                            : std::string("'") + key + "' must be a node index, got " + s;
            return false;
         }
         if (FindNodeByIndex(out) == nullptr)
         {
            err = "unknown node index " + std::to_string(out);
            return false;
         }
         return true;
      }

      // An input slot (number) or its authoring name from `describe` (input, input_2, ...).
      bool ReadSlot(const json& v, const GraphNode& dst, int& out, std::string& err)
      {
         if (v.is_number_integer())
         {
            out = v.get<int>();
            return true;
         }
         const std::string word = v.is_string() ? v.get<std::string>() : v.dump();
         if (IsDigits(word))
         {
            out = std::stoi(word);
            return true;
         }
         const PatchSchema::TypeSchema* t = SchemaFor(dst.typeName);
         std::vector<std::string> names;
         if (t != nullptr && PatchSchema::FindSlot(*t, word, out, names))
            return true;
         err = "'" + word + "' is not an input of " + dst.typeName + " (inputs: " + JoinNames(names) + ")";
         return false;
      }

      // An output (number) or its label from `describe` (case-insensitive).
      bool ReadOutput(const json& v, const GraphNode& src, int& out, std::string& err)
      {
         if (v.is_number_integer())
         {
            out = v.get<int>();
            return true;
         }
         const std::string word = v.is_string() ? v.get<std::string>() : v.dump();
         if (IsDigits(word))
         {
            out = std::stoi(word);
            return true;
         }
         const PatchSchema::TypeSchema* t = SchemaFor(src.typeName);
         std::vector<std::string> labels;
         if (t != nullptr && PatchSchema::FindOutput(*t, word, out, labels))
            return true;
         err = "'" + word + "' is not an output of " + src.typeName + " (outputs: " + JoinNames(labels) + ")";
         return false;
      }

      // A modulatable parameter: its `mod`/`expr` index, or its saved key
      // (`sizeX`), or failing that its on-screen label. Keys are only known for
      // a type once a node of it has drawn a frame (JoinLiveTier1), so a node
      // created in the same batch has to be addressed by index or bound later.
      bool ReadParam(const json& v, const GraphNode& dst, int& out, std::string& err)
      {
         const auto maxIt = gModulatableMax.find(dst.typeName);
         auto inRange = [&](int p)
         {
            if (p < 0 || (maxIt != gModulatableMax.end() && p > maxIt->second))
            {
               err = "param " + std::to_string(p) + " is out of range for " + dst.typeName;
               return false;
            }
            return true;
         };
         if (v.is_number_integer())
         {
            out = v.get<int>();
            return inRange(out);
         }
         const std::string word = v.is_string() ? v.get<std::string>() : v.dump();
         if (IsDigits(word))
         {
            out = std::stoi(word);
            return inRange(out);
         }
         JoinLiveTier1();
         std::vector<std::string> pool;
         auto join = gParamJoin.find(dst.typeName);
         if (join != gParamJoin.end() && join->second.done)
         {
            auto k = join->second.paramOfKey.find(word);
            if (k != join->second.paramOfKey.end())
            {
               out = k->second;
               return true;
            }
            for (const auto& kv : join->second.paramOfKey)
               pool.push_back(kv.first);
         }
         std::string lw = word;
         std::transform(lw.begin(), lw.end(), lw.begin(), [](unsigned char c) { return (char)std::tolower(c); });
         for (const auto& kv : Modulation::Instance().AllKnownParams())
         {
            if (kv.second.nodeIndex != dst.index)
               continue;
            std::string ln = kv.second.name;
            std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ln == lw)
            {
               out = kv.second.paramIndex;
               return true;
            }
            pool.push_back(kv.second.name);
         }
         if (pool.empty())
         {
            // A collapsed, undriven node never registers its controls; ask
            // DrawNodeBodies to this frame, so the next request can resolve it.
            gRpcParamProbe.insert(dst.index);
            err = dst.typeName + " " + std::to_string(dst.index) +
                  " has not reported its parameters yet - retry this call once (they are being read now), "
                  "or pass the numeric param";
         }
         else
         {
            const std::vector<std::string> near = PatchSchema::Nearest(word, pool, 3);
            err = "'" + word + "' is not a modulatable parameter of " + dst.typeName +
                  (near.empty() ? std::string() : " (did you mean: " + JoinNames(near) + ")");
         }
         return false;
      }

      // {"<kind> <key>": [option names]} for every dropdown of `gn` whose
      // saved key is known. Dropdowns register their option names with their
      // ParamRef when the node draws; a node that has not drawn yet is asked
      // to (gRpcParamProbe), so the next call has them.
      json EnumOptionsOf(const GraphNode& gn, const std::vector<std::pair<std::string, std::string>>& raw)
      {
         json out = json::object();
         JoinLiveTier1();
         auto join = gParamJoin.find(gn.typeName);
         bool any = false;
         for (const auto& kv : Modulation::Instance().AllKnownParams())
         {
            if (kv.second.nodeIndex != gn.index)
               continue;
            any = true;
            if (!kv.second.isEnum || kv.second.enumOptions.empty() || join == gParamJoin.end())
               continue;
            auto k = join->second.keyOfParam.find(kv.second.paramIndex);
            if (k == join->second.keyOfParam.end())
               continue;
            for (const auto& r : raw)
               if (r.first.size() > 2 && r.first.compare(2, std::string::npos, k->second) == 0)
               {
                  out[r.first] = kv.second.enumOptions;
                  break;
               }
         }
         if (!any)
            gRpcParamProbe.insert(gn.index);
         return out;
      }

      // A locked group refuses every edit to its members, from the assistant
      // too (NodeEditLocked, panels/PreviewBodies.cpp). One call per touched node.
      // A refused timeline edit changed nothing, so the checkpoint pushed for it
      // would be an empty undo step; drop it again. Only when this call added
      // one (inside a batch checkpoints are suppressed and nothing was pushed).
      void DropUnusedCheckpoint(size_t undoDepthBefore)
      {
         if (gUndoStack.size() > undoDepthBefore)
            gUndoStack.pop_back();
      }

      bool RefuseIfLocked(int nodeIndex, std::string& outError)
      {
         std::string why;
         if (!NodeEditLocked(nodeIndex, &why))
            return false;
         outError = why + " - unlock it first (set_group locked:false)";
         return true;
      }

      GroupNode* GroupAt(int index, std::string& outError)
      {
         GraphNode* gn = FindNodeByIndex(index);
         auto* g = gn != nullptr ? dynamic_cast<GroupNode*>(gn->node.get()) : nullptr;
         if (g == nullptr)
            outError = "node " + std::to_string(index) + " is not a Group";
         return g;
      }

      json GroupJson(const GraphNode& gn, const GroupNode& g)
      {
         json members = json::array();
         for (int m : GroupMemberIndices(&g))
            members.push_back(m);
         return { {"index", gn.index}, {"uid", "u" + std::to_string(gn.uid)}, {"label", g.label}, {"members", members},
                  {"mute", g.mute}, {"solo", g.solo}, {"locked", g.locked} };
      }

      bool ReadIndexList(const json& params, const char* key, std::vector<int>& out, std::string& outError)
      {
         if (!params.contains(key) || !params[key].is_array() || params[key].empty())
         {
            outError = std::string("'") + key + "' must be a non-empty array of node indices";
            return false;
         }
         for (const json& v : params[key])
         {
            if (!v.is_number_integer() || FindNodeByIndex(v.get<int>()) == nullptr)
            {
               outError = "unknown node index in '" + std::string(key) + "': " + v.dump();
               return false;
            }
            out.push_back(v.get<int>());
         }
         return true;
      }

      // The saved key and on-screen label of a parameter index, when known.
      void DescribeParam(const GraphNode& gn, int param, json& rec)
      {
         auto join = gParamJoin.find(gn.typeName);
         if (join != gParamJoin.end())
         {
            auto k = join->second.keyOfParam.find(param);
            if (k != join->second.keyOfParam.end())
               rec["dstKey"] = k->second;
         }
         if (const ParamRef* ref = Modulation::Instance().KnownParam(gn.index, param))
         {
            rec["dstLabel"] = ref->name;
            rec["min"] = ref->minValue;
            rec["max"] = ref->maxValue;
            // The value this frame, after modulation/expressions - so a client
            // can see a modulator actually moving the parameter.
            if (ref->value != nullptr)
               rec["current"] = *ref->value;
         }
      }

      // Inside a batch: swap "$label" strings for the node index a previous
      // create_node {id: label} in the same batch returned. Only index-valued
      // keys are touched, so expression text or string param values that start
      // with '$' pass through untouched.
      bool SubstituteRefs(json& params, const std::map<std::string, int>& labels, std::string& err)
      {
         static const char* kIndexKeys[] = { "index", "srcIndex", "dstIndex", "src", "dst", "output" };
         if (!params.is_object())
            return true;
         for (const char* key : kIndexKeys)
         {
            if (!params.contains(key) || !params[key].is_string())
               continue;
            const std::string s = params[key].get<std::string>();
            if (s.empty() || s[0] != '$')
               continue;
            auto it = labels.find(s.substr(1));
            if (it == labels.end())
            {
               err = "unknown reference '" + s + "' (create_node with \"id\": \"" + s.substr(1) +
                     "\" earlier in this batch)";
               return false;
            }
            params[key] = it->second;
         }
         return true;
      }

      // Text params travel through RPC literally: the patch format escapes
      // backslash and newline (Patch.cpp's EscapeLine/UnescapeLine), but a
      // caller handing over a Windows path like D:\new\file means those
      // characters, not a newline. get_params unescapes what SaveParams wrote
      // and set_param escapes before LoadParams, so a get -> set round trip is
      // the identity.
      std::string EscapeParamText(const std::string& value)
      {
         std::string out;
         for (char c : value)
         {
            if (c == '\\')
               out += "\\\\";
            else if (c == '\n')
               out += "\\n";
            else if (c != '\r')
               out += c;
         }
         return out;
      }

      std::string UnescapeParamText(const std::string& raw)
      {
         std::string out;
         for (size_t i = 0; i < raw.size(); i++)
         {
            if (raw[i] == '\\' && i + 1 < raw.size() && (raw[i + 1] == 'n' || raw[i + 1] == '\\'))
            {
               out += raw[i + 1] == 'n' ? '\n' : '\\';
               i++;
            }
            else
               out += raw[i];
         }
         return out;
      }

      bool ParseNumber(const std::string& text, double& out)
      {
         const char* begin = text.c_str();
         char* end = nullptr;
         out = std::strtod(begin, &end);
         while (end != nullptr && *end == ' ')
            end++;
         return end != begin && end != nullptr && *end == '\0' && std::isfinite(out);
      }

      // A set_param value as the patch-format text for its kind (the letter
      // before the key: f/i/b/c/s), or false with a reason. Before this, any
      // JSON was dumped as-is, so `true` reached Reader::Bool as "true" and
      // silently read as false, and "abc" read as 0.
      bool ParamValueText(char kind, const json& v, std::string& out, std::string& err)
      {
         double num = 0.0;
         switch (kind)
         {
         case 'b':
            if (v.is_boolean())
               out = v.get<bool>() ? "1" : "0";
            else if (v.is_number())
               out = v.get<double>() != 0.0 ? "1" : "0";
            else if (v.is_string())
            {
               std::string w = v.get<std::string>();
               std::transform(w.begin(), w.end(), w.begin(), [](unsigned char c) { return (char)std::tolower(c); });
               if (w == "1" || w == "true" || w == "on" || w == "yes")
                  out = "1";
               else if (w == "0" || w == "false" || w == "off" || w == "no")
                  out = "0";
               else
               {
                  err = "expects true or false, got \"" + v.get<std::string>() + "\"";
                  return false;
               }
            }
            else
            {
               err = "expects true or false";
               return false;
            }
            return true;
         case 'f':
         case 'i':
            if (v.is_boolean())
               num = v.get<bool>() ? 1.0 : 0.0;
            else if (v.is_number())
               num = v.get<double>();
            else if (!(v.is_string() && ParseNumber(v.get<std::string>(), num)))
            {
               err = std::string("expects a number, got ") + (v.is_string() ? "\"" + v.get<std::string>() + "\"" : v.dump());
               return false;
            }
            if (kind == 'i')
               out = std::to_string((long long)std::llround(num));
            else
            {
               char buf[64];
               snprintf(buf, sizeof(buf), "%.9g", num);
               out = buf;
            }
            return true;
         case 'c':
            if (v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number())
            {
               char buf[96];
               snprintf(buf, sizeof(buf), "%.9g %.9g %.9g", v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
               out = buf;
               return true;
            }
            if (v.is_string())
            {
               out = v.get<std::string>();
               return true;
            }
            err = "expects [r, g, b] (0..1 each) or \"r g b\"";
            return false;
         case 's':
            out = EscapeParamText(v.is_string() ? v.get<std::string>() : v.dump());
            return true;
         default:
            out = v.is_string() ? v.get<std::string>() : v.dump();
            return true;
         }
      }
   }

   // Dispatches one RemoteControl JSON-RPC request. Runs on the main thread,
   // once per frame per pending command (see RemoteControl::DrainPending's
   // call site, right after glfwPollEvents()) - so every case here is free to
   // touch gNodes/gEditor/etc exactly as the normal UI code does. Returns
   // false with outError set on any failure; RemoteControl wraps that into a
   // JSON-RPC error reply.
   namespace
   {
      // Stable node ids: every node has a uid that survives undo, redo, reload
      // and other nodes being deleted (indices do not). Any parameter that
      // names a node also takes "u<uid>" (get_graph lists uids); this turns
      // those into the node's current index before the method runs, so every
      // method - and every call inside a batch - accepts both.
      bool ResolveUidRefs(nlohmann::json& v, const std::string& key, std::string& err)
      {
         static const char* const kNodeKeys[] = { "index", "src", "dst", "srcIndex", "dstIndex", "node", "group",
                                                  "instrument", "target", "synth", "nodes" };
         bool nodeKey = false;
         for (const char* k : kNodeKeys)
            nodeKey = nodeKey || key == k;
         if (v.is_object())
         {
            for (auto it = v.begin(); it != v.end(); ++it)
               if (!ResolveUidRefs(it.value(), it.key(), err))
                  return false;
            return true;
         }
         if (v.is_array())
         {
            for (auto& item : v)
               if (!ResolveUidRefs(item, nodeKey ? key : std::string(), err))
                  return false;
            return true;
         }
         if (!nodeKey || !v.is_string())
            return true;
         const std::string s = v.get<std::string>();
         if (s.size() < 2 || s[0] != 'u' || s.find_first_not_of("0123456789", 1) != std::string::npos)
            return true;
         const uint64_t uid = std::stoull(s.substr(1));
         GraphNode* gn = FindNodeByUid(uid);
         if (gn == nullptr)
         {
            err = "no node with uid " + s.substr(1) + " ('" + key + "')";
            return false;
         }
         v = gn->index;
         return true;
      }
   }

   bool HandleRpcCommandImpl(const std::string& method, const nlohmann::json& params,
                             nlohmann::json& outResult, std::string& outError);

   bool HandleRpcCommand(const std::string& method, const nlohmann::json& params,
                         nlohmann::json& outResult, std::string& outError)
   {
      // "calls" of a batch are resolved as each call runs (an earlier call may
      // create the node a later one names), so only the top level here.
      if (method == "batch")
         return HandleRpcCommandImpl(method, params, outResult, outError);
      nlohmann::json resolved = params;
      if (!ResolveUidRefs(resolved, std::string(), outError))
         return false;
      return HandleRpcCommandImpl(method, resolved, outResult, outError);
   }

   bool HandleRpcCommandImpl(const std::string& method, const nlohmann::json& params,
                             nlohmann::json& outResult, std::string& outError)
   {
      using json = nlohmann::json;

      if (method == "list_node_types")
      {
         json categories = json::object();
         for (const std::string& cat : NodeFactory::Instance().GetCategories())
         {
            json names = json::array();
            for (const std::string& n : NodeFactory::Instance().GetNodesInCategory(cat))
               names.push_back(n);
            categories[cat] = names;
         }
         outResult = categories;
         return true;
      }
      else if (method == "create_node")
      {
         const std::string typeName = params.value("typeName", std::string());
         std::string category = params.value("category", std::string());
         if (category.empty())
            category = NodeFactory::Instance().CategoryOf(typeName);
         const float x = params.value("x", 0.0f);
         const float y = params.value("y", 0.0f);
         GraphNode* gn = SpawnNode(typeName, category, x, y);
         if (gn == nullptr)
         {
            std::vector<std::string> pool;
            for (const std::string& cat : NodeFactory::Instance().GetCategories())
               for (const std::string& n : NodeFactory::Instance().GetNodesInCategory(cat))
                  pool.push_back(n);
            const std::vector<std::string> near = PatchSchema::Nearest(typeName, pool, 3);
            outError = "unknown node type '" + typeName + "'" +
                       (near.empty() ? std::string() : " (did you mean: " + JoinNames(near) + ")");
            return false;
         }
         outResult = { {"index", gn->index}, {"uid", "u" + std::to_string(gn->uid)} };
         // Have it register its controls on this frame's draw even though it
         // starts collapsed, so its parameter keys resolve from the next request.
         gRpcParamProbe.insert(gn->index);
         // The batch branch records it so later calls can say "$<id>".
         if (params.contains("id"))
            outResult["id"] = params["id"];
         return true;
      }
      else if (method == "delete_node")
      {
         const int index = params.value("index", -1);
         if (FindNodeByIndex(index) == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         if (RefuseIfLocked(index, outError))
            return false;
         RemoveNodeByIndex(index);
         outResult = json::object();
         return true;
      }
      else if (method == "connect")
      {
         int srcIndex = -1, dstIndex = -1, srcOutput = 0, dstSlot = -1;
         if (!ReadNodeIndex(params, "srcIndex", "src", srcIndex, outError) ||
             !ReadNodeIndex(params, "dstIndex", "dst", dstIndex, outError))
            return false;
         if (params.contains("srcOutput") && !ReadOutput(params["srcOutput"], *FindNodeByIndex(srcIndex), srcOutput, outError))
            return false;
         if (params.contains("dstSlot") && !ReadSlot(params["dstSlot"], *FindNodeByIndex(dstIndex), dstSlot, outError))
            return false;
         if (RefuseIfLocked(srcIndex, outError) || RefuseIfLocked(dstIndex, outError))
            return false;
         if (!ConnectNodes(srcIndex, srcOutput, dstIndex, dstSlot, outError))
            return false;
         outResult = json::object();
         return true;
      }
      else if (method == "disconnect")
      {
         const int linkId = params.value("linkId", -1);
         const LinkInfo* link = FindLink(linkId);
         if (link == nullptr)
         {
            outError = "unknown link id";
            return false;
         }
         if (RefuseIfLocked(GraphNode::NodeIndexFromPin(link->srcPin), outError) ||
             RefuseIfLocked(GraphNode::NodeIndexFromPin(link->dstPin), outError))
            return false;
         PushUndoCheckpoint();
         DisconnectLinkById(linkId);
         outResult = json::object();
         return true;
      }
      else if (method == "get_graph")
      {
         json out = PatchJson::ToJson(BuildPatchData());
         // Patch::Data's cable records carry endpoints but no link id, since
         // ids are a live editor concept and never hit the patch file. The
         // disconnect() method needs one, so publish gLinks alongside the
         // patch, with each pin decoded back to a node index + slot/output.
         json links = json::array();
         for (const LinkInfo& link : gLinks)
         {
            json entry = { {"id", link.id} };
            entry["srcIndex"] = GraphNode::NodeIndexFromPin(link.srcPin);
            entry["srcOutput"] = GraphNode::IsOutputPin(link.srcPin)
                                    ? GraphNode::OutputIndexFromPin(link.srcPin)
                                    : 0;
            entry["dstIndex"] = GraphNode::NodeIndexFromPin(link.dstPin);
            const int dstOff = GraphNode::OffsetFromPin(link.dstPin);
            if (GraphNode::IsParamPin(link.dstPin))
               entry["dstParamPin"] = dstOff - GraphNode::kParamBase;
            else if (GraphNode::IsColorPin(link.dstPin))
               entry["dstColorPin"] = GraphNode::ColorIndexFromPin(link.dstPin);
            else
               entry["dstSlot"] = dstOff - 1;
            links.push_back(entry);
         }
         out["links"] = links;
         outResult = out;
         return true;
      }
      else if (method == "explain")
      {
         JoinLiveTier1();
         outResult = json::object();
         outResult["text"] = ExplainLive(false, params.value("all", false));
         outResult["graph"] = json::parse(ExplainLive(true, true));
         return true;
      }
      else if (method == "get_params")
      {
         const int index = params.value("index", -1);
         GraphNode* gn = FindNodeByIndex(index);
         if (gn == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         std::vector<std::pair<std::string, std::string>> raw;
         Patch::SaveParams(gn->node.get(), raw);
         json out = json::object();
         for (const auto& kv : raw)
            out[kv.first] = kv.first.rfind("s ", 0) == 0 ? UnescapeParamText(kv.second) : kv.second;
         // Dropdowns: the option names, index order, so "i band1Type": "0" can
         // be read as "low shelf". A sibling field keeps the value map flat.
         const json options = EnumOptionsOf(*gn, raw);
         if (!options.empty())
            out["_options"] = options;
         // Parameters a modulator or an expression drives: their value this
         // frame (the plain value above is the same live number; this says
         // which ones move and from what).
         {
            json modulated = json::object();
            auto join = gParamJoin.find(gn->typeName);
            for (const auto& kv : Modulation::Instance().AllKnownParams())
            {
               const ParamRef& ref = kv.second;
               if (ref.nodeIndex != gn->index || ref.value == nullptr)
                  continue;
               const bool mod = Modulation::Instance().IsModulated(gn->index, ref.paramIndex);
               const bool expr = Modulation::Instance().HasExpression(gn->index, ref.paramIndex);
               if (!mod && !expr)
                  continue;
               std::string key = ref.name;
               if (join != gParamJoin.end())
               {
                  auto k = join->second.keyOfParam.find(ref.paramIndex);
                  if (k != join->second.keyOfParam.end())
                     key = k->second;
               }
               modulated[key] = { {"current", *ref.value}, {"by", mod ? "modulator" : "expression"}, {"param", ref.paramIndex} };
            }
            if (!modulated.empty())
               out["_modulated"] = modulated;
         }
         if (gn->label.size())
            out["_label"] = gn->label;
         outResult = out;
         return true;
      }
      else if (method == "set_param")
      {
         const int index = params.value("index", -1);
         const std::string name = params.value("name", std::string());
         GraphNode* gn = FindNodeByIndex(index);
         if (gn == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         if (RefuseIfLocked(index, outError))
            return false;
         // Params are keyed "<type letter> <name>" (Patch.h's f/i/b/c/s
         // convention) - find the existing key for `name` so we replay it
         // with the right type letter rather than guessing one.
         std::vector<std::pair<std::string, std::string>> raw;
         Patch::SaveParams(gn->node.get(), raw);
         std::string matchedKey;
         for (const auto& kv : raw)
         {
            const size_t sp = kv.first.find(' ');
            if (sp != std::string::npos && kv.first.substr(sp + 1) == name)
            {
               matchedKey = kv.first;
               break;
            }
         }
         if (matchedKey.empty())
         {
            outError = "unknown param '" + name + "' on this node";
            return false;
         }
         if (!params.contains("value"))
         {
            outError = "missing 'value'";
            return false;
         }
         json value = params["value"];
         if (value.is_string() && (matchedKey[0] == 'i' || matchedKey[0] == 'f'))
         {
            // A dropdown accepts the option's name ("low shelf") as well as its number.
            const json options = EnumOptionsOf(*gn, raw);
            auto it = options.find(matchedKey);
            if (it != options.end())
            {
               std::string want = value.get<std::string>();
               std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return (char)std::tolower(c); });
               for (size_t i = 0; i < it->size(); i++)
               {
                  std::string opt = (*it)[i].is_string() ? (*it)[i].get<std::string>() : std::string();
                  std::transform(opt.begin(), opt.end(), opt.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                  if (opt == want)
                  {
                     value = (int)i;
                     break;
                  }
               }
               if (value.is_string() && !IsDigits(want))
               {
                  outError = "'" + name + "' has no option '" + value.get<std::string>() + "' (options: " + it->dump() + ")";
                  return false;
               }
            }
         }
         std::string valueStr, valueErr;
         if (!ParamValueText(matchedKey[0], value, valueStr, valueErr))
         {
            outError = "'" + name + "' " + valueErr;
            return false;
         }
         PushUndoCheckpoint();
         Patch::LoadParams(gn->node.get(), { { matchedKey, valueStr } });
         // A file path (or a formula/Field source) only takes effect through the
         // same reload a patch load runs; without it an Audio File given a new
         // path never loaded the file and hid its controls until the next undo.
         if (matchedKey[0] == 's')
            ReloadDerivedState(gn->node.get());
         outResult = json::object();
         return true;
      }
      else if (method == "set_node_position" || method == "get_node_position" || method == "move_node")
      {
         const int index = params.value("index", -1);
         GraphNode* gn = FindNodeByIndex(index);
         if (gn == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         ed::EditorContext* prevEditor = ed::GetCurrentEditor();
         ed::SetCurrentEditor(gEditor);
         if (method == "set_node_position" || method == "move_node")
         {
            if (RefuseIfLocked(index, outError))
            {
               ed::SetCurrentEditor(prevEditor);
               return false;
            }
            // x/y place it; dx/dy move it from where it is. Canvas units,
            // the same numbers get_graph reports.
            const ImVec2 at = ed::GetNodePosition(gn->NodeId());
            float x = params.value("x", at.x);
            float y = params.value("y", at.y);
            x += params.value("dx", 0.0f);
            y += params.value("dy", 0.0f);
            if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 1e6f || std::fabs(y) > 1e6f)
            {
               ed::SetCurrentEditor(prevEditor);
               outError = "position out of range";
               return false;
            }
            PushUndoCheckpoint();
            ed::SetNodePosition(gn->NodeId(), ImVec2(x, y));
            gn->liveX = x;
            gn->liveY = y;
            gPatchDirty = true;
            outResult = { {"x", x}, {"y", y} };
         }
         else
         {
            const ImVec2 p = ed::GetNodePosition(gn->NodeId());
            outResult = { {"x", p.x}, {"y", p.y} };
         }
         ed::SetCurrentEditor(prevEditor);
         return true;
      }
      else if (method == "set_bypass")
      {
         // Bypass = the node passes its input straight through (or goes
         // silent / stops cooking when it has none), as the B key does.
         const int index = params.value("index", -1);
         GraphNode* gn = FindNodeByIndex(index);
         if (gn == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         if (RefuseIfLocked(index, outError))
            return false;
         bool want = !gn->node->bypassed;
         if (params.contains("bypassed"))
         {
            if (!params["bypassed"].is_boolean())
            {
               outError = "'bypassed' must be true or false";
               return false;
            }
            want = params["bypassed"].get<bool>();
         }
         if (want != gn->node->bypassed)
         {
            PushUndoCheckpoint();
            gn->node->bypassed = want;
            if (dynamic_cast<IAudioSource*>(gn->node.get()) != nullptr || dynamic_cast<INoteSource*>(gn->node.get()) != nullptr ||
                gn->node->AudioNodeForNotePorts() != nullptr)
               RebuildAudioTopology();
            gPatchDirty = true;
         }
         outResult = { {"bypassed", gn->node->bypassed} };
         return true;
      }
      else if (method == "rename_node")
      {
         // A title of the user's choosing; "" goes back to the type's own name.
         const int index = params.value("index", -1);
         GraphNode* gn = FindNodeByIndex(index);
         if (gn == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         if (!params.contains("name") || !params["name"].is_string())
         {
            outError = "'name' must be a string (\"\" removes the rename)";
            return false;
         }
         if (RefuseIfLocked(index, outError))
            return false;
         std::string label = params["name"].get<std::string>();
         for (char& ch : label)
            if (ch == '\n' || ch == '\r' || ch == '\t')
               ch = ' ';
         if (label.size() > 80)
            label.resize(80);
         if (label != gn->label)
         {
            PushUndoCheckpoint();
            gn->label = label;
            gPatchDirty = true;
         }
         outResult = { {"title", NodeTitle(*gn)} };
         return true;
      }
      else if (method == "get_notes")
      {
         // The notes a note-file node plays, as numbers: Audio to MIDI (the
         // transcription, in seconds of the file) or MIDI File (in beats).
         const int index = params.value("index", -1);
         GraphNode* gn = FindNodeByIndex(index);
         if (gn == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         const double from = params.value("from", 0.0);
         const double to = params.value("to", 1e12);
         const int low = params.value("low", 0);
         const int high = params.value("high", 127);
         const int limit = std::clamp(params.value("limit", 200), 1, 2000);
         auto noteName = [](int p) {
            static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            return std::string(names[((p % 12) + 12) % 12]) + std::to_string(p / 12 - 1);
         };
         json notes = json::array();
         int total = 0, lo = 128, hi = -1;
         int histogram[12] = {};
         if (auto* a2m = dynamic_cast<AudioToMidiNode*>(gn->node.get()))
         {
            for (const BasicPitch::Note& n : a2m->Notes())
            {
               if (n.end < from || n.start > to || n.pitch < low || n.pitch > high)
                  continue;
               total++;
               lo = std::min(lo, n.pitch);
               hi = std::max(hi, n.pitch);
               histogram[n.pitch % 12]++;
               if ((int)notes.size() < limit)
                  notes.push_back({ {"t", std::round(n.start * 1000.0) / 1000.0}, {"dur", std::round((n.end - n.start) * 1000.0) / 1000.0},
                                    {"note", noteName(n.pitch)}, {"midi", n.pitch}, {"level", std::round(n.amplitude * 100.0f) / 100.0f} });
            }
            outResult = { {"unit", "seconds"}, {"durationSeconds", a2m->DurationSeconds()} };
            if (a2m->GetState() == AudioToMidiNode::State::Converting)
               outResult["warning"] = "still converting (" + std::to_string((int)(a2m->Progress() * 100.0f)) + "%) - ask again shortly";
            else if (a2m->GetState() == AudioToMidiNode::State::Failed)
               outResult["warning"] = a2m->Status();
         }
         else if (auto* mf = dynamic_cast<MidiFileNode*>(gn->node.get()))
         {
            const MidiFile::Song* song = mf->GetSong();
            if (song != nullptr)
            {
               // Pair each note-on with its note-off (same note and track).
               std::vector<const MidiFile::Event*> open;
               for (const MidiFile::Event& e : song->events)
               {
                  if (e.on)
                  {
                     open.push_back(&e);
                     continue;
                  }
                  for (size_t i = 0; i < open.size(); i++)
                     if (open[i]->note == e.note && open[i]->track == e.track)
                     {
                        const MidiFile::Event& on = *open[i];
                        open.erase(open.begin() + (long)i);
                        if (e.beat < from || on.beat > to || on.note < low || on.note > high)
                           break;
                        total++;
                        lo = std::min(lo, (int)on.note);
                        hi = std::max(hi, (int)on.note);
                        histogram[on.note % 12]++;
                        if ((int)notes.size() < limit)
                           notes.push_back({ {"t", std::round(on.beat * 1000.0) / 1000.0}, {"dur", std::round((e.beat - on.beat) * 1000.0) / 1000.0},
                                             {"note", noteName(on.note)}, {"midi", on.note}, {"level", std::round(on.velocity / 1.27) / 100.0},
                                             {"channel", on.channel + 1} });
                        break;
                     }
               }
               std::sort(notes.begin(), notes.end(), [](const json& x, const json& y) { return x["t"].get<double>() < y["t"].get<double>(); });
            }
            outResult = { {"unit", "beats"} };
         }
         else
         {
            outError = "node " + std::to_string(index) + " (" + gn->typeName + ") has no notes - use an Audio to MIDI or MIDI File node";
            return false;
         }
         json pcs = json::object();
         for (int i = 0; i < 12; i++)
            if (histogram[i] > 0)
               pcs[noteName(i + 12).substr(0, noteName(i + 12).size() - 1)] = histogram[i];
         outResult["count"] = total;
         outResult["range"] = total > 0 ? json::array({ noteName(lo), noteName(hi) }) : json::array();
         outResult["pitchClasses"] = pcs;
         outResult["notes"] = notes;
         if (total > (int)notes.size())
            outResult["truncated"] = "showing the first " + std::to_string(notes.size()) + " of " + std::to_string(total) +
                                     " - narrow from/to/low/high or raise limit";
         return true;
      }
      else if (method == "duplicate_node")
      {
         // A copy of one node: same type, parameters, label and (by default)
         // the same inputs, placed beside it (dx/dy, default 40/40). Modulation
         // bindings onto it and its outgoing cables are not copied.
         const int index = params.value("index", -1);
         GraphNode* orig = FindNodeByIndex(index);
         if (orig == nullptr || orig->node == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         if (dynamic_cast<GroupNode*>(orig->node.get()) != nullptr)
         {
            outError = "duplicate_node copies single nodes; to copy a group, duplicate its members and create_group them";
            return false;
         }
         const std::string type = orig->typeName, category = orig->category, label = orig->label;
         const ImVec2 at(orig->liveX + params.value("dx", 40.0f), orig->liveY + params.value("dy", 40.0f));
         const bool connectInputs = params.value("connectInputs", true);
         const uint64_t origUid = orig->uid;
         PushUndoCheckpoint();
         const bool wasSuppressed = gSuppressUndoCheckpoints;
         gSuppressUndoCheckpoints = true;
         GraphNode* copy = SpawnNode(type, category, at.x, at.y);
         gSuppressUndoCheckpoints = wasSuppressed;
         orig = FindNodeByUid(origUid); // SpawnNode may have moved gNodes
         if (copy == nullptr || orig == nullptr)
         {
            outError = "could not create the copy";
            return false;
         }
         CopyParams(copy->node.get(), orig->node.get());
         copy->label = label.empty() ? std::string() : label + " copy";
         ReloadDerivedState(copy->node.get());
         int inputs = 0;
         if (connectInputs)
         {
            // Same sources, same slots - the cables the original already has,
            // so no compatibility question arises.
            for (int slot = 0; slot < InputCountFor(*orig); slot++)
               if (ImageCable* c = CableFor(*orig, slot))
                  if (c->IsConnected())
                     if (ImageCable* d = CableFor(*copy, slot))
                     {
                        d->Connect(c->GetSource(), c->GetSourceOutput());
                        inputs++;
                     }
            for (int slot = 0; slot < kMaxAudioSlots; slot++)
               if (AudioCable* c = orig->node->AudioInputSlot(slot))
                  if (c->IsConnected())
                     if (AudioCable* d = copy->node->AudioInputSlot(slot))
                     {
                        d->Connect(c->GetSource(), c->GetOutputSlot());
                        inputs++;
                     }
            for (int slot = 0; slot < kMaxNoteSlots; slot++)
               if (NoteCable* c = orig->node->NoteInputSlot(slot))
                  if (c->IsConnected())
                     if (NoteCable* d = copy->node->NoteInputSlot(slot))
                     {
                        d->Connect(c->GetSource(), c->GetOutputSlot());
                        inputs++;
                     }
         }
         RebuildAudioTopology();
         gRpcParamProbe.insert(copy->index);
         gPatchDirty = true;
         outResult = { {"index", copy->index}, {"uid", "u" + std::to_string(copy->uid)}, {"inputsConnected", inputs} };
         if (params.contains("id"))
            outResult["id"] = params["id"];
         return true;
      }
      else if (method == "create_group")
      {
         std::vector<int> nodes;
         if (!ReadIndexList(params, "nodes", nodes, outError))
            return false;
         for (int i : nodes)
            if (RefuseIfLocked(i, outError))
               return false;
         const size_t undoDepth = gUndoStack.size();
         PushUndoCheckpoint();
         GroupNode* g = CreateGroupForNodes(nodes, params.value("label", std::string("Group")), outError);
         if (g == nullptr)
         {
            DropUnusedCheckpoint(undoDepth);
            return false;
         }
         for (GraphNode& gn : gNodes)
            if (gn.node.get() == g)
               outResult = GroupJson(gn, *g);
         gPatchDirty = true;
         return true;
      }
      else if (method == "group_add" || method == "group_remove")
      {
         int index = -1;
         if (!ReadNodeIndex(params, "group", "index", index, outError))
            return false;
         GroupNode* g = GroupAt(index, outError);
         std::vector<int> nodes;
         if (g == nullptr || !ReadIndexList(params, "nodes", nodes, outError))
            return false;
         if (RefuseIfLocked(index, outError))
            return false;
         const size_t undoDepth = gUndoStack.size();
         PushUndoCheckpoint();
         const bool ok = method == "group_add" ? AddNodesToGroup(g, nodes, outError) : RemoveNodesFromGroup(g, nodes, outError);
         if (!ok)
         {
            DropUnusedCheckpoint(undoDepth);
            return false;
         }
         outResult = GroupJson(*FindNodeByIndex(index), *g);
         gPatchDirty = true;
         return true;
      }
      else if (method == "set_group")
      {
         // mute / solo / locked / label of one group; omitted fields stay.
         // Mute and solo work on a locked group, as in the UI.
         int index = -1;
         if (!ReadNodeIndex(params, "group", "index", index, outError))
            return false;
         GroupNode* g = GroupAt(index, outError);
         if (g == nullptr)
            return false;
         for (const char* key : { "mute", "solo", "locked" })
            if (params.contains(key) && !params[key].is_boolean())
            {
               outError = std::string("'") + key + "' must be true or false";
               return false;
            }
         if (params.contains("label") && (!params["label"].is_string() || g->locked))
         {
            outError = g->locked ? "Group '" + g->label + "' is locked - unlock it to rename it" : "'label' must be a string";
            return false;
         }
         PushUndoCheckpoint();
         if (params.contains("mute"))
            g->mute = params["mute"].get<bool>();
         if (params.contains("solo"))
            g->solo = params["solo"].get<bool>();
         if (params.contains("locked"))
            g->locked = params["locked"].get<bool>();
         if (params.contains("label"))
            g->label = params["label"].get<std::string>();
         ApplyGroupState();
         gPatchDirty = true;
         outResult = GroupJson(*FindNodeByIndex(index), *g);
         return true;
      }
      else if (method == "list_groups")
      {
         json groups = json::array();
         for (GraphNode& gn : gNodes)
            if (auto* g = dynamic_cast<GroupNode*>(gn.node.get()))
               groups.push_back(GroupJson(gn, *g));
         outResult = { {"groups", groups} };
         return true;
      }
      else if (method == "apply_sound_profile")
      {
         // Sound Analyze's Apply button: its last profile onto a synth.
         int index = -1, target = -1;
         if (!ReadNodeIndex(params, "index", "node", index, outError) || !ReadNodeIndex(params, "target", "synth", target, outError))
            return false;
         GraphNode* gn = FindNodeByIndex(index);
         auto* sa = gn != nullptr ? dynamic_cast<SoundAnalyzeNode*>(gn->node.get()) : nullptr;
         if (sa == nullptr)
         {
            outError = "node " + std::to_string(index) + " is not a Sound Analyze node";
            return false;
         }
         if (RefuseIfLocked(target, outError))
            return false;
         const size_t undoDepth = gUndoStack.size();
         PushUndoCheckpoint();
         std::string message;
         if (!ApplySoundProfile(sa, target, message))
         {
            DropUnusedCheckpoint(undoDepth);
            outError = message;
            return false;
         }
         gPatchDirty = true;
         outResult = { {"applied", message} };
         return true;
      }
      else if (method == "list_clips")
      {
         outResult = ArrangeClipsJson();
         return true;
      }
      else if (method == "add_clip")
      {
         // Places a node or group on the timeline: it plays during the clip.
         int index = -1;
         if (!ReadNodeIndex(params, "node", "index", index, outError))
            return false;
         GraphNode* gn = FindNodeByIndex(index);
         const double start = params.value("startBeats", -1.0), length = params.value("lengthBeats", -1.0);
         if (!(start >= 0.0) || !(length > 0.0))
         {
            outError = "give startBeats >= 0 and lengthBeats > 0";
            return false;
         }
         const size_t undoDepth = gUndoStack.size();
         PushUndoCheckpoint();
         uint64_t clipId = 0;
         if (!ArrangeAddClip(gn->uid, params.value("output", 0), params.value("lane", -1), start, length, clipId, outError))
         {
            DropUnusedCheckpoint(undoDepth);
            return false;
         }
         gPatchDirty = true;
         outResult = { {"clipId", clipId} };
         return true;
      }
      else if (method == "move_clip" || method == "set_clip" || method == "delete_clip")
      {
         if (!params.contains("clipId") || !params["clipId"].is_number_integer())
         {
            outError = "give the integer clipId (list_clips lists them)";
            return false;
         }
         const uint64_t clipId = params["clipId"].get<uint64_t>();
         const size_t undoDepth = gUndoStack.size();
         PushUndoCheckpoint();
         bool ok = false;
         if (method == "delete_clip")
            ok = ArrangeDeleteClip(clipId, outError);
         else if (method == "move_clip")
            ok = ArrangeMoveClip(clipId, params.value("lane", -1), params.value("startBeats", -1.0), params.value("lengthBeats", -1.0), outError);
         else
            ok = ArrangeSetClipProps(clipId, params, outError);
         if (!ok)
         {
            DropUnusedCheckpoint(undoDepth);
            return false;
         }
         gPatchDirty = true;
         outResult = json::object();
         return true;
      }
      else if (method == "set_loop")
      {
         const bool enabled = params.value("enabled", true);
         const double start = params.value("startBeats", 0.0), end = params.value("endBeats", 0.0);
         if (enabled && !(end > start))
         {
            outError = "endBeats must be after startBeats (or enabled:false to clear the loop)";
            return false;
         }
         const size_t undoDepth = gUndoStack.size();
         PushUndoCheckpoint();
         if (!ArrangeSetLoop(enabled, start, end, outError))
         {
            DropUnusedCheckpoint(undoDepth);
            return false;
         }
         gPatchDirty = true;
         outResult = { {"enabled", Transport::Instance().LoopEnabled()}, {"startBeats", Transport::Instance().LoopStartBeats()},
                       {"endBeats", Transport::Instance().LoopEndBeats()} };
         return true;
      }
      else if (method == "render_to_track" || method == "render_status")
      {
         // Bounce: renders a node offline over a range (+ tail) into a WAV on a
         // new track. Start-then-poll like the measurements; the MCP bridge waits.
         if (method == "render_to_track")
         {
            int index = -1;
            if (!ReadNodeIndex(params, "node", "index", index, outError))
               return false;
            Transport& tr = Transport::Instance();
            double start = params.value("startBeats", -1.0), end = params.value("endBeats", -1.0);
            if (start < 0.0 || end <= start)
            {
               // Default: the loop when there is one, else 4 bars from the playhead.
               if (tr.LoopEnabled())
               {
                  start = tr.LoopStartBeats();
                  end = tr.LoopEndBeats();
               }
               else
               {
                  start = std::max(0.0, tr.Beats());
                  end = start + 4.0 * tr.BeatsPerBar();
               }
            }
            if (!StartRenderNodeToTrack(index, start, end, std::clamp(params.value("tailSeconds", 2.0), 0.0, 30.0),
                                        params.value("normalize", false), outError))
               return false;
         }
         const RenderToTrackStatus& st = RenderToTrackState();
         outResult = { {"active", st.active}, {"done", st.done}, {"ok", st.ok}, {"progress", std::round(st.progress * 100.0f) / 100.0f},
                       {"message", st.message}, {"path", st.path}, {"lane", st.laneIndex}, {"clipId", st.clipId} };
         return true;
      }
      else if (method == "add_midi_track")
      {
         // A MIDI lane playing into one instrument node (anything with a note input).
         int index = -1;
         if (!ReadNodeIndex(params, "instrument", "index", index, outError))
            return false;
         GraphNode* inst = FindNodeByIndex(index);
         const size_t undoDepth = gUndoStack.size();
         PushUndoCheckpoint();
         int lane = -1;
         if (!ArrangeAddMidiLane(inst->uid, params.value("name", std::string()), lane, outError))
         {
            DropUnusedCheckpoint(undoDepth);
            return false;
         }
         gPatchDirty = true;
         outResult = { {"lane", lane} };
         return true;
      }
      else if (method == "set_midi_notes" || method == "get_midi_notes")
      {
         if (!params.contains("clipId") || !params["clipId"].is_number_integer())
         {
            outError = "give the integer clipId of a MIDI clip (list_clips lists them)";
            return false;
         }
         const uint64_t clipId = params["clipId"].get<uint64_t>();
         if (method == "get_midi_notes")
         {
            outResult = ArrangeMidiNotesJson(clipId);
            if (outResult.is_null() || (outResult.is_object() && outResult.contains("error")))
            {
               outError = outResult.is_object() ? outResult.value("error", std::string("not a MIDI clip")) : "not a MIDI clip";
               return false;
            }
            return true;
         }
         if (!params.contains("notes") || !params["notes"].is_array())
         {
            outError = "'notes' must be an array of {t, dur, midi, vel}";
            return false;
         }
         const size_t undoDepth = gUndoStack.size();
         PushUndoCheckpoint();
         if (!ArrangeSetMidiNotes(clipId, params["notes"], outError))
         {
            DropUnusedCheckpoint(undoDepth);
            return false;
         }
         gPatchDirty = true;
         outResult = { {"notes", params["notes"].size()} };
         return true;
      }
      else if (method == "capture_notes" || method == "capture_status")
      {
         // A note source's output over a range, captured offline into a MIDI
         // clip on a new MIDI lane. Start-then-poll; the MCP tool waits.
         if (method == "capture_notes")
         {
            int index = -1;
            if (!ReadNodeIndex(params, "node", "index", index, outError))
               return false;
            uint64_t instrumentUid = 0;
            if (params.contains("instrument"))
            {
               int inst = -1;
               if (!ReadNodeIndex(params, "instrument", nullptr, inst, outError))
                  return false;
               instrumentUid = FindNodeByIndex(inst)->uid;
            }
            Transport& tr = Transport::Instance();
            double start = params.value("startBeats", -1.0), end = params.value("endBeats", -1.0);
            if (start < 0.0 || end <= start)
            {
               if (tr.LoopEnabled())
               {
                  start = tr.LoopStartBeats();
                  end = tr.LoopEndBeats();
               }
               else
               {
                  start = std::max(0.0, tr.Beats());
                  end = start + 4.0 * tr.BeatsPerBar();
               }
            }
            // The result lands later; the checkpoint taken now is the state it undoes to.
            const size_t undoDepth = gUndoStack.size();
            PushUndoCheckpoint();
            if (!StartCaptureNotesToTimeline(index, instrumentUid, start, end, outError))
            {
               DropUnusedCheckpoint(undoDepth);
               return false;
            }
            gPatchDirty = true;
         }
         const CaptureNotesStatus& st = CaptureNotesState();
         outResult = { {"active", st.active}, {"done", st.done}, {"ok", st.ok}, {"progress", std::round(st.progress * 100.0f) / 100.0f},
                       {"message", st.message}, {"lane", st.laneIndex}, {"clipId", st.clipId}, {"noteCount", st.noteCount} };
         return true;
      }
      else if (method == "render_audio_to_file" || method == "render_file_status")
      {
         // A node's sound, rendered offline (faster than real time, sample-
         // accurate, from a clean start) into a WAV. Start-then-poll.
         if (method == "render_audio_to_file")
         {
            int index = -1;
            if (!ReadNodeIndex(params, "node", "index", index, outError))
               return false;
            const double start = params.value("start", 0.0), seconds = params.value("seconds", 0.0);
            if (start < 0.0 || !(seconds > 0.0))
            {
               outError = "give start >= 0 and seconds > 0";
               return false;
            }
            const int bits = params.value("bitDepth", 24);
            if (bits != 16 && bits != 24 && bits != 32)
            {
               outError = "bitDepth must be 16, 24 or 32 (32 = float)";
               return false;
            }
            if (!StartRenderToFile(index, start, seconds, std::clamp(params.value("tail", 0.0), 0.0, 60.0),
                                   params.value("path", std::string()), bits, params.value("sampleRate", 0.0), outError))
               return false;
         }
         const RenderToFileStatus& st = RenderToFileState();
         outResult = { {"active", st.active}, {"done", st.done}, {"ok", st.ok}, {"progress", std::round(st.progress * 100.0f) / 100.0f},
                       {"message", st.message}, {"path", st.path}, {"seconds", st.seconds}, {"sampleRate", st.sampleRate},
                       {"channels", st.channels}, {"peakDb", std::round(st.peakDb * 10.0f) / 10.0f}, {"rmsDb", std::round(st.rmsDb * 10.0f) / 10.0f} };
         return true;
      }
      else if (method == "press_button")
      {
         // A node's own momentary button (Looper rec, Audio Out record, ...),
         // exactly as clicking it. Without "button": the node's button names.
         int index = -1;
         if (!ReadNodeIndex(params, "node", "index", index, outError))
            return false;
         GraphNode* gn = FindNodeByIndex(index);
         json names = json::array();
         for (const std::string& b : gn->node->ButtonNames())
            names.push_back(b);
         const std::string button = params.value("button", std::string());
         if (button.empty())
         {
            outResult = { {"buttons", names} };
            return true;
         }
         if (RefuseIfLocked(index, outError))
            return false;
         // Several buttons change the patch (record into a sampler, Molder's
         // roll, Drum Sequencer randomize): one undo step, dropped again when
         // the press is refused.
         const size_t undoDepth = gUndoStack.size();
         PushUndoCheckpoint();
         std::string err;
         if (!gn->node->PressButton(button, err))
         {
            DropUnusedCheckpoint(undoDepth);
            outError = (err.empty() ? std::string("could not press '") + button + "'" : err) +
                       (names.empty() ? std::string(" (this node has no buttons)") : " (buttons: " + names.dump() + ")");
            return false;
         }
         gPatchDirty = true;
         outResult = { {"pressed", button} };
         return true;
      }
      else if (method == "get_sound_profile")
      {
         // A Sound Analyze node's last result: partials, envelope, spectrum.
         const int index = params.value("index", -1);
         GraphNode* gn = FindNodeByIndex(index);
         auto* sa = gn != nullptr ? dynamic_cast<SoundAnalyzeNode*>(gn->node.get()) : nullptr;
         if (sa == nullptr)
         {
            outError = "node " + std::to_string(index) + " is not a Sound Analyze node";
            return false;
         }
         if (const nlohmann::json* profile = sa->ProfileJson())
            outResult = *profile;
         else
            outResult = { {"status", sa->Status().empty() ? std::string("no analysis yet - give it a file (set_param path)") : sa->Status()} };
         return true;
      }
      else if (method == "fit_view")
      {
         gRequestFitView = true;
         outResult = json::object();
         return true;
      }
      else if (method == "fit_view_node")
      {
         const int index = params.value("index", -1);
         if (FindNodeByIndex(index) == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         gRequestFitViewNodeIndex = index;
         outResult = json::object();
         return true;
      }
      else if (method == "expand_node")
      {
         const int index = params.value("index", -1);
         GraphNode* gn = FindNodeByIndex(index);
         if (gn == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         // Documentation screenshots: both "params start collapsed" flags
         // (GraphNode.h) default off so a fresh node leads with its preview -
         // force them open so the full param body actually renders.
         gn->showParams = true;
         gn->showAdvancedParams = true;
         outResult = json::object();
         return true;
      }
      else if (method == "auto_wire_inputs")
      {
         // Feeds every otherwise-unconnected input slot on a node with a
         // plausible source, since most filter/effect/operator nodes render
         // blank with nothing wired in. Spawns one shared feeder per slot
         // *kind* needed (image/audio/note/geometry/modulator), tucked far
         // off in canvas space so it never intrudes on the target's own
         // on-screen rect that screenshot_node crops to - fit_view_node
         // frames only the target node regardless of where else on the
         // canvas a feeder sits.
         //
         // ConnectNodes() below re-validates every guess through the same
         // IsInputSlotCompatible() the UI itself uses, so a wrong guess here
         // just fails to wire that slot rather than mis-wiring it - this
         // deliberately doesn't cover Render3DNode's camera/light/environment
         // slots or SetColorNode's palette slot, both multi-kind special
         // cases not worth the complexity here.
         const int index = params.value("index", -1);
         GraphNode* gn = FindNodeByIndex(index);
         if (gn == nullptr)
         {
            outError = "unknown node index";
            return false;
         }

         std::map<std::string, int> feederIndexByType;
         // SpawnNode() push_backs onto gNodes, which can reallocate and
         // invalidate every GraphNode* into it - including whatever the
         // caller was holding. ensureFeeder always re-resolves through
         // FindNodeByIndex(), and callers below re-resolve `target` after
         // calling it, precisely so no pointer is held across a spawn.
         auto ensureFeeder = [&](const std::string& typeName,
                                 const std::string& category) -> GraphNode*
         {
            auto it = feederIndexByType.find(typeName);
            if (it != feederIndexByType.end())
               return FindNodeByIndex(it->second);
            GraphNode* feeder = SpawnNode(typeName, category, -8000.0f,
                                          -8000.0f + (float)feederIndexByType.size() * 400.0f);
            if (feeder != nullptr)
               feederIndexByType[typeName] = feeder->index;
            return feeder;
         };

         json wired = json::array();
         const int inputs = InputCountFor(*gn);
         for (int slot = 0; slot < inputs; slot++)
         {
            GraphNode* target = FindNodeByIndex(index); // re-resolve: a prior iteration may have spawned a feeder
            if (target == nullptr)
               break;

            GraphNode* feeder = nullptr;
            if (AudioCable* cable = target->node->AudioInputSlot(slot))
            {
               if (cable->IsConnected())
                  continue;
               feeder = ensureFeeder("Wavetable", "Synths");
            }
            else if (NoteCable* cable = target->node->NoteInputSlot(slot))
            {
               if (cable->IsConnected())
                  continue;
               feeder = ensureFeeder("MIDI Notes", "Notes");
            }
            else if (IGeometrySource** field = target->node->GeometryInputSlot(slot))
            {
               if (*field != nullptr)
                  continue;
               feeder = ensureFeeder("Sphere", "3D");
            }
            else if (IModulator** field = target->node->ModulatorInputSlot(slot))
            {
               if (*field != nullptr)
                  continue;
               feeder = ensureFeeder("Envelope", "Modulators");
            }
            else if (ImageCable* cable = CableFor(*gn, slot))
            {
               if (cable->IsConnected())
                  continue;
               feeder = ensureFeeder("Shape", "Source");
            }
            if (feeder == nullptr)
               continue;

            std::string ignoredErr;
            if (ConnectNodes(feeder->index, 0, gn->index, slot, ignoredErr))
               wired.push_back(slot);
         }

         json feederIndices = json::array();
         for (const auto& kv : feederIndexByType)
            feederIndices.push_back(kv.second);
         outResult = { {"wiredSlots", wired}, {"feederIndices", feederIndices} };
         return true;
      }
      else if (method == "load_patch_text")
      {
         // Patch source in the request, no temp file. Applied as one undo step.
         Patch::Data data;
         std::string readError;
         if (!Patch::ReadText(params.value("text", std::string()), data, readError))
         {
            outError = readError;
            return false;
         }
         if (!LoadPatchDataImpl(data, std::string(), true))
         {
            outError = gPatchStatus;
            return false;
         }
         outResult = { {"nodes", (int)gNodes.size()}, {"status", gPatchStatus} };
         return true;
      }
      else if (method == "validate_patch_text")
      {
         // The strict check the CLI runs before a load, against patch source
         // held in the request. Nothing on the canvas changes.
         Patch::Data data;
         std::string readError;
         std::vector<Headless::Issue> errors, warnings;
         if (!Patch::ReadText(params.value("text", std::string()), data, readError))
            errors.push_back({ "E_LOAD", readError, 0, -1 });
         else
         {
            const PatchSchema::Env env = MakeSchemaEnv(false);
            PatchSchema::Resolve(data, env, errors);
            if (errors.empty())
               PatchSchema::ResolveKeys(data, env, errors);
            if (errors.empty())
               PatchSchema::Validate(data, env, errors, warnings);
            if (params.value("strict", true))
               Headless::PromoteWarnings(warnings, errors);
         }
         auto issuesJson = [](const std::vector<Headless::Issue>& v)
         {
            json a = json::array();
            for (const Headless::Issue& is : v)
               a.push_back({ {"code", is.code}, {"message", is.message}, {"line", is.line}, {"node", is.node}, {"hint", is.hint} });
            return a;
         };
         outResult = { {"ok", errors.empty()}, {"errors", issuesJson(errors)}, {"warnings", issuesJson(warnings)},
                       {"nodes", (int)data.nodes.size()} };
         return true;
      }
      else if (method == "describe")
      {
         // The static schema of one node type (or the list of types when none is
         // given). Control ranges need a drawn node, so they are the CLI's job:
         // `Infinite --describe <type> --json`.
         // `params` lists saved keys only; the modulatable parameter indices and
         // keys `bind_modulator` takes are in get_bindings' `params` for a node
         // that has drawn.
         const std::string type = params.value("type", std::string());
         if (type.empty())
         {
            json cats = json::object();
            for (const std::string& cat : NodeFactory::Instance().GetCategories())
               cats[cat] = NodeFactory::Instance().GetNodesInCategory(cat);
            outResult = { {"types", cats} };
            return true;
         }
         const PatchSchema::TypeSchema* ts = SchemaFor(type);
         if (ts == nullptr)
         {
            outError = "unknown node type '" + type + "'";
            return false;
         }
         json pj = json::array(), in = json::array(), out = json::array();
         for (const auto& pi : ts->params)
            pj.push_back({ {"key", pi.key}, {"kind", std::string(1, pi.kind)}, {"default", pi.def} });
         const std::vector<std::string> slotNames = PatchSchema::SlotNames(*ts);
         for (size_t i = 0; i < ts->inputs.size(); i++)
            in.push_back({ {"slot", ts->inputs[i].slot}, {"name", slotNames[i]}, {"kind", ts->inputs[i].kind},
                           {"label", ts->inputs[i].label} });
         for (const auto& oi : ts->outputs)
            out.push_back({ {"label", oi.label}, {"kind", oi.kind}, {"modulator", oi.modulator} });
         outResult = { {"type", ts->name}, {"category", ts->category}, {"params", pj}, {"inputs", in}, {"outputs", out},
                       {"canBypass", ts->canBypass}, {"hardwareDriven", ts->hardwareDriven} };
         // The node's reference text (English, as in the Help window): what it
         // does, its controls' units and what each output's 0..1 means.
         if (const char* help = NodeHelpTextFor(ts->name, ts->category))
            outResult["help"] = help;
         return true;
      }
      else if (method == "batch")
      {
         // Array of {method, params} run in one frame, all or nothing, as ONE
         // undo step. On the first failure everything already done is undone.
         // Node indices are renumbered by that undo (ApplyPatchData remaps), so
         // a caller that rolled back should re-read get_graph.
         //
         // A create_node with "id": "<label>" lets every later call in the same
         // batch name that node as "$<label>" wherever a node index goes. The
         // labels live only for this batch: indices are reused after a delete
         // or undo, so a label kept beyond it could point at the wrong node.
         if (!params.contains("calls") || !params["calls"].is_array())
         {
            outError = "batch needs a 'calls' array of {method, params}";
            return false;
         }
         PushUndoCheckpoint(); // the single checkpoint for the whole batch
         const size_t undoDepth = gUndoStack.size();
         const bool wasSuppressed = gSuppressUndoCheckpoints;
         gSuppressUndoCheckpoints = true; // calls inside must not add their own
         json results = json::array();
         std::map<std::string, int> labels;
         std::string failMsg;
         int failAt = -1;
         for (size_t i = 0; i < params["calls"].size() && failAt < 0; i++)
         {
            const json& call = params["calls"][i];
            const std::string m = call.is_object() ? call.value("method", std::string()) : std::string();
            // load_patch_text replaces the whole graph and clears the undo
            // suppression this batch relies on (ApplyPatchData).
            // transport and the measuring calls act outside the patch, so a
            // failed batch's undo could not take them back.
            if (m.empty() || m == "batch" || m == "undo" || m == "redo" || m == "new_patch" || m == "load_patch" ||
                m == "load_patch_text" || m == "transport" || m == "render_to_track" || m == "capture_notes" || m == "render_audio_to_file" || m == "press_button" || IsAudioProbeRpc(m))
            {
               failMsg = "method '" + m + "' is not allowed inside a batch";
               failAt = (int)i;
               break;
            }
            json callParams = call.contains("params") ? call["params"] : json::object();
            json r;
            std::string e;
            bool ok = SubstituteRefs(callParams, labels, e);
            if (ok && m == "create_node" && callParams.contains("id"))
            {
               const json& id = callParams["id"];
               const std::string label = id.is_string() ? id.get<std::string>() : std::string();
               bool valid = !label.empty() && !std::isdigit((unsigned char)label[0]);
               for (char ch : label)
                  valid = valid && (std::isalnum((unsigned char)ch) || ch == '_');
               if (!valid)
               {
                  ok = false;
                  e = "id must start with a letter or _ and hold letters, digits and _";
               }
               else if (labels.count(label))
               {
                  ok = false;
                  e = "id '" + label + "' is already used in this batch";
               }
            }
            // An exception here must not skip the rollback below, or the
            // batch would leave a half-applied graph with undo suppressed.
            try
            {
               ok = ok && HandleRpcCommand(m, callParams, r, e);
            }
            catch (const std::exception& ex)
            {
               ok = false;
               e = std::string("bad params: ") + ex.what();
            }
            if (!ok)
            {
               failMsg = m + ": " + e;
               failAt = (int)i;
               break;
            }
            if (m == "create_node" && r.contains("id") && r["id"].is_string())
               labels[r["id"].get<std::string>()] = r["index"].get<int>();
            results.push_back(r);
         }
         gSuppressUndoCheckpoints = wasSuppressed;
         if (failAt >= 0)
         {
            if (gUndoStack.size() == undoDepth)
            {
               Undo();
               gRedoStack.clear(); // the rolled-back attempt is not something to redo
            }
            outError = "batch call " + std::to_string(failAt) + " failed, nothing applied - " + failMsg;
            return false;
         }
         outResult = { {"results", results} };
         return true;
      }
      else if (method == "get_bindings")
      {
         // Modulation cables, expressions and globals, with each destination's
         // key/label/range when known. With `index`, also that node's
         // modulatable parameters - what bind_modulator / set_expression take.
         JoinLiveTier1();
         const Patch::Data data = BuildPatchData();
         json mods = json::array(), exprs = json::array(), globs = json::array();
         for (const Patch::ModRecord& m : data.modulation)
         {
            json rec = { {"dst", m.dstIndex}, {"param", m.dstParam}, {"src", m.srcIndex}, {"srcOutput", m.srcOutput},
                         {"lo", m.lo}, {"hi", m.hi}, {"hasRange", m.hasRange}, {"enabled", m.enabled}, {"curve", m.curve} };
            if (const GraphNode* gn = FindNodeByIndex(m.dstIndex))
               DescribeParam(*gn, m.dstParam, rec);
            mods.push_back(rec);
         }
         for (const Patch::ExprRecord& e : data.expressions)
         {
            json rec = { {"dst", e.dstIndex}, {"param", e.dstParam}, {"text", e.text}, {"curve", e.curve} };
            if (const GraphNode* gn = FindNodeByIndex(e.dstIndex))
               DescribeParam(*gn, e.dstParam, rec);
            if (const std::string* err = Modulation::Instance().ExpressionErrorFor(e.dstIndex, e.dstParam))
               if (!err->empty())
                  rec["error"] = *err;
            if (Modulation::Instance().IsModulated(e.dstIndex, e.dstParam))
               rec["shadowedByModulator"] = true;
            exprs.push_back(rec);
         }
         for (const ExprGlobals::Global& g : ExprGlobals::All())
         {
            json rec = { {"name", g.name}, {"expr", g.expr}, {"value", g.value} };
            if (!g.error.empty())
               rec["error"] = g.error;
            globs.push_back(rec);
         }
         outResult = { {"modulation", mods}, {"expressions", exprs}, {"globals", globs} };
         if (params.contains("index"))
         {
            int index = -1;
            if (!ReadNodeIndex(params, "index", nullptr, index, outError))
               return false;
            const GraphNode* gn = FindNodeByIndex(index);
            json ps = json::array();
            for (const auto& kv : Modulation::Instance().AllKnownParams())
            {
               if (kv.second.nodeIndex != index)
                  continue;
               json rec = { {"param", kv.second.paramIndex} };
               DescribeParam(*gn, kv.second.paramIndex, rec);
               if (kv.second.isEnum)
                  rec["options"] = kv.second.enumOptions;
               ps.push_back(rec);
            }
            outResult["params"] = ps;
            if (ps.empty())
            {
               gRpcParamProbe.insert(index);
               outResult["paramsNote"] = "this node has not reported its parameters yet; ask again once (they are being read now)";
            }
         }
         return true;
      }
      else if (method == "bind_modulator")
      {
         // The live `mod` line: patch a modulator output onto a parameter.
         // Installed with RestoreLink like a patch load (Bind() reads this
         // frame's FrameParams, which the RPC drain runs before), so with no
         // lo/hi the range becomes the destination's full span on its first draw.
         int dst = -1, src = -1, param = -1, srcOutput = 0;
         if (!ReadNodeIndex(params, "dst", "dstIndex", dst, outError) ||
             !ReadNodeIndex(params, "src", "srcIndex", src, outError))
            return false;
         if (RefuseIfLocked(dst, outError))
            return false;
         if (dst == src)
         {
            outError = "a node cannot modulate its own parameter";
            return false;
         }
         const GraphNode* dstNode = FindNodeByIndex(dst);
         const GraphNode* srcNode = FindNodeByIndex(src);
         if (!params.contains("param"))
         {
            outError = "missing 'param'";
            return false;
         }
         if (!ReadParam(params["param"], *dstNode, param, outError))
            return false;
         if (params.contains("srcOutput") && !ReadOutput(params["srcOutput"], *srcNode, srcOutput, outError))
            return false;
         const PatchSchema::TypeSchema* ts = SchemaFor(srcNode->typeName);
         if (ts == nullptr || srcOutput < 0 || srcOutput >= (int)ts->outputs.size() || !ts->outputs[srcOutput].modulator)
         {
            outError = srcNode->typeName + " output " + std::to_string(srcOutput) +
                       " is not a modulator (use an LFO, Envelope, Random, Macro, Audio Analyze, ...)";
            return false;
         }
         const bool hasLo = params.contains("lo"), hasHi = params.contains("hi");
         if (hasLo != hasHi)
         {
            outError = "pass both 'lo' and 'hi', or neither for the full range";
            return false;
         }
         Modulation::Source source;
         source.nodeIndex = src;
         source.outputIndex = srcOutput;
         if (hasLo)
         {
            source.lo = params["lo"].get<float>();
            source.hi = params["hi"].get<float>();
            if (!std::isfinite(source.lo) || !std::isfinite(source.hi))
            {
               outError = "'lo' and 'hi' must be finite numbers";
               return false;
            }
            source.hasRange = true;
         }
         source.enabled = params.value("enabled", true);
         source.curve = std::clamp(params.value("curve", 0.0f), -1.0f, 1.0f);
         PushUndoCheckpoint();
         Modulation::Instance().RestoreLink(dst, param, source);
         outResult = { {"dst", dst}, {"param", param} };
         return true;
      }
      else if (method == "unbind_modulator")
      {
         int dst = -1, param = -1;
         if (!ReadNodeIndex(params, "dst", "dstIndex", dst, outError))
            return false;
         if (RefuseIfLocked(dst, outError))
            return false;
         if (!params.contains("param"))
         {
            outError = "missing 'param'";
            return false;
         }
         if (!ReadParam(params["param"], *FindNodeByIndex(dst), param, outError))
            return false;
         if (!Modulation::Instance().IsModulated(dst, param))
         {
            outError = "nothing modulates param " + std::to_string(param) + " of node " + std::to_string(dst);
            return false;
         }
         PushUndoCheckpoint();
         Modulation::Instance().Unbind(dst, param);
         outResult = json::object();
         return true;
      }
      else if (method == "set_expression")
      {
         // The live `expr` line. Empty text removes the expression. A wired
         // modulator on the same parameter wins over it (reported, not refused).
         int dst = -1, param = -1;
         if (!ReadNodeIndex(params, "dst", "dstIndex", dst, outError))
            return false;
         if (RefuseIfLocked(dst, outError))
            return false;
         if (!params.contains("param"))
         {
            outError = "missing 'param'";
            return false;
         }
         if (!ReadParam(params["param"], *FindNodeByIndex(dst), param, outError))
            return false;
         std::string text = params.value("text", std::string());
         if (!text.empty() && text[0] == '=')
            text.erase(0, 1);
         PushUndoCheckpoint();
         if (text.empty())
            Modulation::Instance().ClearExpression(dst, param);
         else
         {
            Modulation::Instance().SetExpression(dst, param, text);
            if (params.contains("curve"))
               Modulation::Instance().SetExpressionCurve(dst, param, params["curve"].get<float>());
            // As the in-node editor does: a looping recording left underneath
            // would resurface once the expression is later removed.
            GestureRecorder::Instance().CancelArm(dst, param);
            GestureRecorder::Instance().StopPlayback(dst, param);
         }
         outResult = { {"dst", dst}, {"param", param} };
         if (Modulation::Instance().IsModulated(dst, param))
            outResult["shadowedByModulator"] = true;
         return true;
      }
      else if (method == "set_global")
      {
         // The live `glob` line: replaces a global of the same name in place
         // (its position matters - a global may read the ones above it), or
         // appends a new one.
         const std::string name = params.value("name", std::string());
         std::string expr = params.value("expr", std::string());
         if (!expr.empty() && expr[0] == '=')
            expr.erase(0, 1);
         std::string nameError;
         if (!ExprGlobals::IsValidName(name, nameError))
         {
            outError = nameError;
            return false;
         }
         if (expr.empty())
         {
            outError = "missing 'expr' (use remove_global to delete one)";
            return false;
         }
         PushUndoCheckpoint();
         std::vector<ExprGlobals::Global>& all = ExprGlobals::All();
         auto it = std::find_if(all.begin(), all.end(), [&](const ExprGlobals::Global& g) { return g.name == name; });
         if (it != all.end())
         {
            it->expr = expr;
            it->error.clear();
         }
         else
            all.push_back({ name, expr, 0.0f, std::string() });
         outResult = json::object();
         return true;
      }
      else if (method == "remove_global")
      {
         const std::string name = params.value("name", std::string());
         std::vector<ExprGlobals::Global>& all = ExprGlobals::All();
         auto it = std::find_if(all.begin(), all.end(), [&](const ExprGlobals::Global& g) { return g.name == name; });
         if (it == all.end())
         {
            outError = "no global named '" + name + "'";
            return false;
         }
         PushUndoCheckpoint();
         all.erase(it);
         outResult = json::object();
         return true;
      }
      else if (method == "save_patch")
      {
         const std::string path = params.value("path", std::string());
         if (!SavePatchTo(path))
         {
            outError = gPatchStatus;
            return false;
         }
         outResult = json::object();
         return true;
      }
      else if (method == "load_patch")
      {
         const std::string path = params.value("path", std::string());
         if (!LoadPatchFrom(path))
         {
            outError = gPatchStatus;
            return false;
         }
         outResult = json::object();
         return true;
      }
      else if (method == "new_patch")
      {
         NewPatch();
         outResult = json::object();
         return true;
      }
      else if (IsAudioProbeRpc(method))
         return HandleAudioProbeRpc(method, params, outResult, outError);
      else if (method == "transport")
      {
         // Play/stop/seek for clients that compare sources: starting both from
         // 0 is what makes two captures line up. "play" also starts the audio
         // engine, which no longer opens at launch.
         Transport& tr = Transport::Instance();
         const std::string action = params.value("action", std::string("state"));
         std::string warning;
         auto ensureAudio = [&]()
         {
            if (AudioEngine::Instance().SampleRate() > 0.0)
               return;
            gAudioStartError.clear();
            if (!StartAudioEngine(gAudioStartError))
               warning = "the audio device could not start: " + gAudioStartError;
         };
         if (params.contains("bpm"))
         {
            const double bpm = params["bpm"].is_number() ? params["bpm"].get<double>() : 0.0;
            if (bpm < 20.0 || bpm > 400.0)
            {
               outError = "bpm must be a number from 20 to 400";
               return false;
            }
            tr.SetTempo((float)bpm);
         }
         if (action == "play" || action == "resume")
         {
            // resume = play on from where it was paused (play does the same
            // unless fromStart); the position is never reset by a pause.
            if (action == "play" && params.value("fromStart", false))
               tr.Rewind();
            ensureAudio();
            tr.SetPlaying(true);
         }
         else if (action == "pause")
            tr.SetPlaying(false); // keeps the position: resume carries on from here
         else if (action == "stop")
         {
            // Stop = pause, and back to the start with rewind:true.
            tr.SetPlaying(false);
            if (params.value("rewind", false))
               tr.Rewind();
         }
         else if (action == "toggle")
         {
            if (!tr.IsPlaying())
               ensureAudio();
            tr.TogglePlay();
         }
         else if (action == "rewind")
            tr.Rewind();
         else if (action == "seek")
         {
            if (params.contains("seconds") && params["seconds"].is_number())
               tr.Seek(std::max(0.0, params["seconds"].get<double>()));
            else if (params.contains("beats") && params["beats"].is_number())
               tr.SeekBeats(std::max(0.0, params["beats"].get<double>()));
            else
            {
               outError = "seek needs 'seconds' or 'beats'";
               return false;
            }
         }
         else if (action == "start_audio")
            ensureAudio();
         else if (action != "state")
         {
            outError = "unknown action '" + action + "' (play, pause, resume, stop, toggle, rewind, seek, start_audio, state)";
            return false;
         }
         outResult = { { "playing", tr.IsPlaying() },
                       { "seconds", std::round(tr.Seconds() * 1000.0) / 1000.0 },
                       { "beats", std::round(tr.Beats() * 1000.0) / 1000.0 },
                       { "bpm", tr.Tempo() },
                       { "audioRunning", AudioEngine::Instance().SampleRate() > 0.0 },
                       { "sampleRate", AudioEngine::Instance().SampleRate() },
                       { "loop", { { "enabled", tr.LoopEnabled() }, { "startBeats", tr.LoopStartBeats() }, { "endBeats", tr.LoopEndBeats() } } } };
         if (!warning.empty())
            outResult["warning"] = warning;
         return true;
      }
      else if (method == "undo")
      {
         Undo();
         outResult = json::object();
         return true;
      }
      else if (method == "redo")
      {
         Redo();
         outResult = json::object();
         return true;
      }
      else if (method == "screenshot_node")
      {
         // Documentation tooling: crop a PNG to exactly one node's on-screen
         // rect. Reads GL_FRONT (the last completed, already-swapped frame)
         // rather than GL_BACK, since this handler runs at the top of the
         // next frame (see RemoteControl::DrainPending's call site) - the
         // back buffer at that point holds stale/undefined content, while
         // front always holds what's actually on screen right now. Callers
         // should fit_view and let a couple of real frames pass first so the
         // node has settled into view and finished cooking.
         const int index = params.value("index", -1);
         const std::string path = params.value("path", std::string());
         GraphNode* gn = FindNodeByIndex(index);
         if (gn == nullptr)
         {
            outError = "unknown node index";
            return false;
         }
         if (path.empty())
         {
            outError = "missing path";
            return false;
         }

         ed::EditorContext* prevEditor = ed::GetCurrentEditor();
         ed::SetCurrentEditor(gEditor);
         const ImVec2 canvasPos = ed::GetNodePosition(gn->NodeId());
         const ImVec2 canvasSize = ed::GetNodeSize(gn->NodeId());
         const ImVec2 screenMin = ed::CanvasToScreen(canvasPos);
         const ImVec2 screenMax =
            ed::CanvasToScreen(ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y));
         ed::SetCurrentEditor(prevEditor);

         GLFWwindow* mainWin = glfwGetCurrentContext();
         int fbW = 0, fbH = 0;
         glfwGetFramebufferSize(mainWin, &fbW, &fbH);
         // ImGui points -> framebuffer pixels. fb/window alone misses the point scale that
         // Windows/X11 apply on hi-DPI monitors (core/UiScale.h).
         const float scaleX = ImGui::GetIO().DisplayFramebufferScale.x;
         const float scaleY = ImGui::GetIO().DisplayFramebufferScale.y;

         const int pad = params.value("padding", 12);
         const int x0 = std::max(0, (int)std::floor(screenMin.x * scaleX) - pad);
         const int y0 = std::max(0, (int)std::floor(screenMin.y * scaleY) - pad);
         const int x1 = std::min(fbW, (int)std::ceil(screenMax.x * scaleX) + pad);
         const int y1 = std::min(fbH, (int)std::ceil(screenMax.y * scaleY) + pad);
         const int cropW = x1 - x0;
         const int cropH = y1 - y0;
         if (cropW <= 0 || cropH <= 0)
         {
            outError = "node has no visible on-screen rect to capture";
            return false;
         }

         std::vector<unsigned char> full((size_t)fbW * fbH * 4);
         glReadBuffer(GL_FRONT);
         glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, full.data());

         // full[] is OpenGL's bottom-up row order; x0/y0/x1/y1 are top-down
         // screen-space. Flip per row while cropping instead of flipping the
         // whole framebuffer first.
         std::vector<unsigned char> crop((size_t)cropW * cropH * 4);
         for (int row = 0; row < cropH; ++row)
         {
            const int srcY = fbH - 1 - (y0 + row);
            std::memcpy(&crop[(size_t)row * cropW * 4], &full[((size_t)srcY * fbW + x0) * 4],
                        (size_t)cropW * 4);
         }
         stbi_write_png(path.c_str(), cropW, cropH, 4, crop.data(), cropW * 4);
         outResult = { {"width", cropW}, {"height", cropH} };
         return true;
      }

      else if (method == "render_frame")
      {
         // The live counterpart of `--frame`: writes the Output's current image (what the last
         // cooked frame produced, at the Output's own size) through the same ExportImage and
         // reports the same per-frame numbers. It does not seek the transport or step time, so
         // a running patch is left exactly as it was; use --frame for a frame at a given time.
         const std::string path = params.value("path", std::string());
         if (path.empty())
         {
            outError = "missing path";
            return false;
         }
         std::vector<int> outs;
         for (GraphNode& gn : gNodes)
            if (gn.typeName == "Output")
               outs.push_back(gn.index);
         if (outs.empty())
         {
            outError = "the patch has no Output node";
            return false;
         }
         int outIndex = params.value("output", -1);
         if (outIndex < 0 && outs.size() > 1)
         {
            outError = "the patch has " + std::to_string(outs.size()) + " Output nodes; pass output = node index";
            return false;
         }
         if (outIndex < 0)
            outIndex = outs.front();
         GraphNode* og = FindNodeByIndex(outIndex);
         if (og == nullptr || og->typeName != "Output")
         {
            outError = "output is not an Output node index";
            return false;
         }
         OutputNode* out = static_cast<OutputNode*>(og->node.get());
         const int w = out->GetOutputWidth(), h = out->GetOutputHeight();
         if (w <= 0 || h <= 0)
         {
            outError = "the Output produced no image (is its input connected?)";
            return false;
         }
         std::error_code ec;
         const std::filesystem::path parent = std::filesystem::path(path).parent_path();
         if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
         std::vector<unsigned char> pixels;
         ExportImage(out, path, 90, &pixels);
         if (!std::filesystem::exists(path, ec))
         {
            outError = "could not write " + path;
            return false;
         }
         const ColorStats::FrameSummary fs = ColorStats::SummarizeRgba8(pixels.data(), w, h);
         // maxSize: rewrite the file as a preview no larger than that on its long
         // side (an assistant looking at the frame needs ~1000 px, not 4K).
         // Stats above stay those of the full frame. `pixels` is GL's bottom-up
         // order, which ExportImage writes with the flip it leaves enabled.
         outResult = json::object();
         const int maxSize = params.value("maxSize", 0);
         if (maxSize > 0 && std::max(w, h) > maxSize)
         {
            const float s = (float)maxSize / (float)std::max(w, h);
            const int nw = std::max(1, (int)std::lround(w * s)), nh = std::max(1, (int)std::lround(h * s));
            std::vector<unsigned char> small = ResizeRgba8(pixels.data(), w, h, nw, nh);
            stbi_flip_vertically_on_write(1);
            std::string lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (lower.size() >= 4 && (lower.rfind(".jpg") == lower.size() - 4 || lower.rfind(".jpeg") == lower.size() - 5))
            {
               std::vector<unsigned char> rgb((size_t)nw * nh * 3);
               for (size_t i = 0; i < (size_t)nw * nh; i++)
                  std::memcpy(&rgb[i * 3], &small[i * 4], 3);
               stbi_write_jpg(path.c_str(), nw, nh, 3, rgb.data(), 85);
            }
            else
               stbi_write_png(path.c_str(), nw, nh, 4, small.data(), nw * 4);
            outResult["previewWidth"] = nw;
            outResult["previewHeight"] = nh;
         }
         outResult.update({ { "file", path }, { "width", w }, { "height", h }, { "output", outIndex },
                       { "frame_stats", json::parse("{" + ColorStats::FrameSummaryJsonFields(fs) + "}") } });
         if (fs.blackPercent >= 100.0)
            outResult["warning"] = "W_BLACK_FRAME: the frame is entirely black";
         return true;
      }

      outError = "unknown method '" + method + "'";
      return false;
   }
}
