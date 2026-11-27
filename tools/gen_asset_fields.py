"""Regenerate the offline compiler's typed gameplay asset fields from the 2016 reflection export."""
import argparse
import json
import re
from collections import defaultdict
from pathlib import Path

BACKEND = Path(__file__).resolve().parents[1] / "src/backend"
# These serialized declaration types store members inside additional native structures.
INLINE = {
    "entityDef": """idAnimatedEntity idEntityFx idEntityFxRandom idAIBehaviors idAIEditable_idAIDeath
        idAI2_idAIConstant idOutlineModel idEntityDamageComponent idGoreComponent idAnimator_AF idClipModelInfo
        trackingParmsStringPair_t idSpeaker idSyncGroup idSyncAttack idLootDropComponent idProp2 idProjectile""",
    "breakable": "idBreakableExplosion idBreakableTrail idBreakableDecal",
    "fx": "idFXSingleAction",
    "goreGraph": "idDeclGoreNode",
    "weapon": "idDeclInventory",
}
DECLS = ("aiBehavior", "ammo", "weapon", "projectile", "projectileImpactEffect", "goreGraph", "rumble")
# The archive's public names for native types with different spelling.
ALIASES = {
    "idMaterial": "material", "idSoundShader": "sound", "idEntityDefRef": "entityDef",
    "idDeclAF": "articulatedFigure", "idDeclAdvancedViewShake": "advancedScreenViewShake",
    "idDeclCodexEntry": "codex", "idDeclGoreWound": "gorewounds", "idDeclOutlineModel": "outlines",
    "idDeclMD6": "md6Def", "idDeclSyncInteraction": "syncInteractions",
    "idDeclProp_UseComponent": "propUseComponent", "idDeclProp_ThinkComponent": "propThinkComponent",
}
# The same serialized spelling has multiple native types across these components.
# The compiler resolves these by their containing record.
CONTEXTUAL = {"decl", "inventoryDecl", "componentDecl", "animWebs"}
def field_rows(schema):
    raw = (BACKEND / "modbundle.c").read_text(encoding="utf8").split("MB_DECL_TYPES[] = {", 1)[1].split("};", 1)[0]
    archive = dict(re.findall(r'\{"([^"]+)","([^"]+)"\}', raw))
    native = {("idDecl" + k).lower(): v for k, v in archive.items()}
    native.update({k.lower(): v for k, v in ALIASES.items()})
    groups = {d: ["idDecl" + d] for d in DECLS}
    groups.update({v: ["idDecl" + v] for k, v in archive.items() if k.startswith("aicomponent_")})
    for owner, classes in INLINE.items():
        groups.setdefault(owner, []).extend(classes.split())
    fields = defaultdict(set)
    for owner, prefixes in groups.items():
        for name, struct in schema["structs"].items():
            if not any(name.lower() == p.lower() or name.lower().startswith(p.lower() + "_") for p in prefixes):
                continue
            for field in struct["fields"]:
                key, typ = field["name"], field["type"]
                if key in CONTEXTUAL or not re.fullmatch(r"\w+", key):
                    continue
                for ref in re.findall(r'\b(id\w+)\s*(?=\*)|\b(idEntityDefRef)\b', typ):
                    dest = native.get((ref[0] or ref[1]).lower())
                    if dest:
                        is_list = "<" in typ or "[" in typ
                        fields[owner, key, is_list].add(dest)
    return sorted(((o, k, a, next(iter(t))) for (o, k, a), t in fields.items() if len(t) == 1),
                  key=lambda row: (row[0], row[1].lower(), row[2]))

def component_types(enum):
    raw = (BACKEND / "modbundle.c").read_text(encoding="utf8").split("MB_DECL_TYPES[] = {", 1)[1].split("};", 1)[0]
    decls = {name.lower().replace("_", ""): name for name in re.findall(r'\{"aicomponent_[^"]+","([^"]+)"\}', raw)}
    slots = [None] * enum["constants"]["AICOMPONENT_MAX"]["value"]
    for k, val in enum["constants"].items():
        index = val["value"]
        if k.startswith("AICOMPONENT_") and 0 <= index < len(slots):
            slots[index] = decls.get(k.lower().replace("_", ""))
    return slots

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("schema", type=Path, help="snaphak-re/reference/idlib_schema.json")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    rows = field_rows(json.loads(args.schema.read_bytes()))
    lines = ["/* Typed gameplay fields from the DOOM 2016 reflection export. See tools/gen_asset_fields.py. */"]
    lines += [f'    {{"{owner}", "{key}", "{dest}", {int(array)}}},' for owner, key, array, dest in rows]
    data = "\n".join(lines) + "\n"
    target = BACKEND / "mod_asset_fields.inc"
    enum = json.loads(args.schema.with_name("enumvals_schema.json").read_bytes())
    slots = component_types(enum)
    ids = "/* aiComponentId_t, in native array order. See tools/gen_asset_fields.py. */\n"
    ids += "".join(f'    "{slot}",\n' if slot else "    NULL,\n" for slot in slots)
    outputs = {target: data, BACKEND / "mod_ai_component_types.inc": ids}
    if args.check:
        return int(any(path.read_text(encoding="utf8") != body for path, body in outputs.items()))
    for path, body in outputs.items():
        path.write_text(body, encoding="utf8", newline="\n")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
