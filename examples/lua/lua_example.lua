local function print_instances(label, instances)
    print(label, #instances)
    for index, instance in ipairs(instances) do
        print(index, instance.name, instance.class_name)
    end
end

local workspace = game.Workspace
if workspace then
    print("workspace", workspace.name, workspace.class_name)
    print_instances("workspace instances", workspace:get_children())
end

local players = game.Players
if players then
    print("players", players.name, players.class_name)
    print_instances("game.Players", players:get_players())
end
