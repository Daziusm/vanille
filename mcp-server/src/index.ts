import fs from "node:fs/promises";
import path from "node:path";
import os from "node:os";
import { randomUUID } from "node:crypto";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

type ExplorerNode = {
  address?: string;
  name?: string;
  class_name?: string;
  path?: string;
  children?: ExplorerNode[];
};

type ExplorerSnapshot = {
  generated_at_unix_ms?: number;
  game_id?: number;
  node_count?: number;
  truncated?: boolean;
  roots?: ExplorerNode[];
};

type VanilleStatus = {
  alive?: boolean;
  updated_at_unix_ms?: number;
  game_id?: number;
  lua_ready?: boolean;
  mcp_root?: string;
  explorer_snapshot_path?: string;
  explorer_snapshot_exists?: boolean;
};

type BridgeResponse = {
  id: string;
  ok: boolean;
  command?: string;
  error?: string;
  result?: unknown;
};

const DEFAULT_TIMEOUT_MS = 15000;

function mcpRoot(): string {
  const localAppData = process.env.LOCALAPPDATA;
  if (!localAppData) {
    return path.join(os.tmpdir(), "Vanille", "mcp");
  }
  return path.join(localAppData, "Vanille", "mcp");
}

function requestsDir(): string {
  return path.join(mcpRoot(), "requests");
}

function responsesDir(): string {
  return path.join(mcpRoot(), "responses");
}

function statusPath(): string {
  return path.join(mcpRoot(), "status.json");
}

function explorerSnapshotPath(): string {
  return path.join(mcpRoot(), "explorer_snapshot.json");
}

async function ensureDirs(): Promise<void> {
  await fs.mkdir(requestsDir(), { recursive: true });
  await fs.mkdir(responsesDir(), { recursive: true });
}

async function readJsonFile<T>(filePath: string): Promise<T | null> {
  try {
    const raw = await fs.readFile(filePath, "utf8");
    return JSON.parse(raw) as T;
  } catch {
    return null;
  }
}

