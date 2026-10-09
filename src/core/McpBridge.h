#pragma once

// `Infinite --mcp`: a Model Context Protocol server on stdin/stdout that lets an
// MCP client (Claude Code, the in-app assistant panel) drive a RUNNING Infinite.
// It holds no app state of its own - every tool call is relayed as one request
// to that instance's RemoteControl server (127.0.0.1, see RemoteControl.h and
// docs/reference/rpc.md) - so it runs before any window, GL or audio setup and
// exits when stdin closes.
namespace McpBridge
{
   // Serves until stdin reaches EOF. Returns the process exit code.
   int Run();
}
