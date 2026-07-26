# Vanille Lua VM Reference

Vanille embeds a **sandboxed Lua runtime** (Lua 5.3 or 5.4 via `lua53-64.dll` / `lua54.dll`) inside the overlay. Scripts run in the Vanille process, not inside Roblox's Luau VM.

Use the **Lua Editor** and **Lua Console** windows in the menu to write, execute, and debug scripts. Scripts can also be saved to and loaded from the `scripts/` folder next to `vanille.exe`.

---

## Quick start

```lua
print("hello from vanille")

local tab = ui.create_tab("My Script")
ui.checkbox(tab, "enabled", "Enabled", true)

local square = Drawing.new("Square")
square.Size = { 100, 100 }
square.Position = { 200, 200 }
square.Color = { 1, 0, 0, 1 }
square.Filled = true
```

Press **Execute** in the Lua Editor to run the current buffer. Output appears in the Lua Console (`print` / `warn`).

Saved scripts live in:

```
<vanille.exe directory>/scripts/*.lua
```

On first launch, example scripts are created automatically: `example_lua_tab`, `ui_example`, and `lua_example`.

---

## Architecture

Each frame, the VM:

1. Syncs `workspace` children from the live game instance tree
2. Syncs `Players` from the player cache (names, IDs, characters, head positions)
3. Fires pending signal callbacks (`PlayerAdded`, `CharacterAdded`)
4. Runs `task.delay` callbacks whose time has elapsed
5. Runs UI button callbacks clicked that frame
6. Renders all `Drawing` objects on the overlay foreground

The `game` / `workspace` tree is a **read-mostly mirror**. It reflects cached game state; it does **not** let scripts call Roblox APIs or write game memory.

---

## Global environment

### Built-in globals

| Global | Description |
|--------|-------------|
| `print(...)` | Logs to the Lua Console (info) |
| `warn(...)` | Logs to the Lua Console (warning) |
| `loadstring(source [, chunk_name])` | Compiles a string into a function. Returns `function` or `nil, error` |
| `load(source)` | Alias of `loadstring` |
| `getgenv()` | Returns the global table `_G` |
| `game` | Root `DataModel` instance |
| `workspace` | Shortcut to `game:GetService("Workspace")` |

### Removed / unavailable (sandbox)

These standard Lua globals are **nil** and cannot be used:

- `io`, `os`, `package`, `debug`
- `dofile`, `loadfile`, `require`

There is no filesystem access, no `require` of external modules, and no direct memory or process APIs from Lua.

---

## Data model (`game` / `workspace`)

### Services

`game:GetService(name)` (or `game.get_service(name)`) returns a service instance. Service names are case-insensitive.

| Service | Class | Notes |
|---------|-------|-------|
| `Workspace` | `Workspace` | Top-level workspace children synced from the real game each frame |
| `Players` | `Players` | Player list synced from cache + Roblox `Players` service |
| `RunService` | `RunService` | Present as a service shell; no extra methods beyond the instance API |
| `UserInputService` | `UserInputService` | Present as a service shell |
| `Lighting` | `Lighting` | Present as a service shell |

`workspace` is also exposed as a global pointing at the Workspace service.

### Instance API

All instances (including `game`, services, players, parts) support:

**Properties** (get/set where noted):

| Property | Read | Write | Description |
|----------|------|-------|-------------|
| `Name` / `name` | ✓ | ✓ | Instance name |
| `ClassName` / `class_name` | ✓ | | Class name string |
| `Parent` / `parent` | ✓ | ✓ | Parent instance (`nil` to detach) |
| `Children` / `children` | ✓ | | Array of child instances |

**Player-specific:**

| Property | Description |
|----------|-------------|
| `Character` | Child model named `"Character"` (created/synced by the VM) |
| `LocalPlayer` | On `Players` service: the player with `is_local == true` |

**Methods** (camelCase and snake_case both work):

