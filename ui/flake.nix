{
  description = "Beacon UI — QML frontend for the beacon_core module";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # Core module — the sibling `core/` in this repo, pinned to GitHub so this
    # flake builds standalone. For local dev against the sibling dir:
    #   nix build --override-input beacon_core path:../core '.#lgx-portable'
    beacon_core.url = "github:hackyguru/beacon?dir=core";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
