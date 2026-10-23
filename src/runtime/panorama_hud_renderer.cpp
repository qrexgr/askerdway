/**
 * @file
 * @brief Архивная реализация Panorama HUD.
 *
 * Файл не включён в ASKERDWAY_RUNTIME_SOURCES текущего CMake target. Содержит альтернативный
 * путь через Panorama callback и очередь JavaScript. Описывает исторический механизм, а не
 * действующий native-рендерер.
 */

// Dota-native Panorama HUD for enemy TP status and minimap intelligence.

#include "panorama_hud_renderer.h"

namespace panorama_hud_detail {

constexpr std::size_t maximum_script_size = 64 * 1024;
constexpr std::ptrdiff_t ui_panel_name_offset = 0x10;

struct panel_registry_layout {
    std::ptrdiff_t panels;
    std::ptrdiff_t count;
};

// Dota and CS2 currently use adjacent revisions of the same Panorama engine.
// Probe the Dota layout first and retain the sibling layout as a safe fallback.
constexpr std::array panel_registry_layouts{
    panel_registry_layout{0x218, 0x220},
    panel_registry_layout{0x228, 0x230},
};

struct panel_data {
    std::uint8_t reserved[8]{};
    std::uint32_t flags = 0;
    std::uint32_t visible = 0;
    std::uintptr_t panel = 0;
    std::uint32_t panel_id = 0;
    std::uint8_t trailing[4]{};
};
static_assert(sizeof(panel_data) == 0x20);

SRWLOCK script_lock = SRWLOCK_INIT;
std::array<char, maximum_script_size> pending_script{};
std::atomic<bool> script_pending{false};
std::uintptr_t panorama_interface = 0;
std::uintptr_t ui_engine = 0;
std::uintptr_t context_panel = 0;
bool resolution_logged = false;
std::atomic<bool> frame_callback_registered{false};
std::atomic<bool> frame_callback_active{false};
std::atomic<int> frame_callback_depth{0};
std::atomic<bool> context_refresh_requested{false};

void panorama_frame_callback() {
    frame_callback_depth.fetch_add(1, std::memory_order_acq_rel);
    if (frame_callback_active.load(std::memory_order_acquire))
        execute_pending_panorama_hud_script();
    frame_callback_depth.fetch_sub(1, std::memory_order_acq_rel);
}

constexpr char setup_script[] = R"PANORAMA(
(function () {
    var requestedContext = $.GetContextPanel();
    // Stop controllers left behind by an older injected build. They keep
    // scheduled Panorama ticks alive even after another DLL is loaded and can
    // otherwise recreate the shared minimap layer with stale coordinates.
    if (typeof AskerdwayHudV4 !== "undefined" && AskerdwayHudV4) {
        try { AskerdwayHudV4.shutdown(); } catch (e) {}
        AskerdwayHudV4 = undefined;
    }
    if (typeof AskerdwayHudV6 !== "undefined" && AskerdwayHudV6) {
        try { AskerdwayHudV6.shutdown(); } catch (e) {}
        AskerdwayHudV6 = undefined;
    }
    if (typeof AskerdwayHudV9 !== "undefined" && AskerdwayHudV9) {
        try { AskerdwayHudV9.shutdown(); } catch (e) {}
        AskerdwayHudV9 = undefined;
    }
    if (typeof AskerdwayHudV10 !== "undefined" && AskerdwayHudV10) {
        try { AskerdwayHudV10.shutdown(); } catch (e) {}
        AskerdwayHudV10 = undefined;
    }
    if (typeof AskerdwayHudV11 !== "undefined" && AskerdwayHudV11) {
        try {
            if (AskerdwayHudV11.isAlive(requestedContext)) return;
            AskerdwayHudV11.shutdown();
        } catch (e) {}
        AskerdwayHudV11 = undefined;
    }

    AskerdwayHudV11 = (function (requestedContext) {
        var alive = true;
        var context = requestedContext;
        var root = context;
        while (root && root.GetParent && root.GetParent()) root = root.GetParent();
        if (!root) root = context;

        var oldHud = root.FindChildTraverse("AskerdwayPanoramaHud");
        if (oldHud) oldHud.DeleteAsync(0);
        var hud = $.CreatePanel("Panel", root, "AskerdwayPanoramaHud");
        hud.hittest = false;
        hud.hittestchildren = false;
        hud.style.width = "100%";
        hud.style.height = "100%";
        hud.style.horizontalAlign = "left";
        hud.style.verticalAlign = "top";

        var settings = {
            lastPosition: true,
            arrows: true,
            campStatus: false,
            iconScale: 50,
            lifetime: 10,
            mapMinimum: [-8288, -8288],
            mapMaximum: [8288, 8288]
        };
        var nativeTeleports = [];
        var nativeTraces = [];
        var nativeCamps = [];
        var tpRows = {};
        var lastError = "";

        function isValid(panel) {
            if (!panel) return false;
            try { return !panel.IsValid || panel.IsValid(); } catch (e) { return false; }
        }

        function gameTime() {
            try { return Game.GetGameTime(); } catch (e) { return 0; }
        }

        function heroImagePath(heroName) {
            if (!heroName) return "";
            return "file://{images}/heroes/icons/" + heroName + ".png";
        }

        function findMinimap() {
            var ids = ["minimap", "Minimap", "HUDMinimap", "HudMinimap", "DotaMinimap"];
            for (var i = 0; i < ids.length; ++i) {
                var panel = root.FindChildTraverse(ids[i]);
                if (isValid(panel)) return panel;
            }
            try {
                var byClass = root.FindChildrenWithClassTraverse("Minimap");
                if (byClass && byClass.length) return byClass[0];
            } catch (e) {}
            return null;
        }

)PANORAMA"
R"PANORAMA(
        function findMinimapContent(minimap) {
            if (!isValid(minimap)) return null;
            var preferredIds = [
                "MinimapMap", "MinimapMapContainer", "MinimapContent",
                "MinimapImage", "MinimapBackground", "MinimapBG", "MapTexture"
            ];
            for (var preferred = 0; preferred < preferredIds.length; ++preferred) {
                var named = minimap.FindChildTraverse(preferredIds[preferred]);
                if (isValid(named) && named.id !== "AskerdwayMinimapLayer" &&
                    (named.actuallayoutwidth || 0) >= 80 &&
                    (named.actuallayoutheight || 0) >= 80)
                    return named;
            }

            // The `minimap` id is an outer HUD container in the stock layout.
            // It can also contain the side buttons and decorative frame. Pick
            // the largest square map descendant instead of projecting over the
            // complete outer rectangle.
            var outerWidth = minimap.actuallayoutwidth || 0;
            var outerHeight = minimap.actuallayoutheight || 0;
            var reference = Math.max(80, Math.min(outerWidth, outerHeight));
            var best = minimap;
            var bestScore = -100000;
            function visit(panel, depth) {
                if (!isValid(panel) || depth > 7 ||
                    panel.id === "AskerdwayMinimapLayer") return;
                var width = panel.actuallayoutwidth || 0;
                var height = panel.actuallayoutheight || 0;
                if (width >= reference * 0.68 && height >= reference * 0.68) {
                    var squareError = Math.abs(width - height) / Math.max(width, height);
                    var areaRatio = Math.min(1, width * height / (reference * reference));
                    var id = String(panel.id || "").toLowerCase();
                    var score = areaRatio * 100 - squareError * 240 + depth * 4;
                    if (id.indexOf("map") >= 0) score += 35;
                    if (id.indexOf("button") >= 0 || id.indexOf("control") >= 0)
                        score -= 100;
                    if (score > bestScore) {
                        best = panel;
                        bestScore = score;
                    }
                }
                var childCount = 0;
                try { childCount = panel.GetChildCount(); } catch (e) {}
                for (var childIndex = 0; childIndex < childCount; ++childIndex) {
                    try { visit(panel.GetChild(childIndex), depth + 1); } catch (e) {}
                }
            }
            visit(minimap, 0);
            return best;
        }

        function ensureMinimapLayer() {
            var minimap = findMinimap();
            if (!isValid(minimap)) return null;
            var content = findMinimapContent(minimap);
            if (!isValid(content)) content = minimap;
            var layer = root.FindChildTraverse("AskerdwayMinimapLayer");
            if (isValid(layer) && layer.GetParent() !== content) {
                layer.DeleteAsync(0);
                layer = null;
            }
            if (!isValid(layer)) {
                layer = $.CreatePanel("Panel", content, "AskerdwayMinimapLayer");
                layer.hittest = false;
                layer.hittestchildren = false;
                layer.style.width = "100%";
                layer.style.height = "100%";
                layer.style.horizontalAlign = "left";
                layer.style.verticalAlign = "top";
                layer.style.zIndex = "10000";
            }
            return layer;
        }

        function ensureWorldLayer() {
            var layer = hud.FindChildTraverse("AskerdwayWorldTraceLayer");
            if (!isValid(layer)) {
                layer = $.CreatePanel("Panel", hud, "AskerdwayWorldTraceLayer");
                layer.hittest = false;
                layer.hittestchildren = false;
                layer.style.width = "100%";
                layer.style.height = "100%";
                layer.style.horizontalAlign = "left";
                layer.style.verticalAlign = "top";
                layer.style.overflow = "noclip";
                layer.style.zIndex = "9000";
            }
            return layer;
        }

        function findTownPortal(hero) {
            if (!hero || hero < 0) return -1;
            var count = 0;
            try { count = Entities.GetAbilityCount(hero); } catch (e) { count = 0; }
            count = Math.max(0, Math.min(64, count || 0));
            for (var slot = 0; slot < count; ++slot) {
                var ability = -1;
                try { ability = Entities.GetAbility(hero, slot); } catch (e) { ability = -1; }
                if (ability === undefined || ability === null || ability < 0) continue;
                var name = "";
                try { name = Abilities.GetAbilityName(ability); } catch (e) {}
                if (name === "item_tpscroll") return ability;
            }
            // Some HUD contexts expose the dedicated TP slot only through the
            // item accessor. The requested ability walk above remains primary.
            if (typeof Entities.GetItemInSlot === "function") {
                for (var itemSlot = 0; itemSlot < 24; ++itemSlot) {
                    var item = -1;
                    try { item = Entities.GetItemInSlot(hero, itemSlot); } catch (e) { item = -1; }
                    if (item === undefined || item === null || item < 0) continue;
                    var itemName = "";
                    try { itemName = Abilities.GetAbilityName(item); } catch (e) {}
                    if (itemName === "item_tpscroll") return item;
                }
            }
            return -1;
        }

        function readCharges(item) {
            if (item < 0) return 0;
            try {
                if (typeof Items !== "undefined" &&
                    typeof Items.GetCurrentCharges === "function")
                    return Math.max(0, Items.GetCurrentCharges(item) || 0);
            } catch (e) {}
            try {
                if (typeof Abilities.GetCurrentCharges === "function")
                    return Math.max(0, Abilities.GetCurrentCharges(item) || 0);
            } catch (e) {}
            return 0;
        }

        function enemyPlayers() {
            var result = [];
            var knownHeroes = {};
            var nextTeamSlot = { 2: 0, 3: 0 };
            var localPlayer = -1;
            var localTeam = 0;
            try {
                localPlayer = Players.GetLocalPlayer();
                localTeam = Players.GetTeam(localPlayer);
            } catch (e) { return result; }
            var maximum = 24;
            try { maximum = Math.min(64, Players.GetMaxPlayers()); } catch (e) {}
            for (var playerId = 0; playerId < maximum; ++playerId) {
                try {
                    if (!Players.IsValidPlayerID(playerId)) continue;
                    var team = Players.GetTeam(playerId);
                    if (!team || team === localTeam || (team !== 2 && team !== 3)) continue;
                    var hero = Players.GetPlayerHeroEntityIndex(playerId);
                    if (hero === undefined || hero === null || hero < 0) continue;
                    knownHeroes[String(hero)] = true;
                    result.push({
                        key: "p" + playerId,
                        playerId: playerId,
                        // Player IDs are not scoreboard slots (especially in
                        // demo mode). Use the hero's order inside its team.
                        displaySlot: nextTeamSlot[team]++,
                        team: team,
                        hero: hero,
                        heroName: Players.GetPlayerSelectedHero(playerId) || ""
                    });
                } catch (e) {}
            }

            // Demo mode can create enemy heroes without allocating a normal
            // player slot. Keep the regular Players path above for real
            // matches, then fill only missing heroes from the entity list.
            try {
                if (typeof Entities.GetAllHeroEntities === "function") {
                    var heroes = Entities.GetAllHeroEntities() || [];
                    for (var heroIndex = 0; heroIndex < heroes.length; ++heroIndex) {
                        var entityHero = heroes[heroIndex];
                        if (entityHero === undefined || entityHero === null ||
                            entityHero < 0 || knownHeroes[String(entityHero)])
                            continue;
                        var entityTeam = Entities.GetTeamNumber(entityHero);
                        if (!entityTeam || entityTeam === localTeam ||
                            (entityTeam !== 2 && entityTeam !== 3))
                            continue;
                        if (typeof Entities.IsRealHero === "function" &&
                            !Entities.IsRealHero(entityHero))
                            continue;
                        knownHeroes[String(entityHero)] = true;
                        result.push({
                            key: "e" + entityHero,
                            playerId: -1,
                            displaySlot: nextTeamSlot[entityTeam]++,
                            team: entityTeam,
                            hero: entityHero,
                            heroName: Entities.GetUnitName(entityHero) || ""
                        });
                    }
                }
            } catch (e) {}
            return result;
        }

        function findTopBarPlayerSlot(player) {
            var containerId = player.team === 2
                ? "TopBarRadiantPlayersContainer"
                : "TopBarDirePlayersContainer";
            var container = root.FindChildTraverse(containerId);
            if (!isValid(container)) return null;
            var count = 0;
            try { count = container.GetChildCount(); } catch (e) { return null; }
            if (count <= 0) return null;
            var slot = Math.max(0, Math.min(count - 1, player.displaySlot));
            try {
                var child = container.GetChild(slot);
                return isValid(child) ? child : null;
            } catch (e) { return null; }
        }

)PANORAMA"
R"PANORAMA(
        function createTpRow(player) {
            var id = "AskerdwayTpStatus" + player.key;
            var host = findTopBarPlayerSlot(player);
            if (!isValid(host)) host = hud;
            var row = root.FindChildTraverse(id);
            if (isValid(row) && row.GetParent() !== host) {
                row.DeleteAsync(0);
                row = null;
            }
            if (!isValid(row)) {
                row = $.CreatePanel("Panel", host, id);
                row.hittest = false;
                row.hittestchildren = false;
                row.style.width = "31px";
                row.style.height = "44px";
                row.style.backgroundColor = "rgba(7, 13, 18, 0.88)";
                row.style.border = "1px solid rgba(118, 218, 255, 0.82)";
                row.style.borderRadius = "4px";
                row.style.boxShadow = "0px 0px 5px 1px rgba(0,0,0,0.75)";
                row.style.verticalAlign = "top";
                row.style.overflow = "noclip";

                var icon = $.CreatePanel("Image", row, "TpIcon");
                icon.SetImage("file://{images}/items/tpscroll.png");
                icon.scaling = "stretch-aspect-preserve";
                icon.style.width = "22px";
                icon.style.height = "22px";
                icon.style.marginTop = "2px";
                icon.style.horizontalAlign = "center";
                icon.style.verticalAlign = "top";

                var label = $.CreatePanel("Label", row, "TpValue");
                label.style.marginTop = "24px";
                label.style.width = "100%";
                label.style.height = "18px";
                label.style.verticalAlign = "top";
                label.style.textAlign = "center";
                label.style.color = "white";
                label.style.fontSize = "11px";
                label.style.fontWeight = "bold";
                label.style.textShadow = "1px 1px 2px 2 #000000";
            }
            if (host !== hud) {
                var slotWidth = host.actuallayoutwidth || 50;
                var portraitWidth = Math.min(50, Math.max(31, slotWidth));
                row.style.horizontalAlign = "left";
                row.style.marginLeft = Math.round((portraitWidth - 31) / 2) + "px";
                row.style.marginTop = "43px";
            } else {
                row.style.horizontalAlign = "left";
                row.style.marginLeft = "0px";
                row.style.marginTop = "0px";
            }
            return row;
        }

        function updateAltStatus() {
            var altDown = false;
            try { altDown = GameUI.IsAltDown(); } catch (e) {}
            if (!altDown) {
                for (var existing in tpRows) {
                    if (isValid(tpRows[existing])) tpRows[existing].style.visibility = "collapse";
                }
                return;
            }

            var players = enemyPlayers();
            var seen = {};
            for (var index = 0; index < players.length; ++index) {
                var player = players[index];
                seen[player.key] = true;
                var row = createTpRow(player);
                tpRows[player.key] = row;
                row.style.visibility = "visible";

                if (row.GetParent() === hud) {
                    var rootWidth = Math.max(900, root.actuallayoutwidth || 1920);
                    var teamSlot = player.displaySlot;
                    var spacing = Math.max(48, Math.min(66, rootWidth / 29));
                    var center = rootWidth / 2;
                    var x = player.team === 2
                        ? center - 78 - (4 - teamSlot) * spacing
                        : center + 78 + teamSlot * spacing;
                    row.style.position = Math.round(x - 15) + "px 43px 0px";
                }

                var tp = findTownPortal(player.hero);
                var label = row.FindChildTraverse("TpValue");
                var icon = row.FindChildTraverse("TpIcon");
                if (tp < 0) {
                    label.text = "—";
                    label.style.color = "#b8c1c8";
                    icon.style.opacity = "0.35";
                    continue;
                }

                var cooldown = 0;
                try { cooldown = Abilities.GetCooldownTimeRemaining(tp) || 0; } catch (e) {}
                var charges = readCharges(tp);
                icon.style.opacity = "1.0";
                if (cooldown > 0.05) {
                    label.text = Math.ceil(cooldown) + "s";
                    label.style.color = "#ffca6b";
                } else {
                    label.text = charges > 0 ? String(charges) : "✓";
                    label.style.color = "#72ef9d";
                }
            }
            for (var key in tpRows) {
                if (!seen[key] && isValid(tpRows[key]))
                    tpRows[key].style.visibility = "collapse";
            }
        }

)PANORAMA"
R"PANORAMA(
        function worldToMinimap(position, width, height) {
            var minimum = [settings.mapMinimum[0], settings.mapMinimum[1]];
            var maximum = [settings.mapMaximum[0], settings.mapMaximum[1]];
            // dota_minimap_boundary includes the image-generation bleed. The
            // stock DOTAMinimap projects the playable 16384-unit square. Strip
            // that symmetric bleed so our center lands on the native icon.
            var rawX = maximum[0] - minimum[0];
            var rawY = maximum[1] - minimum[1];
            if (rawX > 16384 && rawX < 17000) {
                var insetX = (rawX - 16384) / 2;
                minimum[0] += insetX;
                maximum[0] -= insetX;
            }
            if (rawY > 16384 && rawY < 17000) {
                var insetY = (rawY - 16384) / 2;
                minimum[1] += insetY;
                maximum[1] -= insetY;
            }
            var spanX = Math.max(1, maximum[0] - minimum[0]);
            var spanY = Math.max(1, maximum[1] - minimum[1]);
            var nx = Math.max(0, Math.min(1,
                (position[0] - minimum[0]) / spanX));
            var ny = 1 - Math.max(0, Math.min(1,
                (position[1] - minimum[1]) / spanY));
            return [nx * width, ny * height];
        }

        function drawHeroIcon(layer, position, heroName, size, blue, borderColor, id) {
            var holder = $.CreatePanel("Panel", layer, id);
            holder.hittest = false;
            holder.style.position = Math.round(position[0] - size / 2) + "px " +
                Math.round(position[1] - size / 2) + "px 0px";
            holder.style.width = Math.round(size) + "px";
            holder.style.height = Math.round(size) + "px";
            holder.style.border = "2px solid " + borderColor;
            holder.style.borderRadius = "50%";
            holder.style.backgroundColor = "rgba(5,8,12,0.75)";
            holder.style.boxShadow = "0px 0px 5px 1px rgba(0,0,0,0.9)";

            var image = $.CreatePanel("Image", holder, "Hero");
            image.SetImage(heroImagePath(heroName));
            image.scaling = "stretch-to-fit-preserve-aspect";
            image.style.width = "100%";
            image.style.height = "100%";
            image.style.borderRadius = "50%";
            if (blue) {
                image.style.washColor = "#317dff";
                image.style.saturation = "0.55";
                image.style.brightness = "0.72";
            }
            return holder;
        }

        function worldToScreen(position) {
            if (!position || typeof Game.WorldToScreenX !== "function" ||
                typeof Game.WorldToScreenY !== "function") return null;
            var x = -1;
            var y = -1;
            try {
                x = Game.WorldToScreenX(position[0], position[1],
                    (position[2] || 0) + 92);
                y = Game.WorldToScreenY(position[0], position[1],
                    (position[2] || 0) + 92);
            } catch (e) { return null; }
            var width = hud.actuallayoutwidth || root.actuallayoutwidth || 0;
            var height = hud.actuallayoutheight || root.actuallayoutheight || 0;
            if (!isFinite(x) || !isFinite(y) || x < 0 || y < 0 ||
                x > width || y > height) return null;
            return [x, y];
        }

        function resolveTriggerLabel(trace) {
            var raw = String(trace.trigger || "Fog of War");
            if (raw === "Fog of War" || trace.ownerHandle === undefined)
                return raw;
            var hero = trace.ownerHandle & 0x7fff;
            var probe = raw.toLowerCase().replace(/[^a-z0-9]+/g, "_");
            var bestName = "";
            var bestScore = 0;
            try {
                var count = Math.min(64, Entities.GetAbilityCount(hero) || 0);
                for (var slot = 0; slot < count; ++slot) {
                    var ability = Entities.GetAbility(hero, slot);
                    if (ability === undefined || ability === null || ability < 0)
                        continue;
                    var name = Abilities.GetAbilityName(ability) || "";
                    if (!name) continue;
                    var tokens = name.toLowerCase().split("_");
                    var score = 0;
                    for (var token = 0; token < tokens.length; ++token) {
                        if (tokens[token].length >= 4 &&
                            probe.indexOf(tokens[token]) >= 0) ++score;
                    }
                    if (score > bestScore) {
                        bestScore = score;
                        bestName = name;
                    }
                }
            } catch (e) {}
            if (bestName && bestScore >= 2) {
                var key = "#DOTA_Tooltip_Ability_" + bestName;
                var localized = $.Localize(key);
                if (localized && localized !== key) return localized;
                return bestName.replace(/_/g, " ");
            }
            return raw;
        }

)PANORAMA"
R"PANORAMA(
        function drawWorldTrace(layer, position, trace, iconSize,
            remainingMs, id) {
            var width = 240;
            var height = iconSize + 48;
            var holder = $.CreatePanel("Panel", layer, id);
            holder.hittest = false;
            holder.hittestchildren = false;
            holder.style.position = Math.round(position[0] - width / 2) + "px " +
                Math.round(position[1] - height / 2) + "px 0px";
            holder.style.width = width + "px";
            holder.style.height = Math.round(height) + "px";
            holder.style.overflow = "noclip";

            var timer = $.CreatePanel("Label", holder, "Timer");
            timer.text = (Math.max(0, remainingMs) / 1000).toFixed(1) + "s";
            timer.style.width = "100%";
            timer.style.height = "22px";
            timer.style.textAlign = "center";
            timer.style.color = "#ffe286";
            timer.style.fontSize = "16px";
            timer.style.fontWeight = "bold";
            timer.style.textShadow = "1px 1px 3px 3 #000000";

            var icon = $.CreatePanel("Panel", holder, "IconHolder");
            icon.style.width = Math.round(iconSize) + "px";
            icon.style.height = Math.round(iconSize) + "px";
            icon.style.marginTop = "21px";
            icon.style.horizontalAlign = "center";
            icon.style.border = "3px solid #ffd66b";
            icon.style.borderRadius = "50%";
            icon.style.backgroundColor = "rgba(5,8,12,0.82)";
            icon.style.boxShadow = "0px 0px 7px 2px rgba(255,214,107,0.45)";

            var image = $.CreatePanel("Image", icon, "Hero");
            image.SetImage(heroImagePath(trace.heroName));
            image.scaling = "stretch-to-fit-preserve-aspect";
            image.style.width = "100%";
            image.style.height = "100%";
            image.style.borderRadius = "50%";

            var trigger = $.CreatePanel("Label", holder, "Trigger");
            trigger.text = resolveTriggerLabel(trace);
            trigger.style.marginTop = Math.round(iconSize + 23) + "px";
            trigger.style.width = "100%";
            trigger.style.height = "24px";
            trigger.style.textAlign = "center";
            trigger.style.color = "#e9f1f5";
            trigger.style.fontSize = "13px";
            trigger.style.fontWeight = "semi-bold";
            trigger.style.textShadow = "1px 1px 3px 3 #000000";
        }

