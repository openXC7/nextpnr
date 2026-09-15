NPNR_PACKED_STRUCT(struct PipDataPOD {
    int32_t src_wire;
    int32_t dst_wire;

    uint32_t type;
    uint32_t flags;
    int32_t timing_idx;

    RelPtr<uint8_t> extra_data;
});
