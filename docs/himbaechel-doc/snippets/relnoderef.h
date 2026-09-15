NPNR_PACKED_STRUCT(struct RelNodeRefPOD {
    // wire is entirely internal to a single tile
    static constexpr int16_t MODE_TILE_WIRE = 0x7000;
    // where this is the root {wire, dy} form the node shape index
    static constexpr int16_t MODE_IS_ROOT = 0x7001;
    // special cases for the global constant nets
    static constexpr int16_t MODE_ROW_CONST = 0x7002;
    static constexpr int16_t MODE_GLB_CONST = 0x7003;
    // special cases where the user needs to outsmart the deduplication [0x7010, 0x7FFF]
    static constexpr int16_t MODE_USR_BEGIN = 0x7010;
    int16_t dx_mode; // relative X-coord, or a special value
    int16_t dy;      // normally, relative Y-coord
    uint16_t wire;   // normally, node index in tile (x+dx, y+dy)
});