        function renderWorldMarkers() {
            var layer = ensureWorldLayer();
            if (!isValid(layer)) return;
            layer.RemoveAndDeleteChildren();
            if (!settings.lastPosition) return;
            var now = gameTime();
            var scale = Math.max(0, Math.min(100, settings.iconScale)) / 100;
            var iconSize = 38 + scale * 18;
            for (var i = 0; i < nativeTraces.length; ++i) {
                var trace = nativeTraces[i];
                if (!trace) continue;
                var remaining = trace.remainingMs -
                    Math.max(0, (now - trace.receivedAt) * 1000);
                if (remaining <= 0) continue;
                var screen = worldToScreen(trace.position);
                if (!screen) continue;
                drawWorldTrace(layer, screen, trace, iconSize, remaining,
                    "WorldLastPosition" + i);
            }
        }

        function drawRing(layer, position, diameter, color, id) {
            var ring = $.CreatePanel("Panel", layer, id);
            ring.hittest = false;
            ring.style.position = Math.round(position[0] - diameter / 2) + "px " +
                Math.round(position[1] - diameter / 2) + "px 0px";
            ring.style.width = Math.round(diameter) + "px";
            ring.style.height = Math.round(diameter) + "px";
            ring.style.border = "3px solid " + color;
            ring.style.borderRadius = "50%";
            ring.style.boxShadow = "0px 0px 6px 1px " + color;
        }

