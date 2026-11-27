# Customizing the SnapMap editor

Use the `editor/` folder inside your mod. **New mod** in the Mods tab creates a working example. Press **Refresh** after editing it.

```text
my-mod/
  mod.json
  editor/
    toybox.json
    panels.json
    strings/
      en.json
      it.json
  entities/
  materials/
  images/
```

You choose the names of the JSON files directly under `editor/`. Give each entry a single definition within your mod. A tile refers to an entity and optionally inherits a stock tile's behavior. Its icon refers to a material. The material can use one of your images. A saved map refers to the placed entity by its stable identity.

## Move a tile

In **Assets → Reference → Editor defs**, select a tile and choose **Copy editor ID**. The stock chainsaw pickup is `weapons/chainsaw_mp_base`. This example moves it to the built-in Custom tab. Its pickup behavior, icon and property bindings come from the original tile.

```json
{
  "tiles": {
    "weapons/chainsaw_mp_base": { "tab": "snapmap-plus/custom" }
  }
}
```

The same rule works for `unknown/unknown`, stock demons and installed mod tiles. To target another mod's tile, use `mods/<salt>/<tile-id>`, with the salt from its `mod.json` and the tile ID from its editor definition. A row for an existing tile supplies just the fields you want to change. Use `order` for its native integer sort priority within the tab. Custom appears first even when a mod translates or renames it.

## Add a tile and a tab

```json
{
  "tabs": {
    "equipment": { "title": "equipment_tab" }
  },
  "tiles": {
    "control_panel": {
      "entity": "entities/control_panel",
      "base": "interactives/use_panel",
      "title": "panel_name",
      "description": "panel_help",
      "icon": "materials/panel_icon",
      "tab": "equipment"
    }
  }
}
```

Put the entity in `entities/control_panel.decl` and your icon material in `materials/panel_icon.decl`. Put the English text in `editor/strings/en.json`.

```json
{
  "equipment_tab": "Equipment",
  "panel_name": "Control panel",
  "panel_help": "Opens the service hatch."
}
```

Italian players see that English text until you supply the corresponding keys in `editor/strings/it.json`. You can translate one label at a time. The other supported game languages use `de`, `es`, `es-419`, `fr`, `ja`, `pl`, `pt`, `ru` and `zh-Hant`. The same catalogs serve titles, descriptions, sheet labels and settings.

A simple tab ID like `equipment` belongs to its mod. Two mods may both display “Equipment” and still have independent tabs. For a tab that several mods deliberately share, each participating mod declares the same ID and supplies its own caption. The six Campaign Interactables packs each declare `"shared/campaign-interactables": { "title": "ci_tab" }` and put their visible tiles there. Each pack works on its own. A stock tab can be targeted by its existing `#str_…` category tag.

An advanced tile can specify a native `template` under `editor/templates/`. This carries native features such as event wiring, animation or rendering data. The named JSON fields override the template. The installer keeps the original bytes of older tiles here so a migrated tile can reproduce its original build.

## Arrange a property sheet

Choose **Show property IDs** on an editor definition to find its native pages and property paths. Click an ID to copy it. For example, this adds a Collision sheet to Blocking Volume and moves two existing controls.

```json
{
  "sheets": {
    "volume/blocking": {
      "collision": {
        "title": "collision_title",
        "before": "native/displayName",
        "properties": {
          "blockPlayers": {},
          "blockDemons": { "after": "blockPlayers" }
        }
      }
    }
  }
}
```

Add `"collision_title": "Collision"` to your English catalog. Each control keeps its original inspector, binding and stored value. A property can override its `title`, `description` or native `inspector`. An existing binding can appear more than once in native data. The browser gives you its qualified ID in that case. A missing target contributes no change. Existing advanced native editor declarations live at `editor/native/generated/decls/<type>/<name>.decl`.

## Add a setting

```json
{
  "settings": {
    "show_collision": { "scope": "editor", "title": "show_collision_label", "default": true },
    "alarm_enabled": { "scope": "map", "title": "alarm_label", "default": true }
  }
}
```

Provide the two label keys in the English catalog. Each setting is a checkbox and can have a `description`. Native Settings applies or cancels edits together. An editor-scoped value belongs to the player. Put `"when": { "show_collision": true }` on the Collision sheet to make that arrangement optional. A map-scoped value belongs to the map using the mod. Its gameplay script reads it with `map_setting("alarm_enabled")`. The exact script build supplies the default on a clean installation.

## Multiple mods

Each installed mod contributes the fields it explicitly sets. One mod can move Unknown while another changes its icon or adds a sheet. If two mods change the same field, the mod with the alphabetically earlier salt wins. Removing it reveals the other contribution. A shared tab uses its stable ID in every language. The order of installation and the spelling of the on-screen label cannot change which object a rule targets.

## What a published map downloads

A published map records the exact slices for its placed objects and any mods the author enabled map-wide. Play and Edit use the same slices. For example, placing the Crucible from Campaign Interactables includes its entity, interaction and animation dependencies, as well as its own Toybox icon and description. Someone who opens a copy to edit or remix can identify that same placed object. Files already in the player's cache can be reused.

The player's Toybox layout, custom property sheets and editor preferences come from locally installed mods. Playing a map leaves those choices alone. A map-scoped gameplay setting travels with the map and its script.
