bool XilinxImpl::xc7_logic_tile_valid(IdString tile_type, const LogicTileStatus &lts) const
{
    bool is_slicem = (tile_type == id_CLBLM_L) || (tile_type == id_CLBLM_R);
    bool tile_is_memory = false;
    if (lts.cells[(3 << 4) | BEL_6LUT] != nullptr && get_tags(lts.cells[(3 << 4) | BEL_6LUT])->lut.is_memory)
        tile_is_memory = true;
    bool small_memory = false;
    if (lts.cells[(3 << 4) | BEL_5LUT] != nullptr && get_tags(lts.cells[(3 << 4) | BEL_5LUT])->lut.is_memory)
        small_memory = true;
    NetInfo *wclk = nullptr;
    // Check eight-tiles (mostly LUT-related validity)
    for (int i = 0; i < 8; i++) {
        if (lts.eights[i].dirty) {
            lts.eights[i].dirty = false;
            lts.eights[i].valid = false;

            auto lut6 = get_tags(lts.cells[(i << 4) | BEL_6LUT]);
            auto lut5 = get_tags(lts.cells[(i << 4) | BEL_5LUT]);

            // Check 6LUT
            if (lut6) {
                if (!is_slicem && (lut6->lut.is_memory || lut6->lut.is_srl)) {
                    DBG();
                    return false;
                } // Memory and SRLs only valid in SLICEMs
                if (lut6->lut.is_srl && (i >= 4)) {
                    DBG();
                    return false;
                }
                if (lut6->lut.is_memory || lut6->lut.is_srl) {
                    if (wclk == nullptr)
                        wclk = lut6->lut.wclk;
                    else if (lut6->lut.wclk != wclk) {
                        DBG();
                        return false;
                    }
                }
                if (lut5) {