        function drawCampStatus(layer, position, blocked, type, id) {
            var size = type >= 4 ? 15 : (type >= 3 ? 13 : 11);
            var marker = $.CreatePanel("Panel", layer, id);
            marker.hittest = false;
            marker.style.position = Math.round(position[0] - size / 2) + "px " +
                Math.round(position[1] - size / 2) + "px 0px";
            marker.style.width = size + "px";
            marker.style.height = size + "px";
            marker.style.border = "3px solid " +
                (blocked ? "#ff3434" : "#f5cd2d");
            marker.style.backgroundColor = blocked
                ? "rgba(120,0,0,0.50)" : "rgba(80,65,0,0.28)";
            marker.style.boxShadow = "0px 0px 5px 1px " +
                (blocked ? "#ff2020" : "#e7bd22");
        }

        function drawLine(layer, from, to, id, endpointRadius) {
            var dx = to[0] - from[0];
            var dy = to[1] - from[1];
            var length = Math.sqrt(dx * dx + dy * dy);
            if (length < 2) return;
            var angle = Math.atan2(dy, dx) * 180 / Math.PI;
            var line = $.CreatePanel("Panel", layer, id + "Line");
            line.hittest = false;
            line.style.position = Math.round(from[0]) + "px " + Math.round(from[1] - 2) + "px 0px";
            line.style.width = Math.round(length) + "px";
            line.style.height = "4px";
            line.style.backgroundColor = "#63e979";
            line.style.boxShadow = "0px 0px 5px 1px rgba(38,210,86,0.9)";
            line.style.transformOrigin = "0% 50%";
            line.style.transform = "rotateZ(" + angle + "deg)";

            var head = $.CreatePanel("Label", layer, id + "Head");
            head.text = "➤";
            head.hittest = false;
            var headDistance = Math.max(9, (endpointRadius || 0) + 7);
            var headX = to[0] - dx / length * headDistance;
            var headY = to[1] - dy / length * headDistance;
            head.style.position = Math.round(headX - 11) + "px " +
                Math.round(headY - 13) + "px 0px";
            head.style.width = "22px";
            head.style.height = "26px";
            head.style.fontSize = "23px";
            head.style.color = "#63e979";
            head.style.textShadow = "0px 0px 4px 2 #0a3915";
            head.style.zIndex = "50";
            head.style.transformOrigin = "50% 50%";
            head.style.transform = "rotateZ(" + angle + "deg)";
        }

