NPNR_PACKED_STRUCT(struct ChipInfoPOD {
    int32_t magic;
    int32_t version;
    int32_t width, height;

    RelPtr<char> uarch;
    RelPtr<char> name;
    RelPtr<char> generator;

    RelSlice<TileTypePOD> tile_types;
    RelSlice<TileInstPOD> tile_insts;
    RelSlice<NodeShapePOD> node_shapes;
    RelSlice<TileRoutingShapePOD> tile_shapes;

    RelSlice<PackageInfoPOD> packages;
    RelSlice<SpeedGradePOD> speed_grades;

    RelPtr<ConstIDDataPOD> extra_constids;

    RelPtr<uint8_t> extra_data;
});