```lua
instance:FindFirstChild(name)
instance:FindFirstDescendant(name)
instance:GetChildren()          -- returns array
instance:GetDescendants()       -- returns array
instance:IsDescendantOf(ancestor)
instance:IsAncestorOf(descendant)
instance:WaitForChild(name [, timeout_seconds])
instance:GetAttribute(key)
instance:SetAttribute(key, value)  -- nil | boolean | number | string
instance:GetAttributes()           -- returns table of all attributes
```

On `game` only:

```lua
game:GetService(service_name)
```

On `Players` only:

```lua
players:GetPlayers()   -- same as :GetChildren()
```

Child lookup by name also works with dot/index syntax:

```lua
workspace.SomeModel
player.Character.Head
```

### Creating instances

```lua
local part = Instance.new("Part")           -- detached instance
local child = Instance.new("Folder", parent)  -- optional parent
```

`Instance.new` creates **sandbox-only** instances. They are not inserted into the real Roblox game.

### Attributes

Attributes are the main way to read live game data from the mirror.

**Attribute value types:** `nil`, `boolean`, `number`, `string`

#### Player attributes (synced from cache)

| Attribute | Type | Description |
|-----------|------|-------------|
| `address` | number | Player instance memory address |
| `user_id` | number | Roblox user ID |
| `name` / `username` | string | Username |
| `display_name` | string | Display name |
| `is_local` | boolean | Whether this is the local player |
| `character_address` | number | Current character model address |

#### Character attributes

| Attribute | Type | Description |
|-----------|------|-------------|
| `address` | number | Character model address |
| `model_name` | string | Character model name |

#### Head (`Character.Head`) attributes

Updated every frame from the player cache and view matrix:

| Attribute | Type | Description |
|-----------|------|-------------|
| `address` | number | Head part address |
| `position_x`, `position_y`, `position_z` | number | World position |
| `screen_x`, `screen_y` | number | Screen position (overlay coordinates) |
| `on_screen` | boolean | Whether the head projects onto screen |

#### Workspace child attributes

| Attribute | Type | Description |
|-----------|------|-------------|
| `address` | number | Instance memory address |

#### Players service attributes

| Attribute | Type | Description |
|-----------|------|-------------|
| `local_player_address` | number | Local player address (0 / nil if unknown) |

### Signals

Only two Roblox-like signals are implemented:

```lua
local players = game:GetService("Players")

players.PlayerAdded:Connect(function(player)
    print("joined:", player.Name)
end)

players.LocalPlayer.CharacterAdded:Connect(function(character)
    print("character:", character.Name)
end)
```

- `PlayerAdded` fires when a new player appears in the synced list
- `CharacterAdded` fires on a **specific player** when their character address changes (respawn)
- `:Connect(function)` returns `true` on success
- Callbacks receive the relevant instance as their first argument

There is no `:Disconnect()`, `RBXScriptConnection`, or other Roblox event APIs.

---

## Drawing API

Overlay drawing uses ImGui foreground draw list. Objects render on top of the game view.

```lua
local obj = Drawing.new("Square")   -- "Square" | "Text" | "Line"
obj:Remove()                        -- or obj.Remove (method reference)
```

Unsupported types log a warning and return `nil`.

### Common properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `Visible` | boolean | `true` | Show/hide |
| `Color` | `{ r, g, b, a }` | white | RGBA, each 0–1 |
| `Transparency` | number | `1` | Multiplier on alpha (0–1) |
| `ZIndex` | number | `1` | Draw order (lower = behind) |
| `Thickness` | number | `1` | Line/border width (min 1) |

### Square

| Property | Type | Default |
|----------|------|---------|
| `Position` | `{ x, y }` | `{ 0, 0 }` |
| `Size` | `{ x, y }` | `{ 50, 50 }` |
| `Filled` | boolean | `false` |

### Text

| Property | Type | Default |
|----------|------|---------|
| `Position` | `{ x, y }` | `{ 0, 0 }` |
| `Text` | string | `"text"` |
| `Center` | boolean | `false` | Center text on position |