        function renderMinimap() {
            var layer = ensureMinimapLayer();
            if (!isValid(layer)) return;
            layer.RemoveAndDeleteChildren();
            var minimap = layer.GetParent();
            var width = minimap.actuallayoutwidth || 0;
            var height = minimap.actuallayoutheight || 0;
            if (width < 40 || height < 40) return;
            var scaleProgress = Math.max(0, Math.min(100, settings.iconScale)) / 100;
            var iconFraction = 1 / 12 + (1 / 8 - 1 / 12) * scaleProgress;
            var iconSize = Math.min(width, height) * iconFraction;
            iconSize = Math.max(8, iconSize);
            var now = gameTime();

            if (settings.lastPosition) {
                for (var t = 0; t < nativeTraces.length; ++t) {
                    var trace = nativeTraces[t];
                    if (!trace) continue;
                    var traceRemaining = trace.remainingMs -
                        Math.max(0, (now - trace.receivedAt) * 1000);
                    if (traceRemaining <= 0) continue;
                    var tracePos = worldToMinimap(trace.position, width, height);
                    drawHeroIcon(layer, tracePos, trace.heroName, iconSize,
                        false, "#ffd66b", "NativeLastPosition" + t);
                }
            }

            if (settings.campStatus) {
                for (var c = 0; c < nativeCamps.length; ++c) {
                    var camp = nativeCamps[c];
                    if (!camp) continue;
                    drawCampStatus(layer,
                        worldToMinimap(camp.position, width, height),
                        !!camp.blocked, camp.type || 0, "NeutralCamp" + c);
                }
            }

            if (settings.arrows) {
                for (var i = 0; i < nativeTeleports.length; ++i) {
                    var tp = nativeTeleports[i];
                    if (!tp || !tp.hasOrigin || !tp.hasDestination) continue;
                    var worldDx = tp.destination[0] - tp.origin[0];
                    var worldDy = tp.destination[1] - tp.origin[1];
                    if (worldDx * worldDx + worldDy * worldDy <= 4096) continue;
                    var from = worldToMinimap(tp.origin, width, height);
                    var to = worldToMinimap(tp.destination, width, height);
                    var prefix = "Teleport" + i;
                    drawLine(layer, from, to, prefix, iconSize / 2);
                    var elapsed = tp.elapsedMs + Math.max(0, (now - tp.receivedAt) * 1000);
                    var progress = Math.max(0, Math.min(1, elapsed / Math.max(1, tp.durationMs)));
                    var ringSize = iconSize * (1.75 - progress * 0.72);
                    drawRing(layer, from, ringSize, "#3b83ff", prefix + "FromRing");
                    drawRing(layer, to, ringSize, "#42ef73", prefix + "ToRing");
                    drawHeroIcon(layer, from, tp.heroName, iconSize, true,
                        "#3b83ff", prefix + "From");
                    drawHeroIcon(layer, to, tp.heroName, iconSize, false,
                        "#42ef73", prefix + "To");
                }
            }
        }