async function waitForResponse(id: string, timeoutMs: number): Promise<BridgeResponse> {
  const responseFile = path.join(responsesDir(), `${id}.json`);
  const deadline = Date.now() + timeoutMs;

  while (Date.now() < deadline) {
    const response = await readJsonFile<BridgeResponse>(responseFile);
    if (response) {
      await fs.rm(responseFile, { force: true }).catch(() => undefined);
      return response;
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }

  throw new Error(`Timed out waiting for Vanille response (${id}). Is vanille.exe running and injected?`);
}

async function sendCommand(command: string, args: Record<string, unknown> = {}, timeoutMs = DEFAULT_TIMEOUT_MS): Promise<BridgeResponse> {
  await ensureDirs();
  const id = randomUUID();
  const requestFile = path.join(requestsDir(), `${id}.json`);
  await fs.writeFile(
    requestFile,
    JSON.stringify({ id, command, args }, null, 2),
    "utf8",
  );
  return waitForResponse(id, timeoutMs);
}

async function getStatus(): Promise<VanilleStatus> {
  const status = await readJsonFile<VanilleStatus>(statusPath());
  if (!status) {
    return { alive: false };
  }
  return status;
}

async function loadExplorerSnapshot(): Promise<ExplorerSnapshot | null> {
  return readJsonFile<ExplorerSnapshot>(explorerSnapshotPath());
}

function walkNodes(nodes: ExplorerNode[] | undefined, visit: (node: ExplorerNode) => void): void {
  if (!nodes) {
    return;
  }
  for (const node of nodes) {
    visit(node);
    walkNodes(node.children, visit);
  }
}

function findNodeByPath(snapshot: ExplorerSnapshot, dotPath: string): ExplorerNode | null {
  let found: ExplorerNode | null = null;
  walkNodes(snapshot.roots, (node) => {
    if (node.path === dotPath) {
      found = node;
    }
  });
  return found;
}

function searchNodes(
  snapshot: ExplorerSnapshot,
  query: string,
  className?: string,
  limit = 50,
): ExplorerNode[] {
  const needle = query.toLowerCase();
  const classNeedle = className?.toLowerCase();
  const results: ExplorerNode[] = [];

  walkNodes(snapshot.roots, (node) => {
    if (results.length >= limit) {
      return;
    }
    const name = (node.name ?? "").toLowerCase();
    const pathValue = (node.path ?? "").toLowerCase();
    const classValue = (node.class_name ?? "").toLowerCase();
    const nameMatch = !query || name.includes(needle) || pathValue.includes(needle);
    const classMatch = !classNeedle || classValue.includes(classNeedle);
    if (nameMatch && classMatch) {
      results.push(node);
    }
  });

  return results;
}

function summarizeNode(node: ExplorerNode) {
  return {
    address: node.address,
    name: node.name,
    class_name: node.class_name,
    path: node.path,
    child_count: node.children?.length ?? 0,
  };
}

const server = new McpServer({
  name: "vanille",
  version: "1.0.0",
});

server.tool(
  "vanille_status",
  "Check whether Vanille is running and read bridge status (game id, lua ready, explorer snapshot path).",
  {},
  async () => {
    const status = await getStatus();
    return {
      content: [{ type: "text", text: JSON.stringify(status, null, 2) }],
    };
  },
);

server.tool(
  "explorer_refresh",
  "Ask the running Vanille client to rebuild and export the Roblox explorer snapshot.",
  {
    max_depth: z.number().int().positive().optional(),
    max_nodes: z.number().int().positive().optional(),
  },
  async ({ max_depth, max_nodes }) => {
    const response = await sendCommand("explorer_refresh", {
      ...(max_depth !== undefined ? { max_depth } : {}),
      ...(max_nodes !== undefined ? { max_nodes } : {}),
    }, 60000);

    if (!response.ok) {
      throw new Error(response.error ?? "explorer_refresh failed");
    }

    return {
      content: [{ type: "text", text: JSON.stringify(response.result, null, 2) }],
    };
  },
);

server.tool(
  "explorer_snapshot",
  "Read the latest exported explorer snapshot metadata. Set include_tree=true to return the full JSON (can be large).",
  {
    include_tree: z.boolean().optional(),
  },
  async ({ include_tree }) => {
    const snapshot = await loadExplorerSnapshot();
    if (!snapshot) {
      throw new Error(`No explorer snapshot found at ${explorerSnapshotPath()}. Run explorer_refresh first.`);
    }

    if (include_tree) {
      return {
        content: [{ type: "text", text: JSON.stringify(snapshot, null, 2) }],
      };
    }

    const summary = {
      generated_at_unix_ms: snapshot.generated_at_unix_ms,
      game_id: snapshot.game_id,
      node_count: snapshot.node_count,
      truncated: snapshot.truncated,
      path: explorerSnapshotPath(),
      root_count: snapshot.roots?.length ?? 0,
      roots: (snapshot.roots ?? []).map(summarizeNode),
    };

    return {
      content: [{ type: "text", text: JSON.stringify(summary, null, 2) }],
    };
  },
);

server.tool(
  "explorer_find",
  "Find an instance in the latest explorer snapshot by dot path (example: game.Workspace.Players).",
  {
    path: z.string().min(1),
    include_children: z.boolean().optional(),
  },
  async ({ path: dotPath, include_children }) => {
    const snapshot = await loadExplorerSnapshot();
    if (!snapshot) {
      throw new Error("Explorer snapshot missing. Run explorer_refresh first.");
    }

    const node = findNodeByPath(snapshot, dotPath);
    if (!node) {
      return {
        content: [{ type: "text", text: JSON.stringify({ found: false, path: dotPath }, null, 2) }],
      };
    }

    const payload = include_children ? node : summarizeNode(node);
    return {
      content: [{ type: "text", text: JSON.stringify({ found: true, node: payload }, null, 2) }],
    };
  },
);

server.tool(
  "explorer_search",
  "Search the latest explorer snapshot by name/path substring and optional class name.",
  {
    query: z.string().default(""),
    class_name: z.string().optional(),
    limit: z.number().int().positive().max(200).optional(),
  },
  async ({ query, class_name, limit }) => {
    const snapshot = await loadExplorerSnapshot();
    if (!snapshot) {
      throw new Error("Explorer snapshot missing. Run explorer_refresh first.");
    }

    const results = searchNodes(snapshot, query, class_name, limit ?? 50).map(summarizeNode);
    return {
      content: [{ type: "text", text: JSON.stringify({ count: results.length, results }, null, 2) }],
    };
  },
);

server.tool(
  "lua_execute",
  "Execute Lua inside the running Vanille VM (same environment as the in-game script editor).",
  {
    source: z.string().min(1),
    chunk_name: z.string().optional(),
  },
  async ({ source, chunk_name }) => {
    const response = await sendCommand("lua_execute", {
      source,
      ...(chunk_name ? { chunk_name } : {}),
    });

    if (!response.ok) {
      throw new Error(response.error ?? "lua_execute failed");
    }

    return {
      content: [{ type: "text", text: JSON.stringify(response.result, null, 2) }],
    };
  },
);

server.tool(
  "lua_console",
  "Read recent Lua console output from Vanille.",
  {
    max_lines: z.number().int().positive().max(1000).optional(),
  },
  async ({ max_lines }) => {
    const response = await sendCommand("lua_console", {
      ...(max_lines !== undefined ? { max_lines } : {}),
    });

    if (!response.ok) {
      throw new Error(response.error ?? "lua_console failed");
    }

    return {
      content: [{ type: "text", text: JSON.stringify(response.result, null, 2) }],
    };
  },
);

async function main(): Promise<void> {
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
