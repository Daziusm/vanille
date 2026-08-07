# Vanille MCP Server

Stdio MCP server that talks to a **running Vanille client** through the local bridge at:

`%LOCALAPPDATA%\Vanille\mcp\`

Vanille must be injected and running. The bridge processes requests from `requests/` and writes responses to `responses/`.

## Setup

```powershell
cd D:\Vanille\mcp-server
npm install
npm run build
```

## Cursor MCP config

Add to your Cursor MCP settings (`.cursor/mcp.json` or Cursor Settings → MCP):

```json
{
  "mcpServers": {
    "vanille": {
      "command": "node",
      "args": ["D:/Vanille/mcp-server/dist/index.js"]
    }
  }
}
```

Use your actual path to `dist/index.js`.

## Tools

| Tool | Description |
|------|-------------|
| `vanille_status` | Check if Vanille is alive, game id, lua ready |
| `explorer_refresh` | Rebuild + export explorer tree from live datamodel |
| `explorer_snapshot` | Read snapshot metadata (optional full tree) |
| `explorer_find` | Find instance by dot path (`game.Workspace.Players`) |
| `explorer_search` | Search by name/path/class |
| `lua_execute` | Run Lua in Vanille's VM |
| `lua_console` | Read recent Lua console output |

## Typical workflow

1. Launch Roblox + inject Vanille
2. In Cursor, call `vanille_status`
3. Call `explorer_refresh` (first time or after joining a game)
4. Use `explorer_find` / `explorer_search` to inspect structure
5. Use `lua_execute` to run scripts against the live game bridge

## Files

- `status.json` — heartbeat from Vanille (updated every second)
- `explorer_snapshot.json` — exported instance tree
- `requests/*.json` — commands from MCP server
- `responses/*.json` — command results from Vanille