        function tick() {
            if (!alive) return;
            try {
                updateAltStatus();
                renderMinimap();
                renderWorldMarkers();
            } catch (e) {
                var message = String(e);
                if (message !== lastError) {
                    lastError = message;
                    $.Msg("[askerdway] Panorama HUD: " + message);
                }
            }
            var animated = nativeTraces.length > 0 || nativeTeleports.length > 0;
            $.Schedule(animated ? 0.10 : 0.25, tick);
        }

        tick();
        return {
            isAlive: function (currentContext) {
                return alive && isValid(hud) &&
                    (!currentContext || currentContext === context);
            },
            nativeUpdate: function (data) {
                if (!data) return;
                settings.lastPosition = !!data.lastPosition;
                settings.arrows = !!data.arrows;
                settings.campStatus = !!data.campStatus;
                var requestedScale = data.iconScale;
                if (requestedScale === undefined || requestedScale === null)
                    requestedScale = 50;
                settings.iconScale = Math.max(0, Math.min(100, requestedScale));
                settings.lifetime = Math.max(3, Math.min(30, data.lifetime || 10));
                if (data.mapMinimum && data.mapMaximum &&
                    data.mapMaximum[0] > data.mapMinimum[0] &&
                    data.mapMaximum[1] > data.mapMinimum[1]) {
                    settings.mapMinimum = data.mapMinimum;
                    settings.mapMaximum = data.mapMaximum;
                }
                nativeTeleports = data.teleports || [];
                nativeTraces = data.traces || [];
                nativeCamps = data.camps || [];
                var received = gameTime();
                for (var i = 0; i < nativeTeleports.length; ++i)
                    nativeTeleports[i].receivedAt = received;
                for (var t = 0; t < nativeTraces.length; ++t)
                    nativeTraces[t].receivedAt = received;
            },
            shutdown: function () {
                alive = false;
                for (var key in tpRows) {
                    if (isValid(tpRows[key])) tpRows[key].DeleteAsync(0);
                }
                tpRows = {};
                var layer = root.FindChildTraverse("AskerdwayMinimapLayer");
                if (layer) layer.DeleteAsync(0);
                if (hud) hud.DeleteAsync(0);
            }
        };
    })(requestedContext);
})();
)PANORAMA";