### Line

| Property | Type | Default |
|----------|------|---------|
| `From` | `{ x, y }` | `{ 0, 0 }` |
| `To` | `{ x, y }` | `{ 0, 0 }` |

Vectors use **1-based** numeric tables: `{ x, y }` or `{ [1] = x, [2] = y }`.

---

## UI API (`ui` / `vanille_ui`)

`vanille_ui` is an alias for `ui`. Widgets appear as **tabs in the Vanille menu** (same tab system as built-in features).

Tabs can be referenced by **numeric ID** or **name string**:

```lua
local tab = ui.create_tab("Combat")
ui.checkbox("Combat", "enabled", "Enabled", true)  -- name works too
```

### Tab management

```lua
local tab_id = ui.create_tab("My Tab")     -- returns id; reuses existing tab with same name
ui.set_tab_name(tab_id, "Renamed")
ui.clear_tab(tab_id)                       -- removes all widgets in tab
ui.remove_tab(tab_id)                      -- removes tab entirely
```

### Widgets

All widget functions return the **current value** after creation (except `button`).

```lua
-- ui.checkbox(tab, key, label, default) -> boolean
ui.checkbox(tab, "enabled", "Enabled", true)

-- ui.slider(tab, key, label, min, max, default) -> number
ui.slider(tab, "fov", "FOV", 25, 500, 120)

-- ui.dropdown(tab, key, label, options, default_index) -> integer (0-based)
ui.dropdown(tab, "mode", "Mode", { "A", "B", "C" }, 0)

-- ui.multi_dropdown(tab, key, label, options, default_bools) -> boolean[]
ui.multi_dropdown(tab, "flags", "Flags", { "X", "Y" }, { true, false })

-- ui.keybind(tab, key, label, virtual_key, mode) -> { key, mode, enabled }
-- mode: 0 = Toggle, 1 = Hold, 2 = Always
ui.keybind(tab, "aim_key", "Aim Key", 0x02, 0)   -- 0x02 = right mouse button

-- ui.colorpicker(tab, key, label, { r, g, b, a }) -> { r, g, b, a }
ui.colorpicker(tab, "color", "Color", { 1, 0, 0, 1 })

-- ui.input_text(tab, key, label, default) -> string (max 4096 chars)
ui.input_text(tab, "name", "Name", "")

-- ui.button(tab, key, label, callback) -> boolean (success)
ui.button(tab, "run", "Run", function()
    print("clicked")
end)
```

### Reading / writing values

```lua
local enabled = ui.get(tab, "enabled")   -- boolean | number | integer | string | nil
ui.set(tab, "enabled", true)             -- boolean | number | string only
```

`ui.get` does **not** return colors, keybinds, or multi-dropdown tables. Read those from the value returned when you create the widget, or recreate the widget.

Widget keys must be unique per tab. Reusing a key updates the existing widget.

---

## Task scheduling

```lua
task.delay(seconds, function()
    print("delayed")
end)

local elapsed = task.wait(0.5)   -- blocks the Lua thread for 0.5s (uses sleep)
```

- `task.delay` is **non-blocking**; callbacks run on the main frame loop when time elapses
- `task.wait` **blocks** the executing script thread (avoid long waits in callbacks)
- There is no `task.spawn`, `RunService.Heartbeat`, or coroutine scheduler beyond this

---

## Input

```lua
local pos = input.mouse_position()   -- { x, y } overlay mouse position
local down = input.is_key_down(0x46) -- virtual-key code (0x46 = F)
```

`is_key_down` uses `GetAsyncKeyState` (0–255 / `0x00`–`0xFF`). This reads **global Windows input**, not Roblox's input focus.

Common virtual-key codes:

| Key | Code |
|-----|------|
| LMB | `0x01` |
| RMB | `0x02` |
| Insert | `0x2D` |
| F | `0x46` |

---

## Math / UI helper types

Lightweight constructors (plain tables, not full Roblox types):

