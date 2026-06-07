idf_component_register(
    SRCS "momentum_settings.c" "asset_packs.c" "namespoof.c"
    INCLUDE_DIRS "."
    REQUIRES furi storage flipper_format gui
)