constexpr char alt_test_script[] = R"PANORAMA(
(function () {
    if (typeof AskerdwayAltTest !== "undefined" && AskerdwayAltTest) {
        try {
            if (AskerdwayAltTest.isAlive()) return;
            AskerdwayAltTest.shutdown();
        } catch (e) {}
        AskerdwayAltTest = undefined;
    }
    var context = $.GetContextPanel();
    var root = context;
    var depth = 0;
    while (root && root.GetParent && root.GetParent() && depth++ < 64)
        root = root.GetParent();
    if (!root) return;
    var old = root.FindChildTraverse("AskerdwayAltApiTest");
    if (old) old.DeleteAsync(0);
    var host = $.CreatePanel("Panel", root, "AskerdwayAltApiTest");
    host.hittest = false;
    host.style.width = "390px";
    host.style.height = "42px";
    host.style.horizontalAlign = "center";
    host.style.verticalAlign = "top";
    host.style.marginTop = "54px";
    host.style.backgroundColor = "rgba(6, 12, 18, 0.92)";
    host.style.border = "1px solid #63d7ff";
    host.style.visibility = "visible";
    var value = $.CreatePanel("Label", host, "Value");
    value.style.width = "100%";
    value.style.height = "100%";
    value.style.textAlign = "center";
    value.style.verticalAlign = "center";
    value.style.color = "white";
    value.style.fontSize = "18px";

    function findScroll(hero) {
        var count = Entities.GetAbilityCount(hero) || 0;
        for (var slot = 0; slot < count && slot < 64; ++slot) {
            var ability = Entities.GetAbility(hero, slot);
            if (ability >= 0 && Abilities.GetAbilityName(ability) === "item_tpscroll")
                return ability;
        }
        if (typeof Entities.GetItemInSlot === "function") {
            for (var itemSlot = 0; itemSlot < 24; ++itemSlot) {
                var item = Entities.GetItemInSlot(hero, itemSlot);
                if (item >= 0 && Abilities.GetAbilityName(item) === "item_tpscroll")
                    return item;
            }
        }
        return -1;
    }

    function enemyHeroes() {
        var result = [];
        var known = {};
        var local = Players.GetLocalPlayer();
        var localTeam = Players.GetTeam(local);
        var maximum = Players.GetMaxPlayers();
        for (var playerId = 0; playerId < maximum; ++playerId) {
            if (!Players.IsValidPlayerID(playerId) ||
                Players.GetTeam(playerId) === localTeam)
                continue;
            var hero = Players.GetPlayerHeroEntityIndex(playerId);
            if (hero < 0) continue;
            known[String(hero)] = true;
            result.push(hero);
        }
        if (typeof Entities.GetAllHeroEntities === "function") {
            var heroes = Entities.GetAllHeroEntities() || [];
            for (var index = 0; index < heroes.length; ++index) {
                var entityHero = heroes[index];
                if (entityHero < 0 || known[String(entityHero)]) continue;
                var team = Entities.GetTeamNumber(entityHero);
                if (!team || team === localTeam || (team !== 2 && team !== 3))
                    continue;
                if (typeof Entities.IsRealHero === "function" &&
                    !Entities.IsRealHero(entityHero))
                    continue;
                known[String(entityHero)] = true;
                result.push(entityHero);
            }
        }
        return result;
    }

    AskerdwayAltTest = {
        isAlive: function () {
            try { return !host.IsValid || host.IsValid(); } catch (e) { return false; }
        },
        tick: function () {
            try {
                var altDown = GameUI.IsAltDown();
                var result = "TP: enemy not found";
                var heroes = enemyHeroes();
                for (var heroIndex = 0; heroIndex < heroes.length; ++heroIndex) {
                    var hero = heroes[heroIndex];
                    var scroll = findScroll(hero);
                    if (scroll < 0) {
                        result = "hero=" + hero + " TP entity: —";
                    } else {
                        var cooldown = Abilities.GetCooldownTimeRemaining(scroll) || 0;
                        var charges = 0;
                        if (typeof Items !== "undefined" &&
                            typeof Items.GetCurrentCharges === "function")
                            charges = Items.GetCurrentCharges(scroll) || 0;
                        result = cooldown > 0.05
                            ? "hero=" + hero + " item=" + scroll + " CD=" + Math.ceil(cooldown) + "s"
                            : "hero=" + hero + " item=" + scroll + " ready ×" + charges;
                    }
                    break;
                }
                value.text = "ALT=" + (altDown ? "1 " : "0 ") + result;
            } catch (error) {
                host.style.visibility = "visible";
                value.text = "Panorama API: " + String(error);
            }
        },
        shutdown: function () {
            if (host) host.DeleteAsync(0);
        }
    };
})();
)PANORAMA";

bool address_in_module(std::uintptr_t address, HMODULE module) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto size = image_size(base);
    return size != 0 && address >= base && address < base + size;
}

int panel_score(const char* name, std::uint32_t visible) {
    if (name == nullptr || *name == '\0')
        return -1;
    // A hidden panel from the dashboard can remain registered after entering
    // a match. Prefer the currently visible HUD even when the stale panel has
    // a slightly more specific name.
    int score = visible != 0 ? 100 : -100;
    if (std::strcmp(name, "DotaHud") == 0)
        return score + 1000;
    if (std::strcmp(name, "HUDElements") == 0)
        return score + 950;
    if (std::strcmp(name, "Hud") == 0)
        return score + 900;
    if (std::strcmp(name, "minimap") == 0 ||
        std::strcmp(name, "Minimap") == 0)
        return score + 850;
    if (std::strcmp(name, "TopBar") == 0)
        return score + 800;
    if (std::strstr(name, "DotaHud") != nullptr)
        return score + 700;
    if (std::strstr(name, "HUD") != nullptr ||
        std::strstr(name, "Hud") != nullptr)
        return score + 500;
    if (std::strstr(name, "minimap") != nullptr ||
        std::strstr(name, "Minimap") != nullptr)
        return score + 450;
    return -1;
}

bool resolve_context() {
    const HMODULE panorama = GetModuleHandleW(L"panorama.dll");
    if (panorama == nullptr)
        return false;

    using create_interface_fn = void* (*)(const char*, int*);
    const auto create_interface = reinterpret_cast<create_interface_fn>(
        GetProcAddress(panorama, "CreateInterface"));
    if (create_interface == nullptr)
        return false;

    panorama_interface = reinterpret_cast<std::uintptr_t>(
        create_interface("PanoramaUIEngine001", nullptr));
    std::uintptr_t interface_vtable = 0;
    std::uintptr_t get_ui_engine_address = 0;
    if (!plausible_pointer(panorama_interface) ||
        !safe_read(panorama_interface, interface_vtable) ||
        !plausible_pointer(interface_vtable) ||
        !safe_read(interface_vtable + 13 * sizeof(void*), get_ui_engine_address) ||
        !address_in_module(get_ui_engine_address, panorama))
        return false;

    using get_ui_engine_fn = void* (*)(void*);
    ui_engine = reinterpret_cast<std::uintptr_t>(
        reinterpret_cast<get_ui_engine_fn>(get_ui_engine_address)(
            reinterpret_cast<void*>(panorama_interface)));
    if (!plausible_pointer(ui_engine))
        return false;

    int best_score = -1;
    std::uintptr_t best_panel = 0;
    char best_name[128]{};
    int selected_panel_count = 0;
    std::ptrdiff_t selected_layout_offset = 0;
    for (const auto layout : panel_registry_layouts) {
        std::uintptr_t panels = 0;
        std::int32_t panel_count = 0;
        if (!safe_read(ui_engine + layout.panels, panels) ||
            !plausible_pointer(panels) ||
            !safe_read(ui_engine + layout.count, panel_count) ||
            panel_count <= 0 || panel_count > 100000)
            continue;
        for (int index = 0; index < panel_count; ++index) {
            panel_data data{};
            if (!safe_read(panels + sizeof(panel_data) *
                    static_cast<std::size_t>(index), data) ||
                !plausible_pointer(data.panel))
                continue;
            std::uintptr_t name_address = 0;
            char name[128]{};
            if (!safe_read(data.panel + ui_panel_name_offset, name_address) ||
                !read_c_string(name_address, name))
                continue;
            const int score = panel_score(name, data.visible);
            if (score <= best_score)
                continue;
            best_score = score;
            best_panel = data.panel;
            selected_panel_count = panel_count;
            selected_layout_offset = layout.panels;
            std::snprintf(best_name, sizeof(best_name), "%s", name);
        }
        if (best_score >= 900)
            break;
    }
    if (!plausible_pointer(best_panel))
        return false;

    context_panel = best_panel;
    if (!resolution_logged) {
        char message[224]{};
        std::snprintf(message, sizeof(message),
            "Panorama HUD attached to panel '%s' (%d panels, registry +0x%llX).",
            best_name, selected_panel_count,
            static_cast<unsigned long long>(selected_layout_offset));
        log_message("INFO", message);
        resolution_logged = true;
    }
    return true;
}

