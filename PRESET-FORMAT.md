# PRESET FORMAT

This document describes the file format used in mod-connector.

A preset consists of a simple, standards-compliant, non-extended JSON file.
This means no comments or extraneous commas allowed and UTF-8 encoding can be assumed.

Each preset is saved individually, grouping presets into 1 "bank" is simply aesthetic and for caching/preloading.

# Required fields

The preset root has 3 required fields:

- `preset` (object)
- `type` (string)
- `version` (integer)

Where `type` must be "preset" and `version` must be >= 1.
The actual preset data is stored in the `preset` object.

#### Data model

For quick reference, the most simple valid preset is the following:

```json
{
  "preset": {},
  "type": "preset",
  "version": 1
}
```

# Preset data

Inside the `preset` object we can have the following optional fields:

- `background` (object)
- `bindings` (object)
- `chains` (object)
- `name` (string)
- `scene` (integer)
- `sceneNames` (object)
- `uuid` (string)

All fields are optional, but missing `bindings` or `chains` will give a warning.

While `uuid` is optional, a missing or invalid value will generate and apply a new UUID when loaded.

#### Data model

For quick reference, a simple valid preset with all preset data fields inside is the following:

```json
{
  "preset": {
    "background": {
      "color": 0,
      "style": "",
    },
    "bindings": {
      "pot1": {
        "name": "Example",
        "parameters": [
          "block": 1,
          "max": 20.0,
          "min": -20.0,
          "row": 1,
          "symbol": "gain"
        ],
        "properties": [],
        "value": 0.5
      }
    },
    "chains": {
      "1": {
        "blocks": {
          "5": {
            "enabled": true,
            "parameters": {
              "1": {
                "name": "Gain",
                "symbol": "gain",
                "value": 0.0
              }
            },
            "properties": {
              "1": {
                "name": "File",
                "uri": "urn:plugin:file",
                "value": "/path/to/file.ext"
              }
            },
            "quickpot": "gain",
            "scenes": {
              "2": {
                "parameters": [
                  {
                    "symbol": "depth",
                    "value": 6.4
                  } 
                ],
                "properties": []
              }
            },
            "uri": "urn:example:gain"
          }
        }
      }
    },
    "name": "Untitled",
    "scene": 0,
    "sceneNames": {
      "2": "A"
    },
    "uuid": "00000000-0000-0000-0000-000000000000"
  },
  "type": "preset",
  "version": 1
}
```

## background

An object used to store custom colors and style to apply to the signal chain view while the preset is loaded. It is meant only for "artists" and not to be available directly to users. Saving a preset that includes `background` object will remove that object.

The following fields are required:

- `color` (integer)
- `style` (string)

The `color` value is a RGB8-encoded integer, so that `0` is black and `16777215` (`0xffffff` in hexadecimal) is white. Because standard JSON does not support hexadecimal integer type we store the value in decimal form. This value is used as a custom background color in the signal chain view.

The `style` value is a simple string related to an artist’s name, e.g. "Adam" would be adam. TBD a list of possible artists and what they will change. While this value is mandatory, it can be an empty string in which case only the custom background color is applied.

## bindings

The object used for bindings, with 1 sub-object per hardware actuator (e.g. "foot1" and "foot2").

If a hardware actuator is unused for bindings, it should not be part of the preset.

When a hardware actuator is used, then the following fields are optional:

- `name` (string)
- `parameters` (array)
- `properties` (array)
- `value` (double)

The `name` is self-explanatory.

`parameters` are for regular parameter/control bindings, related to LV2 Control ports.

`properties` are for more fancy plugin properties like paths and strings, related to LV2 Patch-based Parameters.

> Note: we do not yet make use of "properties", but they are already in the preset format as WIP compability with more advanced LV2 features in 3rd party plugins (like those made with JUCE)

`value` is normalized to 0↔︎1 and only used for "macros" (multiple bindings in a single actuator). It represents the current knob/pot position of this binding.

### parameters (bindings)

Each parameter must contain the following required fields:

- `block` (integer)
- `row` (integer)
- `symbol` (string)

The `block` and `row` indicate which "block" (as in, plugin) position in the signal chain this binding applies to. This integer counts from 1.

The `symbol` indicates the parameter symbol within that block/plugin.

> We are using some non-LV2-standard control port symbols starting with ":" for parameters with special meaning. Bypass will use ":bypass" and blocks with dynamic filename-based lists will use e.g. ":cab" and ":mic".

Additionally each parameter can contain the following optional fields:

- `min` (double)
- `max` (double)

These indicate the parameter ranges to use for the binding. Both must be provided at the same time to be valid. If not valid or missing the full parameter range is used.

Note that it is valid for min > max, which we use as a way to indicate the binding should control the parameter in an "inverted" fashion - left/empty value is "max" and right/full value is "min".

### properties (bindings)

