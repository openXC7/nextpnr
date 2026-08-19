void XilinxImpl::configurePlacerHeap(PlacerHeapCfg &cfg)
{
    cfg.hpwl_scale_x = 2;
    cfg.hpwl_scale_y = 1;
    cfg.beta = 0.5;
    cfg.placeAllAtOnce = true;
    cfg.get_cell_legalisation_weight = [this](Context *, CellInfo *ci) {
        if (ci->type != id_SLICE_LUTX)
            return 1;
        auto tags = get_tags(ci);
        // Place memory first, because they require entire SLICEMs
        return tags->lut.is_memory ? 100 : 1;
    };

    cfg.ff_bel_bucket = id_SLICE_FFX;
    cfg.ff_control_set_groups.resize(2);
    for (int z = 0; z < 8; z++) {
        cfg.ff_control_set_groups.at(z / 4).push_back((z << 4) | BEL_FF);
        cfg.ff_control_set_groups.at(z / 4).push_back((z << 4) | BEL_FF2);
    }
    cfg.ctrl_set_max_radius = std::vector<int>{18, 15, 12, 9, 6, 3};

    cfg.get_cell_control_set = [this](Context *, const CellInfo *ci) {
        if (ci->type != id_SLICE_FFX)
            return -1;
        auto tags = get_tags(ci);
        return tags->ff.control_set;
    };
}