bool queue_script(HWND game_window, std::string_view script, bool synchronous) {
    if (!IsWindow(game_window) || script.empty() ||
        script.size() >= pending_script.size())
        return false;
    AcquireSRWLockExclusive(&script_lock);
    std::memcpy(pending_script.data(), script.data(), script.size());
    pending_script[script.size()] = '\0';
    script_pending.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&script_lock);

    if (!synchronous)
        return true;

    // The Panorama RunFrame hook consumes the script on the engine's own UI
    // phase. Waiting here is used only by orderly shutdown.
    for (int wait = 0; wait < 150 &&
         script_pending.load(std::memory_order_acquire); ++wait)
        Sleep(2);
    return !script_pending.load(std::memory_order_acquire);
}

void append_js_string(std::string& output, const char* value) {
    output.push_back('"');
    if (value != nullptr) {
        for (const unsigned char character : std::string_view(value)) {
            if (character == '"' || character == '\\')
                output.push_back('\\');
            if (character >= 0x20)
                output.push_back(static_cast<char>(character));
        }
    }
    output.push_back('"');
}

void append_number(std::string& output, float value) {
    char number[48]{};
    std::snprintf(number, sizeof(number), "%.3f",
        std::isfinite(value) ? value : 0.0f);
    output += number;
}

} // namespace panorama_hud_detail

bool install_panorama_frame_dispatch(HWND game_window) {
    if (panorama_hud_detail::frame_callback_registered.load(
            std::memory_order_acquire)) {
        panorama_hud_detail::frame_callback_active.store(true,
            std::memory_order_release);
        return true;
    }

    DWORD window_thread = 0;
    if (IsWindow(game_window))
        window_thread = GetWindowThreadProcessId(game_window, nullptr);
    if (window_thread != 0 && window_thread != GetCurrentThreadId()) {
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(game_window,
            register_panorama_frame_callback_message, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0 && result != 0;
    }

    if (!panorama_hud_detail::resolve_context())
        return false;

    const HMODULE panorama = GetModuleHandleW(L"panorama.dll");
    std::uintptr_t vtable = 0;
    std::uintptr_t register_callback_address = 0;
    constexpr std::size_t register_frame_callback_vtable_index = 25;
    if (panorama == nullptr ||
        !safe_read(panorama_hud_detail::ui_engine, vtable) ||
        !plausible_pointer(vtable))
        return false;
    if (!safe_read(vtable + register_frame_callback_vtable_index * sizeof(void*),
            register_callback_address) ||
        !panorama_hud_detail::address_in_module(register_callback_address, panorama))
        return false;

    // Panorama owns this callback for the rest of the process. Pinning the DLL
    // guarantees that the callback address never becomes dangling, even if the
    // worker is stopped with End.
    HMODULE pinned_module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&panorama_hud_detail::panorama_frame_callback),
            &pinned_module) == FALSE)
        return false;

    panorama_hud_detail::frame_callback_active.store(true,
        std::memory_order_release);
    using register_frame_callback_fn = void (*)(void*, void (*)());
    reinterpret_cast<register_frame_callback_fn>(register_callback_address)(
        reinterpret_cast<void*>(panorama_hud_detail::ui_engine),
        &panorama_hud_detail::panorama_frame_callback);
    panorama_hud_detail::frame_callback_registered.store(true,
        std::memory_order_release);
    log_message("INFO", "Panorama script dispatcher registered in the native frame callback list.");
    return true;
}

bool panorama_frame_dispatch_installed() {
    return panorama_hud_detail::frame_callback_registered.load(
        std::memory_order_acquire);
}

void uninstall_panorama_frame_dispatch() {
    panorama_hud_detail::frame_callback_active.store(false,
        std::memory_order_release);
    for (int wait = 0; wait < 200 &&
         panorama_hud_detail::frame_callback_depth.load(
             std::memory_order_acquire) != 0; ++wait)
        Sleep(1);
}

bool queue_panorama_hud_setup(HWND game_window) {
    panorama_hud_detail::context_refresh_requested.store(true,
        std::memory_order_release);
#if defined(ASKERDWAY_PANORAMA_SMOKE_TEST)
    constexpr char smoke_test[] =
        "$.Msg('[askerdway] Panorama smoke test reached the Dota UI thread');";
    return panorama_hud_detail::queue_script(game_window, smoke_test, false);
#elif defined(ASKERDWAY_PANORAMA_ALT_TEST)
    return panorama_hud_detail::queue_script(game_window,
        panorama_hud_detail::alt_test_script, false);
#else
    return panorama_hud_detail::queue_script(game_window,
        panorama_hud_detail::setup_script, false);
#endif
}