TBD once we have any plugins that make use of them, we can ignore this until then.

## chains

The big preset object that contains the list of blocks in use, their positions in the chain and all parameter values.

We divide the chain into rows, and then blocks. Example of data model:

```json
{
  "chains": {
    "1": {
      "blocks": {
        "1": {},
        "5": {},
        "6": {}
      },
    },
    "2": {
      "blocks": {
        "2": {},
        "3": {},
        "7": {}
      }
    }
  }
}
```

The first data inside `chains` is the "chain row" in a number-as-string fashion, starting from 1.
A "chain row" can be missing from the preset file if it does not contain any blocks.

Then each "chain row" contains another object, for the moment we only use `blocks`. At some point we might add more fields here.

Inside `blocks` we have the "blocks present on a single chain row", referenced also in a number-as-string fashion, starting from 1. A "chain block" can be missing from the preset file if its location is empty.

If a "chain block" is present then the `uri` (string) field is required. This is the LV2 unique ID of a plugin. Then following fields are optional:

- `enabled` (bool)
- `parameters` (object)
- `properties` (object)
- `quickpot` (string)
- `scenes` (object)

The `enabled` field indicates if a block is enabled (not enabled means bypassed). If this field is missing the block is assumed to be enabled.

The `parameters` and `properties` both contain objects in a number-as-string fashion, starting from 1 without any gaps.

Example of parameters and properties:

```json
{
  "parameters": {
    "1": {
      "name": "Boost",
      "symbol": "boost",
      "value": 6.5
    },
    "2": {
      "name": "Character",
      "symbol": "character",
      "value": 7.7
    }
  },
  "properties": {
    "1": {
      "name": "File",
      "uri": "urn:plugin:file",
      "value": "/path/to/file.ext"
    }
  }
}
```

The data model here is very similar between the 2. In both the `name` is purely informational. Then parameters use `symbol` while properties use `uri` but they both serve as identifier. Finally we have `value` field present in both, but parameters use it to store a double-type parameter value while properties store values as a string.

Unlike the case with bindings, we use block/plugin properties in Anagram already. They are used for the blocks that have dynamic filename-based lists, we store the full filename here. No other "properties" use-cases are supported yet.

The `quickpot` is a string matching the parameter symbol meant to be used as quick-pot. If missing the block will use its own default.

> The quickpot field is expected to change "soon" in order to support macros

The `scenes` also contain objects in a number-as-string fashion, starting from 1, but can contain gaps. So e.g. scene 1 and 3 can be present while scene 2 is missing.

Example of scenes:

```json
{
  "scenes": {
    "1": {
      "parameters": [
        {
          "symbol": "depth",
          "value": 6.4
        },
        {
          "symbol": "rate",
          "value": 1.15
        } 
      ],
      "properties": []
    },
    "3": {
      "parameters": [
        { 
          "symbol": "depth",
          "value": 1.5  
        },
        {
          "symbol": "rate",
          "value": 4.5
        }
      ],
      "properties": []
    } 
  }
}
```

## name

A string for naming the preset.

## scene

An integer that indicates the active scene.

> Unlike many other fields on the preset file, here we count from 0. So 1st scene is 0, 2nd scene is 1, etc

## sceneNames

An object that contains possible custom scene names provided by the user.

All scene names are optional, so e.g. a preset might have a name for scene 2 but not for 1 or 3. For this reason the contents are a json object instead of a json array. Example:

```json
{
  "sceneNames": {
    "2": "name"
  }
}
```

## uuid

A string in UUIDv4 format, with all characters lowercase.

Each preset has its own uuid, and saving any preset will change its uuid to a new value.

TODO: better handling of uuids, so we can know if preset is "unchanged" even if saved multiple times.

# Versioning

To deal with backwards and future compatibility the following rules are applied to versioning:

1. Saved presets store their preset format version in the json’s root version field
2. When loading a preset Anagram checks for version which must be
  - &gt;= minimum supported version (for now this is `1`)
  - &lt;= maximum supported version (for now this is `1`)
3. On case of loading a preset that is supported but uses old version, compatibility behaviour is engaged (for now there are no such checks yet)

We should try as much as possible to avoid breakage and even just bumping the version number, as it then needs adjustments on multiple places.

It is likely that the minimum supported version will remain at `1` forever.

# Changelog

Changes made to the file format so far:

- Bindings value is always normalized
  - In early versions on cases of a single binding, the bindings value would be equal to the parameter value it binds to.
  - So e.g. a gain binding that goes from -20 to +20 and is saved while at the minimum position would have bindings value at -20
  - Now the bindings value is always normalized, and unused for the cases of a single binding (as it is not needed)
- Parameter bindings can have minimum and maximum
  - In early versions we did not have ways to specify minimum or maximum ranges of a binding, so this information was not part of the preset
  - For backwards compatibility we assume that missing min and max fields of a parameter binding to mean "use full parameter range"
