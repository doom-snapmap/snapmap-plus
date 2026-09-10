# Weapon ammo displays in override packages

A weapon mod can choose how the existing ammo panel interprets its weapon's
capacity. Put `hud/weapons.json` inside the package:

```json
{
  "schema": "snapmap-plus.weapon-hud.v1",
  "weapons": {
    "weapon/example/sidearm": {
      "ammo_display": "weapon"
    }
  }
}
```

Keys are exact logical weapon declaration names, including the `weapon/` prefix
when it is part of that name. They are not filesystem paths or entityDef names.
List secondary weapon declarations separately if the equipped HUD can use them.
The feature is available to any weapon package; there are no built-in weapon
names or pistol-specific checks.

| `ammo_display` | Behavior |
|---|---|
| `engine` | Preserve the game's normal display decision. Also the default for an unlisted weapon. |
| `weapon` | Remove the multiplayer mode's forced numeric display. The existing HUD displays numbers for a positive maximum ammo count, and its infinity frame with a full bar when the maximum is zero or negative. |

The remaining native HUD conditions, including special weapon handling, still
apply. This setting does not grant ammo, enable firing, change damage, convert a
finite weapon to infinite ammo, or replace HUD art. A weapon's unlimited-ammo
gameplay flag alone does not guarantee its HUD reports a nonpositive maximum;
check its actual capacity before expecting the infinity frame. Custom colors,
layouts, crosshairs and arbitrary runtime setters are outside this schema.

On the tested stock panel the infinite state shows a white infinity symbol and
a full blue bar. It uses artwork already present in SnapMap, so a package does
not need to copy campaign HUD assets to obtain it.

## Installation and conflicts

Install the package normally under the user data directory's `overrides/` and
restart DOOM. Updating or removing it also takes effect after restart. Disabling
user overrides disables these settings. Packages installed through the existing
map-package flow are recaptured during re-arm before the next map load; this is
not a live HUD editing interface. An already cached panel refreshes on its next
native weapon/ammo update or map reload.

Identical requests for the same weapon compose. Different requests for the same
weapon are refused with both package names in `sh_backend.log`; priority does
not choose a winner. Invalid or unreadable policies clear the complete HUD
policy table, preserving engine behavior. Other override features remain
independent. A missing or incompatible engine signature leaves the HUD call
unchanged and emits a refusal diagnostic.

Only `schema` and `weapons` are accepted at the root, and only `ammo_display` in
each weapon's settings. Duplicate JSON keys, unknown modes/fields and embedded
NUL characters are refused. Names use lowercase ASCII letters, digits, slash,
underscore, dash and period, with no empty, `.` or `..` path components. Limits
are 191 bytes per name, 256 distinct weapons across packages, 64 KiB per file
and 256 KiB of policy JSON across packages. Reparse-point HUD folders/files
are refused.

This requires a Snapmap+ build containing this feature on each client that
needs the display change. Vanilla DOOM does not interpret package policies.
The normal package transport can carry the JSON, but the automatic map-use
detector recognizes added declaration identities, not shadow-only changes or
HUD rules alone. Do not assume a map automatically embeds such a package;
distribute/install it explicitly unless its package dependency was established.
Mixed modded/vanilla multiplayer compatibility of a particular weapon mod
requires separate gameplay testing.

## Implementation

`weapon_hud.c` reads a complete bounded snapshot and publishes it under an SRW
lock. The HUD lookup does no filesystem access and stores no engine objects.
One signature-resolved call in the ammo widget routes through an owned, nearby
RX relay. The relay passes the widget alongside the original game argument to
a normal Windows x64 function, following the [Microsoft calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention).
The original mode predicate remains intact for every other caller. The handler
reads the weapon name and substitutes only this call's boolean result; it never
writes ammo, weapon declarations, widget fields or the game mode.

Both the call site and predicate must resolve uniquely, and the decoded call
must target that predicate. The original call bytes are checked again before
installation. The call opcode stays intact; its aligned four-byte displacement
is published with a compare-exchange so concurrent execution cannot observe a
torn address. Unaligned call operands are refused. The relay is written as RW
and changed to RX before publication.
The signatures and field layout were checked independently on Vulkan and
OpenGL; the two-image resolver tests are required when changing them.