bool queue_panorama_hud_snapshot(HWND game_window) {
#if defined(ASKERDWAY_PANORAMA_SMOKE_TEST)
    (void)game_window;
    return true;
#elif defined(ASKERDWAY_PANORAMA_ALT_TEST)
    constexpr char tick[] =
        "if(typeof AskerdwayAltTest!=='undefined'&&AskerdwayAltTest)"
        "AskerdwayAltTest.tick();";
    return panorama_hud_detail::queue_script(game_window, tick, false);
#else
    render_intelligence_state snapshot{};
    {
        render_intelligence_shared_guard guard;
        snapshot = g_render_intelligence;
    }

    const ULONGLONG now = GetTickCount64();
    std::string script;
    script.reserve(8192);
    script += "if(typeof AskerdwayHudV11!=='undefined'&&AskerdwayHudV11)"
        "AskerdwayHudV11.nativeUpdate({lastPosition:";
    script += g_render_trace_icons.load(std::memory_order_relaxed)
        ? "true" : "false";
    script += ",arrows:";
    script += g_render_teleport_arrows.load(std::memory_order_relaxed)
        ? "true" : "false";
    script += ",campStatus:";
    script += g_render_camp_status.load(std::memory_order_relaxed)
        ? "true" : "false";
    script += ",iconScale:" + std::to_string(
        g_render_hero_icon_scale_percent.load(std::memory_order_relaxed));
    script += ",lifetime:" + std::to_string(
        g_render_trace_lifetime_seconds.load(std::memory_order_relaxed));
    const auto& map_minimum = snapshot.has_minimap_bounds
        ? snapshot.minimap_minimum : armlet_vec3{-8288.0f, -8288.0f, 0.0f};
    const auto& map_maximum = snapshot.has_minimap_bounds
        ? snapshot.minimap_maximum : armlet_vec3{8288.0f, 8288.0f, 0.0f};
    script += ",mapMinimum:[";
    panorama_hud_detail::append_number(script, map_minimum.x);
    script.push_back(',');
    panorama_hud_detail::append_number(script, map_minimum.y);
    script += "],mapMaximum:[";
    panorama_hud_detail::append_number(script, map_maximum.x);
    script.push_back(',');
    panorama_hud_detail::append_number(script, map_maximum.y);
    script.push_back(']');
    script += ",teleports:[";

    bool first = true;
    for (std::size_t index = 0; index < snapshot.teleport_count; ++index) {
        const auto& teleport = snapshot.teleports[index];
        if (!teleport.enemy_confirmed || teleport.expires_at <= now)
            continue;
        if (!first)
            script.push_back(',');
        first = false;
        script += "{heroName:";
        panorama_hud_detail::append_js_string(script, teleport.designer_name);
        script += ",origin:[";
        panorama_hud_detail::append_number(script, teleport.origin.x);
        script.push_back(',');
        panorama_hud_detail::append_number(script, teleport.origin.y);
        script.push_back(',');
        panorama_hud_detail::append_number(script, teleport.origin.z);
        script += "],destination:[";
        panorama_hud_detail::append_number(script, teleport.destination.x);
        script.push_back(',');
        panorama_hud_detail::append_number(script, teleport.destination.y);
        script.push_back(',');
        panorama_hud_detail::append_number(script, teleport.destination.z);
        script += "],hasOrigin:";
        script += teleport.has_origin ? "true" : "false";
        script += ",hasDestination:";
        script += teleport.has_destination ? "true" : "false";
        script += ",durationMs:" + std::to_string(teleport.duration_ms);
        script += ",elapsedMs:" + std::to_string(
            now > teleport.first_seen_at ? now - teleport.first_seen_at : 0);
        script.push_back('}');
    }
    script += "],traces:[";
    first = true;
    for (std::size_t index = 0; index < snapshot.trace_count; ++index) {
        const auto& trace = snapshot.traces[index];
        if (trace.expires_at <= now)
            continue;
        if (!first)
            script.push_back(',');
        first = false;
        script += "{heroName:";
        panorama_hud_detail::append_js_string(script, trace.designer_name);
        script += ",position:[";
        panorama_hud_detail::append_number(script, trace.position.x);
        script.push_back(',');
        panorama_hud_detail::append_number(script, trace.position.y);
        script.push_back(',');
        panorama_hud_detail::append_number(script, trace.position.z);
        script += "],remainingMs:" + std::to_string(trace.expires_at - now);
        script += ",ageMs:" + std::to_string(
            now > trace.created_at ? now - trace.created_at : 0);
        script += ",trigger:";
        panorama_hud_detail::append_js_string(script, trace.detector_trigger);
        script += ",ownerHandle:" + std::to_string(trace.owner_handle);
        script.push_back('}');
    }
    script += "],camps:[";
    first = true;
    for (std::size_t index = 0; index < snapshot.camp_count; ++index) {
        const auto& camp = snapshot.camps[index];
        if (!first)
            script.push_back(',');
        first = false;
        script += "{position:[";
        panorama_hud_detail::append_number(script, camp.position.x);
        script.push_back(',');
        panorama_hud_detail::append_number(script, camp.position.y);
        script.push_back(',');
        panorama_hud_detail::append_number(script, camp.position.z);
        script += "],type:" + std::to_string(camp.type);
        script += ",blocked:";
        script += camp.blocked ? "true" : "false";
        script.push_back('}');
    }
    script += "]});";
    return panorama_hud_detail::queue_script(game_window, script, false);
#endif
}

void execute_pending_panorama_hud_script() {
    std::array<char, panorama_hud_detail::maximum_script_size> script{};
    AcquireSRWLockExclusive(&panorama_hud_detail::script_lock);
    if (!panorama_hud_detail::script_pending.load(std::memory_order_acquire)) {
        ReleaseSRWLockExclusive(&panorama_hud_detail::script_lock);
        return;
    }
    script = panorama_hud_detail::pending_script;
    panorama_hud_detail::script_pending.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&panorama_hud_detail::script_lock);

    if (panorama_hud_detail::context_refresh_requested.exchange(false,
            std::memory_order_acq_rel)) {
        panorama_hud_detail::context_panel = 0;
        if (!panorama_hud_detail::resolve_context())
            return;
    }

    std::uintptr_t panel_name = 0;
    char name_probe[4]{};
    if (!plausible_pointer(panorama_hud_detail::ui_engine) ||
        !plausible_pointer(panorama_hud_detail::context_panel) ||
        !safe_read(panorama_hud_detail::context_panel +
            panorama_hud_detail::ui_panel_name_offset, panel_name) ||
        !read_c_string(panel_name, name_probe)) {
        panorama_hud_detail::ui_engine = 0;
        panorama_hud_detail::context_panel = 0;
        if (!panorama_hud_detail::resolve_context())
            return;
    }

    const HMODULE panorama = GetModuleHandleW(L"panorama.dll");
    std::uintptr_t vtable = 0;
    std::uintptr_t run_script_address = 0;
    if (panorama == nullptr ||
        !safe_read(panorama_hud_detail::ui_engine, vtable) ||
        !plausible_pointer(vtable) ||
        !safe_read(vtable + 77 * sizeof(void*), run_script_address) ||
        !panorama_hud_detail::address_in_module(run_script_address, panorama)) {
        panorama_hud_detail::ui_engine = 0;
        panorama_hud_detail::context_panel = 0;
        return;
    }

    // Current Panorama ABI keeps the two source-location integers and two
    // execution flags after the four pointer arguments. Omitting them leaves
    // the callee reading uninitialised stack slots and terminates Dota shortly
    // after the first script.
    using run_script_fn = void (*)(void*, void*, const char*, const char*,
        int, int, bool, bool);
#if defined(ASKERDWAY_PANORAMA_RESOLVE_ONLY)
    return;
#endif
    reinterpret_cast<run_script_fn>(run_script_address)(
        reinterpret_cast<void*>(panorama_hud_detail::ui_engine),
        reinterpret_cast<void*>(panorama_hud_detail::context_panel),
        script.data(), "askerdway_panorama.js", 8, 10, false, false);
}

void shutdown_panorama_hud(HWND game_window) {
    constexpr char cleanup[] =
        "if(typeof AskerdwayHudV11!=='undefined'&&AskerdwayHudV11){"
        "AskerdwayHudV11.shutdown();AskerdwayHudV11=undefined;}";
    panorama_hud_detail::queue_script(game_window, cleanup, true);
    uninstall_panorama_frame_dispatch();
    panorama_hud_detail::ui_engine = 0;
    panorama_hud_detail::context_panel = 0;
}