```lua
Vector3.new(x, y, z)           -- { x, y, z }
Color3.new(r, g, b)            -- { r, g, b }
UDim.new(scale, offset)        -- { scale, offset }
UDim2.new(xs, xo, ys, yo)      -- { x_scale, x_offset, y_scale, y_offset }
```

These are **not** connected to the game engine. Use them for your own script logic or future UI work.

---

## Script storage

| Action | How |
|--------|-----|
| Save | Lua Editor → **Save File** → `scripts/<name>.lua` |
| Load | Select script in the scripts panel |
| Delete / rename / duplicate | Scripts panel in Lua Editor |

Script names are sanitized to alphanumeric, `_`, and `-`.

---

## What the VM can do

- Build custom menu tabs with checkboxes, sliders, dropdowns, keybinds, color pickers, buttons, and text inputs
- Draw 2D overlays (boxes, lines, text) on the Vanille overlay
- Read synced player list, usernames, user IDs, and head world/screen positions
- React to players joining and characters respawning via signals
- Browse workspace instance names/classes from the live tree (shallow mirror)
- Schedule delayed callbacks and poll keyboard/mouse state
- Persist scripts to disk in the `scripts/` folder

## What the VM cannot do

- Execute inside Roblox or call Roblox's Luau API (`RemoteEvent`, `FireServer`, etc.)
- Read or write game memory directly from Lua (no `readmemory` / `writememory` bindings)
- Require external Lua modules or access the filesystem (beyond saved scripts)
- Create real in-game instances or change game state
- Use `RunService`, `TweenService`, `HttpService`, or other unimplemented Roblox services
- Draw 3D in-world (only 2D overlay drawing)
- Network / HTTP from Lua

---

## Example: ESP-style head marker

```lua
local players = game:GetService("Players")
local markers = {}

local function get_marker(player)
    if not markers[player] then
        local dot = Drawing.new("Square")
        dot.Size = { 4, 4 }
        dot.Filled = true
        dot.Color = { 1, 0.2, 0.2, 1 }
        markers[player] = dot
    end
    return markers[player]
end

local function on_frame()
    for _, player in ipairs(players:GetPlayers()) do
        local marker = get_marker(player)
        local char = player.Character
        local head = char and char:FindFirstChild("Head")
        if head and head:GetAttribute("on_screen") then
            local x = head:GetAttribute("screen_x")
            local y = head:GetAttribute("screen_y")
            marker.Position = { x - 2, y - 2 }
            marker.Visible = true
        else
            marker.Visible = false
        end
    end
end

-- Re-run this script each frame by keeping it in a loop, or use task.delay:
local function loop()
    on_frame()
    task.delay(0, loop)
end
loop()
```

> **Note:** Use `local function loop()` — `function loop()` is invalid in Lua.

---

## Example: PlayerAdded handler

```lua
game:GetService("Players").PlayerAdded:Connect(function(player)
    print(player.Name, player:GetAttribute("user_id"))
    player.CharacterAdded:Connect(function(character)
        print("spawned", character.Name)
    end)
end)
```

---

## Errors and debugging

- Compile/runtime errors appear in the **Lua Console** with chunk name and traceback
- `warn()` for non-fatal messages
- `Drawing.new("Unknown")` warns `Drawing.new: not_supported`
- If the VM fails to load, the console shows `lua_runtime_load_failed` or `lua_state_create_failed` (check that `lua53-64.dll` is next to `vanille.exe`)

---

## File map (implementation)

| File | Role |
|------|------|
| `source/lua/lua_vm.cpp` | VM bootstrap, globals, instance bindings, sync loop |
| `source/lua/lua_drawing.cpp` | Drawing object storage and render |
| `source/lua/lua_ui.cpp` | Menu widget state and ImGui render |
| `source/lua/instance.cpp` | Sandbox instance tree |
| `source/lua/datamodel.cpp` | `game` services |
| `source/lua/script_storage.cpp` | `scripts/` folder I/O |
